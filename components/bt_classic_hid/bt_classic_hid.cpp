// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_classic_hid.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace bt_classic_hid {

static const char *const TAG = "bt_classic_hid";

static BTClassicHID *global_instance = nullptr;  // NOLINT
static QueueHandle_t global_queue = nullptr;     // NOLINT

static const uint8_t COD_MAJOR_PERIPHERAL = 0x05;

static std::string format_bda(const uint8_t *bda) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  return buf;
}

static uint64_t bda_to_uint64(const uint8_t *bda) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++)
    v = (v << 8) | bda[i];
  return v;
}

static std::string format_hex_block(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(len * 2);
  char buf[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

// --------------------------------------------------------------------------
// Bluetooth task -> main loop
// --------------------------------------------------------------------------

void BTClassicHID::push_event(BtEvent &ev, const uint8_t *data, uint16_t len) {
  if (global_queue == nullptr)
    return;
  ev.buf = nullptr;
  ev.len = 0;
  if (data != nullptr && len > 0) {
    ev.buf = static_cast<uint8_t *>(malloc(len));
    if (ev.buf == nullptr)
      return;
    memcpy(ev.buf, data, len);
    ev.len = len;
  }
  if (xQueueSend(global_queue, &ev, 0) != pdTRUE && ev.buf != nullptr)
    free(ev.buf);
}

void BTClassicHID::gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  BtEvent ev{};
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      ev.type = BtEvent::DISC_RESULT;
      memcpy(ev.bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
      ev.b = -128;
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
        switch (p->type) {
          case ESP_BT_GAP_DEV_PROP_BDNAME: {
            size_t n = std::min<size_t>(p->len, sizeof(ev.name) - 1);
            memcpy(ev.name, p->val, n);
            ev.name[n] = 0;
            break;
          }
          case ESP_BT_GAP_DEV_PROP_COD:
            ev.a = *static_cast<uint32_t *>(p->val);
            break;
          case ESP_BT_GAP_DEV_PROP_RSSI:
            ev.b = *static_cast<int8_t *>(p->val);
            break;
          case ESP_BT_GAP_DEV_PROP_EIR: {
            if (ev.name[0] != 0)
              break;
            uint8_t len = 0;
            uint8_t *eir = static_cast<uint8_t *>(p->val);
            uint8_t *name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (name == nullptr)
              name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            if (name != nullptr) {
              size_t n = std::min<size_t>(len, sizeof(ev.name) - 1);
              memcpy(ev.name, name, n);
              ev.name[n] = 0;
            }
            break;
          }
          default:
            break;
        }
      }
      push_event(ev);
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      ev.type = BtEvent::DISC_STATE;
      ev.a = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
      push_event(ev);
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      ev.type = BtEvent::AUTH;
      memcpy(ev.bda, param->auth_cmpl.bda, sizeof(esp_bd_addr_t));
      ev.a = param->auth_cmpl.stat;
      strncpy(ev.name, reinterpret_cast<const char *>(param->auth_cmpl.device_name), sizeof(ev.name) - 1);
      push_event(ev);
      break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
      // legacy pairing: answer with the usual default PIN
      esp_bt_pin_code_t pin;
      memset(pin, '0', sizeof(pin));
      esp_bt_gap_pin_reply(param->pin_req.bda, true, param->pin_req.min_16_digit ? 16 : 4, pin);
      break;
    }
#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
      // "Just Works": accept the numeric comparison without user interaction
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
      ESP_LOGI(TAG, "Pairing passkey: %06lu", static_cast<unsigned long>(param->key_notif.passkey));
      break;
    case ESP_BT_GAP_KEY_REQ_EVT:
      ESP_LOGW(TAG, "Remote asks for a passkey, which this host cannot enter");
      break;
#endif
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      ev.type = BtEvent::ACL;
      memcpy(ev.bda, param->acl_conn_cmpl_stat.bda, sizeof(esp_bd_addr_t));
      ev.a = param->acl_conn_cmpl_stat.stat;
      ev.b = 1;
      push_event(ev);
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      ev.type = BtEvent::ACL;
      memcpy(ev.bda, param->acl_disconn_cmpl_stat.bda, sizeof(esp_bd_addr_t));
      ev.a = param->acl_disconn_cmpl_stat.reason;
      ev.b = 0;
      push_event(ev);
      break;
    case ESP_BT_GAP_MODE_CHG_EVT:
      ev.type = BtEvent::LINK_MODE;
      ev.a = param->mode_chg.mode;
      ev.b = param->mode_chg.interval;
      push_event(ev);
      break;
    case ESP_BT_GAP_ENC_CHG_EVT:
      ev.type = BtEvent::ENCRYPTION;
      ev.a = param->enc_chg.enc_mode;
      push_event(ev);
      break;
    default:
      break;
  }
}

