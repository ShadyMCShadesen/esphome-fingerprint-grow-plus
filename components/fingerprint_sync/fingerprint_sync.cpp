#include "fingerprint_sync.h"
#include "crypto.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>

namespace esphome::fingerprint_sync {

static const char *const TAG = "fingerprint_sync";

/// Matching has priority: no sensor job within this time after any scan activity.
static constexpr uint32_t ACTIVITY_HOLDOFF_MS = 1500;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30000;
static constexpr uint32_t SESSION_MAX_MS = 10 * 60 * 1000;
static constexpr uint32_t META_SAVE_DEBOUNCE_MS = 2000;
static constexpr uint32_t PUSH_DELAY_MS = 2000;
static constexpr uint32_t SENSOR_RETRY_MS = 30000;
static constexpr uint16_t MAX_CAPACITY = 1024;
static constexpr size_t MAX_QUEUED_JOBS = 64;

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t duration) { return now - since >= duration; }

// ---------------------------------------------------------------------------------------------
// Setup / config
// ---------------------------------------------------------------------------------------------

void FingerprintSyncComponent::setup() {
  this->boot_ms_ = millis();
  this->link_.set_uart(this);
  this->node_id_ = fnv1_hash(App.get_name());
  if (this->node_id_ == 0)
    this->node_id_ = 1;
  if (this->backend_ == MetaBackend::NOTEPAD) {
    this->storage_ = std::make_unique<NotepadStorage>(&this->link_);
  } else {
    this->storage_ = std::make_unique<PreferenceStorage>(fnv1_hash("fingerprint_sync_meta") ^ this->node_id_);
  }
  this->transport_.setup(TAG);

  auto activity = [this]() { this->last_activity_ms_ = millis(); };
  this->grow_->add_on_finger_scan_start_callback(activity);
  this->grow_->add_on_finger_scan_unmatched_callback(activity);
  this->grow_->add_on_finger_scan_misplaced_callback(activity);
  this->grow_->add_on_finger_scan_invalid_callback(activity);
  this->grow_->add_on_finger_scan_matched_callback([this](uint16_t finger_id, uint16_t confidence) {
    this->last_activity_ms_ = millis();
    if (!this->admin_enabled_ || finger_id != this->admin_slot_)
      return;
    uint32_t now = millis();
    if (this->admin_hits_ == 0 || elapsed(now, this->admin_first_ms_, this->admin_window_ms_)) {
      this->admin_hits_ = 1;
      this->admin_first_ms_ = now;
    } else {
      this->admin_hits_++;
    }
    this->admin_touch_cb_.call(this->admin_hits_);
    if (this->admin_hits_ >= this->admin_touches_) {
      this->admin_hits_ = 0;
      ESP_LOGI(TAG, "Admin finger confirmed, starting enrollment of the next free slot");
      this->enroll_next_free(this->admin_num_scans_);
    }
  });
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
      ESP_LOGI(TAG, "Slot %u enrolled locally; it is not synced (outside the managed slots)", slot);
      return;
    }
    this->queue_simple_job_(JobType::HASH, slot, JOB_FLAG_ENROLLED | JOB_FLAG_PROPAGATE);
    this->pending_hash_++;
  });
  this->grow_->add_on_enrollment_failed_callback([this](uint16_t) {
    this->enrolling_ = false;
    this->last_activity_ms_ = millis();
  });

  this->set_status_("starting");
  this->publish_diagnostics_();
}

void FingerprintSyncComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Fingerprint Sync:");
  ESP_LOGCONFIG(TAG, "  Node: %s (id %08" PRIX32 ")", App.get_name().c_str(), this->node_id_);
  this->transport_.dump_config();
  for (const auto &peer : this->peers_) {
    ESP_LOGCONFIG(TAG, "  Peer: %s:%u%s", peer.host, peer.port, this->is_self_(peer) ? " (this node, skipped)" : "");
  }
  for (const auto &r : this->include_)
    ESP_LOGCONFIG(TAG, "  Include slots: %u-%u", r.from, r.to);
  for (const auto &r : this->exclude_)
    ESP_LOGCONFIG(TAG, "  Exclude slots: %u-%u", r.from, r.to);
  ESP_LOGCONFIG(TAG, "  Sync interval: %" PRIu32 " s", this->sync_interval_ms_ / 1000);
  ESP_LOGCONFIG(TAG, "  Metadata storage: %s", this->storage_ ? this->storage_->name() : "?");
  if (this->dry_run_)
    ESP_LOGCONFIG(TAG, "  DRY RUN: nothing is written to or deleted from the sensor by the sync");
  if (this->admin_enabled_)
    ESP_LOGCONFIG(TAG, "  Admin finger: slot %u, %u touches within %" PRIu32 " ms", this->admin_slot_,
                  this->admin_touches_, this->admin_window_ms_);
  if (this->finger_sensor_ == nullptr)
    ESP_LOGCONFIG(TAG, "  No finger sensor configured: sync jobs only wait for the scan hold-off");
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

bool FingerprintSyncComponent::managed_(uint16_t slot) const {
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

uint16_t FingerprintSyncComponent::next_version_() {
  if (this->clock_ >= MAX_VERSION) {
    ESP_LOGE(TAG, "Version counter exhausted; see docs/protocol.md (version reset)");
    return MAX_VERSION;
  }
  return ++this->clock_;
}

void FingerprintSyncComponent::mark_dirty_() {
  if (!this->meta_dirty_)
    this->meta_dirty_ms_ = millis();
  this->meta_dirty_ = true;
}

void FingerprintSyncComponent::request_round_(uint32_t delay_ms) {
  uint32_t due = millis() + delay_ms;
  if (!this->round_requested_ || (int32_t) (due - this->round_due_ms_) < 0)
    this->round_due_ms_ = due;
  this->round_requested_ = true;
}

void FingerprintSyncComponent::set_status_(const char *status) {
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

uint16_t FingerprintSyncComponent::conflict_count() const {
  uint16_t n = 0;
  for (bool c : this->conflict_)
    n += c ? 1 : 0;
  return n;
}

void FingerprintSyncComponent::publish_diagnostics_() {
#ifdef USE_SENSOR
  if (this->error_count_sensor_ != nullptr)
    this->error_count_sensor_->publish_state(this->error_count_);
  if (this->conflict_count_sensor_ != nullptr)
    this->conflict_count_sensor_->publish_state(this->conflict_count());
#endif
}

bool FingerprintSyncComponent::is_self_(const Peer &peer) const {
  const std::string &name = App.get_name();
  std::string host = peer.host;
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  std::string lname = name;
  std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return std::tolower(c); });
  return host == lname || host == lname + ".local";
}

bool FingerprintSyncComponent::sensor_available_() {
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
  if (this->finger_sensor_ != nullptr && this->finger_sensor_->state)
    return false;
  if (!elapsed(millis(), this->last_activity_ms_, ACTIVITY_HOLDOFF_MS))
    return false;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Job queue
// ---------------------------------------------------------------------------------------------

void FingerprintSyncComponent::queue_job_(Job &&job) { this->jobs_.push_back(std::move(job)); }

void FingerprintSyncComponent::queue_simple_job_(JobType type, uint16_t slot, uint8_t flags) {
  Job job;
  job.type = type;
  job.slot = slot;
  job.flags = flags;
  this->queue_job_(std::move(job));
}

void FingerprintSyncComponent::run_next_job_() {
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
    case JobType::COMPARE:
      this->job_compare_(job);
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
  }
  if (!job.data.empty())
    crypto::wipe(job.data.data(), job.data.size());
}

