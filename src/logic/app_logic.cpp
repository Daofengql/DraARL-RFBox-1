#include "app_logic.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <lvgl.h>

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../config.h"
#include "../drivers/sa818_driver.h"
#include "../drivers/ec11_driver.h"
#include "../ui/ui.h"
#include "connectivity_manager.h"
#include "device_config.h"
#include "edit_controller.h"
#include "http_request_gate.h"
#include "net_audio_link.h"
#include "ota_update.h"
#include "version_utils.h"

// Backlight control is implemented in main.cpp.
extern void updateBacklight(float level);

namespace {
constexpr uint32_t KEY_LONG_PRESS_MS = 1000;
constexpr uint32_t STARTUP_STEP_INTERVAL_MS = 350;
constexpr uint32_t CLOCK_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t INPUT_RESUME_AFTER_PTT_MS = 500;
constexpr uint32_t OTA_CHECK_INTERVAL_MS = 3600000;  // 1小时检查一次
constexpr size_t STARTUP_STEP_COUNT = 6;

enum class AppState {
    POWER_WAIT = 0,
    STARTUP_LOADING,
    MAIN_READY,
};

enum class LocalInputGateState {
    READY = 0,
    TX_PREPARE,
    TX_ACTIVE,
    TX_RELEASE_GUARD,
};

AppState app_state = AppState::POWER_WAIT;
LocalInputGateState local_input_gate_state = LocalInputGateState::READY;

bool key_last_pressed = false;
bool key_long_triggered = false;
uint32_t key_pressed_at_ms = 0;
bool auto_start_enabled = false;

size_t startup_step_index = 0;
uint32_t startup_next_step_at_ms = 0;
uint32_t last_clock_refresh_at_ms = 0;
time_t last_displayed_epoch = static_cast<time_t>(-1);
bool time_synced = false;
bool time_placeholder_rendered = false;
bool timezone_initialized = false;
uint32_t local_input_resume_at_ms = 0;
uint32_t last_ota_check_at_ms = 0;
bool ota_check_done = false;
bool ota_check_in_progress = false;
device_config::OTAConfig ota_config_cache = {};
TaskHandle_t ota_check_task_handle = nullptr;
portMUX_TYPE ota_check_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool ota_check_result_ready = false;
volatile bool ota_check_result_success = false;
ota_update::FirmwareInfo ota_check_result_info = {};
bool ota_waiting_for_wifi_logged = false;
bool ota_waiting_for_initial_delay_logged = false;
bool ota_auto_check_disabled_logged = false;
bool ota_check_in_progress_logged = false;

void reconcile_pending_ota_state_on_boot(device_config::OTAConfig &config) {
    if (!config.has_pending_update || config.available_version[0] == '\0') {
        return;
    }

    const int compare_result = version_utils::compare(FIRMWARE_VERSION, config.available_version);
    Serial.printf("[OTA] Boot reconcile: current=%s pending=%s compare=%d\n",
                  FIRMWARE_VERSION,
                  config.available_version,
                  compare_result);

    if (compare_result < 0) {
        return;
    }

    config.has_pending_update = false;
    config.available_version[0] = '\0';
    config.download_url[0] = '\0';
    config.file_hash[0] = '\0';
    config.file_hash_algorithm[0] = '\0';
    config.file_size = 0;

    if (!device_config::save_ota(config)) {
        Serial.println("[OTA] Failed to persist boot-reconciled OTA state.");
        return;
    }

    Serial.println("[OTA] Cleared pending OTA state because current firmware is already up to date.");
}

void clear_cached_pending_ota_update() {
    ota_config_cache.has_pending_update = false;
    ota_config_cache.available_version[0] = '\0';
    ota_config_cache.download_url[0] = '\0';
    ota_config_cache.file_hash[0] = '\0';
    ota_config_cache.file_hash_algorithm[0] = '\0';
    ota_config_cache.file_size = 0;
}

void cache_pending_ota_update(const ota_update::FirmwareInfo &info, uint32_t now_ms) {
    ota_config_cache.has_pending_update = info.has_update;
    ota_config_cache.last_check_time = now_ms / 1000U;
    strncpy(ota_config_cache.available_version, info.version, sizeof(ota_config_cache.available_version) - 1);
    ota_config_cache.available_version[sizeof(ota_config_cache.available_version) - 1] = '\0';
    strncpy(ota_config_cache.download_url, info.download_url, sizeof(ota_config_cache.download_url) - 1);
    ota_config_cache.download_url[sizeof(ota_config_cache.download_url) - 1] = '\0';
    strncpy(ota_config_cache.file_hash, info.file_hash, sizeof(ota_config_cache.file_hash) - 1);
    ota_config_cache.file_hash[sizeof(ota_config_cache.file_hash) - 1] = '\0';
    strncpy(ota_config_cache.file_hash_algorithm,
            info.file_hash_algorithm,
            sizeof(ota_config_cache.file_hash_algorithm) - 1);
    ota_config_cache.file_hash_algorithm[sizeof(ota_config_cache.file_hash_algorithm) - 1] = '\0';
    ota_config_cache.file_size = info.file_size;
}

void ota_check_task_entry(void *param) {
    (void)param;

    ota_update::FirmwareInfo info = {};
    const bool success = ota_update::check_for_update(info);

    portENTER_CRITICAL(&ota_check_lock);
    ota_check_result_info = info;
    ota_check_result_success = success;
    ota_check_result_ready = true;
    ota_check_in_progress = false;
    ota_check_task_handle = nullptr;
    portEXIT_CRITICAL(&ota_check_lock);

    vTaskDelete(nullptr);
}

bool schedule_ota_check(uint32_t now_ms, const char *tag) {
    if (ota_check_in_progress) {
        return false;
    }

    ota_check_result_ready = false;
    ota_check_in_progress = true;

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        ota_check_task_entry,
        "ota_check",
        8192,
        nullptr,
        1,
        &ota_check_task_handle,
        tskNO_AFFINITY
    );

