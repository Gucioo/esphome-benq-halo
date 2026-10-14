#include "benq_halo.h"

#include <cinttypes>
#include <cmath>
#include <algorithm>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif

namespace esphome {
namespace benq_halo {

static const char *const TAG = "benq_halo";

using namespace proto;

// Timings proven on the bench by the upstream project (src/config.h).
static constexpr uint32_t RESET_WAIT_MS = 40;      // 2 x 20 ms after a soft reset
static constexpr uint32_t RX_REARM_MS = 100;       // periodic RX re-arm
static constexpr uint32_t RX_SILENCE_MS = 500;     // full reconfig after this much silence
static constexpr uint32_t GAP_MS = 100;            // between frames of a burst (remote ~100)
static constexpr uint8_t MAX_ATTEMPTS = 5;         // packets per frame per round
static constexpr uint32_t RETRY_MS = 1000;         // backoff: 1 s x failure rank
static constexpr uint8_t PLAN_RETRIES = 2;         // retries before giving up
static constexpr uint32_t REMOTE_HOLDOFF_MS = 250;  // don't fight the real remote...
static constexpr uint32_t REMOTE_HOLDOFF_MAX_MS = 2000;  // ...unless a request waits this long
static constexpr uint32_t BOOT_IGNORE_MS = 2000;   // light writes ignored right after boot
static constexpr uint32_t SAVE_DELAY_MS = 10000;   // flash write 10 s after the last change
static constexpr uint32_t REINIT_RETRY_MS = 60000;
static constexpr uint32_t PAIR_INTERVAL_MS = 200;  // remote beacon: one payload every ~200 ms
static constexpr uint16_t PAIR_ACKS_DONE = 15;     // remote stopped after ~3 s of acked beacons
static constexpr uint32_t TOOL_DURATION_MS = 60000;
static constexpr uint32_t STATS_PERIOD_MS = 30000;
static constexpr uint32_t SCAN_DWELL_MS = 1200;  // > 10 dial frames (~112 ms apart)

struct SavedState {
  uint8_t power_lamps;
  uint8_t bright;
  uint8_t temp;
  uint8_t last_auto;
  uint8_t addr[4];
} __attribute__((packed));

// ===========================================================================
//  SPI primitives. CSN framing per transaction, mode 0, MSB first, 1 MHz.
// ===========================================================================

void BenqHaloComponent::cmd_(uint8_t c) {
  this->enable();
  this->transfer_byte(c);
  this->disable();
}

uint8_t BenqHaloComponent::rd_(uint8_t reg) {
  this->enable();
  this->transfer_byte(bc::CMD_READ_REG | reg);
  uint8_t v = this->transfer_byte(0x00);
  this->disable();
  return v;
}

void BenqHaloComponent::wr_(uint8_t reg, uint8_t v) {
  this->enable();
  this->transfer_byte(bc::CMD_WRITE_REG | reg);
  this->transfer_byte(v);
  this->disable();
}

void BenqHaloComponent::cmd_data_(uint8_t c, const uint8_t *d, size_t n) {
  this->enable();
  this->transfer_byte(c);
  for (size_t i = 0; i < n; i++)
    this->transfer_byte(d[i]);
  this->disable();
}

void BenqHaloComponent::read_fifo_(uint8_t *out, size_t n) {
  this->enable();
  this->transfer_byte(bc::CMD_READ_RX_FIFO);
  for (size_t i = 0; i < n; i++)
    out[i] = this->transfer_byte(0x00);
  this->disable();
}

void BenqHaloComponent::set_bank_(uint8_t bank) {
  uint8_t cfg1 = this->rd_(bc::REG_CFG1) & 0xFC;
  this->wr_(bc::REG_CFG1, cfg1 | (bank & 0x03));
}

// ===========================================================================
//  Chip bring-up
// ===========================================================================

void BenqHaloComponent::load_recommended_() {
  uint8_t bank = 0xFF;
  for (const auto &r : bc::RECOMMENDED) {
    if (r.bank != bank) {
      this->set_bank_(r.bank);
      bank = r.bank;
    }
    this->wr_(r.reg, r.val);
  }
  this->set_bank_(0);  // OM (0x20) in bank 2 would be another register
}

bool BenqHaloComponent::calibrate_() {
  this->set_bank_(0);
  this->wr_(bc::B0_OM, bc::OM_ACAL_EN);
  delayMicroseconds(100);
  const uint32_t t0 = millis();
  while ((this->rd_(bc::B0_OM) & bc::OM_ACAL_EN) && (millis() - t0) < 200)
    delayMicroseconds(200);
  return !(this->rd_(bc::B0_OM) & bc::OM_ACAL_EN);
}

bool BenqHaloComponent::init_chip_() {
  this->last_init_attempt_ = millis();
  // The ESP reset does not power-cycle the module: always soft-reset it.
  this->cmd_(bc::CMD_SOFTWARE_RESET);
  delay(20);
  // Out of reset the chip speaks 3-wire SPI (SDIO bidirectional). Writes work,
  // so switch to 4-wire (GIO2 = MISO) BEFORE any read.
  this->wr_(bc::REG_IO1, bc::IO1_4WIRE_SPI);
  this->cmd_(bc::CMD_LIGHT_SLEEP);
  delay(1);

  uint8_t v[3];
  this->enable();
  this->transfer_byte(bc::CMD_READ_CHIP_VERSION | bc::CMD_READ_REG);
  for (auto &b : v)
    b = this->transfer_byte(0x00);
  this->disable();
  this->chip_version_ = ((uint32_t) v[0] << 16) | ((uint32_t) v[1] << 8) | v[2];
  this->present_ = this->chip_version_ != 0x000000 && this->chip_version_ != 0xFFFFFF;
  if (!this->present_)
    return false;

  this->set_bank_(0);
  this->load_recommended_();
  this->wr_(bc::REG_IRQ1, bc::IRQ_ALL);
  this->wr_(bc::REG_MASK, this->rd_(bc::REG_MASK) | bc::MASK_PRM_RX);

  // Wait for the crystal, then VCO auto-calibration (every boot: PWRON only
  // re-arms on a real power cycle of the module).
  const uint8_t rc1 = this->rd_(bc::REG_RC1);
  this->cmd_(bc::CMD_LIGHT_SLEEP);
  const uint32_t t0 = millis();
  bool xtal = false;
  while (millis() - t0 < 50) {
    if (this->rd_(bc::REG_RC1) & bc::RC1_XCLK_RDY) {
      xtal = true;
      break;
    }
    delayMicroseconds(200);
  }
  this->xtal_ready_ = xtal;
  this->cal1_ok_ = xtal && this->calibrate_();
  if (rc1 & bc::RC1_PWRON)
    this->wr_(bc::REG_RC1, (uint8_t) (rc1 & ~bc::RC1_PWRON));

  // Calibrate once more with the Halo channel/rate written (upstream does the
  // same: first RFCH write, light sleep, calibrate).
  this->wr_(bc::REG_RFCH, CHANNEL);
  this->wr_(bc::REG_DM1, bc::DM1_HALO);
  this->cmd_data_(bc::CMD_WRITE_PTX_ADDRESS, this->addr_reg_, 4);
  this->cmd_(bc::CMD_LIGHT_SLEEP);
  this->cal2_ok_ = this->calibrate_();
  this->set_bank_(0);
  this->om_after_cal_ = this->rd_(bc::B0_OM);
  this->calibrated_ = this->cal2_ok_;

  this->mode_ = Mode::UNKNOWN;
  this->verify_fails_ = 0;
  this->timeouts_in_row_ = 0;
  return true;
}

// Full configuration for the standard auto-ACK format. Replayed after every
// soft reset, which wipes the analog settings and CFG1 (incl. AGC).
void BenqHaloComponent::configure_(const uint8_t addr_reg[4], bool receiver) {
  this->wr_(bc::REG_IO1, bc::IO1_4WIRE_SPI);
  this->load_recommended_();
  this->wr_(bc::REG_CFG1, bc::CFG1_AGC_EN);  // AGC on, bank 0
  this->wr_(bc::REG_RFCH, CHANNEL);
  this->wr_(bc::REG_DM1, bc::DM1_HALO);
  this->cmd_data_(bc::CMD_WRITE_PTX_ADDRESS, addr_reg, 4);
  // One-byte preamble, like the remote.
  this->wr_(bc::B0_CFO1, (uint8_t) (this->rd_(bc::B0_CFO1) & ~0x40));
  uint8_t mask = this->rd_(bc::REG_MASK);
  mask = receiver ? (uint8_t) (mask | bc::MASK_PRM_RX) : (uint8_t) (mask & ~bc::MASK_PRM_RX);
  this->wr_(bc::REG_MASK, mask);
  this->wr_(bc::REG_PKT1, bc::PKT1_CRC_EN);                          // CRC16, init FFFF
  this->wr_(bc::REG_PKT2, (uint8_t) (this->rd_(bc::REG_PKT2) & 0x7F));  // no whitening
  this->wr_(bc::B0_DPL1, 0x01);
  this->wr_(bc::B0_DPL2, 0x04);
  this->wr_(bc::B0_ENAA, 0x01);
  this->wr_(bc::REG_RT1, bc::RT1_HALO);
  this->wr_(bc::REG_IRQ1, bc::IRQ_ALL);
  this->cmd_(bc::CMD_FLUSH_TX_FIFO);
  this->cmd_(bc::CMD_FLUSH_RX_FIFO);
  this->wr_(bc::REG_CE, 0x00);
}

// Passive listening: we must NEVER acknowledge (we'd collide with the lamp's
// ACK). Without auto-ACK the chip can't parse the 9-bit PCF, so read a fixed
// 64 bits after the address and decode PCF + CRC in software.
void BenqHaloComponent::passive_overrides_() {
  this->wr_(bc::B0_ENAA, 0x00);
  this->wr_(bc::B0_DPL2, 0x00);
  this->wr_(bc::B0_DPL1, 0x00);
  this->wr_(bc::REG_PKT1, 0x00);
  this->wr_(bc::B0_RXPW0, 8);
}

bool BenqHaloComponent::enter_rx_(uint32_t timeout_us) {
  this->wr_(bc::REG_MASK, (uint8_t) (this->rd_(bc::REG_MASK) | bc::MASK_PRM_RX));
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (this->omst_() == bc::OMST_RX)
      return true;
    this->cmd_(bc::CMD_FLUSH_RX_FIFO);
    this->wr_(bc::REG_IRQ1, bc::IRQ_RX_DR);
    this->cmd_(bc::CMD_RX_MODE);
    for (uint32_t waited = 0; waited < timeout_us; waited += 50) {
      if (this->omst_() == bc::OMST_RX)
        return true;
      delayMicroseconds(50);
    }
  }
  return this->omst_() == bc::OMST_RX;
}

// ===========================================================================
//  Radio mode machine
// ===========================================================================

void BenqHaloComponent::want_mode_(Mode m, const uint8_t addr_reg[4]) {
  const bool same_addr = memcmp(addr_reg, this->mode_addr_, 4) == 0;
  if (this->mode_ == Mode::RESETTING) {
    this->mode_target_ = m;
    memcpy(this->mode_addr_, addr_reg, 4);
    return;
  }
  if (this->mode_ == m && same_addr)
    return;
  this->begin_reset_(m, addr_reg);
}

void BenqHaloComponent::begin_reset_(Mode target, const uint8_t addr_reg[4]) {
  this->cmd_(bc::CMD_SOFTWARE_RESET);  // back to 3-wire SPI: no reads until configured
  this->reset_at_ = millis();
  this->mode_target_ = target;
  memcpy(this->mode_addr_, addr_reg, 4);
  this->mode_ = Mode::RESETTING;
}

void BenqHaloComponent::service_radio_(uint32_t now) {
  if (this->mode_ != Mode::RESETTING)
    return;
  if ((int32_t) (now - this->reset_at_) < (int32_t) RESET_WAIT_MS)
    return;
  const bool rx = this->mode_target_ == Mode::RX;
  this->configure_(this->mode_addr_, rx);
  const bool ok = this->rd_(bc::REG_RFCH) == CHANNEL && this->rd_(bc::REG_DM1) == bc::DM1_HALO &&
                  this->rd_(bc::REG_RT1) == bc::RT1_HALO;
  if (!ok) {
    if (++this->verify_fails_ >= 3) {
      ESP_LOGW(TAG, "BM5602 config does not read back (3x): re-initialising the module");
      if (!this->init_chip_()) {
        ESP_LOGE(TAG, "BM5602 lost (chip version 0x%06" PRIX32 ")", this->chip_version_);
      }
    } else {
      this->begin_reset_(this->mode_target_, this->mode_addr_);
    }
    return;
  }
  this->verify_fails_ = 0;
  if (rx) {
    this->passive_overrides_();
    this->enter_rx_();
    this->last_full_ = this->last_arm_ = now;
    this->mode_ = Mode::RX;
  } else {
    this->mode_ = Mode::TX;  // PTX, CE=0: ready to transmit
  }
}

BenqHaloComponent::Verdict BenqHaloComponent::send_one_(const uint8_t *pay, uint8_t len) {
  if (this->mode_ != Mode::TX)
    return Verdict::REFUSED;
  this->wr_(bc::REG_IRQ1, bc::IRQ_ALL);
  this->cmd_(bc::CMD_FLUSH_TX_FIFO);
  this->cmd_data_(bc::CMD_WRITE_TX_FIFO_WITH_ACK, pay, len);
  Verdict v = Verdict::REFUSED;
  if (!(this->rd_(bc::REG_STATUS) & bc::STATUS_TX_FIFO_EMPTY)) {
    uint8_t irq = 0;
    const uint32_t t0 = micros();
    this->wr_(bc::REG_CE, 0x01);
    while (micros() - t0 < 30000) {
      irq = this->rd_(bc::REG_IRQ1);
      if (irq & (bc::IRQ_TX_DS | bc::IRQ_MAX_RT))
        break;
      delayMicroseconds(20);
    }
    if (!(irq & (bc::IRQ_TX_DS | bc::IRQ_MAX_RT)))
      irq = this->rd_(bc::REG_IRQ1);
    v = (irq & bc::IRQ_TX_DS) ? Verdict::ACK : (irq & bc::IRQ_MAX_RT) ? Verdict::MAX_RT : Verdict::TIMEOUT;
  }
  this->wr_(bc::REG_CE, 0x00);  // otherwise the chip re-sends on its own
  this->wr_(bc::REG_IRQ1, bc::IRQ_ALL);
  this->cmd_(bc::CMD_FLUSH_TX_FIFO);
  this->cmd_(bc::CMD_FLUSH_RX_FIFO);
  this->st_packets_++;
  if (v == Verdict::ACK) {
    this->st_acks_++;
    this->timeouts_in_row_ = 0;
  } else {
    if (v == Verdict::TIMEOUT && ++this->timeouts_in_row_ >= 3) {
      // Chip symptom (not a missing lamp, which gives MAX_RT): re-init.
      ESP_LOGW(TAG, "3 TX timeouts in a row: re-initialising the BM5602");
      this->timeouts_in_row_ = 0;
      this->init_chip_();
      return v;
    }
    // After a failure the next sends fail in 64 us unless fully reconfigured.
    // Never after a success: the PID must advance between packets.
    this->begin_reset_(Mode::TX, this->mode_addr_);
  }
  return v;
}

bool BenqHaloComponent::poll_rx_(uint32_t now, uint8_t raw[8]) {
  if (this->mode_ != Mode::RX)
    return false;
  bool got = false;
  if (this->rd_(bc::REG_IRQ1) & bc::IRQ_RX_DR) {
    this->last_frame_ = now;
    this->read_fifo_(raw, 8);
    this->wr_(bc::REG_IRQ1, bc::IRQ_RX_DR);
    this->cmd_(bc::CMD_FLUSH_RX_FIFO);
    // The chip dropped to light sleep after the frame: re-arm.
    this->wr_(bc::REG_CE, 0x00);
    this->enter_rx_();
    this->last_arm_ = now;
    got = true;
  }
  if (this->omst_() != bc::OMST_RX) {
    this->enter_rx_(300);
    this->last_arm_ = now;
  } else if (now - this->last_arm_ > RX_REARM_MS) {
    this->wr_(bc::REG_CE, 0x00);
    this->enter_rx_();
    this->last_arm_ = now;
  }
  // Proven remedy for a listener that goes quiet despite re-arming.
  if (now - this->last_frame_ > RX_SILENCE_MS && now - this->last_full_ > RX_SILENCE_MS)
    this->begin_reset_(Mode::RX, this->mode_addr_);
  return got;
}

// ===========================================================================
//  Component
// ===========================================================================

void BenqHaloComponent::setup() {
  reg_to_air(this->addr_reg_, this->addr_air_);
  if (this->pairing_payloads_.empty()) {
    this->pairing_payloads_ = {{0x5A, 0x5A}, {0xF5, 0xC3}, {0xCF, 0x49}};
  }

  // Restore the believed lamp state (the lamp itself can't be read back).
  this->pref_ = global_preferences->make_preference<SavedState>(fnv1_hash("benq_halo_state"));
  SavedState s{};
  if (this->pref_.load(&s) && memcmp(s.addr, this->addr_reg_, 4) == 0 && (s.power_lamps & F_LAMPS) &&
      s.bright >= BRIGHT_MIN && s.bright <= BRIGHT_MAX && s.temp <= TEMP_MAX) {
    this->believed_.power = (s.power_lamps & F_POWER) != 0;
    this->believed_.lamps = s.power_lamps & F_LAMPS;
    this->believed_.bright = s.bright;
    this->believed_.temp = s.temp;
    this->last_auto_ = s.last_auto;
  }
  this->target_ = this->believed_;

  this->spi_setup();
  if (!this->init_chip_()) {
    ESP_LOGE(TAG, "BM5602 not detected (version 0x%06" PRIX32 "). 0xFFFFFF: MISO/GIO2 not wired; "
                  "0x000000: no power, CSN or SCK/MOSI swapped.",
             this->chip_version_);
    this->status_set_error(LOG_STR("BM5602 not detected"));
  }
  this->boot_at_ = millis();
#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)
  this->pc_.start();
#endif
}

void BenqHaloComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "BenQ ScreenBar Halo (gen 1) via BM5602:");
  LOG_PIN("  CS Pin: ", this->cs_);
  ESP_LOGCONFIG(TAG, "  Module: %s (chip version 0x%06" PRIX32 ")", this->present_ ? "detected" : "NOT DETECTED",
                this->chip_version_);
  ESP_LOGCONFIG(TAG, "  Crystal ready: %s, VCO calibration: first %s, final %s (OM=0x%02X)", YESNO(this->xtal_ready_),
                this->cal1_ok_ ? "ok" : "failed", this->cal2_ok_ ? "ok" : "FAILED", this->om_after_cal_);
  ESP_LOGCONFIG(TAG, "  Link address: %02X%02X%02X%02X (register order; on air %02X %02X %02X %02X), channel %u",
                this->addr_reg_[0], this->addr_reg_[1], this->addr_reg_[2], this->addr_reg_[3], this->addr_air_[0],
                this->addr_air_[1], this->addr_air_[2], this->addr_air_[3], CHANNEL);
  ESP_LOGCONFIG(TAG, "  Listen to remote: %s, repeats %u (min %u ACKs), brightness gamma %.2f",
                YESNO(this->listen_), this->repeats_, this->min_acks_, this->gamma_);
