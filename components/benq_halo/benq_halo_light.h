#pragma once

#include "esphome/core/defines.h"
#ifdef USE_LIGHT

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"

#include "benq_halo.h"

namespace esphome {
namespace benq_halo {

// One light for the whole bar: both lamps share brightness and temperature
// (a radio frame carries a single value). Lamp selection is a separate select.
class BenqHaloLight : public light::LightOutput {
 public:
  void set_parent(BenqHaloComponent *parent) {
    this->parent_ = parent;
    parent->add_on_state_callback([this](const proto::State &s) { this->publish_(s); });
  }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
    traits.set_min_mireds(BenqHaloComponent::MIRED_COLD);
    traits.set_max_mireds(BenqHaloComponent::MIRED_WARM);
    return traits;
  }

  void setup_state(light::LightState *state) override { this->state_ = state; }

  void write_state(light::LightState *state) override {
    const auto &v = state->current_values;
    // Raw values: our own gamma is applied in the hub (lamp is ~logarithmic).
    this->parent_->light_request(v.get_state() > 0.0f, v.get_brightness(), v.get_color_temperature());
  }

 protected:
  // State coming from the real remote (or a give-up / boot restore): mirror it
  // into HA. The resulting write_state() is a no-op because the hub's target
  // already matches.
  void publish_(const proto::State &s) {
    if (this->state_ == nullptr)
      return;
    auto call = this->state_->make_call();
    call.set_state(s.power);
    call.set_brightness(this->parent_->brightness_from_raw(s.bright));
    call.set_color_temperature(BenqHaloComponent::mireds_from_temp(s.temp));
    call.set_transition_length(0);
    call.perform();
  }

  BenqHaloComponent *parent_{nullptr};
  light::LightState *state_{nullptr};
};

}  // namespace benq_halo
}  // namespace esphome

#endif  // USE_LIGHT