    if (task_ok != pdPASS) {
        ota_check_in_progress = false;
        ota_check_task_handle = nullptr;
        Serial.println("[OTA] Failed to create check task.");
        return false;
    }

    last_ota_check_at_ms = now_ms;
    Serial.printf("[OTA] %s check scheduled (current=%s uptime=%lu ms wifi=%d ip=%s)\n",
                  tag ? tag : "Async",
                  FIRMWARE_VERSION,
                  static_cast<unsigned long>(now_ms),
                  static_cast<int>(WiFi.status()),
                  WiFi.localIP().toString().c_str());
    return true;
}

void consume_ota_check_result(uint32_t now_ms) {
    if (!ota_check_result_ready) {
        return;
    }

    ota_update::FirmwareInfo info = {};
    bool success = false;

    portENTER_CRITICAL(&ota_check_lock);
    info = ota_check_result_info;
    success = ota_check_result_success;
    ota_check_result_ready = false;
    portEXIT_CRITICAL(&ota_check_lock);

    if (!success) {
        Serial.printf("[OTA] Check failed: %s\n", ota_update::get_error_message());
        return;
    }

    ota_config_cache.last_check_time = now_ms / 1000U;

    if (info.has_update) {
        cache_pending_ota_update(info, now_ms);
        if (!device_config::save_ota(ota_config_cache)) {
            Serial.println("[OTA] Failed to persist pending update info.");
        }
        edit_controller_set_update_available(true);
        Serial.printf("[OTA] New firmware available: %s\n", info.version);
        return;
    }

    const bool had_pending_update = ota_config_cache.has_pending_update;
    clear_cached_pending_ota_update();
    ota_config_cache.last_check_time = now_ms / 1000U;
    if (!device_config::save_ota(ota_config_cache)) {
        Serial.println("[OTA] Failed to persist cleared update info.");
    }
    if (had_pending_update) {
        edit_controller_set_update_available(false);
    }
    Serial.println("[OTA] No update available.");
}

void reset_local_input_state() {
    key_last_pressed = false;
    key_long_triggered = false;
    key_pressed_at_ms = 0;
}

bool should_poll_local_inputs(uint32_t now_ms) {
    if (sa818_is_tx()) {
        local_input_gate_state = LocalInputGateState::TX_ACTIVE;
        local_input_resume_at_ms = 0;
        reset_local_input_state();
        return false;
    }

    switch (local_input_gate_state) {
        case LocalInputGateState::READY:
            return true;
        case LocalInputGateState::TX_PREPARE:
            return false;
        case LocalInputGateState::TX_ACTIVE:
            local_input_gate_state = LocalInputGateState::TX_RELEASE_GUARD;
            local_input_resume_at_ms = now_ms + INPUT_RESUME_AFTER_PTT_MS;
            return false;
        case LocalInputGateState::TX_RELEASE_GUARD:
            if (local_input_resume_at_ms != 0 &&
                static_cast<int32_t>(now_ms - local_input_resume_at_ms) >= 0) {
                local_input_gate_state = LocalInputGateState::READY;
                local_input_resume_at_ms = 0;
                return true;
            }
            return false;
        default:
            return true;
    }
}

