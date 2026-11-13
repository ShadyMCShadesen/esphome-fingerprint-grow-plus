#!/usr/bin/env python3
"""growplus - backup, restore and inspection tool for fingerprint_grow_plus nodes.

Standard library only. Speaks the same authenticated, encrypted protocol as the nodes
(see docs/protocol.md) and therefore needs the shared key and a node with a `sync:` block.
By default only synced slots are saved; --include-local adds the local-only slots.

Commands
  genkey                                   print a new random key for secrets.yaml
  status  --host H [--include-local]       list the slots of a node with name, guest expiry and
                                           uses (no template data); --include-local adds the
                                           local-only slots (run allow_full_backup first)
  backup  --host H --out FILE              download all synced templates into an encrypted file;
          [--include-local]                --include-local also saves the slots that are not
                                           synced (run allow_full_backup on the node first)
  restore --host H --in FILE [--slots ..]  upload templates (node must run allow_restore first)
  inspect --in FILE                        list the content of a backup file
  log     --host H [--host H2:PORT ...]    access log of one or more nodes (access_log:),
          [--csv]                          merged and sorted by time
  compare A B [--include-local]            compare two backups and/or nodes (A, B = backup file
                                           or host[:port]); exit code 1 if they differ
  copy    --from A --to H [--slots ..]     copy fingers from a node or backup file to another
          [--include-local] [--overwrite]  node (target must run allow_restore and
          [--dry-run]                      allow_full_backup first); slots that hold another
                                           finger on the target are skipped unless --overwrite

The key is read from --key, from --key-file or from the environment variable GROWPLUS_KEY.
Backup files are encrypted and authenticated with a key derived from the shared key.
Keep them out of any repository: they contain biometric templates.
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import time

DEFAULT_PORT = 18530
MAGIC = b"FGP\x07"  # last byte = protocol version
TAG_SIZE = 16
MAX_RECORD_BODY = 2560
(MSG_MANIFEST, MSG_TEMPLATE_REQ, MSG_TEMPLATE, MSG_TEMPLATE_NAK, MSG_DONE, MSG_RESTORE, MSG_RESTORE_ACK,
 MSG_LOCAL_LIST_REQ, MSG_LOCAL_LIST, MSG_LOCK, MSG_ACCESS_LOG_REQ, MSG_ACCESS_LOG) = range(1, 13)
ACCESS_RESULTS = ["matched", "unknown", "outside_schedule", "while_locked", "duress"]
ACCESS_FLAGS = [(1, "admin"), (2, "guest"), (4, "admin-target")]
STATE_NAMES = {0: "none", 1: "present", 2: "present (forced)", 3: "deleted"}


# --------------------------------------------------------------------------- crypto
def _rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _quarter(x, a, b, c, d):
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF
    x[d] = _rotl(x[d] ^ x[a], 16)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF
    x[b] = _rotl(x[b] ^ x[c], 12)
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF
    x[d] = _rotl(x[d] ^ x[a], 8)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF
    x[b] = _rotl(x[b] ^ x[c], 7)


def chacha20_xor(key: bytes, nonce: bytes, counter: int, data: bytes) -> bytes:
    """ChaCha20 as in RFC 8439 section 2.4."""
    const = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    k = list(struct.unpack("<8I", key))
    n = list(struct.unpack("<3I", nonce))
    out = bytearray()
    for block_index in range((len(data) + 63) // 64):
        state = const + k + [(counter + block_index) & 0xFFFFFFFF] + n
        x = list(state)
        for _ in range(10):
            _quarter(x, 0, 4, 8, 12)
            _quarter(x, 1, 5, 9, 13)
            _quarter(x, 2, 6, 10, 14)
            _quarter(x, 3, 7, 11, 15)
            _quarter(x, 0, 5, 10, 15)
            _quarter(x, 1, 6, 11, 12)
            _quarter(x, 2, 7, 8, 13)
            _quarter(x, 3, 4, 9, 14)
        stream = struct.pack("<16I", *[(x[i] + state[i]) & 0xFFFFFFFF for i in range(16)])
        chunk = data[block_index * 64 : (block_index + 1) * 64]
        out.extend(a ^ b for a, b in zip(chunk, stream))
    return bytes(out)


def hkdf(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, t, counter = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        out += t
        counter += 1
    return out[:length]


# --------------------------------------------------------------------------- channel
class Channel:
    """Client side of the SecureChannel record layer (see secure_channel.h)."""

    def __init__(self, sock: socket.socket, psk: bytes, own_id: int, nonce: bytes | None = None):
        self.sock = sock
        self.psk = psk
        self.own_id = own_id
        self.nonce_c = nonce if nonce is not None else os.urandom(16)
        self.rx = b""
        self.tx_seq = 0
        self.rx_seq = 0
        self.peer_id = 0

    def _send_record(self, body: bytes):
        self.sock.sendall(struct.pack(">H", len(body)) + body)

    def _recv_record(self) -> bytes:
        while True:
            if len(self.rx) >= 2:
                length = struct.unpack(">H", self.rx[:2])[0]
                if length == 0 or length > MAX_RECORD_BODY:
                    raise ConnectionError("invalid record length")
                if len(self.rx) >= 2 + length:
                    body = self.rx[2 : 2 + length]
                    self.rx = self.rx[2 + length :]
                    return body
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("connection closed by node")
            self.rx += data

    def handshake(self):
        c_hello = MAGIC + b"\x01" + struct.pack(">I", self.own_id) + self.nonce_c
        self._send_record(c_hello)
        s_hello = self._recv_record()
        if len(s_hello) != 57 or s_hello[:4] != MAGIC or s_hello[4] != 2:
            raise ConnectionError("unexpected server hello (wrong port or protocol version?)")
        server_id = struct.unpack(">I", s_hello[5:9])[0]
        nonce_s = s_hello[9:25]
        info = self.nonce_c + nonce_s + struct.pack(">II", self.own_id, server_id)
        keys = hkdf(b"growplus-v1", self.psk, info, 160)
        self.k_auth = keys[0:32]
        self.enc_tx, self.mac_tx = keys[32:64], keys[64:96]
        self.enc_rx, self.mac_rx = keys[96:128], keys[128:160]
        proof_s = hmac.new(self.k_auth, b"server" + c_hello + s_hello[:25], hashlib.sha256).digest()
        if not hmac.compare_digest(proof_s, s_hello[25:57]):
            raise ConnectionError("node authentication failed (wrong key?)")
        proof_c = hmac.new(self.k_auth, b"client" + c_hello + s_hello, hashlib.sha256).digest()
        self._send_record(MAGIC + b"\x03" + proof_c)
        self.peer_id = server_id

    def send(self, msg: bytes):
        seq = struct.pack(">I", self.tx_seq)
        ct = chacha20_xor(self.enc_tx, seq + bytes(8), 0, msg)
        body_len = 4 + len(ct) + TAG_SIZE
        tag = hmac.new(self.mac_tx, struct.pack(">H", body_len) + seq + ct, hashlib.sha256).digest()[:TAG_SIZE]
        self._send_record(seq + ct + tag)
        self.tx_seq += 1

    def recv(self) -> bytes:
        body = self._recv_record()
        if len(body) < 4 + TAG_SIZE:
            raise ConnectionError("short record")
        seq, ct, tag = body[:4], body[4:-TAG_SIZE], body[-TAG_SIZE:]
        if struct.unpack(">I", seq)[0] != self.rx_seq:
            raise ConnectionError("unexpected sequence number")
        expected = hmac.new(self.mac_rx, struct.pack(">H", len(body)) + seq + ct, hashlib.sha256).digest()[:TAG_SIZE]
        if not hmac.compare_digest(tag, expected):
            raise ConnectionError("record authentication failed")
        self.rx_seq += 1
        return chacha20_xor(self.enc_rx, seq + bytes(8), 0, ct)


# --------------------------------------------------------------------------- session helpers
def load_key(args) -> bytes:
    value = args.key or os.environ.get("GROWPLUS_KEY")
    if args.key_file:
        with open(args.key_file, encoding="utf-8") as f:
            value = f.read().strip()
    if not value:
        sys.exit("No key given (use --key, --key-file or GROWPLUS_KEY)")
    key = base64.b64decode(value)
    if len(key) != 32:
        sys.exit("The key must be base64 of exactly 32 bytes")
    return key


def connect(args, key: bytes) -> Channel:
    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    tool_id = int.from_bytes(os.urandom(4), "big") | 1
    ch = Channel(sock, key, tool_id)
    ch.handshake()
    # Offer an empty manifest: the node will not pull anything from this tool.
    ch.send(bytes([MSG_MANIFEST, 0, 1]) + struct.pack(">HHH", 0, 0, 0))  # capacity, epoch, count
    return ch


MAX_NAME_LEN = 24


def parse_extra(msg: bytes, pos: int):
    """Extra = u32 expires | u8 uses (0 = unlimited) | u8 name_len | name. Returns (dict, new pos)."""
    expires, uses, name_len = struct.unpack(">IBB", msg[pos : pos + 6])
    name = msg[pos + 6 : pos + 6 + name_len].decode("utf-8", "replace")
    return {"expires": expires, "uses": uses, "name": name}, pos + 6 + name_len


def pack_extra(entry: dict) -> bytes:
    name = entry.get("name", "").encode("utf-8")[:MAX_NAME_LEN]
    return struct.pack(">IBB", entry.get("expires", 0), entry.get("uses", 0), len(name)) + name


def read_manifest(ch: Channel):
    entries = {}
    while True:
        msg = ch.recv()
        if msg[0] == MSG_MANIFEST:
            _, flags = msg[1], msg[2]
            count = struct.unpack(">H", msg[7:9])[0]  # after capacity and epoch
            pos = 9
            for _ in range(count):
                slot, meta = struct.unpack(">HH", msg[pos : pos + 4])
                digest = msg[pos + 4 : pos + 12]
                extra, pos = parse_extra(msg, pos + 12)
                entries[slot] = (meta, digest, extra)
            if flags & 1:
                return entries
        elif msg[0] == MSG_DONE:
            continue
        else:
            raise ConnectionError(f"unexpected message 0x{msg[0]:02x} while reading manifest")


def read_local_list(ch: Channel):
    """Ask for the local-only slots (needs an open full-backup window on the node)."""
    ch.send(bytes([MSG_LOCAL_LIST_REQ]))
    entries = {}
    while True:
        msg = ch.recv()
        if msg[0] != MSG_LOCAL_LIST:
            continue  # e.g. DONE from the node
        flags = msg[2]
        if flags & 2:
            return None
        count = struct.unpack(">H", msg[3:5])[0]
        pos = 5
        for _ in range(count):
            slot = struct.unpack(">H", msg[pos : pos + 2])[0]
            entries[slot], pos = parse_extra(msg, pos + 2)
        if flags & 1:
            return entries


def read_access_log(ch: Channel):
    """The node's access log, oldest first; None if the node has no access_log: block."""
    ch.send(bytes([MSG_ACCESS_LOG_REQ]))
    received = time.time()
    entries = []
    while True:
        msg = ch.recv()
        if msg[0] != MSG_ACCESS_LOG:
            continue  # e.g. DONE from the node
        flags = msg[2]
        if flags & 2:
            return None
        count = struct.unpack(">H", msg[3:5])[0]
        pos = 5
        for _ in range(count):
            unix, age, slot, confidence, result, eflags, name_len = struct.unpack(">IIHHBBB", msg[pos : pos + 15])
            name = msg[pos + 15 : pos + 15 + name_len].decode("utf-8", "replace")
            pos += 15 + name_len
            entries.append({"time": unix or int(received - age), "clock": bool(unix), "slot": slot,
                            "confidence": confidence, "name": name, "flags": eflags,
                            "result": ACCESS_RESULTS[result] if result < len(ACCESS_RESULTS) else str(result)})
        if flags & 1:
            return entries


