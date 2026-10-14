#pragma once
// BenQ ScreenBar Halo (1st gen) radio protocol, as documented by
// github.com/Gucioo/benq-screenbar-halo-matter (docs/PROTOCOL.md, Sep 2026).
// Pure C++: no ESPHome includes, so it also builds on the host for tests.
//
// Air frame (BC5602 "standard" / ShockBurst-like):
//   preamble 0x55 | address 4 B | PCF 9 bit (len 6, PID 2, NO_ACK 1) | payload | CRC-16
//   CRC-16/CCITT poly 0x1021, init 0xFFFF, over on-air address + PCF + payload.
// Channel 5 (2405 MHz), 125 kbps. The BC5602 takes the address in REVERSE
// byte order ("register order") compared with the air.
//
// Command payload: 2 bytes, [flags, value]. Every state frame is absolute.
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace benq_halo {
namespace proto {

constexpr uint8_t CHANNEL = 5;

// Pairing beacon address, register order (air 59 01 00 B0).
constexpr uint8_t PAIRING_ADDR_REG[4] = {0xB0, 0x00, 0x01, 0x59};

// ---- First payload byte -----------------------------------------------------
constexpr uint8_t F_POWER = 0x80;
constexpr uint8_t F_FRONT = 0x40;
constexpr uint8_t F_AUTO = 0x20;    // button A: value = press counter
constexpr uint8_t F_RSV = 0x18;     // favourite recall only, no visible effect
constexpr uint8_t F_BRIGHT = 0x04;  // value = brightness 0x4C..0xFE
constexpr uint8_t F_TEMP = 0x02;    // value = colour temp 0x00 (cold)..0x64 (warm)
constexpr uint8_t F_BACK = 0x01;
constexpr uint8_t F_LAMPS = F_FRONT | F_BACK;
constexpr uint8_t F_SELECT = F_AUTO | F_BRIGHT | F_TEMP;

constexpr uint8_t BRIGHT_MIN = 0x4C;  // lamp floor: nothing changes below it
constexpr uint8_t BRIGHT_MAX = 0xFE;
constexpr uint8_t TEMP_MAX = 0x64;
constexpr uint8_t BRIGHT_DEFAULT = 0xA5;
constexpr uint8_t TEMP_DEFAULT = 0x35;

struct Payload {
  uint8_t flags;
  uint8_t value;
};
inline bool operator==(Payload a, Payload b) { return a.flags == b.flags && a.value == b.value; }
inline bool operator!=(Payload a, Payload b) { return !(a == b); }

enum class Kind : uint8_t { TEMP, BRIGHT, AUTO, LAMP_ACK, SERVICE, RESERVED, INVALID, CRC_BAD };
const char *kind_str(Kind k);

uint8_t clamp_bright(uint8_t v);
uint8_t clamp_temp(uint8_t v);
uint8_t lamp_bits(uint8_t lamps);  // never 0: falls back to front

Payload make_bright(bool on, uint8_t lamps, uint8_t bright);
Payload make_temp(bool on, uint8_t lamps, uint8_t temp);
Payload make_auto(bool on, uint8_t lamps, uint8_t counter);
uint8_t next_auto(uint8_t last);  // 0/255 -> 1, else +1

Kind kind_of(Payload p);

// ---- Air frames ---------------------------------------------------------------
void reg_to_air(const uint8_t reg[4], uint8_t air[4]);

// Passive listening reads 64 bits after the address (RXPW0 = 8).
struct AirFrame {
  bool crc_ok;
  uint8_t len, pid, no_ack;
  uint8_t pay[4];
  uint16_t crc;
};
AirFrame decode_air(const uint8_t raw[8], const uint8_t air[4]);
void encode_air(const uint8_t air[4], uint8_t pid, bool no_ack, const uint8_t *pay, uint8_t len, uint8_t raw[8]);
Kind classify(const AirFrame &f);

// ---- Lamp state -------------------------------------------------------------
struct State {
  bool power = false;
  uint8_t lamps = F_LAMPS;  // never 0
  uint8_t bright = BRIGHT_DEFAULT;
  uint8_t temp = TEMP_DEFAULT;
};
inline bool operator==(const State &a, const State &b) {
  return a.power == b.power && a.lamps == b.lamps && a.bright == b.bright && a.temp == b.temp;
}
inline bool operator!=(const State &a, const State &b) { return !(a == b); }

enum : uint8_t { FLD_FLAGS = 1, FLD_BRIGHT = 2, FLD_TEMP = 4, FLD_ALL = 7 };

// Applies a Temp/Bright frame to a state. Returns the FLD_* that changed.
uint8_t apply_payload(State &s, Payload p);
// Fields of `target` that a delivered frame satisfies.
uint8_t covered_by(Payload sent, const State &target);
// Fields to deliver for a request. Powered on + FLAGS also makes BRIGHT due, so
// the lamp turns on at the displayed brightness.
uint8_t due_fields(const State &target, uint8_t fields);

struct Plan {
  bool bright = false, temp = false;
  Payload pb{0, 0}, pt{0, 0};
};
// Off: a temp frame with power=0 carrying the BELIEVED temperature (a pending
// setting must not leak). On: brightness frame carries power + lamps.
Plan plan(const State &target, const State &believed, uint8_t dirty);

}  // namespace proto
}  // namespace benq_halo
}  // namespace esphome
