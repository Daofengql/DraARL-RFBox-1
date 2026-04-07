#include "app_logic.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cstdint>

#include "../config.h"
#include "../drivers/ec11_driver.h"
#include "../ui/ui.h"
#include "connectivity_manager.h"
#include "edit_controller.h"

// Backlight control is implemented in main.cpp.
extern void updateBacklight(float level);

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

bool key_last_pressed = false;
bool key_long_triggered = false;
uint32_t key_pressed_at_ms = 0;

size_t startup_step_index = 0;
uint32_t startup_next_step_at_ms = 0;

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
    edit_controller_on_enter_main_screen();
    connectivity_manager_on_main_screen_enter();

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

    // Turn on backlight only when startup begins.
    updateBacklight(1.0f);

    app_state = AppState::STARTUP_LOADING;
    startup_step_index = 0;
    startup_next_step_at_ms = now_ms;

    prepare_startup_screen();
    const bool radio_ok = edit_controller_boot_radio_init();
    if (radio_ok) {
        append_startup_log("SA818 initialized and config synced.");
    } else {
        append_startup_log("SA818 init/config failed.");
    }
    connectivity_manager_init();
    append_startup_log("WiFi subsystem initialized.");
    Serial.println("Boot sequence started.");
}

void on_encoder_event(EC11Event event, int32_t value) {
    if (connectivity_manager_handle_encoder_event(event, value)) {
        return;
    }

    edit_controller_on_encoder_event(event, value);
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
        const uint32_t press_duration_ms = now_ms - key_pressed_at_ms;
        if (app_state == AppState::MAIN_READY && press_duration_ms < KEY_LONG_PRESS_MS) {
            edit_controller_on_key0_short_press();
        }
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
} // namespace

void app_logic_init() {
    pinMode(KEY0_PIN, INPUT_PULLUP);

    if (!ec11_init()) {
        Serial.println("EC11 init failed.");
    }
    ec11_set_acceleration(false);
    ec11_set_long_press_threshold(900);
    ec11_set_callback(on_encoder_event);

    edit_controller_init();

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
    connectivity_manager_update();
    edit_controller_update();
}
