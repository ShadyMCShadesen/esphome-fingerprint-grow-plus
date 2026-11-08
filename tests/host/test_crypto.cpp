// Host unit test for the portable crypto and the SecureChannel record layer.
//
// Build & run: tests/run_unit_tests.sh
//
// Checks:
//  1. ChaCha20 against OpenSSL's ChaCha20 for many lengths/counters.
//  2. HKDF-SHA256 against RFC 5869 test case 1.
//  3. A full handshake + messages between a client and a server SecureChannel.
//  4. Tampering, replay and wrong keys are rejected.
//  5. Prints a deterministic transcript (fixed nonces) that tests/test_interop.py
//     recomputes with the Python implementation in tools/fpsync.py.

#include "crypto.h"
#include "secure_channel.h"

#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::fingerprint_grow_plus;

static int failures = 0;
#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL: %s\n", msg); \
      failures++; \
    } \
  } while (0)

static std::string hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

static void test_chacha() {
  uint8_t key[32], nonce[12];
  for (int i = 0; i < 32; i++)
    key[i] = (uint8_t) (i * 7 + 1);
  for (int i = 0; i < 12; i++)
    nonce[i] = (uint8_t) (0xA0 + i);
  for (size_t len : {1, 63, 64, 65, 200, 1000}) {
    for (uint32_t counter : {0u, 1u, 7u}) {
      std::vector<uint8_t> a(len), b(len);
      for (size_t i = 0; i < len; i++)
        a[i] = b[i] = (uint8_t) (i * 13);
      crypto::chacha20_xor(key, nonce, counter, a.data(), len);
      uint8_t iv[16];
      iv[0] = (uint8_t) counter;
      iv[1] = (uint8_t) (counter >> 8);
      iv[2] = (uint8_t) (counter >> 16);
      iv[3] = (uint8_t) (counter >> 24);
      memcpy(iv + 4, nonce, 12);
      EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
      EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr, key, iv);
      int out_len = 0;
      std::vector<uint8_t> ref(len);
      EVP_EncryptUpdate(ctx, ref.data(), &out_len, b.data(), (int) len);
      EVP_CIPHER_CTX_free(ctx);
      CHECK(a == ref, "chacha20 differs from OpenSSL");
    }
  }
}

static void test_hkdf() {
  uint8_t ikm[22], salt[13], info[10], okm[42];
  memset(ikm, 0x0b, sizeof(ikm));
  for (int i = 0; i < 13; i++)
    salt[i] = (uint8_t) i;
  for (int i = 0; i < 10; i++)
    info[i] = (uint8_t) (0xf0 + i);
  crypto::hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm));
  CHECK(hex(okm, sizeof(okm)) ==
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        "HKDF RFC 5869 test case 1");
}

// Move all bytes from `wire` into `ch` and collect messages.
static SecureChannel::Result pump(SecureChannel &ch, std::vector<uint8_t> &wire, std::vector<uint8_t> &reply,
                                  std::vector<std::vector<uint8_t>> &messages) {
  ch.feed(wire.data(), wire.size());
  wire.clear();
  std::vector<uint8_t> msg;
  while (true) {
    auto r = ch.poll(reply, msg);
    if (r == SecureChannel::Result::NONE)
      return r;
    if (r == SecureChannel::Result::ERROR)
      return r;
    messages.push_back(msg);
  }
}

