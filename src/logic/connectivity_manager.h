#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <cstdint>

#include "../drivers/ec11_driver.h"

void connectivity_manager_init();
void connectivity_manager_on_main_screen_enter();
void connectivity_manager_update();
bool connectivity_manager_handle_encoder_event(EC11Event event, int32_t value);
bool connectivity_manager_set_ble_enabled(bool enable, bool show_popup);
bool connectivity_manager_is_ble_enabled();
const char *connectivity_manager_get_ble_auth_code();
const char *connectivity_manager_get_ble_device_name();

#endif // CONNECTIVITY_MANAGER_H
