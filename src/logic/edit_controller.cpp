#include "edit_controller.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <lvgl.h>

#include <cctype>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../config.h"
#include "../drivers/sa818_driver.h"
#include "../ui/ui.h"
#include "connectivity_manager.h"
#include "device_config.h"
#include "net_audio_link.h"

// Backlight control is implemented in main.cpp.
extern void updateBacklight(float level);

namespace {
constexpr uint32_t FREQ_MIN_X10000 = 4000000;  // 400.0000 MHz
constexpr uint32_t FREQ_MAX_X10000 = 4700000;  // 470.0000 MHz
constexpr int8_t FREQ_EDITABLE_DIGITS = 7;

constexpr uint32_t INFO_PANEL_COLOR_IDLE = 0x006EC7;
constexpr uint32_t INFO_PANEL_COLOR_TX = 0xB71C1C;
constexpr uint32_t INFO_PANEL_COLOR_RX = 0x2E7D32;
constexpr uint32_t HIGHLIGHT_COLOR_SELECT = 0xFF6A00;
constexpr uint32_t HIGHLIGHT_COLOR_EDIT = 0xFF6A00;
constexpr size_t NETWORK_BRIDGE_SOURCE_TEXT_MAX = 48;

constexpr char DEFAULT_INFO_TEXT[] = "BH5UVN";

enum class EditStage {
    NONE = 0,
    TX_FREQ,
    RX_FREQ,
    TX_TONE,
    RX_TONE,
    RX_SQL,
    BAND,
    SETTINGS,
};

enum class EditMode {
    NONE = 0,
    SELECT,
    EDIT,
};

enum class SettingsCursor : uint8_t {
    BRIGHTNESS = 0,
    BLE,
    AUTOSTART,
    RF,
    ABOUT,
};

enum class SettingsMode : uint8_t {
    NONE = 0,
    SELECT,
    EDIT,
};

enum class RfCursor : uint8_t {
    POWER = 0,
    GUARD_ENABLE,
    SINGLE_LIMIT,
    WINDOW_SIZE,
    WINDOW_MAX_TX,
};

enum class PowerMenuOption : uint8_t {
    CANCEL = 0,
    LOCK_SCREEN,
    POWER_OFF,
};

using SubAudioType = device_config::SubAudioType;
using SubAudioSetting = device_config::SubAudioSetting;

constexpr int32_t BACKLIGHT_PWM_STEP = 1;

// SA818 manual CTCSS 1..38.
constexpr const char *CTCSS_TONES[] = {
    "67.0",  "71.9",  "74.4",  "77.0",  "79.7",  "82.5",  "85.4",  "88.5",  "91.5",  "94.8",
    "97.4",  "100.0", "103.5", "107.2", "110.9", "114.8", "118.8", "123.0", "127.3", "131.8",
    "136.5", "141.3", "146.2", "151.4", "156.7", "162.2", "167.9", "173.8", "179.9", "186.2",
    "192.8", "203.5", "210.7", "218.1", "225.7", "233.6", "241.8", "250.3"
};

// SA818 manual CDCSS base codes (N/I share the same numeric code table).
constexpr uint16_t CDCSS_TONES[] = {
    23,  25,  26,  31,  32,  43,  47,  51,  54,  65,  71,  72,  73,  74,  114, 115, 116,
    125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244, 245,
    251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413,
    423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546, 565, 606, 612, 624, 627, 631,
    632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754
};

constexpr size_t CTCSS_TONE_COUNT = sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]);
constexpr size_t CDCSS_TONE_COUNT = sizeof(CDCSS_TONES) / sizeof(CDCSS_TONES[0]);
constexpr size_t CDCSS_POLARITY_COUNT = 2;
constexpr size_t SUBAUDIO_OPTION_COUNT = 1 + CTCSS_TONE_COUNT + (CDCSS_TONE_COUNT * CDCSS_POLARITY_COUNT);

EditStage edit_stage = EditStage::NONE;
EditMode edit_mode = EditMode::NONE;

uint32_t tx_frequency_x10000 = 4395000;  // 439.5000 MHz
uint32_t rx_frequency_x10000 = 4385000;  // 438.5000 MHz
SubAudioSetting tx_subaudio = {SubAudioType::CTCSS, 7};  // 88.5
SubAudioSetting rx_subaudio = {SubAudioType::CTCSS, 7};  // 88.5
SA818Squelch radio_squelch = SA818Squelch::SQ_4;
bool radio_wide_band = false;
bool radio_power_high = true;
bool rf_guard_enabled = device_config::RF_GUARD_ENABLED_DEFAULT;
uint16_t rf_guard_single_tx_limit_s = device_config::RF_GUARD_SINGLE_TX_LIMIT_DEFAULT_S;
uint16_t rf_guard_window_s = device_config::RF_GUARD_WINDOW_DEFAULT_S;
uint16_t rf_guard_max_tx_in_window_s = device_config::RF_GUARD_MAX_TX_IN_WINDOW_DEFAULT_S;

uint8_t digit_positions[16] = {};
uint8_t digit_count = 0;
int8_t editing_digit_index = -1;

bool sql_rx_active = false;
bool net_bridge_active = false;
char net_bridge_source_text[NETWORK_BRIDGE_SOURCE_TEXT_MAX] = {0};
bool rf_overload_active = false;
bool radio_cfg_dirty = false;
lv_obj_t *save_overlay = nullptr;
uint8_t backlight_pwm = device_config::BACKLIGHT_PWM_MAX;
bool auto_start_enabled = false;
SettingsCursor settings_cursor = SettingsCursor::BRIGHTNESS;
SettingsMode settings_mode = SettingsMode::NONE;
bool info_preselect_active = false;
bool rf_select_active = false;
bool rf_edit_mode = false;
bool rf_page_dirty = false;
RfCursor rf_cursor = RfCursor::POWER;
bool screen_locked = false;
lv_obj_t *power_popup = nullptr;
lv_obj_t *power_popup_option_labels[3] = {nullptr, nullptr, nullptr};
PowerMenuOption power_popup_option = PowerMenuOption::CANCEL;
uint32_t last_settings_ble_refresh_ms = 0;
uint32_t last_info_refresh_ms = 0;

void apply_main_screen_overrides();
void hide_header_update_indicators();
void ensure_power_popup();
void show_power_popup();
void hide_power_popup_internal();
bool is_power_popup_visible_internal();
void refresh_power_popup_options();
void apply_screen_lock(bool locked);

bool is_editing_session_active() {
    return edit_stage != EditStage::NONE;
}

enum class ActiveScreen : uint8_t {
    UNKNOWN = 0,
    MAIN,
    SETTINGS,
    RF,
    INFO,
};

ActiveScreen get_active_screen() {
    lv_obj_t *active = lv_scr_act();
    if (active == ui_main) {
        return ActiveScreen::MAIN;
    }
    if (active == ui_SettingPAGE) {
        return ActiveScreen::SETTINGS;
    }
    if (active == ui_RFPAGE) {
        return ActiveScreen::RF;
    }
    if (active == ui_InfoPage) {
        return ActiveScreen::INFO;
    }
    return ActiveScreen::UNKNOWN;
}

bool is_frequency_stage() {
    return edit_stage == EditStage::TX_FREQ || edit_stage == EditStage::RX_FREQ;
}

bool is_tone_stage() {
    return edit_stage == EditStage::TX_TONE || edit_stage == EditStage::RX_TONE;
}

bool is_sql_stage() {
    return edit_stage == EditStage::RX_SQL;
}

bool is_band_stage() {
    return edit_stage == EditStage::BAND;
}

bool is_rx_stage() {
    return edit_stage == EditStage::RX_FREQ ||
           edit_stage == EditStage::RX_TONE ||
           edit_stage == EditStage::RX_SQL;
}

bool is_select_mode() {
    return is_editing_session_active() && edit_mode == EditMode::SELECT;
}

bool is_edit_mode() {
    return is_editing_session_active() && edit_mode == EditMode::EDIT;
}

bool settings_in_select_mode() {
    return settings_mode == SettingsMode::SELECT;
}

bool settings_in_edit_mode() {
    return settings_mode == SettingsMode::EDIT;
}

bool has_selected_frequency_digit() {
    return is_frequency_stage() &&
           editing_digit_index >= 0 &&
           editing_digit_index < FREQ_EDITABLE_DIGITS;
}

uint32_t active_highlight_color() {
    return is_edit_mode() ? HIGHLIGHT_COLOR_EDIT : HIGHLIGHT_COLOR_SELECT;
}

uint32_t clamp_frequency_range(int64_t value) {
    if (value < static_cast<int64_t>(FREQ_MIN_X10000)) {
        return FREQ_MIN_X10000;
    }
    if (value > static_cast<int64_t>(FREQ_MAX_X10000)) {
        return FREQ_MAX_X10000;
    }
    return static_cast<uint32_t>(value);
}

