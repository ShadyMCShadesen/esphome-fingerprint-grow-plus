#include "fingerprint_grow_plus.h"
#include "crypto.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus";

/// Matching has priority: no sensor job within this time after any scan activity.
static constexpr uint32_t ACTIVITY_HOLDOFF_MS = 1500;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30000;
/// An unauthenticated connection must complete the handshake within this time; it occupies the
/// only inbound slot meanwhile.
static constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 5000;
/// A peer that stops reading our answers must not make the send buffer grow without bound.
static constexpr size_t SESSION_TX_LIMIT = 16 * 1024;
static constexpr uint32_t SESSION_MAX_MS = 10 * 60 * 1000;
static constexpr uint32_t META_SAVE_DEBOUNCE_MS = 2000;
static constexpr uint32_t PUSH_DELAY_MS = 2000;
static constexpr uint32_t SENSOR_RETRY_MS = 30000;
static constexpr uint16_t MAX_CAPACITY = 1024;
static constexpr size_t MAX_QUEUED_JOBS = 64;
/// Hard limit for the job queue (a job without template data is about 48 bytes).
static constexpr size_t JOB_QUEUE_LIMIT = 300;
/// A finger sensor that reports "finger present" longer than this is considered stuck.
static constexpr uint32_t FINGER_STUCK_MS = 60000;
/// A sync round that cannot even refresh the index within this time is given up.
static constexpr uint32_t ROUND_REFRESH_TIMEOUT_MS = 5 * 60 * 1000;

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t duration) { return now - since >= duration; }

// ---------------------------------------------------------------------------------------------
// Setup / config
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::setup() {
  this->boot_ms_ = millis();
  this->link_.set_uart(this);
  this->node_id_ = fnv1_hash(App.get_name());
  if (this->node_id_ == 0)
    this->node_id_ = 1;
  if (this->backend_ == MetaBackend::NOTEPAD) {
    this->storage_ = std::make_unique<NotepadStorage>(&this->link_);
  } else {
    this->storage_ = std::make_unique<PreferenceStorage>(fnv1_hash("fingerprint_grow_plus_meta") ^ this->node_id_);
  }
  if (this->network_)
    this->transport_.setup(TAG);

  auto activity = [this]() { this->last_activity_ms_ = millis(); };
  this->grow_->add_on_finger_scan_start_callback(activity);
  this->grow_->add_on_finger_scan_unmatched_callback([this]() {
    this->last_activity_ms_ = millis();
    if (this->lockout_enabled_)
      this->register_failed_attempt();
  });
  this->grow_->add_on_finger_scan_misplaced_callback(activity);
  this->grow_->add_on_finger_scan_invalid_callback(activity);
  this->grow_->add_on_finger_scan_matched_callback(
      [this](uint16_t finger_id, uint16_t confidence) { this->on_matched_(finger_id, confidence); });
  // Enrollment uses the sensor's character buffers across several loop() calls; no sync job
  // may touch them from the first enrollment scan until the enrollment has finished.
  this->grow_->add_on_enrollment_scan_callback([this](uint8_t, uint16_t) {
    this->enrolling_ = true;
    this->last_activity_ms_ = millis();
  });
  this->grow_->add_on_enrollment_done_callback([this](uint16_t slot) {
    this->enrolling_ = false;
    this->last_activity_ms_ = millis();
    if (!this->params_loaded_ || slot >= this->capacity_)
      return;
    this->local_present_[slot] = true;
    if (!this->managed_(slot)) {
      // Local-only slot (outside include/exclude): no version, no hash, only the guest expiry.
      if (slot == this->pending_guest_slot_) {
        this->set_expiry_(slot, this->pending_guest_expiry_);
        this->pending_guest_slot_ = NO_SLOT;
      }
      ESP_LOGI(TAG, "Slot %u enrolled locally; it is not synced (outside the synced slots)", slot);
      return;
    }
    this->pending_hash_++;  // counted before queueing: queue_job_ undoes it for a duplicate
    this->queue_simple_job_(JobType::HASH, slot, JOB_FLAG_ENROLLED | JOB_FLAG_PROPAGATE);
  });
  this->grow_->add_on_enrollment_failed_callback([this](uint16_t) {
    this->enrolling_ = false;
    this->pending_guest_slot_ = NO_SLOT;
    this->last_activity_ms_ = millis();
  });

  this->set_status_("starting");
  this->publish_diagnostics_();
  this->setup_led_feedback_();
}

void FingerprintGrowPlusComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Fingerprint Grow Plus:");
  ESP_LOGCONFIG(TAG, "  Node: %s (id %08" PRIX32 ")", App.get_name().c_str(), this->node_id_);
  ESP_LOGCONFIG(TAG, "  Metadata storage: %s", this->storage_ ? this->storage_->name() : "?");
  if (this->finger_sensor_ == nullptr)
    ESP_LOGCONFIG(TAG, "  No finger_sensor: admin gestures end 'window' after the last match, no hold");
  if (this->admin_enabled_) {
    ESP_LOGCONFIG(TAG, "  Admin:");
    for (uint16_t slot : this->admin_slots_)
      ESP_LOGCONFIG(TAG, "    Admin finger: slot %u (protected)", slot);
    ESP_LOGCONFIG(TAG, "    Gestures: enroll %u, delete %u, re-enroll %u, guest %u touches (0 = off); hold %" PRIu32 " ms",
                  this->enroll_touches_, this->delete_touches_, this->reenroll_touches_, this->guest_touches_,
                  this->hold_ms_);
    if (this->admin_access_ != 0)
      ESP_LOGCONFIG(TAG, "    Single touch: access (%s)", this->admin_access_ == 1 ? "immediate" : "delayed");
  }
  if (this->guests_enabled_)
    ESP_LOGCONFIG(TAG, "  Guests: slots %u-%u, default validity %" PRIu32 " h%s", this->guest_from_, this->guest_to_,
                  this->guest_valid_s_ / 3600,
                  this->backend_ == MetaBackend::NOTEPAD ? " (expiry not persisted with the notepad)" : "");
  if (!this->network_) {
    ESP_LOGCONFIG(TAG, "  Sync: off (no sync: block), local features only");
    return;
  }
  ESP_LOGCONFIG(TAG, "  Sync:");
  this->transport_.dump_config();
  for (const auto &peer : this->peers_)
    ESP_LOGCONFIG(TAG, "    Peer: %s:%u%s", peer.host, peer.port, this->is_self_(peer) ? " (this node, skipped)" : "");
  for (const auto &r : this->include_)
    ESP_LOGCONFIG(TAG, "    Synced slots: %u-%u", r.from, r.to);
  for (const auto &r : this->exclude_)
    ESP_LOGCONFIG(TAG, "    Not synced: %u-%u", r.from, r.to);
  if (this->sync_interval_ms_ == 0) {
    ESP_LOGCONFIG(TAG, "    Interval: never (manual sync only)");
  } else {
    ESP_LOGCONFIG(TAG, "    Interval: %" PRIu32 " s", this->sync_interval_ms_ / 1000);
  }
  ESP_LOGCONFIG(TAG, "    Sync at boot: %s, after local changes: %s", YESNO(this->sync_at_boot_),
                YESNO(this->sync_after_change_));
  if (this->dry_run_)
    ESP_LOGCONFIG(TAG, "    DRY RUN: the sync writes and deletes nothing on this sensor");
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

bool FingerprintGrowPlusComponent::managed_(uint16_t slot) const {
  if (slot >= this->capacity_)
    return false;
  bool included = this->include_.empty();
  for (const auto &r : this->include_) {
    if (slot >= r.from && slot <= r.to) {
      included = true;
      break;
    }
  }
  if (!included)
    return false;
  for (const auto &r : this->exclude_) {
    if (slot >= r.from && slot <= r.to)
      return false;
  }
  return true;
}

uint16_t FingerprintGrowPlusComponent::next_version_() {
  if (this->clock_ >= MAX_VERSION) {
    ESP_LOGE(TAG, "Version counter exhausted; see docs/protocol.md (version reset)");
    return MAX_VERSION;
  }
  return ++this->clock_;
}

void FingerprintGrowPlusComponent::mark_dirty_() {
  if (!this->meta_dirty_)
    this->meta_dirty_ms_ = millis();
  this->meta_dirty_ = true;
}

void FingerprintGrowPlusComponent::request_change_round_(uint32_t delay_ms) {
  if (this->sync_after_change_)
    this->request_round_(delay_ms);
}

void FingerprintGrowPlusComponent::request_periodic_round_() {
  if (this->sync_interval_ms_ != 0)
    this->request_round_(this->sync_interval_ms_);
}

void FingerprintGrowPlusComponent::request_round_(uint32_t delay_ms) {
  uint32_t due = millis() + delay_ms;
  if (!this->round_requested_ || (int32_t) (due - this->round_due_ms_) < 0)
    this->round_due_ms_ = due;
  this->round_requested_ = true;
}