#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)
  ESP_LOGCONFIG(TAG, "  PC presence: %s via %s%s, off after %" PRIu32 " s, %s", this->pc_.host().c_str(),
                this->pc_.port() ? "TCP port " : "ping", this->pc_.port() ? std::to_string(this->pc_.port()).c_str() : "",
                this->pc_off_delay_ms_ / 1000, this->pc_control_ ? "switches the lamp" : "sensor only");
#endif
  ESP_LOGCONFIG(TAG, "  Restored state: %s, lamps %s, bright 0x%02X, temp %u", this->believed_.power ? "on" : "off",
                this->believed_.lamps == F_LAMPS ? "both" : (this->believed_.lamps == F_FRONT ? "front" : "back"),
                this->believed_.bright, this->believed_.temp);
}

void BenqHaloComponent::loop() {
  const uint32_t now = millis();

  if (!this->booted_ && now - this->boot_at_ >= BOOT_IGNORE_MS) {
    this->booted_ = true;
    this->publish_state_();  // entities now show the believed state
  }
  this->pc_loop_(now);

  if (!this->present_) {
    if (now - this->last_init_attempt_ >= REINIT_RETRY_MS) {
      if (this->init_chip_()) {
        ESP_LOGI(TAG, "BM5602 detected (chip version 0x%06" PRIX32 ")", this->chip_version_);
        this->status_clear_error();
      }
    }
    return;
  }

  this->service_radio_(now);

  switch (this->activity_) {
    case Activity::PAIR_TX:
      this->run_pair_tx_(now);
      break;
    case Activity::PAIR_SNIFF:
      this->run_pair_sniff_(now);
      break;
    case Activity::ADDR_SCAN:
      this->run_addr_scan_(now);
      break;
    case Activity::NORMAL:
    default:
      this->drive_(now);
      break;
  }

  if (this->save_pending_ && (int32_t) (now - this->save_at_) >= 0) {
    this->save_pending_ = false;
    SavedState s{};
    s.power_lamps = (uint8_t) ((this->believed_.power ? F_POWER : 0) | this->believed_.lamps);
    s.bright = this->believed_.bright;
    s.temp = this->believed_.temp;
    s.last_auto = this->last_auto_;
    memcpy(s.addr, this->addr_reg_, 4);
    this->pref_.save(&s);
  }
  this->update_high_freq_();

  if (now - this->stats_at_ >= STATS_PERIOD_MS) {
    this->stats_at_ = now;
    const uint32_t ok = this->st_rx_ok_ - this->st_rx_ok_prev_, bad = this->st_rx_bad_ - this->st_rx_bad_prev_;
    const uint32_t pk = this->st_packets_ - this->st_packets_prev_, ak = this->st_acks_ - this->st_acks_prev_;
    this->st_rx_ok_prev_ = this->st_rx_ok_;
    this->st_rx_bad_prev_ = this->st_rx_bad_;
    this->st_packets_prev_ = this->st_packets_;
    this->st_acks_prev_ = this->st_acks_;
    ESP_LOGI(TAG, "Last 30 s: heard %" PRIu32 " valid + %" PRIu32 " bad-CRC frames on %02X%02X%02X%02X; sent %" PRIu32
                  " packets, %" PRIu32 " ACKed; radio %s",
             ok, bad, this->addr_reg_[0], this->addr_reg_[1], this->addr_reg_[2], this->addr_reg_[3], pk, ak,
             this->mode_ == Mode::RX ? "listening" : this->mode_ == Mode::TX ? "TX" : "resetting");
  }
}

