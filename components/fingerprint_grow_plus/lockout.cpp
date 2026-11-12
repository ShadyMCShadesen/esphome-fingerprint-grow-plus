// Optional lockout after failed attempts (lockout:), shared with selected peers, and the duress
// finger (duress:). All state lives in RAM: nothing here is written to flash.

#include "fingerprint_grow_plus.h"
#include "protocol.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.lockout";
static constexpr uint32_t MAX_REMOTE_LOCK_S = 24 * 3600;
static constexpr uint32_t LOCK_RETRY_MS = 30000;

void FingerprintGrowPlusComponent::add_lock_target(const char *host, uint16_t port) {
  for (size_t i = 0; i < this->peers_.size(); i++) {
    if (strcmp(this->peers_[i].host, host) == 0 && (port == 0 || this->peers_[i].port == port))
      this->lock_targets_[i] = true;
  }
}

bool FingerprintGrowPlusComponent::is_locked() const { return this->locked_; }

uint32_t FingerprintGrowPlusComponent::lock_remaining_s_() const {
  if (!this->locked_)
    return 0;
  int32_t left = (int32_t) (this->locked_until_ms_ - millis());
  return left > 0 ? (uint32_t) left / 1000 + 1 : 0;
}

void FingerprintGrowPlusComponent::register_failed_attempt() {
  if (!this->lockout_enabled_ || this->locked_)
    return;
  this->failed_attempts_++;
  ESP_LOGD(TAG, "Failed attempt %u of %u", this->failed_attempts_, this->lockout_max_);
  this->publish_lockout_();
  if (this->failed_attempts_ >= this->lockout_max_)
    this->lock_(this->lockout_ms_, false);
}

void FingerprintGrowPlusComponent::reset_lockout() {
  if (!this->lockout_enabled_)
    return;
  const bool was_locked = this->locked_;
  this->failed_attempts_ = 0;
  this->locked_ = false;
  if (was_locked) {
    ESP_LOGI(TAG, "Lockout lifted manually");
    this->unlocked_cb_.call();
  }
  this->publish_lockout_();
}

void FingerprintGrowPlusComponent::lock_(uint32_t duration_ms, bool remote) {
  const uint32_t until = millis() + duration_ms;
  if (this->locked_ && (int32_t) (until - this->locked_until_ms_) <= 0)
    return;  // already locked for at least as long
  const bool was_locked = this->locked_;
  this->locked_ = true;
  this->locked_until_ms_ = until;
  ESP_LOGW(TAG, "Locked for %" PRIu32 " s (%s)", duration_ms / 1000,
           remote ? "requested by a peer" : "too many failed attempts");
  if (!was_locked)
    this->locked_cb_.call(std::string(remote ? "remote" : "local"));
  this->publish_lockout_();
  // Only a local lock is passed on; a lock received from a peer is never forwarded (no loops).
  if (!remote && this->network_) {
    bool any = false;
    for (size_t i = 0; i < this->peers_.size(); i++) {
      if (this->lock_targets_[i] && !this->is_self_(this->peers_[i])) {
        this->lock_pending_[i] = true;
        any = true;
      }
    }
    if (any)
      this->request_round_(0);
  }
}

void FingerprintGrowPlusComponent::lockout_loop_() {
  if (!this->locked_)
    return;
  if ((int32_t) (millis() - this->locked_until_ms_) >= 0) {
    this->locked_ = false;
    this->failed_attempts_ = 0;
    std::fill(this->lock_pending_.begin(), this->lock_pending_.end(), false);
    ESP_LOGI(TAG, "Lockout expired");
    this->unlocked_cb_.call();
    this->publish_lockout_();
  }
}

void FingerprintGrowPlusComponent::send_pending_lock_(Session &s) {
  if (!s.is_client || s.peer == nullptr || !this->locked_)
    return;
  const size_t idx = s.peer - this->peers_.data();
  if (idx >= this->lock_pending_.size() || !this->lock_pending_[idx])
    return;
  std::vector<uint8_t> msg;
  MsgWriter w(msg);
  w.u8(MSG_LOCK);
  w.u32(this->lock_remaining_s_());
  this->send_message_(s, msg);
  this->lock_pending_[idx] = false;
  ESP_LOGI(TAG, "Lockout passed on to %s", s.peer->host);
}

void FingerprintGrowPlusComponent::handle_remote_lock_(Session &s, uint32_t seconds) {
  if (!this->lockout_enabled_) {
    ESP_LOGW(TAG, "Peer %08" PRIX32 " requested a lockout, but lockout: is not configured here", s.channel.peer_id());
    return;
  }
  if (seconds == 0)
    return;
  this->lock_(std::min(seconds, MAX_REMOTE_LOCK_S) * 1000, true);
}

void FingerprintGrowPlusComponent::retry_pending_locks_() {
  if (!this->locked_)
    return;
  for (bool pending : this->lock_pending_) {
    if (pending) {
      this->request_round_(LOCK_RETRY_MS);  // a target was unreachable: try again
      return;
    }
  }
}

void FingerprintGrowPlusComponent::publish_lockout_() {
#ifdef USE_SENSOR
  if (this->failed_attempts_sensor_ != nullptr)
    this->failed_attempts_sensor_->publish_state(this->failed_attempts_);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->locked_binary_sensor_ != nullptr)
    this->locked_binary_sensor_->publish_state(this->locked_);
#endif
}

void FingerprintGrowPlusComponent::lockout_matched_(uint16_t finger_id, uint16_t confidence) {
  if (this->duress_enabled_ && this->is_duress_slot(finger_id)) {
    ESP_LOGW(TAG, "Duress finger (slot %u) matched", finger_id);
    this->duress_cb_.call(finger_id, confidence);
  }
  if (this->lockout_enabled_ && this->lockout_reset_on_match_ && !this->locked_ && this->failed_attempts_ > 0) {
    this->failed_attempts_ = 0;
    this->publish_lockout_();
  }
}

bool FingerprintGrowPlusComponent::is_duress_slot(uint16_t slot) const {
  return this->duress_enabled_ && slot >= this->duress_from_ && slot <= this->duress_to_;
}

}  // namespace esphome::fingerprint_grow_plus
