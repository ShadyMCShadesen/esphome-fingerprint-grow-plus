# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
