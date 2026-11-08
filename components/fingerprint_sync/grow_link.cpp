#include "grow_link.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome::fingerprint_sync {

static const char *const TAG = "fingerprint_sync.grow";

static constexpr uint8_t PID_COMMAND = 0x01;
static constexpr uint8_t PID_DATA = 0x02;
static constexpr uint8_t PID_ACK = 0x07;
static constexpr uint8_t PID_END_DATA = 0x08;

static constexpr uint8_t CMD_MATCH = 0x03;
static constexpr uint8_t CMD_STORE = 0x06;
static constexpr uint8_t CMD_LOAD_CHAR = 0x07;
static constexpr uint8_t CMD_UP_CHAR = 0x08;
static constexpr uint8_t CMD_DOWN_CHAR = 0x09;
static constexpr uint8_t CMD_DELETE_CHAR = 0x0C;
static constexpr uint8_t CMD_READ_SYS_PARA = 0x0F;
static constexpr uint8_t CMD_WRITE_NOTEPAD = 0x18;
static constexpr uint8_t CMD_READ_NOTEPAD = 0x19;
static constexpr uint8_t CMD_TEMPLATE_NUM = 0x1D;
static constexpr uint8_t CMD_READ_INDEX_TABLE = 0x1F;

static const char *command_name(uint8_t cmd) {
  switch (cmd) {
    case CMD_MATCH:
      return "Match";
    case CMD_STORE:
      return "Store";
    case CMD_LOAD_CHAR:
      return "LoadChar";
    case CMD_UP_CHAR:
      return "UpChar";
    case CMD_DOWN_CHAR:
      return "DownChar";
    case CMD_DELETE_CHAR:
      return "DeletChar";
    case CMD_READ_SYS_PARA:
      return "ReadSysPara";
    case CMD_WRITE_NOTEPAD:
      return "WriteNotepad";
    case CMD_READ_NOTEPAD:
      return "ReadNotepad";
    case CMD_TEMPLATE_NUM:
      return "TempleteNum";
    case CMD_READ_INDEX_TABLE:
      return "ReadIndexTable";
    default:
      return "command";
  }
}

void GrowLink::drain_() {
  uint8_t byte;
  while (this->uart_->available() > 0)
    this->uart_->read_byte(&byte);
}

void GrowLink::send_packet_(uint8_t pid, const uint8_t *payload, size_t len) {
  uint8_t header[9] = {0xEF,
                       0x01,
                       (uint8_t) (this->address_ >> 24),
                       (uint8_t) (this->address_ >> 16),
                       (uint8_t) (this->address_ >> 8),
                       (uint8_t) this->address_,
                       pid,
                       (uint8_t) ((len + 2) >> 8),
                       (uint8_t) (len + 2)};
  uint16_t sum = pid + header[7] + header[8];
  for (size_t i = 0; i < len; i++)
    sum += payload[i];
  uint8_t trailer[2] = {(uint8_t) (sum >> 8), (uint8_t) sum};
  this->uart_->write_array(header, sizeof(header));
  if (len > 0)
    this->uart_->write_array(payload, len);
  this->uart_->write_array(trailer, sizeof(trailer));
  this->bytes_moved_ += sizeof(header) + len + 2;
}

bool GrowLink::read_byte_(uint8_t &byte, uint32_t deadline) {
  uint32_t spins = 0;
  while (this->uart_->available() == 0) {
    if ((int32_t) (millis() - deadline) >= 0)
      return false;
    if (++spins % 64 == 0)
      App.feed_wdt();
    delayMicroseconds(100);
  }
  this->uart_->read_byte(&byte);
  this->last_rx_bytes_++;
  return true;
}

