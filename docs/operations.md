# Operations: rollout, backup, restore, troubleshooting

## Recommended rollout

1. **Key.** Run `python3 tools/fpsync.py genkey` and put the key into `secrets.yaml` as
   `fingerprint_sync_key`.
2. **Two nodes first, dry run.** Add `fingerprint_sync` with `dry_run: true` to two nodes of the
   same hardware and flash them.
3. **Read the logs.** Look for:
   - `Index ready: N managed templates`
   - `Sync with … finished`
   - `[dry run] would store slot X`
   - `CONFLICT in slot X`, if any

   Resolve every conflict before you continue.
4. **Back up every sensor** that already holds fingers (the files stay on your PC, never in a
   repository):

   ```bash
   export FPSYNC_KEY='<key from secrets.yaml>'
   python3 tools/fpsync.py backup --host <node>.local --out <node>-$(date +%F).fpsb
   python3 tools/fpsync.py inspect --in <node>-$(date +%F).fpsb
   ```

5. **Hardware checks.** Run the compatibility tests in [testing.md](testing.md) (A1–A3).
6. **Go live.** Set `dry_run: false` on both nodes and verify. Then add the remaining nodes,
   again with a dry run first.

## Restore / rollback

1. On the target node, open the restore window: run `fingerprint_sync.allow_restore`, e.g. from
   a web UI button. It stays open for 5 minutes by default.
2. Restore:

   ```bash
   python3 tools/fpsync.py restore --host <node>.local --in backup.fpsb [--slots 0,2,10-19]
   ```

3. Restored slots get a new, forced version. **They replace the slot on all peers**, which is the
   point of a rollback. If you only want to repair one node, restore only the slots in question.

## Replacing a sensor

Install the new sensor and reboot the node. Its index is empty, which the sync treats as
"missing", not as "deleted": all managed slots are restored from the peers. With
`metadata_storage: sensor_notepad` the metadata starts fresh with the new sensor as well.

## Deleting fingers

| what | how | scope |
|---|---|---|
| one finger everywhere | `fingerprint_sync.delete: <slot>` | cluster |
| everything | `fingerprint_sync.delete_all` with code and `confirm_cluster_wide: true` | cluster (managed slots) |
| repair one node | `fingerprint_grow.delete` | local; restored from peers in the next round |

## Troubleshooting

| log / symptom | meaning | fix |
|---|---|---|
| `server authentication failed (wrong key?)` | different `fingerprint_sync_key` on the nodes | use the same key everywhere |
| `not reachable` | peer offline, wrong address, firewall | check `peers`, port 18530, `allowed_networks` |
| `Inbound connection refused: not ready` | node still indexing after boot | normal for the first seconds |
| `CONFLICT in slot X` | different fingers in the same slot | `fingerprint_sync.resolve_conflict` (slot, `keep_local`) |
| `Verification of slot X failed` | sensor stored something else than sent | check wiring/power; compatibility test A1 |
| `Template bytes of slot X changed since indexing` | sensor returns different bytes for the same slot (assumption A2 false) | harmless; the sync falls back to the sensor's own match |
| `Connection lost: 0` (warning from the TCP helper) | peer closed the connection after a finished session | harmless |
| `Comparing slot X failed at Match (0xFF, 0 answer bytes …)` | the module does not answer after a template download | run `fingerprint_sync.self_test` and report the `SELF TEST` lines |
| `Sensor parameters unreliable` | two `ReadSysPara` answers differed or were implausible | retried automatically; power-cycle the sensor if it persists |
| status `sensor error` | `fingerprint_grow` failed / sensor not answering | check sensor, power, UART |
| sync never runs | enrollment still active (a started enrollment blocks the sensor buffers) | finish or cancel the enrollment |
