// Optional built-in Aura LED feedback (led_feedback:). Listens to the component's own events and
// drives the sensor's ring LED through fingerprint_grow. Only endless effects (count 0) are used:
// fingerprint_grow::aura_led_control() blocks the loop while a counted effect is still running,
// so timed effects are ended by a timeout here instead.

#include "fingerprint_grow_plus.h"

#include "esphome/core/log.h"

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.led";

// Aura protocol values (R503 manual)
static constexpr uint8_t LED_FLASHING = 2;
static constexpr uint8_t LED_ON = 3;
static constexpr uint8_t LED_OFF = 4;
static constexpr uint8_t RED = 1, BLUE = 2, PURPLE = 3, GREEN = 4, YELLOW = 5, CYAN = 6, WHITE = 7;

static constexpr uint8_t LED_SPEED = 50;
static constexpr uint32_t RESULT_MS = 1500;  // success / failure flash
static constexpr uint32_t STATUS_MS = 3000;  // status colour after a hold

void FingerprintGrowPlusComponent::setup_led_feedback_() {
  if (!this->led_feedback_)
    return;
  // Preview of the action that runs when the finger stays away (with the default touch counts:
  // 1 cyan, 2 purple = enroll, 3 red = delete, 4 yellow = enroll again, 5 white = guest).
  this->admin_touch_cb_.add([this](uint8_t touches) {
    uint8_t color = CYAN;
    if (touches == this->enroll_touches_)
      color = PURPLE;
    else if (touches == this->delete_touches_)
      color = RED;
    else if (touches == this->reenroll_touches_)
      color = YELLOW;
    else if (touches == this->guest_touches_ && this->guests_enabled_)
      color = WHITE;
    this->led_set_(LED_ON, color, 0);
  });
  // Gesture complete: back to idle; enrolling / waiting set their own colour right after.
  this->admin_gesture_cb_.add([this](uint8_t) { this->led_idle_(); });
  this->admin_hold_cb_.add([this]() {
    static const uint8_t STATUS[] = {GREEN, BLUE, YELLOW, RED};
    this->led_set_(LED_ON, STATUS[std::min<uint8_t>(this->health(), 3)], STATUS_MS);
  });
  this->admin_waiting_cb_.add([this](const std::string &action) {
    this->led_reenroll_pending_ = action == "reenroll";
    this->led_set_(LED_FLASHING, action == "delete" ? RED : YELLOW, 0);
  });
  this->admin_cancelled_cb_.add([this]() {
    this->led_reenroll_pending_ = false;
    this->led_set_(LED_FLASHING, RED, RESULT_MS);
  });
  this->admin_done_cb_.add([this](const std::string &action, uint16_t) {
    if (action == "delete")
      this->led_set_(LED_FLASHING, GREEN, RESULT_MS);
  });
  // Enrolling: purple = new finger, yellow = enroll again, white = guest.
  this->enroll_started_cb_.add([this](uint16_t slot) {
    uint8_t color = PURPLE;
    if (this->in_guest_range_(slot))
      color = WHITE;
    else if (this->led_reenroll_pending_)
      color = YELLOW;
    this->led_reenroll_pending_ = false;
    this->led_enroll_color_ = color;
    this->led_set_(LED_FLASHING, color, 0);
  });
  // Only enrollments started by this component are shown; fingerprint_grow.enroll keeps the
  // user's own feedback.
  this->grow_->add_on_enrollment_scan_callback([this](uint8_t, uint16_t) {
    if (this->led_enroll_color_ != 0)
      this->led_set_(LED_FLASHING, this->led_enroll_color_, 0);
  });
  this->grow_->add_on_enrollment_done_callback([this](uint16_t) {
    if (this->led_enroll_color_ != 0) {
      this->led_enroll_color_ = 0;
      this->led_set_(LED_FLASHING, GREEN, RESULT_MS);
    }
  });
  this->grow_->add_on_enrollment_failed_callback([this](uint16_t) {
    if (this->led_enroll_color_ != 0) {
      this->led_enroll_color_ = 0;
      this->led_set_(LED_FLASHING, RED, RESULT_MS);
    }
  });
}

void FingerprintGrowPlusComponent::led_set_(uint8_t state, uint8_t color, uint32_t idle_after_ms) {
  this->cancel_timeout("led_idle");
  ESP_LOGV(TAG, "LED state %u colour %u", state, color);
  this->grow_->aura_led_control(state, LED_SPEED, color, 0);
  if (idle_after_ms > 0)
    this->set_timeout("led_idle", idle_after_ms, [this]() { this->led_idle_(); });
}

void FingerprintGrowPlusComponent::led_idle_() {
  this->cancel_timeout("led_idle");
  ESP_LOGD(TAG, "LED feedback finished");
  if (this->led_custom_idle_) {
    this->led_idle_cb_.call();  // the user restores their own idle LED
  } else {
    this->grow_->aura_led_control(LED_OFF, 0, BLUE, 0);
  }
}

}  // namespace esphome::fingerprint_grow_plus