void BenqHaloComponent::update_high_freq_() {
  const bool want = this->activity_ != Activity::NORMAL || this->phase_ == Phase::BURST || this->any_active_();
  if (want && !this->high_freq_on_) {
    this->high_freq_.start();
    this->high_freq_on_ = true;
  } else if (!want && this->high_freq_on_) {
    this->high_freq_.stop();
    this->high_freq_on_ = false;
  }
}

// ===========================================================================
//  Command engine
// ===========================================================================

void BenqHaloComponent::drive_(uint32_t now) {
  if (this->phase_ == Phase::BACKOFF && (int32_t) (now - this->retry_at_) >= 0)
    this->phase_ = Phase::IDLE;

  if (this->phase_ == Phase::IDLE) {
    if (!this->any_active_()) {
      this->pending_since_ = 0;
    } else {
      if (this->pending_since_ == 0)
        this->pending_since_ = now;
      const bool remote_busy = now - this->remote_at_ < REMOTE_HOLDOFF_MS && this->remote_at_ != 0;
      if (!remote_busy || now - this->pending_since_ >= REMOTE_HOLDOFF_MAX_MS) {
        this->phase_ = Phase::BURST;
        this->next_tx_at_ = now;
        this->rr_ = 0;
      }
    }
  }

  if (this->phase_ == Phase::BURST) {
    if (this->any_active_()) {
      this->want_mode_(Mode::TX, this->addr_reg_);
      if (this->mode_ == Mode::TX && (int32_t) (now - this->next_tx_at_) >= 0)
        this->send_packet_(now);
    }
    if (this->phase_ == Phase::BURST && !this->any_active_()) {
      this->phase_ = Phase::IDLE;
      this->pending_since_ = 0;
    }
  }

  if (this->phase_ != Phase::BURST && this->listen_) {
    this->want_mode_(Mode::RX, this->addr_reg_);
    uint8_t raw[8];
    if (this->poll_rx_(now, raw))
      this->on_air_(decode_air(raw, this->addr_air_), now);
  }
}

