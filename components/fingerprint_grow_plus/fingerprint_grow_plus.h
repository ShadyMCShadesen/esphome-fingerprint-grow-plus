#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/fingerprint_grow/fingerprint_grow.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "grow_link.h"
#include "meta_storage.h"
#include "protocol.h"
#include "secure_channel.h"
#include "tcp_transport.h"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <map>
#include <vector>

namespace esphome::fingerprint_grow_plus {

struct Peer {
  const char *host;
  uint16_t port;
};

/// Delay between a local change and the sync round that pushes it.
static constexpr uint32_t PUSH_DELAY_MS = 2000;

struct ScheduleWindow {
  uint8_t days;       // bit 0 = Sunday ... bit 6 = Saturday
  uint16_t from_min;  // minutes since midnight
  uint16_t to_min;    // exclusive; < from_min = over midnight
};

struct Schedule {
  uint16_t from;
  uint16_t to;
  std::vector<ScheduleWindow> windows;
};

struct SlotRange {
  uint16_t from;
  uint16_t to;
};

enum class MetaBackend : uint8_t { PREFERENCES, NOTEPAD };

enum class JobType : uint8_t {
  REFRESH_INDEX,
  HASH,
  SERVE,
  WRITE,
  DELETE,
  COUNT,
  SAVE_META,
  MOVE,  // slot -> meta (target slot)
};

enum JobFlag : uint8_t {
  JOB_FLAG_ENROLLED = 1 << 0,   // HASH after a local enrollment
  JOB_FLAG_REPLACE = 1 << 1,    // WRITE replaces a different finger (forced by the peer)
  JOB_FLAG_RESTORE = 1 << 2,    // WRITE from a restore session
  JOB_FLAG_REMOTE = 1 << 3,     // DELETE caused by a peer tombstone
  JOB_FLAG_LOCAL = 1 << 4,      // DELETE requested on this node
  JOB_FLAG_PROPAGATE = 1 << 5,  // start a sync round once the job is done
};

/// Data that travels with a slot version besides the template (see protocol.h, Extra).
struct SlotExtra {
  uint32_t expires{0};  // guest expiry (unix time), 0 = never
  uint8_t uses{0};      // guest uses left, 0 = unlimited
  std::string name;     // optional slot name, up to MAX_NAME_LEN bytes
};

struct Job {
  JobType type;
  uint16_t slot{0};
  uint16_t meta{0};
  uint8_t flags{0};
  uint32_t session{0};
  std::array<uint8_t, HASH_SIZE> hash{};
  SlotExtra extra;
  std::vector<uint8_t> data;
};

enum class AdminArm : uint8_t { NONE, DELETE, REENROLL };

struct Session {
  enum class Phase : uint8_t { IDLE, CONNECTING, HANDSHAKE, SYNC };
  Phase phase{Phase::IDLE};
  bool is_client{false};
  Stream *stream{nullptr};
  uint32_t gen{0};
  uint32_t started_ms{0};
  uint32_t last_rx_ms{0};
  const Peer *peer{nullptr};
  SecureChannel channel;
  std::vector<uint8_t> tx;
  std::vector<uint16_t> pull;
  size_t pull_pos{0};
  bool awaiting_template{false};
  uint16_t awaiting_slot{0};
  uint8_t jobs_pending{0};
  uint8_t serve_pending{0};
  bool manifest_received{false};
  size_t manifest_entries{0};
  bool done_sent{false};
  bool done_received{false};
  uint16_t pulled{0};
  uint16_t served{0};
  uint16_t served_local{0};
  /// The client asked for the local-only slots during a full-backup window (backup tool).
  bool local_ok{false};
  /// Epoch announced in the peer's manifest (version resets, see docs/protocol.md).
  uint16_t peer_epoch{0};
  /// A template of a different size than this sensor's arrived (logged once per session).
  bool incompatible_logged{false};
};

class FingerprintGrowPlusComponent : public Component, public uart::UARTDevice {
 public:
  // ---- configuration (called from generated code) ----
  void set_fingerprint_grow(fingerprint_grow::FingerprintGrowComponent *grow) { this->grow_ = grow; }
  void set_key(const std::array<uint8_t, PSK_SIZE> &key) {
    this->psk_ = key;
    this->network_ = true;
  }
  void set_port(uint16_t port) { this->transport_.set_port(port); }
  void add_peer(const char *host, uint16_t port) {
    this->peers_.push_back(Peer{host, port});
    this->lock_targets_.push_back(false);
    this->lock_pending_.push_back(false);
  }
  void add_include_range(uint16_t from, uint16_t to) { this->include_.push_back(SlotRange{from, to}); }
  void add_exclude_range(uint16_t from, uint16_t to) { this->exclude_.push_back(SlotRange{from, to}); }
  /// 0 = never (no periodic rounds).
  void set_sync_interval(uint32_t ms) { this->sync_interval_ms_ = ms; }
  void set_sync_at_boot(bool on) { this->sync_at_boot_ = on; }
  void set_sync_after_change(bool on) { this->sync_after_change_ = on; }
  void set_startup_delay(uint32_t ms) { this->startup_delay_ms_ = ms; }
  void set_dry_run(bool dry_run) { this->dry_run_ = dry_run; }
  void set_meta_backend(MetaBackend backend) { this->backend_ = backend; }
  void set_delete_all_code(const char *code) { this->delete_all_code_ = code; }
  void add_admin_slot(uint16_t slot) {
    this->admin_enabled_ = true;
    this->admin_slots_.push_back(slot);
  }
  void set_admin_gestures(uint8_t enroll, uint8_t del, uint8_t reenroll, uint8_t guest, uint32_t gesture_ms,
                          uint32_t hold_ms, uint32_t target_ms, uint8_t num_scans) {
    this->enroll_touches_ = enroll;
    this->delete_touches_ = del;
    this->reenroll_touches_ = reenroll;
    this->guest_touches_ = guest;
    this->gesture_timeout_ms_ = gesture_ms;
    this->hold_ms_ = hold_ms;
    this->target_timeout_ms_ = target_ms;
    this->admin_num_scans_ = num_scans;
  }
  // 0 off, 1 immediate (first touch), 2 delayed (gesture completed with a single touch)
  void set_admin_access(uint8_t mode) { this->admin_access_ = mode; }
  // ---- lockout: / duress: (lockout.cpp) ----
  void set_lockout(uint8_t max_attempts, uint32_t duration_ms, bool reset_on_match) {
    this->lockout_enabled_ = true;
    this->lockout_max_ = max_attempts;
    this->lockout_ms_ = duration_ms;
    this->lockout_reset_on_match_ = reset_on_match;
  }
  /// Pass a local lockout on to this peer (host as in sync: peers; port 0 = any port).
  void add_lock_target(const char *host, uint16_t port);
  void set_duress(uint16_t from, uint16_t to) {
    this->duress_enabled_ = true;
    this->duress_from_ = from;
    this->duress_to_ = to;
  }
  template<typename F> void add_on_locked_callback(F &&cb) { this->locked_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_unlocked_callback(F &&cb) { this->unlocked_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_duress_callback(F &&cb) { this->duress_cb_.add(std::forward<F>(cb)); }
  // ---- schedule: (schedule.cpp) ----
  void add_schedule(uint16_t from, uint16_t to);
  void add_schedule_window(uint8_t days_mask, uint16_t from_min, uint16_t to_min);
  void set_schedule_allow_without_time(bool allow) { this->schedule_allow_without_time_ = allow; }
  template<typename F> void add_on_schedule_denied_callback(F &&cb) { this->denied_cb_.add(std::forward<F>(cb)); }
  /// Built-in Aura LED feedback (led_feedback:). `custom_idle`: on_idle restores the user's LED.
  void set_led_feedback(bool custom_idle) {
    this->led_feedback_ = true;
    this->led_custom_idle_ = custom_idle;
  }
  template<typename F> void add_on_led_idle_callback(F &&cb) { this->led_idle_cb_.add(std::forward<F>(cb)); }
  void set_guests(uint16_t from, uint16_t to, uint32_t valid_s) {
    this->guests_enabled_ = true;
    this->guest_from_ = from;
    this->guest_to_ = to;
    this->guest_valid_s_ = valid_s;
  }
  /// Default number of uses for guests (0 = unlimited).
  void set_guest_uses(uint8_t uses) { this->guest_uses_ = uses; }
  void set_enroll_range(uint16_t from, uint16_t to) {
    this->enroll_from_ = from;
    this->enroll_to_ = to;
  }
  void set_finger_sensor(binary_sensor::BinarySensor *sensor) { this->finger_sensor_ = sensor; }
  void set_fingerprint_count_sensor(sensor::Sensor *sensor) { this->count_sensor_ = sensor; }
  void set_fingerprint_capacity_sensor(sensor::Sensor *sensor) { this->capacity_sensor_ = sensor; }
#ifdef USE_SOCKET_IPV4_ALLOW
  void set_allowed_networks(const socket::Ipv4AllowEntry *entries, size_t count) {
    this->transport_.set_allow(entries, count);
  }
#endif
#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif
#ifdef USE_SENSOR
  void set_peers_reachable_sensor(sensor::Sensor *s) { this->peers_reachable_sensor_ = s; }
  void set_error_count_sensor(sensor::Sensor *s) { this->error_count_sensor_ = s; }
  void set_conflict_count_sensor(sensor::Sensor *s) { this->conflict_count_sensor_ = s; }
  void set_last_sync_sensor(sensor::Sensor *s) { this->last_sync_sensor_ = s; }
  void set_versions_left_sensor(sensor::Sensor *s) { this->versions_left_sensor_ = s; }
  void set_failed_attempts_sensor(sensor::Sensor *s) { this->failed_attempts_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_text_sensor_ = s; }
  void set_occupied_text_sensor(text_sensor::TextSensor *s) { this->occupied_text_sensor_ = s; }
  void set_unreachable_text_sensor(text_sensor::TextSensor *s) { this->unreachable_text_sensor_ = s; }
  void set_last_name_text_sensor(text_sensor::TextSensor *s) { this->last_name_text_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_syncing_binary_sensor(binary_sensor::BinarySensor *s) { this->syncing_binary_sensor_ = s; }
  void set_locked_binary_sensor(binary_sensor::BinarySensor *s) { this->locked_binary_sensor_ = s; }
#endif

  // ---- triggers ----
  template<typename F> void add_on_sync_started_callback(F &&cb) { this->sync_started_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_sync_done_callback(F &&cb) { this->sync_done_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_sync_failed_callback(F &&cb) { this->sync_failed_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_conflict_callback(F &&cb) { this->conflict_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_slot_updated_callback(F &&cb) { this->slot_updated_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_slot_deleted_callback(F &&cb) { this->slot_deleted_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_admin_touch_callback(F &&cb) { this->admin_touch_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_admin_access_callback(F &&cb) { this->admin_access_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_admin_gesture_callback(F &&cb) {
    this->admin_gesture_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_admin_hold_callback(F &&cb) { this->admin_hold_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_admin_waiting_callback(F &&cb) {
    this->admin_waiting_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_admin_cancelled_callback(F &&cb) {
    this->admin_cancelled_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_admin_done_callback(F &&cb) { this->admin_done_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_enroll_started_callback(F &&cb) {
    this->enroll_started_cb_.add(std::forward<F>(cb));
  }

  // ---- actions / public API ----
  /// Start a sync round with all peers as soon as possible.
  void sync_now();
  /// Delete a slot here and on all peers (tombstone).
  void delete_slot(uint16_t slot);
  /// Delete all fingers except admins here; synced slots also on all peers. Requires the code.
  void delete_all(const std::string &code);
  /// Enroll the lowest slot that is free on this node and on all peers seen since boot.
  void enroll_next_free(uint8_t num_scans, const std::string &name = "");
  /// Resolve a conflict: keep this node's finger (pushed to all peers) or take the peer's.
  void resolve_conflict(uint16_t slot, bool keep_local);
  /// Accept MSG_RESTORE from the backup tool for `seconds`.
  void allow_restore(uint32_t seconds);
  /// Also hand out local-only slots (outside the synced range) to the backup tool for `seconds`.
  void allow_full_backup(uint32_t seconds);
  /// Make this node the reference for every managed slot it holds: all of them get a new
  /// forced version and replace the peers' templates in these slots (initial rollout).
  void claim_all_slots();
  /// Resolve every conflict found in the last round at once (see resolve_conflict).
  void resolve_all_conflicts(bool keep_local);

  // ---- admin fingers (admin.cpp) ----
  /// True for the configured admin slots.
  bool is_admin_slot(uint16_t slot) const;
  /// True while an admin gesture is being entered or an admin action waits for a finger.
  /// Use it in on_finger_scan_matched to suppress the normal actions.
  bool is_admin_busy() const;
  /// The next matched (non-admin) finger is deleted on all nodes.
  void delete_next_finger();
  /// The next matched (non-admin) finger is enrolled again into its own slot (all nodes).
  void reenroll_next_finger();
  /// Abort a pending admin action.
  void cancel_admin();
  /// Delete an admin slot on all nodes (admin slots are protected otherwise).
  void delete_admin(uint16_t slot, const std::string &code);
  /// Enroll the next free guest slot; it is deleted on all nodes after `valid_s` seconds.
  /// `uses`: number of uses (0 = unlimited, -1 = the guests: default).
  void enroll_guest(uint8_t num_scans, uint32_t valid_s, const std::string &name = "", int16_t uses = -1);
  /// Move a finger to a free slot (with its name and guest data); synced slots on all nodes.
  void move_slot(uint16_t from, uint16_t to);
  /// Guest uses left, 0 = unlimited.
  uint8_t slot_uses_left(uint16_t slot) const { return slot < this->uses_.size() ? this->uses_[slot] : 0; }
  /// Set a new validity for an existing guest slot (counted from now).
  void extend_guest(uint16_t slot, uint32_t valid_s);
  /// Give a slot a name (synced slots: on all nodes, one version). Empty name removes it.
  void set_slot_name(uint16_t slot, const std::string &name);
  /// Name of a slot, empty if it has none.
  std::string slot_name(uint16_t slot) const {
    auto it = this->names_.find(slot);
    return it == this->names_.end() ? std::string() : it->second;
  }
  /// Expiry of a slot as unix time, 0 = never.
  uint32_t slot_expiry(uint16_t slot) const { return slot < this->expires_.size() ? this->expires_[slot] : 0; }
  /// Lockout active (too many failed attempts here, or passed on by a peer).
  bool is_locked() const;
  /// Count a failed attempt (e.g. a match with too low confidence); unmatched scans count by themselves.
  void register_failed_attempt();
  /// Lift a lockout and reset the counter (this node only).
  void reset_lockout();
  bool is_duress_slot(uint16_t slot) const;
  /// False if the slot has a time window (schedule:) that is closed now; true otherwise.
  bool is_allowed(uint16_t slot) const;
  /// 0 = in sync, 1 = sync running, 2 = conflicts, 3 = error (sensor or unreachable peer).
  uint8_t health() const;
  static constexpr uint16_t NO_SLOT = 0xFFFF;
  /// Lowest free slot inside the enroll range, or -1.
  int32_t next_free_slot() const;
  bool is_syncing() const { return this->round_active_; }
  bool is_ready() const { return this->ready_; }
  uint16_t conflict_count() const;

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  struct Decision {
    enum Kind : uint8_t { NOTHING, PULL, ADOPT, DELETE, CONFLICT } kind{NOTHING};
    uint16_t meta{0};
  };

  bool managed_(uint16_t slot) const;
  bool sensor_available_();
  uint16_t next_version_();
  void mark_dirty_();
  void request_round_(uint32_t delay_ms);
  /// Round after a local change (only with sync: after_change) / periodic (not with interval: never).
  void request_change_round_(uint32_t delay_ms);
  void request_periodic_round_();
  void set_status_(const char *status);
  void publish_diagnostics_();
  void queue_job_(Job &&job);
  void queue_simple_job_(JobType type, uint16_t slot = 0, uint8_t flags = 0);
  void run_next_job_();
  void finish_indexing_if_done_();

  // jobs
  void job_refresh_index_();
  void job_hash_(Job &job);
  void job_serve_(Job &job);
  void job_write_(Job &job);
  void job_delete_(Job &job);
  void job_count_();
  void job_save_meta_();
  bool store_verified_(uint8_t buffer, uint16_t slot, const std::vector<uint8_t> &data);
  void apply_stored_(uint16_t slot, uint16_t meta, const uint8_t *hash, const SlotExtra &extra);
  SlotExtra extra_of_(uint16_t slot) const;
  void set_extra_(uint16_t slot, const SlotExtra &extra);
  void write_extra_(MsgWriter &w, uint16_t slot) const;
  static bool read_extra_(MsgReader &r, SlotExtra &extra);
  void set_uses_(uint16_t slot, uint8_t uses);
  void set_name_(uint16_t slot, const std::string &name);

  // sync logic
  Decision decide_(uint16_t slot, uint16_t remote_meta, const uint8_t *remote_hash) const;
  void process_manifest_entry_(Session &s, uint16_t slot, uint16_t meta, const uint8_t *hash,
                               const SlotExtra &extra);
  void handle_template_(Session &s, uint16_t slot, uint16_t meta, const uint8_t *hash, const SlotExtra &extra,
                        const uint8_t *data, size_t len);
  void flag_conflict_(uint16_t slot);
  void set_expiry_(uint16_t slot, uint32_t expires);

  // admin.cpp
  void on_matched_(uint16_t finger_id, uint16_t confidence);
  void admin_loop_();
  void run_gesture_(uint8_t touches);
  void arm_(AdminArm action);
  void run_armed_(uint16_t slot);
  void guest_expiry_loop_();
  bool time_now_(uint32_t &epoch) const;
  int32_t next_free_in_(uint16_t from, uint16_t to, bool guests_allowed) const;
  bool in_guest_range_(uint16_t slot) const {
    return this->guests_enabled_ && slot >= this->guest_from_ && slot <= this->guest_to_;
  }
  void load_expiries_();
  void save_expiries_();

  // sessions
  bool is_self_(const Peer &peer) const;
  void start_client_session_(const Peer &peer);
  void start_server_session_();
  void service_session_(Session &s);
  void handle_message_(Session &s, const std::vector<uint8_t> &msg);
  void send_message_(Session &s, const std::vector<uint8_t> &msg);
  void send_manifest_(Session &s);
  void send_simple_(Session &s, uint8_t type, uint16_t slot, uint8_t value, bool with_value);
  void end_session_(Session &s, bool ok, const char *reason);
  Session *session_by_gen_(uint32_t gen);
  void run_round_();

  // ---- configuration ----
  fingerprint_grow::FingerprintGrowComponent *grow_{nullptr};
  std::array<uint8_t, PSK_SIZE> psk_{};
  bool network_{false};  // false: stand-alone, no listener, no peers
  std::vector<Peer> peers_;
  std::vector<SlotRange> include_;
  std::vector<SlotRange> exclude_;
  uint32_t sync_interval_ms_{15 * 60 * 1000};
  bool sync_at_boot_{true};
  bool sync_after_change_{true};
  uint32_t startup_delay_ms_{10000};
  bool dry_run_{false};
  MetaBackend backend_{MetaBackend::PREFERENCES};
  const char *delete_all_code_{nullptr};
  bool admin_enabled_{false};
  std::vector<uint16_t> admin_slots_;
  uint8_t enroll_touches_{2};
  uint8_t delete_touches_{3};
  uint8_t reenroll_touches_{4};
  uint8_t guest_touches_{0};
  uint32_t gesture_timeout_ms_{2000};
  uint32_t hold_ms_{3000};
  uint32_t target_timeout_ms_{10000};
  uint8_t admin_num_scans_{3};
  bool guests_enabled_{false};
  uint16_t guest_from_{0};
  uint16_t guest_to_{0};
  uint32_t guest_valid_s_{7 * 24 * 3600};
  uint8_t guest_uses_{0};
  uint8_t pending_guest_uses_{0};
  uint16_t last_guest_use_slot_{NO_SLOT};
  uint32_t last_guest_use_ms_{0};
  void guest_used_(uint16_t slot);
  void job_move_(Job &job);
  bool slot_free_everywhere_(uint16_t slot) const;
  uint16_t enroll_from_{1};
  uint16_t enroll_to_{0xFFFF};
  binary_sensor::BinarySensor *finger_sensor_{nullptr};
  sensor::Sensor *count_sensor_{nullptr};
  sensor::Sensor *capacity_sensor_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *peers_reachable_sensor_{nullptr};
  sensor::Sensor *error_count_sensor_{nullptr};
  sensor::Sensor *conflict_count_sensor_{nullptr};
  sensor::Sensor *last_sync_sensor_{nullptr};
  sensor::Sensor *versions_left_sensor_{nullptr};
  sensor::Sensor *failed_attempts_sensor_{nullptr};
  bool versions_low_warned_{false};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *occupied_text_sensor_{nullptr};
  text_sensor::TextSensor *unreachable_text_sensor_{nullptr};
  text_sensor::TextSensor *last_name_text_sensor_{nullptr};
  uint32_t last_occupied_ms_{0};
  void publish_occupied_();
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *syncing_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *locked_binary_sensor_{nullptr};
#endif

  // ---- runtime ----
  GrowLink link_;
  TcpTransport transport_;
  std::unique_ptr<MetaStorage> storage_;
  uint32_t node_id_{0};
  uint32_t boot_ms_{0};
  bool params_loaded_{false};
  bool ready_{false};
  /// The index has been read at least once (ready_ drops briefly while a round re-reads it).
  bool indexed_once_{false};
  bool failed_sensor_{false};
  uint16_t capacity_{0};
  uint16_t clock_{0};
  std::vector<uint16_t> meta_;
  std::vector<std::array<uint8_t, HASH_SIZE>> hash_;
  std::vector<bool> hash_known_;
  std::vector<bool> local_present_;
  std::vector<bool> remote_present_;
  std::vector<bool> conflict_;
  std::vector<bool> conflict_reported_;
  std::deque<Job> jobs_;
  uint16_t pending_hash_{0};
  bool refresh_queued_{false};
  bool count_queued_{false};
  bool save_queued_{false};
  bool meta_dirty_{false};
  uint32_t meta_dirty_ms_{0};
  bool enrolling_{false};
  uint32_t last_activity_ms_{0};
  uint32_t retry_ms_{0};
  uint8_t admin_hits_{0};
  uint32_t admin_last_ms_{0};        // last admin match (hold detection)
  uint32_t admin_quiet_since_ms_{0};  // last admin match or last moment a finger was on the sensor
  bool admin_hold_possible_{false};
  bool hold_lock_{false};
  // ---- lockout / duress (lockout.cpp) ----
  void lock_(uint32_t duration_ms, bool remote);
  void lockout_loop_();
  void lockout_matched_(uint16_t finger_id, uint16_t confidence);
  void send_pending_lock_(Session &s);
  void handle_remote_lock_(Session &s, uint32_t seconds);
  void retry_pending_locks_();
  void publish_lockout_();
  uint32_t lock_remaining_s_() const;
  bool lockout_enabled_{false};
  bool lockout_reset_on_match_{true};
  uint8_t lockout_max_{5};
  uint32_t lockout_ms_{15 * 60 * 1000};
  uint8_t failed_attempts_{0};
  bool locked_{false};
  uint32_t locked_until_ms_{0};
  std::vector<bool> lock_targets_;
  std::vector<bool> lock_pending_;
  CallbackManager<void(std::string)> locked_cb_;
  CallbackManager<void()> unlocked_cb_;
  bool duress_enabled_{false};
  uint16_t duress_from_{0};
  uint16_t duress_to_{0};
  CallbackManager<void(uint16_t, uint16_t)> duress_cb_;
  // ---- schedule (schedule.cpp) ----
  static bool window_open_(const ScheduleWindow &w, uint8_t weekday, uint16_t minute);
  void schedule_matched_(uint16_t finger_id);
  std::vector<Schedule> schedules_;
  bool schedule_allow_without_time_{false};
  CallbackManager<void(uint16_t)> denied_cb_;
  // ---- built-in LED feedback (led_feedback.cpp) ----
  void setup_led_feedback_();
  void led_set_(uint8_t state, uint8_t color, uint32_t idle_after_ms);
  void led_idle_();
  bool led_feedback_{false};
  bool led_custom_idle_{false};
  bool led_reenroll_pending_{false};
  uint8_t led_enroll_color_{0};  // != 0 while an enrollment started by this component runs
  CallbackManager<void()> led_idle_cb_;
  /// Size of this sensor's templates (learned while indexing; 0 = unknown).
  uint16_t template_size_{0};
  /// Hosts that could not be reached in the current round (", "-separated).
  std::string round_unreachable_;
  bool template_compatible_(Session &s, size_t len);      // after a hold: ignore admin matches until the finger is gone
  uint32_t finger_on_ms_{0};   // last time finger_sensor was on (debounced release)
  AdminArm armed_{AdminArm::NONE};
  uint32_t armed_until_ms_{0};
  uint16_t pending_guest_slot_{NO_SLOT};
  uint16_t pending_name_slot_{NO_SLOT};  // name given with enroll_next_free / enroll_guest
  std::string pending_name_;
  uint32_t pending_guest_expiry_{0};
  uint32_t last_expiry_check_ms_{0};
  bool last_round_ok_{true};
  std::vector<uint32_t> expires_;
  std::vector<uint8_t> uses_;                  // guest uses left per slot, 0 = unlimited
  std::map<uint16_t, std::string> names_;      // only slots that have a name use memory
  uint16_t epoch_{0};                          // version-reset generation (docs/protocol.md)
  std::vector<ESPPreferenceObject> expiry_prefs_;
  std::vector<ESPPreferenceObject> uses_prefs_;
  std::vector<ESPPreferenceObject> names_prefs_;
  bool names_dirty_{false};
  uint32_t restore_until_ms_{0};
  bool restore_open_{false};
  uint32_t full_backup_until_ms_{0};
  bool full_backup_open_{false};
  void send_local_list_(Session &s);
  uint32_t error_count_{0};
  const char *status_{""};

  // rounds
  bool round_active_{false};
  bool round_refreshing_{false};
  uint32_t round_started_ms_{0};
  uint32_t finger_since_ms_{0};
  bool finger_stuck_logged_{false};
  bool round_requested_{false};
  uint32_t round_due_ms_{0};
  size_t round_peer_idx_{0};
  uint8_t round_reachable_{0};
  uint8_t round_failures_{0};
  std::string round_last_error_;
  uint32_t session_gen_{0};
  Session inbound_;
  Session outbound_;

  CallbackManager<void()> sync_started_cb_;
  CallbackManager<void()> sync_done_cb_;
  CallbackManager<void(std::string)> sync_failed_cb_;
  CallbackManager<void(uint16_t)> conflict_cb_;
  CallbackManager<void(uint16_t)> slot_updated_cb_;
  CallbackManager<void(uint16_t)> slot_deleted_cb_;
  CallbackManager<void(uint16_t)> enroll_started_cb_;
  CallbackManager<void(uint8_t)> admin_touch_cb_;
  CallbackManager<void(uint8_t)> admin_gesture_cb_;
  CallbackManager<void(uint16_t, uint16_t)> admin_access_cb_;
  uint8_t admin_access_{0};
  uint16_t admin_first_slot_{0};
  uint16_t admin_first_confidence_{0};
  CallbackManager<void()> admin_hold_cb_;
  CallbackManager<void(std::string)> admin_waiting_cb_;
  CallbackManager<void()> admin_cancelled_cb_;
  CallbackManager<void(std::string, uint16_t)> admin_done_cb_;
};

}  // namespace esphome::fingerprint_grow_plus
