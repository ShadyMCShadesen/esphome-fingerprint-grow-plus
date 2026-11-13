// Optional access log (access_log:): the last scans of this node in a RAM ring buffer. Nothing is
// written to flash or sent to peers; the backup tool can read it with the sync key
// (MSG_ACCESS_LOG_REQ), and Home Assistant gets each entry through on_entry / last_access.

#include "fingerprint_grow_plus.h"
#include "protocol.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.access";

const char *FingerprintGrowPlusComponent::access_result_name(uint8_t result) {
  switch (result) {
    case ACCESS_MATCHED:
      return "matched";
    case ACCESS_UNKNOWN:
      return "unknown";
    case ACCESS_OUTSIDE_SCHEDULE:
      return "outside_schedule";
    case ACCESS_WHILE_LOCKED:
      return "while_locked";
    case ACCESS_DURESS:
      return "duress";
    default:
      return "?";
  }
}

uint8_t FingerprintGrowPlusComponent::access_result_of_(uint16_t slot) const {
  if (this->is_duress_slot(slot))
    return ACCESS_DURESS;
  if (this->locked_)
    return ACCESS_WHILE_LOCKED;
  if (!this->schedules_.empty() && !this->is_allowed(slot))
    return ACCESS_OUTSIDE_SCHEDULE;
  return ACCESS_MATCHED;
}

void FingerprintGrowPlusComponent::access_record_(uint16_t slot, uint16_t confidence, uint8_t result,
                                                  uint8_t flags) {
  const bool text = false
#ifdef USE_TEXT_SENSOR
                    || this->last_access_text_sensor_ != nullptr
#endif
      ;
  if (this->access_size_ == 0 && !text)
    return;
  AccessEntry e;
  e.stamp_ms = millis();
  this->time_now_(e.time);
  e.slot = slot;
  e.confidence = confidence;
  e.result = result;
  e.flags = flags;
  if (slot != NO_SLOT)
    e.name = this->slot_name(slot);
  const std::string who = slot == NO_SLOT ? std::string("unknown finger")
                          : e.name.empty() ? "#" + to_string(slot)
                                           : e.name + " (#" + to_string(slot) + ")";
  ESP_LOGD(TAG, "Access: %s %s", who.c_str(), access_result_name(result));
#ifdef USE_TEXT_SENSOR
  if (this->last_access_text_sensor_ != nullptr)
    this->last_access_text_sensor_->publish_state(slot == NO_SLOT ? who : who + ": " + access_result_name(result));
#endif
  if (this->access_size_ == 0)
    return;
  this->access_cb_.call(slot, std::string(access_result_name(result)), e.name);
  if (this->access_.size() < this->access_size_) {
    this->access_.push_back(std::move(e));
  } else {
    this->access_[this->access_next_] = std::move(e);
  }
  this->access_next_ = (this->access_next_ + 1) % this->access_size_;
}

std::vector<AccessEntry> FingerprintGrowPlusComponent::access_log() const {
  std::vector<AccessEntry> out;
  out.reserve(this->access_.size());
  // Before the buffer is full, access_next_ == size and the oldest entry is at 0.
  const size_t start = this->access_.size() < this->access_size_ ? 0 : this->access_next_;
  for (size_t i = 0; i < this->access_.size(); i++)
    out.push_back(this->access_[(start + i) % this->access_.size()]);
  return out;
}

void FingerprintGrowPlusComponent::clear_access_log() {
  this->access_.clear();
  this->access_next_ = 0;
  ESP_LOGI(TAG, "Access log cleared");
}

void FingerprintGrowPlusComponent::dump_access_log() {
  if (this->access_size_ == 0) {
    ESP_LOGW(TAG, "dump_access_log: access_log: is not configured");
    return;
  }
  const uint32_t now = millis();
  auto entries = this->access_log();
  ESP_LOGI(TAG, "Access log: %u entr%s (oldest first)", (unsigned) entries.size(), entries.size() == 1 ? "y" : "ies");
  for (const auto &e : entries) {
    char when[24];
#ifdef USE_TIME
    if (e.time != 0) {
      ESPTime t = ESPTime::from_epoch_local(e.time);
      t.strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S");
    } else
#endif
    {
      snprintf(when, sizeof(when), "%" PRIu32 " s ago", (uint32_t) (now - e.stamp_ms) / 1000);
    }
    if (e.slot == NO_SLOT) {
      ESP_LOGI(TAG, "  %s  unknown finger", when);
    } else {
      ESP_LOGI(TAG, "  %s  slot %u%s%s  %s (confidence %u)%s%s%s", when, e.slot, e.name.empty() ? "" : " ",
               e.name.c_str(), access_result_name(e.result), e.confidence,
               (e.flags & ACCESS_FLAG_ADMIN) ? " admin" : "", (e.flags & ACCESS_FLAG_GUEST) ? " guest" : "",
               (e.flags & ACCESS_FLAG_TARGET) ? " admin-target" : "");
    }
  }
}

void FingerprintGrowPlusComponent::send_access_log_(Session &s) {
  const uint32_t now = millis();
  auto entries = this->access_log();
  const size_t pages =
      std::max<size_t>(1, (entries.size() + ACCESS_LOG_ENTRIES_PER_PAGE - 1) / ACCESS_LOG_ENTRIES_PER_PAGE);
  ESP_LOGI(TAG, "Access log (%u entries) sent to %08" PRIX32, (unsigned) entries.size(), s.channel.peer_id());
  std::vector<uint8_t> msg;
  for (size_t page = 0; page < pages; page++) {
    const size_t first = page * ACCESS_LOG_ENTRIES_PER_PAGE;
    const size_t count = std::min(ACCESS_LOG_ENTRIES_PER_PAGE, entries.size() - std::min(first, entries.size()));
    MsgWriter w(msg);
    w.u8(MSG_ACCESS_LOG);
    w.u8((uint8_t) page);
    w.u8((page + 1 == pages ? 1 : 0) | (this->access_size_ == 0 ? 2 : 0));
    w.u16((uint16_t) count);
    for (size_t i = 0; i < count; i++) {
      const auto &e = entries[first + i];
      w.u32(e.time);
      w.u32((now - e.stamp_ms) / 1000);
      w.u16(e.slot);
      w.u16(e.confidence);
      w.u8(e.result);
      w.u8(e.flags);
      const size_t len = std::min(e.name.size(), MAX_NAME_LEN);
      w.u8((uint8_t) len);
      w.bytes((const uint8_t *) e.name.data(), len);
    }
    this->send_message_(s, msg);
  }
}

}  // namespace esphome::fingerprint_grow_plus
