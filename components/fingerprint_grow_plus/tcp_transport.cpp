#include "tcp_transport.h"

#include "esphome/core/log.h"

namespace esphome::fingerprint_grow_plus {

void TcpTransport::setup(const char *log_tag) {
  this->tag_ = log_tag;
  this->inbound_link_.set_port(this->port_);
  this->inbound_link_.begin(log_tag);
  this->listener_.begin(log_tag);
  // Retries of one outbound attempt happen inside the session's own connect timeout.
  this->outbound_link_.set_reconnect_interval(3000);
}

void TcpTransport::loop(bool may_accept) {
  this->listener_.poll(this->inbound_link_, may_accept);
  if (this->connecting_) {
    this->outbound_link_.poll();
    if (this->outbound_link_.connected())
      this->connecting_ = false;
  }
}

void TcpTransport::connect(const char *host, uint16_t port) {
  this->outbound_link_.close();
  this->outbound_link_.set_host(host);
  this->outbound_link_.set_port(port);
  this->outbound_link_.begin(this->tag_);
  this->connecting_ = true;
}

void TcpTransport::dump_config() {
  ESP_LOGCONFIG(this->tag_, "  Transport: TCP, port %u", this->port_);
  this->listener_.dump_config();
}

}  // namespace esphome::fingerprint_grow_plus
