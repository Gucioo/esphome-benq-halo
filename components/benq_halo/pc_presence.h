#pragma once
// PC presence probe for benq_halo: resolves a host name (mDNS "DESKTOP-xxxx"
// / ".local", then normal DNS, or an IP literal) and probes it with ICMP ping
// (port 0) or a TCP connect (port > 0; a refused connection still proves the
// host is up). Runs in its own FreeRTOS task so the main loop never blocks on
// DNS/mDNS/ping timeouts. ESP32 only.
#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)

#include <atomic>
#include <cstdint>
#include <string>

namespace esphome {
namespace benq_halo {

class PcPresence {
 public:
  void configure(const std::string &host, uint32_t interval_ms, uint16_t port) {
    this->host_ = host;
    this->interval_ms_ = interval_ms;
    this->port_ = port;
  }
  void start();
  // Set by the main loop: probes only run while the network is up.
  void set_network_up(bool up) { this->net_up_.store(up); }
  // -1 unknown (not probed yet / network down), 0 offline, 1 online.
  int8_t result() const { return this->result_.load(); }
  const std::string &host() const { return this->host_; }
  uint16_t port() const { return this->port_; }
  // Last resolved IPv4 (network byte order), 0 if never resolved.
  uint32_t last_ip() const { return this->ip_.load(); }

 protected:
  static void task_(void *arg);
  void run_();
  bool resolve_(uint32_t *ip);
  bool probe_icmp_(uint32_t ip);
  bool probe_tcp_(uint32_t ip, uint16_t port);

  std::string host_;
  uint32_t interval_ms_{10000};
  uint16_t port_{0};
  std::atomic<bool> net_up_{false};
  std::atomic<int8_t> result_{-1};
  std::atomic<uint32_t> ip_{0};
  bool started_{false};
};

}  // namespace benq_halo
}  // namespace esphome

#endif
