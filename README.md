# esphome-fingerprint-sync

Enroll a finger **once** on any ESPHome node with a GROW fingerprint sensor (R503, R30x, …)
and have it on **all** nodes, in the **same slot**. The nodes talk to each other directly over an
authenticated, encrypted TCP connection. No Home Assistant, MQTT broker or cloud service is
involved.

> **Status: beta.** Compiled in CI for ESP32 (Arduino and ESP-IDF) and ESP8266, tested end-to-end
> on the ESPHome host platform with simulated sensors, and checked on real ESP32-S2 + R503
> hardware with `dry_run` (template transfer is byte-identical). Start with `dry_run: true` and
> a backup, see [First rollout](#first-rollout-on-sensors-that-already-hold-fingers).

## How it works

```
 node A                                   node B
 fingerprint_grow  (official, untouched)  fingerprint_grow
 fingerprint_sync ──── TCP 18530 ───────► fingerprint_sync
   R503 on the same UART                    R503 on the same UART
```

`fingerprint_sync` sits next to ESPHome's
[`fingerprint_grow`](https://esphome.io/components/fingerprint_grow/) on the same UART. It reads
templates from one sensor and writes them into the same slot on the other sensors. Matching,
enrollment, Aura LED and the existing sensors keep working as before.

Every slot carries a version. After a change on any node, and every `sync_interval`, the nodes
compare their slot lists and fetch what they are missing. Nodes that were offline catch up when
they come back.

## Features

- **Same slot everywhere**: automations based on the slot number keep working on every node.
- **No silent overwrites**: two different templates in one slot are reported as a conflict and
  left alone until you decide.
- **Deletions propagate**; a slot that is merely missing on one node (new sensor, local delete)
  is restored from the others.
- **Admin fingers** (protected) with gestures: enroll the next free slot, delete or re-enroll a
  finger on all nodes by showing it, status colour on hold.
- **Guest fingers** with an expiry date, deleted on all nodes when they expire.
- **Enroll without Home Assistant**: admin finger, physical button or the web UI.
- **Node-exclusive slot ranges**, dry-run mode, diagnostic entities, LED feedback via triggers
  (the component never drives the LEDs itself).
- **Security**: 256-bit pre-shared key, mutual authentication, ChaCha20 + HMAC-SHA256, replay
  protection, optional source network allow list, templates never logged.
- **Verified writes**: checksum on arrival, read back byte for byte after storing.
- **Routable**: one TCP port; peers as mDNS `.local` names, DNS names or IPv4 addresses.
- **Backup / restore tool** `tools/fpsync.py` (Python standard library only, encrypted files).

## Quick start

1. Create a key once and put it into `secrets.yaml` (same value for all nodes):

   ```bash
   python3 tools/fpsync.py genkey
   ```

   ```yaml
   # secrets.yaml
   fingerprint_sync_key: "<output of genkey>"
   ```

2. Add the component to every node:

   ```yaml
   external_components:
     - source: github://ShadyMCShadesen/esphome-fingerprint-sync@main   # or a release tag
       components: [fingerprint_sync]

   fingerprint_sync:
     key: !secret fingerprint_sync_key
     peers:                 # all nodes; each node skips itself
       - reader-garden.local
       - reader-garage.local
     dry_run: true          # first run: only log what would happen
   ```

   `uart_id` and `fingerprint_grow_id` are only needed if a node has more than one UART or
   fingerprint sensor.

3. Flash all nodes and read the logs: `Index ready`, `Sync with … finished`,
   `[dry run] would store slot …`, possibly `CONFLICT in slot …`.
4. Back up every sensor (`tools/fpsync.py backup`, see [docs/operations.md](docs/operations.md)),
   then set `dry_run: false`.

Complete examples: [`examples/`](examples/) (ESP32 Arduino, ESP32 ESP-IDF, ESP8266).

## Recipes

### First rollout on sensors that already hold fingers

Fingers enrolled separately on each sensor have different template bytes, and the sensor cannot
tell reliably whether two stored templates belong to the same finger. The sync therefore reports
**conflicts** and never guesses. Resolve them once:

1. Back up every sensor.
2. Pick the node whose finger → slot assignment is correct (the *reference*).
3. On that node only, run `fingerprint_sync.claim_all_slots` (e.g. from a web button). All its
   slots replace the same slots on the other nodes; in `dry_run` they log
   `[dry run] would REPLACE slot …`.
4. Check the logs, then set `dry_run: false`.

If two nodes claim, the later claim wins per slot. Claim on one node only.

### Admin fingers

```yaml
fingerprint_sync:
  admin:                # admin finger in slot 0 with all defaults
```

| Admin finger | Effect |
|---|---|
| 2 touches | enroll the next free slot |
| 3 touches, then a finger | that finger is deleted on **all** nodes |
| 4 touches, then a finger | that finger is enrolled again into its own slot; the new template replaces it everywhere |
| 5 touches (only with `guests:`) | enroll a guest finger |
| hold ≥ 3 s (needs `finger_sensor`) | `on_admin_hold`: show the status, see `health()` |

- **Gestures**: a gesture is complete when no finger has been on the sensor for `window` (2 s;
  measured from the last match without `finger_sensor`, so configure it). Showing an admin finger
  instead of the target finger, or no finger within `target_timeout` (10 s), cancels the action.
- **Protection**: admin slots are never a free slot, never a gesture target, refused by
  `fingerprint_sync.delete` and kept by `delete_all`. Only `fingerprint_sync.delete_admin` with
  the delete code removes one. Use the same `admin: slots` on all nodes.
- **No access**: `fingerprint_grow` still reports every match. Keep admin fingers, and fingers
  shown during an admin action, out of your normal actions:

```yaml
fingerprint_grow:
  on_finger_scan_matched:
    - if:
        condition:
          not:
            or:
              - fingerprint_sync.is_admin_slot: !lambda return finger_id;
              - fingerprint_sync.is_admin_busy:
        then:
          - script.execute: open_door
```

Status colour on hold (`health()`: 0 in sync, 1 syncing, 2 conflicts, 3 error):

```yaml
fingerprint_sync:
  finger_sensor: finger_detected
  on_admin_hold:
    - lambda: |-
        static const uint8_t colors[] = {4, 2, 5, 1};  // green, blue, yellow, red
        id(grow).aura_led_control(2, 50, colors[id(fp_sync).health()], 5);
```

### Guest fingers

```yaml
fingerprint_sync:
  time_id: sntp_time
  guests: "150-179"     # valid 7 days by default; `valid_for: 2d` to change
```

Enroll with the 5-touch admin gesture or `fingerprint_sync.enroll_guest`. The expiry date
travels with the template to all nodes; every node with a valid time deletes the finger on all
nodes once it has expired. `fingerprint_sync.extend_guest` sets a new validity.

### Enroll with a button or the web UI

```yaml
binary_sensor:
  - platform: gpio
    pin: {number: GPIO18, mode: INPUT_PULLUP, inverted: true}
    name: Enroll button
    on_press:
      - fingerprint_sync.enroll_next_free: 3

button:
  - platform: template
    name: Enroll next free slot
    on_press:
      - fingerprint_sync.enroll_next_free: 3
```

### Node-exclusive slots

```yaml
fingerprint_sync:
  exclude_slots: ["180-199"]   # never sent, received or deleted by the sync
```

## Configuration reference

```yaml
fingerprint_sync:
  id: fp_sync
  key: !secret fingerprint_sync_key   # required: base64 of 32 random bytes, same on all nodes
  peers:                              # all nodes; a node skips itself (name or name.local)
    - reader-garden.local             # mDNS: same subnet only
    - reader-garage.example.lan       # DNS
    - 192.168.10.23                   # IPv4
    - {host: 10.20.0.5, port: 18531}
  port: 18530                         # listen port and default peer port
  allowed_networks: [192.168.10.0/24] # accept inbound peers only from these networks
  include_slots: ["0-199"]            # default: all slots
  exclude_slots: []                   # node-exclusive slots
  enroll_slots: "1-199"               # range for enroll_next_free (admin and guest slots are skipped)
  sync_interval: 15min
  startup_delay: 10s
  dry_run: false
  delete_all_code: !secret fingerprint_delete_all_code   # needed for delete_all / delete_admin
  finger_sensor: finger_detected      # binary_sensor: sync waits while a finger is on; enables hold
  fingerprint_count_sensor: fingerprint_count            # updated after sync writes and deletes
  fingerprint_capacity_sensor: fingerprint_capacity      # cross-check of the slot range
  time_id: sntp_time                  # needed for guests and the last_sync sensor
  metadata_storage: preferences       # ESP32 default; ESP8266: sensor_notepad
  sensor_address: 0xFFFFFFFF
  uart_id: uart_sensor                # only with several UARTs
  fingerprint_grow_id: grow           # only with several fingerprint_grow
  admin:
    slots: [0]                        # one or more protected admin slots
    enroll_touches: 2                 # 0 disables a gesture
    delete_touches: 3
    reenroll_touches: 4
    guest_touches: 5                  # default 5 with guests:, otherwise 0
    window: 2s                        # pause after lifting that completes a gesture
    hold_time: 3s
    target_timeout: 10s
    num_scans: 3
  guests:
    slots: "150-179"                  # at most 64 slots, outside the admin slots
    valid_for: 7d
```

### Triggers

| Trigger | Variables | When |
|---|---|---|
| `on_sync_started` / `on_sync_done` | – | a sync round starts / ends without errors |
| `on_sync_failed` | `std::string reason` | a round had errors (peer unreachable, sensor busy, …) |
| `on_conflict` | `uint16_t slot` | a different template is stored in this slot on a peer |
| `on_slot_updated` / `on_slot_deleted` | `uint16_t slot` | the sync wrote / deleted a slot on this sensor |
| `on_enroll_started` | `uint16_t slot` | enrollment of a free, guest or re-enrolled slot started |
| `on_admin_touch` | `uint8_t touches` | admin finger matched (1, 2, …) |
| `on_admin_gesture` | `uint8_t touches` | gesture complete |
| `on_admin_hold` | – | admin finger held |
| `on_admin_waiting` | `std::string action` | waiting for the finger to `delete` / `reenroll` |
| `on_admin_cancelled` | – | admin action aborted or timed out |
| `on_admin_done` | `std::string action`, `uint16_t slot` | admin action executed |

### Actions

| Action | Parameters | Effect |
|---|---|---|
| `fingerprint_sync.sync` | – | start a sync round now |
| `fingerprint_sync.enroll_next_free` | `num_scans` (3) | enroll the lowest slot free on all known nodes |
| `fingerprint_sync.delete` | `slot` | delete on all nodes (admin slots refused) |
| `fingerprint_sync.delete_all` | `code`, `confirm_cluster_wide: true` | delete all managed slots except admins on all nodes |
| `fingerprint_sync.delete_next_finger` | – | the next finger shown is deleted on all nodes |
| `fingerprint_sync.reenroll_next_finger` | – | the next finger shown is enrolled again into its slot |
| `fingerprint_sync.cancel_admin` | – | abort a pending admin action |
| `fingerprint_sync.delete_admin` | `slot`, `code` | delete an admin slot on all nodes |
| `fingerprint_sync.enroll_guest` | `num_scans` (3), `valid_for` (7d) | enroll the next free guest slot |
| `fingerprint_sync.extend_guest` | `slot`, `valid_for` (7d) | new validity for a guest |
| `fingerprint_sync.claim_all_slots` | – | this node's slots replace the same slots everywhere |
| `fingerprint_sync.resolve_conflict` | `slot`, `keep_local` | `true`: this node wins; `false`: take the peer's finger |
| `fingerprint_sync.resolve_all_conflicts` | `keep_local` | the same for all current conflicts |
| `fingerprint_sync.allow_restore` | `duration` (5min) | accept uploads from `tools/fpsync.py restore` |
| `fingerprint_sync.self_test` | `slot` (0), `compare_slot` | diagnostic: template round trip on the own sensor, no slot written |

Conditions: `fingerprint_sync.is_syncing`, `is_ready`, `is_admin_busy`, `is_admin_slot`.
Lambdas: `id(fp_sync).health()`, `slot_expiry(slot)`, `next_free_slot()`, `conflict_count()`.

### Diagnostic entities (optional)

```yaml
sensor:
  - platform: fingerprint_sync
    peers_reachable: {name: Sync peers reachable}
    error_count: {name: Sync errors}
    conflict_count: {name: Sync conflicts}
    last_sync: {name: Last sync}          # needs time_id
text_sensor:
  - platform: fingerprint_sync
    status: {name: Sync status}
binary_sensor:
  - platform: fingerprint_sync
    syncing: {name: Syncing}
```

## Good to know

- **Deleting**: `fingerprint_grow.delete` / `delete_all` only delete **locally**, and the next
  round restores the slot from the other nodes. Use `fingerprint_sync.delete`, the 3-touch
  gesture or `fingerprint_sync.delete_all` for permanent deletion.
- **Enrolling into an occupied slot** (e.g. `fingerprint_grow.enroll` with a fixed slot)
  replaces that finger on all nodes. Enrolling into a slot that is free here but used elsewhere
  creates a conflict; `enroll_next_free` and the admin gesture avoid this.
- **Blocking**: one sensor job per loop iteration; writing a template blocks the loop for up to
  about 1.3 s. No job starts while a finger is on the sensor or an enrollment runs.
- **Upgrading**: the sync protocol has a version (currently 2). Nodes with different versions
  refuse each other, so update all nodes and `tools/fpsync.py` together.

## Platforms and tested hardware

| Platform | Status |
|---|---|
| ESP32 family, ESP-IDF | supported, CI compile |
| ESP32 family, Arduino | supported, CI compile |
| ESP8266 | supported, CI compile; metadata in the sensor notepad (guest expiry in RAM only), software UART, little free heap |
| Host (Linux) | end-to-end tests with simulated sensors |

Real hardware so far: ESP32-S2 (lolin_s2_mini) with GROW R503 modules, security level 3,
capacity 200. Observed: 1536-byte templates (the manual says 768), 128-byte data packets,
byte-identical transfer, and an unreliable template-to-template `Match` command (not used).

## Limits

- One fingerprint sensor per node; one inbound and one outbound sync session at a time.
- Templates must be compatible between the sensors (same model and firmware).
- Up to 1024 slots with `preferences`, 248 with `sensor_notepad`; at most 64 guest slots.
- 16,383 versions (enrollments, deletions, claims across all nodes) before the version counter
  must be reset (docs/protocol.md).
- `sensor_power_pin` of `fingerprint_grow` is not supported.
- Nodes that are cut off from each other and change the same slot: the newer change wins.

## Documentation

- [Architecture](docs/architecture.md): design, sync rules, conflicts, failure behaviour
- [Protocol](docs/protocol.md): wire format, handshake, messages
- [Security](docs/security.md): threat model, limits and residual risks
- [Networks](docs/network.md): subnets, VLANs, routed networks, firewall rules
- [Operations](docs/operations.md): rollout, backup, restore, troubleshooting
- [Testing](docs/testing.md): unit, end-to-end and hardware test plan
- [Kurzanleitung (Deutsch)](docs/de/anleitung.md)

## License

[MIT](LICENSE)