bool BenqHaloComponent::any_active_() const {
  for (const auto &s : this->slots_)
    if (s.active)
      return true;
  return false;
}

void BenqHaloComponent::replan_() {
  for (uint8_t pass = 0; pass < 2; pass++) {
    const State before = this->believed_;
    const Plan p = plan(this->target_, this->believed_, this->dirty_);
    this->set_slot_(SLOT_BRIGHT, p.bright, p.pb);
    this->set_slot_(SLOT_TEMP, p.temp, p.pt);
    if (this->believed_ == before)
      break;
  }
}

void BenqHaloComponent::set_slot_(uint8_t id, bool want, Payload p) {
  SlotState &s = this->slots_[id];
  if (!want) {
    if (s.active && s.acks)
      this->apply_believed_(s.pay);  // the lamp has most likely applied it
    s.active = false;
    return;
  }
  if (s.active && s.pay == p)
    return;  // same frame: keep its counters
  if (s.active && s.acks)
    this->apply_believed_(s.pay);  // a moving slider: the last value gets a full burst
  s = SlotState{true, p, 0, 0};
}

int BenqHaloComponent::pick_slot_() {
  const bool b = this->slots_[SLOT_BRIGHT].active, t = this->slots_[SLOT_TEMP].active;
  if (b && t) {
    const int id = this->rr_ ? SLOT_TEMP : SLOT_BRIGHT;
    this->rr_ ^= 1;
    return id;
  }
  if (b)
    return SLOT_BRIGHT;
  if (t)
    return SLOT_TEMP;
  SlotState &a = this->slots_[SLOT_AUTO];
  if (!a.active)
    return -1;
  if (!this->target_.power) {  // turned off since the press: effect unknown, drop it
    a.active = false;
    return -1;
  }
  if (!a.attempts)
    a.pay = make_auto(true, this->target_.lamps, a.pay.value);
  return SLOT_AUTO;
}