void FingerprintGrowPlusComponent::set_status_(const char *status) {
  if (strcmp(this->status_, status) == 0)
    return;
  this->status_ = status;
  ESP_LOGD(TAG, "Status: %s", status);
#ifdef USE_TEXT_SENSOR
  if (this->status_text_sensor_ != nullptr)
    this->status_text_sensor_->publish_state(status);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->syncing_binary_sensor_ != nullptr)
    this->syncing_binary_sensor_->publish_state(this->round_active_);
#endif
}

uint16_t FingerprintGrowPlusComponent::conflict_count() const {
  uint16_t n = 0;
  for (bool c : this->conflict_)
    n += c ? 1 : 0;
  return n;
}

#ifdef USE_TEXT_SENSOR
void FingerprintGrowPlusComponent::publish_occupied_() {
  // HA text states are limited to 255 characters; ranges keep the list short.
  static constexpr size_t MAX_LEN = 250;
  std::string out;
  uint16_t slot = 0;
  while (slot < this->capacity_) {
    if (!this->local_present_[slot]) {
      slot++;
      continue;
    }
    uint16_t end = slot;
    while (end + 1 < this->capacity_ && this->local_present_[end + 1])
      end++;
    char part[16];
    if (end == slot) {
      snprintf(part, sizeof(part), "%s%u", out.empty() ? "" : ", ", slot);
    } else {
      snprintf(part, sizeof(part), "%s%u-%u", out.empty() ? "" : ", ", slot, end);
    }
    if (out.size() + strlen(part) > MAX_LEN) {
      out += ", ...";
      break;
    }
    out += part;
    slot = end + 1;
  }
  if (out.empty())
    out = "-";
  if (!this->occupied_text_sensor_->has_state() || this->occupied_text_sensor_->state != out) {
    ESP_LOGD(TAG, "Occupied slots: %s", out.c_str());
    this->occupied_text_sensor_->publish_state(out);
  }
}
#endif

void FingerprintGrowPlusComponent::publish_diagnostics_() {
  const uint16_t left = MAX_VERSION > this->clock_ ? MAX_VERSION - this->clock_ : 0;
  if (left < 1000 && !this->versions_low_warned_ && this->network_) {
    this->versions_low_warned_ = true;
    ESP_LOGW(TAG, "Only %u sync versions left; plan a version reset (docs/protocol.md)", left);
  }
#ifdef USE_SENSOR
  if (this->versions_left_sensor_ != nullptr &&
      (!this->versions_left_sensor_->has_state() || this->versions_left_sensor_->state != left)) {
    ESP_LOGD(TAG, "Sync versions left: %u", left);
    this->versions_left_sensor_->publish_state(left);
  }
  if (this->error_count_sensor_ != nullptr)
    this->error_count_sensor_->publish_state(this->error_count_);
  if (this->conflict_count_sensor_ != nullptr)
    this->conflict_count_sensor_->publish_state(this->conflict_count());
#endif
}

bool FingerprintGrowPlusComponent::is_self_(const Peer &peer) const {
  const std::string &name = App.get_name();
  std::string host = peer.host;
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  std::string lname = name;
  std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return std::tolower(c); });
  return host == lname || host == lname + ".local";
}

bool FingerprintGrowPlusComponent::sensor_available_() {
  if (this->grow_->is_failed()) {
    if (!this->failed_sensor_) {
      ESP_LOGE(TAG, "fingerprint_grow reports a failed sensor; sync paused");
      this->failed_sensor_ = true;
      this->set_status_("sensor error");
    }
    return false;
  }
  if (this->enrolling_)
    return false;
  if (this->finger_sensor_ != nullptr && this->finger_sensor_->state) {
    const uint32_t now = millis();
    if (this->finger_since_ms_ == 0)
      this->finger_since_ms_ = now;
    if (!elapsed(now, this->finger_since_ms_, FINGER_STUCK_MS))
      return false;
    if (!this->finger_stuck_logged_) {
      ESP_LOGW(TAG, "Finger sensor reports a finger for more than %" PRIu32 " s; treating it as stuck",
               FINGER_STUCK_MS / 1000);
      this->finger_stuck_logged_ = true;
    }
  } else {
    this->finger_since_ms_ = 0;
    this->finger_stuck_logged_ = false;
  }
  if (!elapsed(millis(), this->last_activity_ms_, ACTIVITY_HOLDOFF_MS))
    return false;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Job queue
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::queue_job_(Job &&job) {
  // Jobs without data for the same slot are idempotent; never queue them twice. This keeps the
  // queue bounded when the sensor is blocked (enrollment, finger on the sensor) for a long time.
  if (job.type == JobType::DELETE || job.type == JobType::HASH) {
    for (const auto &queued : this->jobs_) {
      if (queued.type == job.type && queued.slot == job.slot && queued.flags == job.flags &&
          queued.meta == job.meta) {
        if (job.type == JobType::HASH && this->pending_hash_ > 0)
          this->pending_hash_--;  // the caller counted this duplicate
        return;
      }
    }
  }
  if (this->jobs_.size() >= JOB_QUEUE_LIMIT) {
    ESP_LOGE(TAG, "Job queue full (%u), dropping a job; the next round retries", (unsigned) this->jobs_.size());
    this->error_count_++;
    if (job.type == JobType::HASH && this->pending_hash_ > 0)
      this->pending_hash_--;
    Session *s = this->session_by_gen_(job.session);
    if (s != nullptr && (job.type == JobType::WRITE) && s->jobs_pending > 0)
      s->jobs_pending--;
    if (s != nullptr && job.type == JobType::SERVE && s->serve_pending > 0)
      s->serve_pending--;
    return;
  }
  this->jobs_.push_back(std::move(job));
}

void FingerprintGrowPlusComponent::queue_simple_job_(JobType type, uint16_t slot, uint8_t flags) {
  Job job;
  job.type = type;
  job.slot = slot;
  job.flags = flags;
  this->queue_job_(std::move(job));
}

void FingerprintGrowPlusComponent::run_next_job_() {
  if (this->jobs_.empty() || !this->sensor_available_())
    return;
  Job job = std::move(this->jobs_.front());
  this->jobs_.pop_front();
  switch (job.type) {
    case JobType::REFRESH_INDEX:
      this->refresh_queued_ = false;
      this->job_refresh_index_();
      break;
    case JobType::HASH:
      this->job_hash_(job);
      break;
    case JobType::SERVE:
      this->job_serve_(job);
      break;
    case JobType::WRITE:
      this->job_write_(job);
      break;
    case JobType::DELETE:
      this->job_delete_(job);
      break;
    case JobType::COUNT:
      this->count_queued_ = false;
      this->job_count_();
      break;
    case JobType::SAVE_META:
      this->save_queued_ = false;
      this->job_save_meta_();
      break;
      break;
  }
  if (!job.data.empty())
    crypto::wipe(job.data.data(), job.data.size());
}

void FingerprintGrowPlusComponent::finish_indexing_if_done_() {
  if (this->ready_ || !this->params_loaded_ || this->pending_hash_ != 0 || this->refresh_queued_)
    return;
  this->ready_ = true;
  this->indexed_once_ = true;
  uint16_t present = 0, synced = 0;
  for (uint16_t s = 0; s < this->capacity_; s++) {
    present += this->local_present_[s] ? 1 : 0;
    synced += (this->managed_(s) && meta_present(this->meta_[s])) ? 1 : 0;
  }
  if (this->round_active_) {
    ESP_LOGD(TAG, "Index refreshed: %u templates (%u synced), clock %u", present, synced, this->clock_);
    return;
  }
  ESP_LOGI(TAG, "Index ready: %u templates (%u synced), capacity %u, clock %u", present, synced, this->capacity_,
           this->clock_);
  this->set_status_(this->dry_run_ ? "idle (dry run)" : "idle");
  if (!this->round_requested_ && this->sync_at_boot_)
    this->request_round_(1000);
}

// ---------------------------------------------------------------------------------------------
// Jobs (each one is a complete set of sensor transactions inside a single loop() call)
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::job_refresh_index_() {
  if (!this->params_loaded_) {
    // Read the parameters twice and only accept identical, plausible answers: a module that
    // is still busy with an earlier transfer must never shrink the managed slot range.
    GrowParams params, again;
    uint8_t res = this->link_.read_params(params);
    uint8_t raw[16];
    memcpy(raw, this->link_.last_params_raw(), sizeof(raw));
    if (res == GROW_OK)
      res = this->link_.read_params(again);
    bool consistent = res == GROW_OK && memcmp(raw, this->link_.last_params_raw(), sizeof(raw)) == 0;
    char raw_hex[format_hex_pretty_size(sizeof(raw))];
    ESP_LOGD(TAG, "ReadSysPara raw: %s", format_hex_pretty_to(raw_hex, raw, sizeof(raw)));
    if (!consistent || params.capacity == 0 || params.capacity > 3000 || params.security_level < 1 ||
        params.security_level > 5) {
      ESP_LOGW(TAG, "Sensor parameters unreliable (0x%02X, capacity %u), retrying later", res, params.capacity);
      this->error_count_++;
      this->retry_ms_ = millis();
      this->publish_diagnostics_();
      return;
    }
    uint16_t count = 0;
    if (this->link_.template_count(count) != GROW_OK)
      count = 0;
    uint16_t sensor_capacity = params.capacity;
    if (this->capacity_sensor_ != nullptr && this->capacity_sensor_->has_state() &&
        this->capacity_sensor_->state >= 1 && this->capacity_sensor_->state <= 3000) {
      // fingerprint_grow searches slots 0..capacity-1 with the value it read at boot; use the
      // same range so every slot that can match is synced.
      uint16_t grow_capacity = (uint16_t) this->capacity_sensor_->state;
      if (grow_capacity != params.capacity)
        ESP_LOGW(TAG, "ReadSysPara reports capacity %u, fingerprint_grow uses %u; using %u", params.capacity,
                 grow_capacity, grow_capacity);
      sensor_capacity = grow_capacity;
    }
    if (sensor_capacity < count) {
      ESP_LOGW(TAG, "Capacity %u is below the %u stored templates; retrying later", sensor_capacity, count);
      this->error_count_++;
      this->retry_ms_ = millis();
      this->publish_diagnostics_();
      return;
    }
    uint16_t capacity = std::min<uint16_t>(sensor_capacity, MAX_CAPACITY);
    if (capacity > this->storage_->max_slots()) {
      ESP_LOGW(TAG, "Metadata storage (%s) holds %u slots only; slots above are not synced", this->storage_->name(),
               (unsigned) this->storage_->max_slots());
      capacity = (uint16_t) this->storage_->max_slots();
    }
    this->capacity_ = capacity;
    this->meta_.assign(capacity, 0);
    this->hash_.assign(capacity, {});
    this->hash_known_.assign(capacity, false);
    this->local_present_.assign(capacity, false);
    this->remote_present_.assign(capacity, false);
    this->conflict_.assign(capacity, false);
    this->conflict_reported_.assign(capacity, false);
    if (!this->storage_->load(this->meta_)) {
      ESP_LOGW(TAG, "Loading sync metadata failed, retrying later");
      this->error_count_++;
      this->retry_ms_ = millis();
      return;
    }
    this->load_expiries_();
    for (uint16_t m : this->meta_)
      this->clock_ = std::max(this->clock_, meta_version(m));
    this->params_loaded_ = true;
    ESP_LOGI(TAG, "Sensor: capacity %u (ReadSysPara %u), %u templates, security level %u, packet size %u bytes",
             capacity, params.capacity, count, params.security_level, params.packet_size);
  }

  const uint8_t pages = (uint8_t) ((this->capacity_ + 255) / 256);
  std::vector<uint8_t> index(pages * 32, 0);
  for (uint8_t page = 0; page < pages; page++) {
    uint8_t res = this->link_.read_index_page(page, index.data() + page * 32);
    if (res != GROW_OK) {
      ESP_LOGW(TAG, "Reading index table failed (0x%02X), retrying later", res);
      this->error_count_++;
      this->retry_ms_ = millis();
      this->publish_diagnostics_();
      return;
    }
  }

  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    // Index table: byte n covers slots 8n..8n+7, bit k is slot 8n+k (R503 manual, ReadIndexTable).
    bool present = (index[slot / 8] >> (slot % 8)) & 1;
    this->local_present_[slot] = present;
    if (!this->managed_(slot))
      continue;
    uint16_t m = this->meta_[slot];
    SlotState state = meta_state(m);
    if (present) {
      if (state == SLOT_NONE) {
        // Template that the sync has never seen (enrolled before the sync existed or while
        // this component was not running). It joins with version 0.
        this->meta_[slot] = meta_pack(SLOT_PRESENT, 0);
        this->hash_known_[slot] = false;
        this->mark_dirty_();
      } else if (state == SLOT_TOMBSTONE) {
        ESP_LOGW(TAG, "Slot %u is deleted cluster-wide but still occupied here; deleting it again", slot);
        Job job;
        job.type = JobType::DELETE;
        job.slot = slot;
        job.meta = m;
        job.flags = JOB_FLAG_REMOTE;
        this->queue_job_(std::move(job));
        continue;
      }
      if (!this->hash_known_[slot]) {
        this->pending_hash_++;  // counted before queueing: queue_job_ undoes it for a duplicate
        this->queue_simple_job_(JobType::HASH, slot);
      }
    } else if (meta_present(m)) {
      ESP_LOGI(TAG, "Slot %u is missing on this sensor; it will be restored from peers", slot);
      this->meta_[slot] = meta_pack(SLOT_NONE, 0);
      this->hash_known_[slot] = false;
      this->conflict_[slot] = false;
      this->mark_dirty_();
    }
  }
  this->finish_indexing_if_done_();
}

