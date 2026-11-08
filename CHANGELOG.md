# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
- **Renamed to `fingerprint_grow_plus`** (repository `esphome-fingerprint-grow-plus`): the component
  is an extension of `fingerprint_grow`, and the sync is one optional part. Replace
  `fingerprint_sync` with `fingerprint_grow_plus` in YAML (block, actions, conditions, sensor
  platforms). Stored metadata, guest expiry dates, backups and the sync protocol are unchanged.

### Added
- Stand-alone mode: without `key` the node opens no network port and uses admin fingers, guests,
  protected slots and button enrollment locally.

### Added
- Admin gestures: delete a finger on all nodes (3 touches + finger), re-enroll a finger into its
  own slot (4 touches + finger), status on hold (`on_admin_hold`, `health()`); new triggers
  `on_admin_gesture`, `on_admin_waiting`, `on_admin_cancelled`, `on_admin_done`; actions
  `delete_next_finger`, `reenroll_next_finger`, `cancel_admin`; conditions `is_admin_busy`,
  `is_admin_slot`.
- Several protected admin fingers (`admin: slots`): never targets of gestures, refused by
  `delete`, kept by `delete_all`; `delete_admin` removes one with the delete code.
- Guest fingers with an expiry date (`guests:`, `enroll_guest`, `extend_guest`); the expiry is
  synced and enforced by every node with a valid time.

### Fixed
- Admin gestures on real sensors: the pause that completes a gesture now starts when the finger
  is lifted (with `finger_sensor`), not at the match. Matching the next touch takes 1.5-3 s on an
  R503, so two touches were often counted as two single touches and nothing happened.
- `enroll_next_free`, `claim_all_slots`, admin gestures and guest expiry no longer wait for a
  finished sync round after boot; one complete local index is enough.
- Peer slot knowledge is kept between rounds, so an unreachable peer no longer makes its slots
  look free to `enroll_next_free`.

### Changed
- **Protocol version 2** (expiry in MANIFEST, TEMPLATE, RESTORE). Update all nodes and
  `tools/fpsync.py` together; version 1 and 2 nodes refuse each other.
- Simpler configuration: `admin:` without options (admin slot 0, all defaults),
  `guests: "150-179"` shorthand, `guest_touches` defaults to 5 when guests are configured.
- Admin gestures are evaluated after a pause (`window`, default 2 s) instead of acting on the
  last touch; `admin: touches` is now `enroll_touches` (old name still accepted).

### Added
- `fingerprint_grow_plus.self_test` action: step-by-step LoadChar/UpChar/DownChar/Match diagnosis on
  the own sensor (buffers only).

- `fingerprint_grow_plus.claim_all_slots` (make one node the reference) and
  `fingerprint_grow_plus.resolve_all_conflicts` for the initial rollout.
- `self_test` option `compare_slot` to score two different slots.

### Changed
- The module's template-to-template `Match` is no longer used: on real R503 modules it returns
  arbitrary results. Different templates in the same slot are always a conflict; written slots
  are verified by reading them back byte for byte.
- `self_test` checks whether a template survives the round trip sensor → ESP → sensor
  byte-identically.
- `DownChar` pauses 10 ms between data packets by default.
- New option `fingerprint_capacity_sensor`: the synced slot range follows fingerprint_grow's
  capacity; a capacity below the number of stored templates is rejected.

### Security / robustness
- Unauthenticated connections are closed after 5 s without a completed handshake.
- Per-session limits: one manifest of at most capacity entries, 16 KB send buffer, one
  outstanding restore; the job queue is capped and idempotent jobs are deduplicated, also
  across simultaneous sessions with the same peer.
- A stuck finger sensor (> 60 s) no longer blocks the sync forever; a round waiting more than
  5 min for a busy sensor is given up and retried later.
- The receive buffer limit covers one maximum record plus one read chunk.

### Fixed
- ESP32 Arduino builds parsed ReadSysPara wrongly (capacity 2): a helper named `word` collided
  with the Arduino `word(...)` macro.
- Sensor parameters are read twice and checked for plausibility; an inconsistent answer (seen as
  `capacity 2` on real hardware) no longer shrinks the managed slot range.
- `DownChar` logs any unsolicited answer of the module.
- `Match` waits up to 3 s instead of 1 s.
- Error messages name the failed step, the received answer bytes, the template and packet size.
- Simulator and docs use the template size observed on real R503 modules (1536 bytes).

## [0.1.0] - 2026-11-07

### Added
- `fingerprint_grow_plus` external component: direct node-to-node sync of GROW fingerprint
  templates with preserved slot IDs (TCP, pre-shared key, ChaCha20 + HMAC-SHA256).
- Anti-entropy sync with Lamport versions, tombstones, conflict detection via the sensor's
  1:1 match, convergence of separately enrolled identical fingers.
- Actions `sync`, `delete`, `delete_all`, `enroll_next_free`, `resolve_conflict`,
  `allow_restore`; conditions `is_syncing`, `is_ready`; sync triggers.
- Admin finger enrollment (with `on_admin_touch` trigger for feedback), include/exclude slot ranges,
  dry-run mode, source network allow list.
- Diagnostic sensor, text sensor and binary sensor platforms.
- `tools/fpsync.py` for encrypted backup, restore and status.
- Host unit tests, R503 simulator, end-to-end test on the ESPHome host platform, CI.

### Removed
- Earlier Home Assistant based skeleton.
