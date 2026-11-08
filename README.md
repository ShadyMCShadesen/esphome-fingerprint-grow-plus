# esphome-fingerprint-sync

Enroll a finger **once** on any ESPHome node with a GROW fingerprint sensor (R503, R30x, …)
and have it on **all** nodes, in the **same slot**. The nodes talk to each other directly over
an authenticated, encrypted TCP connection. No Home Assistant, MQTT broker or cloud service is
involved in the sync.

> **Status: beta (v0.1.x).** Tested end-to-end on the ESPHome host platform against a sensor
> simulator, compiled for ESP32 (Arduino and ESP-IDF) and ESP8266. Validate it on your hardware
> with `dry_run: true` and a backup first (see [Getting started](#getting-started)).

## Features

- **External component** `fingerprint_sync`. It works next to ESPHome's official
  [`fingerprint_grow`](https://esphome.io/components/fingerprint_grow/) and does not replace it.
  Matching, enrollment, Aura LED and sensors stay untouched.
- **Slot IDs are preserved**, so automations that depend on a slot number keep working on every node.
- **Anti-entropy sync.** After every enrollment or deletion the change is pushed at once, and a
  periodic full reconciliation catches nodes that were offline.
- **No silent overwrites.** A slot with a *different* finger on two nodes is reported as a
  conflict (`on_conflict`) and left alone until you resolve it. A slot with the same finger
  enrolled separately is detected through the sensor's 1:1 match and converged.
- **Deletions propagate** with tombstones. `delete_all` needs a code and an explicit
  `confirm_cluster_wide: true`.
- **Node-exclusive slot ranges** via `include_slots` / `exclude_slots`.
- **Enroll without Home Assistant:** physical button, web UI (`web_server`) or an **admin finger**
  (touch slot 0 twice to enroll the next free slot).
- **Security:** pre-shared 256-bit key, mutual authentication, ChaCha20 + HMAC-SHA256, replay
  protection, optional source network allow list. Templates are never logged.
- **Verified writes:** every received template is checked against its SHA-256 and read back
  from the sensor after storing it.
- **Non-blocking:** one short sensor transaction per loop iteration (≈0.2–0.6 s worst case),
  network I/O fully non-blocking, sensor jobs wait while a finger is on the sensor or an
  enrollment is running.
- **Transport abstraction:** TCP today. It works inside one subnet and across routed networks or
  VLANs with one firewall port. Peers can be IPv4 addresses, DNS names or mDNS `.local` names.
- **Backup / restore tool** (`tools/fpsync.py`, Python standard library only) with encrypted
  backup files.
- **Diagnostics:** status text, syncing flag, reachable peers, error and conflict counters, last
  sync time.

## Getting started

1. Generate a key and put it into `secrets.yaml` on every node:

   ```bash
   python3 tools/fpsync.py genkey
   ```

   ```yaml
   # secrets.yaml
   fingerprint_sync_key: "<output of genkey>"
   fingerprint_delete_all_code: "<something long>"
   ```

2. Add the component, pinned to a release:

   ```yaml
   external_components:
     - source: github://ShadyMCShadesen/esphome-fingerprint-sync@v0.1.0
       components: [fingerprint_sync]

   fingerprint_sync:
     key: !secret fingerprint_sync_key
     peers:
       - reader-garden.local
       - reader-garage.local
     dry_run: true            # first run: only log what would happen
   ```

3. Flash all nodes, check the logs (`[dry run] would store slot …`), make a **backup of every
   sensor** with `tools/fpsync.py backup …` (see [docs/operations.md](docs/operations.md)), then
   set `dry_run: false`.

Complete examples: [`examples/`](examples/) (ESP32 Arduino, ESP32 ESP-IDF, ESP8266).

## Configuration

```yaml
fingerprint_sync:
  id: fp_sync                      # optional
  fingerprint_grow_id: grow        # optional if there is only one fingerprint_grow
  uart_id: uart_sensor             # the SAME UART bus as fingerprint_grow
  key: !secret fingerprint_sync_key   # required, base64 of 32 random bytes
  port: 18530                      # TCP port (listen and default peer port)
  peers:                           # all nodes; the node skips itself (by name / name.local)
    - reader-garden.local          # mDNS (same subnet only)
    - reader-garage.example.lan    # DNS
    - 192.168.10.23                # IPv4
    - host: 10.20.0.5
      port: 18531
  allowed_networks:                # optional: accept inbound peers only from these networks
    - 192.168.10.0/24
  include_slots: ["0-199"]         # optional, default all slots
  exclude_slots: ["180-199"]       # optional, these slots are never sent, received or deleted
  enroll_slots: "1-179"            # optional, range for enroll_next_free (default 1-max)
  sync_interval: 15min             # periodic full reconciliation
  startup_delay: 10s               # wait after boot before touching the sensor
  dry_run: false                   # true = never write/delete because of a peer
  metadata_storage: preferences    # preferences (ESP32 default) | sensor_notepad (ESP8266 default)
  delete_all_code: !secret fingerprint_delete_all_code   # required for delete_all
  finger_sensor: finger_detected   # optional binary_sensor: no sensor jobs while it is on
  fingerprint_count_sensor: fingerprint_count   # optional: updated after sync writes/deletes
  sensor_address: 0xFFFFFFFF       # module address, default 0xFFFFFFFF
  time_id: sntp_time               # optional, needed for the last_sync sensor
  admin:                           # optional admin finger
    slot: 0
    touches: 2                     # matches of the admin finger within `window`
    window: 5s
    num_scans: 3
  on_sync_started: …
  on_sync_done: …
  on_sync_failed: …                # variable: std::string reason
  on_conflict: …                   # variable: uint16_t slot
  on_slot_updated: …               # variable: uint16_t slot (received/replaced/restored)
  on_slot_deleted: …               # variable: uint16_t slot
  on_enroll_started: …             # variable: uint16_t slot (enroll_next_free / admin finger)
  on_admin_touch: …                # variable: uint8_t touches (admin finger matched, 1..touches)
```

### Admin finger

`admin.slot` (default 0) only arms enrollment. `fingerprint_grow` still reports the match, so
exclude that slot from your normal actions:

```yaml
fingerprint_grow:
  on_finger_scan_matched:
    - if:
        condition:
          lambda: "return finger_id != 0;"   # admin finger never opens anything
        then:
          - script.execute: open_door
```

The component never controls the LEDs itself. Use the triggers to fit sync feedback into your
own LED logic.

### Actions and conditions

| Action | Parameters | Effect |
|---|---|---|
| `fingerprint_sync.sync` | – | start a sync round now |
| `fingerprint_sync.delete` | `slot` | delete the slot here **and on all peers** |
| `fingerprint_sync.delete_all` | `code`, `confirm_cluster_wide: true` | delete all managed slots on all nodes |
| `fingerprint_sync.enroll_next_free` | `num_scans` (default 3) | enroll the lowest slot that is free on all known nodes |
| `fingerprint_sync.resolve_conflict` | `slot`, `keep_local` | `true`: this node's finger wins everywhere; `false`: take the peer's finger |
| `fingerprint_sync.allow_restore` | `duration` (default 5 min) | accept uploads from `tools/fpsync.py restore` |
| `fingerprint_sync.self_test` | `slot` (default 0) | diagnostic: LoadChar/UpChar/DownChar/Match round trip on the own sensor, buffers only, logs every step |

Conditions: `fingerprint_sync.is_syncing`, `fingerprint_sync.is_ready`.

> `fingerprint_grow.delete` / `delete_all` delete **locally only**. The slot is then restored
> from the peers in the next round. Use `fingerprint_sync.delete` for permanent deletion.

### Diagnostic entities (all optional)

```yaml
sensor:
  - platform: fingerprint_sync
    peers_reachable: {name: Sync peers reachable}
    error_count: {name: Sync errors}
    conflict_count: {name: Sync conflicts}
    last_sync: {name: Last sync}        # needs time_id
text_sensor:
  - platform: fingerprint_sync
    status: {name: Sync status}
binary_sensor:
  - platform: fingerprint_sync
    syncing: {name: Syncing}
```

## Platforms

| Platform | Status |
|---|---|
| ESP32 / ESP32-S2 / -S3 / -C3, ESP-IDF | supported (CI compile) |
| ESP32 family, Arduino | supported (CI compile) |
| ESP8266 | supported (CI compile). Metadata in the sensor notepad. The sensor UART is usually a software UART. Little free heap. |
| Host (Linux) | used for the end-to-end tests |

## Limits

- One fingerprint sensor per node, one inbound and one outbound sync session at a time.
- Templates must be compatible between the sensors (same model and firmware). This is not
  specified in the R503 manual; verify it on your hardware (docs/testing.md, test A1).
- Real R503 modules upload 1536-byte templates (the manual says 768); the component reads the
  size from the data stream and supports up to 2048 bytes.
- Up to 1024 slots with `preferences`, 248 slots with `sensor_notepad`.
- 16,383 versions (enrollments + deletions across the cluster) before the version counter must
  be reset (docs/protocol.md).
- `sensor_power_pin` of `fingerprint_grow` is not supported.
- Two nodes that are partitioned from each other and change the same slot are resolved by version
  number, i.e. the more recent change in cluster order wins (docs/architecture.md).

## Documentation

- [Architecture](docs/architecture.md): design, sync rules, conflict handling, failure behaviour
- [Protocol](docs/protocol.md): wire format, handshake, messages
- [Security](docs/security.md): threat model and residual risks
- [Networks](docs/network.md): same subnet, VLANs, routed networks, firewall rules
- [Operations](docs/operations.md): backup, restore, rollout, troubleshooting
- [Testing](docs/testing.md): unit, end-to-end and hardware test plan
- [Kurzanleitung (Deutsch)](docs/de/anleitung.md)

## License

[MIT](LICENSE)
