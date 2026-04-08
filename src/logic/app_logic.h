#ifndef APP_LOGIC_H
#define APP_LOGIC_H

#include <cstdint>

void app_logic_init();
void app_logic_update();
void app_logic_set_time_from_server_ms(int64_t unix_ms);

#endif // APP_LOGIC_H