static void test_channel(bool print_transcript) {
  uint8_t psk[32];
  for (int i = 0; i < 32; i++)
    psk[i] = (uint8_t) (0x40 + i);
  uint8_t nc[16], ns[16];
  for (int i = 0; i < 16; i++) {
    nc[i] = (uint8_t) (0x10 + i);
    ns[i] = (uint8_t) (0x80 + i);
  }
  SecureChannel client, server;
  client.set_fixed_nonce(nc);
  server.set_fixed_nonce(ns);
  std::vector<uint8_t> c2s, s2c;
  std::vector<std::vector<uint8_t>> got;
  client.begin_client(psk, 0x11223344, c2s);
  server.begin_server(psk, 0x55667788);
  std::string transcript = "c_hello=" + hex(c2s.data(), c2s.size()) + "\n";
  CHECK(pump(server, c2s, s2c, got) == SecureChannel::Result::NONE, "server hello");
  transcript += "s_hello=" + hex(s2c.data(), s2c.size()) + "\n";
  CHECK(pump(client, s2c, c2s, got) == SecureChannel::Result::NONE, "client auth");
  CHECK(client.established(), "client established");
  transcript += "c_auth=" + hex(c2s.data(), c2s.size()) + "\n";
  const char *hello = "hello from client";
  client.seal(reinterpret_cast<const uint8_t *>(hello), strlen(hello), c2s);
  transcript += "c_msg=" + hex(c2s.data() + 39, c2s.size() - 39) + "\n";
  CHECK(pump(server, c2s, s2c, got) == SecureChannel::Result::NONE, "server receives");
  CHECK(server.established(), "server established");
  CHECK(got.size() == 1 && std::string(got[0].begin(), got[0].end()) == hello, "message content");
  const char *answer = "answer from server";
  server.seal(reinterpret_cast<const uint8_t *>(answer), strlen(answer), s2c);
  transcript += "s_msg=" + hex(s2c.data(), s2c.size()) + "\n";
  std::vector<uint8_t> copy = s2c;  // keep for the replay test
  got.clear();
  pump(client, s2c, c2s, got);
  CHECK(got.size() == 1 && std::string(got[0].begin(), got[0].end()) == answer, "answer content");

  // Replay of the same record must fail.
  got.clear();
  CHECK(pump(client, copy, c2s, got) == SecureChannel::Result::ERROR, "replay rejected");

  if (print_transcript)
    std::printf("%s", transcript.c_str());
}

static void test_tamper_and_wrong_key() {
  uint8_t psk[32] = {1}, other[32] = {2};
  // Wrong key: client must reject the server.
  {
    SecureChannel client, server;
    std::vector<uint8_t> c2s, s2c;
    std::vector<std::vector<uint8_t>> got;
    client.begin_client(psk, 1, c2s);
    server.begin_server(other, 2);
    pump(server, c2s, s2c, got);
    CHECK(pump(client, s2c, c2s, got) == SecureChannel::Result::ERROR, "wrong key rejected");
  }
  // Tampered ciphertext must be rejected.
  {
    SecureChannel client, server;
    std::vector<uint8_t> c2s, s2c;
    std::vector<std::vector<uint8_t>> got;
    client.begin_client(psk, 1, c2s);
    server.begin_server(psk, 2);
    pump(server, c2s, s2c, got);
    pump(client, s2c, c2s, got);
    pump(server, c2s, s2c, got);
    const uint8_t msg[] = {1, 2, 3, 4};
    client.seal(msg, sizeof(msg), c2s);
    c2s[8] ^= 0x01;
    CHECK(pump(server, c2s, s2c, got) == SecureChannel::Result::ERROR, "tampered record rejected");
  }
  // Same node id on both sides (a node connecting to itself) is refused.
  {
    SecureChannel client, server;
    std::vector<uint8_t> c2s, s2c;
    std::vector<std::vector<uint8_t>> got;
    client.begin_client(psk, 7, c2s);
    server.begin_server(psk, 7);
    CHECK(pump(server, c2s, s2c, got) == SecureChannel::Result::ERROR, "self connection refused");
  }
}

int main(int argc, char **argv) {
  bool transcript = argc > 1 && std::string(argv[1]) == "--transcript";
  test_chacha();
  test_hkdf();
  test_channel(transcript);
  test_tamper_and_wrong_key();
  if (!transcript)
    std::printf(failures == 0 ? "all crypto tests passed\n" : "%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
