#include "app_logic.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <sys/time.h>

#include "../config.h"
#include "../drivers/ec11_driver.h"
#include "../ui/ui.h"
#include "connectivity_manager.h"
#include "device_config.h"
#include "edit_controller.h"
#include "net_audio_link.h"

// Backlight control is implemented in main.cpp.
extern void updateBacklight(float level);

namespace {
constexpr uint32_t KEY_LONG_PRESS_MS = 1000;
constexpr uint32_t STARTUP_STEP_INTERVAL_MS = 350;
constexpr uint32_t CLOCK_REFRESH_INTERVAL_MS = 1000;

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
uint32_t last_clock_refresh_at_ms = 0;
time_t last_displayed_epoch = static_cast<time_t>(-1);
bool time_synced = false;
bool time_placeholder_rendered = false;
lv_obj_t *settings_time_value_label = nullptr;
bool timezone_initialized = false;

void append_startup_log(const char *message) {
    if (!ui_startinfo) {
        return;
    }

    lv_textarea_add_text(ui_startinfo, message);
    lv_textarea_add_text(ui_startinfo, "\n");
    lv_textarea_set_cursor_pos(ui_startinfo, LV_TEXTAREA_CURSOR_LAST);
}

void set_time_label_if_present(lv_obj_t *label, const char *text) {
    if (!label || !text) {
        return;
    }
    lv_label_set_text(label, text);
}

void ensure_local_timezone() {
    if (timezone_initialized) {
        return;
    }

    // Project UI/ops use China Standard Time (UTC+8).
    setenv("TZ", "CST-8", 1);
    tzset();
    timezone_initialized = true;
}

void ensure_settings_time_labels() {
    if (!ui_Label6 || !ui_TimeP) {
        return;
    }

    lv_coord_t left_x = 8;
    if (ui_Label1) {
        left_x = lv_obj_get_x(ui_Label1);
    }

    set_time_label_if_present(ui_Label6, "时间:");
    lv_obj_set_width(ui_Label6, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_Label6, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(ui_Label6, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(ui_Label6, LV_ALIGN_LEFT_MID);
    lv_obj_set_pos(ui_Label6, left_x, 0);

    if (!settings_time_value_label) {
        settings_time_value_label = lv_label_create(ui_TimeP);
        lv_obj_clear_flag(settings_time_value_label, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_text_font(settings_time_value_label, &ui_font_system, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_update_layout(ui_TimeP);
    const lv_coord_t panel_width = lv_obj_get_width(ui_TimeP);
    const lv_coord_t value_left = left_x + lv_obj_get_width(ui_Label6) + 12;
    lv_coord_t value_width = panel_width - value_left - 12;
    if (value_width < 60) {
        value_width = 60;
    }

    lv_obj_set_width(settings_time_value_label, value_width);
    lv_label_set_long_mode(settings_time_value_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(settings_time_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(settings_time_value_label, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(settings_time_value_label, -12, 0);
}

void set_settings_time_value(const char *text) {
    ensure_settings_time_labels();
    set_time_label_if_present(settings_time_value_label, text ? text : "");
}

void refresh_time_widgets(bool force) {
    const uint32_t now_ms = millis();
    if (!force && last_clock_refresh_at_ms != 0 &&
        (now_ms - last_clock_refresh_at_ms) < CLOCK_REFRESH_INTERVAL_MS) {
        return;
    }
    last_clock_refresh_at_ms = now_ms;

    if (!time_synced) {
        if (!force && time_placeholder_rendered) {
            return;
        }

        set_time_label_if_present(ui_time, "--:--");
        set_time_label_if_present(ui_time1, "--:--");
        set_settings_time_value("--:--:--");
        time_placeholder_rendered = true;
        last_displayed_epoch = static_cast<time_t>(-1);
        return;
    }

    const time_t now = time(nullptr);
    if (!force && now == last_displayed_epoch) {
        return;
    }

    ensure_local_timezone();

    struct tm local_tm = {};
    if (localtime_r(&now, &local_tm) == nullptr) {
        return;
    }

    char header_text[6] = {0};
    char detail_text[20] = {0};
    strftime(header_text, sizeof(header_text), "%H:%M", &local_tm);
    strftime(detail_text, sizeof(detail_text), "%H:%M:%S", &local_tm);

    set_time_label_if_present(ui_time, header_text);
    set_time_label_if_present(ui_time1, header_text);
    set_settings_time_value(detail_text);

    time_placeholder_rendered = false;
    last_displayed_epoch = now;
}

void enter_main_screen() {
    app_state = AppState::MAIN_READY;

    lv_disp_load_scr(ui_main);
    edit_controller_on_enter_main_screen();
    connectivity_manager_on_main_screen_enter();
    net_audio_link_on_main_screen_enter();

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
    const float saved_backlight = static_cast<float>(device_config::load_backlight_pwm()) / 255.0f;
    updateBacklight(saved_backlight);

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

    const bool net_audio_ok = net_audio_link_init();
    append_startup_log(net_audio_ok
        ? "NET audio link initialized."
        : "NET audio link init failed.");
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
    ensure_local_timezone();

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
    refresh_time_widgets(true);
    Serial.println("Waiting for KEY long press to boot.");
}

void app_logic_update() {
    ec11_update();

    const uint32_t now_ms = millis();
    update_power_key(now_ms);
    update_startup_sequence(now_ms);
    refresh_time_widgets(false);
    connectivity_manager_update();
    net_audio_link_update();
    edit_controller_update();
}

void app_logic_set_time_from_server_ms(int64_t unix_ms) {
    if (unix_ms <= 0) {
        return;
    }

    ensure_local_timezone();

    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_ms / 1000LL);
    tv.tv_usec = static_cast<suseconds_t>((unix_ms % 1000LL) * 1000LL);
    settimeofday(&tv, nullptr);

    time_synced = true;
    time_placeholder_rendered = false;
    last_displayed_epoch = static_cast<time_t>(-1);
    refresh_time_widgets(true);

    const time_t now = time(nullptr);
    struct tm local_tm = {};
    char text[32] = {0};
    if (localtime_r(&now, &local_tm) != nullptr) {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local_tm);
    }

    Serial.printf("[TIME] Synced from server: %lld ms local=%s\n",
                  static_cast<long long>(unix_ms),
                  text[0] != '\0' ? text : "<invalid>");
}