void BTClassicHID::hidh_callback(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
  BtEvent ev{};
  switch (event) {
    case ESP_HIDH_INIT_EVT:
      ev.type = BtEvent::HH_INIT;
      ev.a = param->init.status;
      push_event(ev);
      break;
    case ESP_HIDH_OPEN_EVT:
      ev.type = BtEvent::HH_OPEN;
      memcpy(ev.bda, param->open.bd_addr, sizeof(esp_bd_addr_t));
      ev.a = param->open.status;
      ev.b = param->open.conn_status;
      ev.c = param->open.is_orig;
      push_event(ev);
      break;
    case ESP_HIDH_CLOSE_EVT:
      ev.type = BtEvent::HH_CLOSE;
      ev.a = param->close.status;
      ev.b = param->close.reason;
      ev.c = param->close.conn_status;
      push_event(ev);
      break;
    case ESP_HIDH_GET_DSCP_EVT:
      ev.type = BtEvent::HH_DSCP;
      ev.a = param->dscp.status;
      ev.b = param->dscp.vendor_id;
      ev.c = param->dscp.product_id;
      push_event(ev, param->dscp.dsc_list, param->dscp.dl_len);
      break;
    case ESP_HIDH_DATA_IND_EVT:
      ev.type = BtEvent::HH_DATA;
      ev.a = param->data_ind.status;
      ev.b = param->data_ind.proto_mode;
      push_event(ev, param->data_ind.data, param->data_ind.len);
      break;
    default:
      ev.type = BtEvent::HH_OTHER;
      ev.a = event;
      push_event(ev);
      break;
  }
}

// --------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------

void BTClassicHID::setup() {
  global_instance = this;
  this->queue_ = xQueueCreate(24, sizeof(BtEvent));
  global_queue = this->queue_;
  if (this->queue_ == nullptr) {
    this->mark_failed();
    return;
  }

  esp_err_t err;
  // Only Bluetooth Classic is used; give the BLE part of the controller back to the heap.
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if ((err = esp_bt_controller_init(&cfg)) != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if ((err = esp_bluedroid_init()) != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if ((err = esp_bluedroid_enable()) != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  esp_bt_gap_register_callback(BTClassicHID::gap_callback);
  esp_bt_gap_set_device_name(this->device_name_.c_str());

  // Present ourselves as a computer, like the hosts the remote is made for.
  esp_bt_cod_t cod{};
  cod.major = ESP_BT_COD_MAJOR_DEV_COMPUTER;
  cod.minor = 0x01;  // desktop workstation
  esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);

#if (CONFIG_BT_SSP_ENABLED == true)
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;  // "Just Works" pairing
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
#endif
  esp_bt_pin_code_t pin_code{};
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);

  esp_bt_hid_host_register_callback(BTClassicHID::hidh_callback);
  if ((err = esp_bt_hid_host_init()) != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_hid_host_init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
}

void BTClassicHID::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth Classic HID host:");
  const uint8_t *own = esp_bt_dev_get_address();
  if (own != nullptr)
    ESP_LOGCONFIG(TAG, "  Own address: %s", format_bda(own).c_str());
  ESP_LOGCONFIG(TAG, "  Own name: %s", this->device_name_.c_str());
  if (!this->name_filter_.empty())
    ESP_LOGCONFIG(TAG, "  Pair with names starting with: %s", this->name_filter_.c_str());
  if (this->has_address_)
    ESP_LOGCONFIG(TAG, "  Pair with address: %s", format_bda(this->address_).c_str());
  ESP_LOGCONFIG(TAG, "  Paired devices: %d", this->bond_count_());
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_));
  if (this->is_failed())
    ESP_LOGE(TAG, "  Bluetooth setup failed");
}

int BTClassicHID::bond_count_() const { return esp_bt_gap_get_bond_device_num(); }

