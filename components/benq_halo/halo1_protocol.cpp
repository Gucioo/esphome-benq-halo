#include "halo1_protocol.h"

#include <cstring>

namespace esphome {
namespace benq_halo {
namespace proto {

const char *kind_str(Kind k) {
  switch (k) {
    case Kind::TEMP:
      return "temp";
    case Kind::BRIGHT:
      return "bright";
    case Kind::AUTO:
      return "auto";
    case Kind::LAMP_ACK:
      return "lamp-ack";
    case Kind::SERVICE:
      return "service";
    case Kind::RESERVED:
      return "favourite";
    case Kind::INVALID:
      return "invalid";
    case Kind::CRC_BAD:
    default:
      return "crc-bad";
  }
}

uint8_t clamp_bright(uint8_t v) { return v < BRIGHT_MIN ? BRIGHT_MIN : (v > BRIGHT_MAX ? BRIGHT_MAX : v); }
uint8_t clamp_temp(uint8_t v) { return v > TEMP_MAX ? TEMP_MAX : v; }

uint8_t lamp_bits(uint8_t lamps) {
  lamps &= F_LAMPS;
  return lamps ? lamps : F_FRONT;
}

static uint8_t flags_for(bool on, uint8_t lamps, uint8_t selector) {
  return (uint8_t) ((on ? F_POWER : 0) | lamp_bits(lamps) | selector);
}

Payload make_bright(bool on, uint8_t lamps, uint8_t bright) {
  return {flags_for(on, lamps, F_BRIGHT), clamp_bright(bright)};
}
Payload make_temp(bool on, uint8_t lamps, uint8_t temp) { return {flags_for(on, lamps, F_TEMP), clamp_temp(temp)}; }
Payload make_auto(bool on, uint8_t lamps, uint8_t counter) {
  return {flags_for(on, lamps, F_AUTO), counter ? counter : (uint8_t) 1};
}
uint8_t next_auto(uint8_t last) { return (last == 0 || last == 255) ? 1 : (uint8_t) (last + 1); }

Kind kind_of(Payload p) {
  const uint8_t f = p.flags;
  if ((f & 0xF8) == 0xF8)
    return Kind::SERVICE;  // FF/FE/FD wake-up, FA announce
  if (f & F_RSV)
    return Kind::RESERVED;  // favourite (91 xx, 89 xx)
  const uint8_t sel = f & F_SELECT;
  if (!(f & F_LAMPS) || (sel != F_TEMP && sel != F_BRIGHT && sel != F_AUTO))
    return Kind::INVALID;
  return sel == F_TEMP ? Kind::TEMP : (sel == F_BRIGHT ? Kind::BRIGHT : Kind::AUTO);
}

// ---------------------------------------------------------------------------
//  Air frames
// ---------------------------------------------------------------------------

void reg_to_air(const uint8_t reg[4], uint8_t air[4]) {
  for (uint8_t i = 0; i < 4; i++)
    air[i] = reg[3 - i];
}

static inline uint8_t bit_at(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }

static uint16_t read_bits(const uint8_t *b, unsigned pos, unsigned n) {
  uint16_t v = 0;
  for (unsigned i = 0; i < n; i++)
    v = (uint16_t) ((v << 1) | bit_at(b, pos + i));
  return v;
}

static void write_bits(uint8_t *b, unsigned pos, uint16_t v, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    const uint8_t mask = (uint8_t) (0x80 >> ((pos + i) & 7));
    if ((v >> (n - 1 - i)) & 1) {
      b[(pos + i) >> 3] |= mask;
    } else {
      b[(pos + i) >> 3] &= (uint8_t) ~mask;
    }
  }
}

static uint16_t crc_bit(uint16_t crc, uint8_t bit) {
  crc ^= (uint16_t) (bit << 15);
  return (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
}

static uint16_t frame_crc(const uint8_t air[4], const uint8_t *b, unsigned nbits) {
  uint16_t crc = 0xFFFF;
  for (unsigned i = 0; i < 32; i++)
    crc = crc_bit(crc, bit_at(air, i));
  for (unsigned i = 0; i < nbits; i++)
    crc = crc_bit(crc, bit_at(b, i));
  return crc;
}

AirFrame decode_air(const uint8_t raw[8], const uint8_t air[4]) {
  AirFrame f{};
  f.len = (uint8_t) read_bits(raw, 0, 6);
  f.pid = (uint8_t) read_bits(raw, 6, 2);
  f.no_ack = bit_at(raw, 8);
  if (f.len > 4)
    return f;  // only 64 bits were read: at most 4 payload bytes + CRC
  const unsigned n = 9 + 8u * f.len;
  for (uint8_t q = 0; q < f.len; q++)
    f.pay[q] = (uint8_t) read_bits(raw, 9 + 8u * q, 8);
  f.crc = read_bits(raw, n, 16);
  f.crc_ok = frame_crc(air, raw, n) == f.crc;
  return f;
}

void encode_air(const uint8_t air[4], uint8_t pid, bool no_ack, const uint8_t *pay, uint8_t len, uint8_t raw[8]) {
  if (!pay)
    len = 0;
  if (len > 4)
    len = 4;
  memset(raw, 0, 8);
  write_bits(raw, 0, len, 6);
  write_bits(raw, 6, pid & 3, 2);
  write_bits(raw, 8, no_ack ? 1 : 0, 1);
  for (uint8_t q = 0; q < len; q++)
    write_bits(raw, 9 + 8u * q, pay[q], 8);
  const unsigned n = 9 + 8u * len;
  write_bits(raw, n, frame_crc(air, raw, n), 16);
}

Kind classify(const AirFrame &f) {
  if (!f.crc_ok)
    return Kind::CRC_BAD;
  if (f.len == 0)
    return Kind::LAMP_ACK;  // the lamp's acknowledgement is empty
  if (f.no_ack)
    return Kind::SERVICE;  // FA xx
  if (f.len != 2)
    return Kind::INVALID;
  return kind_of({f.pay[0], f.pay[1]});
}

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

uint8_t apply_payload(State &s, Payload p) {
  const Kind k = kind_of(p);
  if (k != Kind::TEMP && k != Kind::BRIGHT)
    return 0;  // A frames carry no reliable state ('60 01' was observed)
  uint8_t changed = 0;
  const bool power = (p.flags & F_POWER) != 0;
  const uint8_t lamps = p.flags & F_LAMPS;
  if (power != s.power || lamps != s.lamps) {
    s.power = power;
    s.lamps = lamps;
    changed |= FLD_FLAGS;
  }
  if (k == Kind::BRIGHT) {
    const uint8_t v = clamp_bright(p.value);
    if (v != s.bright) {
      s.bright = v;
      changed |= FLD_BRIGHT;
    }
  } else {
    const uint8_t v = clamp_temp(p.value);
    if (v != s.temp) {
      s.temp = v;
      changed |= FLD_TEMP;
    }
  }
  return changed;
}

uint8_t covered_by(Payload sent, const State &target) {
  const Kind k = kind_of(sent);
  if (k != Kind::TEMP && k != Kind::BRIGHT)
    return 0;
  uint8_t f = 0;
  const bool on = (sent.flags & F_POWER) != 0;
  if (on == target.power && (sent.flags & F_LAMPS) == lamp_bits(target.lamps))
    f |= FLD_FLAGS;
  if (on && k == Kind::BRIGHT && clamp_bright(sent.value) == clamp_bright(target.bright))
    f |= FLD_BRIGHT;
  if (on && k == Kind::TEMP && clamp_temp(sent.value) == clamp_temp(target.temp))
    f |= FLD_TEMP;
  return f;
}

uint8_t due_fields(const State &target, uint8_t fields) {
  if ((fields & FLD_FLAGS) && target.power)
    fields |= FLD_BRIGHT;
  return (uint8_t) (fields & FLD_ALL);
}

Plan plan(const State &target, const State &believed, uint8_t dirty) {
  Plan p;
  if (!target.power) {
    if (dirty & FLD_FLAGS) {
      p.temp = true;
      p.pt = make_temp(false, target.lamps, believed.temp);
    }
    return p;
  }
  if (dirty & (FLD_BRIGHT | FLD_FLAGS)) {
    p.bright = true;
    p.pb = make_bright(true, target.lamps, target.bright);
  }
  if (dirty & FLD_TEMP) {
    p.temp = true;
    p.pt = make_temp(true, target.lamps, target.temp);
  }
  return p;
}

}  // namespace proto
}  // namespace benq_halo
}  // namespace esphome
