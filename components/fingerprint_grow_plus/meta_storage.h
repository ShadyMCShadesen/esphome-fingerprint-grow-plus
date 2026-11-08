#pragma once

// Persistent storage for the per-slot sync metadata (state + version, 2 bytes per slot).
//
// - PreferenceStorage: ESPHome preferences (NVS on ESP32). Default on ESP32.
// - NotepadStorage:   the sensor's own 512 byte notepad (16 pages x 32 bytes, R503 manual
//                      "Notepad"). Default on ESP8266, whose flash preference area is too
//                      small. Keeps the metadata with the sensor, so a replaced sensor
//                      automatically starts without stale metadata. Up to 248 slots.

#include "esphome/core/preferences.h"
#include "grow_link.h"

#include <array>
#include <cstdint>
#include <vector>

namespace esphome::fingerprint_grow_plus {

class MetaStorage {
 public:
  virtual ~MetaStorage() = default;
  /// Load metadata for `meta.size()` slots. Unknown slots stay 0. Returns false on I/O error.
  virtual bool load(std::vector<uint16_t> &meta) = 0;
  /// Persist the metadata. Returns false on I/O error.
  virtual bool save(const std::vector<uint16_t> &meta) = 0;
  /// Maximum number of slots this backend can hold.
  virtual size_t max_slots() const = 0;
  virtual const char *name() const = 0;
  /// True if load()/save() talk to the sensor (must run as a sensor job).
  virtual bool uses_sensor() const = 0;
};

class PreferenceStorage final : public MetaStorage {
 public:
  static constexpr size_t SLOTS_PER_CHUNK = 64;
  static constexpr size_t MAX_CHUNKS = 16;
  explicit PreferenceStorage(uint32_t key_base) : key_base_(key_base) {}
  bool load(std::vector<uint16_t> &meta) override;
  bool save(const std::vector<uint16_t> &meta) override;
  size_t max_slots() const override { return SLOTS_PER_CHUNK * MAX_CHUNKS; }
  const char *name() const override { return "preferences"; }
  bool uses_sensor() const override { return false; }

 protected:
  using Chunk = std::array<uint16_t, SLOTS_PER_CHUNK>;
  void ensure_prefs_(size_t chunks);
  uint32_t key_base_;
  std::vector<ESPPreferenceObject> prefs_;
};

class NotepadStorage final : public MetaStorage {
 public:
  static constexpr size_t PAGES = 16;
  static constexpr size_t PAGE_SIZE = 32;
  static constexpr size_t HEADER_SIZE = 16;
  explicit NotepadStorage(GrowLink *link) : link_(link) {}
  bool load(std::vector<uint16_t> &meta) override;
  bool save(const std::vector<uint16_t> &meta) override;
  size_t max_slots() const override { return (PAGES * PAGE_SIZE - HEADER_SIZE) / 2; }
  const char *name() const override { return "sensor notepad"; }
  bool uses_sensor() const override { return true; }

 protected:
  void render_(const std::vector<uint16_t> &meta, uint8_t *image);
  GrowLink *link_;
  uint8_t image_[PAGES * PAGE_SIZE]{};
  bool image_valid_{false};
};

}  // namespace esphome::fingerprint_grow_plus
