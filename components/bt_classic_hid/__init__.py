# SPDX-License-Identifier: GPL-3.0-or-later
"""Bluetooth Classic HID host for the original ESP32.

Pairs with one Bluetooth Classic keyboard / remote control and reports its
key presses. ESPHome's own Bluetooth components only speak BLE, so this
component brings up the Bluetooth stack itself and cannot be combined with
them.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32,
    add_idf_sdkconfig_option,
    only_on_variant,
    request_bluetooth,
    request_software_coexistence,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAC_ADDRESS, CONF_NAME

CODEOWNERS = []
DEPENDENCIES = ["esp32"]
CONFLICTS_WITH = ["esp32_ble", "esp32_ble_tracker", "ble_client"]

CONF_NAME_FILTER = "name_filter"
CONF_AUTO_PAIRING = "auto_pairing"
CONF_ON_KEY = "on_key"
CONF_ON_CONNECT = "on_connect"
CONF_ON_DISCONNECT = "on_disconnect"

bt_classic_hid_ns = cg.esphome_ns.namespace("bt_classic_hid")
BTClassicHID = bt_classic_hid_ns.class_("BTClassicHID", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BTClassicHID),
            # Bluetooth name this host shows to the remote
            cv.Optional(CONF_NAME): cv.All(cv.string, cv.Length(min=1, max=31)),
            # Pair only with devices whose name starts with this text ...
            cv.Optional(CONF_NAME_FILTER): cv.string,
            # ... or with exactly this address
            cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
            # Search for the remote at boot while nothing is paired yet
            cv.Optional(CONF_AUTO_PAIRING, default=True): cv.boolean,
            cv.Optional(CONF_ON_KEY): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_CONNECT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(
                single=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # Bluetooth Classic exists only on the original ESP32
    only_on_variant(supported=[VARIANT_ESP32], msg_prefix="Bluetooth Classic"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_NAME in config:
        cg.add(var.set_device_name(config[CONF_NAME]))
    if CONF_NAME_FILTER in config:
        cg.add(var.set_name_filter(config[CONF_NAME_FILTER]))
    if CONF_MAC_ADDRESS in config:
        cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_auto_pairing(config[CONF_AUTO_PAIRING]))

    if CONF_ON_KEY in config:
        await automation.build_automation(
            var.get_key_trigger(),
            [(cg.std_string, "key"), (cg.std_string, "code"), (bool, "pressed")],
            config[CONF_ON_KEY],
        )
    if CONF_ON_CONNECT in config:
        await automation.build_automation(
            var.get_connect_trigger(), [], config[CONF_ON_CONNECT]
        )
    if CONF_ON_DISCONNECT in config:
        await automation.build_automation(
            var.get_disconnect_trigger(), [], config[CONF_ON_DISCONNECT]
        )

    # Bluetooth stack: Classic only, with the HID host profile
    request_bluetooth()
    request_software_coexistence()
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_HID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_HID_HOST_ENABLED", True)