def fetch_template(ch: Channel, slot: int):
    """Returns (meta, extra, data) or None if the node could not read the slot."""
    ch.send(bytes([MSG_TEMPLATE_REQ]) + struct.pack(">H", slot))
    while True:
        msg = ch.recv()
        if msg[0] in (MSG_TEMPLATE, MSG_TEMPLATE_NAK):
            break
    if msg[0] == MSG_TEMPLATE_NAK:
        return None
    r_slot, r_meta = struct.unpack(">HH", msg[1:5])
    r_hash = msg[5:13]
    extra, pos = parse_extra(msg, 13)
    length = struct.unpack(">H", msg[pos : pos + 2])[0]
    data = msg[pos + 2 : pos + 2 + length]
    if r_slot != slot or hashlib.sha256(data).digest()[:8] != r_hash:
        sys.exit(f"slot {slot}: checksum mismatch, backup aborted")
    return r_meta, extra, data


def finish(ch: Channel):
    ch.send(bytes([MSG_DONE]))
    try:
        ch.sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    ch.sock.close()


def parse_slots(text: str | None):
    if not text:
        return None
    slots = set()
    for part in text.split(","):
        if "-" in part:
            lo, hi = part.split("-")
            slots.update(range(int(lo), int(hi) + 1))
        else:
            slots.add(int(part))
    return slots


