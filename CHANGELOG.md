# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

First version.

### Added
- Component `fingerprint_grow_plus` next to the official `fingerprint_grow` on the same UART.
- `admin:` protected admin fingers with gestures (touch, lift, wait): enroll the next free slot,
  delete or re-enroll the next finger shown, enroll a guest, status on hold (`health()`);
  optional single touch as access (`access: delayed | immediate`, `on_access`).
- `guests:` guest fingers with an expiry date, deleted automatically (also across nodes).
- `sync:` (optional) node-to-node sync of templates in the same slot: anti-entropy with per-slot
  versions and tombstones, conflicts instead of overwrites, `claim_all_slots`,
  `resolve_conflict`, slot ranges (`include_slots` / `exclude_slots`), dry run.
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