void FingerprintGrowPlusComponent::job_hash_(Job &job) {
  const uint16_t slot = job.slot;
  if (this->pending_hash_ > 0)
    this->pending_hash_--;
  std::vector<uint8_t> data;
  uint8_t res = this->link_.read_template(slot, data);
  if (res != GROW_OK) {
    ESP_LOGW(TAG, "Reading template of slot %u failed at %s (0x%02X, %u answer bytes)", slot, this->link_.last_step(),
             res, (unsigned) this->link_.last_rx_bytes());
    this->error_count_++;
    this->publish_diagnostics_();
    this->finish_indexing_if_done_();
    return;
  }
  uint8_t digest[32];
  crypto::sha256(data.data(), data.size(), digest);
  ESP_LOGD(TAG, "Slot %u indexed (template %u bytes)", slot, (unsigned) data.size());
  if (this->template_size_ != data.size()) {
    if (this->template_size_ != 0)
      ESP_LOGW(TAG, "This sensor returned templates of %u and %u bytes", this->template_size_, (unsigned) data.size());
    this->template_size_ = (uint16_t) data.size();
  }
  crypto::wipe(data.data(), data.size());
  memcpy(this->hash_[slot].data(), digest, HASH_SIZE);
  this->hash_known_[slot] = true;
  this->local_present_[slot] = true;

  if (job.flags & JOB_FLAG_ENROLLED) {
    bool was_present = meta_present(this->meta_[slot]);
    if (slot == this->pending_guest_slot_) {
      this->set_expiry_(slot, this->pending_guest_expiry_);
      this->pending_guest_slot_ = NO_SLOT;
    } else if (!was_present) {
      this->set_expiry_(slot, 0);  // a re-enrolled guest keeps its expiry
    }
    // Enrolling over an occupied slot is a deliberate replacement and may overwrite peers.
    this->meta_[slot] = meta_pack(was_present ? SLOT_PRESENT_FORCED : SLOT_PRESENT, this->next_version_());
    this->conflict_[slot] = false;
    this->mark_dirty_();
    ESP_LOGI(TAG, "Slot %u enrolled (version %u%s), propagating", slot, meta_version(this->meta_[slot]),
             was_present ? ", replaces previous finger" : "");
    if (job.flags & JOB_FLAG_PROPAGATE)
      this->request_change_round_(PUSH_DELAY_MS);
  }
  this->finish_indexing_if_done_();
}

void FingerprintGrowPlusComponent::job_serve_(Job &job) {
  Session *s = this->session_by_gen_(job.session);
  if (s != nullptr && s->serve_pending > 0)
    s->serve_pending--;
  if (s == nullptr)
    return;
  const uint16_t slot = job.slot;
  // Synced slots go to every authenticated peer. Local-only slots only to the backup tool, and
  // only after it asked for them during a full-backup window (MSG_LOCAL_LIST_REQ).
  const bool synced = this->managed_(slot) && meta_present(this->meta_[slot]) && this->hash_known_[slot];
  const bool local = s->local_ok && slot < this->capacity_ && !this->managed_(slot) && this->local_present_[slot];
  if (!synced && !local) {
    this->send_simple_(*s, MSG_TEMPLATE_NAK, slot, 1, true);
    return;
  }
  std::vector<uint8_t> data;
  uint8_t res = this->link_.read_template(slot, data);
  if (res != GROW_OK) {
    ESP_LOGW(TAG, "Reading template of slot %u for peer failed (0x%02X)", slot, res);
    this->error_count_++;
    this->send_simple_(*s, MSG_TEMPLATE_NAK, slot, 2, true);
    return;
  }
  uint8_t digest[32];
  crypto::sha256(data.data(), data.size(), digest);
  if (synced && memcmp(digest, this->hash_[slot].data(), HASH_SIZE) != 0) {
    // The module returned different bytes for the same slot (see docs: assumption A2).
    ESP_LOGW(TAG, "Template bytes of slot %u changed since indexing; updating the hash", slot);
    memcpy(this->hash_[slot].data(), digest, HASH_SIZE);
  }
  std::vector<uint8_t> msg;
  MsgWriter w(msg);
  w.u8(MSG_TEMPLATE);
  w.u16(slot);
  w.u16(synced ? this->meta_[slot] : 0);
  w.bytes(digest, HASH_SIZE);
  w.u32(this->expires_[slot]);
  w.u16((uint16_t) data.size());
  w.bytes(data.data(), data.size());
  crypto::wipe(data.data(), data.size());
  this->send_message_(*s, msg);
  crypto::wipe(msg.data(), msg.size());
  s->served++;
  if (!synced)
    s->served_local++;
}