void BenqHaloComponent::send_packet_(uint32_t now) {
  const int id = this->pick_slot_();
  if (id < 0)
    return;
  SlotState &s = this->slots_[id];
  const uint8_t pay[2] = {s.pay.flags, s.pay.value};
  const Verdict v = this->send_one_(pay, 2);
  this->next_tx_at_ = now + GAP_MS;
  s.attempts++;
  if (v == Verdict::ACK) {
    s.acks++;
    this->set_link_(true);
  }
  ESP_LOGV(TAG, "TX %02X %02X #%u -> %s", s.pay.flags, s.pay.value, s.attempts,
           v == Verdict::ACK ? "ACK" : v == Verdict::MAX_RT ? "no ACK" : v == Verdict::TIMEOUT ? "timeout" : "refused");
  const uint8_t need = this->repeats_ < this->min_acks_ ? this->repeats_ : this->min_acks_;
  const uint8_t most = MAX_ATTEMPTS > this->repeats_ ? MAX_ATTEMPTS : this->repeats_;
  if ((s.attempts >= this->repeats_ && s.acks >= need) || s.attempts >= most) {
    if (s.acks >= need) {
      this->slot_done_((uint8_t) id, now);
    } else {
      this->slot_fail_((uint8_t) id, now);
    }
  }
}

void BenqHaloComponent::slot_done_(uint8_t id, uint32_t now) {
  SlotState &s = this->slots_[id];
  const Payload p = s.pay;
  s.active = false;
  this->failures_ = 0;
  ESP_LOGD(TAG, "Delivered %02X %02X (%u/%u ACKs)", p.flags, p.value, s.acks, s.attempts);
  if (id == SLOT_AUTO) {
    this->last_auto_ = p.value;
    this->schedule_save_(now);
  } else {
    this->apply_believed_(p);
    this->dirty_ &= (uint8_t) ~covered_by(p, this->target_);
    this->schedule_save_(now);
  }
  this->replan_();
}

void BenqHaloComponent::slot_fail_(uint8_t id, uint32_t now) {
  SlotState &s = this->slots_[id];
  if (s.acks) {
    // Weak delivery: the lamp received it at least once.
    if (id == SLOT_AUTO) {
      this->last_auto_ = s.pay.value;
    } else {
      this->apply_believed_(s.pay);
    }
  }
  this->failures_++;
  if (this->failures_ > PLAN_RETRIES) {
    this->give_up_();
    return;
  }
  ESP_LOGD(TAG, "Frame %02X %02X: %u/%u ACKs, retry %u in %" PRIu32 " ms", s.pay.flags, s.pay.value, s.acks,
           s.attempts, this->failures_, RETRY_MS * this->failures_);
  s.attempts = s.acks = 0;
  this->phase_ = Phase::BACKOFF;
  this->retry_at_ = now + RETRY_MS * this->failures_;
}

void BenqHaloComponent::give_up_() {
  // Better a correct state in HA than a request the lamp never received.
  for (auto &s : this->slots_)
    s.active = false;
  const bool changed = this->target_ != this->believed_;
  this->target_ = this->believed_;
  this->dirty_ = 0;
  this->failures_ = 0;
  this->phase_ = Phase::IDLE;
  this->pending_since_ = 0;
  this->st_giveups_++;
  this->set_link_(false);
  ESP_LOGW(TAG, "Lamp unreachable: request dropped (is the link address right? is the lamp powered?)");
  if (changed)
    this->publish_state_();
}

void BenqHaloComponent::apply_believed_(Payload p) { apply_payload(this->believed_, p); }

void BenqHaloComponent::set_link_(bool ok) {
  if (this->link_known_ && this->link_ok_ == ok)
    return;
  this->link_known_ = true;
  this->link_ok_ = ok;
  this->link_cb_.call(ok);
}

void BenqHaloComponent::publish_state_() { this->state_cb_.call(this->target_); }

void BenqHaloComponent::schedule_save_(uint32_t now) {
  if (!this->save_pending_) {
    this->save_pending_ = true;
  }
  this->save_at_ = now + SAVE_DELAY_MS;
}

// ---- remote tracking ----