uint16_t clamp_u16_range(uint16_t value, uint16_t min_value, uint16_t max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void format_network_bridge_source_text(const char *call_sign, uint8_t ssid, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (!call_sign || call_sign[0] == '\0') {
        return;
    }

    const unsigned int ssid_value = static_cast<unsigned int>(ssid);
    if (ssid_value > 0U) {
        snprintf(out, out_len, "%s-%u  SPK", call_sign, ssid_value);
    } else {
        snprintf(out, out_len, "%s  SPK", call_sign);
    }
}

SubAudioSetting sanitize_subaudio(SubAudioType type, uint8_t index) {
    switch (type) {
        case SubAudioType::OFF:
            return {SubAudioType::OFF, 0};
        case SubAudioType::CTCSS:
            return {SubAudioType::CTCSS, static_cast<uint8_t>(index % CTCSS_TONE_COUNT)};
        case SubAudioType::CDCSS_N:
            return {SubAudioType::CDCSS_N, static_cast<uint8_t>(index % CDCSS_TONE_COUNT)};
        case SubAudioType::CDCSS_I:
            return {SubAudioType::CDCSS_I, static_cast<uint8_t>(index % CDCSS_TONE_COUNT)};
        default:
            return {SubAudioType::OFF, 0};
    }
}

char cdcss_polarity_suffix(SubAudioType type) {
    return (type == SubAudioType::CDCSS_I) ? 'I' : 'N';
}

void format_frequency(uint32_t value_x10000, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }

    const uint32_t mhz = value_x10000 / 10000;
    const uint32_t decimal = value_x10000 % 10000;
    snprintf(buffer, buffer_len, "%03lu.%04lu", static_cast<unsigned long>(mhz), static_cast<unsigned long>(decimal));
}

void format_subaudio_text_compact(const SubAudioSetting &setting, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }

    switch (setting.type) {
        case SubAudioType::OFF:
            snprintf(buffer, buffer_len, "OFF");
            break;
        case SubAudioType::CTCSS:
            snprintf(buffer,
                     buffer_len,
                     "T%u",
                     static_cast<unsigned int>(((setting.index < CTCSS_TONE_COUNT) ? setting.index : 0) + 1));
            break;
        case SubAudioType::CDCSS_N:
        case SubAudioType::CDCSS_I: {
            const size_t idx = (setting.index < CDCSS_TONE_COUNT) ? setting.index : 0;
            snprintf(buffer,
                     buffer_len,
                     "D%03u%c",
                     static_cast<unsigned int>(CDCSS_TONES[idx]),
                     cdcss_polarity_suffix(setting.type));
            break;
        }
        default:
            snprintf(buffer, buffer_len, "OFF");
            break;
    }
}

void log_radio_config(const char *tag) {
    char tx_freq[16] = {0};
    char rx_freq[16] = {0};
    char tx_tone[16] = {0};
    char rx_tone[16] = {0};

    format_frequency(tx_frequency_x10000, tx_freq, sizeof(tx_freq));
    format_frequency(rx_frequency_x10000, rx_freq, sizeof(rx_freq));
    format_subaudio_text_compact(tx_subaudio, tx_tone, sizeof(tx_tone));
    format_subaudio_text_compact(rx_subaudio, rx_tone, sizeof(rx_tone));

    Serial.printf("[RADIO][%s] TX=%s RX=%s TXTone=%s RXTone=%s SQL=%u BW=%s PWR=%s Guard=%d Single=%us Window=%us Max=%us dirty=%d\n",
                  tag ? tag : "LOG",
                  tx_freq,
                  rx_freq,
                  tx_tone,
                  rx_tone,
                  static_cast<unsigned int>(radio_squelch),
                  radio_wide_band ? "wide" : "narrow",
                  radio_power_high ? "high" : "low",
                  rf_guard_enabled ? 1 : 0,
                  static_cast<unsigned int>(rf_guard_single_tx_limit_s),
                  static_cast<unsigned int>(rf_guard_window_s),
                  static_cast<unsigned int>(rf_guard_max_tx_in_window_s),
                  radio_cfg_dirty ? 1 : 0);
}

uint32_t get_display_frequency_value() {
    // Runtime default: RX frequency. Switch to TX only while PTT is active.
    if (!is_editing_session_active()) {
        return sa818_is_tx() ? tx_frequency_x10000 : rx_frequency_x10000;
    }

    if (is_rx_stage()) {
        return rx_frequency_x10000;
    }
    return tx_frequency_x10000;
}

uint32_t &get_active_frequency_value() {
    if (edit_stage == EditStage::RX_FREQ) {
        return rx_frequency_x10000;
    }
    return tx_frequency_x10000;
}

SubAudioSetting &get_active_subaudio_setting() {
    if (edit_stage == EditStage::RX_TONE) {
        return rx_subaudio;
    }
    return tx_subaudio;
}

uint32_t digit_step_value(int8_t digit_index) {
    static constexpr uint32_t DIGIT_STEPS[FREQ_EDITABLE_DIGITS] = {
        1, 10, 100, 1000, 10000, 100000, 1000000
    };

    if (digit_index < 0 || digit_index >= FREQ_EDITABLE_DIGITS) {
        return 0;
    }

    return DIGIT_STEPS[digit_index];
}

void rebuild_digit_positions(const char *formatted_frequency) {
    digit_count = 0;

    if (!formatted_frequency) {
        return;
    }

    const int length = static_cast<int>(strlen(formatted_frequency));
    for (int i = length - 1; i >= 0; --i) {
        if (std::isdigit(static_cast<unsigned char>(formatted_frequency[i]))) {
            digit_positions[digit_count++] = static_cast<uint8_t>(i);

            if (digit_count >= sizeof(digit_positions)) {
                break;
            }
        }
    }
}

void format_subaudio_text(const SubAudioSetting &setting, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }

    switch (setting.type) {
        case SubAudioType::OFF:
            snprintf(buffer, buffer_len, "OFF");
            break;
        case SubAudioType::CTCSS: {
            const size_t idx = (setting.index < CTCSS_TONE_COUNT) ? setting.index : 0;
            snprintf(buffer, buffer_len, "%s", CTCSS_TONES[idx]);
            break;
        }
        case SubAudioType::CDCSS_N: {
            const size_t idx = (setting.index < CDCSS_TONE_COUNT) ? setting.index : 0;
            snprintf(buffer,
                     buffer_len,
                     "D%03u%c",
                     static_cast<unsigned int>(CDCSS_TONES[idx]),
                     cdcss_polarity_suffix(setting.type));
            break;
        }
        case SubAudioType::CDCSS_I: {
            const size_t idx = (setting.index < CDCSS_TONE_COUNT) ? setting.index : 0;
            snprintf(buffer,
                     buffer_len,
                     "D%03u%c",
                     static_cast<unsigned int>(CDCSS_TONES[idx]),
                     cdcss_polarity_suffix(setting.type));
            break;
        }
        default:
            snprintf(buffer, buffer_len, "OFF");
            break;
    }
}

size_t subaudio_to_option(const SubAudioSetting &setting) {
    switch (setting.type) {
        case SubAudioType::OFF:
            return 0;
        case SubAudioType::CTCSS: {
            const size_t idx = (setting.index < CTCSS_TONE_COUNT) ? setting.index : 0;
            return 1 + idx;
        }
        case SubAudioType::CDCSS_N: {
            const size_t idx = (setting.index < CDCSS_TONE_COUNT) ? setting.index : 0;
            return 1 + CTCSS_TONE_COUNT + idx;
        }
        case SubAudioType::CDCSS_I: {
            const size_t idx = (setting.index < CDCSS_TONE_COUNT) ? setting.index : 0;
            return 1 + CTCSS_TONE_COUNT + CDCSS_TONE_COUNT + idx;
        }
        default:
            return 0;
    }
}

SubAudioSetting option_to_subaudio(size_t option) {
    if (option == 0) {
        return {SubAudioType::OFF, 0};
    }

    if (option <= CTCSS_TONE_COUNT) {
        return {SubAudioType::CTCSS, static_cast<uint8_t>(option - 1)};
    }

    const size_t dcs_option = option - (1 + CTCSS_TONE_COUNT);
    if (dcs_option < CDCSS_TONE_COUNT) {
        return {SubAudioType::CDCSS_N, static_cast<uint8_t>(dcs_option)};
    }
    if (dcs_option < (CDCSS_TONE_COUNT * CDCSS_POLARITY_COUNT)) {
        return {SubAudioType::CDCSS_I, static_cast<uint8_t>(dcs_option - CDCSS_TONE_COUNT)};
    }

    return {SubAudioType::OFF, 0};
}

