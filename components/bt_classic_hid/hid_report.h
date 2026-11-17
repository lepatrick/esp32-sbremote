// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal HID report descriptor parser: turns input reports into the set of
// usages that are currently active (keys held down). Plain C++, no ESP
// dependencies, so it can be tested on a PC.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace bt_classic_hid {

/// A usage is identified by (usage page << 16) | usage id.
using HidUsage = uint32_t;

struct HidInputField {
  uint8_t report_id{0};
  uint32_t bit_offset{0};  // offset inside the report payload (after the report id byte)
  uint8_t bit_size{0};     // bits per element
  uint16_t count{0};       // number of elements
  bool is_array{false};    // array: each element holds the index of one active usage
  int32_t logical_min{0};
  HidUsage usage_min{0};          // used when no explicit usage list is given
  std::vector<HidUsage> usages;   // explicit usages, one per element / index
};

class HidReportMap {
 public:
  bool parse(const uint8_t *data, size_t len) {
    this->fields_.clear();
    this->has_report_ids_ = false;

    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    uint8_t report_size = 0;
    uint16_t report_count = 0;
    uint8_t report_id = 0;
    std::vector<HidUsage> usages;
    bool has_range = false;
    HidUsage usage_min = 0;
    // running bit offset of the input report, per report id
    std::vector<std::pair<uint8_t, uint32_t>> offsets;

    auto offset_of = [&offsets](uint8_t id) -> uint32_t & {
      for (auto &o : offsets) {
        if (o.first == id)
          return o.second;
      }
      offsets.emplace_back(id, 0);
      return offsets.back().second;
    };

    size_t i = 0;
    while (i < len) {
      uint8_t prefix = data[i++];
      if (prefix == 0xFE) {  // long item: skip
        if (i + 1 >= len)
          break;
        i += 2 + data[i];
        continue;
      }
      uint8_t size = prefix & 0x03;
      if (size == 3)
        size = 4;
      uint8_t type = (prefix >> 2) & 0x03;
      uint8_t tag = prefix >> 4;
      if (i + size > len)
        break;
      uint32_t value = 0;
      for (uint8_t b = 0; b < size; b++)
        value |= static_cast<uint32_t>(data[i + b]) << (8 * b);
      int32_t svalue = static_cast<int32_t>(value);
      if (size == 1)
        svalue = static_cast<int8_t>(value);
      else if (size == 2)
        svalue = static_cast<int16_t>(value);
      i += size;

      if (type == 1) {  // global item
        switch (tag) {
          case 0:
            usage_page = value;
            break;
          case 1:
            logical_min = svalue;
            break;
          case 7:
            report_size = value;
            break;
          case 8:
            report_id = value;
            this->has_report_ids_ = true;
            break;
          case 9:
            report_count = value;
            break;
          default:
            break;
        }
      } else if (type == 2) {  // local item
        HidUsage full = size == 4 ? value : (static_cast<HidUsage>(usage_page) << 16) | value;
        switch (tag) {
          case 0:
            usages.push_back(full);
            break;
          case 1:
            usage_min = full;
            has_range = true;
            break;
          default:
            break;
        }
      } else if (type == 0) {  // main item
        if (tag == 8) {  // Input
          uint32_t &offset = offset_of(report_id);
          bool is_const = value & 0x01;
          bool is_variable = value & 0x02;
          if (!is_const && report_size > 0 && report_count > 0) {
            HidInputField f;
            f.report_id = report_id;
            f.bit_offset = offset;
            f.bit_size = report_size;
            f.count = report_count;
            f.is_array = !is_variable;
            f.logical_min = logical_min;
            f.usages = usages;
            f.usage_min = has_range ? usage_min : 0;
            if (!has_range && usages.empty())
              f.usage_min = static_cast<HidUsage>(usage_page) << 16;
            this->fields_.push_back(std::move(f));
          }
          offset += static_cast<uint32_t>(report_size) * report_count;
        }
        // every main item ends the scope of the local items
        usages.clear();
        has_range = false;
        usage_min = 0;
      }
    }
    return !this->fields_.empty();
  }

  bool has_report_ids() const { return this->has_report_ids_; }
  bool empty() const { return this->fields_.empty(); }
  size_t field_count() const { return this->fields_.size(); }

