# Testing

## Automated (runs in CI)

`tests/check.sh` runs all of them with short output (`--quick` skips the end-to-end tests).

| test | command | covers |
|---|---|---|
| crypto unit tests | `tests/run_unit_tests.sh` | ChaCha20 vs OpenSSL, HKDF vs RFC 5869, handshake, tamper/replay/wrong key/self connection, byte-exact interop with `tools/growplus.py` |
| end-to-end | `python3 tests/e2e/run_e2e.py` | 7 real firmware builds (ESPHome host platform) + simulated R503 sensors, see below |
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
   all synced slots except the admin fingers (another node's exclusive range survives).
7. Restore from the backup is refused without a restore window, works with one, and propagates.
8. Hostile peers are cut off (handshake timeout, duplicate manifest).
9. An admin deletes a finger on all nodes by presenting it after 3 touches.
10. A stand-alone node (no `sync:` block) enrolls with the admin gesture; with
    `access: delayed` a single admin touch fires `on_access`, two touches do not, and holding the
    admin finger (even with a second match during the hold) shows the status without access;
    `led_feedback` shows the expected colour sequence (checked in the simulator's LED log).
11. A node that syncs only the admin slot receives it, keeps its local fingers out of the sync,
    enrolls into a local slot with the admin gesture and reports `occupied_slots`. A default backup
    holds only the synced slot, `--include-local` is refused without `allow_full_backup` and
    includes the local slots with it; a local slot is restored without being synced.
12. A node with `interval: never`, `at_boot: false`, `after_change: false` syncs only when asked;
    a finger enrolled there reaches the others with the next manual sync.
13. Diagnostics: a peer with the wrong key is listed in `unreachable_peers`, `versions_left` is
    reported, and a restore with a template of a different size is refused.
14. A node locks after 3 unrecognised fingers and passes the lockout only to the peer in
    `share_with`; both unlock after the duration; a duress finger fires `on_duress`.
15. Time windows: `is_allowed` for an open window, a closed window, a slot without schedule and
    windows over midnight; a finger outside its window fires `on_denied`.
16. `SetPwd` is never sent to any sensor.

The simulator (`tests/sim/r503_sim.py`) implements the R503 packet protocol subset used by
`fingerprint_grow` and `fingerprint_grow_plus` with synthetic templates (no real biometric data).

## Hardware test plan

Recommended for the proof of concept: two nodes with the same hardware but different frameworks
(ESP-IDF and Arduino).

### A. Assumptions about the sensor (verify before going live)

| ID | assumption | test | expected |
|---|---|---|---|
| A1 | a template exported from sensor X matches on sensor Y | limit both nodes to one test slot (`sync: include_slots: ["190"]`, `dry_run: false`), enroll a test finger there on node 1, sync, touch node 2 | `on_finger_scan_matched` with the same slot on node 2 |
| A2 | `UpChar` of a slot returns identical bytes each time | log line `Template bytes of slot … changed` must not appear after two rounds | no such line (if it appears: harmless, sync still correct) |
| A3 | template size and packet size | check `Slot N indexed (template … bytes)` and `Sensor: capacity …, packet size …` | **observed on R503: 1536 bytes, 128-byte packets** (manual: 768) |
| A4 | a template survives sensor → ESP → sensor unchanged | every sync write is read back and compared byte by byte; `Verification of slot … failed` must not appear | **observed on R503: byte-identical with 0, 10 and 40 ms packet gaps** (10 ms used as margin) |
| A5 | `Match` template vs template | (checked during development) | **observed on R503: arbitrary scores (0 or 92 for the same pair)** → not used |

### B. Functional tests

| # | test | expected |
|---|---|---|
| B1 | enroll on node 1 → sync → match on node 2 | same slot ID, match OK |
| B2 | slot IDs incl. special IDs (0, 2, 4, 99) | identical slot numbers on all nodes |
| B3 | delete on node 2 | gone on all nodes, does not come back after reboots |
| B4 | collision: different fingers in the same slot on two nodes | `on_conflict`, nothing overwritten |
| B5 | node offline while enrolling elsewhere, then online | node catches up within one `sync: interval` |
| B6 | power off a node during a sync | no half-written slot; next round completes |
| B7 | sensor full (all slots used) | `enroll_next_free` refuses; no crash |
| B8 | touch the sensor during a sync | match works, delayed at most about 1.7 s |
| B9 | admin finger, button and web UI enrollment | next free slot enrolled and propagated |
| B10 | `delete_all` wrong code / right code | nothing / all fingers except admins, on all nodes |

### C. Backup and rollback

Before the first live sync, back up every sensor (`docs/operations.md`). After the tests, practise
one restore of a single slot.
