# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- `fingerprint_sync.self_test` action: step-by-step LoadChar/UpChar/DownChar/Match diagnosis on
  the own sensor (buffers only).

- `fingerprint_sync.claim_all_slots` (make one node the reference) and
  `fingerprint_sync.resolve_all_conflicts` for the initial rollout.
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
- `fingerprint_sync` external component: direct node-to-node sync of GROW fingerprint
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
