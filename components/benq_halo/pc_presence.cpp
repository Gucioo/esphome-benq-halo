#include "pc_presence.h"

#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)

#include <cerrno>
#include <cstring>
#include <strings.h>

#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#ifdef USE_MDNS
#include <mdns.h>
#endif

#include "esphome/core/log.h"

namespace esphome {
namespace benq_halo {

static const char *const TAG = "benq_halo.pc";

void PcPresence::start() {
  if (this->started_ || this->host_.empty())
    return;
  this->started_ = true;
  xTaskCreate(PcPresence::task_, "benq_pc", 4096, this, 1, nullptr);
}

void PcPresence::task_(void *arg) {
  static_cast<PcPresence *>(arg)->run_();
  vTaskDelete(nullptr);
}

void PcPresence::run_() {
  uint8_t fails = 0;
  while (true) {
    if (!this->net_up_.load()) {
      this->result_.store(-1);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    uint32_t ip = 0;
    bool resolved = this->resolve_(&ip);
    if (!resolved)
      ip = this->ip_.load();  // PC off often also means "no mDNS answer": try the last known IP
    bool up = false;
    if (ip != 0)
      up = this->port_ ? this->probe_tcp_(ip, this->port_) : this->probe_icmp_(ip);
    if (resolved && ip != this->ip_.load()) {
      this->ip_.store(ip);
      char s[16];
      inet_ntoa_r(*reinterpret_cast<struct in_addr *>(&ip), s, sizeof(s));
      ESP_LOGI(TAG, "%s resolved to %s", this->host_.c_str(), s);
    }
    // Online on the first answer; offline only after 2 misses in a row.
    if (up) {
      fails = 0;
      this->result_.store(1);
    } else if (++fails >= 2) {
      fails = 2;
      this->result_.store(0);
    }
    vTaskDelay(pdMS_TO_TICKS(this->interval_ms_));
  }
}

bool PcPresence::resolve_(uint32_t *ip) {
  struct in_addr a;
  if (inet_aton(this->host_.c_str(), &a)) {  // IP literal
    *ip = a.s_addr;
    return true;
  }
#ifdef USE_MDNS
  {
    // mDNS wants the bare name: "DESKTOP-ABC" for "DESKTOP-ABC.local".
    std::string name = this->host_;
    const size_t n = name.size();
    if (n > 6 && strcasecmp(name.c_str() + n - 6, ".local") == 0)
      name.resize(n - 6);
    if (name.find('.') == std::string::npos) {
      esp_ip4_addr_t addr{};
      if (mdns_query_a(name.c_str(), 2000, &addr) == ESP_OK && addr.addr != 0) {
        *ip = addr.addr;
        return true;
      }
    }
  }
#endif
  // Router DNS often knows DHCP host names too.
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(this->host_.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
    *ip = reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return *ip != 0;
  }
  if (res != nullptr)
    freeaddrinfo(res);
  return false;
}

namespace {
struct PingCtx {
  SemaphoreHandle_t done;
  volatile bool ok;
};
void on_ping_success(esp_ping_handle_t, void *arg) { static_cast<PingCtx *>(arg)->ok = true; }
void on_ping_end(esp_ping_handle_t, void *arg) { xSemaphoreGive(static_cast<PingCtx *>(arg)->done); }
}  // namespace

bool PcPresence::probe_icmp_(uint32_t ip) {
  PingCtx ctx{xSemaphoreCreateBinary(), false};
  if (ctx.done == nullptr)
    return false;
  // Fields set one by one: ESP_PING_DEFAULT_CONFIG() is a C designated initializer.
  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.data_size = 64;
  cfg.tos = 0;
  cfg.ttl = 64;
  cfg.task_prio = 2;
  cfg.interface = 0;
  ip4_addr_t v4;
  v4.addr = ip;
  IP_ADDR4(&cfg.target_addr, ip4_addr1(&v4), ip4_addr2(&v4), ip4_addr3(&v4), ip4_addr4(&v4));
  cfg.count = 2;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1000;
  cfg.task_stack_size = 3072;
  esp_ping_callbacks_t cbs{};
  cbs.cb_args = &ctx;
  cbs.on_ping_success = on_ping_success;
  cbs.on_ping_end = on_ping_end;
  esp_ping_handle_t ping = nullptr;
  bool ok = false;
  if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
    esp_ping_start(ping);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(5000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    ok = ctx.ok;
  } else {
    ESP_LOGW(TAG, "Could not create ping session");
  }
  vSemaphoreDelete(ctx.done);
  return ok;
}

bool PcPresence::probe_tcp_(uint32_t ip, uint16_t port) {
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0)
    return false;
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  struct sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = ip;
  bool up = false;
  int r = connect(s, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
  if (r == 0) {
    up = true;
  } else if (errno == ECONNREFUSED) {
    up = true;  // RST: the host is there, the port is just closed
  } else if (errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv{1, 500000};
    if (select(s + 1, nullptr, &w, nullptr, &tv) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
      up = (err == 0 || err == ECONNREFUSED);
    }
  }
  close(s);
  return up;
}

}  // namespace benq_halo
}  // namespace esphome

#endif