void BenqHaloComponent::on_air_(const AirFrame &f, uint32_t now) {
  const Kind k = classify(f);
  if (k == Kind::CRC_BAD) {
    this->st_rx_bad_++;
    return;
  }
  this->st_rx_ok_++;
  const Payload p{f.pay[0], f.pay[1]};
  char buf[48];
  if (k == Kind::LAMP_ACK) {
    snprintf(buf, sizeof(buf), "lamp ACK pid %u", f.pid);
    // The lamp answers the remote: it is alive, a pending retry can go now.
    if (this->phase_ == Phase::BACKOFF)
      this->retry_at_ = now;
  } else {
    snprintf(buf, sizeof(buf), "%02X %02X (%s) pid %u", p.flags, p.value, kind_str(k), f.pid);
  }
  ESP_LOGD(TAG, "RX %s", buf);
  this->frame_cb_.call(std::string(buf));

  switch (k) {
    case Kind::TEMP:
    case Kind::BRIGHT:
      this->remote_at_ = now;
      this->on_remote_state_(p);
      this->schedule_save_(now);
      break;
    case Kind::AUTO: {
      this->remote_at_ = now;
      SlotState &a = this->slots_[SLOT_AUTO];
      // The remote just used our number: the lamp would ignore ours as a repeat.
      if (a.active && !a.acks && a.pay.value == p.value)
        a.pay.value = next_auto(p.value);
      this->last_auto_ = a.active ? a.pay.value : p.value;
      this->schedule_save_(now);
      break;
    }
    case Kind::SERVICE:
    case Kind::RESERVED:
      this->remote_at_ = now;
      break;
    default:
      break;
  }
}

void BenqHaloComponent::on_remote_state_(Payload p) {
  const Kind k = kind_of(p);
  for (uint8_t id : {(uint8_t) SLOT_BRIGHT, (uint8_t) SLOT_TEMP})
    if (this->slots_[id].active && this->slots_[id].acks)
      this->apply_believed_(this->slots_[id].pay);
  apply_payload(this->believed_, p);
  const uint8_t changed = apply_payload(this->target_, p);
  // The remote wins field by field; a pending setting on another field still goes.
  this->dirty_ &= (uint8_t) ~(FLD_FLAGS | (k == Kind::BRIGHT ? FLD_BRIGHT : FLD_TEMP));
  this->replan_();
  if (changed)
    this->publish_state_();
}

// ===========================================================================
//  PC presence: lamp follows the PC (transitions only, so a manual change sticks)
// ===========================================================================

void BenqHaloComponent::set_power_(bool on, const char *why) {
  if (!this->booted_ || this->target_.power == on)
    return;
  this->target_.power = on;
  this->dirty_ |= due_fields(this->target_, FLD_FLAGS);
  ESP_LOGI(TAG, "Lamp %s (%s)", on ? "on" : "off", why);
  this->replan_();
  this->publish_state_();
}

void BenqHaloComponent::pc_loop_(uint32_t now) {
#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)
#ifdef USE_NETWORK
  this->pc_.set_network_up(network::is_connected());
#else
  this->pc_.set_network_up(true);
#endif
  const int8_t r = this->pc_.result();
  if (r < 0)
    return;
  const bool online = r == 1;
  if (!this->pc_known_) {
    // First answer after boot: baseline only, never switch the lamp at boot.
    this->pc_known_ = true;
    this->pc_online_ = online;
    ESP_LOGI(TAG, "PC %s is %s", this->pc_.host().c_str(), online ? "online" : "offline");
    this->pc_cb_.call(online);
    return;
  }
  if (online != this->pc_online_) {
    this->pc_online_ = online;
    ESP_LOGI(TAG, "PC %s went %s", this->pc_.host().c_str(), online ? "online" : "offline");
    this->pc_cb_.call(online);
    if (online) {
      this->pc_offline_since_ = 0;
      if (this->pc_control_)
        this->set_power_(true, "PC online");
    } else {
      this->pc_offline_since_ = now == 0 ? 1 : now;
    }
  }
  if (!this->pc_online_ && this->pc_offline_since_ != 0 && now - this->pc_offline_since_ >= this->pc_off_delay_ms_) {
    this->pc_offline_since_ = 0;
    if (this->pc_control_)
      this->set_power_(false, "PC offline");
  }
#else
  (void) now;
#endif
}

// ===========================================================================
//  Entity API
// ===========================================================================

float BenqHaloComponent::brightness_from_raw(uint8_t raw) const {
  raw = clamp_bright(raw);
  const float x = (float) (raw - BRIGHT_MIN) / (float) (BRIGHT_MAX - BRIGHT_MIN);
  return 0.01f + 0.99f * powf(x, 1.0f / this->gamma_);
}

uint8_t BenqHaloComponent::raw_from_brightness(float b) const {
  if (!(b > 0.01f))
    return BRIGHT_MIN;
  if (b > 1.0f)
    b = 1.0f;
  const float x = powf((b - 0.01f) / 0.99f, this->gamma_);
  return clamp_bright((uint8_t) (BRIGHT_MIN + lroundf(x * (BRIGHT_MAX - BRIGHT_MIN))));
}

float BenqHaloComponent::mireds_from_temp(uint8_t t) {
  return MIRED_COLD + (float) clamp_temp(t) * (MIRED_WARM - MIRED_COLD) / (float) TEMP_MAX;
}

uint8_t BenqHaloComponent::temp_from_mireds(float m) {
  if (m < MIRED_COLD)
    m = MIRED_COLD;
  if (m > MIRED_WARM)
    m = MIRED_WARM;
  return clamp_temp((uint8_t) lroundf((m - MIRED_COLD) * TEMP_MAX / (MIRED_WARM - MIRED_COLD)));
}

void BenqHaloComponent::light_request(bool on, float brightness, float mireds) {
  if (!this->booted_)
    return;  // restore/boot writes: nothing is sent at startup
  State t = this->target_;
  t.power = on;
  t.bright = this->raw_from_brightness(brightness);
  t.temp = temp_from_mireds(mireds);
  uint8_t fields = 0;
  if (t.power != this->target_.power)
    fields |= FLD_FLAGS;
  if (t.bright != this->target_.bright)
    fields |= FLD_BRIGHT;
  if (t.temp != this->target_.temp)
    fields |= FLD_TEMP;
  if (!fields)
    return;  // e.g. the echo of a state we just published from the remote
  this->target_ = t;
  this->dirty_ |= due_fields(t, fields);
  ESP_LOGD(TAG, "Request: %s, bright 0x%02X, temp %u", t.power ? "on" : "off", t.bright, t.temp);
  this->replan_();
}

