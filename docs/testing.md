# Testing

## Automated (runs in CI)

| test | command | covers |
|---|---|---|
| crypto unit tests | `tests/run_unit_tests.sh` | ChaCha20 vs OpenSSL, HKDF vs RFC 5869, handshake, tamper/replay/wrong key/self connection, byte-exact interop with `tools/growplus.py` |
| end-to-end | `python3 tests/e2e/run_e2e.py` | 6 real firmware builds (ESPHome host platform) + simulated R503 sensors, see below |
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
3. Admin finger (slot 0 touched twice) enrolls the next free slot, which propagates; a second
   admin finger re-enrolls a slot with 4 touches + finger (new template replaces it everywhere);
   a guest finger is enrolled, synced and deleted everywhere when it expires; admin slots
   cannot be deleted (action refused, admin finger as target cancels).
4. Deletion propagates.
5. Node offline → catches up after restart; tombstones survive restarts.
6. Encrypted backup; `delete_all` with a wrong code does nothing, with the right code it wipes
   all managed slots except the admin fingers (exclusive range survives).
7. Restore from the backup is refused without a restore window, works with one, and propagates.
8. Hostile peers are cut off (handshake timeout, duplicate manifest).
9. An admin deletes a finger on all nodes by presenting it after 3 touches.
10. A stand-alone node (no key) enrolls with the admin gesture; the finger stays local; with
    `access: delayed` a single admin touch fires `on_admin_access`, two touches do not.
11. A node that syncs only the admin slot receives it, keeps its local fingers out of the sync,
    enrolls into a local slot with the admin gesture and reports `occupied_slots`.
12. `SetPwd` is never sent to any sensor.

The simulator (`tests/sim/r503_sim.py`) implements the R503 packet protocol subset used by
`fingerprint_grow` and `fingerprint_grow_plus` with synthetic templates (no real biometric data).

## Hardware test plan

Recommended for the proof of concept: two nodes with the same hardware but different frameworks
(ESP-IDF and Arduino).

### A. Assumptions about the sensor (verify before going live)

| ID | assumption | test | expected |
|---|---|---|---|
| A1 | a template exported from sensor X matches on sensor Y | limit both nodes to one test slot (`include_slots: ["190"]`, `dry_run: false`), enroll a test finger there on node 1, sync, touch node 2 | `on_finger_scan_matched` with the same slot on node 2 |
| A2 | `UpChar` of a slot returns identical bytes each time | log line `Template bytes of slot … changed` must not appear after two rounds | no such line (if it appears: harmless, sync still correct) |
| A3 | template size and packet size | check `Slot N indexed (template … bytes)` and `Sensor: capacity …, packet size …` | **observed on R503: 1536 bytes, 128-byte packets** (manual: 768) |
| A4 | a template survives sensor → ESP → sensor unchanged | run `fingerprint_grow_plus.self_test` and read `SELF TEST 2/3/4: round trip …` | **observed on R503: BYTE-IDENTICAL with 0, 10 and 40 ms gaps** (10 ms default kept as margin) |
| A5 | `Match` template vs template | `self_test` variants 1 and 5 | **observed on R503: arbitrary scores (0 or 92 for the same pair)** → not used by the sync |

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
