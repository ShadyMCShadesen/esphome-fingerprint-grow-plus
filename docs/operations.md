# Operations: rollout, backup, restore, troubleshooting

## Recommended rollout

1. **Key.** Run `python3 tools/growplus.py genkey` and put the key into `secrets.yaml` as
   `fingerprint_grow_plus_key`.
2. **Two nodes first, dry run.** Add `fingerprint_grow_plus` with a `sync:` block and
   `dry_run: true` to two nodes of the same hardware and flash them.
3. **Read the logs.** Look for:
   - `Index ready: N templates (M synced)`
   - `Sync with … finished`
   - `[dry run] would store slot X`
   - `CONFLICT in slot X`, if any

   Resolve every conflict before you continue.
4. **Back up every sensor** that already holds fingers (the files stay on your PC, never in a
   repository):

   ```bash
   python3 tools/growplus.py backup  --host <node-ip> --port 18530 --key '<key>' --out <node>.fgpb --include-local
   python3 tools/growplus.py inspect --key '<key>' --in <node>.fgpb
   ```

   `<key>` is `fingerprint_grow_plus_key` from `secrets.yaml`. On Windows use `py tools\growplus.py`
   with the same arguments. Instead of `--key` you can use `--key-file <file>` or the environment
   variable `GROWPLUS_KEY`. `--include-local` needs `allow_full_backup` on the node (see below);
   leave it out to save only the synced slots.

5. **Hardware checks.** Run the compatibility tests in [testing.md](testing.md) (A1–A4).
   Nodes that already hold fingers will report **conflicts** for every slot whose template
   differs. That is expected: the sync never guesses which finger is right.
   - Decide which node holds the correct assignment of fingers to slots (the *reference*).
   - With backups of all sensors in place, run `fingerprint_grow_plus.claim_all_slots` **on the
     reference node only**. All its templates become forced versions and replace the same slots on
     the other nodes. In `dry_run` they log `[dry run] would REPLACE slot …`.
   - Claim on **one** node only. If two nodes claim, the later claim wins per slot, which can mix
     the assignment of two nodes.
   - Slots that exist only on other nodes are copied to the reference node and everywhere else.
6. **Go live.** Set `sync: dry_run: false` on both nodes and verify. Then add the remaining nodes,
   again with a dry run first.

The tool talks to the node over the sync connection, so the node needs a `sync:` block (for a
single node a block with only `key` is enough). By default it saves the synced slots. To include
the slots that are not synced (outside `include_slots`), open the full-backup window on the node
first and add `--include-local`:

```bash
# on the node: fingerprint_grow_plus.allow_full_backup (open for 5 minutes by default)
python3 tools/growplus.py backup --host <node-ip> --port 18530 --key '<key>' --out <node>.fgpb --include-local
```

## Restore / rollback

1. On the target node, open the restore window: run `fingerprint_grow_plus.allow_restore`, e.g. from
   a web UI button. It stays open for 5 minutes by default.
2. Restore:

   ```bash
   python3 tools/growplus.py restore --host <node-ip> --port 18530 --key '<key>' --in <node>.fgpb [--slots 0,2,10-19]
   ```

3. Restored synced slots get a new, forced version. **They replace the slot on all peers**, which
   is the point of a rollback. If you only want to repair one node, restore only the slots in
   question. Local-only slots from an `--include-local` backup are written to this node only.

## Replacing a sensor

Install the new sensor and reboot the node. Its index is empty, which the sync treats as
"missing", not as "deleted": all synced slots are restored from the peers (local-only slots
have to be enrolled again or restored from a backup). With
`metadata_storage: sensor_notepad` the metadata starts fresh with the new sensor as well.

## Deleting fingers

| what | how | scope |
|---|---|---|
| one finger | `fingerprint_grow_plus.delete: <slot>` or the 3-touch admin gesture | all nodes (synced slot) or this node (local slot) |
| everything except admin fingers | `fingerprint_grow_plus.delete_all: <code>` | synced slots on all nodes, local slots on this node |
| an admin finger | `fingerprint_grow_plus.delete_admin` with slot and code | like `delete` |
| repair one node | `fingerprint_grow.delete` | this node; a synced slot comes back from the peers in the next round |

## Troubleshooting

| log / symptom | meaning | fix |
|---|---|---|
| `server authentication failed (wrong key?)` | different `fingerprint_grow_plus_key` on the nodes | use the same key everywhere |
| `not reachable` | peer offline, wrong address, firewall, VPN route | check `sync: peers`, port 18530, `allowed_networks` on the other side |
| `Inbound connection refused: not ready` | node still indexing after boot | normal for the first seconds |
| `CONFLICT in slot X` | different fingers in the same slot | `fingerprint_grow_plus.resolve_conflict` (slot, `keep_local`) |
| `Verification of slot X failed` | sensor stored something else than sent | check wiring/power; compatibility test A1 |
| `Template bytes of slot X changed since indexing` | sensor returned different bytes for the same slot (assumption A2 false) | harmless; the stored hash is updated |
| `Incompatible template from …: X bytes, this sensor uses Y bytes` | the nodes use different sensor models or firmware versions | use the same sensor model/firmware on all synced nodes; the template is not stored |
| `Only N sync versions left` | the version counter approaches its limit (16383) | plan a version reset (docs/protocol.md); watch the `versions_left` sensor |
| `Connection lost: 0` (warning from the TCP helper) | peer closed the connection after a finished session | harmless |
| `Sensor parameters unreliable` | two `ReadSysPara` answers differed or were implausible | retried automatically; power-cycle the sensor if it persists |
| status `sensor error` | `fingerprint_grow` failed / sensor not answering | check sensor, power, UART |
| sync never runs | enrollment still active (a started enrollment blocks the sensor buffers) | finish or cancel the enrollment |
