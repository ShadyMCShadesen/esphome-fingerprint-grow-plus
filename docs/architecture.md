# Architecture

## Overview

```
 ┌───────────── node ──────────────┐             ┌───────────── node ──────────────┐
 │ fingerprint_grow  (official)    │             │ fingerprint_grow                │
 │   matching, enroll, LED         │             │                                 │
 │        ▲  callbacks             │             │                                 │
 │ fingerprint_grow_plus           │  TCP 18530  │ fingerprint_grow_plus           │
 │   admin / guests                │◄───────────►│   admin / guests                │
 │   GrowLink ──UART── R503        │  encrypted  │   GrowLink ──UART── R503        │
 │   job queue (1 job / loop)      │  (sync:)    │                                 │
 │   slot metadata + hashes        │             │                                 │
 │   SecureChannel / TcpTransport  │             │                                 │
 └─────────────────────────────────┘             └─────────────────────────────────┘
```

`fingerprint_grow_plus` is a separate component on the same UART bus as `fingerprint_grow`.
Admin fingers and guests work on one node; the `sync:` block adds the network part. Without it
no socket is opened and the sync rounds below only re-read the local index.
The official class is `final` and its protocol helpers are `protected`, so it can be neither
subclassed nor reused. `fingerprint_grow_plus` therefore has its own small driver (`GrowLink`) for the
template commands of the R503 manual (`LoadChar`, `UpChar`, `DownChar`, `Store`, `DeletChar`,
`ReadSysPara`, `TempleteNum`, `ReadIndexTable`, `ReadNotepad`, `WriteNotepad`).
`SetPwd` and `SetSysPara` are not implemented: the sensor password and parameters are never changed.

### Sharing the UART safely

ESPHome runs all components cooperatively in one thread. Each `GrowLink` method performs a
**complete** transaction (command, acknowledge and data packets) before it returns, and
`fingerprint_grow` does the same in its `update()`. Two transactions can therefore never interleave.

The sensor's character buffers are shared state as well. Every sync job is self-contained: it
loads what it needs into the buffers and uses them within the same `loop()` call, so a scan by
`fingerprint_grow` between two jobs cannot corrupt anything. The only multi-loop user of the
buffers is an enrollment. From the first `on_enrollment_scan` until `on_enrollment_done` /
`on_enrollment_failed` no sync job runs.

Matching has priority:

- No job runs within 1.5 s after any scan activity (`on_finger_scan_*`).
- No job runs while the optional `finger_sensor` is on.
- At most one job runs per `loop()`. Reading or writing a template (1536 bytes on the R503,
  57600 baud) blocks the loop for about 0.3 s (read) to 1.2–1.7 s (store + read-back
  verification). A finger placed during a job is matched right after it.

## Data model

Per slot (`protocol.h`):

| field | size | meaning |
|---|---|---|
| state | 2 bit | `NONE`, `PRESENT`, `PRESENT_FORCED`, `TOMBSTONE` |
| version | 14 bit | Lamport clock value of the last change |
| hash | 8 byte | first 8 bytes of SHA-256 over the template bytes (RAM only, recomputed at boot) |

- The persistent part (state and version, 2 bytes per slot) lives in ESPHome preferences (ESP32)
  or in the sensor's notepad (ESP8266, optional on ESP32).
- **Origin node:** shown in the logs, not stored. The protocol does not need it.
- **Clock:** the cluster clock is the maximum version seen. A local change gets `clock + 1`.

## Sync rules

A sync **round** is started:

- after boot (`sync: at_boot`, default on),
- 2 s after a local change (`sync: after_change`, default on),
- every `sync: interval` (default 15 min; `never` switches it off),
- by `fingerprint_grow_plus.sync` (always).

A round first re-reads the sensor's index table, which also catches changes made by
`fingerprint_grow` actions. It then opens one session per peer, one after the other. In a session
both sides send their manifest (all synced, non-empty slots with state, version and hash). Each
side then pulls what it needs, so one connection syncs both directions.

Decision for one slot, local `L`, remote `R`:

