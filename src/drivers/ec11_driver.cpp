#include "ec11_driver.h"

#include "../config.h"

namespace {
RotaryEncoder *encoder = nullptr;
EC11Callback event_callback = nullptr;

int32_t counter = 0;
int32_t last_encoder_value = 0;
EC11Direction last_direction = EC11Direction::NONE;

bool button_pressed = false;
bool last_button_state = false;
uint32_t button_pressed_at_ms = 0;
uint32_t long_press_threshold_ms = 800;

bool acceleration_enabled = false;
uint32_t last_rotate_time = 0;
int rotate_speed = 1;

void emit_event(EC11Event event, int32_t value) {
    if (event_callback) {
        event_callback(event, value);
    }
}

int get_rotate_speed() {
    if (!acceleration_enabled) {
        return 1;
    }

    const uint32_t now = millis();
    const uint32_t time_diff = now - last_rotate_time;
    last_rotate_time = now;

    if (time_diff < 50) {
        return 5;
    }
    if (time_diff < 100) {
        return 3;
    }
    if (time_diff < 200) {
        return 2;
    }
    return 1;
}

void on_knob_turned(long value) {
    const int32_t diff = static_cast<int32_t>(value) - last_encoder_value;
    if (diff == 0) {
        return;
    }

    rotate_speed = get_rotate_speed();
    const int32_t steps = (diff > 0) ? diff : -diff;
    const int32_t delta = steps * rotate_speed;

    if (diff > 0) {
        last_direction = EC11Direction::COUNTER_CLOCKWISE;
        counter += delta;
        emit_event(EC11Event::ROTATE_CCW, delta);
    } else {
        last_direction = EC11Direction::CLOCKWISE;
        counter -= delta;
        emit_event(EC11Event::ROTATE_CW, delta);
    }

    last_encoder_value = static_cast<int32_t>(value);
}

void on_button_pressed(unsigned long duration) {
    (void)duration;
}
} // namespace

bool ec11_init(void) {
    ec11_deinit();

    encoder = new RotaryEncoder(EC11_CLK, EC11_DT, EC11_SW);
    if (!encoder) {
        return false;
    }

    encoder->setEncoderType(EncoderType::HAS_PULLUP);
    encoder->setBoundaries(-1000000, 1000000, false);
    encoder->onTurned(&on_knob_turned);
    encoder->onPressed(&on_button_pressed);
    encoder->begin();

    pinMode(EC11_SW, INPUT_PULLUP);

    counter = 0;
    last_encoder_value = 0;
    last_direction = EC11Direction::NONE;
    button_pressed = false;
    last_button_state = false;
    button_pressed_at_ms = 0;
    rotate_speed = 1;
    last_rotate_time = 0;

    return true;
}

void ec11_deinit(void) {
    if (encoder) {
        delete encoder;
        encoder = nullptr;
    }

    event_callback = nullptr;
}

void ec11_set_callback(EC11Callback callback) {
    event_callback = callback;
}

int32_t ec11_get_counter(void) {
    return counter;
}

void ec11_reset_counter(int32_t value) {
    counter = value;
}

bool ec11_get_button_state(void) {
    return button_pressed;
}

EC11Direction ec11_get_direction(void) {
    return last_direction;
}

void ec11_set_acceleration(bool enable) {
    acceleration_enabled = enable;
}

void ec11_set_debounce(uint32_t debounce_ms) {
    (void)debounce_ms;
}

void ec11_set_long_press_threshold(uint32_t threshold_ms) {
    long_press_threshold_ms = threshold_ms;
}

void ec11_update(void) {
    const bool current_button_state = (digitalRead(EC11_SW) == LOW);

    if (current_button_state && !last_button_state) {
        button_pressed = true;
        button_pressed_at_ms = millis();
        emit_event(EC11Event::BUTTON_PRESS, 0);
    }

    if (!current_button_state && last_button_state) {
        button_pressed = false;
        const uint32_t duration = millis() - button_pressed_at_ms;

        emit_event(EC11Event::BUTTON_RELEASE, static_cast<int32_t>(duration));
        if (duration >= long_press_threshold_ms) {
            emit_event(EC11Event::BUTTON_LONG_PRESS, static_cast<int32_t>(duration));
        } else {
            emit_event(EC11Event::BUTTON_CLICK, static_cast<int32_t>(duration));
        }
    }

    last_button_state = current_button_state;
}
