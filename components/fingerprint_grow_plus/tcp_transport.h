#pragma once

#include "esphome/core/defines.h"
#include "transport.h"

#include "esphome/components/socket/tcp_client_link.h"
#include "esphome/components/socket/tcp_listener.h"

namespace esphome::fingerprint_grow_plus {

/// Stream adapter around ESPHome's TcpClientLink.
class TcpStream final : public Stream {
 public:
  explicit TcpStream(socket::TcpClientLink *link, bool *connecting = nullptr) : link_(link), connecting_(connecting) {}
  bool connected() const override { return this->link_->connected(); }
  int32_t read(uint8_t *buf, size_t len) override { return (int32_t) this->link_->read(buf, len); }
  size_t write(const uint8_t *buf, size_t len) override { return this->link_->queue(buf, len); }
  bool flush() override { return this->link_->flush_tx(); }
  void close() override {
    if (this->connecting_ != nullptr)
      *this->connecting_ = false;  // stop reconnect attempts of an outbound link
    this->link_->close();
  }

 protected:
  socket::TcpClientLink *link_;
  bool *connecting_;
};

/// TCP transport: one listening socket (one inbound peer at a time) and one outbound
/// connection. Works on ESP32 (BSD/lwIP sockets) and ESP8266 (raw lwIP TCP).
class TcpTransport final : public Transport {
 public:
  void set_port(uint16_t port) { this->port_ = port; }
#ifdef USE_SOCKET_IPV4_ALLOW
  void set_allow(const socket::Ipv4AllowEntry *entries, size_t count) { this->listener_.set_allow(entries, count); }
#endif

  void setup(const char *log_tag) override;
  void loop(bool may_accept) override;
  Stream *inbound() override { return &this->inbound_stream_; }
  void connect(const char *host, uint16_t port) override;
  Stream *outbound() override { return &this->outbound_stream_; }
  void dump_config() override;

 protected:
  const char *tag_{""};
  uint16_t port_{18530};
  bool connecting_{false};
  socket::TcpListener listener_;
  socket::TcpClientLink inbound_link_;
  socket::TcpClientLink outbound_link_;
  TcpStream inbound_stream_{&inbound_link_};
  TcpStream outbound_stream_{&outbound_link_, &connecting_};
};

}  // namespace esphome::fingerprint_grow_plus
