#!/usr/bin/env python3
"""Minimal GROW R503 simulator for host-platform tests.

Creates a pseudo terminal, links its slave side to --link and answers the R503 packet
protocol (subset used by ESPHome's fingerprint_grow and by fingerprint_sync).

Templates are synthetic, never real biometric data: 768 bytes whose first 16 bytes are a
"finger identity" (used by Match/Search), the rest is per-enrollment noise. That models
"same finger, different template bytes" for convergence tests.

Control: lines appended to --control are executed:
    touch <finger-name>    finger is present for exactly one GetImage call
The state is dumped to --state (JSON) after every change.
"""

import argparse
import hashlib
import json
import os
import random
import select
import sys
import time
import tty

CAPACITY = 200
TEMPLATE_SIZE = 768
PACKET_CODE = 2  # 128 byte data packets


def finger_id(name: str) -> bytes:
    # The synthetic "finger identity" is the readable name, so every simulator can tell
    # which finger a template received from another node belongs to.
    return name.encode()[:16].ljust(16, b"\0")


def finger_name(template: bytes) -> str:
    return template[:16].rstrip(b"\0").decode(errors="replace") or "?"


def make_template(name: str, noise_seed: str) -> bytes:
    noise = b""
    counter = 0
    while len(noise) < TEMPLATE_SIZE - 16:
        noise += hashlib.sha256(f"{noise_seed}:{counter}".encode()).digest()
        counter += 1
    return finger_id(name) + noise[: TEMPLATE_SIZE - 16]


