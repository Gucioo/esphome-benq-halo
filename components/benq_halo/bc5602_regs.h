#pragma once
// Holtek BC5602 (BM5602-60-1 module) registers and commands used here.
// Values from the BC5602 v1.20 datasheet as applied by the upstream project.
#include <cstdint>

namespace esphome {
namespace benq_halo {
namespace bc {

// SPI commands
constexpr uint8_t CMD_WRITE_REG = 0x40;
constexpr uint8_t CMD_READ_REG = 0xC0;
constexpr uint8_t CMD_READ_CHIP_VERSION = 0x9F;
constexpr uint8_t CMD_SOFTWARE_RESET = 0x08;
constexpr uint8_t CMD_WRITE_PTX_ADDRESS = 0x10;
constexpr uint8_t CMD_WRITE_TX_FIFO_WITH_ACK = 0x11;
constexpr uint8_t CMD_FLUSH_TX_FIFO = 0x09;
constexpr uint8_t CMD_FLUSH_RX_FIFO = 0x89;
constexpr uint8_t CMD_READ_RX_FIFO = 0xBF;
constexpr uint8_t CMD_LIGHT_SLEEP = 0x0C;
constexpr uint8_t CMD_RX_MODE = 0x8E;

// Common registers
constexpr uint8_t REG_CFG1 = 0x00;
constexpr uint8_t CFG1_AGC_EN = 0x40;
constexpr uint8_t REG_RC1 = 0x01;
constexpr uint8_t RC1_PWRON = 0x80;
constexpr uint8_t RC1_XCLK_RDY = 0x20;
constexpr uint8_t REG_MASK = 0x03;
constexpr uint8_t MASK_PRM_RX = 0x01;
constexpr uint8_t REG_IRQ1 = 0x04;
constexpr uint8_t IRQ_RX_DR = 0x40;
constexpr uint8_t IRQ_TX_DS = 0x20;
constexpr uint8_t IRQ_MAX_RT = 0x10;
constexpr uint8_t IRQ_ALL = IRQ_RX_DR | IRQ_TX_DS | IRQ_MAX_RT;
constexpr uint8_t REG_STATUS = 0x05;
constexpr uint8_t STATUS_TX_FIFO_EMPTY = 0x10;
constexpr uint8_t REG_IO1 = 0x06;
constexpr uint8_t IO1_4WIRE_SPI = 0x48;  // GIO2 becomes SPI data out (MISO)
constexpr uint8_t REG_PKT1 = 0x09;
constexpr uint8_t PKT1_CRC_EN = 0x20;
constexpr uint8_t REG_PKT2 = 0x0A;
constexpr uint8_t REG_RFCH = 0x10;
constexpr uint8_t REG_DM1 = 0x11;
constexpr uint8_t DM1_HALO = 0x82;  // 4-byte address, 125 kbps
constexpr uint8_t REG_RT1 = 0x13;
constexpr uint8_t RT1_HALO = 0x73;  // ARD 2000 us, ARC 3
constexpr uint8_t REG_RT2 = 0x14;
constexpr uint8_t REG_CE = 0x15;

// Bank 0
constexpr uint8_t B0_OM = 0x20;
constexpr uint8_t OM_ACAL_EN = 0x08;
constexpr uint8_t B0_CFO1 = 0x21;
constexpr uint8_t B0_STA1 = 0x26;
constexpr uint8_t OMST_MASK = 0x07;
constexpr uint8_t OMST_RX = 5;
constexpr uint8_t B0_DPL1 = 0x2A;
constexpr uint8_t B0_DPL2 = 0x2B;
constexpr uint8_t B0_RXPW0 = 0x2C;
constexpr uint8_t B0_ENAA = 0x32;

// Holtek "recommended" values for reserved analog registers (LNA, filter,
// PLL, modem). Reset values are NOT operational: SPI works, RX hears nothing.
struct RegInit {
  uint8_t bank, reg, val;
};
constexpr RegInit RECOMMENDED[] = {
    {0, 0x0D, 0x20}, {0, 0x0F, 0x14}, {0, 0x16, 0x66}, {0, 0x17, 0xAA}, {0, 0x18, 0x45},
    {1, 0x20, 0x0C}, {1, 0x21, 0x03}, {1, 0x23, 0x10}, {1, 0x25, 0xCC}, {1, 0x26, 0x4C},
    {1, 0x27, 0x80}, {2, 0x28, 0xA0}, {2, 0x2D, 0x18}, {2, 0x2E, 0xEC}, {2, 0x36, 0x03},
    {2, 0x38, 0x0A}, {2, 0x39, 0x12}, {2, 0x3B, 0x94}, {2, 0x3C, 0x43},
};

}  // namespace bc
}  // namespace benq_halo
}  // namespace esphome
