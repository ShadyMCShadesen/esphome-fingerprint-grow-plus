# CLAUDE.md – project context

ESPHome external component `fingerprint_grow_plus`: extensions for `fingerprint_grow`
(GROW R503/R30x): admin fingers with gestures (`admin:`), guest fingers with expiry (`guests:`),
protected slots, and optional template sync directly between ESPHome nodes (`sync:`; no Home
Assistant, no broker, no cloud). Without `sync:` a node runs stand-alone (no network port).

## Check everything (use this, not the single steps)
- `tests/check.sh --quick` – unit tests + example configs (~1 min)
- `tests/check.sh` – plus end-to-end tests (~8 min). Prints only failures and a summary.
- `ESPHOME=/path/to/esphome` if `esphome` is not on PATH. ESP32 builds only run in CI.

## Where to change what
| Task | Files |
|---|---|
| YAML option / action / condition | `components/fingerprint_grow_plus/__init__.py` (+ setter in `fingerprint_grow_plus.h`) |
| Admin gestures, protected slots, guests, expiry | `admin.cpp` |
| Sync decisions, jobs, sessions, rounds, backup serving | `fingerprint_grow_plus.cpp` |
| R503 commands on the UART | `grow_link.*` |
| Wire format | `protocol.h`, `docs/protocol.md`, `tools/growplus.py` (bump `PROTOCOL_VERSION`) |
| Entities | `sensor.py`, `text_sensor.py`, `binary_sensor.py` |
| Tests | `tests/e2e/run_e2e.py` (scenarios, node configs `NODES`/`YAML_TEMPLATE`), `tests/sim/r503_sim.py` |
| User docs | `README.md` (reference), `docs/de/anleitung.md` (German), `docs/*.md` |

## Decisions
- Separate component next to the official `fingerprint_grow` (class is `final`, helpers protected).
- Sensor access only in jobs, max one job per `loop()`, never split a sensor transaction across
  loops; no jobs during enrollment, while a finger is on the sensor, or 1.5 s after scan activity.
- Anti-entropy with per-slot Lamport versions + tombstones; absence is never deletion.
- `sync: include_slots/exclude_slots` only limit the sync; local features use all slots.
  Local-only slots go to the backup tool only during `allow_full_backup`.
- Different finger in the same slot => conflict, never overwrite (unless explicitly forced).
- The module's template `Match` is unreliable on R503: never used. Writes are verified by read-back.
- Never implement SetPwd/SetSysPara. Never log template bytes.
- Target the current ESPHome release (automation API: `register_apply_action`,
  `build_callback_automations`).

## Conventions
- Persistence and crypto labels must stay stable: `fingerprint_grow_plus_meta`,
  `fingerprint_grow_plus_expiry` (preference keys), `growplus-v1`, `growplus-backup-v1` (HKDF salts),
  magic `FGP` + `PROTOCOL_VERSION`. Changing them loses stored data or breaks backups.
- Arduino core macros (`word`, `bit`, `lowByte`, `highByte`, `constrain`, `sq`, `round`, …) must
  never be used as identifiers (a lambda named `word` broke ReadSysPara on Arduino builds only).
- C++: ESPHome style (`this->`, trailing `_` for protected members), English comments.
- Every change: `tests/check.sh` must pass. Update README reference and the German guide with it.

## Privacy rules (public repository)
- Never commit real node configurations, device names, pins, IP addresses, entity IDs,
  secrets, keys or template dumps/backups. Examples must be invented and generic.
