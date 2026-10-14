#pragma once

#include "esphome/core/defines.h"
#ifdef USE_BUTTON

#include "esphome/components/button/button.h"

#include "benq_halo.h"

namespace esphome {
namespace benq_halo {

enum class ButtonAction : uint8_t { AUTO_MODE, RESYNC, PAIR_LAMP, SNIFF_PAIRING, SCAN_ADDRESSES };

class BenqHaloButton : public button::Button {
 public:
  void set_parent(BenqHaloComponent *parent) { this->parent_ = parent; }
  void set_action(ButtonAction a) { this->action_ = a; }

 protected:
  void press_action() override {
    switch (this->action_) {
      case ButtonAction::AUTO_MODE:
        this->parent_->press_auto();
        break;
      case ButtonAction::RESYNC:
        this->parent_->resync();
        break;
      case ButtonAction::PAIR_LAMP:
        this->parent_->start_pairing();
        break;
      case ButtonAction::SNIFF_PAIRING:
        this->parent_->start_pairing_sniff();
        break;
      case ButtonAction::SCAN_ADDRESSES:
        this->parent_->start_address_scan();
        break;
    }
  }

  BenqHaloComponent *parent_{nullptr};
  ButtonAction action_{ButtonAction::RESYNC};
};

}  // namespace benq_halo
}  // namespace esphome

#endif  // USE_BUTTON