# --------------------------------------------------------------------------- backup file
FILE_MAGIC = b"FGPB\x01"


def file_keys(key: bytes, salt: bytes):
    k = hkdf(b"growplus-backup-v1", key, salt, 64)
    return k[:32], k[32:]


def write_backup(path: str, key: bytes, payload: dict):
    salt = os.urandom(16)
    enc, mac = file_keys(key, salt)
    plain = json.dumps(payload).encode()
    ct = chacha20_xor(enc, bytes(12), 0, plain)
    tag = hmac.new(mac, FILE_MAGIC + salt + ct, hashlib.sha256).digest()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(FILE_MAGIC + salt + ct + tag)


def read_backup(path: str, key: bytes) -> dict:
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(FILE_MAGIC) or len(raw) < len(FILE_MAGIC) + 16 + 32:
        sys.exit("Not a growplus backup file")
    salt = raw[5:21]
    ct, tag = raw[21:-32], raw[-32:]
    enc, mac = file_keys(key, salt)
    if not hmac.compare_digest(tag, hmac.new(mac, FILE_MAGIC + salt + ct, hashlib.sha256).digest()):
        sys.exit("Backup file authentication failed (wrong key or corrupted file)")
    return json.loads(chacha20_xor(enc, bytes(12), 0, ct))


