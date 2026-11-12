// Admin fingers (gestures, protected slots) and guest fingers with an expiry date.

#include "fingerprint_grow_plus.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.admin";
static constexpr uint32_t EXPIRY_CHECK_INTERVAL_MS = 30000;
static constexpr size_t EXPIRY_PREF_CHUNK = 64;
// The touch output of the sensor may drop for a moment while a finger rests on it; shorter gaps
// than this do not count as "lifted".
static constexpr uint32_t FINGER_OFF_DEBOUNCE_MS = 500;

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t duration) { return now - since >= duration; }

static const char *arm_name(AdminArm arm) {
  switch (arm) {
    case AdminArm::DELETE:
      return "delete";
    case AdminArm::REENROLL:
      return "reenroll";
    default:
      return "none";
  }
}

// ---------------------------------------------------------------------------------------------
// Admin slots
// ---------------------------------------------------------------------------------------------

bool FingerprintGrowPlusComponent::is_admin_slot(uint16_t slot) const {
  for (uint16_t s : this->admin_slots_) {
    if (s == slot)
      return true;
  }
  return false;
}

bool FingerprintGrowPlusComponent::is_admin_busy() const {
  return this->admin_hits_ > 0 || this->armed_ != AdminArm::NONE || this->hold_lock_;
}

