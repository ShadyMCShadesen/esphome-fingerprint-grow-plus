#!/usr/bin/env python3
"""End-to-end test: several fingerprint_grow_plus nodes on the ESPHome host platform.

Each node is the real firmware (ESPHome host build) talking to a simulated R503
(tests/sim/r503_sim.py) through a pseudo terminal. Nodes sync over TCP on localhost.

Usage:  python3 tests/e2e/run_e2e.py [--esphome /path/to/esphome] [--keep]
Exit code 0 = all scenarios passed.
"""

import argparse
import base64
import re
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "e2e" / ".out"
SIM = ROOT / "tests" / "sim" / "r503_sim.py"

KEY = "q83vEjRWeJCrze8SNFZ4kKvN7xI0VniQq83vEjRWeJA="
WRONG_KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="

def _clock(minutes):
    minutes %= 24 * 60
    return f"{minutes // 60:02d}:{minutes % 60:02d}"


def schedule_block():
    """Slots 40-49: window open now (now +- 2 h); slots 50-59: window closed (now + 3..5 h)."""
    now = time.localtime()
    m = now.tm_hour * 60 + now.tm_min
    return (
        "\n  schedule:"
        "\n    on_denied:"
        "\n      - logger.log:"
        "\n          format: 'TEST-EVENT denied %u'"
        "\n          args: [finger_id]"
        "\n    ranges:"
        "\n      - slots: '40-49'"
        f"\n        times: [{{days: daily, from: '{_clock(m - 120)}', to: '{_clock(m + 120)}'}}]"
        "\n      - slots: '50-59'"
        f"\n        times: [{{days: daily, from: '{_clock(m + 180)}', to: '{_clock(m + 300)}'}}]"
        # over midnight (from > to): open except now+60..now+120 -> open now
        "\n      - slots: '60-69'"
        f"\n        times: [{{days: daily, from: '{_clock(m + 120)}', to: '{_clock(m + 60)}'}}]"
        # over midnight: open except now-60..now+60 -> closed now
        "\n      - slots: '70-79'"
        f"\n        times: [{{days: daily, from: '{_clock(m + 60)}', to: '{_clock(m - 60)}'}}]"
    )


LOCKOUT = (
    "\n  lockout:"
    "\n    max_attempts: 3"
    "\n    duration: 40s"
    "\n    share_with: [{share}]"
    "\n    on_locked:"
    "\n      - logger.log:"
    "\n          format: 'TEST-EVENT locked %s'"
    "\n          args: [source.c_str()]"
    "\n    on_unlocked:"
    "\n      - logger.log: 'TEST-EVENT unlocked'"
)
DURESS = (
    "\n  duress:"
    "\n    slots: '150-159'"
    "\n    on_duress:"
    "\n      - logger.log:"
    "\n          format: 'TEST-EVENT duress %u'"
    "\n          args: [finger_id]"
)

NODES = {
    "node-a": {"port": 18531, "peers": ["node-b", "node-c", "node-d"], "include": ["0-199"], "exclude": []},
    # node-b locks after 3 unrecognised fingers and passes the lockout on to node-c only
    "node-b": {"port": 18532, "peers": ["node-a", "node-c"], "include": ["0-199"], "exclude": [],
               "top_extra": LOCKOUT.format(share="127.0.0.1:18533")},
    # node-c keeps 150-199 for itself (node-exclusive range)
    "node-c": {"port": 18533, "peers": ["node-a", "node-b"], "include": ["0-199"], "exclude": ["150-199"],
               "top_extra": LOCKOUT.format(share="") + DURESS},
    # node-d has the wrong key and must be rejected
    "node-d": {"port": 18534, "peers": ["node-a"], "include": ["0-199"], "exclude": [], "key": WRONG_KEY},
    # node-e runs in dry-run mode: it must log what it would do and change nothing
    "node-e": {"port": 18535, "peers": ["node-a"], "include": ["0-199"], "exclude": [], "dry_run": True},
    # node-f runs stand-alone (no sync: block): admin features only, no network port
    "node-f": {"port": 0, "peers": [], "include": ["0-199"], "exclude": [], "standalone": True},
    # node-g syncs only the admin slot 0 with node-a; all other slots are local
    "node-g": {"port": 18537, "peers": ["node-a"], "include": ["0"], "exclude": []},
    # node-h only syncs when asked to (interval: never, no sync at boot or after changes)
    "node-h": {"port": 18538, "peers": ["node-a"], "include": ["0-199"], "exclude": [],
               "sync_extra": "    at_boot: false\n    after_change: false\n", "interval": "never"},
}

SEEDS = {
    "node-a": {"0": {"finger": "admin"}, "1": {"finger": "alice"}, "2": {"finger": "bob", "noise": "bob-on-a"},
               "9": {"finger": "boss"}},
    "node-b": {"2": {"finger": "bob", "noise": "bob-on-b"}, "5": {"finger": "carol"}},
    "node-c": {"2": {"finger": "dave"}, "150": {"finger": "eve"}},
    "node-d": {"7": {"finger": "mallory"}},
    "node-e": {"9": {"finger": "grace"}},
    "node-f": {"0": {"finger": "admin"}, "45": {"finger": "alma"}, "55": {"finger": "dora"}},
    "node-g": {"1": {"finger": "gina"}},
    "node-h": {},
}

SYNC_BLOCK = """  sync:
    key: "{key}"
    port: {port}
    peers: {peers}
    include_slots: {include}
    exclude_slots: {exclude}
    interval: {interval}
    dry_run: {dry_run}
{sync_extra}    on_done:
      - logger.log: "TEST-EVENT sync_done"
    on_failed:
      - logger.log:
          format: "TEST-EVENT sync_failed %s"
          args: [reason.c_str()]
    on_conflict:
      - logger.log:
          format: "TEST-EVENT conflict %u"
          args: [slot]
"""

