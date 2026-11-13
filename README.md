# esphome-fingerprint-grow-plus

Extra features for ESPHome's [`fingerprint_grow`](https://esphome.io/components/fingerprint_grow/)
(GROW R503, R30x, …), all without Home Assistant:

- **Admin fingers** run the sensor: enroll new fingers, delete or re-enroll a finger by showing
  it, show a status colour.
- **Guest fingers** expire after a set time and are deleted automatically.
- **Protected slots**: admin fingers cannot be deleted by accident.
- **Sync** *(optional)*: several ESPHome nodes keep the same fingers in the same slots. Enroll once,
  open everywhere. The nodes talk directly to each other, encrypted; no broker, no cloud.

> **Status: beta.** Compiled in CI for ESP32 (Arduino, ESP-IDF) and ESP8266, tested end-to-end
> with simulated sensors, running on real ESP32-S2 + R503 hardware (several synced nodes).

## How it works

```
 fingerprint_grow        (official component: matching, enrollment, LEDs - unchanged)
 fingerprint_grow_plus   (this component, same UART)
   ├─ admin:   gestures with an admin finger
   ├─ guests:  fingers with an expiry date
   └─ sync:    ◄── TCP 18530, encrypted ──►  other nodes   (optional)
```

`fingerprint_grow_plus` talks to the same sensor as `fingerprint_grow`, between its scans. Each
part is switched on by its own block in the YAML; without `sync:` nothing leaves the node and no
network port is opened.

## Quick start

### One node

```yaml
external_components:
  - source: github://ShadyMCShadesen/esphome-fingerprint-grow-plus@v0.2.0   # or @main for the latest state
    components: [fingerprint_grow_plus]

fingerprint_grow_plus:
  finger_sensor: finger_detected   # binary_sensor on the sensor's touch (wake-up) pin
  admin:                           # admin finger in slot 0
```