bool apply_subaudio_to_sa818(bool is_tx, const SubAudioSetting &setting) {
    switch (setting.type) {
        case SubAudioType::OFF:
            return is_tx ? sa818_set_ctcss_tx(0) : sa818_set_ctcss_rx(0);
        case SubAudioType::CTCSS:
            if (setting.index >= CTCSS_TONE_COUNT) {
                return false;
            }
            return is_tx ? sa818_set_ctcss_tx(static_cast<uint16_t>(setting.index + 1))
                         : sa818_set_ctcss_rx(static_cast<uint16_t>(setting.index + 1));
        case SubAudioType::CDCSS_N:
        case SubAudioType::CDCSS_I:
            if (setting.index >= CDCSS_TONE_COUNT) {
                return false;
            }
            return is_tx ? sa818_set_cdcss_tx(CDCSS_TONES[setting.index], setting.type == SubAudioType::CDCSS_I)
                         : sa818_set_cdcss_rx(CDCSS_TONES[setting.index], setting.type == SubAudioType::CDCSS_I);
        default:
            return false;
    }
}

void build_subaudio_string(const SubAudioSetting &setting, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }
    switch (setting.type) {
        case SubAudioType::CTCSS:
            if (setting.index < CTCSS_TONE_COUNT) {
                snprintf(buffer, buffer_len, "%04u", static_cast<unsigned int>(setting.index + 1));
            } else {
                snprintf(buffer, buffer_len, "0000");
            }
            break;
        case SubAudioType::CDCSS_N:
        case SubAudioType::CDCSS_I:
            if (setting.index < CDCSS_TONE_COUNT) {
                snprintf(buffer,
                         buffer_len,
                         "%03u%c",
                         static_cast<unsigned int>(CDCSS_TONES[setting.index]),
                         cdcss_polarity_suffix(setting.type));
            } else {
                snprintf(buffer, buffer_len, "0000");
            }
            break;
        case SubAudioType::OFF:
        default:
            snprintf(buffer, buffer_len, "0000");
            break;
    }
}

bool apply_all_radio_config_to_sa818() {
    if (!sa818_is_enabled()) {
        return false;
    }

    sa818_set_power(radio_power_high ? SA818Power::POWER_HIGH : SA818Power::POWER_LOW);

    char tx_sub[8] = {0};
    char rx_sub[8] = {0};
    build_subaudio_string(tx_subaudio, tx_sub, sizeof(tx_sub));
    build_subaudio_string(rx_subaudio, rx_sub, sizeof(rx_sub));

    SA818GroupConfig group = {};
    group.tx_freq_khz = tx_frequency_x10000 / 10;
    group.rx_freq_khz = rx_frequency_x10000 / 10;
    group.tx_subaudio = tx_sub;
    group.rx_subaudio = rx_sub;
    group.squelch = radio_squelch;
    group.wide_band = radio_wide_band;

    return sa818_apply_group(group);
}

device_config::RadioConfig build_radio_config_snapshot() {
    device_config::RadioConfig config = {};
    config.tx_frequency_x10000 = tx_frequency_x10000;
    config.rx_frequency_x10000 = rx_frequency_x10000;
    config.tx_subaudio = tx_subaudio;
    config.rx_subaudio = rx_subaudio;
    config.squelch = static_cast<uint8_t>(radio_squelch);
    config.wide_band = radio_wide_band;
    config.power_high = radio_power_high;
    config.rf_guard_enabled = rf_guard_enabled;
    config.rf_guard_single_tx_limit_s = rf_guard_single_tx_limit_s;
    config.rf_guard_window_s = rf_guard_window_s;
    config.rf_guard_max_tx_in_window_s = rf_guard_max_tx_in_window_s;
    return config;
}

void apply_runtime_radio_config(const device_config::RadioConfig &config) {
    tx_frequency_x10000 = clamp_frequency_range(config.tx_frequency_x10000);
    rx_frequency_x10000 = clamp_frequency_range(config.rx_frequency_x10000);
    tx_subaudio = sanitize_subaudio(config.tx_subaudio.type, config.tx_subaudio.index);
    rx_subaudio = sanitize_subaudio(config.rx_subaudio.type, config.rx_subaudio.index);

    uint8_t sql = config.squelch;
    if (sql > 8) {
        sql = 4;
    }
    radio_squelch = static_cast<SA818Squelch>(sql);
    radio_wide_band = config.wide_band;
    radio_power_high = config.power_high;
    rf_guard_enabled = config.rf_guard_enabled;
    rf_guard_single_tx_limit_s = clamp_u16_range(
        config.rf_guard_single_tx_limit_s,
        device_config::RF_GUARD_SINGLE_TX_LIMIT_MIN_S,
        device_config::RF_GUARD_SINGLE_TX_LIMIT_MAX_S
    );
    rf_guard_window_s = clamp_u16_range(
        config.rf_guard_window_s,
        device_config::RF_GUARD_WINDOW_MIN_S,
        device_config::RF_GUARD_WINDOW_MAX_S
    );
    rf_guard_max_tx_in_window_s = clamp_u16_range(
        config.rf_guard_max_tx_in_window_s,
        device_config::RF_GUARD_MAX_TX_IN_WINDOW_MIN_S,
        rf_guard_window_s
    );
}

void load_radio_config_from_storage() {
    device_config::DeviceConfig full_config = {};
    device_config::set_defaults(full_config);
    device_config::load(full_config);
    apply_runtime_radio_config(full_config.radio);
}

bool save_radio_config_to_storage() {
    return device_config::save_radio(build_radio_config_snapshot());
}

void on_save_overlay_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == save_overlay) {
        save_overlay = nullptr;
    }
}