YAML_TEMPLATE = """
esphome:
  name: {name}
host:
logger:
  level: DEBUG
external_components:
  - source:
      type: local
      path: {components}
    components: [fingerprint_grow_plus]
time:
  - platform: host
    id: host_time
uart:
  id: uart_bus
  port: {pty}
  baud_rate: 57600
fingerprint_grow:
  id: grow
  uart_id: uart_bus
  update_interval: 300ms
sensor:
  - platform: fingerprint_grow
    fingerprint_grow_id: grow
    fingerprint_count:
      name: Fingerprint Count
      id: fp_count
    capacity:
      name: Fingerprint Capacity
      id: fp_capacity
  - platform: fingerprint_grow_plus
    peers_reachable:
      name: Peers reachable
    conflict_count:
      name: Sync conflicts
    versions_left:
      name: Versions left
binary_sensor:
  - platform: template
    id: finger_detected
text_sensor:
  - platform: fingerprint_grow_plus
    status:
      name: Sync status
    occupied_slots:
      name: Occupied slots
    unreachable_peers:
      name: Unreachable peers
    last_name:
      name: Last name
fingerprint_grow_plus:
  id: fp_plus
  startup_delay: 2s
  fingerprint_count_sensor: fp_count
  fingerprint_capacity_sensor: fp_capacity
  delete_all_code: TEST_ALL
  time_id: host_time
  finger_sensor: finger_detected{top_extra}
  on_enroll_started:
    - logger.log:
        format: "TEST-EVENT enroll_started %u"
        args: [slot]
  admin:
    slots: [0, 9]
    target_timeout: 15s
    num_scans: 2{admin_extra}
    on_touch:
      - logger.log:
          format: "TEST-EVENT admin_touch %u"
          args: [touches]
    on_access:
      - logger.log:
          format: "TEST-EVENT admin_access %u"
          args: [finger_id]
    on_gesture:
      - logger.log:
          format: "TEST-EVENT admin_gesture %u"
          args: [touches]
    on_waiting:
      - logger.log:
          format: "TEST-EVENT admin_waiting %s"
          args: [action.c_str()]
    on_hold:
      - logger.log: "TEST-EVENT admin_hold"
    on_cancelled:
      - logger.log: "TEST-EVENT admin_cancelled"
    on_done:
      - logger.log:
          format: "TEST-EVENT admin_done %s %u"
          args: [action.c_str(), slot]
  guests:
    slots: "100-109"
    valid_for: 25s
{sync_block}
interval:
  - interval: 300ms
    then:
      - lambda: |-
          FILE *f = fopen("{cmd}", "r");
          if (f == nullptr) return;
          char cmd[32] = {{0}};
          int a = 0, b = 0;
          char arg[64] = {{0}};
          int n = fscanf(f, "%31s %d %63s", cmd, &a, arg);
          b = atoi(arg);
          fclose(f);
          remove("{cmd}");
          if (n < 1) return;
          std::string c(cmd);
          if (c == "delete") id(fp_plus).delete_slot(a);
          else if (c == "resolve") id(fp_plus).resolve_conflict(a, b != 0);
          else if (c == "sync") id(fp_plus).sync_now();
          else if (c == "name") id(fp_plus).set_slot_name(a, arg);
          else if (c == "showname") ESP_LOGD("test", "TEST-EVENT name %d %s", a, id(fp_plus).slot_name(a).c_str());
          else if (c == "enroll_named") id(fp_plus).enroll_next_free(a, arg);
          else if (c == "enroll_guest_uses") id(fp_plus).enroll_guest(2, 600, "", a);
          else if (c == "showuses") ESP_LOGD("test", "TEST-EVENT uses %d %u", a, id(fp_plus).slot_uses_left(a));
          else if (c == "move") id(fp_plus).move_slot(a, b);
          else if (c == "reset_versions") id(fp_plus).reset_versions("TEST_ALL");
          else if (c == "allowed") ESP_LOGD("test", "TEST-EVENT allowed %d %d", a, id(fp_plus).is_allowed(a) ? 1 : 0);
          else if (c == "finger_on") id(finger_detected).publish_state(true);
          else if (c == "finger_off") id(finger_detected).publish_state(false);
          else if (c == "delete_all") id(fp_plus).delete_all("TEST_ALL");
          else if (c == "delete_all_wrong") id(fp_plus).delete_all("nope");
          else if (c == "allow_restore") id(fp_plus).allow_restore(120);
          else if (c == "allow_full_backup") id(fp_plus).allow_full_backup(120);
          else if (c == "resolve_all") id(fp_plus).resolve_all_conflicts(a != 0);
          else if (c == "claim_all") id(fp_plus).claim_all_slots();
          else if (c == "enroll_guest") id(fp_plus).enroll_guest(2, 25);
"""


