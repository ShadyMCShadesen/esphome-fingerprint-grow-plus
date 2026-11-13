#pragma once

// Minimal driver for the template related commands of GROW (Hangzhou Grow) R30x/R50x
// fingerprint modules. It shares the UART with ESPHome's fingerprint_grow component.
//
// Every public method performs one complete, synchronous transaction (command, answer and
// any data packets). Because ESPHome runs all components cooperatively in a single
// thread, a transaction can never interleave with a fingerprint_grow transaction as long
// as it finishes inside one loop() call. Never split a transaction across loop() calls.
//
// Reference: R503 user manual (Hangzhou Grow), sections "Data package format", UpChar (0x08),
// DownChar (0x09), Store (0x06), LoadChar (0x07), DeletChar (0x0C), Match (0x03),
// ReadSysPara (0x0F), TempleteNum (0x1D), ReadIndexTable (0x1F), WriteNotepad (0x18),
// ReadNotepad (0x19). SetPwd (0x12) and SetSysPara (0x0E) are intentionally NOT implemented.

#include "esphome/components/uart/uart.h"

#include <cstdint>
#include <vector>

namespace esphome::fingerprint_grow_plus {

enum GrowCode : uint8_t {
  GROW_OK = 0x00,
  GROW_PACKET_RCV_ERR = 0x01,
  GROW_NO_MATCH = 0x08,
  GROW_BAD_LOCATION = 0x0B,
  GROW_READ_TEMPLATE_ERR = 0x0C,
  GROW_UPLOAD_FEATURE_FAIL = 0x0D,
  GROW_PACKET_RESPONSE_FAIL = 0x0E,
  GROW_DELETE_FAIL = 0x10,
  GROW_FLASH_ERR = 0x18,
  GROW_BAD_NOTEPAD_PAGE = 0x1C,
  // Local pseudo codes (never sent by the module)
  GROW_BAD_PACKET = 0xFE,
  GROW_TIMEOUT = 0xFF,
};

struct GrowParams {
  uint16_t status{0};
  uint16_t system_id{0};
  uint16_t capacity{0};
  uint16_t security_level{0};
  uint16_t packet_size{128};
};

class GrowLink {
 public:
  /// Upper bound for a template upload; the R503 manual specifies 768 bytes.
  static constexpr size_t MAX_TEMPLATE_SIZE = 2048;

  void set_uart(uart::UARTDevice *uart) { this->uart_ = uart; }

  uint8_t read_params(GrowParams &params);
  uint8_t read_index_page(uint8_t page, uint8_t out[32]);
  uint8_t template_count(uint16_t &count);
  uint8_t load_char(uint8_t buffer, uint16_t slot);
  /// Upload CharBuffer `buffer` to the host (UpChar).
  uint8_t up_char(uint8_t buffer, std::vector<uint8_t> &out);
  /// Download `data` into CharBuffer `buffer` (DownChar).
  uint8_t down_char(uint8_t buffer, const std::vector<uint8_t> &data);
  /// Raw ReadSysPara answer (16 parameter bytes) of the last read_params() call.
  const uint8_t *last_params_raw() const { return this->last_params_raw_; }
  /// Search the library slots [start, start + count) for the features in CharBuffer `buffer`
  /// (the module's normal 1:N matching). GROW_OK: `slot` and `score` are set; 0x09: no match.
  uint8_t search(uint8_t buffer, uint16_t start, uint16_t count, uint16_t &slot, uint16_t &score);
  uint8_t store(uint8_t buffer, uint16_t slot);
  uint8_t delete_char(uint16_t slot, uint16_t count);
  uint8_t read_notepad(uint8_t page, uint8_t out[32]);
  uint8_t write_notepad(uint8_t page, const uint8_t data[32]);

  /// Convenience: LoadChar + UpChar.
  uint8_t read_template(uint16_t slot, std::vector<uint8_t> &out, uint8_t buffer = 1);

  /// Diagnostics for the most recent failure: the step that failed and how many bytes of the
  /// (incomplete) answer had arrived. 0 bytes means the module did not answer at all.
  const char *last_step() const { return this->last_step_; }
  size_t last_rx_bytes() const { return this->last_rx_bytes_; }

 protected:
  void drain_();
  void send_packet_(uint8_t pid, const uint8_t *payload, size_t len);
  bool read_byte_(uint8_t &byte, uint32_t deadline);
  /// Receive one packet. On success `pid` and `payload` (without checksum) are set.
  uint8_t receive_packet_(uint8_t &pid, std::vector<uint8_t> &payload, uint32_t timeout_ms);
  /// Send a command packet and wait for the acknowledge packet.
  uint8_t command_(const uint8_t *cmd, size_t len, std::vector<uint8_t> &ack, uint32_t timeout_ms = 1000);

  uart::UARTDevice *uart_{nullptr};
  /// Module address in every packet; fingerprint_grow always uses the default 0xFFFFFFFF.
  static constexpr uint32_t ADDRESS{0xFFFFFFFF};
  uint16_t packet_size_{128};
  const char *last_step_{""};
  size_t last_rx_bytes_{0};
  uint8_t last_params_raw_[16]{};
};

}  // namespace esphome::fingerprint_grow_plus