void BTClassicHID::set_scan_mode_(bool discoverable) {
  // Always connectable, so that a paired remote can reconnect on its own.
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                           discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

// --------------------------------------------------------------------------
// Pairing
// --------------------------------------------------------------------------

void BTClassicHID::start_pairing(uint32_t seconds) {
  if (!this->ready_) {
    ESP_LOGW(TAG, "Bluetooth is not ready yet");
    return;
  }
  ESP_LOGI(TAG, "Pairing mode for %u s: searching for the remote", static_cast<unsigned>(seconds));
  this->pairing_ = true;
  this->pairing_until_ = millis() + seconds * 1000UL;
  this->target_pending_ = false;
  this->seen_.clear();
  this->set_scan_mode_(true);
  if (!this->discovering_ && !this->connecting_)
    this->start_discovery_();
}

void BTClassicHID::stop_pairing() {
  if (!this->pairing_)
    return;
  this->pairing_ = false;
  this->target_pending_ = false;
  if (this->discovering_)
    esp_bt_gap_cancel_discovery();
  this->set_scan_mode_(false);
  ESP_LOGI(TAG, "Pairing mode ended");
}

void BTClassicHID::start_discovery_() {
  // 8 * 1.28 s per inquiry round; restarted while pairing mode lasts
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_bt_gap_start_discovery failed: %s", esp_err_to_name(err));
}

void BTClassicHID::remove_bonds() {
  int num = esp_bt_gap_get_bond_device_num();
  if (this->connected_)
    esp_bt_hid_host_disconnect(this->peer_);
  if (num <= 0) {
    ESP_LOGI(TAG, "No paired devices to remove");
    return;
  }
  std::vector<esp_bd_addr_t> list(num);
  if (esp_bt_gap_get_bond_device_list(&num, list.data()) != ESP_OK)
    return;
  for (int i = 0; i < num; i++) {
    ESP_LOGI(TAG, "Removing pairing with %s", format_bda(list[i]).c_str());
    esp_bt_gap_remove_bond_device(list[i]);
  }
}

bool BTClassicHID::matches_(const BtEvent &ev) const {
  if (this->has_address_ && memcmp(ev.bda, this->address_, sizeof(esp_bd_addr_t)) == 0)
    return true;
  if (!this->name_filter_.empty()) {
    std::string name = str_lower_case(ev.name);
    std::string filter = str_lower_case(this->name_filter_);
    if (name.compare(0, filter.size(), filter) == 0)
      return true;
  }
  // without any filter: take the first keyboard / remote-control style device
  if (!this->has_address_ && this->name_filter_.empty()) {
    uint8_t major = (static_cast<uint32_t>(ev.a) >> 8) & 0x1F;
    return major == COD_MAJOR_PERIPHERAL;
  }
  return false;
}

// --------------------------------------------------------------------------
// Main loop
// --------------------------------------------------------------------------

void BTClassicHID::loop() {
  if (this->queue_ == nullptr)
    return;
  BtEvent ev;
  while (xQueueReceive(this->queue_, &ev, 0) == pdTRUE) {
    this->handle_event_(ev);
    if (ev.buf != nullptr)
      free(ev.buf);
  }

  uint32_t now = millis();
  if (this->pairing_ && static_cast<int32_t>(now - this->pairing_until_) > 0) {
    ESP_LOGW(TAG, "No remote paired within the pairing time");
    this->stop_pairing();
  }
  if (this->reconnect_pending_ && static_cast<int32_t>(now - this->reconnect_at_) >= 0) {
    this->reconnect_pending_ = false;
    if (!this->connected_ && !this->connecting_ && !this->pairing_) {
      int num = 1;
      esp_bd_addr_t bonded;
      if (esp_bt_gap_get_bond_device_list(&num, &bonded) == ESP_OK && num > 0) {
        ESP_LOGI(TAG, "Reconnecting to paired remote %s", format_bda(bonded).c_str());
        this->connecting_ = true;
        this->connect_started_ = now;
        if (esp_bt_hid_host_connect(bonded) != ESP_OK)
          this->connecting_ = false;
      }
    }
  }
  // a connection attempt that never reports back must not block pairing mode forever
  if (this->connecting_ && now - this->connect_started_ > 40000) {
    ESP_LOGW(TAG, "Connection attempt timed out");
    this->connecting_ = false;
    if (this->pairing_ && !this->discovering_)
      this->start_discovery_();
  }
}

void BTClassicHID::handle_event_(const BtEvent &ev) {
  switch (ev.type) {
    case BtEvent::HH_INIT: {
      if (ev.a != ESP_HIDH_OK) {
        ESP_LOGE(TAG, "HID host init failed, status %d", static_cast<int>(ev.a));
        this->mark_failed();
        break;
      }
      this->ready_ = true;
      this->set_scan_mode_(false);
      int bonds = this->bond_count_();
      ESP_LOGI(TAG, "HID host ready, %d paired device(s)", bonds);
      if (bonds == 0) {
        if (this->auto_pairing_)
          this->start_pairing(600);
      } else {
        // After a restart the link is gone; try once to get it back. If the
        // remote is asleep this fails and it reconnects on the next key press.
        this->reconnect_pending_ = true;
        this->reconnect_at_ = millis() + 3000;
      }
      break;
    }

    case BtEvent::DISC_STATE: {
      this->discovering_ = ev.a != 0;
      if (this->discovering_)
        break;
      if (this->target_pending_) {
        this->target_pending_ = false;
        ESP_LOGI(TAG, "Connecting to %s", format_bda(this->target_).c_str());
        this->connecting_ = true;
        this->connect_started_ = millis();
        esp_err_t err = esp_bt_hid_host_connect(this->target_);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "esp_bt_hid_host_connect failed: %s", esp_err_to_name(err));
          this->connecting_ = false;
        }
      }
      if (this->pairing_ && !this->connecting_ && !this->connected_)
        this->start_discovery_();
      break;
    }

    case BtEvent::DISC_RESULT: {
      uint64_t addr = bda_to_uint64(ev.bda);
      bool known = std::find(this->seen_.begin(), this->seen_.end(), addr) != this->seen_.end();
      bool match = this->matches_(ev);
      if (!known || match) {
        if (!known && this->seen_.size() < 64)
          this->seen_.push_back(addr);
        ESP_LOGI(TAG, "Found %s  name='%s'  class=0x%06X  RSSI=%d%s", format_bda(ev.bda).c_str(), ev.name,
                 static_cast<unsigned>(ev.a), static_cast<int>(ev.b), match ? "  <- matches" : "");
      }
      if (match && this->pairing_ && !this->target_pending_ && !this->connecting_ && !this->connected_) {
        memcpy(this->target_, ev.bda, sizeof(esp_bd_addr_t));
        this->target_pending_ = true;
        esp_bt_gap_cancel_discovery();  // connect once the inquiry has stopped
      }
      break;
    }

    case BtEvent::AUTH:
      if (ev.a == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Paired with %s ('%s')", format_bda(ev.bda).c_str(), ev.name);
      } else {
        ESP_LOGW(TAG, "Pairing with %s failed, status %d", format_bda(ev.bda).c_str(), static_cast<int>(ev.a));
      }
      break;

    case BtEvent::ACL:
      if (ev.b != 0) {
        ESP_LOGD(TAG, "Link to %s up, status %d", format_bda(ev.bda).c_str(), static_cast<int>(ev.a));
      } else {
        ESP_LOGD(TAG, "Link to %s down, reason 0x%02X", format_bda(ev.bda).c_str(), static_cast<unsigned>(ev.a));
      }
      break;

    case BtEvent::LINK_MODE:
      ESP_LOGD(TAG, "Link mode: %s, interval %.1f ms", ev.a == ESP_BT_PM_MD_ACTIVE ? "active" : ev.a == ESP_BT_PM_MD_SNIFF ? "sniff" : "other",
               ev.b * 0.625f);
      break;

    case BtEvent::ENCRYPTION:
      ESP_LOGD(TAG, "Link encryption mode: %d", static_cast<int>(ev.a));
      break;

    case BtEvent::HH_OTHER:
      ESP_LOGD(TAG, "HID host event %d", static_cast<int>(ev.a));
      break;

    case BtEvent::HH_OPEN: {
      if (ev.a == ESP_HIDH_OK && ev.b == ESP_HIDH_CONN_STATE_CONNECTED) {
        memcpy(this->peer_, ev.bda, sizeof(esp_bd_addr_t));
        this->connecting_ = false;
        this->connected_ = true;
        ESP_LOGI(TAG, "Connected to %s (%s)", format_bda(ev.bda).c_str(),
                 ev.c ? "started by this host" : "started by the remote");
        if (this->pairing_) {
          this->pairing_ = false;
          this->target_pending_ = false;
          if (this->discovering_)
            esp_bt_gap_cancel_discovery();
          this->set_scan_mode_(false);
        }
        this->connect_trigger_.trigger();
      } else if (ev.a == ESP_HIDH_OK && ev.b == ESP_HIDH_CONN_STATE_CONNECTING) {
        ESP_LOGD(TAG, "Connecting to %s ...", format_bda(ev.bda).c_str());
      } else {
        ESP_LOGW(TAG, "Connection to %s failed, status %d, state %d", format_bda(ev.bda).c_str(),
                 static_cast<int>(ev.a), static_cast<int>(ev.b));
        this->connecting_ = false;
        if (this->pairing_ && !this->discovering_)
          this->start_discovery_();
      }
      break;
    }

    case BtEvent::HH_CLOSE: {
      ESP_LOGI(TAG, "Disconnected, status %d, reason %d", static_cast<int>(ev.a), static_cast<int>(ev.b));
      bool was_connected = this->connected_;
      this->connected_ = false;
      this->connecting_ = false;
      this->release_all_keys_();
      if (was_connected)
        this->disconnect_trigger_.trigger();
      if (this->pairing_ && !this->discovering_)
        this->start_discovery_();
      break;
    }

    case BtEvent::HH_DSCP: {
      ESP_LOGI(TAG, "HID descriptor: status %d, vendor 0x%04X, product 0x%04X, %u bytes", static_cast<int>(ev.a),
               static_cast<unsigned>(ev.b), static_cast<unsigned>(ev.c), ev.len);
      if (ev.buf == nullptr || ev.len == 0)
        break;
      for (uint16_t off = 0; off < ev.len; off += 32) {
        ESP_LOGI(TAG, "  %03u: %s", off,
                 format_hex_block(ev.buf + off, std::min<size_t>(32, ev.len - off)).c_str());
      }
      if (this->report_map_.parse(ev.buf, ev.len)) {
        ESP_LOGI(TAG, "  parsed %u input field(s), report ids: %s",
                 static_cast<unsigned>(this->report_map_.field_count()),
                 YESNO(this->report_map_.has_report_ids()));
      } else {
        ESP_LOGW(TAG, "  descriptor has no usable input fields");
      }
      break;
    }

    case BtEvent::HH_DATA:
      if (ev.buf != nullptr && ev.len > 0)
        this->handle_report_(ev.buf, ev.len);
      break;

    default:
      break;
  }
}