| remote | local | action |
|---|---|---|
| present | none | pull and store |
| present | tombstone | pull if `R.version > L.version` |
| present | present, same hash | adopt the higher version (metadata only) |
| present | present, different hash, R forced & newer | pull and **replace** (deliberate re-enrollment) |
| present | present, different hash, L forced & newer | nothing (the peer pulls ours) |
| present | present, different hash, otherwise | **conflict** |
| tombstone | present | delete if `R.version > L.version` |
| tombstone | none / older tombstone | adopt the tombstone |

**Conflict:** nothing is written; `on_conflict(slot)` fires once and the `conflict_count` sensor
goes up.

> Why no automatic "same finger" detection: on real R503 modules the template-to-template
> `Match` command returns arbitrary results (an identical copy scored 92 in one run and 0 in the
> next, two different fingers scored 92). Two templates in one slot can therefore not be judged
> automatically, and a stored finger is never replaced on a guess.

Resolve a conflict with `resolve_conflict(slot, keep_local)`, or all of them with
`resolve_all_conflicts(keep_local)`:
  - `keep_local: true` marks the local template `PRESENT_FORCED` with a new version, and all
    peers replace theirs.
  - `keep_local: false` drops the local template without a tombstone, and the next round fetches
    the peer's.

**"Absence is not deletion."** A slot that is merely missing locally (new or replaced sensor,
`fingerprint_grow.delete`) is restored from the peers. Only tombstones delete.

**Enrollment:**

- `on_enrollment_done(slot)` hashes the new template and gives it `clock + 1`.
- If the slot was occupied before, the user deliberately re-enrolled it, and the template is
  marked `PRESENT_FORCED`. It then replaces the old finger on the peers (with a WARN log).
- If the slot was empty locally but holds a different finger elsewhere, the result is a
  conflict, never an overwrite.
- `enroll_next_free` avoids this. It picks the lowest slot in `enroll_slots` that is empty here,
  has never been seen occupied on a peer since boot, has no conflict or tombstone, and is neither
  an admin slot nor (except for guests) a guest slot.

**Deletion:**

- `fingerprint_grow_plus.delete` writes a tombstone with a new version, which propagates.
- `delete_all` requires the configured code. It writes tombstones for all synced slots except
  the admin slots and deletes the local-only slots of this node. Local-only slots of other nodes
  are never touched.

**Slot filter:** `sync: include_slots` / `exclude_slots` define the synced slots of a node.
Other slots are local only: not announced, not accepted and not deleted by the sync, but admin
gestures, guests, `enroll_next_free` and `delete_all` still work on them. Excluding a range on
all nodes but one makes it node-exclusive; `include_slots: ["99"]` shares only slot 99.

## Failure behaviour

| situation | behaviour |
|---|---|
| peer offline | session fails after 12 s, round reports `sync: on_failed`, retried next round; the peer catches up when it is back |
| reboot during a transfer | nothing half-written: a template is stored only after the complete, checksum-verified message arrived; the store is verified by a byte-exact read-back and deleted if verification fails |
| connection lost mid-session | session ends, remaining slots are synced in the next round |
| sensor replaced | index empty → all slots "missing" → restored from peers (no deletion) |
| sensor full / slot out of range | `Store` error (0x0B/0x18) → error counter, nothing else changes |
| transmission error | record MAC / template SHA-256 mismatch → session aborted, template discarded |
| sensor status not OK / `fingerprint_grow` failed | jobs paused, status `sensor error`, retried |
| scan or enrollment during sync | jobs wait (see above); sessions tolerate the delay |
| two partitions change the same slot | higher version wins (Lamport order); equal versions → compare/conflict |

## Why not …

- **Home Assistant as hub.** Excluded by design. The sync must work while HA is down.
- **ESP-NOW.** ESP32 only and not routable.
- **`packet_transport` / `udp`.** Built for small sensor states. A 1536-byte template would need
  fragmentation and our own retransmission. `udp` also accepts IPv4 literals only, no hostnames.
- **A master node.** It would be a single point of failure, and enrolling would be restricted to
  one node.
