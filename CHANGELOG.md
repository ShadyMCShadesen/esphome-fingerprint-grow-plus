# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Text sensor `unreachable_peers`: the peers that failed in the last sync round, with the reason.
- Sensor `versions_left`: synced changes left before the version counter must be reset; a log
  warning below 1000.
- `led_feedback:` (optional): built-in ring LED feedback for admin gestures, enrollment, results
  and the status on hold; `on_idle` restores the user's own LED afterwards. Uses only endless
  effects, so `aura_led_control` never blocks the loop.
- `lockout:` (optional): lock after failed attempts, condition `is_locked`, actions
  `register_failed_attempt` / `reset_lockout`, entities `failed_attempts` / `locked`;
  `share_with` passes a lockout on to selected peers (protocol version 5, message `LOCK`).
- `duress:` (optional): a duress finger range that fires `on_duress`; condition `is_duress_slot`.
- Templates of a different size than this sensor's (other sensor model or firmware) are refused
  with a clear log line, both from peers and on restore.

### Fixed
- Holding the admin finger with `access:` could open the door: the sensor matched the resting
  finger a second time, which counted as a new single touch. After a hold, matches are ignored
  until the finger has been off the sensor for `window`; short drop-outs of the touch signal
  (< 0.5 s) no longer count as lifting.

## [0.2.0] - 2026-11-09

Complete rework of 0.1.0. Not compatible with 0.1.0: update all nodes and the tool together and
take a new backup afterwards.

### Changed (breaking)
- Component renamed from `fingerprint_sync` to `fingerprint_grow_plus`, repository renamed to
  `esphome-fingerprint-grow-plus`.
- Configuration grouped into the blocks `admin:`, `guests:` and `sync:`; the triggers moved into
  their blocks (`admin: on_touch`, `sync: on_conflict`, …). Without `sync:` a node is stand-alone.
- Sync protocol version 4 (0.1.0: version 1); nodes with different versions refuse each other.
- `tools/fpsync.py` is now `tools/growplus.py`; backups of 0.1.0 cannot be read.
- Conflicts are no longer resolved with the sensor's template match (unreliable on R503): two
  different templates in one slot are always a conflict.
- `delete_all` only needs the code.

### Added
- Component `fingerprint_grow_plus` next to the official `fingerprint_grow` on the same UART.
- `admin:` protected admin fingers with gestures (touch, lift, wait): enroll the next free slot,
  delete or re-enroll the next finger shown, enroll a guest, status on hold (`health()`);
  optional single touch as access (`access: delayed | immediate`, `on_access`).
- `guests:` guest fingers with an expiry date, deleted automatically (also across nodes).
- `sync:` (optional) node-to-node sync of templates in the same slot: anti-entropy with per-slot
  versions and tombstones, conflicts instead of overwrites, `claim_all_slots`,
  `resolve_conflict`, slot ranges (`include_slots` / `exclude_slots`), dry run; automatic syncs
  configurable (`interval` incl. `never`, `at_boot`, `after_change`).
  Without `sync:` the node is stand-alone and opens no network port.
- Security: pre-shared key, mutual authentication, ChaCha20 + HMAC-SHA256, replay protection,
  source network allow list, resource limits; templates are never logged.
- Enrollment without Home Assistant: admin finger, button, web UI (`enroll_next_free`).
- Entities: `occupied_slots`, sync status, peers reachable, errors, conflicts, last sync, syncing.
- `tools/growplus.py`: key generation, status, encrypted backup, restore, inspect;
  `backup --include-local` also saves the slots that are not synced (after `allow_full_backup`).
  A `sync:` block with only `key` gives a single node backup access without syncing.
- Tests: crypto unit tests, Python/C++ interop, end-to-end tests with simulated R503 sensors on the
  ESPHome host platform; CI compile for ESP32 (Arduino, ESP-IDF) and ESP8266.

### Removed
- `sensor_address` option (fingerprint_grow always uses the default address).
- `self_test` action (only needed for the initial hardware validation).

## [0.1.0] - 2026-11-07

First pre-release as `fingerprint_sync`: node-to-node template sync with preserved slot IDs,
admin finger enrollment, backup tool, tests.