void BTClassicHID::handle_report_(const uint8_t *data, uint16_t len) {
  ESP_LOGD(TAG, "Report: %s", format_hex_block(data, len).c_str());
  if (this->report_map_.empty())
    return;

  uint8_t report_id = 0;
  if (this->report_map_.has_report_ids()) {
    report_id = data[0];
    data++;
    len--;
  }
  std::vector<HidUsage> active;
  this->report_map_.decode(report_id, data, len, active);
  std::vector<HidUsage> &held = this->held_[report_id];

  auto emit = [this](HidUsage usage, bool pressed) {
    char code[12];
    snprintf(code, sizeof(code), "%02X_%04X", static_cast<unsigned>(usage >> 16), static_cast<unsigned>(usage & 0xFFFF));
    const char *name = hid_usage_name(usage);
    ESP_LOGI(TAG, "Key %s (%s) %s", name != nullptr ? name : code, code, pressed ? "pressed" : "released");
    this->key_trigger_.trigger(name != nullptr ? name : code, code, pressed);
  };

  for (HidUsage u : held) {
    if (std::find(active.begin(), active.end(), u) == active.end())
      emit(u, false);
  }
  for (HidUsage u : active) {
    if (std::find(held.begin(), held.end(), u) == held.end())
      emit(u, true);
  }
  held = active;
}

void BTClassicHID::release_all_keys_() {
  for (auto &entry : this->held_) {
    for (HidUsage usage : entry.second) {
      char code[12];
      snprintf(code, sizeof(code), "%02X_%04X", static_cast<unsigned>(usage >> 16),
               static_cast<unsigned>(usage & 0xFFFF));
      const char *name = hid_usage_name(usage);
      this->key_trigger_.trigger(name != nullptr ? name : code, code, false);
    }
    entry.second.clear();
  }
}

}  // namespace bt_classic_hid
}  // namespace esphome

#endif  // USE_ESP32