bool FingerprintGrowPlusComponent::store_verified_(uint8_t buffer, uint16_t slot, const std::vector<uint8_t> &data) {
  // Precondition: CharBuffer `buffer` holds `data`.
  uint8_t res = this->link_.store(buffer, slot);
  if (res != GROW_OK) {
    ESP_LOGE(TAG, "Storing slot %u failed at %s (0x%02X, %u answer bytes)", slot, this->link_.last_step(), res,
             (unsigned) this->link_.last_rx_bytes());
    return false;
  }
  // Verification: read the stored slot back and compare it byte for byte with what was sent.
  const uint8_t other = buffer == 1 ? 2 : 1;
  std::vector<uint8_t> readback;
  res = this->link_.read_template(slot, readback, other);
  bool ok = false;
  if (res == GROW_OK) {
    ok = readback.size() == data.size() && crypto::equal_ct(readback.data(), data.data(), data.size());
    if (!ok)
      ESP_LOGW(TAG, "Slot %u read back differs from the template sent (%u vs %u bytes)", slot,
               (unsigned) readback.size(), (unsigned) data.size());
    crypto::wipe(readback.data(), readback.size());
  }
  if (!ok) {
    ESP_LOGE(TAG, "Verification of slot %u failed; removing the unverified template", slot);
    this->link_.delete_char(slot, 1);
  }
  return ok;
}

void FingerprintGrowPlusComponent::apply_stored_(uint16_t slot, uint16_t meta, const uint8_t *hash, uint32_t expires) {
  this->meta_[slot] = meta;
  this->set_expiry_(slot, expires);
  memcpy(this->hash_[slot].data(), hash, HASH_SIZE);
  this->hash_known_[slot] = true;
  this->local_present_[slot] = true;
  this->conflict_[slot] = false;
  this->mark_dirty_();
  if (!this->count_queued_) {
    this->count_queued_ = true;
    this->queue_simple_job_(JobType::COUNT);
  }
  this->slot_updated_cb_.call(slot);
}

void FingerprintGrowPlusComponent::job_write_(Job &job) {
  Session *s = this->session_by_gen_(job.session);
  if (s != nullptr && s->jobs_pending > 0)
    s->jobs_pending--;
  const uint16_t slot = job.slot;
  const bool restore = job.flags & JOB_FLAG_RESTORE;

  if (this->dry_run_ && !restore) {
    ESP_LOGI(TAG, "[dry run] would %s slot %u (peer version %u)",
             (job.flags & JOB_FLAG_REPLACE) ? "REPLACE" : "store", slot, meta_version(job.meta));
    return;
  }

  uint8_t res = this->link_.down_char(1, job.data);
  bool ok = res == GROW_OK && this->store_verified_(1, slot, job.data);
  if (res != GROW_OK)
    ESP_LOGE(TAG, "Downloading template for slot %u failed at %s (0x%02X, %u answer bytes, template %u bytes)", slot,
             this->link_.last_step(), res, (unsigned) this->link_.last_rx_bytes(), (unsigned) job.data.size());

  if (restore && ok && !this->managed_(slot)) {
    // Local-only slot: no version, nothing to propagate.
    this->local_present_[slot] = true;
    this->set_expiry_(slot, job.expires);
    if (!this->count_queued_) {
      this->count_queued_ = true;
      this->queue_simple_job_(JobType::COUNT);
    }
    this->slot_updated_cb_.call(slot);
    ESP_LOGI(TAG, "Slot %u restored (local only)", slot);
    if (s != nullptr)
      this->send_simple_(*s, MSG_RESTORE_ACK, slot, 0, true);
    return;
  }
  uint16_t meta = job.meta;
  if (restore) {
    meta = meta_pack(SLOT_PRESENT_FORCED, this->next_version_());
  }
  if (ok) {
    this->apply_stored_(slot, meta, job.hash.data(), job.expires);
    ESP_LOGI(TAG, "Slot %u %s (version %u)", slot,
             restore ? "restored" : ((job.flags & JOB_FLAG_REPLACE) ? "replaced from peer" : "received from peer"),
             meta_version(meta));
    if (s != nullptr)
      s->pulled++;
    if (restore)
      this->request_change_round_(PUSH_DELAY_MS);
  } else {
    this->error_count_++;
    this->local_present_[slot] = false;
    if (meta_present(this->meta_[slot]) && (job.flags & JOB_FLAG_REPLACE)) {
      // The previous template is gone as well; let a later round fetch it again.
      this->meta_[slot] = meta_pack(SLOT_NONE, 0);
      this->hash_known_[slot] = false;
      this->mark_dirty_();
    }
    this->publish_diagnostics_();
  }
  if (restore && s != nullptr)
    this->send_simple_(*s, MSG_RESTORE_ACK, slot, ok ? 0 : 1, true);
}

void FingerprintGrowPlusComponent::flag_conflict_(uint16_t slot) {
  if (this->conflict_[slot])
    return;
  this->conflict_[slot] = true;
  if (this->conflict_reported_[slot])
    return;  // still the same conflict as in the previous round
  this->conflict_reported_[slot] = true;
  ESP_LOGW(TAG, "CONFLICT in slot %u: a peer stores a different template in this slot. Nothing was "
           "overwritten; use fingerprint_grow_plus.resolve_conflict or resolve_all_conflicts.", slot);
  this->conflict_cb_.call(slot);
  this->publish_diagnostics_();
}

void FingerprintGrowPlusComponent::job_delete_(Job &job) {
  const uint16_t slot = job.slot;
  const bool remote = job.flags & JOB_FLAG_REMOTE;
  if (this->dry_run_ && remote) {
    ESP_LOGI(TAG, "[dry run] would delete slot %u (tombstone version %u)", slot, meta_version(job.meta));
    return;
  }
  uint8_t res = this->link_.delete_char(slot, 1);
  if (res != GROW_OK) {
    ESP_LOGE(TAG, "Deleting slot %u failed (0x%02X)", slot, res);
    this->error_count_++;
    this->publish_diagnostics_();
    return;
  }
  if (slot < this->capacity_) {
    this->local_present_[slot] = false;
    this->set_expiry_(slot, 0);
    if (this->managed_(slot)) {
      uint16_t meta = job.meta;
      if (job.flags & JOB_FLAG_LOCAL)
        meta = meta_pack(SLOT_TOMBSTONE, this->next_version_());
      this->meta_[slot] = meta;
      this->hash_known_[slot] = false;
      this->conflict_[slot] = false;
      this->mark_dirty_();
    }
  }
  ESP_LOGI(TAG, "Slot %u deleted%s", slot, remote ? " (deleted on a peer)" : "");
  if (!this->count_queued_) {
    this->count_queued_ = true;
    this->queue_simple_job_(JobType::COUNT);
  }
  this->slot_deleted_cb_.call(slot);
  if (job.flags & JOB_FLAG_PROPAGATE)
    this->request_change_round_(PUSH_DELAY_MS);
}

void FingerprintGrowPlusComponent::job_count_() {
  uint16_t count = 0;
  if (this->link_.template_count(count) == GROW_OK && this->count_sensor_ != nullptr)
    this->count_sensor_->publish_state(count);
}

void FingerprintGrowPlusComponent::claim_all_slots() {
  if (!this->indexed_once_) {
    ESP_LOGW(TAG, "claim_all_slots refused: index not ready yet");
    return;
  }
  uint16_t n = 0;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    if (!this->managed_(slot) || !meta_present(this->meta_[slot]))
      continue;
    this->meta_[slot] = meta_pack(SLOT_PRESENT_FORCED, this->next_version_());
    this->conflict_[slot] = false;
    n++;
  }
  this->mark_dirty_();
  ESP_LOGW(TAG, "This node claims %u slot(s): peers replace their templates in these slots with this node's", n);
  this->publish_diagnostics_();
  this->request_change_round_(PUSH_DELAY_MS);
}

void FingerprintGrowPlusComponent::resolve_all_conflicts(bool keep_local) {
  if (!this->params_loaded_)
    return;
  uint16_t n = 0;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    if (this->conflict_[slot]) {
      this->resolve_conflict(slot, keep_local);
      n++;
    }
  }
  ESP_LOGI(TAG, "%u conflict(s) resolved (%s)", n, keep_local ? "this node's fingers win" : "peers' fingers win");
}

void FingerprintGrowPlusComponent::job_save_meta_() {
  this->save_expiries_();
  if (!this->storage_->save(this->meta_)) {
    this->error_count_++;
    this->meta_dirty_ = true;  // retry
    this->meta_dirty_ms_ = millis();
    this->publish_diagnostics_();
  }
}

// ---------------------------------------------------------------------------------------------
// Public actions
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::sync_now() {
  ESP_LOGI(TAG, "Manual sync requested");
  this->request_round_(0);
}

