#include "secure_channel.h"
#include "crypto.h"

#include <cstring>

namespace esphome::fingerprint_grow_plus {

// The last byte is the protocol version: nodes with different versions refuse each other.
static const uint8_t MAGIC[4] = {'F', 'P', 'S', 0x02};
static const uint8_t HKDF_SALT[] = {'f', 'p', 's', 'y', 'n', 'c', '-', 'v', '1'};
static constexpr uint8_t HS_C_HELLO = 0x01;
static constexpr uint8_t HS_S_HELLO = 0x02;
static constexpr uint8_t HS_C_AUTH = 0x03;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t C_HELLO_SIZE = 25;
static constexpr size_t S_HELLO_SIZE = 57;
static constexpr size_t C_AUTH_SIZE = 37;

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v >> 24);
  p[1] = (uint8_t) (v >> 16);
  p[2] = (uint8_t) (v >> 8);
  p[3] = (uint8_t) v;
}

static uint32_t get_u32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

void SecureChannel::reset() {
  crypto::wipe(this->keys_, sizeof(this->keys_));
  crypto::wipe(this->nonce_c_, sizeof(this->nonce_c_));
  crypto::wipe(this->nonce_s_, sizeof(this->nonce_s_));
  if (!this->rx_.empty())
    crypto::wipe(this->rx_.data(), this->rx_.size());
  this->rx_.clear();
  this->rx_.shrink_to_fit();
  this->state_ = State::IDLE;
  this->tx_seq_ = 0;
  this->rx_seq_ = 0;
  this->peer_id_ = 0;
  this->error_ = "";
}

SecureChannel::Result SecureChannel::fail_(const char *why) {
  this->error_ = why;
  this->state_ = State::FAILED;
  crypto::wipe(this->keys_, sizeof(this->keys_));
  return Result::ERROR;
}

bool SecureChannel::make_nonce_(uint8_t *nonce) {
  if (this->fixed_nonce_ != nullptr) {
    memcpy(nonce, this->fixed_nonce_, 16);
    return true;
  }
  return crypto::random(nonce, 16);
}

void SecureChannel::append_record_(std::vector<uint8_t> &out, const uint8_t *body, size_t len) {
  out.push_back((uint8_t) (len >> 8));
  out.push_back((uint8_t) len);
  out.insert(out.end(), body, body + len);
}

bool SecureChannel::begin_client(const uint8_t *psk, uint32_t own_id, std::vector<uint8_t> &out) {
  this->reset();
  this->is_client_ = true;
  this->psk_ = psk;
  this->own_id_ = own_id;
  this->client_id_ = own_id;
  if (!this->make_nonce_(this->nonce_c_)) {
    this->fail_("rng failure");
    return false;
  }
  memcpy(this->c_hello_, MAGIC, 4);
  this->c_hello_[4] = HS_C_HELLO;
  put_u32(this->c_hello_ + 5, own_id);
  memcpy(this->c_hello_ + 9, this->nonce_c_, 16);
  append_record_(out, this->c_hello_, C_HELLO_SIZE);
  this->state_ = State::WAIT_S_HELLO;
  return true;
}

void SecureChannel::begin_server(const uint8_t *psk, uint32_t own_id) {
  this->reset();
  this->is_client_ = false;
  this->psk_ = psk;
  this->own_id_ = own_id;
  this->server_id_ = own_id;
  this->state_ = State::WAIT_C_HELLO;
}

bool SecureChannel::feed(const uint8_t *data, size_t len) {
  // One complete record plus one read chunk of the next (callers read in chunks of <= 512 bytes).
  if (this->rx_.size() + len > MAX_RECORD_BODY + 2 + 512) {
    // A peer that keeps sending without completing records is broken or hostile.
    this->fail_("receive buffer overflow");
    return false;
  }
  this->rx_.insert(this->rx_.end(), data, data + len);
  return true;
}

