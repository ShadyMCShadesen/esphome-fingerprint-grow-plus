// Optional duplicate check while enrolling (duplicate_check:). After the first enrollment scan,
// CharBuffer 1 holds the features of a fresh image; searching the library with it is the
// module's normal 1:N matching (reliable, unlike template-to-template Match). The target slot
// itself is excluded, so enrolling a finger again into its own slot is not a duplicate.

#include "fingerprint_grow_plus.h"

#include "esphome/core/log.h"

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.duplicate";
static constexpr uint8_t ENROLL_REFUSED = 0x0A;  // any non-OK result ends the enrollment as failed

void FingerprintGrowPlusComponent::setup_duplicate_check_() {
  if (this->duplicate_mode_ == 0)
    return;
  this->grow_->add_on_enrollment_scan_callback([this](uint8_t scan, uint16_t target) {
    if (scan != 1 || !this->params_loaded_)
      return;
    const uint16_t cap = this->capacity_;
    uint16_t found = NO_SLOT, score = 0, slot = 0;
    // Two ranges around the target slot; each search is one complete UART transaction.
    if (target > 0 && this->link_.search(1, 0, target, slot, score) == GROW_OK)
      found = slot;
    if (found == NO_SLOT && target + 1 < cap &&
        this->link_.search(1, target + 1, cap - target - 1, slot, score) == GROW_OK)
      found = slot;
    if (found == NO_SLOT)
      return;
    ESP_LOGW(TAG, "Finger being enrolled into slot %u is already stored in slot %u (score %u)%s", target, found, score,
             this->duplicate_mode_ == 2 ? "; enrollment cancelled" : "");
    this->duplicate_cb_.call(found, target);
    if (this->duplicate_mode_ == 2)
      this->duplicate_cancel_ = true;  // finished in loop(): not inside fingerprint_grow's update()
  });
}

void FingerprintGrowPlusComponent::duplicate_loop_() {
  if (!this->duplicate_cancel_)
    return;
  this->duplicate_cancel_ = false;
  this->grow_->finish_enrollment(ENROLL_REFUSED);
}

}  // namespace esphome::fingerprint_grow_plus