# --------------------------------------------------------------------------- commands
def cmd_genkey(_args):
    print(base64.b64encode(os.urandom(32)).decode())


def cmd_status(args):
    key = load_key(args)
    ch = connect(args, key)
    entries = read_manifest(ch)
    local = read_local_list(ch) if args.include_local else {}
    finish(ch)
    if local is None:
        sys.exit("Node refused the local slots: run the fingerprint_grow_plus.allow_full_backup action "
                 "on the node first (or leave out --include-local)")
    rows = {slot: f"{STATE_NAMES[meta >> 14]:<17} version {meta & 0x3FFF}{describe(extra)}"
            for slot, (meta, _h, extra) in entries.items()}
    rows.update({slot: f"{'local only':<17} {'':9}{describe(extra)}" for slot, extra in local.items()})
    present = [e for e in entries.values() if (e[0] >> 14) in (1, 2)]
    extras = [e[2] for e in present] + list(local.values())
    print(f"Node {ch.peer_id:08X} at {args.host}: {len(present)} synced finger(s), "
          f"{len(entries) - len(present)} deleted slot(s)"
          + (f", {len(local)} local-only finger(s)" if args.include_local else " (local-only slots not listed)"))
    print(f"  {sum(1 for e in extras if e['name'])} named, {sum(1 for e in extras if e['expires'])} guest(s)")
    for slot in sorted(rows):
        print(f"  slot {slot:4d}  {rows[slot]}")


