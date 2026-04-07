#include "edit_controller.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../drivers/sa818_driver.h"
#include "../ui/ui.h"
#include "device_config.h"

namespace {
constexpr uint32_t FREQ_MIN_X10000 = 4000000;  // 400.0000 MHz
constexpr uint32_t FREQ_MAX_X10000 = 4700000;  // 470.0000 MHz
constexpr int8_t FREQ_EDITABLE_DIGITS = 7;

constexpr uint32_t INFO_PANEL_COLOR_IDLE = 0x006EC7;
constexpr uint32_t INFO_PANEL_COLOR_TX = 0xB71C1C;
constexpr uint32_t INFO_PANEL_COLOR_RX = 0x2E7D32;
constexpr uint32_t HIGHLIGHT_COLOR_SELECT = 0xFF6A00;
constexpr uint32_t HIGHLIGHT_COLOR_EDIT = 0xFF6A00;

constexpr char DEFAULT_INFO_TEXT[] = "BH5UVN";

enum class EditStage {
    NONE = 0,
    TX_FREQ,
    RX_FREQ,
    TX_TONE,
    RX_TONE,
    RX_SQL,
    SETTINGS,
};

enum class EditMode {
    NONE = 0,
    SELECT,
    EDIT,
};

using SubAudioType = device_config::SubAudioType;
using SubAudioSetting = device_config::SubAudioSetting;

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
constexpr size_t SUBAUDIO_OPTION_COUNT = 1 + CTCSS_TONE_COUNT + CDCSS_TONE_COUNT;

EditStage edit_stage = EditStage::NONE;
EditMode edit_mode = EditMode::NONE;

uint32_t tx_frequency_x10000 = 4395000;  // 439.5000 MHz
uint32_t rx_frequency_x10000 = 4385000;  // 438.5000 MHz
SubAudioSetting tx_subaudio = {SubAudioType::CTCSS, 7};  // 88.5
SubAudioSetting rx_subaudio = {SubAudioType::CTCSS, 7};  // 88.5
SA818Squelch radio_squelch = SA818Squelch::SQ_4;
bool radio_wide_band = false;

uint8_t digit_positions[16] = {};
uint8_t digit_count = 0;
int8_t editing_digit_index = -1;

bool sql_rx_active = false;
bool radio_cfg_dirty = false;
lv_obj_t *save_overlay = nullptr;

bool is_editing_session_active() {
    return edit_stage != EditStage::NONE;
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

SubAudioSetting sanitize_subaudio(SubAudioType type, uint8_t index) {
    switch (type) {
        case SubAudioType::OFF:
            return {SubAudioType::OFF, 0};
        case SubAudioType::CTCSS:
            return {SubAudioType::CTCSS, static_cast<uint8_t>(index % CTCSS_TONE_COUNT)};
        case SubAudioType::CDCSS_N:
            return {SubAudioType::CDCSS_N, static_cast<uint8_t>(index % CDCSS_TONE_COUNT)};
        default:
            return {SubAudioType::OFF, 0};
    }
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
            snprintf(buffer, buffer_len, "T%u", static_cast<unsigned int>(setting.index + 1));
            break;
        case SubAudioType::CDCSS_N:
            snprintf(buffer, buffer_len, "D%03uN", static_cast<unsigned int>(CDCSS_TONES[setting.index]));
            break;
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

    Serial.printf("[RADIO][%s] TX=%s RX=%s TXTone=%s RXTone=%s SQL=%u BW=%s dirty=%d\n",
                  tag ? tag : "LOG",
                  tx_freq,
                  rx_freq,
                  tx_tone,
                  rx_tone,
                  static_cast<unsigned int>(radio_squelch),
                  radio_wide_band ? "wide" : "narrow",
                  radio_cfg_dirty ? 1 : 0);
}

uint32_t get_display_frequency_value() {
    // In idle runtime, when RF is being received and forwarded to NET,
    // show RX frequency on the main big frequency label.
    if (!is_editing_session_active() && sql_rx_active) {
        return rx_frequency_x10000;
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
            snprintf(buffer, buffer_len, "D%03uN", static_cast<unsigned int>(CDCSS_TONES[idx]));
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
            if (setting.index >= CDCSS_TONE_COUNT) {
                return false;
            }
            return is_tx ? sa818_set_cdcss_tx(CDCSS_TONES[setting.index])
                         : sa818_set_cdcss_rx(CDCSS_TONES[setting.index]);
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
            if (setting.index < CDCSS_TONE_COUNT) {
                snprintf(buffer, buffer_len, "%03uN", static_cast<unsigned int>(CDCSS_TONES[setting.index]));
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

void show_save_overlay() {
    if (save_overlay || !lv_scr_act()) {
        return;
    }

    save_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(save_overlay);
    lv_obj_set_size(save_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(save_overlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(save_overlay, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(save_overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(save_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_overlay, LV_OBJ_FLAG_CLICKABLE);

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

    lv_obj_move_foreground(save_overlay);
    lv_obj_invalidate(save_overlay);
    lv_refr_now(lv_disp_get_default());
}

void hide_save_overlay() {
    if (!save_overlay) {
        return;
    }

    lv_obj_del(save_overlay);
    save_overlay = nullptr;
    lv_refr_now(lv_disp_get_default());
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
        case EditStage::SETTINGS:
            lv_obj_set_style_bg_color(ui_infoPanel, lv_color_hex(INFO_PANEL_COLOR_IDLE), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(ui_infoPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(ui_Info, "Settings");
            break;
        case EditStage::NONE:
        default:
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

void enter_edit_session() {
    set_edit_stage(EditStage::TX_FREQ);
}

void exit_edit_session() {
    set_edit_stage(EditStage::NONE);
}

void open_settings_page() {
    if (!ui_SettingPAGE) {
        return;
    }

    exit_edit_session();
    lv_disp_load_scr(ui_SettingPAGE);
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

void apply_main_screen_overrides() {
    if (ui_hasNewUpdate) {
        lv_obj_add_flag(ui_hasNewUpdate, LV_OBJ_FLAG_HIDDEN);
    }
}
} // namespace

void edit_controller_init() {
    load_radio_config_from_storage();
    radio_cfg_dirty = false;
    log_radio_config("LOADED");
    set_edit_stage(EditStage::NONE);
}

void edit_controller_on_enter_main_screen() {
    apply_main_screen_overrides();
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
    if (!sa818_is_enabled()) {
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
    if (!is_editing_session_active()) {
        return;
    }
    advance_to_next_stage();
}

void edit_controller_get_radio_config(device_config::RadioConfig &config) {
    config = build_radio_config_snapshot();
}

bool edit_controller_set_radio_config(const device_config::RadioConfig &config, bool persist) {
    apply_runtime_radio_config(config);
    radio_cfg_dirty = !persist;

    if (persist) {
        if (!save_radio_config_to_storage()) {
            Serial.println("External radio config save failed.");
            log_radio_config("EXT_SAVE_FAIL");
            refresh_edit_widgets();
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
    return ok;
}
