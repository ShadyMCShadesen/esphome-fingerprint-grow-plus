#!/usr/bin/env python3
"""End-to-end test: several fingerprint_sync nodes on the ESPHome host platform.

Each node is the real firmware (ESPHome host build) talking to a simulated R503
(tests/sim/r503_sim.py) through a pseudo terminal. Nodes sync over TCP on localhost.

Usage:  python3 tests/e2e/run_e2e.py [--esphome /path/to/esphome] [--keep]
Exit code 0 = all scenarios passed.
"""

import argparse
import json
import re
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "e2e" / ".out"
SIM = ROOT / "tests" / "sim" / "r503_sim.py"

KEY = "q83vEjRWeJCrze8SNFZ4kKvN7xI0VniQq83vEjRWeJA="
WRONG_KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="

NODES = {
    "node-a": {"port": 18531, "peers": ["node-b", "node-c", "node-d"], "include": ["0-199"], "exclude": []},
    "node-b": {"port": 18532, "peers": ["node-a", "node-c"], "include": ["0-199"], "exclude": []},
    # node-c keeps 150-199 for itself (node-exclusive range)
    "node-c": {"port": 18533, "peers": ["node-a", "node-b"], "include": ["0-199"], "exclude": ["150-199"]},
    # node-d has the wrong key and must be rejected
    "node-d": {"port": 18534, "peers": ["node-a"], "include": ["0-199"], "exclude": [], "key": WRONG_KEY},
    # node-e runs in dry-run mode: it must log what it would do and change nothing
    "node-e": {"port": 18535, "peers": ["node-a"], "include": ["0-199"], "exclude": [], "dry_run": True},
}

SEEDS = {
    "node-a": {"0": {"finger": "admin"}, "1": {"finger": "alice"}, "2": {"finger": "bob", "noise": "bob-on-a"}},
    "node-b": {"2": {"finger": "bob", "noise": "bob-on-b"}, "5": {"finger": "carol"}},
    "node-c": {"2": {"finger": "dave"}, "150": {"finger": "eve"}},
    "node-d": {"7": {"finger": "mallory"}},
    "node-e": {"9": {"finger": "grace"}},
}

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
    components: [fingerprint_sync]
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
  - platform: fingerprint_sync
    peers_reachable:
      name: Peers reachable
    conflict_count:
      name: Sync conflicts
text_sensor:
  - platform: fingerprint_sync
    status:
      name: Sync status
fingerprint_sync:
  id: fp_sync
  key: "{key}"
  port: {port}
  peers: {peers}
  include_slots: {include}
  exclude_slots: {exclude}
  sync_interval: 15s
  dry_run: {dry_run}
  startup_delay: 2s
  fingerprint_count_sensor: fp_count
  fingerprint_capacity_sensor: fp_capacity
  delete_all_code: TEST_ALL
  admin:
    slot: 0
    touches: 2
    window: 6s
    num_scans: 2
  on_sync_done:
    - logger.log: "TEST-EVENT sync_done"
  on_sync_failed:
    - logger.log:
        format: "TEST-EVENT sync_failed %s"
        args: [reason.c_str()]
  on_conflict:
    - logger.log:
        format: "TEST-EVENT conflict %u"
        args: [slot]
  on_admin_touch:
    - logger.log:
        format: "TEST-EVENT admin_touch %u"
        args: [touches]
  on_enroll_started:
    - logger.log:
        format: "TEST-EVENT enroll_started %u"
        args: [slot]
interval:
  - interval: 300ms
    then:
      - lambda: |-
          FILE *f = fopen("{cmd}", "r");
          if (f == nullptr) return;
          char cmd[32] = {{0}};
          int a = 0, b = 0;
          int n = fscanf(f, "%31s %d %d", cmd, &a, &b);
          fclose(f);
          remove("{cmd}");
          if (n < 1) return;
          std::string c(cmd);
          if (c == "delete") id(fp_sync).delete_slot(a);
          else if (c == "resolve") id(fp_sync).resolve_conflict(a, b != 0);
          else if (c == "sync") id(fp_sync).sync_now();
          else if (c == "delete_all") id(fp_sync).delete_all("TEST_ALL");
          else if (c == "delete_all_wrong") id(fp_sync).delete_all("nope");
          else if (c == "allow_restore") id(fp_sync).allow_restore(120);
          else if (c == "self_test") id(fp_sync).self_test(a, b);
          else if (c == "resolve_all") id(fp_sync).resolve_all_conflicts(a != 0);
