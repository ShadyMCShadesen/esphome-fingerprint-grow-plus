#include "meta_storage.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome::fingerprint_grow_plus {

static const char *const TAG = "fingerprint_grow_plus.store";
static const uint8_t NOTEPAD_MAGIC[4] = {'F', 'P', 'S', 'M'};

void PreferenceStorage::ensure_prefs_(size_t chunks) {
  while (this->prefs_.size() < chunks) {
    uint32_t key = this->key_base_ + (uint32_t) this->prefs_.size();
    this->prefs_.push_back(global_preferences->make_preference<Chunk>(key, true));
  }
}

bool PreferenceStorage::load(std::vector<uint16_t> &meta) {
  size_t chunks = (meta.size() + SLOTS_PER_CHUNK - 1) / SLOTS_PER_CHUNK;
  this->ensure_prefs_(chunks);
  for (size_t c = 0; c < chunks; c++) {
    Chunk chunk{};
    if (!this->prefs_[c].load(&chunk))
      continue;  // never saved: all slots unknown
    for (size_t i = 0; i < SLOTS_PER_CHUNK; i++) {
      size_t slot = c * SLOTS_PER_CHUNK + i;
      if (slot < meta.size())
        meta[slot] = chunk[i];
    }
  }
  return true;
}

bool PreferenceStorage::save(const std::vector<uint16_t> &meta) {
  size_t chunks = (meta.size() + SLOTS_PER_CHUNK - 1) / SLOTS_PER_CHUNK;
  this->ensure_prefs_(chunks);
  bool ok = true;
  for (size_t c = 0; c < chunks; c++) {
    Chunk chunk{};
    for (size_t i = 0; i < SLOTS_PER_CHUNK; i++) {
      size_t slot = c * SLOTS_PER_CHUNK + i;
      chunk[i] = slot < meta.size() ? meta[slot] : 0;
    }
    ok &= this->prefs_[c].save(&chunk);
  }
  // Tombstones must survive a power loss right after a delete.
  ok &= global_preferences->sync();
  return ok;
}

void NotepadStorage::render_(const std::vector<uint16_t> &meta, uint8_t *image) {
  memset(image, 0, PAGES * PAGE_SIZE);
  memcpy(image, NOTEPAD_MAGIC, 4);
  image[4] = (uint8_t) (meta.size() >> 8);
  image[5] = (uint8_t) meta.size();
  for (size_t slot = 0; slot < meta.size() && slot < this->max_slots(); slot++) {
    image[HEADER_SIZE + 2 * slot] = (uint8_t) (meta[slot] >> 8);
    image[HEADER_SIZE + 2 * slot + 1] = (uint8_t) meta[slot];
  }
}

bool NotepadStorage::load(std::vector<uint16_t> &meta) {
  for (uint8_t page = 0; page < PAGES; page++) {
    if (this->link_->read_notepad(page, this->image_ + page * PAGE_SIZE) != GROW_OK) {
      ESP_LOGE(TAG, "Reading notepad page %u failed", page);
      return false;
    }
  }
  this->image_valid_ = true;
  if (memcmp(this->image_, NOTEPAD_MAGIC, 4) != 0) {
    ESP_LOGI(TAG, "Sensor notepad holds no sync metadata yet (new or replaced sensor)");
    return true;
  }
  for (size_t slot = 0; slot < meta.size() && slot < this->max_slots(); slot++) {
    meta[slot] = ((uint16_t) this->image_[HEADER_SIZE + 2 * slot] << 8) | this->image_[HEADER_SIZE + 2 * slot + 1];
  }
  return true;
}

bool NotepadStorage::save(const std::vector<uint16_t> &meta) {
  uint8_t image[PAGES * PAGE_SIZE];
  this->render_(meta, image);
  for (uint8_t page = 0; page < PAGES; page++) {
    const uint8_t *data = image + page * PAGE_SIZE;
    if (this->image_valid_ && memcmp(data, this->image_ + page * PAGE_SIZE, PAGE_SIZE) == 0)
      continue;  // unchanged, spare the flash
    if (this->link_->write_notepad(page, data) != GROW_OK) {
      ESP_LOGE(TAG, "Writing notepad page %u failed", page);
      this->image_valid_ = false;
      return false;
    }
    memcpy(this->image_ + page * PAGE_SIZE, data, PAGE_SIZE);
  }
  this->image_valid_ = true;
  return true;
}

}  // namespace esphome::fingerprint_grow_plus
