#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "bc5602_regs.h"
#include "pc_presence.h"
#include "halo1_protocol.h"

namespace esphome {
namespace benq_halo {

// Hub: drives a BM5602 (Holtek BC5602) as the ScreenBar Halo 1 remote.
//  - commands go out as acknowledged bursts (3 frames, 100 ms apart, >= 2 ACKs),
//    exactly like the original remote;
//  - between bursts it listens passively (never ACKs) to the real remote and
//    tracks the state it sends, so Home Assistant follows the dial/buttons;
//  - two maintenance tools: sniff the pairing beacon, and (experimental)
//    re-pair the lamp to this ESP by replaying a pairing beacon.
class BenqHaloComponent : public Component,
                          public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  // ---- configuration (codegen) ----
  void set_address(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3) {
    addr_reg_[0] = a0;
    addr_reg_[1] = a1;
    addr_reg_[2] = a2;
    addr_reg_[3] = a3;
  }
  void set_listen(bool listen) { listen_ = listen; }
  void set_brightness_gamma(float g) { gamma_ = g; }
  void set_repeats(uint8_t r) { repeats_ = r; }
  void set_min_acks(uint8_t a) { min_acks_ = a; }
  void add_pairing_payload(uint8_t a, uint8_t b) { pairing_payloads_.push_back({a, b}); }

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- API for the entity platforms ----
  bool ready() const { return booted_; }
  // From the light entity: absolute on/off, brightness 0..1, colour temp (mireds).
  void light_request(bool on, float brightness, float mireds);
  void set_lamps(uint8_t lamp_bits);  // proto::F_FRONT / F_BACK / both
  void press_auto();
  void resync();
  void start_pairing();
  void start_pairing_sniff();
  void start_address_scan();
  void set_remote_beacon(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    beacon_id_[0] = a;
    beacon_id_[1] = b;
    beacon_id_[2] = c;
    beacon_id_[3] = d;
    beacon_known_ = true;
  }

  const proto::State &target() const { return target_; }
  float brightness_from_raw(uint8_t raw) const;
  uint8_t raw_from_brightness(float b) const;
  static float mireds_from_temp(uint8_t t);
  static uint8_t temp_from_mireds(float m);

  void add_on_state_callback(std::function<void(const proto::State &)> &&cb) { state_cb_.add(std::move(cb)); }
  void add_on_link_callback(std::function<void(bool)> &&cb) { link_cb_.add(std::move(cb)); }
#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)
  void set_pc_presence(const std::string &host, uint32_t interval_ms, uint32_t off_delay_ms, uint16_t port,
                       bool control_lamp) {
    this->pc_.configure(host, interval_ms, port);
    this->pc_off_delay_ms_ = off_delay_ms;
    this->pc_control_ = control_lamp;
  }
#endif
  void add_on_pc_callback(std::function<void(bool)> &&cb) { pc_cb_.add(std::move(cb)); }
  void add_on_frame_callback(std::function<void(const std::string &)> &&cb) { frame_cb_.add(std::move(cb)); }

  static constexpr float MIRED_COLD = 153.0f;  // temp 0x00, nominal ~6500 K
  static constexpr float MIRED_WARM = 370.0f;  // temp 0x64, nominal ~2700 K

 protected:
  // ---- SPI primitives ----
  void cmd_(uint8_t c);
  uint8_t rd_(uint8_t reg);
  void wr_(uint8_t reg, uint8_t v);
  void cmd_data_(uint8_t c, const uint8_t *d, size_t n);
  void read_fifo_(uint8_t *out, size_t n);
  void set_bank_(uint8_t bank);
  uint8_t omst_() { return this->rd_(bc::B0_STA1) & bc::OMST_MASK; }

  // ---- chip bring-up and proven sequences ----
  bool init_chip_();
  void load_recommended_();
  bool calibrate_();
  void configure_(const uint8_t addr_reg[4], bool receiver);
  void passive_overrides_();
  bool enter_rx_(uint32_t timeout_us = 1500);

  // ---- radio mode machine (non-blocking 40 ms soft reset) ----
  enum class Mode : uint8_t { UNKNOWN, RESETTING, TX, RX };
  void want_mode_(Mode m, const uint8_t addr_reg[4]);
  void begin_reset_(Mode target, const uint8_t addr_reg[4]);
  void service_radio_(uint32_t now);
  enum class Verdict : uint8_t { ACK, MAX_RT, TIMEOUT, REFUSED };
  Verdict send_one_(const uint8_t *pay, uint8_t len);
  bool poll_rx_(uint32_t now, uint8_t raw[8]);