void BenqHaloComponent::set_lamps(uint8_t lamp_bits_in) {
  const uint8_t l = lamp_bits(lamp_bits_in);
  if (l == this->target_.lamps)
    return;
  this->target_.lamps = l;
  if (this->target_.power) {
    this->dirty_ |= due_fields(this->target_, FLD_FLAGS);
    this->replan_();
  } else {
    // Off: remembered, applied by the next turn-on (the frame always carries the lamp bits).
    this->believed_.lamps = l;
    this->schedule_save_(millis());
  }
  this->publish_state_();
}

void BenqHaloComponent::press_auto() {
  if (!this->target_.power) {
    ESP_LOGW(TAG, "Button A ignored: lamp is off");
    return;
  }
  SlotState &a = this->slots_[SLOT_AUTO];
  if (a.active)
    return;
  a = SlotState{true, make_auto(true, this->target_.lamps, next_auto(this->last_auto_)), 0, 0};
  ESP_LOGD(TAG, "Button A, press #%u", a.pay.value);
}

void BenqHaloComponent::resync() {
  this->dirty_ = this->target_.power ? FLD_ALL : FLD_FLAGS;
  // Force fresh bursts even if identical frames are already queued.
  for (auto &s : this->slots_)
    if (s.active)
      s.attempts = s.acks = 0;
  this->replan_();
  ESP_LOGI(TAG, "Resync: resending the full state");
}

// ===========================================================================
//  Tools: pairing beacon sniff, and (experimental) lamp re-pairing
// ===========================================================================

void BenqHaloComponent::start_pairing_sniff() {
  if (!this->present_)
    return;
  for (auto &s : this->slots_)
    s.active = false;
  this->phase_ = Phase::IDLE;
  this->activity_ = Activity::PAIR_SNIFF;
  this->tool_until_ = millis() + TOOL_DURATION_MS;
  this->tool_sent_ = 0;
  this->sniff_n_ = 0;
  ESP_LOGW(TAG, "Listening for the pairing beacon for 60 s. On the REMOTE: hold favourite + lamp switch ~5 s.");
}

void BenqHaloComponent::run_pair_sniff_(uint32_t now) {
  this->want_mode_(Mode::RX, PAIRING_ADDR_REG);
  uint8_t raw[8];
  if (this->poll_rx_(now, raw)) {
    uint8_t air[4];
    reg_to_air(PAIRING_ADDR_REG, air);
    const AirFrame f = decode_air(raw, air);
    if (f.crc_ok) {
      char buf[64];
      if (f.len == 0) {
        snprintf(buf, sizeof(buf), "pairing: lamp ACK pid %u", f.pid);
      } else {
        snprintf(buf, sizeof(buf), "pairing beacon: %02X %02X (len %u, pid %u)", f.pay[0], f.pay[1], f.len, f.pid);
      }
      ESP_LOGI(TAG, "%s", buf);
      this->frame_cb_.call(std::string(buf));
      this->tool_sent_++;
      // Beacon cycles 5A5A, X1X2, X3X4: capture X1..X4 (the remote's ID).
      if (f.len == 2 && (f.pay[0] != this->sniff_prev_[0] || f.pay[1] != this->sniff_prev_[1])) {
        this->sniff_prev_[0] = f.pay[0];
        this->sniff_prev_[1] = f.pay[1];
        if (f.pay[0] == 0x5A && f.pay[1] == 0x5A) {
          this->sniff_n_ = 1;
        } else if (this->sniff_n_ == 1 || this->sniff_n_ == 2) {
          this->sniff_seq_[(this->sniff_n_ - 1) * 2] = f.pay[0];
          this->sniff_seq_[(this->sniff_n_ - 1) * 2 + 1] = f.pay[1];
          if (++this->sniff_n_ == 3 && !(this->beacon_known_ && memcmp(this->beacon_id_, this->sniff_seq_, 4) == 0)) {
            memcpy(this->beacon_id_, this->sniff_seq_, 4);
            this->beacon_known_ = true;
            ESP_LOGW(TAG, "Remote beacon ID captured: %02X%02X%02X%02X  (add  remote_beacon: \"%02X%02X%02X%02X\"  to YAML)",
                     this->beacon_id_[0], this->beacon_id_[1], this->beacon_id_[2], this->beacon_id_[3],
                     this->beacon_id_[0], this->beacon_id_[1], this->beacon_id_[2], this->beacon_id_[3]);
          }
        } else {
          this->sniff_n_ = 0;
        }
      }
    }
  }
  if ((int32_t) (now - this->tool_until_) >= 0) {
    ESP_LOGW(TAG, "Pairing sniff finished: %u valid frames", this->tool_sent_);
    this->end_tool_();
  }
}

// ---------------------------------------------------------------------------
//  Address scan: test link addresses derived from the remote's beacon ID by
//  simple byte-wise relations fitted to the one known pair (upstream: beacon
//  F5C3CF49 -> air 63FDF04F). Speculative: the real derivation may be a hash.
//  A hit is proven by frames whose CRC (which covers the address) checks out.
// ---------------------------------------------------------------------------