void SecureChannel::derive_keys_() {
  uint8_t info[40];
  memcpy(info, this->nonce_c_, 16);
  memcpy(info + 16, this->nonce_s_, 16);
  put_u32(info + 32, this->client_id_);
  put_u32(info + 36, this->server_id_);
  crypto::hkdf_sha256(HKDF_SALT, sizeof(HKDF_SALT), this->psk_, PSK_SIZE, info, sizeof(info), this->keys_,
                      sizeof(this->keys_));
}

void SecureChannel::proof_(const char *label, bool include_s_proof, uint8_t out[32]) {
  crypto::Segment seg[] = {
      {reinterpret_cast<const uint8_t *>(label), strlen(label)},
      {this->c_hello_, C_HELLO_SIZE},
      {this->s_hello_, include_s_proof ? S_HELLO_SIZE : S_HELLO_SIZE - 32},
  };
  crypto::hmac_sha256(this->keys_, 32, seg, 3, out);
}

SecureChannel::Result SecureChannel::handle_handshake_(const uint8_t *body, size_t len, std::vector<uint8_t> &out) {
  if (len < 5 || memcmp(body, MAGIC, 4) != 0)
    return this->fail_("bad handshake magic or version");
  uint8_t type = body[4];

  if (this->state_ == State::WAIT_C_HELLO) {
    if (type != HS_C_HELLO || len != C_HELLO_SIZE)
      return this->fail_("expected client hello");
    memcpy(this->c_hello_, body, C_HELLO_SIZE);
    this->client_id_ = get_u32(body + 5);
    memcpy(this->nonce_c_, body + 9, 16);
    if (this->client_id_ == this->own_id_)
      return this->fail_("connection from own node id");
    if (!this->make_nonce_(this->nonce_s_))
      return this->fail_("rng failure");
    this->derive_keys_();
    memcpy(this->s_hello_, MAGIC, 4);
    this->s_hello_[4] = HS_S_HELLO;
    put_u32(this->s_hello_ + 5, this->own_id_);
    memcpy(this->s_hello_ + 9, this->nonce_s_, 16);
    this->proof_("server", false, this->s_hello_ + 25);
    append_record_(out, this->s_hello_, S_HELLO_SIZE);
    this->peer_id_ = this->client_id_;
    this->state_ = State::WAIT_C_AUTH;
    return Result::NONE;
  }

  if (this->state_ == State::WAIT_S_HELLO) {
    if (type != HS_S_HELLO || len != S_HELLO_SIZE)
      return this->fail_("expected server hello");
    memcpy(this->s_hello_, body, S_HELLO_SIZE);
    this->server_id_ = get_u32(body + 5);
    memcpy(this->nonce_s_, body + 9, 16);
    if (this->server_id_ == this->own_id_)
      return this->fail_("connected to own node id");
    this->derive_keys_();
    uint8_t expected[32];
    this->proof_("server", false, expected);
    if (!crypto::equal_ct(expected, body + 25, 32))
      return this->fail_("server authentication failed (wrong key?)");
    uint8_t auth[C_AUTH_SIZE];
    memcpy(auth, MAGIC, 4);
    auth[4] = HS_C_AUTH;
    this->proof_("client", true, auth + 5);
    append_record_(out, auth, C_AUTH_SIZE);
    this->peer_id_ = this->server_id_;
    this->state_ = State::ESTABLISHED;
    return Result::NONE;
  }

  if (this->state_ == State::WAIT_C_AUTH) {
    if (type != HS_C_AUTH || len != C_AUTH_SIZE)
      return this->fail_("expected client auth");
    uint8_t expected[32];
    this->proof_("client", true, expected);
    if (!crypto::equal_ct(expected, body + 5, 32))
      return this->fail_("client authentication failed (wrong key?)");
    this->state_ = State::ESTABLISHED;
    return Result::NONE;
  }

  return this->fail_("unexpected handshake record");
}

