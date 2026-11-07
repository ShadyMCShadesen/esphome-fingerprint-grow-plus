#!/usr/bin/env python3
"""Check that tools/fpsync.py produces byte-identical records to the C++ SecureChannel.

Usage: python3 tests/test_interop.py /path/to/test_crypto   (built by tests/run_unit_tests.sh)
"""

import hashlib
import hmac
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import fpsync  # noqa: E402


def main(binary: str) -> int:
    out = subprocess.run([binary, "--transcript"], capture_output=True, text=True, check=True).stdout
    cpp = dict(line.split("=", 1) for line in out.strip().splitlines())

    psk = bytes(range(0x40, 0x60))
    nc = bytes(range(0x10, 0x20))
    ns = bytes(range(0x80, 0x90))
    cid, sid = 0x11223344, 0x55667788

    c_hello = fpsync.MAGIC + b"\x01" + struct.pack(">I", cid) + nc
    keys = fpsync.hkdf(b"fpsync-v1", psk, nc + ns + struct.pack(">II", cid, sid), 160)
    k_auth, enc_c2s, mac_c2s, enc_s2c, mac_s2c = (keys[i : i + 32] for i in range(0, 160, 32))
    s_prefix = fpsync.MAGIC + b"\x02" + struct.pack(">I", sid) + ns
    s_hello = s_prefix + hmac.new(k_auth, b"server" + c_hello + s_prefix, hashlib.sha256).digest()
    c_auth = fpsync.MAGIC + b"\x03" + hmac.new(k_auth, b"client" + c_hello + s_hello, hashlib.sha256).digest()

    def record(body: bytes) -> str:
        return (struct.pack(">H", len(body)) + body).hex()

    def sealed(enc: bytes, mac: bytes, seq: int, msg: bytes) -> str:
        s = struct.pack(">I", seq)
        ct = fpsync.chacha20_xor(enc, s + bytes(8), 0, msg)
        body_len = 4 + len(ct) + 16
        tag = hmac.new(mac, struct.pack(">H", body_len) + s + ct, hashlib.sha256).digest()[:16]
        return record(s + ct + tag)

    expected = {
        "c_hello": record(c_hello),
        "s_hello": record(s_hello),
        "c_auth": record(c_auth),
        "c_msg": sealed(enc_c2s, mac_c2s, 0, b"hello from client"),
        "s_msg": sealed(enc_s2c, mac_s2c, 0, b"answer from server"),
    }
    failures = 0
    for name, value in expected.items():
        if cpp.get(name) != value:
            print(f"FAIL {name}\n  C++:    {cpp.get(name)}\n  Python: {value}")
            failures += 1
    print("interop OK" if failures == 0 else f"{failures} interop failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