void FingerprintGrowPlusComponent::delete_slot(uint16_t slot) {
  if (!this->params_loaded_ || slot >= this->capacity_) {
    ESP_LOGW(TAG, "Cannot delete slot %u (sensor not indexed yet or slot out of range)", slot);
    return;
  }
  if (this->is_admin_slot(slot)) {
    ESP_LOGW(TAG, "Slot %u is an admin finger and protected; use fingerprint_grow_plus.delete_admin", slot);
    return;
  }
  Job job;
  job.type = JobType::DELETE;
  job.slot = slot;
  job.flags = JOB_FLAG_LOCAL | (this->managed_(slot) ? JOB_FLAG_PROPAGATE : 0);
  this->queue_job_(std::move(job));
}

void FingerprintGrowPlusComponent::delete_all(const std::string &code) {
  if (this->delete_all_code_ == nullptr || this->delete_all_code_[0] == '\0' || code != this->delete_all_code_) {
    ESP_LOGW(TAG, "delete_all refused: wrong or unconfigured code");
    return;
  }
  if (!this->params_loaded_)
    return;
  ESP_LOGW(TAG, "Deleting ALL fingers except the admin fingers on this node, synced slots also on all peers");
  uint16_t queued = 0;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    if (this->is_admin_slot(slot))
      continue;  // admin fingers survive delete_all
    if (!this->managed_(slot)) {
      // Local-only slot (outside include/exclude): delete it here, nothing to propagate.
      if (this->local_present_[slot]) {
        Job job;
        job.type = JobType::DELETE;
        job.slot = slot;
        job.flags = JOB_FLAG_LOCAL;
        this->queue_job_(std::move(job));
        queued++;
      }
      continue;
    }
    if (!this->local_present_[slot] && !meta_present(this->meta_[slot])) {
      // Nothing here, but peers may still have it: write a tombstone without a sensor call.
      this->meta_[slot] = meta_pack(SLOT_TOMBSTONE, this->next_version_());
      this->mark_dirty_();
      continue;
    }
    Job job;
    job.type = JobType::DELETE;
    job.slot = slot;
    job.flags = JOB_FLAG_LOCAL;
    this->queue_job_(std::move(job));
    queued++;
  }
  ESP_LOGI(TAG, "%u deletions queued", queued);
  this->request_change_round_(PUSH_DELAY_MS + 1000u * queued);
}

int32_t FingerprintGrowPlusComponent::next_free_slot() const {
  return this->next_free_in_(this->enroll_from_, this->enroll_to_, false);
}

void FingerprintGrowPlusComponent::enroll_next_free(uint8_t num_scans) {
  if (!this->indexed_once_) {
    ESP_LOGW(TAG, "Enrollment refused: index not ready yet");
    return;
  }
  if (this->enrolling_) {
    ESP_LOGW(TAG, "Enrollment refused: an enrollment is already running");
    return;
  }
  int32_t slot = this->next_free_slot();
  if (slot < 0) {
    ESP_LOGE(TAG, "Enrollment refused: no free slot in the enroll range");
    return;
  }
  ESP_LOGI(TAG, "Enrolling next free slot %" PRId32 " with %u scans", slot, num_scans);
  this->enroll_started_cb_.call((uint16_t) slot);
  this->grow_->enroll_fingerprint((uint16_t) slot, num_scans);
}

void FingerprintGrowPlusComponent::resolve_conflict(uint16_t slot, bool keep_local) {
  if (!this->params_loaded_ || !this->managed_(slot)) {
    ESP_LOGW(TAG, "resolve_conflict: slot %u is not a managed slot", slot);
    return;
  }
  if (keep_local) {
    if (!meta_present(this->meta_[slot])) {
      ESP_LOGW(TAG, "resolve_conflict: slot %u is empty here, nothing to keep", slot);
      return;
    }
    this->meta_[slot] = meta_pack(SLOT_PRESENT_FORCED, this->next_version_());
    this->conflict_[slot] = false;
    this->mark_dirty_();
    ESP_LOGI(TAG, "Slot %u: keeping this node's finger; peers will replace theirs", slot);
    this->request_change_round_(PUSH_DELAY_MS);
  } else {
    // Drop the local template without a tombstone; the next round fetches the peer's finger.
    Job job;
    job.type = JobType::DELETE;
    job.slot = slot;
    job.meta = meta_pack(SLOT_NONE, 0);
    job.flags = JOB_FLAG_PROPAGATE;
    this->queue_job_(std::move(job));
    ESP_LOGI(TAG, "Slot %u: taking the peer's finger", slot);
  }
  this->publish_diagnostics_();
}

void FingerprintGrowPlusComponent::allow_full_backup(uint32_t seconds) {
  if (!this->network_) {
    ESP_LOGW(TAG, "allow_full_backup: needs the sync: block (the backup tool connects over the network)");
    return;
  }
  this->full_backup_until_ms_ = millis() + seconds * 1000;
  this->full_backup_open_ = seconds > 0;
  ESP_LOGW(TAG, "Full backup window %s for %" PRIu32 " s (local-only slots readable by the backup tool)",
           this->full_backup_open_ ? "OPEN" : "closed", seconds);
}

void FingerprintGrowPlusComponent::send_local_list_(Session &s) {
  const bool open = this->full_backup_open_ && (int32_t) (this->full_backup_until_ms_ - millis()) > 0;
  std::vector<uint16_t> slots;
  if (open && !s.is_client) {
    for (uint16_t slot = 0; slot < this->capacity_; slot++) {
      if (!this->managed_(slot) && this->local_present_[slot])
        slots.push_back(slot);
    }
    s.local_ok = true;
    ESP_LOGW(TAG, "Full backup for %08" PRIX32 ": %u local-only slot(s) offered in addition to the synced slots",
             s.channel.peer_id(), (unsigned) slots.size());
  } else {
    ESP_LOGW(TAG, "Local slot list refused (full backup window closed)");
  }
  size_t pages = std::max<size_t>(1, (slots.size() + LOCAL_LIST_ENTRIES_PER_PAGE - 1) / LOCAL_LIST_ENTRIES_PER_PAGE);
  std::vector<uint8_t> msg;
  for (size_t page = 0; page < pages; page++) {
    size_t first = page * LOCAL_LIST_ENTRIES_PER_PAGE;
    size_t count = std::min(LOCAL_LIST_ENTRIES_PER_PAGE, slots.size() - std::min(first, slots.size()));
    MsgWriter w(msg);
    w.u8(MSG_LOCAL_LIST);
    w.u8((uint8_t) page);
    w.u8((page + 1 == pages ? 1 : 0) | (s.local_ok ? 0 : 2));
    w.u16((uint16_t) count);
    for (size_t i = 0; i < count; i++) {
      w.u16(slots[first + i]);
      w.u32(this->expires_[slots[first + i]]);
    }
    this->send_message_(s, msg);
  }
}

void FingerprintGrowPlusComponent::allow_restore(uint32_t seconds) {
  if (!this->network_) {
    ESP_LOGW(TAG, "allow_restore: needs the sync: block (restore runs over the network)");
    return;
  }
  this->restore_until_ms_ = millis() + seconds * 1000;
  this->restore_open_ = seconds > 0;
  ESP_LOGW(TAG, "Restore window %s for %" PRIu32 " s", this->restore_open_ ? "OPEN" : "closed", seconds);
}

// ---------------------------------------------------------------------------------------------
// Sync decisions
// ---------------------------------------------------------------------------------------------

FingerprintGrowPlusComponent::Decision FingerprintGrowPlusComponent::decide_(uint16_t slot, uint16_t remote,
                                                                     const uint8_t *remote_hash) const {
  Decision d;
  const uint16_t local = this->meta_[slot];
  const SlotState ls = meta_state(local), rs = meta_state(remote);
  const uint16_t lv = meta_version(local), rv = meta_version(remote);

  if (rs == SLOT_NONE)
    return d;

  if (rs == SLOT_TOMBSTONE) {
    if (meta_present(local)) {
      if (rv > lv) {
        d.kind = Decision::DELETE;
        d.meta = remote;
      }
    } else if (ls == SLOT_NONE || rv > lv) {
      if (ls != SLOT_TOMBSTONE || rv > lv) {
        d.kind = Decision::ADOPT;
        d.meta = remote;
      }
    }
    return d;
  }

  // Remote slot is present.
  if (ls == SLOT_NONE) {
    d.kind = Decision::PULL;
    return d;
  }
  if (ls == SLOT_TOMBSTONE) {
    if (rv > lv)
      d.kind = Decision::PULL;
    return d;
  }
  // Both present.
  if (!this->hash_known_[slot])
    return d;
  if (memcmp(this->hash_[slot].data(), remote_hash, HASH_SIZE) == 0) {
    if (rv > lv || (rv == lv && meta_forced(remote) && !meta_forced(local))) {
      d.kind = Decision::ADOPT;
      d.meta = remote;
    }
    return d;
  }
  if (this->conflict_[slot] && !meta_forced(remote))
    return d;  // already reported; waits for resolve_conflict
  if (meta_forced(remote) && rv > lv) {
    d.kind = Decision::PULL;  // explicit replacement
    return d;
  }
  if (meta_forced(local) && lv > rv)
    return d;  // our replacement wins; the peer pulls it
  // Different template bytes in the same slot. Whether both belong to the same finger cannot be
  // decided reliably (the module's template-to-template Match returns arbitrary results on real
  // R503 modules), so this is always a conflict that the user resolves.
  d.kind = Decision::CONFLICT;
  return d;
}