1. Enroll your admin finger once into slot 0 (e.g. `fingerprint_grow.enroll`).
2. Touch it twice, lift it and wait 2 s: the next free slot is enrolled.
3. Keep admin fingers out of your door action (see [Admin fingers](#admin-fingers)).

A complete example: [`examples/standalone.yaml`](examples/standalone.yaml).

### Several nodes with sync

1. Create a key once and put it into `secrets.yaml` (the same key for all nodes):

   ```bash
   python3 tools/growplus.py genkey
   ```

   ```yaml
   fingerprint_grow_plus_key: "<output of genkey>"
   ```

2. Add a `sync:` block to every node:

   ```yaml
   fingerprint_grow_plus:
     finger_sensor: finger_detected
     admin:
     sync:
       key: !secret fingerprint_grow_plus_key
       peers:                 # all nodes, the same list everywhere; a node skips itself
         - reader-garden.local
         - reader-garage.local
       dry_run: true          # first run: only log what would happen
   ```

3. Flash all nodes and read the logs: `Index ready`, `Sync with … finished`,
   `[dry run] would store slot …`, possibly `CONFLICT in slot …`.
4. Back up every sensor ([Backup](#backup-and-restore)), then set `dry_run: false`.

If the sensors already hold fingers that were enrolled separately, read
[First rollout](#first-rollout-on-sensors-that-already-hold-fingers) first.
Complete examples: [`examples/`](examples/) (ESP32 Arduino, ESP32 ESP-IDF, ESP8266).

## Admin fingers

```yaml
fingerprint_grow_plus:
  finger_sensor: finger_detected
  admin:
    slots: [0, 1]        # default: [0]
```

Touch the admin finger, lift it, and touch again; the gesture is complete when no finger has been
on the sensor for 2 s (`window`):

| Admin finger | Effect |
|---|---|
| 2× | enroll the next free slot |
| 3×, then a finger | that finger is deleted (with sync: on all nodes) |
| 4×, then a finger | that finger is enrolled again into its own slot |
| 5× (only with `guests:`) | enroll a guest finger |
| keep it on for 3 s | `on_hold`: show the status, see `health()` |
| 1× (only with `access:`) | `on_access`: behave like a normal finger, see below |

Showing an admin finger instead of the target finger, or no finger within 10 s
(`target_timeout`), cancels the action.

**Holding** only shows the status: while the finger rests on the sensor and until it has been off
for `window`, further matches of the admin finger are ignored, so a hold never ends in a gesture
or in `on_access`. Short drop-outs (< 0.5 s) of the touch signal do not count as lifting.

**Protection**: admin slots are never chosen as a free slot, cannot be deleted by a gesture or by
`fingerprint_grow_plus.delete`, and survive `delete_all`. Only `fingerprint_grow_plus.delete_admin`
with the delete code removes one. With sync, use the same admin slots on all nodes.

**Keep admin fingers out of your door action.** `fingerprint_grow` still reports every match,
including admin fingers and the finger shown during an admin action:

```yaml
fingerprint_grow:
  on_finger_scan_matched:
    - if:
        condition:
          not:
            or:
              - fingerprint_grow_plus.is_admin_slot: !lambda return finger_id;
              - fingerprint_grow_plus.is_admin_busy:
        then:
          - script.execute: open_door
```

**Admin finger that also opens** (`access:`): one touch fires `on_access`, two or more touches
open the admin menu.

```yaml
fingerprint_grow_plus:
  admin:
    access: delayed      # or immediate
    on_access:           # variables: finger_id, confidence
      - script.execute: open_door
```

- `delayed`: fires once the gesture is complete and it stayed one touch, about 2 s after lifting.
  Using the admin menu never opens the door.
- `immediate`: fires at once on the first touch. A second touch opens the menu, but the door has
  already opened.

**LED feedback** (optional): with `led_feedback:` the component drives the sensor's ring LED itself.

```yaml
fingerprint_grow_plus:
  led_feedback:
    on_idle:                 # optional: restore your own LED afterwards (otherwise: LED off)
      - script.execute: my_idle_led
```

| Moment | LED |
|---|---|
| admin finger touched 1× / 2× / 3× / 4× / 5× (preview of the action) | steady cyan / purple / red / yellow / white |
| waiting for the finger to delete / to enroll again | flashing red / yellow |
| enrolling (new finger / again / guest) | flashing purple / yellow / white |
| enrolled, deleted | green flashes 1.5 s |
| failed, cancelled | red flashes 1.5 s |
| admin finger held: status (`health()`) | steady 3 s: green = all fine, blue = sync running, yellow = conflict, red = error |

Only enrollments started by this component are shown (`fingerprint_grow.enroll` keeps your own
feedback). Without `led_feedback:` the component never touches the LED; build your own feedback
with the triggers `on_touch`, `on_gesture`, `on_waiting`, `on_cancelled`, `on_done`, `on_hold` and
`on_enroll_started` instead.

## Guest fingers

```yaml
fingerprint_grow_plus:
  time_id: sntp_time     # required: the expiry needs the real time
  guests: "150-179"      # valid 7 days; or {slots: "150-179", valid_for: 2d}
```

**Number of uses** (optional): `guests: {slots: "150-179", uses: 3}` (default for the gesture) or
`enroll_guest: {uses: 3}`. Every recognition counts one use (a finger resting on the sensor counts
once per 10 s); the last use still opens and then deletes the guest, with sync on all nodes. With
sync every use costs one sync version. `0` = unlimited (default).

Enroll a guest with the 5-touch gesture or `fingerprint_grow_plus.enroll_guest`;
`fingerprint_grow_plus.extend_guest` sets a new validity. Once expired, the finger is deleted.
With sync the expiry date travels with the finger, and the first node that notices the expiry
deletes it everywhere.

## Names

Slots can carry a name (up to 24 bytes, UTF-8). With sync it travels with the finger to all nodes.

```yaml
# set a name
- fingerprint_grow_plus.set_name: {slot: 12, name: "Anna index finger"}
# enroll with a name
- fingerprint_grow_plus.enroll_next_free: {num_scans: 3, name: "Ben"}
# use it, e.g. in on_finger_scan_matched
- logger.log:
    format: "Welcome %s"
    args: ['id(fp_plus).slot_name(finger_id).c_str()']
```

The text sensor `last_name` shows the name of the last recognised finger (`#<slot>` without a
name), and backups keep the names. Names cost RAM and flash only for slots that have one; they
are written to flash only when a name changes. Renaming a synced slot uses one sync version.

## Duplicate check

```yaml
fingerprint_grow_plus:
  duplicate_check:
    mode: refuse               # warn: only report; refuse: cancel the enrollment
    on_duplicate:              # finger_id (already stored), slot (being enrolled)
      - logger.log:
          format: "This finger is already stored in slot %u"
          args: [finger_id]
```

At the first scan of every enrollment the fresh image is searched in the library (the module's
normal matching, the same as for opening the door). If the finger is already stored in another
slot, `on_duplicate` fires; with `refuse` the enrollment is cancelled (`on_enrollment_failed`).
Enrolling a finger again into its own slot is not a duplicate. The search adds up to about
1 s to the first scan.

## Access log

```yaml
fingerprint_grow_plus:
  access_log:
    size: 50                   # last scans kept in RAM (1-500, about 40 bytes each)
    on_entry:                  # finger_id (65535 = unknown finger), result, name
      - homeassistant.event:
          event: esphome.fingerprint_access
          data:
            slot: !lambda "return to_string(finger_id);"
            result: !lambda "return result;"
            name: !lambda "return name;"
```

Every scan is logged with time, slot, name and result: `matched`, `unknown` (finger not
recognised), `outside_schedule` (`schedule:`), `while_locked` (`lockout:`) or `duress`. The result
says what the sensor saw, not whether the door opened: that is up to your own automation. Admin
fingers, guests and fingers shown as the target of an admin action are marked.

The log lives only in RAM (lost on reboot, never written to flash, never sent to peers). Read it:
- **Tool:** `python3 tools/growplus.py log --host reader-garden.local --host 192.168.10.23 --key '<key>'`
  merges the logs of several nodes, sorted by time (`--csv` for a spreadsheet). Needs a `sync:`
  block (key) like backups. A `~` in front of the time means the node's clock was not set; the
  time is then estimated from its uptime.
- **Home Assistant:** `on_entry` (e.g. as an event, above) or the text sensor `last_access`
  (`Anna (#5): matched`). HA keeps its own history of both.
- **Device log:** `fingerprint_grow_plus.dump_access_log`; `clear_access_log` empties it.

The text sensor `last_access` also works without `access_log:`. Note that HA records a state only
when it changes: the same person twice in a row is one entry there; `on_entry` fires every time.

## Lockout and duress finger

```yaml
fingerprint_grow_plus:
  lockout:                     # lock after failed attempts
    max_attempts: 5            # unrecognised fingers in a row (default 5)
    duration: 15min
    share_with:                # optional: lock these stations too (as written in sync: peers)
      - reader-garage.local
    on_locked: …               # variable source: "local" or "remote"
    on_unlocked: …
  duress:                      # duress finger: opens normally, but also fires on_duress
    slots: "190-199"
    on_duress:                 # finger_id, confidence: e.g. a silent alarm to Home Assistant
      - homeassistant.event:
          event: esphome.duress
```

- **Lockout**: unrecognised fingers are counted; a recognised finger resets the counter
  (`reset_on_match: false` to keep it). With `fingerprint_grow_plus.register_failed_attempt` you
  can count your own failures too, e.g. matches with too low confidence. The lockout only sets
  the condition `fingerprint_grow_plus.is_locked`: add it to your door action, as with
  `is_admin_busy`. `fingerprint_grow_plus.reset_lockout` lifts it early on this node.
- **Shared lockout**: `share_with` passes a local lockout on to the listed stations (they need a
  `lockout:` block themselves). A lockout received from another station is never passed on.
- **Duress finger**: the component does not stop the door; the finger works like any other
  and additionally fires `on_duress`. Condition `fingerprint_grow_plus.is_duress_slot`.
- Counter and lockout live in RAM only: nothing is written to flash, a reboot clears them.

## Time windows

```yaml
fingerprint_grow_plus:
  time_id: sntp_time
  schedule:
    without_time: deny         # while the time is unknown: deny (default) or allow
    on_denied:                 # finger_id: a finger of a range matched outside its window
      - logger.log: "outside the time window"
    ranges:
      - slots: "100-119"       # e.g. cleaning staff
        times:
          - days: [mon, wed]   # or mon-fri, daily, weekend
            from: "08:00"
            to: "12:00"
      - slots: "120-129"
        times:
          - days: fri
            from: "22:00"      # over midnight: until 06:00 on Saturday
            to: "06:00"
```

The schedule only sets the condition `fingerprint_grow_plus.is_allowed: <finger_id>` (true when
the slot has no schedule or one of its windows is open); add it to your door action. Slots without
a schedule are always allowed; admin slots must not be in a schedule. Every node uses its own
`schedule:` block, so put the same block on all nodes that should apply it.

## Sync

```yaml
fingerprint_grow_plus:
  sync:
    key: !secret fingerprint_grow_plus_key
    peers: [reader-garden.local, reader-garage.local, 192.168.10.23]
```

- **Same slot everywhere**: a finger keeps its slot number on every node, so automations based on
  the slot keep working.
- **Nothing is overwritten silently**: two different fingers in the same slot are reported as a
  *conflict* (`on_conflict`) and left alone until you decide (`resolve_conflict`).
- **Deleting with `fingerprint_grow_plus.delete`** (or the 3-touch gesture) deletes on all nodes.
  A finger that is merely missing on one node (new sensor, local delete with
  `fingerprint_grow.delete`) is copied back from the others.
- **When it syncs**: after boot (`at_boot`), about 2 s after a local change such as an enrollment
  or deletion (`after_change`), every 15 min (`interval`), and on `fingerprint_grow_plus.sync`.
  Offline nodes catch up when they come back. `interval: never`, `at_boot: false` and
  `after_change: false` switch the automatic syncs off, so changes then wait for the next manual
  sync. Without regular syncs `enroll_next_free` may not know which slots the others use and can
  pick one that is taken elsewhere (reported as a conflict).
- **Only some slots**: `include_slots` / `exclude_slots` decide what is synced. Slots outside stay
  on the node; admin gestures, guests and `delete_all` still work there.

  ```yaml
  fingerprint_grow_plus:
    sync:
      key: !secret fingerprint_grow_plus_key
      peers: [reader-garden.local, reader-garage.local]
      exclude_slots: ["180-199"]   # e.g. node-exclusive fingers that stay on each node
      # include_slots: ["0"]       # or: share only slot 0 (e.g. the admin finger), keep the rest local
  ```

- **Security**: 256-bit pre-shared key, mutual authentication, ChaCha20 + HMAC-SHA256, replay
  protection, optional allow list of source networks; templates are never logged. Every write
  is checked and read back byte for byte.

### First rollout on sensors that already hold fingers

Fingers enrolled separately on each sensor differ byte by byte, and the sensor cannot tell
reliably whether two stored templates belong to the same finger. The sync therefore reports
conflicts instead of guessing. Resolve them once:

1. Back up every sensor.
2. Pick the node whose finger → slot assignment is correct (the *reference*).
3. On that node only, run `fingerprint_grow_plus.claim_all_slots` (e.g. from a web button). Its
   fingers replace the same slots on all other nodes; with `dry_run` they log
   `[dry run] would REPLACE slot …`.
4. Check the logs, then set `dry_run: false`.

### Backup and restore

`tools/growplus.py` (Python 3, standard library only) reads and writes the fingers of a node over
the sync connection; backup files are encrypted with the key. Complete commands
(`<key>` = the value of `fingerprint_grow_plus_key` from `secrets.yaml`):

```bash
# Linux / macOS
python3 tools/growplus.py backup  --host 192.168.10.21 --port 18530 --key '<key>' --out reader-garden.fgpb --include-local
python3 tools/growplus.py inspect --key '<key>' --in reader-garden.fgpb
python3 tools/growplus.py restore --host 192.168.10.21 --port 18530 --key '<key>' --in reader-garden.fgpb
```

```powershell
# Windows (PowerShell)
py tools\growplus.py backup  --host 192.168.10.21 --port 18530 --key '<key>' --out reader-garden.fgpb --include-local
py tools\growplus.py inspect --key '<key>' --in reader-garden.fgpb
py tools\growplus.py restore --host 192.168.10.21 --port 18530 --key '<key>' --in reader-garden.fgpb
```

The same tool reads access logs: `growplus.py log --host H1 --host H2:18531 --key '<key>'`
(see [Access log](#access-log)).

- `--host`: IP address or DNS name of the node (`.local` works where your PC resolves mDNS);
  `--port` only if the node does not use 18530.
- `--key`: instead of the key on the command line you can use `--key-file <file>` or the
  environment variable `GROWPLUS_KEY`.
- `--include-local`: also save the slots that are not synced (outside `include_slots`). The node
  hands them out only after `fingerprint_grow_plus.allow_full_backup` (open for 5 minutes).
  Without it only the synced slots are saved.
- `--slots 0,2,10-19` limits backup or restore to some slots.
- `restore` needs `fingerprint_grow_plus.allow_restore` on the node first. Restored synced slots
  replace the slot on all nodes; local-only slots stay local.
- The tool connects over the sync port, so the node needs a `sync:` block. For a single node
  without other nodes, a `sync:` block with only `key` is enough: it opens the port for the tool
  and syncs with nobody.

Details: [docs/operations.md](docs/operations.md).

## Configuration reference

```yaml
fingerprint_grow_plus:
  id: fp_plus
  finger_sensor: finger_detected      # touch pin: gestures end on lift, hold, sync waits for it
  time_id: sntp_time                  # needed for guests and the last_sync sensor
  enroll_slots: "1-199"               # where enroll_next_free looks (default: all slots)
  delete_all_code: !secret fingerprint_delete_all_code   # needed for delete_all / delete_admin
  fingerprint_count_sensor: fingerprint_count            # refreshed after writes and deletes
  fingerprint_capacity_sensor: fingerprint_capacity      # cross-check of the capacity
  startup_delay: 10s                  # first sensor access after boot
  metadata_storage: preferences       # ESP32 default; ESP8266: sensor_notepad
  uart_id: uart_sensor                # only with several UARTs
  fingerprint_grow_id: grow           # only with several fingerprint_grow
  on_enroll_started: []               # slot
  on_slot_updated: []                 # slot: a finger was written by the sync / restore
  on_slot_deleted: []                 # slot
  led_feedback:                       # optional built-in ring LED feedback (table above)
    on_idle: []                       # restore your own LED; without it the LED is switched off

  admin:
    slots: [0]                        # protected admin slots (1-10)
    enroll_touches: 2                 # 0 switches a gesture off
    delete_touches: 3
    reenroll_touches: 4
    guest_touches: 5                  # default 5 with guests:, otherwise 0
    window: 2s                        # no finger for this long completes a gesture
    hold_time: 3s
    target_timeout: 10s               # time to show the finger to delete / re-enroll
    num_scans: 3                      # scans per enrollment
    access: off                       # off | delayed | immediate
    on_touch: []                      # touches: admin finger matched (1, 2, …)
    on_gesture: []                    # touches: gesture complete
    on_access: []                     # finger_id, confidence: single touch with access
    on_hold: []                       # admin finger kept on the sensor
    on_waiting: []                    # action ("delete" / "reenroll"): show the finger now
    on_cancelled: []                  # admin action cancelled or timed out
    on_done: []                       # action, slot: admin action executed

  guests:
    slots: "150-179"                  # at most 64 slots, outside the admin slots
    valid_for: 7d
    uses: 0                           # uses per guest, 0 = unlimited

  lockout:
    max_attempts: 5                   # unrecognised fingers in a row
    duration: 15min
    reset_on_match: true              # a recognised finger resets the counter
    share_with: []                    # peers (host or host:port from sync: peers) locked too
    on_locked: []                     # source: "local" / "remote"
    on_unlocked: []

  duress:
    slots: "190-199"                  # or just duress: "190-199"
    on_duress: []                     # finger_id, confidence

  duplicate_check:
    mode: refuse                      # warn | refuse
    on_duplicate: []                  # finger_id (stored), slot (being enrolled)

  access_log:
    size: 50                          # last scans in RAM (1-500); or just access_log: 50
    on_entry: []                      # finger_id (65535 = unknown), result, name

  schedule:                           # needs time_id
    without_time: deny                # deny | allow while the time is unknown
    on_denied: []                     # finger_id
    ranges:
      - slots: "100-119"
        times:
          - days: mon-fri             # [mon, wed], mon-fri, daily (default), weekend
            from: "08:00"             # HH:MM; to < from = over midnight; "24:00" = end of day
            to: "12:00"

  sync:
    key: !secret fingerprint_grow_plus_key   # required: base64 of 32 random bytes
    peers:                            # all nodes (name, name.local, IP, host:port); none = tool only
      - reader-garden.local           # mDNS: same subnet only
      - reader-garage.example.lan     # DNS
      - 192.168.10.23
      - 10.20.0.5:18531
    port: 18530                       # listen port and default peer port
    allowed_networks: [192.168.10.0/24]   # accept peers only from these networks
    include_slots: ["0-199"]          # synced slots (default: all)
    exclude_slots: []                 # never synced
    interval: 15min                   # periodic full comparison with all peers, or never
    at_boot: true                     # sync shortly after boot
    after_change: true                # sync ~2 s after a local change (enroll, delete, restore, …)
    dry_run: false                    # true: only log what the sync would write or delete
    on_started: []
    on_done: []                       # round finished without errors
    on_failed: []                     # reason: peer unreachable, sensor busy, …
    on_conflict: []                   # slot: a different finger is stored in this slot elsewhere
```

### Actions

| Action | Parameters | Effect |
|---|---|---|
| `enroll_next_free` | `num_scans` (3), `name` | enroll the lowest free slot (with sync: free on all nodes) |
| `move_slot` | `from`, `to` | move a finger with its name and guest data to a free slot; synced slots on all nodes |
| `set_name` | `slot`, `name` | name a slot (empty name removes it); synced slots on all nodes |
| `delete` | `slot` | delete a finger (with sync: on all nodes); admin slots refused |
| `delete_all` | `code` | delete all fingers except the admin fingers (with sync: on all nodes) |
| `delete_next_finger` | – | the next finger shown is deleted |
| `reenroll_next_finger` | – | the next finger shown is enrolled again into its slot |
| `cancel_admin` | – | cancel a pending admin action |
| `delete_admin` | `slot`, `code` | delete an admin finger |
| `enroll_guest` | `num_scans` (3), `valid_for` (7d), `name`, `uses` | enroll the next free guest slot |
| `extend_guest` | `slot`, `valid_for` (7d) | new validity for a guest |
| `sync` | – | start a sync round now |
| `claim_all_slots` | – | this node's fingers replace the same slots on all nodes |
| `resolve_conflict` | `slot`, `keep_local` | `true`: this node's finger wins; `false`: take the peer's |
| `resolve_all_conflicts` | `keep_local` | the same for all current conflicts |
| `reset_versions` | `code` | start the sync versions again at 1 on all nodes (needs a round that reached every peer, no conflicts) |
| `allow_restore` | `duration` (5min) | accept a restore from `tools/growplus.py` |
| `allow_full_backup` | `duration` (5min) | let `tools/growplus.py backup --include-local` read the slots that are not synced |
| `register_failed_attempt` | – | count a failed attempt for `lockout:` (unmatched scans count by themselves) |
| `reset_lockout` | – | lift a lockout and reset the counter on this node |
| `dump_access_log` | – | write the access log to the device log |
| `clear_access_log` | – | empty the access log |

All actions are called as `fingerprint_grow_plus.<action>`.
Conditions: `fingerprint_grow_plus.is_ready`, `is_syncing`, `is_admin_busy`, `is_admin_slot`,
`is_locked`, `is_duress_slot`, `is_allowed`.
Lambdas: `id(fp_plus).access_log()` (entries, oldest first), `id(fp_plus).slot_name(slot)`, `slot_uses_left(slot)`, `health()`, `next_free_slot()`, `slot_expiry(slot)`, `conflict_count()`.

### Entities (all optional)

```yaml
text_sensor:
  - platform: fingerprint_grow_plus
    occupied_slots: {name: Occupied slots}   # e.g. "0, 2-5, 17, 150-151"
    status: {name: Sync status}
    unreachable_peers: {name: Unreachable peers}   # "-" or "reader-garage.local (not reachable)"
    last_name: {name: Last recognised}       # name of the last recognised finger
    last_access: {name: Last access}         # e.g. "Anna (#5): matched", "unknown finger"
sensor:
  - platform: fingerprint_grow_plus
    peers_reachable: {name: Sync peers reachable}
    error_count: {name: Sync errors}
    conflict_count: {name: Sync conflicts}
    last_sync: {name: Last sync}             # needs time_id
    versions_left: {name: Sync versions left}   # changes left before a version reset (max 16383)
    failed_attempts: {name: Failed attempts}    # lockout: counter
binary_sensor:
  - platform: fingerprint_grow_plus
    syncing: {name: Syncing}
    locked: {name: Locked}                     # lockout: active
```

## Good to know

- **Deleting with sync**: `fingerprint_grow.delete` / `delete_all` delete only on this sensor, and
  the next round copies the finger back from the other nodes. Delete with
  `fingerprint_grow_plus.delete`, the 3-touch gesture or `fingerprint_grow_plus.delete_all`.
- **Enrolling into a fixed slot** with `fingerprint_grow.enroll`: an occupied slot is replaced on
  all nodes; a slot that is free here but used elsewhere becomes a conflict.
  `enroll_next_free` and the admin gesture avoid this.
- **Timing**: one sensor command sequence per loop; writing a finger (store and read-back check)
  blocks the loop for about 1.2–1.7 s. Nothing runs while a finger is on the sensor or an enrollment is in progress.
- **Updates**: nodes with a different protocol version refuse each other; update all nodes and
  `tools/growplus.py` together.

## Platforms and limits

| Platform | Status |
|---|---|
| ESP32 family, ESP-IDF / Arduino | supported, CI compile, tested on ESP32-S2 + R503 |
| ESP8266 | supported, CI compile; metadata in the sensor notepad (guest expiry in RAM only), little free heap |
| Host (Linux) | end-to-end tests with simulated sensors |

- One fingerprint sensor per node; the sensors must be the same model and firmware for sync.
- Up to 1024 slots with `preferences`, 248 with `sensor_notepad`; at most 64 guest slots.
- 16,383 synced changes (enrollments, deletions, renames, guest uses, claims across all nodes)
  before the version counter must be reset; the `versions_left` sensor shows how many are left,
  and the log warns below 1000. `fingerprint_grow_plus.reset_versions` (with the delete code, on
  one node) starts again at 1 on all nodes; it needs a last round that reached every peer and no
  conflicts ([docs/protocol.md](docs/protocol.md)).
- Templates must have the same size on all nodes (same sensor model and firmware); a template of
  a different size is refused with `Incompatible template from …`.
- `sensor_power_pin` of `fingerprint_grow` is not supported.
- Observed on R503: 1536-byte templates (the manual says 768) and an unreliable
  template-to-template `Match` command, which is therefore not used.

## Documentation

- [Architecture](docs/architecture.md): design, sync rules, conflicts, failure behaviour
- [Protocol](docs/protocol.md): wire format, handshake, messages
- [Security](docs/security.md): threat model, limits and residual risks
- [Networks](docs/network.md): subnets, VLANs, VPNs, firewall rules
- [Operations](docs/operations.md): rollout, backup, restore, troubleshooting
- [Testing](docs/testing.md): unit, end-to-end and hardware tests
- [Anleitung (Deutsch)](docs/de/anleitung.md)

## License

[MIT](LICENSE)
