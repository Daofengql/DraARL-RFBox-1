#ifndef EDIT_CONTROLLER_H
#define EDIT_CONTROLLER_H

#include <cstdint>

#include "../drivers/ec11_driver.h"
#include "device_config.h"

void edit_controller_init();
void edit_controller_on_enter_main_screen();
bool edit_controller_boot_radio_init();
void edit_controller_update();
void edit_controller_on_encoder_event(EC11Event event, int32_t value);
void edit_controller_on_key0_short_press();
void edit_controller_on_key0_long_press();
void edit_controller_get_radio_config(device_config::RadioConfig &config);
bool edit_controller_set_radio_config(const device_config::RadioConfig &config, bool persist);
void edit_controller_set_network_bridge_active(bool active);
void edit_controller_set_network_bridge_source(const char *call_sign, uint8_t ssid);
void edit_controller_set_rf_overload_active(bool active);
void edit_controller_hide_power_popup();
bool edit_controller_is_power_popup_visible();

#endif // EDIT_CONTROLLER_H