SecureChannel::Result SecureChannel::open_(const uint8_t *body, size_t len, std::vector<uint8_t> &message) {
  if (len < 4 + TAG_SIZE)
    return this->fail_("short record");
  const uint8_t *enc_key = this->is_client_ ? this->keys_ + 96 : this->keys_ + 32;
  const uint8_t *mac_key = this->is_client_ ? this->keys_ + 128 : this->keys_ + 64;
  uint32_t seq = get_u32(body);
  if (seq != this->rx_seq_)
    return this->fail_("unexpected sequence number (replay?)");
  size_t ct_len = len - 4 - TAG_SIZE;
  uint8_t len_be[2] = {(uint8_t) (len >> 8), (uint8_t) len};
  uint8_t tag[32];
  crypto::Segment seg[] = {{len_be, 2}, {body, 4 + ct_len}};
  crypto::hmac_sha256(mac_key, 32, seg, 2, tag);
  if (!crypto::equal_ct(tag, body + 4 + ct_len, TAG_SIZE))
    return this->fail_("record authentication failed");
  message.assign(body + 4, body + 4 + ct_len);
  uint8_t nonce[12] = {0};
  memcpy(nonce, body, 4);
  crypto::chacha20_xor(enc_key, nonce, 0, message.data(), message.size());
  this->rx_seq_++;
  return Result::MESSAGE;
}

SecureChannel::Result SecureChannel::poll(std::vector<uint8_t> &out, std::vector<uint8_t> &message) {
  // Handshake records are consumed internally; keep going so that an application record that
  // arrived in the same read (e.g. right behind C_AUTH) is returned without waiting for more data.
  while (true) {
    if (this->state_ == State::FAILED)
      return Result::ERROR;
    if (this->state_ == State::IDLE || this->rx_.size() < 2)
      return Result::NONE;
    size_t len = ((size_t) this->rx_[0] << 8) | this->rx_[1];
    if (len == 0 || len > MAX_RECORD_BODY)
      return this->fail_("invalid record length");
    if (this->rx_.size() < 2 + len)
      return Result::NONE;

    Result res;
    if (this->state_ == State::ESTABLISHED) {
      res = this->open_(this->rx_.data() + 2, len, message);
    } else {
      res = this->handle_handshake_(this->rx_.data() + 2, len, out);
    }
    crypto::wipe(this->rx_.data(), 2 + len);
    this->rx_.erase(this->rx_.begin(), this->rx_.begin() + 2 + len);
    if (res != Result::NONE)
      return res;
  }
}

bool SecureChannel::seal(const uint8_t *msg, size_t len, std::vector<uint8_t> &out) {
  if (this->state_ != State::ESTABLISHED || len > MAX_MESSAGE_SIZE)
    return false;
  const uint8_t *enc_key = this->is_client_ ? this->keys_ + 32 : this->keys_ + 96;
  const uint8_t *mac_key = this->is_client_ ? this->keys_ + 64 : this->keys_ + 128;
  size_t body_len = 4 + len + TAG_SIZE;
  size_t start = out.size();
  out.resize(start + 2 + body_len);
  uint8_t *p = out.data() + start;
  p[0] = (uint8_t) (body_len >> 8);
  p[1] = (uint8_t) body_len;
  put_u32(p + 2, this->tx_seq_);
  memcpy(p + 6, msg, len);
  uint8_t nonce[12] = {0};
  memcpy(nonce, p + 2, 4);
  crypto::chacha20_xor(enc_key, nonce, 0, p + 6, len);
  uint8_t tag[32];
  crypto::Segment seg[] = {{p, 2}, {p + 2, 4 + len}};
  crypto::hmac_sha256(mac_key, 32, seg, 2, tag);
  memcpy(p + 6 + len, tag, TAG_SIZE);
  this->tx_seq_++;
  return true;
}

}  // namespace esphome::fingerprint_grow_plus