void FingerprintGrowPlusComponent::process_manifest_entry_(Session &s, uint16_t slot, uint16_t meta,
                                                       const uint8_t *hash, uint32_t expires) {
  if (slot >= this->capacity_ || !this->managed_(slot))
    return;
  this->remote_present_[slot] = this->remote_present_[slot] || meta_present(meta);
  if (meta_version(meta) > this->clock_)
    this->clock_ = meta_version(meta);
  Decision d = this->decide_(slot, meta, hash);
  switch (d.kind) {
    case Decision::PULL:
      s.pull.push_back(slot);
      break;
    case Decision::ADOPT:
      this->meta_[slot] = d.meta;
      this->set_expiry_(slot, meta_present(d.meta) ? expires : 0);
      this->mark_dirty_();
      break;
    case Decision::DELETE: {
      Job job;
      job.type = JobType::DELETE;
      job.slot = slot;
      job.meta = d.meta;
      job.flags = JOB_FLAG_REMOTE;
      this->queue_job_(std::move(job));
      break;
    }
    case Decision::CONFLICT:
      this->flag_conflict_(slot);
      break;
    case Decision::NOTHING:
      // Same template and version: a node that lost the guest expiry (e.g. no persistence) takes
      // it over from the peer, so every node can enforce it.
      if (expires != 0 && meta_present(meta) && meta == this->meta_[slot] && this->expires_[slot] == 0 &&
          this->hash_known_[slot] && memcmp(this->hash_[slot].data(), hash, HASH_SIZE) == 0)
        this->set_expiry_(slot, expires);
      break;
  }
}

bool FingerprintGrowPlusComponent::template_compatible_(Session &s, size_t len) {
  if (this->template_size_ == 0 || len == this->template_size_)
    return true;
  this->error_count_++;
  if (!s.incompatible_logged) {
    s.incompatible_logged = true;
    ESP_LOGE(TAG, "Incompatible template from %08" PRIX32 ": %u bytes, this sensor uses %u bytes (different sensor "
             "model or firmware?); not stored",
             s.channel.peer_id(), (unsigned) len, this->template_size_);
    if (s.is_client)
      this->round_last_error_ = "incompatible sensor";
  }
  return false;
}