uint8_t GrowLink::receive_packet_(uint8_t &pid, std::vector<uint8_t> &payload, uint32_t timeout_ms) {
  const uint32_t deadline = millis() + timeout_ms;
  this->last_rx_bytes_ = 0;
  const uint8_t expected[6] = {0xEF,
                               0x01,
                               (uint8_t) (this->address_ >> 24),
                               (uint8_t) (this->address_ >> 16),
                               (uint8_t) (this->address_ >> 8),
                               (uint8_t) this->address_};
  uint8_t byte;
  size_t idx = 0;
  // Synchronise on header + address.
  while (idx < 6) {
    if (!this->read_byte_(byte, deadline))
      return GROW_TIMEOUT;
    if (byte == expected[idx]) {
      idx++;
    } else {
      idx = (byte == expected[0]) ? 1 : 0;
    }
  }
  uint8_t len_hi, len_lo;
  if (!this->read_byte_(pid, deadline) || !this->read_byte_(len_hi, deadline) || !this->read_byte_(len_lo, deadline))
    return GROW_TIMEOUT;
  uint16_t length = ((uint16_t) len_hi << 8) | len_lo;
  if (length < 2 || length > 256 + 2)
    return GROW_BAD_PACKET;
  payload.resize(length - 2);
  uint16_t sum = pid + len_hi + len_lo;
  for (size_t i = 0; i < payload.size(); i++) {
    if (!this->read_byte_(byte, deadline))
      return GROW_TIMEOUT;
    payload[i] = byte;
    sum += byte;
  }
  uint8_t sum_hi, sum_lo;
  if (!this->read_byte_(sum_hi, deadline) || !this->read_byte_(sum_lo, deadline))
    return GROW_TIMEOUT;
  this->bytes_moved_ += 9 + length;
  if ((((uint16_t) sum_hi << 8) | sum_lo) != sum) {
    ESP_LOGW(TAG, "Checksum mismatch in packet from sensor");
    return GROW_BAD_PACKET;
  }
  return GROW_OK;
}

uint8_t GrowLink::command_(const uint8_t *cmd, size_t len, std::vector<uint8_t> &ack, uint32_t timeout_ms) {
  if (this->uart_ == nullptr)
    return GROW_TIMEOUT;
  this->last_step_ = command_name(cmd[0]);
  this->drain_();
  this->send_packet_(PID_COMMAND, cmd, len);
  uint8_t pid;
  uint8_t res = this->receive_packet_(pid, ack, timeout_ms);
  if (res != GROW_OK)
    return res;
  if (pid != PID_ACK || ack.empty())
    return GROW_BAD_PACKET;
  return ack[0];
}

uint8_t GrowLink::read_params(GrowParams &params) {
  const uint8_t cmd[] = {CMD_READ_SYS_PARA};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  if (ack.size() < 17)
    return GROW_BAD_PACKET;
  auto word = [&ack](size_t index) -> uint16_t {
    return ((uint16_t) ack[1 + 2 * index] << 8) | ack[2 + 2 * index];
  };
  params.status = word(0);
  params.system_id = word(1);
  params.capacity = word(2);
  params.security_level = word(3);
  // word 4..5 = device address, word 6 = packet size code (0..3 -> 32/64/128/256 bytes)
  uint16_t code = word(6);
  params.packet_size = (uint16_t) (32u << (code > 3 ? 2 : code));
  this->packet_size_ = params.packet_size;
  return GROW_OK;
}

uint8_t GrowLink::read_index_page(uint8_t page, uint8_t out[32]) {
  const uint8_t cmd[] = {CMD_READ_INDEX_TABLE, page};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  if (ack.size() < 33)
    return GROW_BAD_PACKET;
  memcpy(out, ack.data() + 1, 32);
  return GROW_OK;
}

uint8_t GrowLink::template_count(uint16_t &count) {
  const uint8_t cmd[] = {CMD_TEMPLATE_NUM};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  if (ack.size() < 3)
    return GROW_BAD_PACKET;
  count = ((uint16_t) ack[1] << 8) | ack[2];
  return GROW_OK;
}

uint8_t GrowLink::load_char(uint8_t buffer, uint16_t slot) {
  const uint8_t cmd[] = {CMD_LOAD_CHAR, buffer, (uint8_t) (slot >> 8), (uint8_t) slot};
  std::vector<uint8_t> ack;
  return this->command_(cmd, sizeof(cmd), ack);
}

