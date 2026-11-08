# Testing

## Automated (runs in CI)

| test | command | covers |
|---|---|---|
| crypto unit tests | `tests/run_unit_tests.sh` | ChaCha20 vs OpenSSL, HKDF vs RFC 5869, handshake, tamper/replay/wrong key/self connection, byte-exact interop with `tools/fpsync.py` |
| end-to-end | `python3 tests/e2e/run_e2e.py` | 5 real firmware builds (ESPHome host platform) + simulated R503 sensors, see below |
| config + compile | GitHub Actions | `esphome config` and `esphome compile` for ESP32 Arduino, ESP32 ESP-IDF, ESP8266 |

End-to-end scenarios:

1. Initial full sync of three nodes:
   - slot IDs preserved
   - same finger enrolled separately on two nodes converges
   - a different finger in the same slot raises a conflict and is not overwritten
   - a node-exclusive slot range stays local
   - a node with the wrong key is rejected
   - a dry-run node changes nothing
2. Conflict resolution.
3. Admin finger (slot 0 touched twice) enrolls the next free slot, which propagates.
4. Deletion propagates.
5. Node offline → catches up after restart; tombstones survive restarts.
6. Encrypted backup; `delete_all` with a wrong code does nothing, with the right code it wipes
   all managed slots (exclusive range survives).
7. Restore from the backup is refused without a restore window, works with one, and propagates.
8. `SetPwd` is never sent to any sensor.

The simulator (`tests/sim/r503_sim.py`) implements the R503 packet protocol subset used by
`fingerprint_grow` and `fingerprint_sync` with synthetic templates (no real biometric data).

## Hardware test plan

Recommended for the proof of concept: two nodes with the same hardware but different frameworks
(ESP-IDF and Arduino).

### A. Assumptions about the sensor (verify before going live)

| ID | assumption | test | expected |
|---|---|---|---|
| A1 | a template exported from sensor X matches on sensor Y | limit both nodes to one test slot (`include_slots: ["190"]`, `dry_run: false`), enroll a test finger there on node 1, sync, touch node 2 | `on_finger_scan_matched` with the same slot on node 2 |
| A2 | `UpChar` of a slot returns identical bytes each time | log line `Template bytes of slot … changed` must not appear after two rounds | no such line (if it appears: harmless, sync still correct) |
| A3 | template size and packet size | check `Slot N indexed (template … bytes)` and `Sensor: capacity …, packet size …` | **observed on R503: 1536 bytes, 128-byte packets** (manual: 768) |
| A4 | `DownChar` delivers the template unchanged | run `fingerprint_sync.self_test` (slot 0) and compare the scores | **observed on R503:** score 0 without a packet gap (corrupted), score 92 with 10/40 ms gaps = identical to variant 1 (no download). Default gap: 10 ms |
| A5 | `Match` template vs template | `self_test` variant 1 and variant 5 (`compare_slot`) | **observed on R503:** identical templates give *no match*, score 92 → `Match` is only a hint, never a reason to overwrite |

### B. Functional tests

| # | test | expected |
|---|---|---|
| B1 | enroll on node 1 → sync → match on node 2 | same slot ID, match OK |
| B2 | slot IDs incl. special IDs (0, 2, 4, 99) | identical slot numbers on all nodes |
| B3 | delete on node 2 | gone on all nodes, does not come back after reboots |
| B4 | collision: different fingers in the same slot on two nodes | `on_conflict`, nothing overwritten |
| B5 | node offline while enrolling elsewhere, then online | node catches up within one `sync_interval` |
| B6 | power off a node during a sync | no half-written slot; next round completes |
| B7 | sensor full (all managed slots used) | `enroll_next_free` refuses; no crash |
| B8 | touch the sensor during a sync | match works, delayed at most about 0.6 s |
| B9 | admin finger, button and web UI enrollment | next free slot enrolled and propagated |
| B10 | `delete_all` wrong code / right code | nothing / all managed slots on all nodes |

### C. Backup and rollback

Before the first live sync, back up every sensor (`docs/operations.md`). After the tests, practise
one restore of a single slot.
