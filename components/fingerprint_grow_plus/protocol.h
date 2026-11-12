#pragma once

// Sync protocol messages (carried inside SecureChannel records). See docs/protocol.md.
// All integers are big endian.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome::fingerprint_grow_plus {

static constexpr uint8_t PROTOCOL_VERSION = 6;

enum MsgType : uint8_t {
  /// u8 page | u8 flags(bit0 = last page) | u16 capacity | u16 epoch | u16 count | count x Entry
  /// Entry = u16 slot | u16 meta | u8 hash[8] | Extra
  /// Extra = u32 expires (unix time, 0 = never) | u8 uses_left (0 = unlimited) | u8 name_len | name
  MSG_MANIFEST = 0x01,
  /// u16 slot
  MSG_TEMPLATE_REQ = 0x02,
  /// u16 slot | u16 meta | u8 hash[8] | Extra | u16 len | data[len]
  MSG_TEMPLATE = 0x03,
  /// u16 slot | u8 reason
  MSG_TEMPLATE_NAK = 0x04,
  /// no payload: "I need nothing more from you"
  MSG_DONE = 0x05,
  /// u16 slot | Extra | u16 len | data[len]  (only accepted while a restore window is open)
  MSG_RESTORE = 0x06,
  /// u16 slot | u8 result (0 = ok)
  MSG_RESTORE_ACK = 0x07,
  /// no payload: list the local-only slots (outside the synced range) for a full backup.
  /// Only answered while a full-backup window is open; peers never send it.
  MSG_LOCAL_LIST_REQ = 0x08,
  /// u8 page | u8 flags(bit0 = last page, bit1 = refused) | u16 count | count x (u16 slot | Extra)
  MSG_LOCAL_LIST = 0x09,
  /// u32 seconds: lock this node for that long (lockout shared by a peer; never forwarded)
  MSG_LOCK = 0x0A,
};
static constexpr size_t LOCAL_LIST_ENTRIES_PER_PAGE = 48;

static constexpr size_t HASH_SIZE = 8;
/// Longest slot name in bytes (UTF-8).
static constexpr size_t MAX_NAME_LEN = 24;
/// Worst case: 48 entries x (2 + 2 + 8 + 4 + 1 + 1 + 24) bytes stays below the record limit.
static constexpr size_t MANIFEST_ENTRIES_PER_PAGE = 48;

/// Packed per-slot metadata: bits 15..14 = state, bits 13..0 = version (Lamport clock).
enum SlotState : uint8_t {
  SLOT_NONE = 0,
  SLOT_PRESENT = 1,
  /// Present and explicitly authorised to replace a different finger in this slot on peers.
  SLOT_PRESENT_FORCED = 2,
  SLOT_TOMBSTONE = 3,
};
static constexpr uint16_t MAX_VERSION = 0x3FFF;

inline uint16_t meta_pack(SlotState state, uint16_t version) {
  return (uint16_t) (((uint16_t) state << 14) | (version & MAX_VERSION));
}
inline SlotState meta_state(uint16_t meta) { return (SlotState) (meta >> 14); }
inline uint16_t meta_version(uint16_t meta) { return meta & MAX_VERSION; }
inline bool meta_present(uint16_t meta) {
  SlotState s = meta_state(meta);
  return s == SLOT_PRESENT || s == SLOT_PRESENT_FORCED;
}
inline bool meta_forced(uint16_t meta) { return meta_state(meta) == SLOT_PRESENT_FORCED; }

class MsgWriter {
 public:
  explicit MsgWriter(std::vector<uint8_t> &buf) : buf_(buf) { buf_.clear(); }
  void u8(uint8_t v) { this->buf_.push_back(v); }
  void u16(uint16_t v) {
    this->buf_.push_back((uint8_t) (v >> 8));
    this->buf_.push_back((uint8_t) v);
  }
  void u32(uint32_t v) {
    this->u16((uint16_t) (v >> 16));
    this->u16((uint16_t) v);
  }
  void bytes(const uint8_t *data, size_t len) { this->buf_.insert(this->buf_.end(), data, data + len); }

 protected:
  std::vector<uint8_t> &buf_;
};

class MsgReader {
 public:
  MsgReader(const uint8_t *data, size_t len) : data_(data), len_(len) {}
  bool u8(uint8_t &v) {
    if (this->pos_ + 1 > this->len_)
      return this->bad_();
    v = this->data_[this->pos_++];
    return true;
  }
  bool u16(uint16_t &v) {
    if (this->pos_ + 2 > this->len_)
      return this->bad_();
    v = ((uint16_t) this->data_[this->pos_] << 8) | this->data_[this->pos_ + 1];
    this->pos_ += 2;
    return true;
  }
  bool u32(uint32_t &v) {
    uint16_t hi, lo;
    if (!this->u16(hi) || !this->u16(lo))
      return false;
    v = ((uint32_t) hi << 16) | lo;
    return true;
  }
  bool bytes(uint8_t *out, size_t len) {
    if (this->pos_ + len > this->len_)
      return this->bad_();
    memcpy(out, this->data_ + this->pos_, len);
    this->pos_ += len;
    return true;
  }
  const uint8_t *ptr(size_t len) {
    if (this->pos_ + len > this->len_) {
      this->bad_();
      return nullptr;
    }
    const uint8_t *p = this->data_ + this->pos_;
    this->pos_ += len;
    return p;
  }
  bool ok() const { return this->ok_; }

 protected:
  bool bad_() {
    this->ok_ = false;
    return false;
  }
  const uint8_t *data_;
  size_t len_;
  size_t pos_{0};
  bool ok_{true};
};

}  // namespace esphome::fingerprint_grow_plus