# --------------------------------------------------------------------------- snapshots
def split_host(spec: str, default_port: int):
    host, _, port = spec.rpartition(":") if spec.count(":") == 1 else (spec, "", "")
    return (host, int(port)) if port else (spec, default_port)


def node_snapshot(ch: Channel, include_local: bool, templates: bool):
    """Slots of a live node: {slot: entry}; entry like a backup entry plus 'hash' (bytes).
    Synced slots carry the hash from the manifest; local-only slots (include_local) are fetched
    to compute it. With templates=True every template is fetched and kept in 'template'."""
    snap = {}
    for slot, (meta, digest, extra) in read_manifest(ch).items():
        if (meta >> 14) in (1, 2):
            snap[slot] = dict(extra, meta=meta, local=False, hash=digest)
    if include_local:
        local = read_local_list(ch)
        if local is None:
            return None
        for slot, extra in local.items():
            snap[slot] = dict(extra, meta=0, local=True, hash=None)
    for slot, entry in snap.items():
        if templates or entry["hash"] is None:
            result = fetch_template(ch, slot)
            if result is None:
                entry["error"] = "node could not read it"
                continue
            data = result[2]
            entry["hash"] = hashlib.sha256(data).digest()[:8]
            if templates:
                entry["template"] = base64.b64encode(data).decode()
    return {slot: e for slot, e in snap.items() if "error" not in e}


def file_snapshot(path: str, key: bytes):
    data = read_backup(path, key)
    snap = {}
    for slot_text, entry in data["slots"].items():
        tpl = base64.b64decode(entry["template"])
        snap[int(slot_text)] = dict(entry, hash=hashlib.sha256(tpl).digest()[:8])
    return snap


def load_snapshot(spec: str, args, key: bytes, templates: bool):
    """A backup file (if the path exists) or a node given as host[:port]. Returns (label, snapshot)."""
    if os.path.isfile(spec):
        return spec, file_snapshot(spec, key)
    host, port = split_host(spec, args.port)
    ch = connect(argparse.Namespace(host=host, port=port, timeout=args.timeout), key)
    snap = node_snapshot(ch, args.include_local, templates)
    finish(ch)
    if snap is None:
        sys.exit(f"{spec} refused the local slots: run allow_full_backup on that node first "
                 "(or leave out --include-local)")
    return f"{host}:{port}", snap


def same_extra(a: dict, b: dict) -> bool:
    return all(a.get(k, d) == b.get(k, d) for k, d in (("name", ""), ("expires", 0), ("uses", 0)))


def describe(extra: dict) -> str:
    text = f"  {extra['name']}" if extra.get("name") else ""
    if extra.get("expires"):
        text += f"  guest until {time.ctime(extra['expires'])}"
    if extra.get("uses"):
        text += f"  {extra['uses']} use(s) left"
    return text