class Sim:
    def __init__(self, args):
        self.args = args
        self.templates: dict[int, bytes] = {}
        self.names: dict[bytes, str] = {}
        self.buffers: dict[int, bytes] = {}
        self.notepad = [bytes(32) for _ in range(16)]
        self.touch_queue: list[str] = []
        self.image: str | None = None
        self.control_pos = 0
        self.rx = bytearray()
        self.downchar: tuple[int, bytearray] | None = None
        self.scan_bufs: list[int] = []
        self.stats = {"store": 0, "delete": 0, "upchar": 0, "downchar": 0, "match": 0}
        if args.seed:
            with open(args.seed, encoding="utf-8") as f:
                seed = json.load(f)
            for slot, spec in seed.get("slots", {}).items():
                self.add_finger(int(slot), spec["finger"], spec.get("noise", f"{args.name}:{slot}"))
            if "notepad" in seed:
                raw = bytes.fromhex(seed["notepad"])
                self.notepad = [raw[i * 32 : (i + 1) * 32] for i in range(16)]

    def add_finger(self, slot, name, noise):
        tpl = make_template(name, noise)
        self.templates[slot] = tpl
        self.names[tpl[:16]] = name

    # ------------------------------------------------------------------ io helpers
    def dump(self):
        state = {
            "slots": {
                str(slot): {
                    "finger": finger_name(tpl),
                    "sha": hashlib.sha256(tpl).hexdigest()[:16],
                }
                for slot, tpl in sorted(self.templates.items())
            },
            "stats": self.stats,
        }
        tmp = self.args.state + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=1, sort_keys=True)
        os.replace(tmp, self.args.state)

    def send(self, pid: int, payload: bytes):
        length = len(payload) + 2
        body = bytes([pid, length >> 8, length & 0xFF]) + payload
        csum = sum(body) & 0xFFFF
        pkt = b"\xef\x01\xff\xff\xff\xff" + body + bytes([csum >> 8, csum & 0xFF])
        os.write(self.master, pkt)

    def ack(self, code: int, extra: bytes = b""):
        self.send(0x07, bytes([code]) + extra)

    def poll_control(self):
        try:
            with open(self.args.control, encoding="utf-8") as f:
                f.seek(self.control_pos)
                lines = f.readlines()
                self.control_pos = f.tell()
        except FileNotFoundError:
            return
        for line in lines:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "touch":
                self.touch_queue.append(parts[1])

    # ------------------------------------------------------------------ protocol
    def handle_packet(self, pid: int, payload: bytes):
        if self.downchar is not None and pid in (0x02, 0x08):
            buf, data = self.downchar
            data.extend(payload)
            if pid == 0x08:
                self.buffers[buf] = bytes(data)
                self.downchar = None
                self.stats["downchar"] += 1
            return
        if pid != 0x01 or not payload:
            return
        cmd, args = payload[0], payload[1:]
        changed = False
        if cmd == 0x13:  # VerifyPassword
            self.ack(0x00)
        elif cmd == 0x0F:  # ReadSysPara
            params = bytes(
                [0, 0, 0, 9, CAPACITY >> 8, CAPACITY & 0xFF, 0, 3, 0xFF, 0xFF, 0xFF, 0xFF, 0, PACKET_CODE, 0, 6]
            )
            self.ack(0x00, params)
        elif cmd == 0x1D:  # TemplateNum
            n = len(self.templates)
            self.ack(0x00, bytes([n >> 8, n & 0xFF]))
        elif cmd == 0x1F:  # ReadIndexTable
            page = args[0]
            table = bytearray(32)
            for slot in self.templates:
                if page * 256 <= slot < (page + 1) * 256:
                    rel = slot - page * 256
                    table[rel // 8] |= 1 << (rel % 8)
            self.ack(0x00, bytes(table))
        elif cmd == 0x01:  # GetImage
            if self.touch_queue:
                self.image = self.touch_queue.pop(0)
                self.ack(0x00)
            else:
                self.image = None
                self.ack(0x02)
        elif cmd == 0x02:  # Img2Tz
            if self.image is None:
                self.ack(0x15)
            else:
                self.buffers[args[0]] = make_template(self.image, f"scan:{random.random()}")
                self.scan_bufs = [1] if args[0] == 1 else self.scan_bufs + [args[0]]
                self.names.setdefault(finger_id(self.image), self.image)
                self.ack(0x00)
        elif cmd == 0x05:  # RegModel
            ids = {self.buffers.get(b, b"")[:16] for b in self.scan_bufs}
            if len(ids) == 1 and self.scan_bufs:
                tpl = self.buffers[1][:16] + make_template("x", f"model:{random.random()}")[16:]
                self.buffers[1] = self.buffers[2] = tpl
                self.ack(0x00)
            else:
                self.ack(0x0A)
        elif cmd == 0x04:  # Search
            want = self.buffers.get(args[0], b"")[:16]
            for slot, tpl in sorted(self.templates.items()):
                if tpl[:16] == want:
                    self.ack(0x00, bytes([slot >> 8, slot & 0xFF, 0, 200]))
                    break
            else:
                self.ack(0x09)
        elif cmd == 0x06:  # Store
            buf, slot = args[0], (args[1] << 8) | args[2]
            if slot >= CAPACITY or buf not in self.buffers:
                self.ack(0x0B)
            else:
                self.templates[slot] = self.buffers[buf]
                self.stats["store"] += 1
                changed = True
                self.ack(0x00)
        elif cmd == 0x07:  # LoadChar
            buf, slot = args[0], (args[1] << 8) | args[2]
            if slot in self.templates:
                self.buffers[buf] = self.templates[slot]
                self.ack(0x00)
            else:
                self.ack(0x0C)
        elif cmd == 0x08:  # UpChar
            data = self.buffers.get(args[0])
            if data is None:
                self.ack(0x0D)
            else:
                self.ack(0x00)
                chunks = [data[i : i + 128] for i in range(0, len(data), 128)]
                for i, chunk in enumerate(chunks):
                    self.send(0x08 if i == len(chunks) - 1 else 0x02, chunk)
                self.stats["upchar"] += 1
        elif cmd == 0x09:  # DownChar
            self.downchar = (args[0], bytearray())
            self.ack(0x00)
        elif cmd == 0x0C:  # DeletChar
            slot, count = (args[0] << 8) | args[1], (args[2] << 8) | args[3]
            for s in range(slot, slot + count):
                self.templates.pop(s, None)
            self.stats["delete"] += 1
            changed = True
            self.ack(0x00)
        elif cmd == 0x0D:  # Empty
            self.templates.clear()
            changed = True
            self.ack(0x00)
        elif cmd == 0x03:  # Match
            self.stats["match"] += 1
            a, b = self.buffers.get(1, b"x"), self.buffers.get(2, b"y")
            if a[:16] == b[:16]:
                self.ack(0x00, bytes([0, 150]))
            else:
                self.ack(0x08, bytes([0, 0]))
        elif cmd == 0x19:  # ReadNotepad
            self.ack(0x00, self.notepad[args[0]])
        elif cmd == 0x18:  # WriteNotepad
            self.notepad[args[0]] = bytes(args[1:33])
            self.ack(0x00)
        elif cmd in (0x35, 0x50, 0x51):  # LED control
            self.ack(0x00)
        elif cmd == 0x12:  # SetPwd must never be used
            print("SIM ERROR: SetPwd received", file=sys.stderr, flush=True)
            self.ack(0x01)
        else:
            self.ack(0x01)
        if changed:
            self.dump()

    def parse(self):
        while True:
            start = self.rx.find(b"\xef\x01")
            if start < 0:
                self.rx.clear()
                return
            del self.rx[:start]
            if len(self.rx) < 9:
                return
            length = (self.rx[7] << 8) | self.rx[8]
            if len(self.rx) < 9 + length:
                return
            pid = self.rx[6]
            payload = bytes(self.rx[9 : 9 + length - 2])
            csum = (self.rx[9 + length - 2] << 8) | self.rx[9 + length - 1]
            if (sum(self.rx[6 : 9 + length - 2]) & 0xFFFF) != csum:
                print("SIM: bad checksum", file=sys.stderr, flush=True)
            del self.rx[: 9 + length]
            self.handle_packet(pid, payload)

    def run(self):
        self.master, slave = os.openpty()
        tty.setraw(slave)
        slave_name = os.ttyname(slave)
        if os.path.lexists(self.args.link):
            os.unlink(self.args.link)
        os.symlink(slave_name, self.args.link)
        self.dump()
        print(f"SIM {self.args.name}: ready on {self.args.link}", flush=True)
        while True:
            ready, _, _ = select.select([self.master], [], [], 0.05)
            self.poll_control()
            if ready:
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    time.sleep(0.05)
                    continue
                self.rx.extend(data)
                self.parse()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--link", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--seed")
    Sim(parser.parse_args()).run()


if __name__ == "__main__":
    main()