void FingerprintGrowPlusComponent::handle_template_(Session &s, uint16_t slot, uint16_t meta, const uint8_t *hash,
                                                uint32_t expires, const uint8_t *data, size_t len) {
  if (!this->template_compatible_(s, len))
    return;
  uint8_t digest[32];
  crypto::sha256(data, len, digest);
  if (memcmp(digest, hash, HASH_SIZE) != 0) {
    ESP_LOGE(TAG, "Template for slot %u failed the checksum check; discarded", slot);
    this->error_count_++;
    return;
  }
  // Decide again: local state may have changed since the manifest arrived.
  Decision d = this->decide_(slot, meta, hash);
  if (d.kind != Decision::PULL) {
    if (d.kind == Decision::ADOPT) {
      this->meta_[slot] = d.meta;
      this->set_expiry_(slot, expires);
      this->mark_dirty_();
    }
    return;
  }
  Job job;
  job.slot = slot;
  job.meta = meta;
  job.expires = expires;
  job.session = s.gen;
  memcpy(job.hash.data(), hash, HASH_SIZE);
  job.data.assign(data, data + len);
  for (const auto &queued : this->jobs_) {
    if (queued.type == JobType::WRITE && queued.slot == slot && queued.hash == job.hash) {
      ESP_LOGD(TAG, "Slot %u: the same template is already queued for writing", slot);
      return;  // e.g. two simultaneous sessions with the same peer delivering the same template
    }
  }
  const uint16_t local = this->meta_[slot];
  if (!meta_present(local)) {
    job.type = JobType::WRITE;
  } else if (meta_forced(meta) && meta_version(meta) > meta_version(local)) {
    job.type = JobType::WRITE;
    job.flags = JOB_FLAG_REPLACE;
    ESP_LOGW(TAG, "Slot %u is replaced by the peer's forced version (resolve_conflict, restore or re-enrollment)",
             slot);
  } else {
    this->flag_conflict_(slot);
    return;
  }
  s.jobs_pending++;
  this->queue_job_(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------

Session *FingerprintGrowPlusComponent::session_by_gen_(uint32_t gen) {
  if (gen == 0)
    return nullptr;
  if (this->inbound_.phase != Session::Phase::IDLE && this->inbound_.gen == gen)
    return &this->inbound_;
  if (this->outbound_.phase != Session::Phase::IDLE && this->outbound_.gen == gen)
    return &this->outbound_;
  return nullptr;
}

static void reset_session(Session &s) {
  s.channel.reset();
  if (!s.tx.empty())
    crypto::wipe(s.tx.data(), s.tx.size());
  s.tx.clear();
  s.tx.shrink_to_fit();
  s.pull.clear();
  s.pull.shrink_to_fit();
  s.pull_pos = 0;
  s.awaiting_template = false;
  s.jobs_pending = 0;
  s.serve_pending = 0;
  s.manifest_received = false;
  s.manifest_entries = 0;
  s.done_sent = false;
  s.done_received = false;
  s.pulled = 0;
  s.served = 0;
  s.served_local = 0;
  s.local_ok = false;  // the full-backup permission belongs to one session only
  s.peer = nullptr;
}

void FingerprintGrowPlusComponent::start_client_session_(const Peer &peer) {
  Session &s = this->outbound_;
  reset_session(s);
  s.is_client = true;
  s.peer = &peer;
  s.stream = this->transport_.outbound();
  s.gen = ++this->session_gen_;
  s.started_ms = millis();
  s.last_rx_ms = s.started_ms;
  s.phase = Session::Phase::CONNECTING;
  ESP_LOGD(TAG, "Connecting to %s:%u", peer.host, peer.port);
  this->transport_.connect(peer.host, peer.port);
}

void FingerprintGrowPlusComponent::start_server_session_() {
  Session &s = this->inbound_;
  reset_session(s);
  s.is_client = false;
  s.stream = this->transport_.inbound();
  s.gen = ++this->session_gen_;
  s.started_ms = millis();
  s.last_rx_ms = s.started_ms;
  s.phase = Session::Phase::HANDSHAKE;
  s.channel.begin_server(this->psk_.data(), this->node_id_);
}

void FingerprintGrowPlusComponent::send_message_(Session &s, const std::vector<uint8_t> &msg) {
  if (!s.channel.seal(msg.data(), msg.size(), s.tx)) {
    ESP_LOGE(TAG, "Message too large or channel closed");
  }
}

void FingerprintGrowPlusComponent::send_simple_(Session &s, uint8_t type, uint16_t slot, uint8_t value, bool with_value) {
  std::vector<uint8_t> msg;
  MsgWriter w(msg);
  w.u8(type);
  w.u16(slot);
  if (with_value)
    w.u8(value);
  this->send_message_(s, msg);
}

void FingerprintGrowPlusComponent::send_manifest_(Session &s) {
  std::vector<uint16_t> slots;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    if (!this->managed_(slot))
      continue;
    uint16_t m = this->meta_[slot];
    SlotState st = meta_state(m);
    if (st == SLOT_NONE)
      continue;
    if (meta_present(m) && !this->hash_known_[slot])
      continue;
    slots.push_back(slot);
  }
  size_t pages = std::max<size_t>(1, (slots.size() + MANIFEST_ENTRIES_PER_PAGE - 1) / MANIFEST_ENTRIES_PER_PAGE);
  std::vector<uint8_t> msg;
  for (size_t page = 0; page < pages; page++) {
    size_t first = page * MANIFEST_ENTRIES_PER_PAGE;
    size_t count = std::min(MANIFEST_ENTRIES_PER_PAGE, slots.size() - std::min(first, slots.size()));
    MsgWriter w(msg);
    w.u8(MSG_MANIFEST);
    w.u8((uint8_t) page);
    w.u8(page + 1 == pages ? 1 : 0);
    w.u16(this->capacity_);
    w.u16((uint16_t) count);
    for (size_t i = 0; i < count; i++) {
      uint16_t slot = slots[first + i];
      w.u16(slot);
      w.u16(this->meta_[slot]);
      static const uint8_t ZERO[HASH_SIZE] = {0};
      w.bytes(meta_present(this->meta_[slot]) ? this->hash_[slot].data() : ZERO, HASH_SIZE);
      w.u32(this->expires_[slot]);
    }
    this->send_message_(s, msg);
  }
}

void FingerprintGrowPlusComponent::handle_message_(Session &s, const std::vector<uint8_t> &msg) {
  if (msg.empty()) {
    this->end_session_(s, false, "empty message");
    return;
  }
  MsgReader r(msg.data() + 1, msg.size() - 1);
  switch (msg[0]) {
    case MSG_MANIFEST: {
      uint8_t page, flags;
      uint16_t capacity, count;
      if (!r.u8(page) || !r.u8(flags) || !r.u16(capacity) || !r.u16(count))
        break;
      if (s.manifest_received) {
        this->end_session_(s, false, "manifest sent twice");
        return;
      }
      s.manifest_entries += count;
      if (s.manifest_entries > (size_t) this->capacity_ + MANIFEST_ENTRIES_PER_PAGE) {
        this->end_session_(s, false, "manifest larger than the slot range");
        return;
      }
      for (uint16_t i = 0; i < count; i++) {
        uint16_t slot, meta;
        uint8_t hash[HASH_SIZE];
        uint32_t expires;
        if (!r.u16(slot) || !r.u16(meta) || !r.bytes(hash, HASH_SIZE) || !r.u32(expires))
          break;
        this->process_manifest_entry_(s, slot, meta, hash, expires);
      }
      if (r.ok() && (flags & 1)) {
        s.manifest_received = true;
        ESP_LOGD(TAG, "Peer %08" PRIX32 ": manifest received, %u slots to fetch", s.channel.peer_id(),
                 (unsigned) s.pull.size());
      }
      break;
    }
    case MSG_TEMPLATE_REQ: {
      uint16_t slot;
      if (!r.u16(slot))
        break;
      if (s.serve_pending >= 4) {
        this->end_session_(s, false, "too many outstanding requests");
        return;
      }
      Job job;
      job.type = JobType::SERVE;
      job.slot = slot;
      job.session = s.gen;
      s.serve_pending++;
      this->queue_job_(std::move(job));
      break;
    }
    case MSG_TEMPLATE: {
      uint16_t slot, meta, len;
      uint8_t hash[HASH_SIZE];
      uint32_t expires;
      if (!r.u16(slot) || !r.u16(meta) || !r.bytes(hash, HASH_SIZE) || !r.u32(expires) || !r.u16(len))
        break;
      const uint8_t *data = r.ptr(len);
      if (data == nullptr || len == 0 || len > GrowLink::MAX_TEMPLATE_SIZE)
        break;
      if (!s.awaiting_template || slot != s.awaiting_slot) {
        this->end_session_(s, false, "unexpected template");
        return;
      }
      s.awaiting_template = false;
      if (slot < this->capacity_ && this->managed_(slot))
        this->handle_template_(s, slot, meta, hash, expires, data, len);
      break;
    }
    case MSG_TEMPLATE_NAK: {
      uint16_t slot;
      uint8_t reason;
      if (!r.u16(slot) || !r.u8(reason))
        break;
      if (s.awaiting_template && slot == s.awaiting_slot)
        s.awaiting_template = false;
      ESP_LOGD(TAG, "Peer could not provide slot %u (reason %u)", slot, reason);
      break;
    }
    case MSG_DONE:
      s.done_received = true;
      break;
    case MSG_LOCK: {
      uint32_t seconds;
      if (!r.u32(seconds))
        break;
      this->handle_remote_lock_(s, seconds);
      break;
    }
    case MSG_LOCAL_LIST_REQ:
      if (s.local_ok) {
        this->end_session_(s, false, "local slot list requested twice");
        return;
      }
      this->send_local_list_(s);
      break;
    case MSG_RESTORE: {
      uint16_t slot, len;
      uint32_t expires;
      if (!r.u16(slot) || !r.u32(expires) || !r.u16(len))
        break;
      const uint8_t *data = r.ptr(len);
      if (data == nullptr || len == 0 || len > GrowLink::MAX_TEMPLATE_SIZE)
        break;
      bool window = this->restore_open_ && (int32_t) (this->restore_until_ms_ - millis()) > 0;
      if (window && !this->template_compatible_(s, len)) {
        this->send_simple_(s, MSG_RESTORE_ACK, slot, 1, true);
        break;
      }
      if (!window || slot >= this->capacity_) {
        ESP_LOGW(TAG, "Restore of slot %u refused (restore window closed or slot out of range)", slot);
        this->send_simple_(s, MSG_RESTORE_ACK, slot, 2, true);
        break;
      }
      if (s.jobs_pending >= 1) {
        this->end_session_(s, false, "restore sent before the previous one was acknowledged");
        return;
      }
      Job job;
      job.type = JobType::WRITE;
      job.slot = slot;
      job.flags = JOB_FLAG_RESTORE;
      job.session = s.gen;
      job.expires = expires;
      uint8_t digest[32];
      crypto::sha256(data, len, digest);
      memcpy(job.hash.data(), digest, HASH_SIZE);
      job.data.assign(data, data + len);
      s.jobs_pending++;
      this->queue_job_(std::move(job));
      break;
    }
    default:
      this->end_session_(s, false, "unknown message type");
      return;
  }
  if (!r.ok())
    this->end_session_(s, false, "malformed message");
}

void FingerprintGrowPlusComponent::end_session_(Session &s, bool ok, const char *reason) {
  if (s.phase == Session::Phase::IDLE)
    return;
  const char *who = s.is_client && s.peer != nullptr ? s.peer->host : "inbound peer";
  if (ok && s.local_ok) {
    ESP_LOGI(TAG, "Full backup for %08" PRIX32 " finished: %u template(s) sent (%u synced, %u local-only)",
             s.channel.peer_id(), s.served, s.served - s.served_local, s.served_local);
  } else if (ok) {
    ESP_LOGI(TAG, "Sync with %s finished: %u received, %u sent", who, s.pulled, s.served);
  } else {
    ESP_LOGW(TAG, "Sync with %s failed: %s", who, reason);
    this->error_count_++;
    if (s.is_client) {
      this->round_last_error_ = std::string(who) + ": " + reason;
      if (s.peer != nullptr) {
        if (!this->round_unreachable_.empty())
          this->round_unreachable_ += ", ";
        this->round_unreachable_ += std::string(s.peer->host) + " (" + reason + ")";
      }
    }
  }
  s.stream->close();
  bool was_client = s.is_client;
  reset_session(s);
  s.phase = Session::Phase::IDLE;
  if (was_client) {
    if (ok) {
      this->round_reachable_++;
    } else {
      this->round_failures_++;
    }
  }
  this->publish_diagnostics_();
}

void FingerprintGrowPlusComponent::service_session_(Session &s) {
  if (s.phase == Session::Phase::IDLE)
    return;
  const uint32_t now = millis();

  if (s.phase == Session::Phase::CONNECTING) {
    if (s.stream->connected()) {
      s.phase = Session::Phase::HANDSHAKE;
      s.last_rx_ms = now;
      s.started_ms = now;  // handshake and session time limits count from the connection
      s.channel.begin_client(this->psk_.data(), this->node_id_, s.tx);
    } else if (elapsed(now, s.started_ms, CONNECT_TIMEOUT_MS)) {
      this->end_session_(s, false, "not reachable");
      return;
    } else {
      return;
    }
  }

  // Receive and process records. read() must be called until it returns 0 (socket contract).
  uint8_t buf[256];
  std::vector<uint8_t> msg;
  while (s.phase != Session::Phase::IDLE) {
    int32_t n = s.stream->read(buf, sizeof(buf));
    if (n < 0) {
      bool ok = s.done_sent && s.done_received;
      this->end_session_(s, ok, "connection closed by peer");
      return;
    }
    if (n == 0)
      break;
    s.last_rx_ms = now;
    if (!s.channel.feed(buf, (size_t) n)) {
      this->end_session_(s, false, s.channel.error());
      return;
    }
    while (s.phase != Session::Phase::IDLE) {
      auto res = s.channel.poll(s.tx, msg);
      if (res == SecureChannel::Result::NONE)
        break;
      if (res == SecureChannel::Result::ERROR) {
        this->end_session_(s, false, s.channel.error());
        return;
      }
      this->handle_message_(s, msg);
    }
  }
  if (!msg.empty())
    crypto::wipe(msg.data(), msg.size());
  if (s.phase == Session::Phase::IDLE)
    return;

  if (s.phase == Session::Phase::HANDSHAKE && s.channel.established()) {
    s.phase = Session::Phase::SYNC;
    ESP_LOGD(TAG, "Authenticated session with node %08" PRIX32, s.channel.peer_id());
    this->send_manifest_(s);
    this->send_pending_lock_(s);
  }

  if (s.phase == Session::Phase::SYNC) {
    if (!s.awaiting_template && s.jobs_pending == 0 && s.pull_pos < s.pull.size() && s.tx.size() < 512) {
      s.awaiting_slot = s.pull[s.pull_pos++];
      s.awaiting_template = true;
      this->send_simple_(s, MSG_TEMPLATE_REQ, s.awaiting_slot, 0, false);
    }
    if (s.manifest_received && !s.done_sent && !s.awaiting_template && s.jobs_pending == 0 &&
        s.pull_pos >= s.pull.size()) {
      std::vector<uint8_t> done{MSG_DONE};
      this->send_message_(s, done);
      s.done_sent = true;
    }
  }

  // Transmit.
  s.stream->flush();
  while (!s.tx.empty()) {
    size_t n = s.stream->write(s.tx.data(), s.tx.size());
    if (n == 0)
      break;
    crypto::wipe(s.tx.data(), n);
    s.tx.erase(s.tx.begin(), s.tx.begin() + n);
    s.stream->flush();
  }
  bool flushed = s.tx.empty() && s.stream->flush();
  if (s.tx.size() > SESSION_TX_LIMIT) {
    this->end_session_(s, false, "peer does not read its data");
    return;
  }
  if (s.phase == Session::Phase::HANDSHAKE && elapsed(now, s.started_ms, HANDSHAKE_TIMEOUT_MS)) {
    this->end_session_(s, false, "handshake timeout");
    return;
  }

  if (s.is_client && s.done_sent && s.done_received && s.serve_pending == 0 && s.jobs_pending == 0 && flushed) {
    this->end_session_(s, true, "");
    return;
  }
  bool waiting_for_us = s.jobs_pending > 0 || s.serve_pending > 0;
  if (!waiting_for_us && elapsed(now, s.last_rx_ms, SESSION_IDLE_TIMEOUT_MS)) {
    this->end_session_(s, false, "timeout");
    return;
  }
  if (elapsed(now, s.started_ms, SESSION_MAX_MS))
    this->end_session_(s, false, "session took too long");
}

// ---------------------------------------------------------------------------------------------
// Rounds and main loop
// ---------------------------------------------------------------------------------------------

void FingerprintGrowPlusComponent::run_round_() {
  const uint32_t now = millis();
  if (!this->round_active_) {
    if (!this->ready_ || !this->round_requested_ || (int32_t) (now - this->round_due_ms_) < 0)
      return;
    this->round_requested_ = false;
    this->round_active_ = true;
    this->round_refreshing_ = true;
    this->round_started_ms_ = now;
    this->round_peer_idx_ = 0;
    this->round_reachable_ = 0;
    this->round_failures_ = 0;
    this->round_last_error_.clear();
    this->round_unreachable_.clear();
    // remote_present_ is deliberately kept across rounds: clearing it here would let
    // enroll_next_free pick a slot that a peer holds until that peer's manifest arrives.
    // Conflicts are re-evaluated every round; resolved ones disappear on their own.
    std::fill(this->conflict_.begin(), this->conflict_.end(), false);
    this->set_status_("syncing");
    this->sync_started_cb_.call();
    // Re-read the index first: catches local changes made outside this component.
    if (!this->refresh_queued_) {
      this->refresh_queued_ = true;
      this->ready_ = false;
      this->queue_simple_job_(JobType::REFRESH_INDEX);
    }
    return;
  }
  if (this->round_refreshing_) {
    if (!this->ready_) {
      if (elapsed(now, this->round_started_ms_, ROUND_REFRESH_TIMEOUT_MS)) {
        // The sensor stayed busy (e.g. an abandoned enrollment). Give the round up instead of
        // waiting forever; the queued index refresh still runs once the sensor is free.
        this->round_active_ = false;
        this->round_refreshing_ = false;
        ESP_LOGW(TAG, "Sync round given up: the sensor was busy for %" PRIu32 " s",
                 ROUND_REFRESH_TIMEOUT_MS / 1000);
        this->error_count_++;
        this->last_round_ok_ = false;
        this->sync_failed_cb_.call(std::string("sensor busy"));
        this->set_status_("sensor busy");
        this->publish_diagnostics_();
        if (!this->round_requested_)
          this->request_periodic_round_();
      }
      return;
    }
    this->round_refreshing_ = false;
  }
  if (this->outbound_.phase != Session::Phase::IDLE)
    return;
  while (this->round_peer_idx_ < this->peers_.size()) {
    const Peer &peer = this->peers_[this->round_peer_idx_++];
    if (this->is_self_(peer))
      continue;
    this->start_client_session_(peer);
    return;
  }
  // Round complete.
  this->round_active_ = false;
  this->conflict_reported_ = this->conflict_;
#ifdef USE_SENSOR
  if (this->peers_reachable_sensor_ != nullptr)
    this->peers_reachable_sensor_->publish_state(this->round_reachable_);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->unreachable_text_sensor_ != nullptr) {
    // HA text states are limited to 255 characters.
    std::string text = this->round_unreachable_.empty() ? "-" : this->round_unreachable_.substr(0, 250);
    if (!this->unreachable_text_sensor_->has_state() || this->unreachable_text_sensor_->state != text) {
      ESP_LOGD(TAG, "Unreachable peers: %s", text.c_str());
      this->unreachable_text_sensor_->publish_state(text);
    }
  }
#endif
  this->last_round_ok_ = this->round_failures_ == 0;
  if (this->round_failures_ == 0) {
#ifdef USE_SENSOR
#ifdef USE_TIME
    if (this->last_sync_sensor_ != nullptr && this->time_ != nullptr) {
      auto t = this->time_->now();
      if (t.is_valid())
        this->last_sync_sensor_->publish_state(t.timestamp);
    }
#endif
#endif
    ESP_LOGI(TAG, "Sync round done, %u peer(s) reached", this->round_reachable_);
    this->sync_done_cb_.call();
  } else {
    std::string reason = this->round_last_error_.empty() ? "peer error" : this->round_last_error_;
    ESP_LOGW(TAG, "Sync round finished with %u failure(s)", this->round_failures_);
    this->sync_failed_cb_.call(reason);
  }
  this->set_status_(this->dry_run_ ? "idle (dry run)" : (this->round_failures_ ? "idle (last round had errors)" : "idle"));
  this->publish_diagnostics_();
  this->retry_pending_locks_();
  if (!this->round_requested_)
    this->request_periodic_round_();
}

