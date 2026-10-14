#pragma once
// Host test stub: SPIDevice routed to a fake BC5602.
#include <cstdint>
#include "esphome/core/gpio.h"
namespace esphome {
namespace spi {
enum SPIBitOrder { BIT_ORDER_LSB_FIRST, BIT_ORDER_MSB_FIRST };
enum SPIClockPolarity { CLOCK_POLARITY_LOW = false, CLOCK_POLARITY_HIGH = true };
enum SPIClockPhase { CLOCK_PHASE_LEADING, CLOCK_PHASE_TRAILING };
enum SPIDataRate : uint32_t { DATA_RATE_1MHZ = 1000000 };
struct FakeBus {
  virtual void begin() = 0;
  virtual uint8_t xfer(uint8_t b) = 0;
  virtual void end() = 0;
};
extern FakeBus *g_fake_bus;
template<SPIBitOrder B, SPIClockPolarity P, SPIClockPhase C, SPIDataRate R> class SPIDevice {
 public:
  void spi_setup() {}
  void enable() { g_fake_bus->begin(); }
  void disable() { g_fake_bus->end(); }
  uint8_t transfer_byte(uint8_t d) { return g_fake_bus->xfer(d); }
 protected:
  GPIOPin *cs_{nullptr};
};
}  // namespace spi
}  // namespace esphome