bool init_spiffs_storage() {
    if (SPIFFS.begin(false)) {
        const size_t total_bytes = SPIFFS.totalBytes();
        const size_t used_bytes = SPIFFS.usedBytes();
        Serial.printf("[SPIFFS] mounted. total=%u used=%u\n",
                      static_cast<unsigned int>(total_bytes),
                      static_cast<unsigned int>(used_bytes));
        return true;
    }

    Serial.println("[SPIFFS] mount failed, formatting...");
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] format+mount failed.");
        return false;
    }

    const size_t total_bytes = SPIFFS.totalBytes();
    const size_t used_bytes = SPIFFS.usedBytes();
    Serial.printf("[SPIFFS] formatted and mounted. total=%u used=%u\n",
                  static_cast<unsigned int>(total_bytes),
                  static_cast<unsigned int>(used_bytes));
    return true;
}

void append_startup_log(const char *message) {
    if (!ui_startinfo) {
        return;
    }

    lv_textarea_add_text(ui_startinfo, message);
    lv_textarea_add_text(ui_startinfo, "\n");
    lv_textarea_set_cursor_pos(ui_startinfo, LV_TEXTAREA_CURSOR_LAST);
}

void set_startup_progress(uint8_t progress, const char *message) {
    if (ui_processstat) {
        lv_bar_set_value(ui_processstat, progress, LV_ANIM_OFF);
    }
    if (message && message[0] != '\0') {
        append_startup_log(message);
    }
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
    strftime(header_text, sizeof(header_text), "%H:%M", &local_tm);

    set_time_label_if_present(ui_time, header_text);
    set_time_label_if_present(ui_time1, header_text);

    time_placeholder_rendered = false;
    last_displayed_epoch = now;
}

void check_ota_update_if_needed(uint32_t now_ms) {
    consume_ota_check_result(now_ms);

    if (app_state != AppState::MAIN_READY) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (!ota_waiting_for_wifi_logged) {
            Serial.printf("[OTA] Waiting for WiFi before check (status=%d uptime=%lu ms)\n",
                          static_cast<int>(WiFi.status()),
                          static_cast<unsigned long>(now_ms));
            ota_waiting_for_wifi_logged = true;
        }
        return;
    }
    if (ota_waiting_for_wifi_logged) {
        Serial.printf("[OTA] WiFi ready for OTA checks. IP=%s uptime=%lu ms\n",
                      WiFi.localIP().toString().c_str(),
                      static_cast<unsigned long>(now_ms));
        ota_waiting_for_wifi_logged = false;
    }

    if (!ota_config_cache.auto_check_enabled) {
        if (!ota_auto_check_disabled_logged) {
            Serial.printf("[OTA] Auto-check disabled in config. last_check=%lu pending=%d pending_ver=%s\n",
                          static_cast<unsigned long>(ota_config_cache.last_check_time),
                          ota_config_cache.has_pending_update ? 1 : 0,
                          ota_config_cache.available_version[0] != '\0'
                              ? ota_config_cache.available_version
                              : "<none>");
            ota_auto_check_disabled_logged = true;
        }
        return;
    }
    ota_auto_check_disabled_logged = false;

    if (ota_check_in_progress) {
        if (!ota_check_in_progress_logged) {
            Serial.println("[OTA] Check task already in progress, waiting for result...");
            ota_check_in_progress_logged = true;
        }
        return;
    }
    ota_check_in_progress_logged = false;

    // 首次检查：WiFi连接后5秒
    if (!ota_check_done && now_ms <= 5000) {
        if (!ota_waiting_for_initial_delay_logged) {
            Serial.printf("[OTA] Waiting first-check delay. uptime=%lu/5000 ms\n",
                          static_cast<unsigned long>(now_ms));
            ota_waiting_for_initial_delay_logged = true;
        }
        return;
    }
    ota_waiting_for_initial_delay_logged = false;

    if (!ota_check_done && now_ms > 5000) {
        if (schedule_ota_check(now_ms, "First")) {
            ota_check_done = true;
        }
        return;
    }

    // 定时检查：每小时一次
    if (ota_check_done && (now_ms - last_ota_check_at_ms) >= OTA_CHECK_INTERVAL_MS) {
        schedule_ota_check(now_ms, "Periodic");
    }
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

    append_startup_log("Power key accepted. Starting boot...");
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
    set_startup_progress(5, "[5%] Startup screen ready");
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

    if (startup_step_index >= STARTUP_STEP_COUNT) {
        enter_main_screen();
        return;
    }

    switch (startup_step_index) {
        case 0: {
            const bool radio_ok = edit_controller_boot_radio_init();
            set_startup_progress(radio_ok ? 25 : 20,
                                 radio_ok
                                     ? "[25%] RF module ready, config synced"
                                     : "[20%] RF module init failed");
            break;
        }
        case 1: {
            const bool spiffs_ok = init_spiffs_storage();
            set_startup_progress(spiffs_ok
                                     ? 40
                                     : 35,
                                 spiffs_ok
                                     ? "[40%] SPIFFS mounted"
                                     : "[35%] SPIFFS init failed");
            break;
        }
        case 2:
            connectivity_manager_init();
            set_startup_progress(60, "[60%] Connectivity manager initialized");
            break;
        case 3: {
            const bool net_audio_ok = net_audio_link_init();
            set_startup_progress(net_audio_ok
                                     ? 80
                                     : 75,
                                 net_audio_ok
                                     ? "[80%] NET audio link initialized"
                                     : "[75%] NET audio link init failed");
            break;
        }
        case 4:
            refresh_time_widgets(true);
            set_startup_progress(90, "[90%] Runtime checks complete");
            break;
        case 5: {
            ota_update::init();
            device_config::DeviceConfig config;
            device_config::load(config);
            reconcile_pending_ota_state_on_boot(config.ota);
            ota_config_cache = config.ota;
            Serial.printf("[OTA] Config loaded: auto=%d last_check=%lu pending=%d pending_ver=%s api=%s current=%s model=%d\n",
                          ota_config_cache.auto_check_enabled ? 1 : 0,
                          static_cast<unsigned long>(ota_config_cache.last_check_time),
                          ota_config_cache.has_pending_update ? 1 : 0,
                          ota_config_cache.available_version[0] != '\0'
                              ? ota_config_cache.available_version
                              : "<none>",
                          config.server.http_api_base_url[0] != '\0'
                              ? config.server.http_api_base_url
                              : "<empty>",
                          FIRMWARE_VERSION,
                          DEVICE_MODEL);

            // 如果之前检查过有更新，显示图标
            if (ota_config_cache.has_pending_update) {
                Serial.printf("[OTA] Pending update available: %s\n", ota_config_cache.available_version);
                edit_controller_set_update_available(true);
            }

            set_startup_progress(100, "[100%] Boot complete, entering main screen");
            break;
        }
        default:
            break;
    }

    ++startup_step_index;
    startup_next_step_at_ms = now_ms + STARTUP_STEP_INTERVAL_MS;
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

    if (app_state == AppState::MAIN_READY && pressed && !key_long_triggered) {
        if ((now_ms - key_pressed_at_ms) >= KEY_LONG_PRESS_MS) {
            key_long_triggered = true;
            edit_controller_on_key0_long_press();
        }
    }

    key_last_pressed = pressed;
}
} // namespace