static uint8_t rev8(uint8_t x) {
  x = (uint8_t) (((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
  x = (uint8_t) (((x & 0xCC) >> 2) | ((x & 0x33) << 2));
  return (uint8_t) (((x & 0xAA) >> 1) | ((x & 0x55) << 1));
}

void BenqHaloComponent::build_candidates_() {
  static const uint8_t REF_B[4] = {0xF5, 0xC3, 0xCF, 0x49};
  static const uint8_t REF_A[4] = {0x63, 0xFD, 0xF0, 0x4F};  // on air
  this->candidates_.clear();
  auto add = [this](const uint8_t air[4]) {
    std::array<uint8_t, 4> reg{air[3], air[2], air[1], air[0]};
    for (const auto &c : this->candidates_)
      if (c == reg)
        return;
    this->candidates_.push_back(reg);
  };
  uint8_t p[4] = {0, 1, 2, 3};
  do {
    for (uint8_t op = 0; op < 5; op++) {
      uint8_t air[4];
      for (uint8_t i = 0; i < 4; i++) {
        const uint8_t rb = REF_B[p[i]], mb = this->beacon_id_[p[i]], ra = REF_A[i];
        switch (op) {
          case 0:  // xor constant
            air[i] = (uint8_t) (mb ^ (rb ^ ra));
            break;
          case 1:  // add constant
            air[i] = (uint8_t) (mb + (uint8_t) (ra - rb));
            break;
          case 2:  // bit-reverse then xor
            air[i] = (uint8_t) (rev8(mb) ^ (rev8(rb) ^ ra));
            break;
          case 3:  // bit-reverse then add
            air[i] = (uint8_t) (rev8(mb) + (uint8_t) (ra - rev8(rb)));
            break;
          default:  // beacon bytes used as-is
            air[i] = mb;
            break;
        }
      }
      add(air);
    }
  } while (std::next_permutation(p, p + 4));
}

void BenqHaloComponent::start_address_scan() {
  if (!this->present_)
    return;
  if (!this->beacon_known_) {
    ESP_LOGW(TAG, "Address scan needs your remote's beacon ID: run 'Sniff remote pairing' first, or set remote_beacon");
    return;
  }
  this->build_candidates_();
  for (auto &s : this->slots_)
    s.active = false;
  this->phase_ = Phase::IDLE;
  this->activity_ = Activity::ADDR_SCAN;
  this->cand_idx_ = 0;
  this->cand_hits_ = 0;
  this->scan_round_ = 0;
  this->cand_until_ = millis() + SCAN_DWELL_MS;
  ESP_LOGW(TAG, "Address scan: %u candidates from beacon %02X%02X%02X%02X, %u ms each (~%u s per round, 2 rounds). "
                "KEEP TURNING THE REMOTE'S DIAL the whole time.",
           (unsigned) this->candidates_.size(), this->beacon_id_[0], this->beacon_id_[1], this->beacon_id_[2],
           this->beacon_id_[3], (unsigned) SCAN_DWELL_MS,
           (unsigned) (this->candidates_.size() * SCAN_DWELL_MS / 1000));
}

void BenqHaloComponent::run_addr_scan_(uint32_t now) {
  const auto &cand = this->candidates_[this->cand_idx_];
  this->want_mode_(Mode::RX, cand.data());
  uint8_t raw[8];
  if (this->poll_rx_(now, raw)) {
    uint8_t air[4];
    reg_to_air(cand.data(), air);
    const AirFrame f = decode_air(raw, air);
    if (f.crc_ok) {
      ESP_LOGI(TAG, "Candidate %02X%02X%02X%02X: valid frame len %u %02X %02X", cand[0], cand[1], cand[2], cand[3],
               f.len, f.pay[0], f.pay[1]);
      if (++this->cand_hits_ >= 2) {
        memcpy(this->addr_reg_, cand.data(), 4);
        reg_to_air(this->addr_reg_, this->addr_air_);
        ESP_LOGW(TAG, "FOUND your link address: %02X%02X%02X%02X. Using it now; put  address: \"%02X%02X%02X%02X\"  in YAML.",
                 cand[0], cand[1], cand[2], cand[3], cand[0], cand[1], cand[2], cand[3]);
        char buf[48];
        snprintf(buf, sizeof(buf), "address found: %02X%02X%02X%02X", cand[0], cand[1], cand[2], cand[3]);
        this->frame_cb_.call(std::string(buf));
        this->end_tool_();
        return;
      }
    }
  }
  if ((int32_t) (now - this->cand_until_) >= 0) {
    this->cand_hits_ = 0;
    if (++this->cand_idx_ >= this->candidates_.size()) {
      this->cand_idx_ = 0;
      if (++this->scan_round_ >= 2) {
        ESP_LOGW(TAG, "Address scan: no candidate matched. The derivation isn't a simple byte relation; "
                      "the CC2500 raw capture route is next.");
        this->end_tool_();
        return;
      }
      ESP_LOGI(TAG, "Address scan: round %u", this->scan_round_ + 1);
    }
    this->cand_until_ = now + SCAN_DWELL_MS;
  }
}

void BenqHaloComponent::start_pairing() {
  if (!this->present_)
    return;
  for (auto &s : this->slots_)
    s.active = false;
  this->phase_ = Phase::IDLE;
  this->activity_ = Activity::PAIR_TX;
  this->tool_until_ = millis() + TOOL_DURATION_MS;
  this->tool_next_ = millis();
  this->tool_idx_ = 0;
  this->tool_acks_ = 0;
  this->tool_sent_ = 0;
  ESP_LOGW(TAG, "EXPERIMENTAL lamp pairing: beacon running for up to 60 s. Now: unplug the lamp, cover its "
                "light sensor, plug it back in.");
}

void BenqHaloComponent::run_pair_tx_(uint32_t now) {
  this->want_mode_(Mode::TX, PAIRING_ADDR_REG);
  if (this->mode_ == Mode::TX && (int32_t) (now - this->tool_next_) >= 0) {
    const Payload p = this->pairing_payloads_[this->tool_idx_ % this->pairing_payloads_.size()];
    const uint8_t pay[2] = {p.flags, p.value};
    const Verdict v = this->send_one_(pay, 2);
    this->tool_sent_++;
    this->tool_idx_++;
    this->tool_next_ = now + PAIR_INTERVAL_MS;
    if (v == Verdict::ACK) {
      if (this->tool_acks_ == 0) {
        ESP_LOGW(TAG, "Lamp is acknowledging the pairing beacon!");
      }
      this->tool_acks_++;
    }
    if (this->tool_acks_ >= PAIR_ACKS_DONE) {
      ESP_LOGW(TAG, "Pairing done: %u acknowledged beacons out of %u sent. Try the light now.", this->tool_acks_,
               this->tool_sent_);
      this->set_link_(true);
      this->end_tool_();
      return;
    }
  }
  if ((int32_t) (now - this->tool_until_) >= 0) {
    ESP_LOGW(TAG, "Pairing timed out: %u/%u beacons acknowledged. Lamp not in pairing mode?", this->tool_acks_,
             this->tool_sent_);
    this->end_tool_();
  }
}

void BenqHaloComponent::end_tool_() {
  this->activity_ = Activity::NORMAL;
  this->mode_ = Mode::UNKNOWN;  // force a reconfig on the link address
}

}  // namespace benq_halo
}  // namespace esphome
