#include "ec11_driver.h"

#include "../config.h"

namespace {
EC11Callback event_callback = nullptr;

int32_t counter = 0;
EC11Direction last_direction = EC11Direction::NONE;

bool button_pressed = false;
bool button_last_raw_state = false;
bool button_stable_state = false;
uint32_t button_pressed_at_ms = 0;
uint32_t button_last_change_at_ms = 0;
uint32_t long_press_threshold_ms = 800;
uint32_t button_debounce_ms = 25;

bool acceleration_enabled = false;
uint32_t last_rotate_time = 0;
uint32_t last_rotation_emit_at_ms = 0;
int rotate_speed = 1;

uint8_t last_encoder_state = 0;
int8_t encoder_accumulator = 0;
uint32_t rotation_guard_ms = 12;

constexpr uint8_t EVENT_QUEUE_CAPACITY = 32;
struct QueuedEvent {
    EC11Event event = EC11Event::NONE;
    int32_t value = 0;
};

QueuedEvent event_queue[EVENT_QUEUE_CAPACITY];
uint8_t event_queue_head = 0;
uint8_t event_queue_tail = 0;
uint8_t event_queue_size = 0;

// Valid quadrature state transitions. Using the same table shape as the previous
// interrupt-driven library lets us keep the existing CW/CCW behavior.
constexpr int8_t ENCODER_TRANSITIONS[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0
};

uint8_t read_encoder_state() {
    return static_cast<uint8_t>(((digitalRead(EC11_CLK) == HIGH) ? 0x02U : 0x00U) |
                                ((digitalRead(EC11_DT) == HIGH) ? 0x01U : 0x00U));
}

void reset_event_queue() {
    event_queue_head = 0;
    event_queue_tail = 0;
    event_queue_size = 0;
}

void queue_event(EC11Event event, int32_t value) {
    if (event == EC11Event::NONE) {
        return;
    }

    // Keep newest events when queue is full to avoid UI lock-up on fast rotation.
    if (event_queue_size >= EVENT_QUEUE_CAPACITY) {
        event_queue_tail = static_cast<uint8_t>((event_queue_tail + 1U) % EVENT_QUEUE_CAPACITY);
        --event_queue_size;
    }

    event_queue[event_queue_head].event = event;
    event_queue[event_queue_head].value = value;
    event_queue_head = static_cast<uint8_t>((event_queue_head + 1U) % EVENT_QUEUE_CAPACITY);
    ++event_queue_size;
}

bool pop_event(QueuedEvent &out) {
    if (event_queue_size == 0) {
        return false;
    }

    out = event_queue[event_queue_tail];
    event_queue_tail = static_cast<uint8_t>((event_queue_tail + 1U) % EVENT_QUEUE_CAPACITY);
    --event_queue_size;
    return true;
}

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

void apply_rotation_step(int32_t detents) {
    if (detents == 0) {
        return;
    }

    const uint32_t now = millis();
    if (last_rotation_emit_at_ms != 0 &&
        (now - last_rotation_emit_at_ms) < rotation_guard_ms) {
        return;
    }

    last_rotation_emit_at_ms = now;
    rotate_speed = get_rotate_speed();
    const int32_t steps = (detents > 0) ? detents : -detents;
    const int32_t delta = steps * rotate_speed;

    if (detents > 0) {
        last_direction = EC11Direction::COUNTER_CLOCKWISE;
        counter += delta;
        queue_event(EC11Event::ROTATE_CCW, delta);
    } else {
        last_direction = EC11Direction::CLOCKWISE;
        counter -= delta;
        queue_event(EC11Event::ROTATE_CW, delta);
    }
}
} // namespace

bool ec11_init(void) {
    ec11_deinit();

    pinMode(EC11_CLK, INPUT_PULLUP);
    pinMode(EC11_DT, INPUT_PULLUP);
    pinMode(EC11_SW, INPUT_PULLUP);

    counter = 0;
    last_direction = EC11Direction::NONE;
    button_pressed = false;
    button_last_raw_state = (digitalRead(EC11_SW) == LOW);
    button_stable_state = button_last_raw_state;
    button_pressed_at_ms = 0;
    button_last_change_at_ms = millis();
    rotate_speed = 1;
    last_rotate_time = 0;
    last_rotation_emit_at_ms = 0;
    last_encoder_state = read_encoder_state();
    encoder_accumulator = 0;
    reset_event_queue();

    return true;
}

void ec11_deinit(void) {
    event_callback = nullptr;
    button_pressed = false;
    button_last_raw_state = false;
    button_stable_state = false;
    button_pressed_at_ms = 0;
    button_last_change_at_ms = 0;
    last_direction = EC11Direction::NONE;
    counter = 0;
    last_rotate_time = 0;
    last_rotation_emit_at_ms = 0;
    rotate_speed = 1;
    last_encoder_state = 0;
    encoder_accumulator = 0;
    reset_event_queue();
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
    debounce_ms = (debounce_ms == 0U) ? 1U : debounce_ms;
    button_debounce_ms = debounce_ms;
}

void ec11_set_long_press_threshold(uint32_t threshold_ms) {
    long_press_threshold_ms = threshold_ms;
}

void ec11_update(void) {
    const uint32_t now = millis();

    const uint8_t current_encoder_state = read_encoder_state();
    if (current_encoder_state != last_encoder_state) {
        const uint8_t transition = static_cast<uint8_t>(((last_encoder_state & 0x03U) << 2U) |
                                                        (current_encoder_state & 0x03U));
        encoder_accumulator = static_cast<int8_t>(encoder_accumulator + ENCODER_TRANSITIONS[transition & 0x0FU]);
        last_encoder_state = current_encoder_state;

        if (encoder_accumulator >= 4) {
            apply_rotation_step(1);
            encoder_accumulator = 0;
        } else if (encoder_accumulator <= -4) {
            apply_rotation_step(-1);
            encoder_accumulator = 0;
        }
    }

    const bool current_button_raw = (digitalRead(EC11_SW) == LOW);
    if (current_button_raw != button_last_raw_state) {
        button_last_raw_state = current_button_raw;
        button_last_change_at_ms = now;
    }

    if (current_button_raw != button_stable_state &&
        (now - button_last_change_at_ms) >= button_debounce_ms) {
        button_stable_state = current_button_raw;

        if (button_stable_state) {
            button_pressed = true;
            button_pressed_at_ms = now;
            queue_event(EC11Event::BUTTON_PRESS, 0);
        } else {
            button_pressed = false;
            const uint32_t duration = now - button_pressed_at_ms;

            queue_event(EC11Event::BUTTON_RELEASE, static_cast<int32_t>(duration));
            if (duration >= long_press_threshold_ms) {
                queue_event(EC11Event::BUTTON_LONG_PRESS, static_cast<int32_t>(duration));
            } else {
                queue_event(EC11Event::BUTTON_CLICK, static_cast<int32_t>(duration));
            }
        }
    }

    QueuedEvent event = {};
    while (pop_event(event)) {
        emit_event(event.event, event.value);
    }
}
