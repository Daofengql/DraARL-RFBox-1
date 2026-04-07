#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <cstdint>

#include "../drivers/ec11_driver.h"

void connectivity_manager_init();
void connectivity_manager_on_main_screen_enter();
void connectivity_manager_update();
bool connectivity_manager_handle_encoder_event(EC11Event event, int32_t value);

#endif // CONNECTIVITY_MANAGER_H