def cmd_backup(args):
    key = load_key(args)
    if os.path.exists(args.out):
        sys.exit(f"{args.out} exists; refusing to overwrite")
    ch = connect(args, key)
    entries = read_manifest(ch)
    wanted = parse_slots(args.slots)
    todo = [(slot, False) for slot, (meta, _d, _e) in sorted(entries.items()) if (meta >> 14) in (1, 2)]
    if args.include_local:
        local = read_local_list(ch)
        if local is None:
            finish(ch)
            sys.exit("Node refused the local slots: run the fingerprint_grow_plus.allow_full_backup action "
                     "on the node first (or leave out --include-local)")
        todo += [(slot, True) for slot in sorted(local)]
    slots = {}
    for slot, is_local in sorted(todo):
        if wanted and slot not in wanted:
            continue
        result = fetch_template(ch, slot)
        if result is None:
            print(f"  slot {slot}: node could not read it", file=sys.stderr)
            continue
        meta, extra, data = result
        slots[str(slot)] = {"meta": meta, "expires": extra["expires"], "uses": extra["uses"],
                            "name": extra["name"], "local": is_local,
                            "template": base64.b64encode(data).decode()}
        print(f"  slot {slot}: {len(data)} bytes{' (local only)' if is_local else ''}{describe(extra)}")
    finish(ch)
    write_backup(args.out, key, {"node": f"{ch.peer_id:08X}", "host": args.host, "created": int(time.time()),
                                 "slots": slots})
    n_local = sum(1 for e in slots.values() if e["local"])
    print(f"Backup of {len(slots)} template(s) ({n_local} local only) written to {args.out}")


def cmd_inspect(args):
    data = read_backup(args.input, load_key(args))
    print(f"Backup of node {data['node']} ({data['host']}), created {time.ctime(data['created'])}")
    for slot, entry in sorted(data["slots"].items(), key=lambda kv: int(kv[0])):
        size = len(base64.b64decode(entry["template"]))
        kind = "local only" if entry.get("local") else f"version {entry['meta'] & 0x3FFF}"
        print(f"  slot {int(slot):4d}  {kind:12s}  {size} bytes{describe(entry)}")


def cmd_restore(args):
    key = load_key(args)
    data = read_backup(args.input, key)
    wanted = parse_slots(args.slots)
    ch = connect(args, key)
    read_manifest(ch)
    ok = 0
    for slot_text, entry in sorted(data["slots"].items(), key=lambda kv: int(kv[0])):
        slot = int(slot_text)
        if wanted and slot not in wanted:
            continue
        result = restore_slot(ch, slot, entry)
        if result == 0:
            ok += 1
            print(f"  slot {slot}: restored")
        elif result == 2:
            finish(ch)
            sys.exit("Node refused the restore: run the fingerprint_grow_plus.allow_restore action on the node first")
        else:
            print(f"  slot {slot}: FAILED on the node", file=sys.stderr)
    finish(ch)
    print(f"{ok} template(s) restored; synced slots are propagated to the peers, local-only slots stay on the node")


def cmd_compare(args):
    key = load_key(args)
    wanted = parse_slots(args.slots)
    label_a, a = load_snapshot(args.a, args, key, templates=False)
    label_b, b = load_snapshot(args.b, args, key, templates=False)
    if wanted:
        a = {k: v for k, v in a.items() if k in wanted}
        b = {k: v for k, v in b.items() if k in wanted}
    print(f"A = {label_a} ({len(a)} finger(s))\nB = {label_b} ({len(b)} finger(s))")
    if not args.include_local and not (os.path.isfile(args.a) and os.path.isfile(args.b)):
        print("  (local-only slots of nodes are not compared; use --include-local)")
    same = 0
    lines = []
    for slot in sorted(set(a) | set(b)):
        ea, eb = a.get(slot), b.get(slot)
        if eb is None:
            lines.append(f"  slot {slot:4d}  only in A{describe(ea)}")
        elif ea is None:
            lines.append(f"  slot {slot:4d}  only in B{describe(eb)}")
        elif ea["hash"] != eb["hash"]:
            lines.append(f"  slot {slot:4d}  different template (another finger or enrolled again)"
                         f"  A:{describe(ea) or ' -'}  B:{describe(eb) or ' -'}")
        elif not same_extra(ea, eb):
            lines.append(f"  slot {slot:4d}  same finger, other data  A:{describe(ea) or ' -'}  B:{describe(eb) or ' -'}")
        else:
            same += 1
    print("\n".join(lines))
    print(f"{same} identical, {len(lines)} different")
    sys.exit(1 if lines else 0)