void ensure_save_overlay() {
    if (save_overlay && lv_obj_is_valid(save_overlay)) {
        return;
    }

    save_overlay = nullptr;
    lv_obj_t *root = lv_layer_top();
    if (!root) {
        return;
    }

    save_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(save_overlay);
    lv_obj_set_size(save_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(save_overlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(save_overlay, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(save_overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(save_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(save_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(save_overlay, on_save_overlay_deleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t *panel = lv_obj_create(save_overlay);
    lv_obj_set_size(panel, 180, 76);
    lv_obj_set_align(panel, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(panel, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1E2A36), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x4A657F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, "Saving...");
    lv_obj_set_style_text_color(label, lv_color_hex(0xDFF3FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
}

void show_save_overlay() {
    ensure_save_overlay();
    if (!save_overlay || !lv_obj_is_valid(save_overlay)) {
        save_overlay = nullptr;
        return;
    }

    lv_obj_clear_flag(save_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(save_overlay);
    lv_obj_invalidate(save_overlay);
    lv_refr_now(lv_disp_get_default());
}

void hide_save_overlay() {
    if (!save_overlay || !lv_obj_is_valid(save_overlay)) {
        save_overlay = nullptr;
        return;
    }

    lv_obj_add_flag(save_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(save_overlay);
    lv_refr_now(lv_disp_get_default());
}

const char *power_menu_option_text(PowerMenuOption option) {
    switch (option) {
        case PowerMenuOption::CANCEL:
            return "Cancel";
        case PowerMenuOption::LOCK_SCREEN:
            return "Lock Screen";
        case PowerMenuOption::POWER_OFF:
            return "Power Off";
        default:
            return "Cancel";
    }
}

void refresh_power_popup_options() {
    constexpr uint32_t SELECTED_COLOR = HIGHLIGHT_COLOR_SELECT;
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *label = power_popup_option_labels[i];
        if (!label) {
            continue;
        }

        const PowerMenuOption option = static_cast<PowerMenuOption>(i);
        const char *text = power_menu_option_text(option);
        if (option == power_popup_option) {
            char highlighted[48] = {0};
            lv_label_set_recolor(label, true);
            snprintf(highlighted,
                     sizeof(highlighted),
                     "#%06lX %s#",
                     static_cast<unsigned long>(SELECTED_COLOR),
                     text);
            lv_label_set_text(label, highlighted);
        } else {
            lv_label_set_recolor(label, false);
            lv_label_set_text(label, text);
        }
    }
}

void ensure_power_popup() {
    if (power_popup && lv_obj_is_valid(power_popup)) {
        return;
    }

    power_popup = nullptr;
    lv_obj_t *root = lv_layer_top();
    if (!root) {
        return;
    }

    power_popup = lv_obj_create(root);
    lv_obj_remove_style_all(power_popup);
    lv_obj_set_size(power_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(power_popup, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(power_popup, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(power_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_popup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(power_popup);
    lv_obj_set_size(panel, 220, 170);
    lv_obj_set_align(panel, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1A2330), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x4A657F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Power Menu");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE4F0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (size_t i = 0; i < 3; ++i) {
        power_popup_option_labels[i] = lv_label_create(panel);
        lv_obj_set_align(power_popup_option_labels[i], LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(power_popup_option_labels[i], 22, 46 + static_cast<lv_coord_t>(i * 32));
        lv_obj_set_style_text_font(power_popup_option_labels[i], &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(power_popup_option_labels[i], lv_color_hex(0xDFF3FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    power_popup_option = PowerMenuOption::CANCEL;
    refresh_power_popup_options();
}

bool is_power_popup_visible_internal() {
    return power_popup && lv_obj_is_valid(power_popup) && !lv_obj_has_flag(power_popup, LV_OBJ_FLAG_HIDDEN);
}

void hide_power_popup_internal() {
    if (!power_popup || !lv_obj_is_valid(power_popup)) {
        power_popup = nullptr;
        return;
    }

    lv_obj_add_flag(power_popup, LV_OBJ_FLAG_HIDDEN);
}

void apply_screen_lock(bool locked) {
    if (screen_locked == locked) {
        return;
    }

    screen_locked = locked;
    if (screen_locked) {
        updateBacklight(0.0f);
        Serial.println("[UI] Screen locked (backlight=0).");
        return;
    }

    updateBacklight(static_cast<float>(backlight_pwm) / 255.0f);
    Serial.println("[UI] Screen unlocked.");
}

void execute_power_popup_action() {
    switch (power_popup_option) {
        case PowerMenuOption::CANCEL:
            hide_power_popup_internal();
            break;
        case PowerMenuOption::LOCK_SCREEN:
            hide_power_popup_internal();
            apply_screen_lock(true);
            break;
        case PowerMenuOption::POWER_OFF:
            hide_power_popup_internal();
            // Ensure backlight is fully off before entering deep sleep.
            updateBacklight(0.0f);
            pinMode(BACKLIGHT_PIN, OUTPUT);
            digitalWrite(BACKLIGHT_PIN, LOW);
            // Put SA818 control lines into a defined power-down state.
            pinMode(SA818_EN, OUTPUT);
            digitalWrite(SA818_EN, LOW);
            pinMode(SA818_PTT, OUTPUT);
            digitalWrite(SA818_PTT, HIGH);
            pinMode(SA818_SQL, OUTPUT);
            digitalWrite(SA818_SQL, HIGH);
            Serial.println("[PWR] Entering deep sleep (power off).");
            delay(30);
            esp_deep_sleep_start();
            break;
        default:
            hide_power_popup_internal();
            break;
    }
}

void move_power_popup_cursor(int32_t delta) {
    if (!is_power_popup_visible_internal() || delta == 0) {
        return;
    }

    int32_t next = static_cast<int32_t>(power_popup_option) + delta;
    constexpr int32_t OPTION_COUNT = 3;
    next %= OPTION_COUNT;
    if (next < 0) {
        next += OPTION_COUNT;
    }

    power_popup_option = static_cast<PowerMenuOption>(next);
    refresh_power_popup_options();
}

void show_power_popup() {
    ensure_power_popup();
    if (!power_popup || !lv_obj_is_valid(power_popup)) {
        power_popup = nullptr;
        return;
    }

    apply_screen_lock(false);
    connectivity_manager_hide_ble_popup();
    net_audio_link_hide_bind_popup();

    power_popup_option = PowerMenuOption::CANCEL;
    refresh_power_popup_options();
    lv_obj_clear_flag(power_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_popup);
}

void refresh_settings_autostart_widget() {
    if (!ui_StartS) {
        return;
    }

    if (auto_start_enabled) {
        lv_obj_add_state(ui_StartS, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_StartS, LV_STATE_CHECKED);
    }
}

void toggle_autostart_from_settings() {
    auto_start_enabled = !auto_start_enabled;
    if (!device_config::save_auto_start_enabled(auto_start_enabled)) {
        Serial.println("Auto-start save failed.");
        auto_start_enabled = device_config::load_auto_start_enabled();
    }
    refresh_settings_autostart_widget();
}

void save_and_apply_radio_config() {
    if (!radio_cfg_dirty) {
        return;
    }

    show_save_overlay();

    if (!save_radio_config_to_storage()) {
        Serial.println("Radio config save failed.");
        log_radio_config("SAVE_FAIL");
    } else {
        Serial.println("Radio config saved.");
        radio_cfg_dirty = false;
        log_radio_config("SAVED");
        net_audio_link_schedule_radio_config_sync();
    }

    if (sa818_is_enabled() && !apply_all_radio_config_to_sa818()) {
        Serial.println("SA818 apply config failed after save.");
    }

    hide_save_overlay();
}

void update_info_panel_state() {
    if (!ui_infoPanel || !ui_Info) {
        return;
    }

    switch (edit_stage) {
        case EditStage::TX_FREQ:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_TX), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "TX Freq");
            break;
        case EditStage::RX_FREQ:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_RX), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "RX Freq");
            break;
        case EditStage::TX_TONE:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_TX), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "TX Tone");
            break;
        case EditStage::RX_TONE:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_RX), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "RX Tone");
            break;
        case EditStage::RX_SQL:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_RX), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "RX SQL");
            break;
        case EditStage::BAND:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_IDLE), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "Band");
            break;
        case EditStage::SETTINGS:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_IDLE), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "Settings");
            break;
        case EditStage::NONE:
        default:
            if (rf_overload_active) {
                lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_TX), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_label_set_text(ui_Info, "RF OverLoad");
                break;
            }
            if (net_bridge_active) {
                lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_TX), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_label_set_text(ui_Info, (net_bridge_source_text[0] != '\0') ? net_bridge_source_text : "NET->RF");
                break;
            }
            lv_obj_set_style_bg_color(
                ui_infoPanel,
                lv_color_hex(sql_rx_active ? INFO_PANEL_COLOR_RX : INFO_PANEL_COLOR_IDLE),
                LV_PART_MAIN | LV_STATE_DEFAULT
            );
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, sql_rx_active ? "RF->NET" : DEFAULT_INFO_TEXT);
            break;
    }
}

void render_subaudio_labels() {
    char tx_text[16] = {0};
    char rx_text[16] = {0};
    format_subaudio_text(tx_subaudio, tx_text, sizeof(tx_text));
    format_subaudio_text(rx_subaudio, rx_text, sizeof(rx_text));

    const uint32_t highlight_color = active_highlight_color();

    if (ui_TXCTCSS) {
        if (edit_stage == EditStage::TX_TONE) {
            lv_label_set_recolor(ui_TXCTCSS, true);
            char rendered[32] = {0};
            snprintf(rendered, sizeof(rendered), "#%06lX %s#", static_cast<unsigned long>(highlight_color), tx_text);
            lv_label_set_text(ui_TXCTCSS, rendered);
        } else {
            lv_label_set_recolor(ui_TXCTCSS, false);
            lv_label_set_text(ui_TXCTCSS, tx_text);
        }
    }

    if (ui_RXCTCSS) {
        if (edit_stage == EditStage::RX_TONE) {
            lv_label_set_recolor(ui_RXCTCSS, true);
            char rendered[32] = {0};
            snprintf(rendered, sizeof(rendered), "#%06lX %s#", static_cast<unsigned long>(highlight_color), rx_text);
            lv_label_set_text(ui_RXCTCSS, rendered);
        } else {
            lv_label_set_recolor(ui_RXCTCSS, false);
            lv_label_set_text(ui_RXCTCSS, rx_text);
        }
    }
}

void render_sql_label() {
    if (!ui_RXSQL) {
        return;
    }

    char sql_text[8] = {0};
    snprintf(sql_text, sizeof(sql_text), "%u", static_cast<unsigned int>(radio_squelch));

    if (edit_stage == EditStage::RX_SQL) {
        lv_label_set_recolor(ui_RXSQL, true);
        char rendered[24] = {0};
        snprintf(rendered, sizeof(rendered), "#%06lX %s#", static_cast<unsigned long>(active_highlight_color()), sql_text);
        lv_label_set_text(ui_RXSQL, rendered);
        return;
    }

    lv_label_set_recolor(ui_RXSQL, false);
    lv_label_set_text(ui_RXSQL, sql_text);
}

void render_band_label() {
    if (!ui_Band) {
        return;
    }

    const char *band_text = radio_wide_band ? "25kHz" : "12.5kHz";

    if (edit_stage == EditStage::BAND) {
        lv_label_set_recolor(ui_Band, true);
        char rendered[32] = {0};
        snprintf(rendered, sizeof(rendered), "#%06lX %s#", static_cast<unsigned long>(active_highlight_color()), band_text);
        lv_label_set_text(ui_Band, rendered);
        return;
    }

    lv_label_set_recolor(ui_Band, false);
    lv_label_set_text(ui_Band, band_text);
}

void clear_selection_border(lv_obj_t *obj) {
    if (!obj) {
        return;
    }
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void apply_selection_border(lv_obj_t *obj, uint32_t color, int16_t padding) {
    if (!obj) {
        return;
    }
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, padding, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void update_selection_border_state() {
    clear_selection_border(ui_Freq);
    clear_selection_border(ui_TXCTCSS);
    clear_selection_border(ui_RXCTCSS);
    clear_selection_border(ui_RXSQL);
    clear_selection_border(ui_Band);
    clear_selection_border(ui_toSetting);

    if (!is_editing_session_active()) {
        return;
    }

    const uint32_t border_color = active_highlight_color();

    if (is_frequency_stage()) {
        apply_selection_border(ui_Freq, border_color, 4);
        return;
    }

    if (edit_stage == EditStage::TX_TONE) {
        apply_selection_border(ui_TXCTCSS, border_color, 2);
        return;
    }

    if (edit_stage == EditStage::RX_TONE) {
        apply_selection_border(ui_RXCTCSS, border_color, 2);
        return;
    }

    if (edit_stage == EditStage::RX_SQL) {
        apply_selection_border(ui_RXSQL, border_color, 2);
        return;
    }

    if (edit_stage == EditStage::BAND) {
        apply_selection_border(ui_Band, border_color, 2);
        return;
    }

    if (edit_stage == EditStage::SETTINGS) {
        apply_selection_border(ui_toSetting, border_color, 2);
    }
}

void render_frequency_label() {
    if (!ui_Freq) {
        return;
    }

    char formatted[16] = {0};
    format_frequency(get_display_frequency_value(), formatted, sizeof(formatted));

    if (!has_selected_frequency_digit()) {
        lv_label_set_recolor(ui_Freq, false);
        lv_label_set_text(ui_Freq, formatted);
        return;
    }

    rebuild_digit_positions(formatted);
    if (digit_count == 0 || editing_digit_index >= static_cast<int8_t>(digit_count)) {
        lv_label_set_recolor(ui_Freq, false);
        lv_label_set_text(ui_Freq, formatted);
        return;
    }

    lv_label_set_recolor(ui_Freq, true);
    const uint32_t highlight_color = active_highlight_color();
    const uint8_t selected_pos = digit_positions[editing_digit_index];
    const size_t length = strlen(formatted);

    char rendered[64] = {0};
    size_t write_pos = 0;

    for (size_t i = 0; i < length; ++i) {
        if (i == selected_pos) {
            const int written = snprintf(
                rendered + write_pos,
                sizeof(rendered) - write_pos,
                "#%06lX %c#",
                static_cast<unsigned long>(highlight_color),
                formatted[i]
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

            rendered[write_pos++] = formatted[i];
            rendered[write_pos] = '\0';
        }
    }

    lv_label_set_text(ui_Freq, rendered);
}

void refresh_edit_widgets() {
    update_info_panel_state();
    render_frequency_label();
    render_subaudio_labels();
    render_sql_label();
    render_band_label();
    update_selection_border_state();
}

void set_edit_stage(EditStage stage) {
    edit_stage = stage;

    if (stage == EditStage::NONE) {
        edit_mode = EditMode::NONE;
        editing_digit_index = -1;
    } else {
        // Only frequency needs SELECT->EDIT confirmation flow.
        edit_mode = is_frequency_stage() ? EditMode::SELECT : EditMode::EDIT;
        editing_digit_index = is_frequency_stage() ? 0 : -1;
    }

    refresh_edit_widgets();
}

bool enter_edit_session() {
    if (sa818_is_tx()) {
        Serial.println("[EDIT] blocked while RF TX is active.");
        return false;
    }

    set_edit_stage(EditStage::TX_FREQ);
    return true;
}

void exit_edit_session() {
    set_edit_stage(EditStage::NONE);
}

void set_label_text_or_empty(lv_obj_t *label, const char *text) {
    if (!label) {
        return;
    }

    if (!text || text[0] == '\0') {
        lv_label_set_text(label, "");
        return;
    }

    lv_label_set_text(label, text);
}

void refresh_info_panel_preselect_state() {
    clear_selection_border(ui_InfoP1);
    if (info_preselect_active) {
        apply_selection_border(ui_InfoP1, HIGHLIGHT_COLOR_SELECT, 2);
    }
}

void apply_backlight_pwm(uint8_t pwm, bool persist) {
    backlight_pwm = device_config::sanitize_backlight_pwm(pwm);
    if (screen_locked) {
        updateBacklight(0.0f);
    } else {
        updateBacklight(static_cast<float>(backlight_pwm) / 255.0f);
    }

    if (ui_LightS) {
        lv_slider_set_range(ui_LightS, device_config::BACKLIGHT_PWM_MIN, device_config::BACKLIGHT_PWM_MAX);
        if (lv_slider_get_value(ui_LightS) != static_cast<int32_t>(backlight_pwm)) {
            lv_slider_set_value(ui_LightS, backlight_pwm, LV_ANIM_OFF);
        }
    }

    if (persist) {
        if (!device_config::save_backlight_pwm(backlight_pwm)) {
            Serial.println("Backlight save failed.");
        }
    }
}

bool save_radio_config_immediately(bool schedule_sync) {
    if (!save_radio_config_to_storage()) {
        Serial.println("Radio config immediate save failed.");
        log_radio_config("IMMEDIATE_SAVE_FAIL");
        return false;
    }

    radio_cfg_dirty = false;

    if (schedule_sync) {
        net_audio_link_schedule_radio_config_sync();
    }

    log_radio_config("IMMEDIATE_SAVE");
    return true;
}

void refresh_rf_page_widgets() {
    hide_header_update_indicators();

    if (ui_PWS) {
        if (radio_power_high) {
            lv_obj_add_state(ui_PWS, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui_PWS, LV_STATE_CHECKED);
        }
    }

    if (ui_rfguard) {
        if (rf_guard_enabled) {
            lv_obj_add_state(ui_rfguard, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui_rfguard, LV_STATE_CHECKED);
        }
    }

    set_label_text_or_empty(ui_PWN, radio_power_high ? "高" : "低");

    char text[16] = {0};
    snprintf(text, sizeof(text), "%us", static_cast<unsigned int>(rf_guard_single_tx_limit_s));
    set_label_text_or_empty(ui_rfguardsingle, text);
    snprintf(text, sizeof(text), "%us", static_cast<unsigned int>(rf_guard_window_s));
    set_label_text_or_empty(ui_rfguardwindow, text);
    snprintf(text, sizeof(text), "%us", static_cast<unsigned int>(rf_guard_max_tx_in_window_s));
    set_label_text_or_empty(ui_rfguardmaxtxinwindow, text);

    clear_selection_border(ui_PWP);
    clear_selection_border(ui_rfguard);
    clear_selection_border(ui_rfguardsingle);
    clear_selection_border(ui_rfguardwindow);
    clear_selection_border(ui_rfguardmaxtxinwindow);

    if (rf_select_active) {
        lv_obj_t *target = nullptr;
        switch (rf_cursor) {
            case RfCursor::POWER:
                target = ui_PWP;
                break;
            case RfCursor::GUARD_ENABLE:
                target = ui_rfguard;
                break;
            case RfCursor::SINGLE_LIMIT:
                target = ui_rfguardsingle;
                break;
            case RfCursor::WINDOW_SIZE:
                target = ui_rfguardwindow;
                break;
            case RfCursor::WINDOW_MAX_TX:
                target = ui_rfguardmaxtxinwindow;
                break;
            default:
                break;
        }
        if (target) {
            apply_selection_border(target, rf_edit_mode ? HIGHLIGHT_COLOR_EDIT : HIGHLIGHT_COLOR_SELECT, 2);
        }
    }
}

void set_radio_power_high(bool high, bool persist, bool schedule_sync) {
    radio_power_high = high;
    if (sa818_is_enabled()) {
        sa818_set_power(radio_power_high ? SA818Power::POWER_HIGH : SA818Power::POWER_LOW);
    }

    if (persist) {
        save_radio_config_immediately(schedule_sync);
    }

    refresh_rf_page_widgets();
}

void refresh_settings_ble_widgets() {
    const bool ble_enabled = connectivity_manager_is_ble_enabled();
    if (ui_BLES) {
        if (ble_enabled) {
            lv_obj_add_state(ui_BLES, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui_BLES, LV_STATE_CHECKED);
        }
    }

    const char *ble_text = ble_enabled
        ? connectivity_manager_get_ble_auth_code()
        : connectivity_manager_get_ble_device_name();
    set_label_text_or_empty(ui_BLEN, ble_text);
}

void refresh_settings_selection_state() {
    clear_selection_border(ui_lightP);
    clear_selection_border(ui_BLEP);
    clear_selection_border(ui_autostart);
    clear_selection_border(ui_RFP);
    clear_selection_border(ui_InfoP);

    if (settings_mode == SettingsMode::NONE) {
        return;
    }

    lv_obj_t *target = nullptr;
    switch (settings_cursor) {
        case SettingsCursor::BRIGHTNESS:
            target = ui_lightP;
            break;
        case SettingsCursor::BLE:
            target = ui_BLEP;
            break;
        case SettingsCursor::AUTOSTART:
            target = ui_autostart;
            break;
        case SettingsCursor::RF:
            target = ui_RFP;
            break;
        case SettingsCursor::ABOUT:
            target = ui_InfoP;
            break;
        default:
            break;
    }

    if (!target) {
        return;
    }

    const uint32_t color = (settings_mode == SettingsMode::EDIT) ? HIGHLIGHT_COLOR_EDIT : HIGHLIGHT_COLOR_SELECT;
    apply_selection_border(target, color, 2);
}

void refresh_settings_page_widgets() {
    hide_header_update_indicators();

    if (ui_LightS) {
        lv_slider_set_range(ui_LightS, device_config::BACKLIGHT_PWM_MIN, device_config::BACKLIGHT_PWM_MAX);
        if (lv_slider_get_value(ui_LightS) != static_cast<int32_t>(backlight_pwm)) {
            lv_slider_set_value(ui_LightS, backlight_pwm, LV_ANIM_OFF);
        }
    }
    refresh_settings_ble_widgets();
    refresh_settings_autostart_widget();
    refresh_settings_selection_state();
}

void refresh_info_page_content() {
    hide_header_update_indicators();

    // Keep value fields left-anchored so dynamic text does not "float" horizontally.
    auto align_info_value = [](lv_obj_t *label, lv_coord_t y) {
        if (!label) {
            return;
        }
        lv_obj_set_width(label, 175);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(label, 118, y);
    };

    align_info_value(ui_Mac, 16);
    align_info_value(ui_Mac1, 50);
    align_info_value(ui_ip2, 84);
    align_info_value(ui_Callsign, 120);
    align_info_value(ui_username, 154);

    device_config::DeviceConfig config = {};
    device_config::set_defaults(config);
    device_config::load(config);

    const uint64_t mac = ESP.getEfuseMac();
    char mac_text[24] = {0};
    snprintf(mac_text,
             sizeof(mac_text),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned int>((mac >> 40) & 0xFFULL),
             static_cast<unsigned int>((mac >> 32) & 0xFFULL),
             static_cast<unsigned int>((mac >> 24) & 0xFFULL),
             static_cast<unsigned int>((mac >> 16) & 0xFFULL),
             static_cast<unsigned int>((mac >> 8) & 0xFFULL),
             static_cast<unsigned int>(mac & 0xFFULL));

    char server_text[96] = {0};
    if (config.server.udp_host[0] != '\0' && config.server.udp_port != 0) {
        snprintf(server_text, sizeof(server_text), "%s:%u", config.server.udp_host, config.server.udp_port);
    }

    char ip_text[20] = {0};
    if (WiFi.status() == WL_CONNECTED) {
        const String ip = WiFi.localIP().toString();
        snprintf(ip_text, sizeof(ip_text), "%s", ip.c_str());
    }

    set_label_text_or_empty(ui_Mac, mac_text);
    set_label_text_or_empty(ui_Mac1, server_text);
    set_label_text_or_empty(ui_ip2, ip_text);
    set_label_text_or_empty(ui_Callsign, config.server.callsign);
    set_label_text_or_empty(ui_username, config.server.account);
}

void open_info_page() {
    if (!ui_InfoPage) {
        return;
    }

    info_preselect_active = false;
    refresh_info_page_content();
    lv_disp_load_scr(ui_InfoPage);
    refresh_info_panel_preselect_state();
}

void close_info_page_to_settings() {
    if (!ui_SettingPAGE) {
        return;
    }

    info_preselect_active = false;
    lv_disp_load_scr(ui_SettingPAGE);
    refresh_settings_page_widgets();
}

void open_rf_page() {
    if (!ui_RFPAGE) {
        return;
    }

    rf_select_active = false;
    rf_edit_mode = false;
    rf_page_dirty = false;
    rf_cursor = RfCursor::POWER;
    refresh_rf_page_widgets();
    lv_disp_load_scr(ui_RFPAGE);
    refresh_rf_page_widgets();
}

void close_rf_page_to_settings() {
    if (!ui_SettingPAGE) {
        return;
    }

    rf_select_active = false;
    rf_edit_mode = false;
    rf_page_dirty = false;
    rf_cursor = RfCursor::POWER;
    lv_disp_load_scr(ui_SettingPAGE);
    refresh_settings_page_widgets();
}

void close_settings_page_to_main() {
    if (!ui_main) {
        return;
    }

    settings_mode = SettingsMode::NONE;
    settings_cursor = SettingsCursor::BRIGHTNESS;
    rf_select_active = false;
    rf_edit_mode = false;
    rf_page_dirty = false;
    rf_cursor = RfCursor::POWER;
    lv_disp_load_scr(ui_main);
    exit_edit_session();
    apply_main_screen_overrides();
}

void open_settings_page() {
    if (!ui_SettingPAGE) {
        return;
    }

    exit_edit_session();
    settings_mode = SettingsMode::NONE;
    settings_cursor = SettingsCursor::BRIGHTNESS;
    info_preselect_active = false;
    rf_select_active = false;
    rf_edit_mode = false;
    rf_page_dirty = false;
    rf_cursor = RfCursor::POWER;
    refresh_settings_page_widgets();
    lv_disp_load_scr(ui_SettingPAGE);
    refresh_settings_page_widgets();
}

void move_settings_cursor(int32_t delta) {
    if (!settings_in_select_mode() || delta == 0) {
        return;
    }

    int32_t cursor = static_cast<int32_t>(settings_cursor) + delta;
    constexpr int32_t SETTINGS_ITEM_COUNT = 5;
    cursor %= SETTINGS_ITEM_COUNT;
    if (cursor < 0) {
        cursor += SETTINGS_ITEM_COUNT;
    }

    settings_cursor = static_cast<SettingsCursor>(cursor);
    refresh_settings_selection_state();
}

void step_backlight(int32_t delta) {
    if (!settings_in_edit_mode() || settings_cursor != SettingsCursor::BRIGHTNESS || delta == 0) {
        return;
    }

    int32_t next = static_cast<int32_t>(backlight_pwm) + delta * BACKLIGHT_PWM_STEP;
    if (next < static_cast<int32_t>(device_config::BACKLIGHT_PWM_MIN)) {
        next = device_config::BACKLIGHT_PWM_MIN;
    }
    if (next > static_cast<int32_t>(device_config::BACKLIGHT_PWM_MAX)) {
        next = device_config::BACKLIGHT_PWM_MAX;
    }

    apply_backlight_pwm(static_cast<uint8_t>(next), true);
}

void toggle_ble_from_settings() {
    const bool target = !connectivity_manager_is_ble_enabled();
    connectivity_manager_set_ble_enabled(target, false);
    refresh_settings_ble_widgets();
}

void toggle_radio_power_from_rf_page() {
    set_radio_power_high(!radio_power_high, true, true);
}

void toggle_rf_guard_from_rf_page() {
    rf_guard_enabled = !rf_guard_enabled;
    save_radio_config_immediately(true);
    refresh_rf_page_widgets();
}

void move_rf_cursor(int32_t delta) {
    if (!rf_select_active || rf_edit_mode || delta == 0) {
        return;
    }

    int32_t cursor = static_cast<int32_t>(rf_cursor) + delta;
    constexpr int32_t RF_ITEM_COUNT = 5;
    cursor %= RF_ITEM_COUNT;
    if (cursor < 0) {
        cursor += RF_ITEM_COUNT;
    }
    rf_cursor = static_cast<RfCursor>(cursor);
    refresh_rf_page_widgets();
}

void step_rf_guard_value(int32_t delta) {
    if (!rf_select_active || !rf_edit_mode || delta == 0) {
        return;
    }

    switch (rf_cursor) {
        case RfCursor::SINGLE_LIMIT: {
            int32_t value = static_cast<int32_t>(rf_guard_single_tx_limit_s) + delta;
            value = std::max<int32_t>(device_config::RF_GUARD_SINGLE_TX_LIMIT_MIN_S, value);
            value = std::min<int32_t>(device_config::RF_GUARD_SINGLE_TX_LIMIT_MAX_S, value);
            rf_guard_single_tx_limit_s = static_cast<uint16_t>(value);
            rf_page_dirty = true;
            break;
        }
        case RfCursor::WINDOW_SIZE: {
            int32_t value = static_cast<int32_t>(rf_guard_window_s) + delta;
            value = std::max<int32_t>(device_config::RF_GUARD_WINDOW_MIN_S, value);
            value = std::min<int32_t>(device_config::RF_GUARD_WINDOW_MAX_S, value);
            rf_guard_window_s = static_cast<uint16_t>(value);
            if (rf_guard_max_tx_in_window_s > rf_guard_window_s) {
                rf_guard_max_tx_in_window_s = rf_guard_window_s;
            }
            rf_page_dirty = true;
            break;
        }
        case RfCursor::WINDOW_MAX_TX: {
            int32_t value = static_cast<int32_t>(rf_guard_max_tx_in_window_s) + delta;
            value = std::max<int32_t>(device_config::RF_GUARD_MAX_TX_IN_WINDOW_MIN_S, value);
            value = std::min<int32_t>(static_cast<int32_t>(rf_guard_window_s), value);
            rf_guard_max_tx_in_window_s = static_cast<uint16_t>(value);
            rf_page_dirty = true;
            break;
        }
        case RfCursor::POWER:
        case RfCursor::GUARD_ENABLE:
        default:
            break;
    }

    refresh_rf_page_widgets();
}

void commit_rf_guard_edit_if_needed() {
    if (!rf_page_dirty) {
        return;
    }
    rf_page_dirty = false;
    save_radio_config_immediately(true);
}

void on_rf_button_click() {
    if (!rf_select_active) {
        rf_select_active = true;
        rf_edit_mode = false;
        rf_cursor = RfCursor::POWER;
        refresh_rf_page_widgets();
        return;
    }

    if (rf_edit_mode) {
        rf_edit_mode = false;
        commit_rf_guard_edit_if_needed();
        refresh_rf_page_widgets();
        return;
    }

    switch (rf_cursor) {
        case RfCursor::POWER:
            toggle_radio_power_from_rf_page();
            break;
        case RfCursor::GUARD_ENABLE:
            toggle_rf_guard_from_rf_page();
            break;
        case RfCursor::SINGLE_LIMIT:
        case RfCursor::WINDOW_SIZE:
        case RfCursor::WINDOW_MAX_TX:
            rf_edit_mode = true;
            refresh_rf_page_widgets();
            break;
        default:
            break;
    }
}

void on_settings_button_click() {
    switch (settings_mode) {
        case SettingsMode::NONE:
            settings_mode = SettingsMode::SELECT;
            settings_cursor = SettingsCursor::BRIGHTNESS;
            refresh_settings_selection_state();
            break;
        case SettingsMode::SELECT:
            switch (settings_cursor) {
                case SettingsCursor::BRIGHTNESS:
                    settings_mode = SettingsMode::EDIT;
                    refresh_settings_selection_state();
                    break;
                case SettingsCursor::BLE:
                    toggle_ble_from_settings();
                    break;
                case SettingsCursor::AUTOSTART:
                    toggle_autostart_from_settings();
                    break;
                case SettingsCursor::RF:
                    settings_mode = SettingsMode::NONE;
                    open_rf_page();
                    break;
                case SettingsCursor::ABOUT:
                    settings_mode = SettingsMode::NONE;
                    open_info_page();
                    break;
                default:
                    break;
            }
            break;
        case SettingsMode::EDIT:
            settings_mode = SettingsMode::SELECT;
            refresh_settings_selection_state();
            break;
        default:
            break;
    }
}

void on_rf_key0_short_press() {
    if (rf_edit_mode) {
        rf_edit_mode = false;
        commit_rf_guard_edit_if_needed();
        refresh_rf_page_widgets();
        return;
    }

    if (rf_select_active) {
        rf_select_active = false;
        rf_cursor = RfCursor::POWER;
        refresh_rf_page_widgets();
        return;
    }

    close_rf_page_to_settings();
}

void on_settings_key0_short_press() {
    if (settings_mode == SettingsMode::EDIT) {
        settings_mode = SettingsMode::SELECT;
        refresh_settings_selection_state();
        return;
    }

    if (settings_mode == SettingsMode::SELECT) {
        settings_mode = SettingsMode::NONE;
        refresh_settings_selection_state();
        return;
    }

    close_settings_page_to_main();
}

void on_info_button_click() {
    info_preselect_active = !info_preselect_active;
    refresh_info_panel_preselect_state();
}

void on_info_key0_short_press() {
    if (info_preselect_active) {
        info_preselect_active = false;
        refresh_info_panel_preselect_state();
        return;
    }

    close_info_page_to_settings();
}

void advance_to_next_stage() {
    switch (edit_stage) {
        case EditStage::TX_FREQ:
            set_edit_stage(EditStage::RX_FREQ);
            break;
        case EditStage::RX_FREQ:
            set_edit_stage(EditStage::TX_TONE);
            break;
        case EditStage::TX_TONE:
            set_edit_stage(EditStage::RX_TONE);
            break;
        case EditStage::RX_TONE:
            set_edit_stage(EditStage::RX_SQL);
            break;
        case EditStage::RX_SQL:
            set_edit_stage(EditStage::BAND);
            break;
        case EditStage::BAND:
            // Leave SA818 settings: persist and then apply to module.
            save_and_apply_radio_config();
            set_edit_stage(EditStage::SETTINGS);
            break;
        case EditStage::SETTINGS:
        default:
            exit_edit_session();
            break;
    }
}

void toggle_frequency_edit_mode() {
    if (!is_editing_session_active()) {
        return;
    }
    if (!is_frequency_stage()) {
        return;
    }

    edit_mode = (edit_mode == EditMode::SELECT) ? EditMode::EDIT : EditMode::SELECT;
    refresh_edit_widgets();
}

void step_frequency_digit(int32_t delta) {
    if (!is_frequency_stage() || !is_edit_mode() || delta == 0) {
        return;
    }

    const uint32_t step = digit_step_value(editing_digit_index);
    if (step == 0) {
        return;
    }

    uint32_t &active_frequency = get_active_frequency_value();
    const int64_t candidate = static_cast<int64_t>(active_frequency) + static_cast<int64_t>(delta) * step;
    active_frequency = clamp_frequency_range(candidate);
    radio_cfg_dirty = true;

    render_frequency_label();
}

void move_selected_digit(int32_t delta) {
    if (!is_frequency_stage() || !is_select_mode() || delta == 0) {
        return;
    }

    int32_t next_index = static_cast<int32_t>(editing_digit_index) + delta;
    next_index %= FREQ_EDITABLE_DIGITS;
    if (next_index < 0) {
        next_index += FREQ_EDITABLE_DIGITS;
    }

    editing_digit_index = static_cast<int8_t>(next_index);
    render_frequency_label();
}

void step_subaudio_option(int32_t delta) {
    if (!is_tone_stage() || !is_edit_mode() || delta == 0) {
        return;
    }

    int32_t option = static_cast<int32_t>(subaudio_to_option(get_active_subaudio_setting()));
    option += delta;

    const int32_t count = static_cast<int32_t>(SUBAUDIO_OPTION_COUNT);
    option %= count;
    if (option < 0) {
        option += count;
    }

    get_active_subaudio_setting() = option_to_subaudio(static_cast<size_t>(option));
    radio_cfg_dirty = true;
    refresh_edit_widgets();
}

void step_squelch(int32_t delta) {
    if (!is_sql_stage() || !is_edit_mode() || delta == 0) {
        return;
    }

    int32_t sql = static_cast<int32_t>(radio_squelch);
    sql += delta;

    constexpr int32_t SQ_COUNT = 9;
    sql %= SQ_COUNT;
    if (sql < 0) {
        sql += SQ_COUNT;
    }

    radio_squelch = static_cast<SA818Squelch>(sql);
    radio_cfg_dirty = true;
    refresh_edit_widgets();
}

void step_band(int32_t delta) {
    if (!is_band_stage() || !is_edit_mode() || delta == 0) {
        return;
    }

    // Any rotate toggles between narrow(12.5kHz) and wide(25kHz).
    radio_wide_band = !radio_wide_band;
    radio_cfg_dirty = true;
    refresh_edit_widgets();
}

void apply_main_screen_overrides() {
    hide_header_update_indicators();
}

void hide_header_update_indicators() {
    if (ui_hasNewUpdate) {
        lv_obj_add_flag(ui_hasNewUpdate, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_hasNewUpdate1) {
        lv_obj_add_flag(ui_hasNewUpdate1, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_hasNewUpdate2) {
        lv_obj_add_flag(ui_hasNewUpdate2, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_hasNewUpdate3) {
        lv_obj_add_flag(ui_hasNewUpdate3, LV_OBJ_FLAG_HIDDEN);
    }
}
} // namespace

void edit_controller_init() {
    load_radio_config_from_storage();
    backlight_pwm = device_config::load_backlight_pwm();
    auto_start_enabled = device_config::load_auto_start_enabled();
    screen_locked = false;
    radio_cfg_dirty = false;
    log_radio_config("LOADED");
    set_edit_stage(EditStage::NONE);
    refresh_settings_page_widgets();
    refresh_rf_page_widgets();
}

void edit_controller_on_enter_main_screen() {
    apply_main_screen_overrides();
    settings_mode = SettingsMode::NONE;
    settings_cursor = SettingsCursor::BRIGHTNESS;
    info_preselect_active = false;
    rf_select_active = false;
    rf_edit_mode = false;
    rf_cursor = RfCursor::POWER;
    net_bridge_active = false;
    net_bridge_source_text[0] = '\0';
    rf_overload_active = false;
    exit_edit_session();
}

bool edit_controller_boot_radio_init() {
    if (!sa818_init(SA818Type::SA818_UHF)) {
        Serial.println("SA818 init failed.");
        return false;
    }

    // Required boot sequence: EN high -> wait 500ms -> send AT probe.
    sa818_enable(true);
    delay(500);

    const bool at_ok = sa818_is_connected();
    if (!at_ok) {
        Serial.println("SA818 AT probe failed.");
        return false;
    }

    sql_rx_active = sa818_is_rx();

    const bool cfg_ok = apply_all_radio_config_to_sa818();
    if (!cfg_ok) {
        Serial.println("SA818 initial config apply failed.");
    } else {
        log_radio_config("BOOT_APPLY");
    }
    return cfg_ok;
}

void edit_controller_update() {
    const uint32_t now_ms = millis();
    const ActiveScreen screen = get_active_screen();

    if (screen == ActiveScreen::SETTINGS) {
        if (last_settings_ble_refresh_ms == 0 || (now_ms - last_settings_ble_refresh_ms) >= 250) {
            last_settings_ble_refresh_ms = now_ms;
            refresh_settings_ble_widgets();
        }
    } else {
        last_settings_ble_refresh_ms = 0;
    }

    if (screen == ActiveScreen::INFO) {
        if (last_info_refresh_ms == 0 || (now_ms - last_info_refresh_ms) >= 1000) {
            last_info_refresh_ms = now_ms;
            refresh_info_page_content();
        }
    } else {
        last_info_refresh_ms = 0;
    }

    if (screen == ActiveScreen::RF) {
        refresh_rf_page_widgets();
    }

    if (!sa818_is_enabled()) {
        return;
    }

    if (sa818_is_tx()) {
        return;
    }

    const bool current_sql_state = sa818_is_rx();
    if (current_sql_state == sql_rx_active) {
        return;
    }

    sql_rx_active = current_sql_state;
    if (!is_editing_session_active()) {
        refresh_edit_widgets();
    }
}

void edit_controller_on_encoder_event(EC11Event event, int32_t value) {
    if (screen_locked) {
        if (event == EC11Event::BUTTON_CLICK) {
            apply_screen_lock(false);
        }
        return;
    }

    if (is_power_popup_visible_internal()) {
        switch (event) {
            case EC11Event::ROTATE_CW:
                move_power_popup_cursor((value == 0) ? 1 : value);
                break;
            case EC11Event::ROTATE_CCW:
                move_power_popup_cursor(-((value == 0) ? 1 : value));
                break;
            case EC11Event::BUTTON_CLICK:
                execute_power_popup_action();
                break;
            default:
                break;
        }
        return;
    }

    if (connectivity_manager_is_ble_popup_visible() || net_audio_link_is_bind_popup_visible()) {
        return;
    }

    const ActiveScreen screen = get_active_screen();

    if (screen == ActiveScreen::SETTINGS) {
        switch (event) {
            case EC11Event::ROTATE_CW:
                if (settings_in_edit_mode()) {
                    step_backlight((value == 0) ? 1 : value);
                } else if (settings_in_select_mode()) {
                    move_settings_cursor((value == 0) ? 1 : value);
                }
                break;
            case EC11Event::ROTATE_CCW:
                if (settings_in_edit_mode()) {
                    step_backlight(-((value == 0) ? 1 : value));
                } else if (settings_in_select_mode()) {
                    move_settings_cursor(-((value == 0) ? 1 : value));
                }
                break;
            case EC11Event::BUTTON_CLICK:
                on_settings_button_click();
                break;
            default:
                break;
        }
        return;
    }

    if (screen == ActiveScreen::INFO) {
        if (event == EC11Event::BUTTON_CLICK) {
            on_info_button_click();
        }
        return;
    }

    if (screen == ActiveScreen::RF) {
        switch (event) {
            case EC11Event::ROTATE_CW: {
                const int32_t step = (value == 0) ? 1 : value;
                if (rf_edit_mode) {
                    step_rf_guard_value(step);
                } else {
                    move_rf_cursor(step);
                }
                break;
            }
            case EC11Event::ROTATE_CCW: {
                const int32_t step = (value == 0) ? 1 : value;
                if (rf_edit_mode) {
                    step_rf_guard_value(-step);
                } else {
                    move_rf_cursor(-step);
                }
                break;
            }
            case EC11Event::BUTTON_CLICK:
                on_rf_button_click();
                break;
            default:
                break;
        }
        return;
    }

    if (screen != ActiveScreen::MAIN) {
        return;
    }

    switch (event) {
        case EC11Event::ROTATE_CW: {
            const int32_t step = (value == 0) ? 1 : value;
            if (is_frequency_stage()) {
                if (is_edit_mode()) {
                    step_frequency_digit(step);
                } else {
                    move_selected_digit(step);
                }
            } else if (is_tone_stage()) {
                step_subaudio_option(step);
            } else if (is_sql_stage()) {
                step_squelch(step);
            } else if (is_band_stage()) {
                step_band(step);
            }
            break;
        }
        case EC11Event::ROTATE_CCW: {
            const int32_t step = (value == 0) ? 1 : value;
            if (is_frequency_stage()) {
                if (is_edit_mode()) {
                    step_frequency_digit(-step);
                } else {
                    move_selected_digit(-step);
                }
            } else if (is_tone_stage()) {
                step_subaudio_option(-step);
            } else if (is_sql_stage()) {
                step_squelch(-step);
            } else if (is_band_stage()) {
                step_band(-step);
            }
            break;
        }
        case EC11Event::BUTTON_CLICK:
            if (!is_editing_session_active()) {
                enter_edit_session();
            } else if (is_frequency_stage()) {
                toggle_frequency_edit_mode();
            } else if (edit_stage == EditStage::SETTINGS) {
                open_settings_page();
            }
            break;
        case EC11Event::BUTTON_LONG_PRESS:
            // Stage switching now uses KEY0 short press.
            break;
        default:
            break;
    }
}

void edit_controller_on_key0_short_press() {
    if (screen_locked) {
        apply_screen_lock(false);
        return;
    }

    if (is_power_popup_visible_internal()) {
        hide_power_popup_internal();
        return;
    }

    if (connectivity_manager_is_ble_popup_visible() || net_audio_link_is_bind_popup_visible()) {
        return;
    }

    const ActiveScreen screen = get_active_screen();

    if (screen == ActiveScreen::SETTINGS) {
        on_settings_key0_short_press();
        return;
    }

    if (screen == ActiveScreen::INFO) {
        on_info_key0_short_press();
        return;
    }

    if (screen == ActiveScreen::RF) {
        on_rf_key0_short_press();
        return;
    }

    if (screen != ActiveScreen::MAIN) {
        return;
    }

    if (is_editing_session_active()) {
        advance_to_next_stage();
    }
}

void edit_controller_on_key0_long_press() {
    if (screen_locked) {
        apply_screen_lock(false);
        return;
    }

    if (is_power_popup_visible_internal()) {
        return;
    }

    const ActiveScreen screen = get_active_screen();
    if (screen != ActiveScreen::MAIN) {
        return;
    }

    if (is_editing_session_active()) {
        return;
    }

    show_power_popup();
}

void edit_controller_get_radio_config(device_config::RadioConfig &config) {
    config = build_radio_config_snapshot();
}

bool edit_controller_set_radio_config(const device_config::RadioConfig &config, bool persist) {
    if (sa818_is_tx()) {
        Serial.println("External radio config apply blocked while RF TX is active.");
        return false;
    }

    apply_runtime_radio_config(config);
    radio_cfg_dirty = !persist;

    if (persist) {
        if (!save_radio_config_to_storage()) {
            Serial.println("External radio config save failed.");
            log_radio_config("EXT_SAVE_FAIL");
            refresh_edit_widgets();
            refresh_rf_page_widgets();
            return false;
        }
        radio_cfg_dirty = false;
    }

    bool ok = true;
    if (sa818_is_enabled()) {
        ok = apply_all_radio_config_to_sa818();
        if (!ok) {
            Serial.println("External radio config apply failed.");
        }
    }

    log_radio_config("EXT_APPLY");
    refresh_edit_widgets();
    refresh_rf_page_widgets();
    return ok;
}

void edit_controller_set_network_bridge_active(bool active) {
    if (net_bridge_active == active) {
        return;
    }

    net_bridge_active = active;
    refresh_edit_widgets();
}

void edit_controller_set_network_bridge_source(const char *call_sign, uint8_t ssid) {
    char rendered[NETWORK_BRIDGE_SOURCE_TEXT_MAX] = {0};
    format_network_bridge_source_text(call_sign, ssid, rendered, sizeof(rendered));

    if (strncmp(net_bridge_source_text, rendered, sizeof(net_bridge_source_text)) == 0) {
        return;
    }

    strncpy(net_bridge_source_text, rendered, sizeof(net_bridge_source_text) - 1);
    net_bridge_source_text[sizeof(net_bridge_source_text) - 1] = '\0';
    if (net_bridge_active) {
        refresh_edit_widgets();
    }
}

void edit_controller_set_rf_overload_active(bool active) {
    if (rf_overload_active == active) {
        return;
    }

    rf_overload_active = active;
    refresh_edit_widgets();
}

void edit_controller_hide_power_popup() {
    hide_power_popup_internal();
}

bool edit_controller_is_power_popup_visible() {
    return is_power_popup_visible_internal();
}

bool edit_controller_is_editing() {
    return is_editing_session_active();
}
