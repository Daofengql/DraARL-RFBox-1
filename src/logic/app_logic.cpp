#include "app_logic.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "../config.h"
#include "../drivers/ec11_driver.h"
#include "../ui/ui.h"

namespace {
constexpr uint32_t KEY_LONG_PRESS_MS = 1000;
constexpr uint32_t STARTUP_STEP_INTERVAL_MS = 350;

struct StartupStep {
    const char *message;
    uint8_t progress;
};

constexpr StartupStep STARTUP_STEPS[] = {
    {"[10%] Display pipeline ready", 10},
    {"[25%] Audio codec initialized", 25},
    {"[40%] RF module handshake OK", 40},
    {"[55%] Loading local configuration", 55},
    {"[75%] Network service startup", 75},
    {"[90%] Runtime checks complete", 90},
    {"[100%] Boot complete", 100},
};

enum class AppState {
    POWER_WAIT = 0,
    STARTUP_LOADING,
    MAIN_READY,
};

AppState app_state = AppState::POWER_WAIT;

char frequency_text[16] = "438.5000";
uint8_t digit_positions[16] = {};
uint8_t digit_count = 0;
int8_t editing_digit_index = -1;

bool key_last_pressed = false;
bool key_long_triggered = false;
uint32_t key_pressed_at_ms = 0;

size_t startup_step_index = 0;
uint32_t startup_next_step_at_ms = 0;

bool is_editing_frequency() {
    return editing_digit_index >= 0 && editing_digit_index < static_cast<int8_t>(digit_count);
}

void rebuild_digit_positions() {
    digit_count = 0;

    const int length = static_cast<int>(strlen(frequency_text));
    for (int i = length - 1; i >= 0; --i) {
        if (std::isdigit(static_cast<unsigned char>(frequency_text[i]))) {
            digit_positions[digit_count++] = static_cast<uint8_t>(i);
        }
    }
}

void render_frequency_label() {
    if (!ui_Freq) {
        return;
    }

    if (!is_editing_frequency()) {
        lv_label_set_recolor(ui_Freq, false);
        lv_label_set_text(ui_Freq, frequency_text);
        return;
    }

    lv_label_set_recolor(ui_Freq, true);

    const uint8_t selected_pos = digit_positions[editing_digit_index];
    const size_t length = strlen(frequency_text);

    char rendered[64] = {0};
    size_t write_pos = 0;

    for (size_t i = 0; i < length; ++i) {
        if (i == selected_pos) {
            const int written = snprintf(
                rendered + write_pos,
                sizeof(rendered) - write_pos,
                "#00F2FF %c#",
                frequency_text[i]
            );

            if (written <= 0) {
                break;
            }

            write_pos += static_cast<size_t>(written);
            if (write_pos >= sizeof(rendered)) {
                rendered[sizeof(rendered) - 1] = '\0';
                break;
            }
        } else {
            if (write_pos + 1 >= sizeof(rendered)) {
                rendered[sizeof(rendered) - 1] = '\0';
                break;
            }

            rendered[write_pos++] = frequency_text[i];
            rendered[write_pos] = '\0';
        }
    }

    lv_label_set_text(ui_Freq, rendered);
}

void exit_frequency_edit() {
    editing_digit_index = -1;
    render_frequency_label();
}

void step_frequency_digit(int32_t delta) {
    if (!is_editing_frequency() || delta == 0) {
        return;
    }

    const uint8_t char_index = digit_positions[editing_digit_index];
    char &digit = frequency_text[char_index];

    if (editing_digit_index == 0) {
        if ((delta % 2) != 0) {
            digit = (digit == '5') ? '0' : '5';
        }
    } else if (editing_digit_index == static_cast<int8_t>(digit_count - 1)) {
        if ((delta % 2) != 0) {
            digit = (digit == '4') ? '1' : '4';
        }
    } else {
        int value = digit - '0';
        value = (value + static_cast<int>(delta)) % 10;
        if (value < 0) {
            value += 10;
        }
        digit = static_cast<char>('0' + value);
    }

    render_frequency_label();
}

void handle_encoder_click() {
    if (digit_count == 0) {
        return;
    }

    if (!is_editing_frequency()) {
        editing_digit_index = 0;
        render_frequency_label();
        return;
    }

    if (editing_digit_index < static_cast<int8_t>(digit_count - 1)) {
        ++editing_digit_index;
        render_frequency_label();
        return;
    }

    // At the highest digit, next click exits edit mode instead of looping.
    exit_frequency_edit();
}

void append_startup_log(const char *message) {
    if (!ui_startinfo) {
        return;
    }

    lv_textarea_add_text(ui_startinfo, message);
    lv_textarea_add_text(ui_startinfo, "\n");
    lv_textarea_set_cursor_pos(ui_startinfo, LV_TEXTAREA_CURSOR_LAST);
}

void enter_main_screen() {
    app_state = AppState::MAIN_READY;

    lv_disp_load_scr(ui_main);
    rebuild_digit_positions();
    exit_frequency_edit();

    Serial.println("Boot sequence finished. Main screen loaded.");
}

void prepare_startup_screen() {
    lv_disp_load_scr(ui_StartUP);

    if (ui_startinfo) {
        lv_textarea_set_text(ui_startinfo, "");
        lv_textarea_set_placeholder_text(ui_startinfo, "");
        lv_textarea_set_cursor_click_pos(ui_startinfo, false);
        lv_textarea_set_text_selection(ui_startinfo, false);
        lv_obj_clear_flag(ui_startinfo, LV_OBJ_FLAG_CLICKABLE);
    }

    if (ui_processstat) {
        lv_bar_set_range(ui_processstat, 0, 100);
        lv_bar_set_value(ui_processstat, 0, LV_ANIM_OFF);
    }

    append_startup_log("Power key accepted, booting...");
}

void start_boot_sequence(uint32_t now_ms) {
    if (app_state != AppState::POWER_WAIT) {
        return;
    }

    app_state = AppState::STARTUP_LOADING;
    startup_step_index = 0;
    startup_next_step_at_ms = now_ms;

    prepare_startup_screen();
    Serial.println("Boot sequence started.");
}

void update_startup_sequence(uint32_t now_ms) {
    if (app_state != AppState::STARTUP_LOADING) {
        return;
    }

    if (now_ms < startup_next_step_at_ms) {
        return;
    }

    if (startup_step_index < (sizeof(STARTUP_STEPS) / sizeof(STARTUP_STEPS[0]))) {
        const StartupStep &step = STARTUP_STEPS[startup_step_index++];

        append_startup_log(step.message);
        if (ui_processstat) {
            lv_bar_set_value(ui_processstat, step.progress, LV_ANIM_OFF);
        }

        startup_next_step_at_ms = now_ms + STARTUP_STEP_INTERVAL_MS;
        return;
    }

    enter_main_screen();
}

void update_power_key(uint32_t now_ms) {
    const bool pressed = (digitalRead(KEY0_PIN) == LOW);

    if (pressed && !key_last_pressed) {
        key_pressed_at_ms = now_ms;
        key_long_triggered = false;
    }

    if (!pressed && key_last_pressed) {
        key_long_triggered = false;
    }

    if (app_state == AppState::POWER_WAIT && pressed && !key_long_triggered) {
        if ((now_ms - key_pressed_at_ms) >= KEY_LONG_PRESS_MS) {
            key_long_triggered = true;
            start_boot_sequence(now_ms);
        }
    }

    key_last_pressed = pressed;
}

void handle_encoder_event(EC11Event event, int32_t value) {
    if (app_state != AppState::MAIN_READY) {
        return;
    }

    switch (event) {
        case EC11Event::ROTATE_CW: {
            const int32_t step = (value == 0) ? 1 : value;
            step_frequency_digit(step);
            break;
        }
        case EC11Event::ROTATE_CCW: {
            const int32_t step = (value == 0) ? 1 : value;
            step_frequency_digit(-step);
            break;
        }
        case EC11Event::BUTTON_CLICK:
            handle_encoder_click();
            break;
        case EC11Event::BUTTON_LONG_PRESS:
            if (is_editing_frequency()) {
                exit_frequency_edit();
            }
            break;
        default:
            break;
    }
}
} // namespace

void app_logic_init() {
    pinMode(KEY0_PIN, INPUT_PULLUP);

    if (!ec11_init()) {
        Serial.println("EC11 init failed.");
    }
    ec11_set_acceleration(false);
    ec11_set_long_press_threshold(900);
    ec11_set_callback(handle_encoder_event);

    rebuild_digit_positions();

    if (ui____initial_actions0) {
        lv_obj_set_style_bg_color(ui____initial_actions0, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui____initial_actions0, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_disp_load_scr(ui____initial_actions0);
    }

    app_state = AppState::POWER_WAIT;
    Serial.println("Waiting for KEY long press to boot.");
}

void app_logic_update() {
    ec11_update();

    const uint32_t now_ms = millis();
    update_power_key(now_ms);
    update_startup_sequence(now_ms);
}