def restore_slot(ch: Channel, slot: int, entry: dict) -> int:
    """Send one RESTORE and wait for its ACK. Returns the node's result (0 ok, 1 failed, 2 refused)."""
    tpl = base64.b64decode(entry["template"])
    ch.send(bytes([MSG_RESTORE]) + struct.pack(">H", slot) + pack_extra(entry) + struct.pack(">H", len(tpl)) + tpl)
    while True:
        msg = ch.recv()
        if msg[0] == MSG_RESTORE_ACK:
            return msg[3]


def cmd_copy(args):
    key = load_key(args)
    wanted = parse_slots(args.slots)
    label_src, src = load_snapshot(args.source, args, key, templates=True)
    if wanted:
        src = {k: v for k, v in src.items() if k in wanted}
    host, port = split_host(args.to, args.port)
    ch = connect(argparse.Namespace(host=host, port=port, timeout=args.timeout), key)
    target = {slot: (digest, extra) for slot, (meta, digest, extra) in read_manifest(ch).items()
              if (meta >> 14) in (1, 2)}
    target_local = read_local_list(ch)  # only with allow_full_backup on the target
    print(f"Copy {len(src)} finger(s) from {label_src} to {host}:{port}"
          + (" (dry run)" if args.dry_run else ""))
    hidden = target_local is None
    if hidden:
        target_local = {}
        print("  note: the target's local-only slots are not visible (allow_full_backup is not active there)")
    plan = []
    for slot, entry in sorted(src.items()):
        if slot in target and target[slot][0] == entry["hash"]:
            if same_extra(entry, target[slot][1]):
                print(f"  slot {slot:4d}  already there, skipped{describe(entry)}")
                continue
            # Same finger, other name / guest data: taking over the data replaces nothing.
            plan.append((slot, entry))
            print(f"  slot {slot:4d}  update name / guest data{describe(entry)}")
            continue
        occupied = slot in target or slot in target_local
        if occupied and not args.overwrite:
            print(f"  slot {slot:4d}  SKIPPED: the target holds another finger here (--overwrite replaces it)")
            continue
        plan.append((slot, entry))
        print(f"  slot {slot:4d}  {'replace' if occupied else 'copy'}{describe(entry)}")
    if args.dry_run or not plan:
        finish(ch)
        print(f"{len(plan)} slot(s) would be written" if args.dry_run else "Nothing to copy")
        return
    if hidden and not args.overwrite:
        finish(ch)
        sys.exit("Not copied: a local-only finger on the target could be replaced unseen. Run "
                 "fingerprint_grow_plus.allow_full_backup on the target as well (or pass --overwrite)")
    ok = 0
    for slot, entry in plan:
        result = restore_slot(ch, slot, entry)
        if result == 2:
            finish(ch)
            sys.exit("Target refused the restore: run the fingerprint_grow_plus.allow_restore action on it first")
        if result == 0:
            ok += 1
        else:
            print(f"  slot {slot}: FAILED on the target", file=sys.stderr)
    finish(ch)
    print(f"{ok} of {len(plan)} finger(s) copied; synced slots are propagated to the target's peers")