void app_logic_init() {
    pinMode(KEY0_PIN, INPUT_PULLUP);
    ensure_local_timezone();
    http_request_gate::init();

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
    local_input_gate_state = LocalInputGateState::READY;
    local_input_resume_at_ms = 0;
    auto_start_enabled = device_config::load_auto_start_enabled();
    device_config::set_defaults(ota_config_cache);
    ota_check_task_handle = nullptr;
    ota_check_result_ready = false;
    ota_check_result_success = false;
    ota_check_in_progress = false;
    ota_check_done = false;
    last_ota_check_at_ms = 0;
    ota_waiting_for_wifi_logged = false;
    ota_waiting_for_initial_delay_logged = false;
    ota_auto_check_disabled_logged = false;
    ota_check_in_progress_logged = false;
    refresh_time_widgets(true);
    if (auto_start_enabled) {
        Serial.println("Auto-start is enabled. Starting boot sequence.");
        start_boot_sequence(millis());
        return;
    }

    Serial.println("Waiting for KEY long press to boot.");
}

void app_logic_update() {
    const uint32_t now_ms = millis();
    update_startup_sequence(now_ms);
    refresh_time_widgets(false);
    check_ota_update_if_needed(now_ms);
    connectivity_manager_update();
    net_audio_link_update();

    if (should_poll_local_inputs(now_ms)) {
        ec11_update();
        update_power_key(now_ms);
        edit_controller_update();
    }
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

void app_logic_on_ptt_prepare() {
    local_input_gate_state = LocalInputGateState::TX_PREPARE;
    local_input_resume_at_ms = 0;
    reset_local_input_state();
}

void app_logic_on_ptt_started() {
    local_input_gate_state = LocalInputGateState::TX_ACTIVE;
    local_input_resume_at_ms = 0;
    reset_local_input_state();
}

void app_logic_on_ptt_released() {
    local_input_gate_state = LocalInputGateState::TX_RELEASE_GUARD;
    local_input_resume_at_ms = millis() + INPUT_RESUME_AFTER_PTT_MS;
    reset_local_input_state();
}
