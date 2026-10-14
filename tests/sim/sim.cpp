// Behavioural simulation of the benq_halo hub against a fake BC5602 + fake lamp.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#include "esphome/components/benq_halo/benq_halo.h"
#include "esphome/core/preferences.h"
#include "esphome/components/host/preferences.h"
using namespace esphome;
using namespace esphome::benq_halo;
using namespace esphome::benq_halo::proto;

// ---------------- fake clock ----------------
static uint64_t g_us = 1000000;
namespace esphome {
uint32_t millis() { return (uint32_t) (g_us / 1000); }
uint32_t micros() { return (uint32_t) g_us; }
void delay(uint32_t ms) { g_us += (uint64_t) ms * 1000; }
void delayMicroseconds(uint32_t us) { g_us += us; }
static bool g_quiet = false;
void esp_log_printf_(int level, const char *tag, int line, const char *fmt, ...) {
  if (g_quiet) return;
  char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  printf("  [%7.3f] %s\n", g_us / 1e6, b);
}
void log_pin(const char *, const char *, GPIOPin *) {}
// Component status stubs
void Component::status_set_error(const LogString *) {}
void Component::status_clear_error_slow_path_() {}
void Component::setup() {}
void Component::loop() {}
void Component::dump_config() {}
float Component::get_setup_priority() const { return 0; }
bool Component::can_proceed() { return true; }
void Component::call_setup() {}
void *callback_manager_grow(void *data, uint16_t size, uint16_t &capacity, size_t elem_size) {
  uint16_t new_cap = size + 1;
  auto *new_data = ::operator new(new_cap * elem_size);
  if (data) { __builtin_memcpy(new_data, data, size * elem_size); ::operator delete(data); }
  capacity = new_cap;
  return new_data;
}
uint32_t fnv1_hash(const char *) { return 1; }
uint8_t HighFrequencyLoopRequester::num_requests = 0;
void HighFrequencyLoopRequester::start() { num_requests++; }
void HighFrequencyLoopRequester::stop() { num_requests--; }
// Preferences: in-memory (HostPreferences without its file backing)
namespace host {
static HostPreferences s_prefs;
HostPreferences *get_preferences() { return &s_prefs; }
void HostPreferences::setup_() { setup_complete_ = true; }
bool HostPreferences::sync() { return true; }
bool HostPreferences::reset() { data_.clear(); return true; }
ESPPreferenceObject HostPreferences::make_preference(size_t, uint32_t type, bool) {
  return ESPPreferenceObject(new HostPreferenceBackend(type));
}
bool HostPreferenceBackend::save(const uint8_t *data, size_t len) const { return s_prefs.save(key_, data, len); }
bool HostPreferenceBackend::load(uint8_t *data, size_t len) const { return s_prefs.load(key_, data, len); }
}  // namespace host
ESPPreferences *global_preferences = &host::s_prefs;
}  // namespace esphome