void FingerprintSyncComponent::finish_indexing_if_done_() {
  if (this->ready_ || !this->params_loaded_ || this->pending_hash_ != 0 || this->refresh_queued_)
    return;
  this->ready_ = true;
  uint16_t present = 0;
  for (uint16_t s = 0; s < this->capacity_; s++)
    present += (this->managed_(s) && meta_present(this->meta_[s])) ? 1 : 0;
  if (this->round_active_) {
    ESP_LOGD(TAG, "Index refreshed: %u managed templates, clock %u", present, this->clock_);
    return;
  }
  ESP_LOGI(TAG, "Index ready: %u managed templates, capacity %u, clock %u", present, this->capacity_, this->clock_);
  this->set_status_(this->dry_run_ ? "idle (dry run)" : "idle");
  if (!this->round_requested_)
    this->request_round_(1000);
}

// ---------------------------------------------------------------------------------------------
// Jobs (each one is a complete set of sensor transactions inside a single loop() call)
// ---------------------------------------------------------------------------------------------

void FingerprintSyncComponent::job_refresh_index_() {
  if (!this->params_loaded_) {
    GrowParams params;
    uint8_t res = this->link_.read_params(params);
    if (res != GROW_OK) {
      ESP_LOGW(TAG, "Reading sensor parameters failed (0x%02X), retrying later", res);
      this->error_count_++;
      this->retry_ms_ = millis();
      this->publish_diagnostics_();
      return;
    }
    uint16_t capacity = std::min<uint16_t>(params.capacity, MAX_CAPACITY);
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
    for (uint16_t m : this->meta_)
      this->clock_ = std::max(this->clock_, meta_version(m));
    this->params_loaded_ = true;
    ESP_LOGI(TAG, "Sensor: capacity %u, security level %u, packet size %u bytes", params.capacity,
             params.security_level, params.packet_size);
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
        this->queue_simple_job_(JobType::HASH, slot);
        this->pending_hash_++;
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

void FingerprintSyncComponent::job_hash_(Job &job) {
  const uint16_t slot = job.slot;
  if (this->pending_hash_ > 0)
    this->pending_hash_--;
  std::vector<uint8_t> data;
  uint8_t res = this->link_.read_template(slot, data);
  if (res != GROW_OK) {
    ESP_LOGW(TAG, "Reading template of slot %u failed (0x%02X)", slot, res);
    this->error_count_++;
    this->publish_diagnostics_();
    this->finish_indexing_if_done_();
    return;
  }
  uint8_t digest[32];
  crypto::sha256(data.data(), data.size(), digest);
  crypto::wipe(data.data(), data.size());
  memcpy(this->hash_[slot].data(), digest, HASH_SIZE);
  this->hash_known_[slot] = true;
  this->local_present_[slot] = true;

  if (job.flags & JOB_FLAG_ENROLLED) {
    bool was_present = meta_present(this->meta_[slot]);
    // Enrolling over an occupied slot is a deliberate replacement and may overwrite peers.
    this->meta_[slot] = meta_pack(was_present ? SLOT_PRESENT_FORCED : SLOT_PRESENT, this->next_version_());
    this->conflict_[slot] = false;
    this->mark_dirty_();
    ESP_LOGI(TAG, "Slot %u enrolled (version %u%s), propagating", slot, meta_version(this->meta_[slot]),
             was_present ? ", replaces previous finger" : "");
    if (job.flags & JOB_FLAG_PROPAGATE)
      this->request_round_(PUSH_DELAY_MS);
  }
  this->finish_indexing_if_done_();
}

void FingerprintSyncComponent::job_serve_(Job &job) {
  Session *s = this->session_by_gen_(job.session);
  if (s != nullptr && s->serve_pending > 0)
    s->serve_pending--;
  if (s == nullptr)
    return;
  const uint16_t slot = job.slot;
  if (!this->managed_(slot) || !meta_present(this->meta_[slot]) || !this->hash_known_[slot]) {
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
  if (memcmp(digest, this->hash_[slot].data(), HASH_SIZE) != 0) {
    // The module returned different bytes for the same slot (see docs: assumption A2).
    ESP_LOGW(TAG, "Template bytes of slot %u changed since indexing; updating the hash", slot);
    memcpy(this->hash_[slot].data(), digest, HASH_SIZE);
  }
  std::vector<uint8_t> msg;
  MsgWriter w(msg);
  w.u8(MSG_TEMPLATE);
  w.u16(slot);
  w.u16(this->meta_[slot]);
  w.bytes(digest, HASH_SIZE);
  w.u16((uint16_t) data.size());
  w.bytes(data.data(), data.size());
  crypto::wipe(data.data(), data.size());
  this->send_message_(*s, msg);
  crypto::wipe(msg.data(), msg.size());
  s->served++;
}

bool FingerprintSyncComponent::store_verified_(uint8_t buffer, uint16_t slot, const std::vector<uint8_t> &data) {
  // Precondition: CharBuffer `buffer` holds `data`.
  uint8_t res = this->link_.store(buffer, slot);
  if (res != GROW_OK) {
    ESP_LOGE(TAG, "Storing slot %u failed (0x%02X)", slot, res);
    return false;
  }
  // Verification: read back and compare. If the module re-encodes the template, fall back to
  // the module's own 1:1 match between the read back template and the original.
  const uint8_t other = buffer == 1 ? 2 : 1;
  std::vector<uint8_t> readback;
  res = this->link_.read_template(slot, readback, other);
  bool ok = false;
  if (res == GROW_OK) {
    ok = readback.size() == data.size() && crypto::equal_ct(readback.data(), data.data(), data.size());
    crypto::wipe(readback.data(), readback.size());
    if (!ok) {
      uint16_t score = 0;
      ok = this->link_.match(score) == GROW_OK;
      ESP_LOGD(TAG, "Slot %u read back differs bytewise; module match %s (score %u)", slot, ok ? "ok" : "FAILED",
               score);
    }
  }
  if (!ok) {
    ESP_LOGE(TAG, "Verification of slot %u failed; removing the unverified template", slot);
    this->link_.delete_char(slot, 1);
  }
  return ok;
}

void FingerprintSyncComponent::apply_stored_(uint16_t slot, uint16_t meta, const uint8_t *hash) {
  this->meta_[slot] = meta;
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

void FingerprintSyncComponent::job_write_(Job &job) {
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
    ESP_LOGE(TAG, "Downloading template for slot %u failed (0x%02X)", slot, res);

  uint16_t meta = job.meta;
  if (restore) {
    meta = meta_pack(SLOT_PRESENT_FORCED, this->next_version_());
  }
  if (ok) {
    this->apply_stored_(slot, meta, job.hash.data());
    ESP_LOGI(TAG, "Slot %u %s (version %u)", slot,
             restore ? "restored" : ((job.flags & JOB_FLAG_REPLACE) ? "replaced from peer" : "received from peer"),
             meta_version(meta));
    if (s != nullptr)
      s->pulled++;
    if (restore)
      this->request_round_(PUSH_DELAY_MS);
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

void FingerprintSyncComponent::job_compare_(Job &job) {
  Session *s = this->session_by_gen_(job.session);
  if (s != nullptr && s->jobs_pending > 0)
    s->jobs_pending--;
  const uint16_t slot = job.slot;
  if (!meta_present(this->meta_[slot]) || !this->hash_known_[slot])
    return;  // local state changed meanwhile; the next round decides again

  uint8_t res = this->link_.load_char(1, slot);
  if (res == GROW_OK)
    res = this->link_.down_char(2, job.data);
  uint16_t score = 0;
  if (res == GROW_OK)
    res = this->link_.match(score);

  if (res == GROW_NO_MATCH) {
    this->flag_conflict_(slot);
    return;
  }
  if (res != GROW_OK) {
    ESP_LOGW(TAG, "Comparing slot %u failed (0x%02X)", slot, res);
    this->error_count_++;
    this->publish_diagnostics_();
    return;
  }

  // Same finger, different template bytes (e.g. enrolled separately on two nodes). Converge
  // deterministically on one byte representation: higher version wins, then the higher hash.
  const uint16_t local = this->meta_[slot];
  const uint16_t lv = meta_version(local), rv = meta_version(job.meta);
  bool remote_wins = rv > lv || (rv == lv && memcmp(job.hash.data(), this->hash_[slot].data(), HASH_SIZE) > 0);
  const uint16_t version = std::max(lv, rv);
  if (!remote_wins) {
    if (rv > lv) {
      this->meta_[slot] = meta_pack(meta_state(local), version);
      this->mark_dirty_();
    }
    ESP_LOGD(TAG, "Slot %u holds the same finger as the peer; keeping the local template", slot);
    return;
  }
  if (this->dry_run_) {
    ESP_LOGI(TAG, "[dry run] slot %u: same finger, would adopt the peer's template bytes", slot);
    return;
  }
  // CharBuffer2 still holds the peer's template.
  if (this->store_verified_(2, slot, job.data)) {
    this->apply_stored_(slot, meta_pack(meta_state(job.meta), version), job.hash.data());
    ESP_LOGI(TAG, "Slot %u: same finger as on the peer, converged to the peer's template", slot);
  } else {
    this->error_count_++;
    this->meta_[slot] = meta_pack(SLOT_NONE, 0);
    this->hash_known_[slot] = false;
    this->local_present_[slot] = false;
    this->mark_dirty_();
    this->publish_diagnostics_();
  }
}

void FingerprintSyncComponent::flag_conflict_(uint16_t slot) {
  if (this->conflict_[slot])
    return;
  this->conflict_[slot] = true;
  if (this->conflict_reported_[slot])
    return;  // still the same conflict as in the previous round
  this->conflict_reported_[slot] = true;
  ESP_LOGW(TAG, "CONFLICT in slot %u: a different finger is stored on a peer. Nothing was overwritten; "
           "use fingerprint_sync.resolve_conflict.", slot);
  this->conflict_cb_.call(slot);
  this->publish_diagnostics_();
}

void FingerprintSyncComponent::job_delete_(Job &job) {
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
  uint16_t meta = job.meta;
  if (job.flags & JOB_FLAG_LOCAL)
    meta = meta_pack(SLOT_TOMBSTONE, this->next_version_());
  if (slot < this->capacity_) {
    this->local_present_[slot] = false;
    if (this->managed_(slot)) {
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
    this->request_round_(PUSH_DELAY_MS);
}

void FingerprintSyncComponent::job_count_() {
  uint16_t count = 0;
  if (this->link_.template_count(count) == GROW_OK && this->count_sensor_ != nullptr)
    this->count_sensor_->publish_state(count);
}

void FingerprintSyncComponent::job_save_meta_() {
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

void FingerprintSyncComponent::sync_now() {
  ESP_LOGI(TAG, "Manual sync requested");
  this->request_round_(0);
}

void FingerprintSyncComponent::delete_slot(uint16_t slot) {
  if (!this->params_loaded_ || slot >= this->capacity_) {
    ESP_LOGW(TAG, "Cannot delete slot %u (sensor not indexed yet or slot out of range)", slot);
    return;
  }
  Job job;
  job.type = JobType::DELETE;
  job.slot = slot;
  job.flags = JOB_FLAG_LOCAL | (this->managed_(slot) ? JOB_FLAG_PROPAGATE : 0);
  this->queue_job_(std::move(job));
}

void FingerprintSyncComponent::delete_all(const std::string &code) {
  if (this->delete_all_code_ == nullptr || this->delete_all_code_[0] == '\0' || code != this->delete_all_code_) {
    ESP_LOGW(TAG, "delete_all refused: wrong or unconfigured code");
    return;
  }
  if (!this->params_loaded_)
    return;
  ESP_LOGW(TAG, "Deleting ALL managed slots on this node and on all peers");
  uint16_t queued = 0;
  for (uint16_t slot = 0; slot < this->capacity_; slot++) {
    if (!this->managed_(slot))
      continue;
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
  this->request_round_(PUSH_DELAY_MS + 1000u * queued);
}

int32_t FingerprintSyncComponent::next_free_slot() const {
  if (!this->params_loaded_)
    return -1;
  uint16_t to = std::min<uint16_t>(this->enroll_to_, this->capacity_ - 1);
  for (uint16_t slot = this->enroll_from_; slot <= to; slot++) {
    if (this->admin_enabled_ && slot == this->admin_slot_)
      continue;
    if (this->local_present_[slot] || this->remote_present_[slot] || this->conflict_[slot])
      continue;
    if (meta_present(this->meta_[slot]))
      continue;
    return slot;
  }
  return -1;
}

void FingerprintSyncComponent::enroll_next_free(uint8_t num_scans) {
  if (!this->ready_) {
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

void FingerprintSyncComponent::resolve_conflict(uint16_t slot, bool keep_local) {
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
    this->request_round_(PUSH_DELAY_MS);
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

void FingerprintSyncComponent::allow_restore(uint32_t seconds) {
  this->restore_until_ms_ = millis() + seconds * 1000;
  this->restore_open_ = seconds > 0;
  ESP_LOGW(TAG, "Restore window %s for %" PRIu32 " s", this->restore_open_ ? "OPEN" : "closed", seconds);
}

// ---------------------------------------------------------------------------------------------
// Sync decisions
// ---------------------------------------------------------------------------------------------

FingerprintSyncComponent::Decision FingerprintSyncComponent::decide_(uint16_t slot, uint16_t remote,
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
  d.kind = Decision::PULL;  // compare with the module's 1:1 match
  return d;
}

void FingerprintSyncComponent::process_manifest_entry_(Session &s, uint16_t slot, uint16_t meta,
                                                       const uint8_t *hash) {
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
    case Decision::NOTHING:
      break;
  }
}

void FingerprintSyncComponent::handle_template_(Session &s, uint16_t slot, uint16_t meta, const uint8_t *hash,
                                                const uint8_t *data, size_t len) {
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
      this->mark_dirty_();
    }
    return;
  }
  Job job;
  job.slot = slot;
  job.meta = meta;
  job.session = s.gen;
  memcpy(job.hash.data(), hash, HASH_SIZE);
  job.data.assign(data, data + len);
  const uint16_t local = this->meta_[slot];
  if (!meta_present(local)) {
    job.type = JobType::WRITE;
  } else if (meta_forced(meta) && meta_version(meta) > meta_version(local)) {
    job.type = JobType::WRITE;
    job.flags = JOB_FLAG_REPLACE;
    ESP_LOGW(TAG, "Slot %u is being replaced by a deliberate re-enrollment on a peer", slot);
  } else {
    job.type = JobType::COMPARE;
  }
  s.jobs_pending++;
  this->queue_job_(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------

Session *FingerprintSyncComponent::session_by_gen_(uint32_t gen) {
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
  s.done_sent = false;
  s.done_received = false;
  s.pulled = 0;
  s.served = 0;
  s.peer = nullptr;
}

void FingerprintSyncComponent::start_client_session_(const Peer &peer) {
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

void FingerprintSyncComponent::start_server_session_() {
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

void FingerprintSyncComponent::send_message_(Session &s, const std::vector<uint8_t> &msg) {
  if (!s.channel.seal(msg.data(), msg.size(), s.tx)) {
    ESP_LOGE(TAG, "Message too large or channel closed");
  }
}

void FingerprintSyncComponent::send_simple_(Session &s, uint8_t type, uint16_t slot, uint8_t value, bool with_value) {
  std::vector<uint8_t> msg;
  MsgWriter w(msg);
  w.u8(type);
  w.u16(slot);
  if (with_value)
    w.u8(value);
  this->send_message_(s, msg);
}

void FingerprintSyncComponent::send_manifest_(Session &s) {
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
    }
    this->send_message_(s, msg);
  }
}

void FingerprintSyncComponent::handle_message_(Session &s, const std::vector<uint8_t> &msg) {
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
      for (uint16_t i = 0; i < count; i++) {
        uint16_t slot, meta;
        uint8_t hash[HASH_SIZE];
        if (!r.u16(slot) || !r.u16(meta) || !r.bytes(hash, HASH_SIZE))
          break;
        this->process_manifest_entry_(s, slot, meta, hash);
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
      if (!r.u16(slot) || !r.u16(meta) || !r.bytes(hash, HASH_SIZE) || !r.u16(len))
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
        this->handle_template_(s, slot, meta, hash, data, len);
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
    case MSG_RESTORE: {
      uint16_t slot, len;
      if (!r.u16(slot) || !r.u16(len))
        break;
      const uint8_t *data = r.ptr(len);
      if (data == nullptr || len == 0 || len > GrowLink::MAX_TEMPLATE_SIZE)
        break;
      bool window = this->restore_open_ && (int32_t) (this->restore_until_ms_ - millis()) > 0;
      if (!window || slot >= this->capacity_) {
        ESP_LOGW(TAG, "Restore of slot %u refused (restore window closed or slot out of range)", slot);
        this->send_simple_(s, MSG_RESTORE_ACK, slot, 2, true);
        break;
      }
      Job job;
      job.type = JobType::WRITE;
      job.slot = slot;
      job.flags = JOB_FLAG_RESTORE;
      job.session = s.gen;
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

void FingerprintSyncComponent::end_session_(Session &s, bool ok, const char *reason) {
  if (s.phase == Session::Phase::IDLE)
    return;
  const char *who = s.is_client && s.peer != nullptr ? s.peer->host : "inbound peer";
  if (ok) {
    ESP_LOGI(TAG, "Sync with %s finished: %u received, %u sent", who, s.pulled, s.served);
  } else {
    ESP_LOGW(TAG, "Sync with %s failed: %s", who, reason);
    this->error_count_++;
    if (s.is_client)
      this->round_last_error_ = std::string(who) + ": " + reason;
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

void FingerprintSyncComponent::service_session_(Session &s) {
  if (s.phase == Session::Phase::IDLE)
    return;
  const uint32_t now = millis();

  if (s.phase == Session::Phase::CONNECTING) {
    if (s.stream->connected()) {
      s.phase = Session::Phase::HANDSHAKE;
      s.last_rx_ms = now;
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

void FingerprintSyncComponent::run_round_() {
  const uint32_t now = millis();
  if (!this->round_active_) {
    if (!this->ready_ || !this->round_requested_ || (int32_t) (now - this->round_due_ms_) < 0)
      return;
    this->round_requested_ = false;
    this->round_active_ = true;
    this->round_refreshing_ = true;
    this->round_peer_idx_ = 0;
    this->round_reachable_ = 0;
    this->round_failures_ = 0;
    this->round_last_error_.clear();
    std::fill(this->remote_present_.begin(), this->remote_present_.end(), false);
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
    if (!this->ready_)
      return;
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
  if (!this->round_requested_)
    this->request_round_(this->sync_interval_ms_);
}

void FingerprintSyncComponent::loop() {
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

  // Network.
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
  this->run_round_();

  // One sensor job per loop iteration keeps every loop() call short.
  this->run_next_job_();

  if (this->meta_dirty_ && elapsed(now, this->meta_dirty_ms_, META_SAVE_DEBOUNCE_MS) && !this->save_queued_) {
    this->meta_dirty_ = false;
    this->save_queued_ = true;
    this->queue_simple_job_(JobType::SAVE_META);
  }
  if (this->restore_open_ && (int32_t) (this->restore_until_ms_ - now) <= 0) {
    this->restore_open_ = false;
    ESP_LOGI(TAG, "Restore window closed");
  }
}

}  // namespace esphome::fingerprint_sync