uint8_t FingerprintGrowPlusComponent::health() const {
  if (this->failed_sensor_ || !this->last_round_ok_)
    return 3;
  if (this->conflict_count() > 0)
    return 2;
  if (this->round_active_)
    return 1;
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::on_matched_(uint16_t finger_id, uint16_t confidence) {
  const uint32_t now = millis();
  this->last_activity_ms_ = now;
  this->lockout_matched_(finger_id, confidence);
  this->schedule_matched_(finger_id);
#ifdef USE_TEXT_SENSOR
  if (this->last_name_text_sensor_ != nullptr) {
    std::string name = this->slot_name(finger_id);
    if (name.empty())
      name = "#" + to_string(finger_id);
    ESP_LOGD(TAG, "Last name: %s", name.c_str());
    this->last_name_text_sensor_->publish_state(name);
  }
#endif
  if (!this->admin_enabled_)
    return;
  const bool admin = this->is_admin_slot(finger_id);

  if (this->armed_ != AdminArm::NONE) {
    if (admin) {
      // An admin finger instead of a target finger aborts: admin slots are never targets.
      ESP_LOGI(TAG, "Admin finger presented again: %s action cancelled", arm_name(this->armed_));
      this->armed_ = AdminArm::NONE;
      this->admin_cancelled_cb_.call();
      return;
    }
    this->run_armed_(finger_id);
    return;
  }
  if (!admin)
    return;
  if (this->hold_lock_) {
    // The admin finger is still (or again) on the sensor after a hold: the sensor matched it a
    // second time. That is part of the hold, never a new touch, so it triggers nothing.
    this->admin_quiet_since_ms_ = now;
    ESP_LOGD(TAG, "Admin finger matched again during a hold: ignored");
    return;
  }

  if (this->admin_hits_ > 0 && elapsed(now, this->admin_quiet_since_ms_, this->gesture_timeout_ms_))
    this->admin_hits_ = 0;
  this->admin_hits_++;
  this->admin_last_ms_ = now;
  this->admin_quiet_since_ms_ = now;
  // A hold is only possible on the first touch and needs a finger sensor to see the finger.
  this->admin_hold_possible_ = this->admin_hits_ == 1 && this->hold_ms_ > 0 && this->finger_sensor_ != nullptr;
  ESP_LOGD(TAG, "Admin finger (slot %u), touch %u", finger_id, this->admin_hits_);
  if (this->admin_hits_ == 1) {
    this->admin_first_slot_ = finger_id;
    this->admin_first_confidence_ = confidence;
  }
  this->admin_touch_cb_.call(this->admin_hits_);
  if (this->admin_access_ == 1 && this->admin_hits_ == 1) {
    ESP_LOGI(TAG, "Admin finger (slot %u): access", finger_id);
    this->admin_access_cb_.call(finger_id, confidence);
  }
}

void FingerprintGrowPlusComponent::admin_loop_() {
  const uint32_t now = millis();
  if (this->armed_ != AdminArm::NONE && (int32_t) (now - this->armed_until_ms_) >= 0) {
    ESP_LOGI(TAG, "No finger presented in time: %s action cancelled", arm_name(this->armed_));
    this->armed_ = AdminArm::NONE;
    this->admin_cancelled_cb_.call();
  }
  bool finger_on = false;
  if (this->finger_sensor_ != nullptr) {
    if (this->finger_sensor_->state)
      this->finger_on_ms_ = now;
    finger_on = this->finger_sensor_->has_state() && !elapsed(now, this->finger_on_ms_, FINGER_OFF_DEBOUNCE_MS);
  }
  if (this->hold_lock_) {
    // After a hold nothing is counted until the finger has been off the sensor for `window`.
    if (finger_on) {
      this->admin_quiet_since_ms_ = now;
    } else if (elapsed(now, this->admin_quiet_since_ms_, this->gesture_timeout_ms_)) {
      this->hold_lock_ = false;
      ESP_LOGD(TAG, "Admin hold finished");
    }
    return;
  }
  if (this->admin_hits_ == 0)
    return;

  // The pause that completes a gesture starts when the finger is lifted, not at the match: on real
  // sensors the next touch is matched 1.5-3 s after the previous match (lift, touch, image, search),
  // and a finger lying on the sensor keeps the gesture open until the sensor reports the match.
  if (finger_on)
    this->admin_quiet_since_ms_ = now;
  if (this->admin_hold_possible_ && finger_on) {
    if (elapsed(now, this->admin_last_ms_, this->hold_ms_)) {
      this->admin_hits_ = 0;
      this->admin_hold_possible_ = false;
      this->hold_lock_ = true;  // a hold never ends in a gesture or access, even if matched again
      this->admin_quiet_since_ms_ = now;
      ESP_LOGI(TAG, "Admin finger held: status %u", this->health());
      this->admin_hold_cb_.call();
    }
    return;  // still deciding between a hold and a single touch
  }
  if (finger_on || !elapsed(now, this->admin_quiet_since_ms_, this->gesture_timeout_ms_))
    return;
  uint8_t touches = this->admin_hits_;
  this->admin_hits_ = 0;
  this->admin_hold_possible_ = false;
  this->run_gesture_(touches);
}

void FingerprintGrowPlusComponent::run_gesture_(uint8_t touches) {
  ESP_LOGI(TAG, "Admin gesture: %u touch(es)", touches);
  this->admin_gesture_cb_.call(touches);
  if (touches == 1 && this->admin_access_ == 2) {
    ESP_LOGI(TAG, "Admin finger (slot %u): access", this->admin_first_slot_);
    this->admin_access_cb_.call(this->admin_first_slot_, this->admin_first_confidence_);
  } else if (touches == this->enroll_touches_) {
    this->enroll_next_free(this->admin_num_scans_);
  } else if (touches == this->delete_touches_) {
    this->arm_(AdminArm::DELETE);
  } else if (touches == this->reenroll_touches_) {
    this->arm_(AdminArm::REENROLL);
  } else if (touches == this->guest_touches_ && this->guests_enabled_) {
    this->enroll_guest(this->admin_num_scans_, this->guest_valid_s_);
  }
}

void FingerprintGrowPlusComponent::arm_(AdminArm action) {
  if (!this->indexed_once_) {
    ESP_LOGW(TAG, "Admin action refused: index not ready yet");
    return;
  }
  this->armed_ = action;
  this->armed_until_ms_ = millis() + this->target_timeout_ms_;
  ESP_LOGI(TAG, "Waiting %" PRIu32 " s for the finger to %s", this->target_timeout_ms_ / 1000, arm_name(action));
  this->admin_waiting_cb_.call(std::string(arm_name(action)));
}

void FingerprintGrowPlusComponent::delete_next_finger() { this->arm_(AdminArm::DELETE); }
void FingerprintGrowPlusComponent::reenroll_next_finger() { this->arm_(AdminArm::REENROLL); }

void FingerprintGrowPlusComponent::cancel_admin() {
  if (this->armed_ == AdminArm::NONE && this->admin_hits_ == 0)
    return;
  this->armed_ = AdminArm::NONE;
  this->admin_hits_ = 0;
  this->admin_cancelled_cb_.call();
}

void FingerprintGrowPlusComponent::run_armed_(uint16_t slot) {
  const AdminArm action = this->armed_;
  this->armed_ = AdminArm::NONE;
  if (action == AdminArm::DELETE) {
    ESP_LOGW(TAG, "Admin: deleting slot %u on all nodes", slot);
    this->delete_slot(slot);
  } else if (action == AdminArm::REENROLL) {
    if (this->enrolling_) {
      ESP_LOGW(TAG, "Admin: re-enrollment refused, an enrollment is already running");
      this->admin_cancelled_cb_.call();
      return;
    }
    ESP_LOGW(TAG, "Admin: re-enrolling slot %u; the new template replaces it on all nodes", slot);
    this->enroll_started_cb_.call(slot);
    this->grow_->enroll_fingerprint(slot, this->admin_num_scans_);
  }
  this->admin_done_cb_.call(std::string(arm_name(action)), slot);
}

void FingerprintGrowPlusComponent::delete_admin(uint16_t slot, const std::string &code) {
  if (!this->is_admin_slot(slot)) {
    ESP_LOGW(TAG, "delete_admin: slot %u is not an admin slot; use fingerprint_grow_plus.delete", slot);
    return;
  }
  if (this->delete_all_code_ == nullptr || this->delete_all_code_[0] == '\0' || code != this->delete_all_code_) {
    ESP_LOGW(TAG, "delete_admin refused: wrong or unconfigured code");
    return;
  }
  if (!this->params_loaded_ || slot >= this->capacity_)
    return;
  ESP_LOGW(TAG, "Deleting ADMIN slot %u on all nodes", slot);
  Job job;
  job.type = JobType::DELETE;
  job.slot = slot;
  job.flags = JOB_FLAG_LOCAL | JOB_FLAG_PROPAGATE;
  this->queue_job_(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Guests
// ---------------------------------------------------------------------------------------------

bool FingerprintGrowPlusComponent::time_now_(uint32_t &epoch) const {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    auto t = this->time_->now();
    if (t.is_valid()) {
      epoch = (uint32_t) t.timestamp;
      return true;
    }
  }
#endif
  return false;
}

void FingerprintGrowPlusComponent::enroll_guest(uint8_t num_scans, uint32_t valid_s, const std::string &name) {
  if (!this->guests_enabled_) {
    ESP_LOGW(TAG, "Guest enrollment refused: no guest slots configured");
    return;
  }
  uint32_t now;
  if (!this->time_now_(now)) {
    ESP_LOGW(TAG, "Guest enrollment refused: the time is not known yet");
    return;
  }
  if (!this->indexed_once_ || this->enrolling_) {
    ESP_LOGW(TAG, "Guest enrollment refused: index not ready or an enrollment is running");
    return;
  }
  int32_t slot = this->next_free_in_(this->guest_from_, this->guest_to_, true);
  if (slot < 0) {
    ESP_LOGE(TAG, "Guest enrollment refused: all guest slots are in use");
    return;
  }
  this->pending_guest_slot_ = (uint16_t) slot;
  this->pending_name_slot_ = name.empty() ? NO_SLOT : (uint16_t) slot;
  this->pending_name_ = name;
  this->pending_guest_expiry_ = now + valid_s;
  ESP_LOGI(TAG, "Enrolling guest in slot %" PRId32 ", valid for %" PRIu32 " h", slot, valid_s / 3600);
  this->enroll_started_cb_.call((uint16_t) slot);
  this->grow_->enroll_fingerprint((uint16_t) slot, num_scans);
}

void FingerprintGrowPlusComponent::extend_guest(uint16_t slot, uint32_t valid_s) {
  uint32_t now;
  if (!this->params_loaded_ || slot >= this->capacity_ || !meta_present(this->meta_[slot]) ||
      this->expires_[slot] == 0) {
    ESP_LOGW(TAG, "extend_guest: slot %u is not a guest finger", slot);
    return;
  }
  if (!this->time_now_(now)) {
    ESP_LOGW(TAG, "extend_guest refused: the time is not known yet");
    return;
  }
  // Same template, new version: peers adopt the new expiry without moving the template.
  this->meta_[slot] = meta_pack(meta_state(this->meta_[slot]), this->next_version_());
  this->set_expiry_(slot, now + valid_s);
  this->mark_dirty_();
  ESP_LOGI(TAG, "Guest slot %u now valid for %" PRIu32 " h", slot, valid_s / 3600);
  this->request_change_round_(2000);
}

void FingerprintGrowPlusComponent::set_expiry_(uint16_t slot, uint32_t expires) {
  if (slot < this->expires_.size() && this->expires_[slot] != expires) {
    this->expires_[slot] = expires;
    this->mark_dirty_();
  }
}

void FingerprintGrowPlusComponent::guest_expiry_loop_() {
  const uint32_t now_ms = millis();
  if (!this->indexed_once_ || !elapsed(now_ms, this->last_expiry_check_ms_, EXPIRY_CHECK_INTERVAL_MS))
    return;
  this->last_expiry_check_ms_ = now_ms;
  uint32_t now;
  if (!this->time_now_(now))
    return;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    const uint32_t exp = this->expires_[slot];
    const bool synced = this->managed_(slot);
    const bool present = synced ? meta_present(this->meta_[slot]) : this->local_present_[slot];
    if (exp == 0 || now < exp || !present || this->is_admin_slot(slot))
      continue;
    ESP_LOGI(TAG, "Guest finger in slot %u expired; deleting it%s", slot, synced ? " on all nodes" : "");
    Job job;
    job.type = JobType::DELETE;
    job.slot = slot;
    job.flags = JOB_FLAG_LOCAL | (synced ? JOB_FLAG_PROPAGATE : 0);
    this->queue_job_(std::move(job));
  }
}

int32_t FingerprintGrowPlusComponent::next_free_in_(uint16_t from, uint16_t to, bool guests_allowed) const {
  if (!this->params_loaded_ || this->capacity_ == 0)
    return -1;
  to = std::min<uint16_t>(to, this->capacity_ - 1);
  for (uint32_t slot = from; slot <= to; slot++) {
    if (this->is_admin_slot(slot) || (!guests_allowed && this->in_guest_range_(slot)))
      continue;
    if (this->local_present_[slot])
      continue;
    // Synced slots must also be free on the peers. Slots outside include/exclude are local
    // only: free when this sensor has nothing there.
    if (this->managed_(slot) &&
        (this->remote_present_[slot] || this->conflict_[slot] || meta_present(this->meta_[slot])))
      continue;
    return (int32_t) slot;
  }
  return -1;
}

// ---------------------------------------------------------------------------------------------
// Persistence of the slot extras: guest expiry, guest uses, names (preferences backend only; the
// notepad has no room for them). ESPHome compares with the stored value before writing, so only
// changed blocks wear the flash; names are only written once one was set or changed.
// ---------------------------------------------------------------------------------------------

static constexpr size_t USES_PREF_CHUNK = 64;
static constexpr size_t NAMES_PREF_CHUNK = 16;
using NameBlock = std::array<uint8_t, NAMES_PREF_CHUNK * (1 + MAX_NAME_LEN)>;

void FingerprintGrowPlusComponent::load_expiries_() {
  this->expires_.assign(this->capacity_, 0);
  this->uses_.assign(this->capacity_, 0);
  this->names_.clear();
  if (this->backend_ != MetaBackend::PREFERENCES)
    return;
  const uint32_t exp_base = fnv1_hash("fingerprint_grow_plus_expiry") ^ this->node_id_;
  for (size_t c = 0; c * EXPIRY_PREF_CHUNK < this->capacity_; c++) {
    this->expiry_prefs_.push_back(
        global_preferences->make_preference<std::array<uint32_t, EXPIRY_PREF_CHUNK>>(exp_base + c, true));
    std::array<uint32_t, EXPIRY_PREF_CHUNK> chunk{};
    if (!this->expiry_prefs_[c].load(&chunk))
      continue;
    for (size_t i = 0; i < EXPIRY_PREF_CHUNK && c * EXPIRY_PREF_CHUNK + i < this->capacity_; i++)
      this->expires_[c * EXPIRY_PREF_CHUNK + i] = chunk[i];
  }
  const uint32_t uses_base = fnv1_hash("fingerprint_grow_plus_uses") ^ this->node_id_;
  for (size_t c = 0; c * USES_PREF_CHUNK < this->capacity_; c++) {
    this->uses_prefs_.push_back(
        global_preferences->make_preference<std::array<uint8_t, USES_PREF_CHUNK>>(uses_base + c, true));
    std::array<uint8_t, USES_PREF_CHUNK> chunk{};
    if (!this->uses_prefs_[c].load(&chunk))
      continue;
    for (size_t i = 0; i < USES_PREF_CHUNK && c * USES_PREF_CHUNK + i < this->capacity_; i++)
      this->uses_[c * USES_PREF_CHUNK + i] = chunk[i];
  }
  const uint32_t names_base = fnv1_hash("fingerprint_grow_plus_names") ^ this->node_id_;
  for (size_t c = 0; c * NAMES_PREF_CHUNK < this->capacity_; c++) {
    this->names_prefs_.push_back(global_preferences->make_preference<NameBlock>(names_base + c, true));
    NameBlock block{};
    if (!this->names_prefs_[c].load(&block))
      continue;
    for (size_t i = 0; i < NAMES_PREF_CHUNK && c * NAMES_PREF_CHUNK + i < this->capacity_; i++) {
      const uint8_t *entry = block.data() + i * (1 + MAX_NAME_LEN);
      const uint8_t len = std::min<uint8_t>(entry[0], MAX_NAME_LEN);
      if (len > 0)
        this->names_[c * NAMES_PREF_CHUNK + i] = std::string((const char *) entry + 1, len);
    }
  }
}

void FingerprintGrowPlusComponent::save_expiries_() {
  for (size_t c = 0; c < this->expiry_prefs_.size(); c++) {
    std::array<uint32_t, EXPIRY_PREF_CHUNK> chunk{};
    for (size_t i = 0; i < EXPIRY_PREF_CHUNK && c * EXPIRY_PREF_CHUNK + i < this->capacity_; i++)
      chunk[i] = this->expires_[c * EXPIRY_PREF_CHUNK + i];
    this->expiry_prefs_[c].save(&chunk);
  }
  for (size_t c = 0; c < this->uses_prefs_.size(); c++) {
    std::array<uint8_t, USES_PREF_CHUNK> chunk{};
    for (size_t i = 0; i < USES_PREF_CHUNK && c * USES_PREF_CHUNK + i < this->capacity_; i++)
      chunk[i] = this->uses_[c * USES_PREF_CHUNK + i];
    this->uses_prefs_[c].save(&chunk);
  }
  if (!this->names_dirty_)
    return;
  this->names_dirty_ = false;
  for (size_t c = 0; c < this->names_prefs_.size(); c++) {
    NameBlock block{};
    for (size_t i = 0; i < NAMES_PREF_CHUNK; i++) {
      auto it = this->names_.find(c * NAMES_PREF_CHUNK + i);
      if (it == this->names_.end())
        continue;
      uint8_t *entry = block.data() + i * (1 + MAX_NAME_LEN);
      entry[0] = (uint8_t) std::min(it->second.size(), MAX_NAME_LEN);
      memcpy(entry + 1, it->second.data(), entry[0]);
    }
    this->names_prefs_[c].save(&block);
  }
}

}  // namespace esphome::fingerprint_grow_plus