// ---------------- fake BC5602 + lamp ----------------
struct Fake : spi::FakeBus {
  uint8_t bank = 0, regs[4][64] = {};
  bool cs = false; std::vector<uint8_t> tr;
  bool fourwire = false;
  bool ce = false; uint8_t irq = 0; bool prm_rx = false; uint8_t omst = 2;
  uint8_t addr[4] = {};
  std::vector<uint8_t> txfifo; std::deque<std::vector<uint8_t>> rxq; std::vector<uint8_t> rxfifo;
  // lamp model
  uint8_t lamp_addr[4] = {0x4F, 0xF0, 0xFD, 0x63};
  bool lamp_on = true; int lamp_drop = 0;      // lamp powered, ACKs to drop
  bool lamp_pairing = false; uint8_t paired_from[2] = {};
  std::vector<Payload> lamp_rx;                // frames the lamp accepted
  int sends = 0;
  uint8_t &R(uint8_t r) { return r < 0x20 ? regs[0][r] : regs[bank][r]; }
  void begin() override { cs = true; tr.clear(); }
  uint8_t xfer(uint8_t b) override {
    uint8_t out = 0;
    if (!tr.empty()) {
      uint8_t c = tr[0];
      if ((c & 0xC0) == 0xC0 && c != 0xDF && c != 0xFF) {  // read reg
        uint8_t r = c & 0x3F; out = fourwire ? rd(r) : 0xFF;
      } else if (c == 0xDF) out = fourwire ? 0x56 : 0xFF;  // chip version 0x56xxxx
      else if (c == 0xBF) { size_t i = tr.size() - 1; out = i < rxfifo.size() ? rxfifo[i] : 0; }
    }
    tr.push_back(b);
    return out;
  }
  uint8_t rd(uint8_t r) {
    if (r == 0x00) return (R(0) & 0xFC) | bank;
    if (r == 0x01) return 0x20;  // XCLK ready
    if (r == 0x04) return irq;
    if (r == 0x05) return txfifo.empty() ? 0x10 : 0x00;
    if (r == 0x20 && bank == 0) return 0;  // ACAL done
    if (r == 0x26 && bank == 0) return omst;
    return R(r);
  }
  void end() override {
    cs = false; if (tr.empty()) return;
    uint8_t c = tr[0];
    if (tr.size() == 1) {
      switch (c) {
        case 0x08: fourwire = false; bank = 0; memset(regs, 0, sizeof regs); ce = false; omst = 2; irq = 0; break;
        case 0x09: txfifo.clear(); break;
        case 0x89: rxfifo.clear(); break;
        case 0x0C: omst = 2; break;
        case 0x8E: if (R(0x03) & 1) { omst = 5; deliver(); } break;
      }
      return;
    }
    if ((c & 0xC0) == 0x40 && tr.size() == 2) {  // write reg
      uint8_t r = c & 0x3F, v = tr[1];
      if (r == 0x00) { bank = v & 3; R(0) = v; return; }
      if (r == 0x06) { fourwire = (v == 0x48); }
      if (r == 0x04) { irq &= ~v; return; }
      if (r == 0x15) { ce = v & 1; if (ce) fire(); return; }
      R(r) = v; return;
    }
    if (c == 0x10 && tr.size() == 5) { memcpy(addr, &tr[1], 4); return; }
    if (c == 0x11) { txfifo.assign(tr.begin() + 1, tr.end()); return; }
  }
  void deliver() {
    if (omst == 5 && !rxq.empty() && !(irq & 0x40)) { rxfifo = rxq.front(); rxq.pop_front(); irq |= 0x40; omst = 2; }
  }
  void fire() {  // CE=1 in PTX with a frame in the FIFO
    if ((R(0x03) & 1) || txfifo.empty()) return;
    sends++;
    static const uint8_t PAIR[4] = {0xB0, 0x00, 0x01, 0x59};
    bool ack = false;
    if (!memcmp(addr, PAIR, 4)) {
      if (lamp_pairing) { ack = true; paired_from[0] = txfifo[0]; }
    } else if (lamp_on && !memcmp(addr, lamp_addr, 4)) {
      if (lamp_drop > 0) lamp_drop--; else { ack = true; lamp_rx.push_back({txfifo[0], txfifo[1]}); }
    }
    irq |= ack ? 0x20 : 0x10;
    txfifo.clear();
  }
  // Remote frame on air, as the passive listener would read it.
  void remote(uint8_t f, uint8_t v) {
    uint8_t air[4]; reg_to_air(lamp_addr, air);
    const uint8_t pay[2] = {f, v}; uint8_t raw[8]; encode_air(air, 1, false, pay, 2, raw);
    rxq.push_back(std::vector<uint8_t>(raw, raw + 8)); deliver();
  }
};
Fake fake;
spi::FakeBus *spi::g_fake_bus = &fake;

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("PASS  %s\n", msg); else { printf("FAIL  %s\n", msg); fails++; } } while (0)

static void run(BenqHaloComponent &h, uint32_t ms) {
  uint64_t end = g_us + (uint64_t) ms * 1000;
  while (g_us < end) { h.loop(); g_us += 2000; }
}

