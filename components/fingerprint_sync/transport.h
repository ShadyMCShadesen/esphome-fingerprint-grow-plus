#pragma once

// Transport abstraction. The sync logic only needs a reliable, ordered byte stream to a
// peer plus the ability to accept one inbound stream; everything above (record layer,
// encryption, sync protocol) is transport independent. TcpTransport is the only
// implementation today; it works inside one subnet as well as across routed networks.

#include <cstddef>
#include <cstdint>

namespace esphome::fingerprint_sync {

class Stream {
 public:
  virtual ~Stream() = default;
  virtual bool connected() const = 0;
  /// Bytes read, 0 if nothing is available now, -1 if the stream dropped.
  virtual int32_t read(uint8_t *buf, size_t len) = 0;
  /// Queue bytes for sending; returns how many were accepted.
  virtual size_t write(const uint8_t *buf, size_t len) = 0;
  /// Push queued bytes to the network; true once nothing is pending.
  virtual bool flush() = 0;
  virtual void close() = 0;
};

class Transport {
 public:
  virtual ~Transport() = default;
  virtual void setup(const char *log_tag) = 0;
  /// Drive the transport; `may_accept` is false while an inbound session is active.
  virtual void loop(bool may_accept) = 0;
  /// The inbound stream (check connected()).
  virtual Stream *inbound() = 0;
  /// Begin connecting the outbound stream to host:port (hostname, mDNS name or IPv4).
  virtual void connect(const char *host, uint16_t port) = 0;
  /// The outbound stream (check connected()).
  virtual Stream *outbound() = 0;
  virtual void dump_config() = 0;
};

}  // namespace esphome::fingerprint_sync