void FingerprintGrowPlusComponent::loop() {
  const uint32_t now = millis();

  if (!this->params_loaded_ && !this->refresh_queued_) {
    bool first = this->retry_ms_ == 0;
    if ((first && elapsed(now, this->boot_ms_, this->startup_delay_ms_)) ||
        (!first && elapsed(now, this->retry_ms_, SENSOR_RETRY_MS))) {
      this->refresh_queued_ = true;
      this->set_status_("indexing");
      this->queue_simple_job_(JobType::REFRESH_INDEX);
    }
  } else if (this->params_loaded_ && !this->ready_ && !this->refresh_queued_ && this->pending_hash_ == 0 &&
             this->retry_ms_ != 0 && elapsed(now, this->retry_ms_, SENSOR_RETRY_MS)) {
    this->retry_ms_ = 0;
    this->refresh_queued_ = true;
    this->queue_simple_job_(JobType::REFRESH_INDEX);
  }
  if (this->failed_sensor_ && !this->grow_->is_failed()) {
    this->failed_sensor_ = false;
  }

  // Network. Without a key the node runs stand-alone: rounds still re-read the local index and
  // bookkeep local changes, they just have no peers.
  if (this->network_) {
    const bool may_accept = this->inbound_.phase == Session::Phase::IDLE;
    this->transport_.loop(may_accept);
    if (may_accept && this->transport_.inbound()->connected()) {
      if (!this->ready_ || this->jobs_.size() > MAX_QUEUED_JOBS) {
        ESP_LOGD(TAG, "Inbound connection refused: not ready");
        this->transport_.inbound()->close();
      } else {
        this->start_server_session_();
      }
    }
    this->service_session_(this->inbound_);
    this->service_session_(this->outbound_);
  }
  this->run_round_();

  this->admin_loop_();
  this->lockout_loop_();
  this->guest_expiry_loop_();
#ifdef USE_TEXT_SENSOR
  if (this->occupied_text_sensor_ != nullptr && this->indexed_once_ && elapsed(now, this->last_occupied_ms_, 2000)) {
    this->last_occupied_ms_ = now;
    this->publish_occupied_();
  }
#endif

  // One sensor job per loop iteration keeps every loop() call short.
  this->run_next_job_();

  if (this->meta_dirty_ && elapsed(now, this->meta_dirty_ms_, META_SAVE_DEBOUNCE_MS) && !this->save_queued_) {
    this->meta_dirty_ = false;
    this->save_queued_ = true;
    this->queue_simple_job_(JobType::SAVE_META);
  }
  if (this->full_backup_open_ && (int32_t) (this->full_backup_until_ms_ - now) <= 0) {
    this->full_backup_open_ = false;
    ESP_LOGI(TAG, "Full backup window closed");
  }
  if (this->restore_open_ && (int32_t) (this->restore_until_ms_ - now) <= 0) {
    this->restore_open_ = false;
    ESP_LOGI(TAG, "Restore window closed");
  }
}

}  // namespace esphome::fingerprint_grow_plus