class Cluster:
    def __init__(self, esphome: str):
        self.esphome = esphome
        self.procs: dict[str, subprocess.Popen] = {}
        self.sims: dict[str, subprocess.Popen] = {}

    def path(self, name, suffix):
        if suffix == "pty":
            # The host UART only accepts a device path directly below a top-level directory.
            return Path(f"/tmp/growplus-e2e-{name}.pty")
        return OUT / f"{name}.{suffix}"

    def generate_and_compile(self):
        OUT.mkdir(parents=True, exist_ok=True)
        for name, cfg in NODES.items():
            peers = [f"127.0.0.1:{NODES[p]['port']}" for p in cfg["peers"]]
            sync_block = "" if cfg.get("standalone") else SYNC_BLOCK.format(
                key=cfg.get("key", KEY),
                port=cfg["port"],
                peers=json.dumps(peers),
                include=json.dumps(cfg["include"]),
                exclude=json.dumps(cfg["exclude"]),
                dry_run="true" if cfg.get("dry_run") else "false",
                interval=cfg.get("interval", "15s"),
                sync_extra=cfg.get("sync_extra", ""),
            )
            yaml = YAML_TEMPLATE.format(
                name=name,
                components=ROOT / "components",
                pty=self.path(name, "pty"),
                cmd=self.path(name, "cmd"),
                admin_extra="\n    access: delayed" if cfg.get("standalone") else "",
                top_extra=("\n  led_feedback:" + schedule_block() if cfg.get("standalone") else "")
                + cfg.get("top_extra", ""),
                sync_block=sync_block,
            )
            yaml_path = self.path(name, "yaml")
            yaml_path.write_text(yaml, encoding="utf-8")
            print(f"== compiling {name}", flush=True)
            res = subprocess.run(
                [self.esphome, "compile", str(yaml_path)], capture_output=True, text=True, check=False
            )
            if res.returncode != 0:
                print(res.stdout[-4000:], res.stderr[-4000:])
                raise SystemExit(f"compile of {name} failed")

    def program(self, name):
        return OUT / ".esphome" / "build" / name / ".pioenvs" / name / "program"

    def start_sim(self, name):
        seed = self.path(name, "seed.json")
        seed.write_text(json.dumps({"slots": SEEDS.get(name, {})}), encoding="utf-8")
        self.path(name, "ctl").write_text("", encoding="utf-8")
        log = open(self.path(name, "sim.log"), "w", encoding="utf-8")
        self.sims[name] = subprocess.Popen(
            [sys.executable, str(SIM), "--name", name, "--link", str(self.path(name, "pty")),
             "--state", str(self.path(name, "state.json")), "--control", str(self.path(name, "ctl")),
             "--seed", str(seed)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        for _ in range(50):
            if self.path(name, "pty").exists():
                return
            time.sleep(0.1)
        raise SystemExit(f"simulator {name} did not start")

    def start_node(self, name):
        env = dict(os.environ)
        env["ESPHOME_PREFDIR"] = str(OUT / f"{name}.prefs")
        log = open(self.path(name, "log"), "a", encoding="utf-8")
        self.procs[name] = subprocess.Popen([str(self.program(name))], stdout=log, stderr=subprocess.STDOUT, env=env)

    def stop_node(self, name):
        proc = self.procs.pop(name, None)
        if proc:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()

    def stop_all(self):
        for name in list(self.procs):
            self.stop_node(name)
        for proc in self.sims.values():
            proc.kill()

    def state(self, name):
        try:
            return json.loads(self.path(name, "state.json").read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError):
            return {"slots": {}, "stats": {}}

    def slots(self, name):
        return {int(k): v["finger"] for k, v in self.state(name)["slots"].items()}

    def sha(self, name, slot):
        return self.state(name)["slots"].get(str(slot), {}).get("sha")

    def log(self, name):
        try:
            return self.path(name, "log").read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def cmd(self, name, text):
        self.path(name, "cmd").write_text(text + "\n", encoding="utf-8")

    def touches(self, name, finger, count, gap=1.2):
        for i in range(count):
            self.touch(name, finger)
            time.sleep(gap)

    def touch(self, name, finger):
        with open(self.path(name, "ctl"), "a", encoding="utf-8") as f:
            f.write(f"touch {finger}\n")


def wait_until(desc, predicate, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            print(f"  ok   {desc}", flush=True)
            return True
        time.sleep(0.5)
    print(f"  FAIL {desc}", flush=True)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--esphome", default=shutil.which("esphome") or "esphome")
    parser.add_argument("--keep", action="store_true", help="keep processes running on exit")
    parser.add_argument("--skip-compile", action="store_true")
    args = parser.parse_args()

    if OUT.exists() and not args.skip_compile:
        for p in OUT.glob("*"):
            if p.is_file():
                p.unlink()
            elif p.name.endswith(".prefs"):
                shutil.rmtree(p)
    elif OUT.exists():
        for p in OUT.glob("*"):
            if p.is_file() and not p.name.endswith(".yaml"):
                p.unlink()
            elif p.name.endswith(".prefs"):
                shutil.rmtree(p)
    c = Cluster(args.esphome)
    if not args.skip_compile:
        c.generate_and_compile()
    results = []
    try:
        for name in NODES:
            c.start_sim(name)
        for name in NODES:
            c.start_node(name)
        # Test-harness robustness: very rarely the very first exchange with a freshly created
        # pseudo terminal is lost and fingerprint_grow marks the (simulated) sensor as failed.
        # That is an artefact of the simulator, so restart such a node once.
        time.sleep(4)
        for name in NODES:
            if "fingerprint_grow was marked as failed" in c.log(name):
                print(f"  note: restarting {name} (simulated sensor missed the first command)", flush=True)
                c.stop_node(name)
                c.path(name, "log").write_text("", encoding="utf-8")
                c.start_node(name)

        print("== Scenario 1: initial full sync, convergence, conflict, exclusive range, auth")
        common = {0: "admin", 1: "alice", 5: "carol"}
        results.append(wait_until(
            "A, B and C hold slots 0, 1, 5 with the same fingers",
            lambda: all(all(c.slots(n).get(s) == f for s, f in common.items()) for n in ("node-a", "node-b", "node-c")),
            90))
        sha_a, sha_b = c.sha("node-a", 2), c.sha("node-b", 2)
        results.append(wait_until(
            "slot 2 (bob, enrolled separately on A and B): conflict reported on both, nothing overwritten",
            lambda: "CONFLICT in slot 2" in c.log("node-a") and "CONFLICT in slot 2" in c.log("node-b")
            and c.sha("node-a", 2) == sha_a and c.sha("node-b", 2) == sha_b and sha_a != sha_b, 60))
        results.append(wait_until(
            "slot 2 on C (dave) was NOT overwritten and a conflict was reported",
            lambda: c.slots("node-c").get(2) == "dave" and "TEST-EVENT conflict 2" in c.log("node-c"), 60))
        results.append(wait_until(
            "node-exclusive slot 150 stays on C only",
            lambda: c.slots("node-c").get(150) == "eve" and 150 not in c.slots("node-a") and 150 not in c.slots("node-b"),
            10))
        results.append(wait_until(
            "node-d with a wrong key is rejected and gets nothing",
            lambda: "authentication failed" in c.log("node-a") and set(c.slots("node-d")) == {7}
            and 7 not in c.slots("node-a"), 60))

        results.append(wait_until(
            "dry-run node E logs planned writes but its sensor stays unchanged",
            lambda: "[dry run] would store slot 0" in c.log("node-e") and c.slots("node-e") == {9: "grace"}, 60))

        print("== Scenario 1c: slot names")
        c.cmd("node-a", "name 0 Chef")
        time.sleep(2)

        def name_on(node, slot, name):
            c.cmd(node, f"showname {slot}")
            time.sleep(1.5)
            return f"TEST-EVENT name {slot} {name}" in c.log(node)

        results.append(wait_until("a name set on A reaches B with the slot",
                                  lambda: name_on("node-b", 0, "Chef"), 60))
        c.touch("node-b", "admin")
        results.append(wait_until("last_name shows the name of the recognised finger",
                                  lambda: "Last name: Chef" in c.log("node-b"), 10))
        time.sleep(4)  # let the single admin touch close

        print("== Scenario 2: node A becomes the reference (resolve_all_conflicts keep_local)")
        c.cmd("node-a", "resolve_all 1")
        results.append(wait_until(
            "B and C replaced slot 2 with A's template (C's dave is gone, B holds A's exact bytes)",
            lambda: c.slots("node-c").get(2) == "bob" and c.sha("node-c", 2) == sha_a
            and c.sha("node-b", 2) == sha_a, 60))

        print("== Scenario 2b: node C claims all its slots (reference node without conflicts)")
        before = {n: {s: c.sha(n, s) for s in (0, 1, 2, 5)} for n in ("node-a", "node-b")}
        c.cmd("node-c", "claim_all")
        results.append(wait_until(
            "claim_all_slots on C: logged, propagated, contents of A and B unchanged (identical bytes)",
            lambda: "This node claims" in c.log("node-c")
            and "would REPLACE" not in c.log("node-a")
            and all(c.sha(n, s) == before[n][s] for n in before for s in before[n])
            and c.sha("node-a", 2) == c.sha("node-c", 2), 45))

        print("== Scenario 3: admin finger enrolls the next free slot on A, propagates")
        time.sleep(2)
        c.touch("node-a", "admin")
        time.sleep(1.5)
        c.touch("node-a", "admin")
        results.append(wait_until("on_touch fired for touch 1 and 2",
                                  lambda: "TEST-EVENT admin_touch 1" in c.log("node-a")
                                  and "TEST-EVENT admin_touch 2" in c.log("node-a"), 20))
        results.append(wait_until("enrollment started for slot 3",
                                  lambda: "TEST-EVENT enroll_started 3" in c.log("node-a"), 20))
        time.sleep(1.5)
        c.touch("node-a", "frank")
        time.sleep(1.5)
        c.touch("node-a", "frank")
        results.append(wait_until("frank enrolled in slot 3 on A", lambda: c.slots("node-a").get(3) == "frank", 30))
        results.append(wait_until("frank reached B and C",
                                  lambda: c.slots("node-b").get(3) == "frank" and c.slots("node-c").get(3) == "frank", 60))

        print("== Scenario 3b: second admin finger (boss) re-enrolls slot 3 with 4 touches")
        old_sha = c.sha("node-a", 3)
        time.sleep(3)
        c.touches("node-a", "boss", 4)
        results.append(wait_until("re-enroll waits for the target finger",
                                  lambda: "TEST-EVENT admin_waiting reenroll" in c.log("node-a"), 10))
        c.touch("node-a", "frank")  # target: slot 3
        results.append(wait_until("re-enrollment of slot 3 started",
                                  lambda: "TEST-EVENT admin_done reenroll 3" in c.log("node-a"), 10))
        time.sleep(1.5)
        c.touches("node-a", "frank", 2, gap=1.5)
        results.append(wait_until(
            "slot 3 re-enrolled on A and the new template replaced it on B and C",
            lambda: c.sha("node-a", 3) not in (None, old_sha)
            and c.sha("node-b", 3) == c.sha("node-a", 3) == c.sha("node-c", 3), 60))

        print("== Scenario 3c: guest finger with expiry")
        time.sleep(3)
        c.cmd("node-a", "enroll_guest")
        results.append(wait_until("guest enrollment started in slot 100",
                                  lambda: "TEST-EVENT enroll_started 100" in c.log("node-a"), 10))
        time.sleep(1.5)
        c.touches("node-a", "gina", 2, gap=1.5)
        results.append(wait_until("guest gina reached A, B and C",
                                  lambda: all(c.slots(n).get(100) == "gina" for n in ("node-a", "node-b", "node-c")),
                                  45))
        results.append(wait_until("guest expired and was deleted on all nodes",
                                  lambda: all(100 not in c.slots(n) for n in ("node-a", "node-b", "node-c")), 120))

        print("== Scenario 3e: guest with a number of uses")
        time.sleep(3)
        c.cmd("node-a", "enroll_guest_uses 2")
        results.append(wait_until("guest enrollment with 2 uses started",
                                  lambda: "2 use(s)" in c.log("node-a"), 10))
        found = re.search(r"Enrolling guest in slot (\d+)[^\n]*2 use\(s\)", c.log("node-a"))
        gslot = int(found.group(1)) if found else -1
        time.sleep(1.5)
        c.touches("node-a", "gus", 2, gap=1.5)
        results.append(wait_until("guest gus reached A, B and C",
                                  lambda: all(c.slots(n).get(gslot) == "gus" for n in ("node-a", "node-b", "node-c")),
                                  45))
        time.sleep(3)
        c.touch("node-a", "gus")
        results.append(wait_until("first use counted on A",
                                  lambda: f"Guest in slot {gslot}: 1 use(s) left" in c.log("node-a"), 10))

        def uses_on(node, slot, n):
            c.cmd(node, f"showuses {slot}")
            time.sleep(1.5)
            return f"TEST-EVENT uses {slot} {n}" in c.log(node)

        results.append(wait_until("the remaining uses reach B", lambda: uses_on("node-b", gslot, 1), 45))
        time.sleep(11)  # one use per 10 s and slot
        c.touch("node-b", "gus")  # the last use may happen at any node
        results.append(wait_until("the last use deletes the guest on A, B and C",
                                  lambda: all(gslot not in c.slots(n) for n in ("node-a", "node-b", "node-c")), 60))

        print("== Scenario 3d: admin fingers are protected")
        c.cmd("node-a", "delete 9")
        time.sleep(3)
        c.touches("node-b", "admin", 3)
        results.append(wait_until("delete gesture waits for the target finger",
                                  lambda: "TEST-EVENT admin_waiting delete" in c.log("node-b"), 10))
        c.touch("node-b", "boss")  # an admin finger as target aborts
        results.append(wait_until(
            "admin slots cannot be deleted (action refused, gesture cancelled)",
            lambda: "protected" in c.log("node-a") and "TEST-EVENT admin_cancelled" in c.log("node-b")
            and all(c.slots(n).get(9) == "boss" and c.slots(n).get(0) == "admin" for n in ("node-a", "node-b")),
            15))

        print("== Scenario 4: delete on B propagates")
        c.cmd("node-b", "delete 1")
        results.append(wait_until("slot 1 deleted on A, B, C",
                                  lambda: all(1 not in c.slots(n) for n in ("node-a", "node-b", "node-c")), 60))

        print("== Scenario 5: node offline, catches up after restart; tombstones persist")
        c.stop_node("node-c")
        c.cmd("node-a", "delete 5")
        results.append(wait_until("slot 5 deleted on A and B",
                                  lambda: 5 not in c.slots("node-a") and 5 not in c.slots("node-b"), 60))
        time.sleep(3)
        c.start_node("node-c")
        results.append(wait_until("restarted C deleted slot 5 and kept slot 1 deleted",
                                  lambda: 5 not in c.slots("node-c") and 1 not in c.slots("node-c"), 90))
        c.stop_node("node-a")
        time.sleep(1)
        c.start_node("node-a")
        time.sleep(20)
        results.append(wait_until("the name survives the restart of A",
                                  lambda: name_on("node-a", 0, "Chef"), 20))
        results.append(wait_until("after restart of A nothing deleted came back",
                                  lambda: all(s not in c.slots(n) for n in ("node-a", "node-b", "node-c") for s in (1, 5)),
                                  20))

        print("== Scenario 6a: encrypted backup of node A with tools/growplus.py")
        backup = OUT / "node-a.fgpb"
        tool = [sys.executable, str(ROOT / "tools" / "growplus.py")]
        env = dict(os.environ, GROWPLUS_KEY=KEY)
        res = subprocess.run(tool + ["backup", "--host", "127.0.0.1", "--port", "18531", "--out", str(backup)],
                             capture_output=True, text=True, env=env, check=False)
        print(res.stdout + res.stderr)
        results.append(wait_until("the backup keeps slot names", lambda: "slot 0: 1536 bytes  Chef" in res.stdout, 1))
        results.append(wait_until("backup contains slots 0, 2, 3",
                                  lambda: res.returncode == 0 and all(f"  slot {n}:" in res.stdout for n in (0, 2, 3)), 1))
        raw = backup.read_bytes() if backup.exists() else b""
        results.append(wait_until("backup file is encrypted (no template bytes in clear)",
                                  lambda: b"frank" not in raw and b"admin" not in raw and len(raw) > 100, 1))

        print("== Scenario 6: delete_all needs the code, then wipes synced slots everywhere")
        c.cmd("node-a", "delete_all_wrong")
        time.sleep(5)
        results.append(wait_until("wrong code changes nothing", lambda: c.slots("node-a").get(3) == "frank", 5))
        c.cmd("node-a", "delete_all")
        results.append(wait_until(
            "all synced slots gone on A, B, C except the admin fingers; C's exclusive slot 150 survives",
            lambda: c.slots("node-a") == {0: "admin", 9: "boss"} and c.slots("node-b") == {0: "admin", 9: "boss"}
            and c.slots("node-c") == {0: "admin", 9: "boss", 150: "eve"}, 90))

        print("== Scenario 7: restore (rollback) from the backup, propagated to all nodes")
        res = subprocess.run(tool + ["restore", "--host", "127.0.0.1", "--port", "18531", "--in", str(backup)],
                             capture_output=True, text=True, env=env, check=False)
        results.append(wait_until("restore is refused while the restore window is closed",
                                  lambda: res.returncode != 0 and "allow_restore" in (res.stdout + res.stderr), 1))
        c.cmd("node-a", "allow_restore")
        time.sleep(2)
        res = subprocess.run(tool + ["restore", "--host", "127.0.0.1", "--port", "18531", "--in", str(backup)],
                             capture_output=True, text=True, env=env, check=False)
        print(res.stdout + res.stderr)
        expected = {0: "admin", 2: "bob", 3: "frank"}
        results.append(wait_until("restored slots are back on A, B and C",
                                  lambda: all(all(c.slots(n).get(s) == f for s, f in expected.items())
                                              for n in ("node-a", "node-b", "node-c")), 90))
        res = subprocess.run(tool + ["status", "--host", "127.0.0.1", "--port", "18532"],
                             capture_output=True, text=True, env=env, check=False)
        results.append(wait_until("status command lists node B's slots", lambda: "slot    3" in res.stdout, 1))

        print("== Scenario 8: hostile peers are cut off (handshake timeout, duplicate manifest)")
        import socket as _socket
        sys.path.insert(0, str(ROOT / "tools"))
        import growplus  # noqa: E402

        def silent_connection_closed():
            sock = _socket.create_connection(("127.0.0.1", 18532), timeout=12)
            start = time.time()
            try:
                data = sock.recv(16)
            except OSError:
                data = b""
            sock.close()
            return data == b"" and time.time() - start < 11

        results.append(wait_until("a connection that never sends a handshake is closed after ~5 s",
                                  silent_connection_closed, 1))
        time.sleep(1)

        def duplicate_manifest_rejected():
            sock = _socket.create_connection(("127.0.0.1", 18532), timeout=10)
            ch = growplus.Channel(sock, __import__("base64").b64decode(KEY), 0x1234)
            ch.handshake()
            empty = bytes([growplus.MSG_MANIFEST, 0, 1, 0, 200, 0, 0, 0, 0])  # capacity, epoch, count
            ch.send(empty)
            ch.send(empty)
            try:
                while True:
                    ch.recv()
            except (ConnectionError, OSError):
                pass
            sock.close()
            return "manifest sent twice" in c.log("node-b")

        results.append(wait_until("a peer sending its manifest twice is disconnected",
                                  duplicate_manifest_rejected, 1))

        print("== Scenario 8b: diagnostics - unreachable peers, versions left, incompatible template")
        results.append(wait_until("A names the peer it could not sync with (node D, wrong key)",
                                  lambda: "Unreachable peers: 127.0.0.1 (" in c.log("node-a"), 5))
        results.append(wait_until("A reports the versions left below the maximum",
                                  lambda: any(l.split("Sync versions left: ")[1].split("\x1b")[0].strip().isdigit()
                                              and int(l.split("Sync versions left: ")[1].split("\x1b")[0]) < 16383
                                              for l in c.log("node-a").splitlines() if "Sync versions left: " in l), 5))
        c.cmd("node-b", "allow_restore")
        time.sleep(2)

        def incompatible_restore_refused():
            sock = _socket.create_connection(("127.0.0.1", 18532), timeout=10)
            ch = growplus.Channel(sock, base64.b64decode(KEY), 0x2345)
            ch.handshake()
            ch.send(bytes([growplus.MSG_MANIFEST, 0, 1]) + struct.pack(">HHH", 0, 0, 0))
            growplus.read_manifest(ch)
            tpl = bytes(768)  # half the size of the simulated sensor's templates
            ch.send(bytes([growplus.MSG_RESTORE]) + struct.pack(">H", 120) + growplus.pack_extra({})
                    + struct.pack(">H", len(tpl)) + tpl)
            while True:
                msg = ch.recv()
                if msg[0] == growplus.MSG_RESTORE_ACK:
                    break
            sock.close()
            return msg[3] == 1

        results.append(wait_until("a template of a different size is refused with a clear log line",
                                  lambda: incompatible_restore_refused()
                                  and "Incompatible template from" in c.log("node-b")
                                  and 120 not in c.slots("node-b"), 1))

        print("== Scenario 9: admin deletes a finger by presenting it (3 touches + finger)")
        time.sleep(3)
        c.touches("node-a", "admin", 3)
        results.append(wait_until("delete gesture armed",
                                  lambda: c.log("node-a").count("TEST-EVENT admin_waiting delete") >= 1, 10))
        c.touch("node-a", "frank")
        results.append(wait_until("frank (slot 3) deleted on A, B and C by the admin gesture",
                                  lambda: all(3 not in c.slots(n) for n in ("node-a", "node-b", "node-c")), 60))

        print("== Scenario 9b: move a finger to another slot")
        before = c.slots("node-a").get(2)
        c.cmd("node-a", "move 2 33")
        results.append(wait_until("the finger moved from slot 2 to 33 on A, B and C",
                                  lambda: before is not None and all(c.slots(n).get(33) == before and 2 not in c.slots(n)
                                                                     for n in ("node-a", "node-b", "node-c")), 60))
        c.cmd("node-a", "move 0 34")
        time.sleep(3)
        results.append(wait_until("an admin slot cannot be moved",
                                  lambda: "admin slots cannot be moved" in c.log("node-a") and 34 not in c.slots("node-a"), 1))

        print("== Scenario 10: stand-alone node (no sync: block) enrolls with the admin gesture")
        results.append(wait_until("stand-alone node reports the network sync as off",
                                  lambda: "Sync: off" in c.log("node-f"), 5))
        c.touch("node-f", "admin")
        results.append(wait_until("a single admin touch fires on_access once the gesture is complete",
                                  lambda: c.log("node-f").count("TEST-EVENT admin_access 0") == 1, 10))
        time.sleep(1)
        c.touch("node-f", "admin")
        time.sleep(1.5)
        c.touch("node-f", "admin")
        results.append(wait_until("enrollment started for slot 1 on the stand-alone node",
                                  lambda: "TEST-EVENT enroll_started 1" in c.log("node-f"), 20))
        time.sleep(1.5)
        c.touches("node-f", "zoe", 2, gap=1.5)
        results.append(wait_until("zoe enrolled in slot 1 on the stand-alone node",
                                  lambda: c.slots("node-f").get(1) == "zoe", 30))
        def led_seq(name, *expected):
            """True if the LED commands (state, colour) appear in this order in the simulator log."""
            got = [line.strip() for line in c.path(name, "sim.log").read_text(encoding="utf-8").splitlines()
                   if line.startswith("LED ")]
            pos = 0
            for item in got:
                if pos < len(expected) and item == expected[pos]:
                    pos += 1
            return pos == len(expected)

        # touch 1 cyan, touch 2 purple (preview), gesture -> off, enrolling flashes purple,
        # done flashes green, then off again
        results.append(wait_until("led_feedback: preview, enrolling, success and idle colours",
                                  lambda: led_seq("node-f", "LED 3 6", "LED 3 3", "LED 4 2", "LED 2 3",
                                                  "LED 2 4", "LED 4 2"), 10))
        results.append(wait_until("two admin touches open the menu without access",
                                  lambda: c.log("node-f").count("TEST-EVENT admin_access") == 1, 1))
        results.append(wait_until("the stand-alone finger stays local",
                                  lambda: all(c.slots(n).get(1) != "zoe" for n in ("node-a", "node-b", "node-c")), 1))

        print("== Scenario 10b: holding the admin finger shows the status and never opens")
        access_before = c.log("node-f").count("TEST-EVENT admin_access")
        time.sleep(4)
        c.cmd("node-f", "finger_on")
        time.sleep(0.5)
        c.touch("node-f", "admin")
        results.append(wait_until("hold detected after 3 s", lambda: "TEST-EVENT admin_hold" in c.log("node-f"), 10))
        c.touch("node-f", "admin")  # the sensor matches the resting finger a second time
        time.sleep(2)
        c.cmd("node-f", "finger_off")
        time.sleep(5)
        results.append(wait_until("led_feedback: hold shows the status colour (green = all fine)",
                                  lambda: led_seq("node-f", "LED 3 6", "LED 3 4"), 1))
        results.append(wait_until("a hold (with a second match) triggers no access",
                                  lambda: c.log("node-f").count("TEST-EVENT admin_access") == access_before
                                  and "matched again during a hold: ignored" in c.log("node-f"), 1))
        c.touch("node-f", "admin")
        results.append(wait_until("afterwards a single short touch opens again",
                                  lambda: c.log("node-f").count("TEST-EVENT admin_access") == access_before + 1, 10))

        print("== Scenario 10c: time windows for slot ranges")
        for slot in (45, 55, 7, 65, 75):
            c.cmd("node-f", f"allowed {slot}")
            time.sleep(1)
        results.append(wait_until("is_allowed: open / closed window, no schedule, windows over midnight",
                                  lambda: all(f"TEST-EVENT allowed {sl} {v}" in c.log("node-f")
                                              for sl, v in ((45, 1), (55, 0), (7, 1), (65, 1), (75, 0))), 5))
        c.touch("node-f", "dora")
        results.append(wait_until("a finger outside its window fires on_denied",
                                  lambda: "TEST-EVENT denied 55" in c.log("node-f"), 10))
        time.sleep(3)
        c.touch("node-f", "alma")
        time.sleep(4)
        results.append(wait_until("a finger inside its window does not",
                                  lambda: "TEST-EVENT denied 45" not in c.log("node-f"), 1))

        print("== Scenario 11: node syncing only the admin slot, local features on all other slots")
        results.append(wait_until("node-g received the admin finger (slot 0) from A",
                                  lambda: c.slots("node-g").get(0) == "admin", 60))
        results.append(wait_until("G's local slot 1 (gina) is kept, not synced and never a conflict",
                                  lambda: c.slots("node-g").get(1) == "gina" and c.slots("node-a").get(1) != "gina"
                                  and "CONFLICT" not in c.log("node-g"), 5))
        c.touch("node-g", "admin")
        time.sleep(1.5)
        c.touch("node-g", "admin")
        results.append(wait_until("admin gesture on G enrolls the next free local slot (2)",
                                  lambda: "TEST-EVENT enroll_started 2" in c.log("node-g"), 20))
        time.sleep(1.5)
        c.touches("node-g", "hank", 2, gap=1.5)
        results.append(wait_until("hank enrolled in slot 2 on G",
                                  lambda: c.slots("node-g").get(2) == "hank", 30))
        time.sleep(20)  # at least one more round between G and A
        results.append(wait_until("occupied_slots text sensor on G shows 0-2",
                                  lambda: "Occupied slots: 0-2" in c.log("node-g"), 10))
        results.append(wait_until("G's local fingers never reach A",
                                  lambda: all(f not in c.slots("node-a").values() for f in ("gina", "hank")), 1))

        c.cmd("node-g", "enroll_named 2 Ivo")
        results.append(wait_until("enroll_next_free with a name starts an enrollment",
                                  lambda: "TEST-EVENT enroll_started 3" in c.log("node-g"), 15))
        time.sleep(1.5)
        c.touches("node-g", "ivo", 2, gap=1.5)
        results.append(wait_until("the new finger carries its name",
                                  lambda: c.slots("node-g").get(3) == "ivo" and name_on("node-g", 3, "Ivo"), 30))

        print("== Scenario 11b: backup and restore of local-only slots (--include-local)")
        g_port = str(NODES["node-g"]["port"])

        def g_tool(*extra):
            r = subprocess.run(tool + list(extra) + ["--host", "127.0.0.1", "--port", g_port],
                               capture_output=True, text=True, env=env, check=False)
            print(r.stdout + r.stderr)
            return r

        res = g_tool("backup", "--out", str(OUT / "node-g-synced.fgpb"))
        results.append(wait_until("default backup of G holds only the synced slot 0",
                                  lambda: res.returncode == 0 and "  slot 0:" in res.stdout
                                  and "  slot 1:" not in res.stdout, 1))
        res = g_tool("backup", "--include-local", "--out", str(OUT / "node-g-refused.fgpb"))
        results.append(wait_until("--include-local is refused while the full backup window is closed",
                                  lambda: res.returncode != 0 and "allow_full_backup" in (res.stdout + res.stderr)
                                  and not (OUT / "node-g-refused.fgpb").exists(), 1))
        c.cmd("node-g", "allow_full_backup")
        time.sleep(2)
        full = OUT / "node-g-full.fgpb"
        res = g_tool("backup", "--include-local", "--out", str(full))
        results.append(wait_until("full backup of G holds slot 0 and the local slots 1 and 2",
                                  lambda: res.returncode == 0 and "  slot 0:" in res.stdout
                                  and "  slot 1: 1536 bytes (local only)" in res.stdout
                                  and "  slot 2: 1536 bytes (local only)" in res.stdout, 1))
        results.append(wait_until("node logs the full backup with synced and local counts",
                                  lambda: "template(s) sent (1 synced, 3 local-only)" in c.log("node-g"), 5))

        # The permission belongs to that one session: a later session must not get local slots,
        # even while the window is still open, unless it asks for them itself.
        def later_session_gets_local_slot():
            sock = _socket.create_connection(("127.0.0.1", int(g_port)), timeout=10)
            ch = growplus.Channel(sock, base64.b64decode(KEY), 0x1235)
            ch.handshake()
            ch.send(bytes([growplus.MSG_MANIFEST, 0, 1]) + struct.pack(">HHH", 0, 0, 0))
            growplus.read_manifest(ch)
            ch.send(bytes([growplus.MSG_TEMPLATE_REQ]) + struct.pack(">H", 1))
            while True:
                msg = ch.recv()
                if msg[0] in (growplus.MSG_TEMPLATE, growplus.MSG_TEMPLATE_NAK):
                    break
            sock.close()
            return msg[0] == growplus.MSG_TEMPLATE

        time.sleep(2)
        results.append(wait_until("a later session cannot read local slots without asking",
                                  lambda: not later_session_gets_local_slot(), 1))
        c.cmd("node-g", "delete 1")
        results.append(wait_until("local slot 1 deleted on G", lambda: 1 not in c.slots("node-g"), 15))
        c.cmd("node-g", "allow_restore")
        time.sleep(2)
        res = g_tool("restore", "--in", str(full), "--slots", "1")
        results.append(wait_until("local slot 1 restored on G and still not synced",
                                  lambda: c.slots("node-g").get(1) == "gina"
                                  and "gina" not in c.slots("node-a").values(), 20))

        print("== Scenario 12: interval never, no sync at boot or after changes, manual sync works")
        results.append(wait_until("node-h did not sync on its own (no admin finger from A yet)",
                                  lambda: 0 not in c.slots("node-h") and "Interval: never" in c.log("node-h")
                                  and "Sync with" not in c.log("node-h"), 1))
        c.cmd("node-h", "sync")
        results.append(wait_until("manual sync: H receives A's fingers (admin in slot 0)",
                                  lambda: c.slots("node-h").get(0) == "admin", 60))
        time.sleep(3)
        c.touch("node-h", "admin")
        time.sleep(1.5)
        c.touch("node-h", "admin")
        results.append(wait_until("admin gesture on H starts an enrollment",
                                  lambda: "TEST-EVENT enroll_started" in c.log("node-h"), 20))
        time.sleep(1.5)
        c.touches("node-h", "hugo", 2, gap=1.5)
        results.append(wait_until("hugo enrolled on H", lambda: "hugo" in c.slots("node-h").values(), 30))
        time.sleep(15)
        results.append(wait_until("after_change: false - the new finger is not pushed by itself",
                                  lambda: "hugo" not in c.slots("node-a").values()
                                  and c.log("node-h").count("Sync with") == 1, 1))
        c.cmd("node-h", "sync")
        results.append(wait_until("the next manual sync brings hugo to A",
                                  lambda: "hugo" in c.slots("node-a").values(), 60))

        print("== Scenario 13: lockout shared with selected peers, duress finger")
        for i in range(3):
            c.touch("node-b", f"stranger{i}")
            time.sleep(2.5)
        results.append(wait_until("B locks itself after 3 unrecognised fingers",
                                  lambda: "TEST-EVENT locked local" in c.log("node-b"), 15))
        results.append(wait_until("the lockout reaches C (share_with) ...",
                                  lambda: "TEST-EVENT locked remote" in c.log("node-c"), 30))
        results.append(wait_until("... but not A (not in share_with)",
                                  lambda: "Locked for" not in c.log("node-a") and "requested a lockout" not in c.log("node-a"), 1))
        results.append(wait_until("both unlock again after the duration",
                                  lambda: "TEST-EVENT unlocked" in c.log("node-b") and "TEST-EVENT unlocked" in c.log("node-c"), 60))
        c.touch("node-c", "eve")  # slot 150 on C is in the duress range
        results.append(wait_until("the duress finger fires on_duress",
                                  lambda: "TEST-EVENT duress 150" in c.log("node-c"), 15))

        print("== Scenario 14: version reset")
        c.cmd("node-a", "reset_versions")
        results.append(wait_until("A may not reset: its last round did not reach node D (wrong key)",
                                  lambda: "Version reset refused: the last sync round did not reach every peer"
                                  in c.log("node-a"), 10))
        before = {n: c.slots(n) for n in ("node-a", "node-b", "node-c")}
        conflicts_before = sum(c.log(n).count("CONFLICT in slot") for n in ("node-a", "node-b", "node-c"))
        c.cmd("node-b", "sync")
        time.sleep(10)
        c.cmd("node-b", "reset_versions")
        results.append(wait_until("B resets (epoch 1), A and C follow",
                                  lambda: "Version reset: epoch 1" in c.log("node-b")
                                  and "Version reset by peer" in c.log("node-a")
                                  and "Version reset by peer" in c.log("node-c"), 60))
        results.append(wait_until("versions_left is back near the maximum on B",
                                  lambda: "Sync versions left: 16382" in c.log("node-b"), 10))
        c.cmd("node-a", "sync")
        time.sleep(20)
        results.append(wait_until("after the reset the same fingers are in the same slots, no new conflicts",
                                  lambda: {n: c.slots(n) for n in ("node-a", "node-b", "node-c")} == before
                                  and sum(c.log(n).count("CONFLICT in slot") for n in ("node-a", "node-b", "node-c"))
                                  == conflicts_before, 30))
        c.cmd("node-b", "delete 33")
        results.append(wait_until("after the reset changes propagate again (deletion on B reaches A and C)",
                                  lambda: all(33 not in c.slots(n) for n in ("node-a", "node-b", "node-c")), 60))

        sim_logs = "".join(c.path(n, "sim.log").read_text(encoding="utf-8") for n in NODES)
        results.append(wait_until("no SetPwd was ever sent to a sensor", lambda: "SetPwd" not in sim_logs, 1))
    finally:
        if not args.keep:
            c.stop_all()

    passed = sum(1 for r in results if r)
    print(f"== {passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
