// Optional time windows for slot ranges (schedules:). The component only answers is_allowed();
// the door action decides. Configuration only, nothing is stored or synced.

#include "fingerprint_grow_plus.h"

#include "esphome/core/log.h"

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.schedule";

void FingerprintGrowPlusComponent::add_schedule(uint16_t from, uint16_t to) {
  this->schedules_.push_back(Schedule{from, to, {}});
}

void FingerprintGrowPlusComponent::add_schedule_window(uint8_t days_mask, uint16_t from_min, uint16_t to_min) {
  if (!this->schedules_.empty())
    this->schedules_.back().windows.push_back(ScheduleWindow{days_mask, from_min, to_min});
}

bool FingerprintGrowPlusComponent::window_open_(const ScheduleWindow &w, uint8_t weekday, uint16_t minute) {
  // weekday: 0 = Sunday ... 6 = Saturday; minute: minutes since midnight
  const uint8_t today = 1u << weekday;
  const uint8_t yesterday = 1u << ((weekday + 6) % 7);
  if (w.from_min < w.to_min)
    return (w.days & today) && minute >= w.from_min && minute < w.to_min;
  // Over midnight (e.g. 22:00-06:00): the day is the day the window starts.
  return ((w.days & today) && minute >= w.from_min) || ((w.days & yesterday) && minute < w.to_min);
}

bool FingerprintGrowPlusComponent::is_allowed(uint16_t slot) const {
  bool scheduled = false;
  for (const auto &sch : this->schedules_) {
    if (slot < sch.from || slot > sch.to)
      continue;
    scheduled = true;
#ifdef USE_TIME
    if (this->time_ != nullptr) {
      auto t = this->time_->now();
      if (t.is_valid()) {
        const uint8_t weekday = (uint8_t) (t.day_of_week - 1);  // ESPTime: 1 = Sunday
        const uint16_t minute = (uint16_t) (t.hour * 60 + t.minute);
        for (const auto &w : sch.windows) {
          if (window_open_(w, weekday, minute))
            return true;
        }
        continue;
      }
    }
#endif
    if (this->schedule_allow_without_time_)
      return true;
  }
  return !scheduled;  // slots without a schedule are always allowed
}

void FingerprintGrowPlusComponent::schedule_matched_(uint16_t finger_id) {
  if (this->schedules_.empty() || this->is_allowed(finger_id))
    return;
  ESP_LOGI(TAG, "Slot %u matched outside its time window", finger_id);
  this->denied_cb_.call(finger_id);
}

}  // namespace esphome::fingerprint_grow_plus
