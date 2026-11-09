# CLAUDE.md – project context

ESPHome external component `fingerprint_grow_plus`: extensions for `fingerprint_grow`
(GROW R503/R30x): admin fingers with gestures (`admin:`), guest fingers with expiry (`guests:`),
protected slots, and optional template sync directly between ESPHome nodes (`sync:`; no Home
Assistant, no broker, no cloud). Without `sync:` a node runs stand-alone (no network port).

## Layout
- `components/fingerprint_grow_plus/` – the component
  - `grow_link.*` R503 template commands on the shared UART (one complete transaction per call)
  - `secure_channel.*` handshake + ChaCha20/HMAC record layer; `crypto.*` primitives
  - `transport.h`, `tcp_transport.*` transport abstraction (TCP via ESPHome socket helpers)
  - `protocol.h` messages and slot metadata encoding; `meta_storage.*` persistence
  - `fingerprint_grow_plus.*` job queue, sync decisions, sessions, rounds
  - `admin.cpp` admin gestures, protected slots, guests and expiry
  - `__init__.py` (schema: general options + `admin:` / `guests:` / `sync:` blocks, actions),
    `sensor.py`, `text_sensor.py`, `binary_sensor.py` codegen
- `tools/growplus.py` backup/restore/status CLI (stdlib only, must stay byte-compatible)
- `tests/` unit tests (`run_unit_tests.sh`), simulator (`sim/`), end-to-end (`e2e/run_e2e.py`)
- `examples/` generic, invented example configs; `docs/` documentation (German guide in `docs/de/`)

## Decisions
- Separate component next to the official `fingerprint_grow` (class is `final`, helpers protected).
- Sensor access only in jobs, max one job per `loop()`, never split a sensor transaction across
  loops; no jobs during enrollment, while a finger is on the sensor, or 1.5 s after scan activity.
- Anti-entropy with per-slot Lamport versions + tombstones; absence is never deletion.
- `sync: include_slots/exclude_slots` only limit the sync; local features use all slots.
- Different finger in the same slot => conflict, never overwrite (unless explicitly forced).
- Never implement SetPwd/SetSysPara. Never log template bytes.
- Target the current ESPHome release (new automation API: `register_apply_action`,
  `build_callback_automations`).

## Conventions
- Persistence and crypto labels must stay stable: `fingerprint_grow_plus_meta`,
  `fingerprint_grow_plus_expiry` (preference keys), `growplus-v1`, `growplus-backup-v1` (HKDF salts),
  magic `FGP`. Changing them loses stored versions/expiry dates or breaks existing backups.
- Arduino core macros (`word(...)`, `bit`, `lowByte`, `highByte`, `constrain`, `sq`, `round`, …)
  must never be used as identifiers: a lambda named `word` silently broke ReadSysPara parsing on
  Arduino builds only.
- C++: ESPHome style (`this->`, trailing `_` for protected members), English comments.
- Every change: `tests/run_unit_tests.sh` and `python3 tests/e2e/run_e2e.py` must pass.
- Protocol changes: bump `PROTOCOL_VERSION`, update `docs/protocol.md` and `tools/growplus.py`.

## Privacy rules (public repository)
- Never commit real node configurations, device names, pins, IP addresses, entity IDs,
  secrets, keys or template dumps/backups. Examples must be invented and generic.
