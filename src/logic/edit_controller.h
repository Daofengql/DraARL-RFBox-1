#ifndef EDIT_CONTROLLER_H
#define EDIT_CONTROLLER_H

#include <cstdint>

#include "../drivers/ec11_driver.h"

void edit_controller_init();
void edit_controller_on_enter_main_screen();
void edit_controller_on_encoder_event(EC11Event event, int32_t value);
void edit_controller_on_key0_short_press();

#endif // EDIT_CONTROLLER_H
