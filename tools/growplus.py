#!/usr/bin/env python3
"""growplus - backup, restore and inspection tool for fingerprint_grow_plus nodes.

Standard library only. Speaks the same authenticated, encrypted protocol as the nodes
(see docs/protocol.md) and therefore needs the shared key and a node with a `sync:` block.
By default only synced slots are saved; --include-local adds the local-only slots.

Commands
  genkey                                   print a new random key for secrets.yaml
  status  --host H                         list the synced slots of a node (no template data)
  backup  --host H --out FILE              download all synced templates into an encrypted file;
          [--include-local]                --include-local also saves the slots that are not
                                           synced (run allow_full_backup on the node first)
  restore --host H --in FILE [--slots ..]  upload templates (node must run allow_restore first)
  inspect --in FILE                        list the content of a backup file

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
MAGIC = b"FGP\x05"  # last byte = protocol version
TAG_SIZE = 16
MAX_RECORD_BODY = 2560
(MSG_MANIFEST, MSG_TEMPLATE_REQ, MSG_TEMPLATE, MSG_TEMPLATE_NAK, MSG_DONE, MSG_RESTORE, MSG_RESTORE_ACK,
 MSG_LOCAL_LIST_REQ, MSG_LOCAL_LIST) = range(1, 10)
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
    ch.send(bytes([MSG_MANIFEST, 0, 1]) + struct.pack(">HH", 0, 0))
    return ch


def read_manifest(ch: Channel):
    entries = {}
    while True:
        msg = ch.recv()
        if msg[0] == MSG_MANIFEST:
            _, flags = msg[1], msg[2]
            count = struct.unpack(">H", msg[5:7])[0]
            pos = 7
            for _ in range(count):
                slot, meta = struct.unpack(">HH", msg[pos : pos + 4])
                expires = struct.unpack(">I", msg[pos + 12 : pos + 16])[0]
                entries[slot] = (meta, msg[pos + 4 : pos + 12], expires)
                pos += 16
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
            slot, expires = struct.unpack(">HI", msg[pos : pos + 6])
            entries[slot] = expires
            pos += 6
        if flags & 1:
            return entries


def fetch_template(ch: Channel, slot: int):
    """Returns (meta, expires, data) or None if the node could not read the slot."""
    ch.send(bytes([MSG_TEMPLATE_REQ]) + struct.pack(">H", slot))
    while True:
        msg = ch.recv()
        if msg[0] in (MSG_TEMPLATE, MSG_TEMPLATE_NAK):
            break
    if msg[0] == MSG_TEMPLATE_NAK:
        return None
    r_slot, r_meta = struct.unpack(">HH", msg[1:5])
    r_hash = msg[5:13]
    r_expires = struct.unpack(">I", msg[13:17])[0]
    length = struct.unpack(">H", msg[17:19])[0]
    data = msg[19 : 19 + length]
    if r_slot != slot or hashlib.sha256(data).digest()[:8] != r_hash:
        sys.exit(f"slot {slot}: checksum mismatch, backup aborted")
    return r_meta, r_expires, data


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
    finish(ch)
    print(f"Node {ch.peer_id:08X} at {args.host}: {len(entries)} managed slot entries")
    for slot, (meta, _h, expires) in sorted(entries.items()):
        until = f"  guest until {time.ctime(expires)}" if expires else ""
        print(f"  slot {slot:4d}  {STATE_NAMES[meta >> 14]:<17} version {meta & 0x3FFF}{until}")


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
        meta, expires, data = result
        slots[str(slot)] = {"meta": meta, "expires": expires, "local": is_local,
                            "template": base64.b64encode(data).decode()}
        print(f"  slot {slot}: {len(data)} bytes{' (local only)' if is_local else ''}")
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
        print(f"  slot {int(slot):4d}  {kind:12s}  {size} bytes")


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
        tpl = base64.b64decode(entry["template"])
        ch.send(bytes([MSG_RESTORE]) + struct.pack(">HIH", slot, entry.get("expires", 0), len(tpl)) + tpl)
        while True:
            msg = ch.recv()
            if msg[0] == MSG_RESTORE_ACK:
                break
        result = msg[3]
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
    p.set_defaults(func=cmd_status)
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
    p = sub.add_parser("inspect")
    common(p, needs_host=False)
    p.add_argument("--in", dest="input", required=True)
    p.set_defaults(func=cmd_inspect)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
