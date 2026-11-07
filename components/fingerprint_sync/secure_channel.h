#pragma once

// Authenticated and encrypted record layer on top of any reliable byte stream.
//
// Wire format: every record is `u16 length (big endian) | body`.
//
// Handshake (plaintext bodies, see docs/protocol.md):
//   C -> S  C_HELLO  "FPS" 0x01 0x01 | client_id u32 | nonce_c[16]
//   S -> C  S_HELLO  "FPS" 0x01 0x02 | server_id u32 | nonce_s[16] | proof_s[32]
//   C -> S  C_AUTH   "FPS" 0x01 0x03 | proof_c[32]
//
// Keys: HKDF-SHA256(salt="fpsync-v1", ikm=PSK, info=nonce_c|nonce_s|client_id|server_id)
//       -> k_auth | k_enc_c2s | k_mac_c2s | k_enc_s2c | k_mac_s2c (5 x 32 bytes)
// Data records: seq u32 | ChaCha20(ciphertext) | HMAC-SHA256(k_mac, len|seq|ciphertext)[0..16]
// The sequence number must be exactly the expected one, which rejects replays and
// reordering inside a session; fresh nonces from both sides make every session unique.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::fingerprint_sync {

static constexpr size_t PSK_SIZE = 32;
static constexpr size_t MAX_RECORD_BODY = 2560;
static constexpr size_t MAX_MESSAGE_SIZE = MAX_RECORD_BODY - 4 - 16;

class SecureChannel {
 public:
  enum class Result : uint8_t { NONE, MESSAGE, ERROR };
  enum class State : uint8_t { IDLE, WAIT_S_HELLO, WAIT_C_HELLO, WAIT_C_AUTH, ESTABLISHED, FAILED };

  ~SecureChannel() { this->reset(); }

  /// Forget all session state and wipe keys.
  void reset();
  /// Start a client session; the C_HELLO record is appended to `out`.
  bool begin_client(const uint8_t *psk, uint32_t own_id, std::vector<uint8_t> &out);
  /// Start a server session that waits for C_HELLO.
  void begin_server(const uint8_t *psk, uint32_t own_id);
  /// Feed raw bytes received from the stream. Returns false if the record buffer overflows.
  bool feed(const uint8_t *data, size_t len);
  /// Process buffered records. Handshake replies are appended to `out`; a decrypted
  /// application message is returned through `message` together with Result::MESSAGE.
  Result poll(std::vector<uint8_t> &out, std::vector<uint8_t> &message);
  /// Encrypt one application message and append the record to `out`.
  bool seal(const uint8_t *msg, size_t len, std::vector<uint8_t> &out);

  bool established() const { return this->state_ == State::ESTABLISHED; }
  bool failed() const { return this->state_ == State::FAILED; }
  State state() const { return this->state_; }
  uint32_t peer_id() const { return this->peer_id_; }
  const char *error() const { return this->error_; }

  /// Test hook: use a fixed nonce instead of the RNG (host tests only).
  void set_fixed_nonce(const uint8_t *nonce) { this->fixed_nonce_ = nonce; }

 protected:
  Result fail_(const char *why);
  bool make_nonce_(uint8_t *nonce);
  void derive_keys_();
  void proof_(const char *label, bool include_s_proof, uint8_t out[32]);
  static void append_record_(std::vector<uint8_t> &out, const uint8_t *body, size_t len);
  Result handle_handshake_(const uint8_t *body, size_t len, std::vector<uint8_t> &out);
  Result open_(const uint8_t *body, size_t len, std::vector<uint8_t> &message);

  State state_{State::IDLE};
  bool is_client_{false};
  const uint8_t *psk_{nullptr};
  const uint8_t *fixed_nonce_{nullptr};
  uint32_t own_id_{0};
  uint32_t peer_id_{0};
  uint32_t client_id_{0};
  uint32_t server_id_{0};
  uint8_t nonce_c_[16]{};
  uint8_t nonce_s_[16]{};
  uint8_t c_hello_[25]{};
  uint8_t s_hello_[57]{};
  // k_auth | enc_c2s | mac_c2s | enc_s2c | mac_s2c
  uint8_t keys_[160]{};
  uint32_t tx_seq_{0};
  uint32_t rx_seq_{0};
  std::vector<uint8_t> rx_;
  const char *error_{""};
};

}  // namespace esphome::fingerprint_sync