  /// Decode one input report. `payload` is the report without the report id byte.
  void decode(uint8_t report_id, const uint8_t *payload, size_t len, std::vector<HidUsage> &active) const {
    active.clear();
    for (const auto &f : this->fields_) {
      if (f.report_id != report_id)
        continue;
      for (uint16_t n = 0; n < f.count; n++) {
        uint32_t bit = f.bit_offset + static_cast<uint32_t>(n) * f.bit_size;
        if (bit + f.bit_size > len * 8)
          break;
        uint32_t v = 0;
        for (uint8_t b = 0; b < f.bit_size && b < 32; b++) {
          uint32_t pos = bit + b;
          if (payload[pos / 8] & (1 << (pos % 8)))
            v |= 1UL << b;
        }
        if (f.is_array) {
          if (v == 0)
            continue;
          int64_t index = static_cast<int64_t>(v) - f.logical_min;
          if (index < 0)
            continue;
          HidUsage u;
          if (!f.usages.empty()) {
            if (static_cast<size_t>(index) >= f.usages.size())
              continue;
            u = f.usages[index];
          } else {
            u = f.usage_min + static_cast<HidUsage>(index);
          }
          if ((u & 0xFFFF) == 0)
            continue;
          // keyboard page: 1..3 are error codes (rollover etc.), not keys
          if ((u >> 16) == 0x07 && (u & 0xFFFF) <= 3)
            continue;
          active.push_back(u);
        } else {
          if (f.bit_size != 1 || v == 0)
            continue;  // only on/off controls are reported as keys
          HidUsage u;
          if (n < f.usages.size())
            u = f.usages[n];
          else if (!f.usages.empty())
            u = f.usages.back();
          else
            u = f.usage_min + n;
          active.push_back(u);
        }
      }
    }
  }

 protected:
  std::vector<HidInputField> fields_;
  bool has_report_ids_{false};
};

/// Readable name for the usages a remote control typically sends, or nullptr.
inline const char *hid_usage_name(HidUsage u) {
  uint16_t page = u >> 16;
  uint16_t id = u & 0xFFFF;
  if (page == 0x07) {  // keyboard
    switch (id) {
      case 0x28: return "ENTER";
      case 0x29: return "ESCAPE";
      case 0x2A: return "BACKSPACE";
      case 0x2B: return "TAB";
      case 0x2C: return "SPACE";
      case 0x4A: return "HOME";
      case 0x4B: return "PAGE_UP";
      case 0x4D: return "END";
      case 0x4E: return "PAGE_DOWN";
      case 0x4F: return "RIGHT";
      case 0x50: return "LEFT";
      case 0x51: return "DOWN";
      case 0x52: return "UP";
      case 0x65: return "APPLICATION";
      case 0x66: return "KEYBOARD_POWER";
      case 0x76: return "KEYBOARD_MENU";
      case 0xE0: return "LEFT_CTRL";
      case 0xE1: return "LEFT_SHIFT";
      case 0xE2: return "LEFT_ALT";
      case 0xE3: return "LEFT_GUI";
      case 0xE4: return "RIGHT_CTRL";
      case 0xE5: return "RIGHT_SHIFT";
      case 0xE6: return "RIGHT_ALT";
      case 0xE7: return "RIGHT_GUI";
      default: return nullptr;
    }
  }
  if (page == 0x0C) {  // consumer
    switch (id) {
      case 0x30: return "POWER";
      case 0x32: return "SLEEP";
      case 0x40: return "MENU";
      case 0x41: return "MENU_PICK";
      case 0x42: return "MENU_UP";
      case 0x43: return "MENU_DOWN";
      case 0x44: return "MENU_LEFT";
      case 0x45: return "MENU_RIGHT";
      case 0x46: return "MENU_ESCAPE";
      case 0x69: return "RED";
      case 0x6A: return "GREEN";
      case 0x6B: return "BLUE";
      case 0x6C: return "YELLOW";
      case 0x89: return "MEDIA_SELECT_TV";
      case 0x8D: return "PROGRAM_GUIDE";
      case 0x9C: return "CHANNEL_UP";
      case 0x9D: return "CHANNEL_DOWN";
      case 0xB0: return "PLAY";
      case 0xB1: return "PAUSE";
      case 0xB2: return "RECORD";
      case 0xB3: return "FAST_FORWARD";
      case 0xB4: return "REWIND";
      case 0xB5: return "NEXT_TRACK";
      case 0xB6: return "PREVIOUS_TRACK";
      case 0xB7: return "STOP";
      case 0xCD: return "PLAY_PAUSE";
      case 0xCF: return "VOICE_COMMAND";
      case 0xE2: return "MUTE";
      case 0xE9: return "VOLUME_UP";
      case 0xEA: return "VOLUME_DOWN";
      case 0x221: return "SEARCH";
      case 0x223: return "HOME";
      case 0x224: return "BACK";
      case 0x225: return "FORWARD";
      default: return nullptr;
    }
  }
  if (page == 0x01) {  // generic desktop
    switch (id) {
      case 0x81: return "SYSTEM_POWER";
      case 0x82: return "SYSTEM_SLEEP";
      case 0x83: return "SYSTEM_WAKE";
      default: return nullptr;
    }
  }
  return nullptr;
}

}  // namespace bt_classic_hid
}  // namespace esphome
