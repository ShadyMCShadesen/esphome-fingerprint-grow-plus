# esphome-fingerprint-grow-plus

Extensions for ESPHome's [`fingerprint_grow`](https://esphome.io/components/fingerprint_grow/)
(GROW R503, R30x, …): admin fingers that run the sensor without Home Assistant, guest fingers
with an expiry date, protected slots, and **optionally** synchronisation of all fingers between
several ESPHome nodes, in the same slot, directly over an encrypted connection (no Home
Assistant, MQTT broker or cloud service).

> **Status: beta.** Compiled in CI for ESP32 (Arduino and ESP-IDF) and ESP8266, tested end-to-end
> on the ESPHome host platform with simulated sensors, and running on real ESP32-S2 + R503
> hardware (four synchronised nodes). Before enabling the sync on sensors that already hold
> fingers, read [First rollout](#first-rollout-on-sensors-that-already-hold-fingers).

> **Renamed:** this project was called `fingerprint_sync`. Replace `fingerprint_sync` with
> `fingerprint_grow_plus` in your YAML and update all nodes together (sync protocol 3). Slot
> versions and guest expiry dates start fresh, backups made with `fpsync.py` cannot be read by
> `tools/growplus.py`: make a new backup after the update.

## How it works

```
 node A                               node B (optional)
 fingerprint_grow       (official)    fingerprint_grow
 fingerprint_grow_plus ─ TCP 18530 ─► fingerprint_grow_plus
 R503 on one UART                     R503 on one UART
```

`fingerprint_grow_plus` sits next to `fingerprint_grow` on the same UART and leaves it
untouched: matching, enrollment, Aura LED and its sensors keep working as before. It adds:

| Feature | Needs |
|---|---|
| Admin fingers: enroll, delete and re-enroll fingers by gesture, status on hold | `admin:` |
| Guest fingers deleted automatically when they expire | `guests:`, `time_id` |
| Enroll the next free slot from a button or the web UI | – |
| Protected admin slots, delete-all with a code | – / `delete_all_code` |
| Sync between nodes, same slot everywhere, conflicts instead of overwrites | `key`, `peers` |

Without `key` the node runs **stand-alone**: no network port is opened and nothing leaves the
node. With `key` and `peers` every slot carries a version; after a change on any node, and every
`sync_interval`, the nodes compare their slot lists and fetch what they are missing. Admin
gestures, deletions and guest expiry then apply to all nodes.

## Features

- **Admin fingers** (protected) with gestures: enroll the next free slot, delete or re-enroll a
  finger by showing it, enroll a guest, status colour on hold.
- **Guest fingers** with an expiry date, deleted (on all nodes) when they expire.
- **Enroll without Home Assistant**: admin finger, physical button or the web UI.
- **LED feedback via triggers**: the component never drives the LEDs itself.
- **Sync (optional)**: same slot on every node, so automations based on the slot number keep
  working everywhere; deletions propagate; a slot that is merely missing on one node is restored
  from the others; two different templates in one slot are a conflict and never overwritten;
  node-exclusive slot ranges; dry-run mode.
- **Security**: 256-bit pre-shared key, mutual authentication, ChaCha20 + HMAC-SHA256, replay
  protection, optional source network allow list, templates never logged.
- **Verified writes**: checksum on arrival, read back byte for byte after storing.
- **Routable**: one TCP port; peers as mDNS `.local` names, DNS names or IPv4 addresses.
- **Backup / restore tool** `tools/growplus.py` (Python standard library only, encrypted files).

## Quick start

### One node, no sync

```yaml
external_components:
  - source: github://ShadyMCShadesen/esphome-fingerprint-grow-plus@main   # or a release tag
    components: [fingerprint_grow_plus]

fingerprint_grow_plus:
  finger_sensor: finger_detected   # binary_sensor on the sensor's touch (wakeup) pin
  admin:                           # admin finger in slot 0; enroll it once with fingerprint_grow
```

Touch the admin finger twice and lift it: the next free slot is enrolled. See
[Admin fingers](#admin-fingers) and [Guest fingers](#guest-fingers).

### Several nodes with sync

1. Create a key once and put it into `secrets.yaml` (same value for all nodes):

   ```bash
   python3 tools/growplus.py genkey
   ```

   ```yaml
   # secrets.yaml
   fingerprint_grow_plus_key: "<output of genkey>"
   ```

2. Add to every node:

   ```yaml
   fingerprint_grow_plus:
     key: !secret fingerprint_grow_plus_key
     peers:                 # all nodes; each node skips itself
       - reader-garden.local
       - reader-garage.local
     dry_run: true          # first run: only log what would happen
   ```

3. Flash all nodes and read the logs: `Index ready`, `Sync with … finished`,
   `[dry run] would store slot …`, possibly `CONFLICT in slot …`.
4. Back up every sensor (`tools/growplus.py backup`, see [docs/operations.md](docs/operations.md)),
   then set `dry_run: false`.

`uart_id` and `fingerprint_grow_id` are only needed if a node has more than one UART or
fingerprint sensor. Complete examples: [`examples/`](examples/) (stand-alone, ESP32 Arduino,
ESP32 ESP-IDF, ESP8266).

## Recipes

### First rollout on sensors that already hold fingers

Fingers enrolled separately on each sensor have different template bytes, and the sensor cannot
tell reliably whether two stored templates belong to the same finger. The sync therefore reports
**conflicts** and never guesses. Resolve them once:

1. Back up every sensor.
2. Pick the node whose finger → slot assignment is correct (the *reference*).
3. On that node only, run `fingerprint_grow_plus.claim_all_slots` (e.g. from a web button). All its
   slots replace the same slots on the other nodes; in `dry_run` they log
   `[dry run] would REPLACE slot …`.
4. Check the logs, then set `dry_run: false`.

If two nodes claim, the later claim wins per slot. Claim on one node only.

### Admin fingers

```yaml
fingerprint_grow_plus:
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
  `fingerprint_grow_plus.delete` and kept by `delete_all`. Only `fingerprint_grow_plus.delete_admin` with
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
              - fingerprint_grow_plus.is_admin_slot: !lambda return finger_id;
              - fingerprint_grow_plus.is_admin_busy:
        then:
          - script.execute: open_door
```

Status colour on hold (`health()`: 0 in sync, 1 syncing, 2 conflicts, 3 error):

```yaml
fingerprint_grow_plus:
  finger_sensor: finger_detected
  on_admin_hold:
    - lambda: |-
        static const uint8_t colors[] = {4, 2, 5, 1};  // green, blue, yellow, red
        id(grow).aura_led_control(2, 50, colors[id(fp_plus).health()], 5);
```

### Guest fingers

```yaml
fingerprint_grow_plus:
  time_id: sntp_time
  guests: "150-179"     # valid 7 days by default; `valid_for: 2d` to change
```

Enroll with the 5-touch admin gesture or `fingerprint_grow_plus.enroll_guest`. The expiry date
travels with the template to all nodes; every node with a valid time deletes the finger on all
nodes once it has expired. `fingerprint_grow_plus.extend_guest` sets a new validity.

### Enroll with a button or the web UI

```yaml
binary_sensor:
  - platform: gpio
    pin: {number: GPIO18, mode: INPUT_PULLUP, inverted: true}
    name: Enroll button
    on_press:
      - fingerprint_grow_plus.enroll_next_free: 3

button:
  - platform: template
    name: Enroll next free slot
    on_press:
      - fingerprint_grow_plus.enroll_next_free: 3
```

### Node-exclusive slots

```yaml
fingerprint_grow_plus:
  exclude_slots: ["180-199"]   # never sent, received or deleted by the sync
```

## Configuration reference

```yaml
fingerprint_grow_plus:
  id: fp_plus
  finger_sensor: finger_detected      # binary_sensor on the touch pin: gestures end on lift,
                                      # hold detection, sync jobs wait while a finger is on
  time_id: sntp_time                  # needed for guests and the last_sync sensor
  enroll_slots: "1-199"               # range for enroll_next_free (admin and guest slots are skipped)
  delete_all_code: !secret fingerprint_delete_all_code   # needed for delete_all / delete_admin
  fingerprint_count_sensor: fingerprint_count            # updated after writes and deletes
  fingerprint_capacity_sensor: fingerprint_capacity      # cross-check of the slot range
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

  # --- sync: all optional; without key the node runs stand-alone ---
  key: !secret fingerprint_grow_plus_key   # base64 of 32 random bytes, same on all nodes
  peers:                              # all nodes; a node skips itself (name or name.local)
    - reader-garden.local             # mDNS: same subnet only
    - reader-garage.example.lan       # DNS
    - 192.168.10.23                   # IPv4
    - {host: 10.20.0.5, port: 18531}
  port: 18530                         # listen port and default peer port
  allowed_networks: [192.168.10.0/24] # accept inbound peers only from these networks
  include_slots: ["0-199"]            # default: all slots
  exclude_slots: []                   # node-exclusive slots
  sync_interval: 15min                # also re-reads the local index when stand-alone
  startup_delay: 10s
  dry_run: false
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
| `fingerprint_grow_plus.sync` | – | start a sync round now |
| `fingerprint_grow_plus.enroll_next_free` | `num_scans` (3) | enroll the lowest slot free on all known nodes |
| `fingerprint_grow_plus.delete` | `slot` | delete on all nodes (admin slots refused) |
| `fingerprint_grow_plus.delete_all` | `code`, `confirm_cluster_wide: true` | delete all managed slots except admins on all nodes |
| `fingerprint_grow_plus.delete_next_finger` | – | the next finger shown is deleted on all nodes |
| `fingerprint_grow_plus.reenroll_next_finger` | – | the next finger shown is enrolled again into its slot |
| `fingerprint_grow_plus.cancel_admin` | – | abort a pending admin action |
| `fingerprint_grow_plus.delete_admin` | `slot`, `code` | delete an admin slot on all nodes |
| `fingerprint_grow_plus.enroll_guest` | `num_scans` (3), `valid_for` (7d) | enroll the next free guest slot |
| `fingerprint_grow_plus.extend_guest` | `slot`, `valid_for` (7d) | new validity for a guest |
| `fingerprint_grow_plus.claim_all_slots` | – | this node's slots replace the same slots everywhere |
| `fingerprint_grow_plus.resolve_conflict` | `slot`, `keep_local` | `true`: this node wins; `false`: take the peer's finger |
| `fingerprint_grow_plus.resolve_all_conflicts` | `keep_local` | the same for all current conflicts |
| `fingerprint_grow_plus.allow_restore` | `duration` (5min) | accept uploads from `tools/growplus.py restore` |
| `fingerprint_grow_plus.self_test` | `slot` (0), `compare_slot` | diagnostic: template round trip on the own sensor, no slot written |

Conditions: `fingerprint_grow_plus.is_syncing`, `is_ready`, `is_admin_busy`, `is_admin_slot`.
Lambdas: `id(fp_plus).health()`, `slot_expiry(slot)`, `next_free_slot()`, `conflict_count()`.

### Diagnostic entities (optional)

```yaml
sensor:
  - platform: fingerprint_grow_plus
    peers_reachable: {name: Sync peers reachable}
    error_count: {name: Sync errors}
    conflict_count: {name: Sync conflicts}
    last_sync: {name: Last sync}          # needs time_id
text_sensor:
  - platform: fingerprint_grow_plus
    status: {name: Sync status}
binary_sensor:
  - platform: fingerprint_grow_plus
    syncing: {name: Syncing}
```

## Good to know

- **Deleting**: `fingerprint_grow.delete` / `delete_all` only delete **locally**, and the next
  round restores the slot from the other nodes. Use `fingerprint_grow_plus.delete`, the 3-touch
  gesture or `fingerprint_grow_plus.delete_all` for permanent deletion.
- **Enrolling into an occupied slot** (e.g. `fingerprint_grow.enroll` with a fixed slot)
  replaces that finger on all nodes. Enrolling into a slot that is free here but used elsewhere
  creates a conflict; `enroll_next_free` and the admin gesture avoid this.
- **Blocking**: one sensor job per loop iteration; writing a template blocks the loop for up to
  about 1.3 s. No job starts while a finger is on the sensor or an enrollment runs.
- **Upgrading**: the sync protocol has a version (currently 3). Nodes with different versions
  refuse each other, so update all nodes and `tools/growplus.py` together.

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