uint8_t GrowLink::up_char(uint8_t buffer, std::vector<uint8_t> &out) {
  const uint8_t cmd[] = {CMD_UP_CHAR, buffer};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  this->last_step_ = "UpChar data";
  out.clear();
  out.reserve(1024);
  std::vector<uint8_t> payload;
  while (true) {
    uint8_t pid;
    res = this->receive_packet_(pid, payload, 1000);
    if (res != GROW_OK)
      break;
    if (pid != PID_DATA && pid != PID_END_DATA) {
      res = GROW_BAD_PACKET;
      break;
    }
    if (out.size() + payload.size() > MAX_TEMPLATE_SIZE) {
      res = GROW_BAD_PACKET;
      break;
    }
    out.insert(out.end(), payload.begin(), payload.end());
    if (pid == PID_END_DATA)
      break;
  }
  if (!payload.empty())
    memset(payload.data(), 0, payload.size());
  this->last_template_size_ = out.size();
  if (res != GROW_OK) {
    if (!out.empty())
      memset(out.data(), 0, out.size());
    out.clear();
  }
  return res;
}

uint8_t GrowLink::down_char(uint8_t buffer, const std::vector<uint8_t> &data) {
  if (data.empty() || data.size() > MAX_TEMPLATE_SIZE)
    return GROW_BAD_PACKET;
  const uint8_t cmd[] = {CMD_DOWN_CHAR, buffer};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  this->last_step_ = "DownChar data";
  this->last_template_size_ = data.size();
  size_t pos = 0;
  const size_t chunk = this->packet_size_;
  while (pos < data.size()) {
    size_t n = data.size() - pos < chunk ? data.size() - pos : chunk;
    bool last = (pos + n) >= data.size();
    this->send_packet_(last ? PID_END_DATA : PID_DATA, data.data() + pos, n);
    pos += n;
    App.feed_wdt();
  }
  this->uart_->flush();
  return GROW_OK;
}

uint8_t GrowLink::store(uint8_t buffer, uint16_t slot) {
  const uint8_t cmd[] = {CMD_STORE, buffer, (uint8_t) (slot >> 8), (uint8_t) slot};
  std::vector<uint8_t> ack;
  return this->command_(cmd, sizeof(cmd), ack, 2000);
}

uint8_t GrowLink::delete_char(uint16_t slot, uint16_t count) {
  const uint8_t cmd[] = {CMD_DELETE_CHAR, (uint8_t) (slot >> 8), (uint8_t) slot, (uint8_t) (count >> 8),
                         (uint8_t) count};
  std::vector<uint8_t> ack;
  return this->command_(cmd, sizeof(cmd), ack, 2000);
}

uint8_t GrowLink::match(uint16_t &score) {
  const uint8_t cmd[] = {CMD_MATCH};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  score = ack.size() >= 3 ? (((uint16_t) ack[1] << 8) | ack[2]) : 0;
  return res;
}

uint8_t GrowLink::read_notepad(uint8_t page, uint8_t out[32]) {
  const uint8_t cmd[] = {CMD_READ_NOTEPAD, page};
  std::vector<uint8_t> ack;
  uint8_t res = this->command_(cmd, sizeof(cmd), ack);
  if (res != GROW_OK)
    return res;
  if (ack.size() < 33)
    return GROW_BAD_PACKET;
  memcpy(out, ack.data() + 1, 32);
  return GROW_OK;
}

uint8_t GrowLink::write_notepad(uint8_t page, const uint8_t data[32]) {
  uint8_t cmd[34];
  cmd[0] = CMD_WRITE_NOTEPAD;
  cmd[1] = page;
  memcpy(cmd + 2, data, 32);
  std::vector<uint8_t> ack;
  return this->command_(cmd, sizeof(cmd), ack, 2000);
}

uint8_t GrowLink::read_template(uint16_t slot, std::vector<uint8_t> &out, uint8_t buffer) {
  uint8_t res = this->load_char(buffer, slot);
  if (res != GROW_OK)
    return res;
  return this->up_char(buffer, out);
}

}  // namespace esphome::fingerprint_sync
