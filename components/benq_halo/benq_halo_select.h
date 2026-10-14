#pragma once

#include "esphome/core/defines.h"
#ifdef USE_SELECT

#include "esphome/components/select/select.h"

#include "benq_halo.h"

namespace esphome {
namespace benq_halo {

// Options, in this order (codegen): "Front", "Back", "Both".
class BenqHaloLampSelect : public select::Select {
 public:
  void set_parent(BenqHaloComponent *parent) {
    this->parent_ = parent;
    parent->add_on_state_callback([this](const proto::State &s) { this->publish_state(index_for_(s.lamps)); });
  }

 protected:
  static size_t index_for_(uint8_t lamps) {
    if (lamps == proto::F_FRONT)
      return 0;
    if (lamps == proto::F_BACK)
      return 1;
    return 2;
  }
  void control(size_t index) override {
    static const uint8_t BITS[3] = {proto::F_FRONT, proto::F_BACK, proto::F_LAMPS};
    if (index > 2)
      return;
    this->parent_->set_lamps(BITS[index]);
    this->publish_state(index);
  }

  BenqHaloComponent *parent_{nullptr};
};

}  // namespace benq_halo
}  // namespace esphome

#endif  // USE_SELECT
