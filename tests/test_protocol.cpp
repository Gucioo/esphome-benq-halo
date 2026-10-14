// Host test: golden air vectors from the upstream project (halo1_proto.cpp
// selfTest), plus planning cases. Build: g++ -std=c++17 -I../components ...
#include <cstdio>
#include <cstring>
#include "benq_halo/halo1_protocol.h"
using namespace esphome::benq_halo::proto;
static int fails = 0;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
struct Golden { uint8_t pid, noack, len, pay[2]; uint16_t crc; uint8_t raw[8]; };
static const Golden G[] = {
    {0, 0, 2, {0xC3, 0x35}, 0xF7A9, {0x08, 0x61, 0x9A, 0xFB, 0xD4, 0x80, 0x00, 0x00}},
    {1, 0, 2, {0xC3, 0x35}, 0x99C9, {0x09, 0x61, 0x9A, 0xCC, 0xE4, 0x80, 0x00, 0x00}},
    {1, 0, 2, {0xC4, 0xBC}, 0x00FF, {0x09, 0x62, 0x5E, 0x00, 0x7F, 0x80, 0x00, 0x00}},
    {0, 0, 2, {0xC4, 0xFE}, 0x0619, {0x08, 0x62, 0x7F, 0x03, 0x0C, 0x80, 0x00, 0x00}},
    {2, 0, 2, {0xC4, 0xFE}, 0xDAD9, {0x0A, 0x62, 0x7F, 0x6D, 0x6C, 0x80, 0x00, 0x00}},
    {0, 0, 2, {0xE1, 0x01}, 0xE1FA, {0x08, 0x70, 0x80, 0xF0, 0xFD, 0x00, 0x00, 0x00}},
    {0, 0, 2, {0x42, 0x35}, 0xDF00, {0x08, 0x21, 0x1A, 0xEF, 0x80, 0x00, 0x00, 0x00}},
    {0, 0, 2, {0xC5, 0xA0}, 0x8E13, {0x08, 0x62, 0xD0, 0x47, 0x09, 0x80, 0x00, 0x00}},
    {1, 1, 0, {0, 0}, 0x5C90, {0x01, 0xAE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {3, 1, 0, {0, 0}, 0x1C14, {0x03, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
int main() {
  const uint8_t reg[4] = {0x4F, 0xF0, 0xFD, 0x63};
  uint8_t air[4];
  reg_to_air(reg, air);
  EXPECT(air[0] == 0x63 && air[3] == 0x4F, "air order");
  for (auto &g : G) {
    uint8_t raw[8];
    encode_air(air, g.pid, g.noack, g.pay, g.len, raw);
    EXPECT(!memcmp(raw, g.raw, 8), "encode %02X %02X pid %u", g.pay[0], g.pay[1], g.pid);
    AirFrame f = decode_air(g.raw, air);
    EXPECT(f.crc_ok && f.crc == g.crc && f.len == g.len && f.pid == g.pid, "decode %02X %02X", g.pay[0], g.pay[1]);
  }
  for (unsigned i = 0; i < 41; i++) {
    uint8_t raw[8]; memcpy(raw, G[3].raw, 8); raw[i >> 3] ^= 0x80 >> (i & 7);
    EXPECT(!decode_air(raw, air).crc_ok, "bitflip %u undetected", i);
  }
  EXPECT(kind_of({0xC3, 0x35}) == Kind::TEMP && kind_of({0xC5, 0xA0}) == Kind::BRIGHT &&
         kind_of({0xE1, 0x01}) == Kind::AUTO && kind_of({0xFF, 0}) == Kind::SERVICE &&
         kind_of({0x91, 0}) == Kind::RESERVED && kind_of({0xC6, 0x10}) == Kind::INVALID, "kinds");
  EXPECT(make_bright(true, 0, 0x20) == (Payload{0xC4, 0x4C}), "makeBright no lamp");
  EXPECT(make_temp(false, 0xFF, 0xFF) == (Payload{0x43, 0x64}), "makeTemp clamp");
  // plan cases (from upstream D.3 table)
  State on{true, F_LAMPS, 0xA5, 0x35}, off{false, F_LAMPS, 0xA5, 0x35};
  Plan p = plan(off, on, FLD_FLAGS);
  EXPECT(!p.bright && p.temp && p.pt == (Payload{0x43, 0x35}), "plan off");
  State t{true, F_LAMPS, 0xA5, 0x64}, b{false, F_LAMPS, 0xA5, 0x64};
  p = plan(t, b, due_fields(t, FLD_FLAGS));
  EXPECT(p.bright && p.pb == (Payload{0xC5, 0xA5}) && !p.temp, "plan on");
  State f{true, F_FRONT, 0xA5, 0x35};
  p = plan(f, on, FLD_FLAGS);
  EXPECT(p.bright && p.pb == (Payload{0xC4, 0xA5}), "plan front");
  State lo{true, F_LAMPS, 0x4C, 0x00};
  p = plan(lo, on, FLD_BRIGHT | FLD_TEMP);
  EXPECT(p.pb == (Payload{0xC5, 0x4C}) && p.pt == (Payload{0xC3, 0x00}), "plan both");
  EXPECT(covered_by({0x43, 0x35}, off) == FLD_FLAGS, "covered off");
  EXPECT(covered_by({0xC5, 0xA5}, on) == (FLD_FLAGS | FLD_BRIGHT), "covered C5");
  printf(fails ? "%d FAILURE(S)\n" : "all protocol tests passed\n", fails);
  return fails ? 1 : 0;
}