"""


class Cluster:
    def __init__(self, esphome: str):
        self.esphome = esphome
        self.procs: dict[str, subprocess.Popen] = {}
        self.sims: dict[str, subprocess.Popen] = {}

    def path(self, name, suffix):
        if suffix == "pty":
            # The host UART only accepts a device path directly below a top-level directory.
            return Path(f"/tmp/fpsync-e2e-{name}.pty")
        return OUT / f"{name}.{suffix}"

    def generate_and_compile(self):
        OUT.mkdir(parents=True, exist_ok=True)
        for name, cfg in NODES.items():
            peers = [f"127.0.0.1:{NODES[p]['port']}" for p in cfg["peers"]]
            yaml = YAML_TEMPLATE.format(
                name=name,
                components=ROOT / "components",
                pty=self.path(name, "pty"),
                cmd=self.path(name, "cmd"),
                key=cfg.get("key", KEY),
                port=cfg["port"],
                peers=json.dumps(peers),
                include=json.dumps(cfg["include"]),
                exclude=json.dumps(cfg["exclude"]),
                dry_run="true" if cfg.get("dry_run") else "false",
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

        print("== Scenario 1b: sensor self test (buffers only)")
        c.cmd("node-a", "self_test 0 1")
        results.append(wait_until(
            "self test: template round trip through the host is byte-identical",
            lambda: "SELF TEST 1: Match (no download)" in c.log("node-a")
            and len(re.findall(r"round trip BYTE-IDENTICAL", c.log("node-a"))) == 3
            and "SELF TEST 5: slot 0 vs slot 1" in c.log("node-a")
            and "-- self test finished" in c.log("node-a"), 60))

        print("== Scenario 2: node A becomes the reference (resolve_all_conflicts keep_local)")
        c.cmd("node-a", "resolve_all 1")
        results.append(wait_until(
            "B and C replaced slot 2 with A's template (C's dave is gone, B holds A's exact bytes)",
            lambda: c.slots("node-c").get(2) == "bob" and c.sha("node-c", 2) == sha_a
            and c.sha("node-b", 2) == sha_a, 60))

        print("== Scenario 3: admin finger enrolls the next free slot on A, propagates")
        time.sleep(2)
        c.touch("node-a", "admin")
        time.sleep(1.5)
        c.touch("node-a", "admin")
        results.append(wait_until("on_admin_touch fired for touch 1 and 2",
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
        results.append(wait_until("after restart of A nothing deleted came back",
                                  lambda: all(s not in c.slots(n) for n in ("node-a", "node-b", "node-c") for s in (1, 5)),
                                  20))

        print("== Scenario 6a: encrypted backup of node A with tools/fpsync.py")
        backup = OUT / "node-a.fpsb"
        tool = [sys.executable, str(ROOT / "tools" / "fpsync.py")]
        env = dict(os.environ, FPSYNC_KEY=KEY)
        res = subprocess.run(tool + ["backup", "--host", "127.0.0.1", "--port", "18531", "--out", str(backup)],
                             capture_output=True, text=True, env=env, check=False)
        print(res.stdout + res.stderr)
        results.append(wait_until("backup contains slots 0, 2, 3",
                                  lambda: res.returncode == 0 and all(f"  slot {n}:" in res.stdout for n in (0, 2, 3)), 1))
        raw = backup.read_bytes() if backup.exists() else b""
        results.append(wait_until("backup file is encrypted (no template bytes in clear)",
                                  lambda: b"frank" not in raw and b"admin" not in raw and len(raw) > 100, 1))

        print("== Scenario 6: delete_all needs the code, then wipes managed slots everywhere")
        c.cmd("node-a", "delete_all_wrong")
        time.sleep(5)
        results.append(wait_until("wrong code changes nothing", lambda: c.slots("node-a").get(3) == "frank", 5))
        c.cmd("node-a", "delete_all")
        results.append(wait_until(
            "all managed slots gone on A, B, C; C's exclusive slot 150 survives",
            lambda: not c.slots("node-a") and not c.slots("node-b") and c.slots("node-c") == {150: "eve"}, 90))

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
