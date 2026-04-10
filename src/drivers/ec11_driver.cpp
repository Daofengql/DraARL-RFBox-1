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

constexpr uint8_t EVENT_QUEUE_CAPACITY = 32;
struct QueuedEvent {
    EC11Event event = EC11Event::NONE;
    int32_t value = 0;
};

QueuedEvent event_queue[EVENT_QUEUE_CAPACITY];
uint8_t event_queue_head = 0;
uint8_t event_queue_tail = 0;
uint8_t event_queue_size = 0;
portMUX_TYPE event_queue_lock = portMUX_INITIALIZER_UNLOCKED;

void reset_event_queue() {
    portENTER_CRITICAL(&event_queue_lock);
    event_queue_head = 0;
    event_queue_tail = 0;
    event_queue_size = 0;
    portEXIT_CRITICAL(&event_queue_lock);
}

void queue_event(EC11Event event, int32_t value) {
    if (event == EC11Event::NONE) {
        return;
    }

    portENTER_CRITICAL(&event_queue_lock);

    // Keep newest events when queue is full to avoid UI lock-up on fast rotation.
    if (event_queue_size >= EVENT_QUEUE_CAPACITY) {
        event_queue_tail = static_cast<uint8_t>((event_queue_tail + 1U) % EVENT_QUEUE_CAPACITY);
        --event_queue_size;
    }

    event_queue[event_queue_head].event = event;
    event_queue[event_queue_head].value = value;
    event_queue_head = static_cast<uint8_t>((event_queue_head + 1U) % EVENT_QUEUE_CAPACITY);
    ++event_queue_size;

    portEXIT_CRITICAL(&event_queue_lock);
}

bool pop_event(QueuedEvent &out) {
    bool has_event = false;

    portENTER_CRITICAL(&event_queue_lock);
    if (event_queue_size > 0) {
        out = event_queue[event_queue_tail];
        event_queue_tail = static_cast<uint8_t>((event_queue_tail + 1U) % EVENT_QUEUE_CAPACITY);
        --event_queue_size;
        has_event = true;
    }
    portEXIT_CRITICAL(&event_queue_lock);

    return has_event;
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
        queue_event(EC11Event::ROTATE_CCW, delta);
    } else {
        last_direction = EC11Direction::CLOCKWISE;
        counter -= delta;
        queue_event(EC11Event::ROTATE_CW, delta);
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
    reset_event_queue();

    return true;
}

void ec11_deinit(void) {
    if (encoder) {
        delete encoder;
        encoder = nullptr;
    }

    event_callback = nullptr;
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
        queue_event(EC11Event::BUTTON_PRESS, 0);
    }

    if (!current_button_state && last_button_state) {
        button_pressed = false;
        const uint32_t duration = millis() - button_pressed_at_ms;

        queue_event(EC11Event::BUTTON_RELEASE, static_cast<int32_t>(duration));
        if (duration >= long_press_threshold_ms) {
            queue_event(EC11Event::BUTTON_LONG_PRESS, static_cast<int32_t>(duration));
        } else {
            queue_event(EC11Event::BUTTON_CLICK, static_cast<int32_t>(duration));
        }
    }

    last_button_state = current_button_state;

    QueuedEvent event = {};
    while (pop_event(event)) {
        emit_event(event.event, event.value);
    }
}