def cmd_log(args):
    key = load_key(args)
    rows = []
    for spec in args.host:
        host, _, port = spec.partition(":")
        node_args = argparse.Namespace(host=host, port=int(port) if port else args.port, timeout=args.timeout)
        try:
            ch = connect(node_args, key)
            read_manifest(ch)
            entries = read_access_log(ch)
            finish(ch)
        except (OSError, ConnectionError) as err:
            print(f"{spec}: not reachable ({err})", file=sys.stderr)
            continue
        if entries is None:
            print(f"{spec}: no access log (access_log: is not configured on this node)", file=sys.stderr)
            continue
        rows += [dict(e, node=spec) for e in entries]
    rows.sort(key=lambda e: e["time"])
    if args.csv:
        print("time,node,slot,name,result,confidence,flags")
    for e in rows:
        when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(e["time"]))
        flags = " ".join(n for bit, n in ACCESS_FLAGS if e["flags"] & bit)
        slot = "" if e["slot"] == 0xFFFF else str(e["slot"])
        if args.csv:
            name = e["name"].replace('"', '""')
            print(f'{when},{e["node"]},{slot},"{name}",{e["result"]},{e["confidence"]},{flags}')
            continue
        who = "unknown finger" if not slot else f"slot {slot:>4}" + (f"  {e['name']}" if e["name"] else "")
        approx = "" if e["clock"] else "~"  # node clock was not set: time estimated from its uptime
        print(f"{approx}{when}  {e['node']:<22} {who:<32} {e['result']}"
              + (f" ({flags})" if flags else ""))
    if not args.csv:
        print(f"{len(rows)} entr{'y' if len(rows) == 1 else 'ies'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p, needs_host=True):
        p.add_argument("--key")
        p.add_argument("--key-file")
        if needs_host:
            p.add_argument("--host", required=True)
            p.add_argument("--port", type=int, default=DEFAULT_PORT)
            p.add_argument("--timeout", type=float, default=30)

    sub.add_parser("genkey").set_defaults(func=cmd_genkey)
    p = sub.add_parser("status")
    common(p)
    p.add_argument("--include-local", "--all", action="store_true",
                   help="also list slots that are not synced (needs allow_full_backup on the node)")
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("compare")
    common(p, needs_host=False)
    p.add_argument("a", help="backup file or node (host[:port])")
    p.add_argument("b", help="backup file or node (host[:port])")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="default port for nodes")
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--slots", help="e.g. 0,2,10-19 (default: all)")
    p.add_argument("--include-local", "--all", action="store_true",
                   help="also compare a node's local-only slots (needs allow_full_backup there)")
    p.set_defaults(func=cmd_compare)
    p = sub.add_parser("copy")
    common(p, needs_host=False)
    p.add_argument("--from", dest="source", required=True, help="backup file or node (host[:port])")
    p.add_argument("--to", required=True, help="target node (host[:port]); run allow_restore there first")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="default port for nodes")
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--slots", help="e.g. 0,2,10-19 (default: all)")
    p.add_argument("--include-local", "--all", action="store_true",
                   help="also copy the source node's local-only slots (needs allow_full_backup there)")
    p.add_argument("--overwrite", action="store_true", help="replace other fingers on the target")
    p.add_argument("--dry-run", action="store_true", help="only show what would be copied")
    p.set_defaults(func=cmd_copy)
    p = sub.add_parser("backup")
    common(p)
    p.add_argument("--out", required=True, help="backup file to write, e.g. reader-garden.fgpb")
    p.add_argument("--slots", help="e.g. 0,2,10-19 (default: all)")
    p.add_argument("--include-local", "--all", action="store_true",
                   help="also save slots that are not synced (needs allow_full_backup on the node)")
    p.set_defaults(func=cmd_backup)
    p = sub.add_parser("restore")
    common(p)
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--slots", help="e.g. 0,2,10-19 (default: all in the file)")
    p.set_defaults(func=cmd_restore)
    p = sub.add_parser("log")
    p.add_argument("--key")
    p.add_argument("--key-file")
    p.add_argument("--host", action="append", required=True,
                   help="node as host or host:port; repeat for a merged log of several nodes")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--csv", action="store_true", help="comma-separated output")
    p.set_defaults(func=cmd_log)
    p = sub.add_parser("inspect")
    common(p, needs_host=False)
    p.add_argument("--in", dest="input", required=True)
    p.set_defaults(func=cmd_inspect)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
