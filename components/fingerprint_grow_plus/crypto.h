#pragma once

// Portable cryptographic primitives used by the sync channel.
//
// - ChaCha20 (RFC 8439) is implemented here because ESPHome offers no cipher that
//   is available on ESP32 (Arduino and ESP-IDF) as well as ESP8266.
// - HMAC-SHA256 is delegated to ESPHome's hmac_sha256 component (mbedTLS/PSA on
//   ESP32, BearSSL on ESP8266). Host unit tests define GROWPLUS_HOST_TEST and use
//   OpenSSL instead.

#include <cstddef>
#include <cstdint>

namespace esphome::fingerprint_grow_plus::crypto {

static constexpr size_t KEY_SIZE = 32;
static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t HMAC_SIZE = 32;

/// XOR `len` bytes of `data` in place with the ChaCha20 keystream (RFC 8439, section 2.4).
void chacha20_xor(const uint8_t key[KEY_SIZE], const uint8_t nonce[NONCE_SIZE], uint32_t counter, uint8_t *data,
                  size_t len);

/// One input segment for hmac_sha256().
struct Segment {
  const uint8_t *data;
  size_t len;
};

/// HMAC-SHA256 over the concatenation of all segments.
void hmac_sha256(const uint8_t *key, size_t key_len, const Segment *segments, size_t segment_count,
                 uint8_t out[HMAC_SIZE]);

/// HKDF-SHA256 (RFC 5869), extract and expand. `out_len` must be <= 255 * 32.
void hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *info,
                 size_t info_len, uint8_t *out, size_t out_len);

/// Plain SHA-256.
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/// Constant time comparison.
bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

/// Overwrite memory in a way the compiler may not optimise away.
void wipe(void *data, size_t len);

/// Cryptographically secure random bytes from the platform RNG.
bool random(uint8_t *data, size_t len);

}  // namespace esphome::fingerprint_grow_plus::crypto
