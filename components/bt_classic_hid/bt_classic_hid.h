// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_ESP32

#include <map>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh_api.h>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "hid_report.h"

namespace esphome {
namespace bt_classic_hid {

/// One event handed from the Bluetooth task to the main loop.
struct BtEvent {
  enum Type : uint8_t {
    DISC_RESULT,
    DISC_STATE,
    AUTH,
    ACL,
    LINK_MODE,
    ENCRYPTION,
    HH_INIT,
    HH_OPEN,
    HH_CLOSE,
    HH_DSCP,
    HH_DATA,
    HH_OTHER,
  };
  Type type;
  esp_bd_addr_t bda;
  int32_t a;
  int32_t b;
  int32_t c;
  uint16_t len;
  uint8_t *buf;  // heap copy, freed by the main loop
  char name[32];
};

/// Bluetooth Classic HID host: pairs with one remote control / keyboard and
/// reports its key presses.
class BTClassicHID : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_name_filter(const std::string &filter) { this->name_filter_ = filter; }
  void set_address(uint64_t address) {
    this->has_address_ = true;
    for (int i = 0; i < 6; i++)
      this->address_[i] = (address >> (8 * (5 - i))) & 0xFF;
  }
  void set_auto_pairing(bool auto_pairing) { this->auto_pairing_ = auto_pairing; }

  /// Search for the remote and pair with it; gives up after `seconds`.
  void start_pairing(uint32_t seconds);
  void stop_pairing();
  /// Forget all paired devices.
  void remove_bonds();

  bool is_connected() const { return this->connected_; }
  bool is_pairing() const { return this->pairing_; }

  Trigger<std::string, std::string, bool> *get_key_trigger() { return &this->key_trigger_; }
  Trigger<> *get_connect_trigger() { return &this->connect_trigger_; }
  Trigger<> *get_disconnect_trigger() { return &this->disconnect_trigger_; }

 protected:
  static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void hidh_callback(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param);
  static void push_event(BtEvent &ev, const uint8_t *data = nullptr, uint16_t len = 0);

  void handle_event_(const BtEvent &ev);
  void handle_report_(const uint8_t *data, uint16_t len);
  void release_all_keys_();
  void start_discovery_();
  bool matches_(const BtEvent &ev) const;
  int bond_count_() const;
  void set_scan_mode_(bool discoverable);

  std::string device_name_{"esphome-hid-host"};
  std::string name_filter_;
  bool has_address_{false};
  esp_bd_addr_t address_{};
  bool auto_pairing_{true};

  QueueHandle_t queue_{nullptr};
  bool ready_{false};
  bool connected_{false};
  bool pairing_{false};
  bool discovering_{false};
  bool connecting_{false};
  bool target_pending_{false};
  esp_bd_addr_t target_{};
  esp_bd_addr_t peer_{};
  uint32_t pairing_until_{0};
  uint32_t connect_started_{0};
  bool reconnect_pending_{false};
  uint32_t reconnect_at_{0};
  std::vector<uint64_t> seen_;

  HidReportMap report_map_;
  std::map<uint8_t, std::vector<HidUsage>> held_;

  Trigger<std::string, std::string, bool> key_trigger_;
  Trigger<> connect_trigger_;
  Trigger<> disconnect_trigger_;
};

}  // namespace bt_classic_hid
}  // namespace esphome

#endif  // USE_ESP32
