#ifndef EC11_DRIVER_H
#define EC11_DRIVER_H

#include <Arduino.h>
#include <functional>

enum class EC11Direction {
    NONE = 0,
    CLOCKWISE,
    COUNTER_CLOCKWISE
};

enum class EC11Event {
    NONE = 0,
    ROTATE_CW,
    ROTATE_CCW,
    BUTTON_PRESS,
    BUTTON_RELEASE,
    BUTTON_CLICK,
    BUTTON_LONG_PRESS
};

using EC11Callback = std::function<void(EC11Event event, int32_t value)>;

bool ec11_init(void);
void ec11_deinit(void);
void ec11_set_callback(EC11Callback callback);
int32_t ec11_get_counter(void);
void ec11_reset_counter(int32_t value = 0);
bool ec11_get_button_state(void);
EC11Direction ec11_get_direction(void);
void ec11_set_acceleration(bool enable);
void ec11_set_debounce(uint32_t debounce_ms);
void ec11_set_long_press_threshold(uint32_t threshold_ms);
void ec11_update(void);

#endif // EC11_DRIVER_H