  // ---- command engine ----
  enum Slot : uint8_t { SLOT_BRIGHT = 0, SLOT_TEMP = 1, SLOT_AUTO = 2, SLOT_N = 3 };
  struct SlotState {
    bool active = false;
    proto::Payload pay{0, 0};
    uint8_t attempts = 0, acks = 0;
  };
  enum class Phase : uint8_t { IDLE, BURST, BACKOFF };
  enum class Activity : uint8_t { NORMAL, PAIR_TX, PAIR_SNIFF, ADDR_SCAN };

  void drive_(uint32_t now);
  void replan_();
  void set_slot_(uint8_t id, bool want, proto::Payload p);
  bool any_active_() const;
  int pick_slot_();
  void send_packet_(uint32_t now);
  void slot_done_(uint8_t id, uint32_t now);
  void slot_fail_(uint8_t id, uint32_t now);
  void give_up_();
  void on_air_(const proto::AirFrame &f, uint32_t now);
  void on_remote_state_(proto::Payload p);
  void apply_believed_(proto::Payload p);
  void set_link_(bool ok);
  void publish_state_();
  void schedule_save_(uint32_t now);
  void run_pair_tx_(uint32_t now);
  void run_pair_sniff_(uint32_t now);
  void run_addr_scan_(uint32_t now);
  void build_candidates_();
  void end_tool_();
  void update_high_freq_();
  void set_power_(bool on, const char *why);
  void pc_loop_(uint32_t now);

  // config
  uint8_t addr_reg_[4] = {0x4F, 0xF0, 0xFD, 0x63};
  uint8_t addr_air_[4] = {0x63, 0xFD, 0xF0, 0x4F};
  bool listen_{true};
  float gamma_{2.0f};
  uint8_t repeats_{3};
  uint8_t min_acks_{2};
  std::vector<proto::Payload> pairing_payloads_;

  // chip / radio
  bool present_{false};
  uint32_t chip_version_{0};
  bool calibrated_{false};
  bool xtal_ready_{false}, cal1_ok_{false}, cal2_ok_{false};
  uint8_t om_after_cal_{0xFF};
  uint32_t last_init_attempt_{0};
  Mode mode_{Mode::UNKNOWN};
  Mode mode_target_{Mode::UNKNOWN};
  uint8_t mode_addr_[4] = {0, 0, 0, 0};
  uint32_t reset_at_{0};
  uint32_t last_arm_{0}, last_frame_{0}, last_full_{0};
  uint8_t verify_fails_{0};
  uint8_t timeouts_in_row_{0};

  // engine
  proto::State target_, believed_;
  uint8_t dirty_{0};
  SlotState slots_[SLOT_N];
  Phase phase_{Phase::IDLE};
  uint32_t next_tx_at_{0}, retry_at_{0};
  uint32_t remote_at_{0}, pending_since_{0};
  uint8_t failures_{0};
  uint8_t rr_{0};
  uint8_t last_auto_{0};
  bool link_ok_{false};
  bool link_known_{false};

  // tools
  Activity activity_{Activity::NORMAL};
  uint32_t tool_until_{0}, tool_next_{0};
  uint8_t tool_idx_{0};
  uint16_t tool_acks_{0}, tool_sent_{0};
  // remote beacon ID (the 4 bytes after 5A5A) and the address-scan candidates
  uint8_t beacon_id_[4] = {0, 0, 0, 0};
  bool beacon_known_{false};
  uint8_t sniff_prev_[2] = {0, 0};
  uint8_t sniff_seq_[4] = {0, 0, 0, 0};
  uint8_t sniff_n_{0};
  std::vector<std::array<uint8_t, 4>> candidates_;
  size_t cand_idx_{0};
  uint32_t cand_until_{0};
  uint8_t cand_hits_{0};
  uint16_t scan_round_{0};

  // boot / persistence
  bool booted_{false};
  uint32_t boot_at_{0};
  ESPPreferenceObject pref_;
  uint32_t save_at_{0};
  bool save_pending_{false};

  // stats
  uint32_t st_packets_{0}, st_acks_{0}, st_rx_ok_{0}, st_rx_bad_{0}, st_giveups_{0};
  uint32_t st_packets_prev_{0}, st_acks_prev_{0}, st_rx_ok_prev_{0}, st_rx_bad_prev_{0}, stats_at_{0};

  HighFrequencyLoopRequester high_freq_;
  bool high_freq_on_{false};

  CallbackManager<void(const proto::State &)> state_cb_;
  CallbackManager<void(bool)> link_cb_;
  CallbackManager<void(const std::string &)> frame_cb_;
  CallbackManager<void(bool)> pc_cb_;
#if defined(USE_ESP32) && defined(USE_BENQ_HALO_PC_PRESENCE)
  PcPresence pc_;
  uint32_t pc_off_delay_ms_{120000};
  bool pc_control_{true};
  bool pc_known_{false};
  bool pc_online_{false};
  uint32_t pc_offline_since_{0};
#endif
};

}  // namespace benq_halo
}  // namespace esphome