int main() {
  BenqHaloComponent h;
  State last{}; int pubs = 0; bool link = false; std::string frame;
  h.add_on_state_callback([&](const State &s) { last = s; pubs++; });
  h.add_on_link_callback([&](bool ok) { link = ok; });
  h.add_on_frame_callback([&](const std::string &s) { frame = s; });
  h.setup();
  h.dump_config();
  run(h, 2500);
  CHECK(h.ready() && pubs == 1, "boot: ready after 2 s, believed state published once");
  CHECK(fake.lamp_rx.empty(), "boot: nothing transmitted at startup");

  // 1. Turn on at 50 %, 4000 K-ish.
  printf("-- turn on 50%%\n");
  h.light_request(true, 0.5f, 250.0f);
  run(h, 1500);
  CHECK(fake.lamp_rx.size() >= 3, "on: burst of >= 3 frames acknowledged");
  int c5 = 0; for (auto &p : fake.lamp_rx) c5 += (p.flags == 0xC5 && p.value == h.raw_from_brightness(0.5f));
  CHECK(c5 >= 3, "on: >= 3 x C5 (on, both lamps, brightness 50%)");
  // temp 250 mired -> unchanged? default temp 0x35 -> 268 mired; 250 -> different -> temp frame expected too
  int temps = 0; for (auto &p : fake.lamp_rx) temps += (p.flags == 0xC3);
  printf("   frames: "); for (auto &p : fake.lamp_rx) printf("%02X%02X ", p.flags, p.value); printf("\n");
  CHECK(link, "link sensor ON after ACKs");

  // 2. Slider sweep: only the final value gets a full burst.
  printf("-- slider sweep\n");
  fake.lamp_rx.clear();
  for (int i = 0; i < 10; i++) { h.light_request(true, 0.1f + i * 0.09f, 250.0f); run(h, 20); }
  run(h, 1500);
  const uint8_t final_raw = h.raw_from_brightness(0.1f + 9 * 0.09f);
  int finals = 0; for (auto &p : fake.lamp_rx) finals += (p.flags == 0xC5 && p.value == final_raw);
  printf("   %zu frames sent, %d with the final value %02X\n", fake.lamp_rx.size(), finals, final_raw);
  CHECK(finals >= 3 && fake.lamp_rx.back().value == final_raw, "sweep: last value delivered with a full burst");

  // 3. Turn off: temp frame with power bit clear.
  printf("-- off\n");
  fake.lamp_rx.clear();
  h.light_request(false, 0.91f, 250.0f);
  run(h, 1500);
  CHECK(!fake.lamp_rx.empty() && fake.lamp_rx[0].flags == 0x43, "off: 43 xx frames (off, both, temp selector)");

  // 4. Remote dial while ESP listens: state follows, nothing re-sent.
  printf("-- remote: on, front only, brightness E0\n");
  fake.lamp_rx.clear(); pubs = 0;
  fake.remote(0xC4, 0xE0); run(h, 120); fake.remote(0xC4, 0xE0); run(h, 600);
  CHECK(pubs >= 1 && last.power && last.lamps == F_FRONT && last.bright == 0xE0, "remote: HA state follows (on, front, E0)");
  // Echo through the light (as LightState would do after publish):
  h.light_request(true, h.brightness_from_raw(0xE0), BenqHaloComponent::mireds_from_temp(last.temp));
  run(h, 800);
  CHECK(fake.lamp_rx.empty(), "remote: echo of the published state sends nothing");
  CHECK(frame.find("C4 E0") != std::string::npos, "last_frame text sensor shows 'C4 E0'");

  // 5. Lamp select while on -> C6? no: front+back+bright = C5
  printf("-- select Both\n");
  h.set_lamps(F_LAMPS); run(h, 1500);
  CHECK(!fake.lamp_rx.empty() && fake.lamp_rx.back().flags == 0xC5 && fake.lamp_rx.back().value == 0xE0,
        "select both: C5 E0 sent");

  // 6. Button A.
  printf("-- button A\n");
  fake.lamp_rx.clear(); h.press_auto(); run(h, 1500);
  CHECK(!fake.lamp_rx.empty() && fake.lamp_rx[0].flags == 0xE1 && fake.lamp_rx[0].value == 1, "auto: E1 01");
  fake.lamp_rx.clear(); h.press_auto(); run(h, 1500);
  CHECK(!fake.lamp_rx.empty() && fake.lamp_rx[0].value == 2, "auto: next press numbered 02");

  // 7. Lamp unplugged: retries then give-up, HA reverts.
  printf("-- lamp unplugged\n");
  fake.lamp_on = false; State before = h.target(); pubs = 0;
  h.light_request(true, 0.2f, 300.0f);
  run(h, 8000);
  CHECK(!link && h.target() == before && pubs >= 1, "unreachable: gives up, target reverts, link OFF");

  // 8. Flaky lamp: 2 ACKs dropped, still delivered.
  printf("-- flaky lamp\n");
  fake.lamp_on = true; fake.lamp_drop = 2; fake.lamp_rx.clear();
  h.light_request(true, 0.3f, 300.0f);
  run(h, 5000);
  CHECK(link && h.target().bright == h.raw_from_brightness(0.3f), "flaky: delivered after retries, link back ON");

  // 9. Conversions round trip exactly.
  bool rt = true;
  for (int r = BRIGHT_MIN; r <= BRIGHT_MAX; r++) rt &= h.raw_from_brightness(h.brightness_from_raw(r)) == r;
  for (int t = 0; t <= TEMP_MAX; t++) rt &= BenqHaloComponent::temp_from_mireds(BenqHaloComponent::mireds_from_temp(t)) == t;
  CHECK(rt, "brightness/temperature conversions round-trip for every raw value");

  // 10. Pairing replay.
  printf("-- pairing\n");
  g_quiet = false;
  fake.lamp_pairing = true; fake.lamp_rx.clear();
  int sends0 = fake.sends;
  h.start_pairing(); run(h, 5000);
  CHECK(fake.paired_from[0] != 0 && fake.sends - sends0 >= 15, "pairing: beacon acknowledged, tool stops itself");
  fake.lamp_pairing = false;
  h.light_request(false, 0.3f, 300.0f); run(h, 1500);
  CHECK(!fake.lamp_rx.empty(), "after pairing: back on the link address");

  // 11. Persistence: a new instance restores believed state.
  run(h, 12000);
  BenqHaloComponent h2; State s2{}; int p2 = 0;
  h2.add_on_state_callback([&](const State &s) { s2 = s; p2++; });
  g_quiet = true; h2.setup(); run(h2, 2500); g_quiet = false;
  CHECK(p2 == 1 && s2 == h.target(), "reboot: believed state restored from preferences");

  // 12. Address scan finds a derived address while the dial streams frames.
  printf("-- address scan\n");
  {
    BenqHaloComponent h3; std::string fr;
    h3.add_on_frame_callback([&](const std::string &x) { fr = x; });
    g_quiet = true; h3.setup(); run(h3, 2500); g_quiet = false;
    const uint8_t target[4] = {0xDB, 0x07, 0x1B, 0xA3};  // xor candidate (air A3 1B 07 DB), register order
    memcpy(fake.lamp_addr, target, 4);
    h3.set_remote_beacon(0x35, 0x25, 0x38, 0xDD);
    h3.start_address_scan();
    for (int i = 0; i < 150000 && fr.find("found") == std::string::npos; i++) {
      if (i % 56 == 0) fake.remote(0xC4, 0x80 + (i / 56) % 60);  // dial frame every ~112 ms
      h3.loop(); g_us += 2000;
    }
    CHECK(fr == "address found: DB071BA3", "scan: derived address found from the dial");
  }

  printf(fails ? "\n%d FAILURE(S)\n" : "\nALL SIMULATION CHECKS PASSED\n", fails);
  return fails ? 1 : 0;
}
