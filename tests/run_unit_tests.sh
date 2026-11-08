#!/usr/bin/env bash
# Builds and runs the host unit tests (needs g++ and the OpenSSL headers, e.g. libssl-dev).
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -Wno-deprecated-declarations -DGROWPLUS_HOST_TEST \
  -I components/fingerprint_grow_plus \
  tests/host/test_crypto.cpp components/fingerprint_grow_plus/crypto.cpp \
  components/fingerprint_grow_plus/secure_channel.cpp -lcrypto -o "$build/test_crypto"
"$build/test_crypto"
python3 tests/test_interop.py "$build/test_crypto"
