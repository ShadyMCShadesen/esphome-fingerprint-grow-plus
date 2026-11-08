# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- `fingerprint_sync.self_test` action: step-by-step LoadChar/UpChar/DownChar/Match diagnosis on
  the own sensor (buffers only).

- `fingerprint_sync.resolve_all_conflicts` action for the initial rollout.
- `self_test` option `compare_slot` to score two different slots.

### Changed
- A template the module matches as the same finger is no longer overwritten; both templates are
  kept. Real R503 modules report *no match* even for identical templates (score 92), so the match
  result is only a hint. Conflict messages include the score.
- `DownChar` pauses 10 ms between data packets by default: without a pause a real R503 received a
  corrupted template.

### Fixed
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
