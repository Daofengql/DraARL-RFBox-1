#include "net_audio_link.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <opus.h>

#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "../drivers/es8388_driver.h"
#include "../drivers/i2c_driver.h"
#include "../drivers/i2s_driver.h"
#include "../drivers/sa818_driver.h"
#include "../ui/ui.h"
#include "app_logic.h"
#include "connectivity_manager.h"
#include "device_config.h"
#include "edit_controller.h"

namespace {
constexpr size_t DRA_HEADER_SIZE = 90;
constexpr size_t DRA_MAX_PACKET_SIZE = 800;
constexpr uint8_t DRA_VERSION[4] = {'D', 'r', 'a', 'A'};

constexpr uint8_t TYPE_HEARTBEAT = 2;
constexpr uint8_t TYPE_CONFIG = 3;
constexpr uint8_t TYPE_OPUS_16K = 5;
constexpr uint8_t TYPE_SERVER_VOICE = 6;

constexpr uint8_t DEV_MODEL_ESP32 = 1;
constexpr size_t HEARTBEAT_GPS_PAYLOAD_SIZE = 24;
constexpr size_t HEARTBEAT_MAC_TEXT_SIZE = 17;
constexpr uint8_t HEARTBEAT_STATUS_SUCCESS = 0;
constexpr uint8_t HEARTBEAT_STATUS_DEVICE_CONFLICT_ONLINE = 1;
constexpr uint8_t HEARTBEAT_STATUS_RESERVED_SSID = 2;
constexpr uint8_t HEARTBEAT_STATUS_AUTH_FAILED = 3;

constexpr uint32_t OPUS_SAMPLE_RATE = 16000;
constexpr uint8_t OPUS_CHANNELS = 1;
constexpr int OPUS_FRAME_SAMPLES_MAX = 1920;  // 120ms @ 16kHz
constexpr int OPUS_TX_FRAME_SAMPLES = 960;    // 60ms @ 16kHz
constexpr uint8_t OPUS_TX_FRAMES_PER_PACKET = 2;  // server usually expects two 60ms frames merged
constexpr size_t OPUS_TX_MAX_BYTES = 400;
constexpr size_t OPUS_TX_MERGED_MAX_BYTES = DRA_MAX_PACKET_SIZE - DRA_HEADER_SIZE;
constexpr int OPUS_BITRATE = 24000;
constexpr int OPUS_COMPLEXITY = 1;
constexpr int RT_PCM_WORK_SAMPLES = OPUS_FRAME_SAMPLES_MAX * 2;

constexpr uint32_t HTTP_RETRY_MS = 5000;
constexpr uint32_t BIND_POLL_MS = 5000;
constexpr uint32_t DYNAMIC_CODE_REFRESH_MS = 55000;
constexpr uint32_t REQUEST_CODE_RETRY_MS = 60000;
constexpr uint32_t RESOLVE_RETRY_MS = 3000;
constexpr uint32_t AUTH_TIMEOUT_MS = 3000;
constexpr uint8_t AUTH_RETRY_LIMIT = 3;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 5000;
constexpr uint32_t CONFIG_SYNC_RETRY_MS = 1000;
constexpr uint32_t VOICE_TX_TIMEOUT_MS = 600;
constexpr uint32_t VOICE_BRIDGE_PTT_SETTLE_MS = 20;
constexpr uint32_t RF_GUARD_RECOVER_MIN_BUDGET_MS = 2000;
constexpr uint32_t RF_GUARD_NEXT_BURST_GAP_MS = 3000;
constexpr bool AUDIO_STATS_LOG_ENABLED = false;
constexpr uint32_t AUDIO_STATS_LOG_INTERVAL_MS = 5000;
constexpr uint32_t AUDIO_ERROR_LOG_INTERVAL_MS = 2000;

constexpr uint8_t CONFIG_CMD_QUERY = 0x01;
constexpr uint8_t CONFIG_CMD_APPLY = 0x02;
constexpr uint8_t CONFIG_CMD_TIME = 0x03;

constexpr uint8_t CONFIG_TLV_RX_FREQ = 0x01;
constexpr uint8_t CONFIG_TLV_TX_FREQ = 0x02;
constexpr uint8_t CONFIG_TLV_RX_CTCSS = 0x03;
constexpr uint8_t CONFIG_TLV_TX_CTCSS = 0x04;
constexpr uint8_t CONFIG_TLV_SQL_LEVEL = 0x05;
constexpr uint8_t CONFIG_TLV_POWER_LEVEL = 0x06;
constexpr uint8_t CONFIG_TLV_TX_BANDWIDTH = 0x07;
constexpr uint8_t CONFIG_TLV_RX_TONE_MODE = 0x08;
constexpr uint8_t CONFIG_TLV_RX_TONE_VALUE = 0x09;
constexpr uint8_t CONFIG_TLV_TX_TONE_MODE = 0x0A;
constexpr uint8_t CONFIG_TLV_TX_TONE_VALUE = 0x0B;
constexpr uint8_t CONFIG_TLV_TIMESTAMP = 0x10;
constexpr int64_t VALID_TIME_SYNC_MIN_MS = 946684800000LL;   // 2000-01-01T00:00:00Z
constexpr int64_t VALID_TIME_SYNC_MAX_MS = 4102444800000LL;  // 2100-01-01T00:00:00Z

constexpr float CTCSS_TONES[] = {
    67.0f,  71.9f,  74.4f,  77.0f,  79.7f,  82.5f,  85.4f,  88.5f,  91.5f,  94.8f,
    97.4f,  100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f,
    136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 162.2f, 167.9f, 173.8f, 179.9f, 186.2f,
    192.8f, 203.5f, 210.7f, 218.1f, 225.7f, 233.6f, 241.8f, 250.3f
};

constexpr uint16_t CDCSS_CODES[] = {
    23,  25,  26,  31,  32,  43,  47,  51,  54,  65,  71,  72,  73,  74,  114, 115, 116,
    125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244, 245,
    251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413,
    423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546, 565, 606, 612, 624, 627, 631,
    632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754
};

constexpr size_t CTCSS_TONE_COUNT = sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]);
constexpr size_t CDCSS_CODE_COUNT = sizeof(CDCSS_CODES) / sizeof(CDCSS_CODES[0]);

enum class LinkState : uint8_t {
    IDLE = 0,
    WAIT_WIFI,
    PRECHECK,
    REQUEST_CODE,
    CONFIRM_BIND,
    RESOLVE_SERVER,
    UDP_AUTH_SEND,
    UDP_AUTH_WAIT,
    RUNNING,
};

enum class VoiceBridgeState : uint8_t {
    IDLE = 0,
    NET_TO_RF_ACTIVE,
};

enum class PrecheckResult : uint8_t {
    RETRY = 0,
    AUTHENTICATED,
    NEED_BIND,
};

enum class ConfirmBindResult : uint8_t {
    RETRY = 0,
    WAITING,
    READY,
};

struct DraPacketHeader {
    uint8_t type;
    uint8_t dev_model;
    uint8_t ssid;
    uint32_t dmr_id;
    char username[33];
    char call_sign[33];
};

struct AudioRxStats {
    uint32_t packets_type5 = 0;
    uint32_t packets_type6 = 0;
    uint32_t audio_payload_bytes = 0;
    uint32_t decoded_frames_ok = 0;
    uint32_t decode_fail = 0;
    uint32_t i2s_fail = 0;
    uint32_t audio_not_ready = 0;
    uint32_t merged_parse_fail = 0;
    uint32_t server_voice_short = 0;
    uint32_t rf_guard_drop = 0;
};

struct AudioTxStats {
    uint32_t frames_read_ok = 0;
    uint32_t packets_sent_ok = 0;
    uint32_t payload_bytes_sent = 0;
    uint32_t i2s_read_fail = 0;
    uint32_t encode_fail = 0;
    uint32_t udp_send_fail = 0;
};

struct RfGuardConfig {
    bool enabled = device_config::RF_GUARD_ENABLED_DEFAULT;
    uint16_t single_tx_limit_s = device_config::RF_GUARD_SINGLE_TX_LIMIT_DEFAULT_S;
    uint16_t window_s = device_config::RF_GUARD_WINDOW_DEFAULT_S;
    uint16_t max_tx_in_window_s = device_config::RF_GUARD_MAX_TX_IN_WINDOW_DEFAULT_S;
};

device_config::DeviceConfig g_config = {};
bool g_initialized = false;
bool g_main_screen_ready = false;
bool g_audio_ready = false;
bool g_i2c_ready = false;
bool g_codec_ready = false;
bool g_i2s_ready = false;
bool g_opus_ready = false;

LinkState g_state = LinkState::IDLE;
uint32_t g_next_action_at_ms = 0;
uint32_t g_last_heartbeat_at_ms = 0;
uint8_t g_auth_retry = 0;
bool g_radio_config_sync_pending = false;
uint32_t g_next_radio_config_sync_at_ms = 0;
bool g_config_sync_visible = false;
uint32_t g_config_sync_hide_at_ms = 0;

WiFiUDP g_udp;
bool g_udp_open = false;
IPAddress g_server_ip;
uint16_t g_server_port = 0;
uint16_t g_local_port = 0;

OpusEncoder *g_opus_encoder = nullptr;
OpusDecoder *g_opus_decoder = nullptr;

char g_dynamic_code[16] = {0};
uint32_t g_dynamic_code_issued_at_ms = 0;

VoiceBridgeState g_voice_state = VoiceBridgeState::IDLE;
uint32_t g_last_voice_packet_at_ms = 0;
AudioRxStats g_audio_rx_stats = {};
AudioTxStats g_audio_tx_stats = {};
uint32_t g_last_audio_stats_log_at_ms = 0;
uint32_t g_last_audio_not_ready_log_at_ms = 0;
uint32_t g_last_decode_fail_log_at_ms = 0;
uint32_t g_last_i2s_fail_log_at_ms = 0;
uint32_t g_last_i2s_read_fail_log_at_ms = 0;
uint32_t g_last_encode_fail_log_at_ms = 0;
uint32_t g_last_udp_send_fail_log_at_ms = 0;
RfGuardConfig g_rf_guard_cfg = {};
bool g_rf_guard_overload_active = false;
float g_rf_guard_budget_ms = 0.0f;
uint32_t g_rf_guard_last_refill_at_ms = 0;
uint32_t g_rf_guard_tx_started_at_ms = 0;
uint32_t g_rf_guard_tx_last_account_at_ms = 0;
uint32_t g_rf_guard_last_rx_packet_at_ms = 0;
bool g_rf_guard_wait_next_burst = false;
bool g_rf_guard_budget_recovered = false;
uint32_t g_rf_guard_trip_single_count = 0;
uint32_t g_rf_guard_trip_duty_count = 0;
const char *g_rf_guard_last_trip_reason = "none";

// Half-duplex runtime workspace:
// One packet buffer + one PCM buffer are time-division multiplexed by RX/TX/decode path.
uint8_t g_rt_packet_buffer[DRA_MAX_PACKET_SIZE] = {0};
int16_t g_rt_pcm_work[RT_PCM_WORK_SAMPLES] = {0};
int16_t g_tx_pcm_mono[OPUS_TX_FRAME_SAMPLES] = {0};
uint8_t g_tx_opus_buffer[OPUS_TX_MAX_BYTES] = {0};
uint8_t g_tx_merged_payload[OPUS_TX_MERGED_MAX_BYTES] = {0};
size_t g_tx_merged_payload_len = 0;
uint8_t g_tx_merged_frame_count = 0;

lv_obj_t *g_bind_popup = nullptr;
lv_obj_t *g_bind_code_label = nullptr;
lv_obj_t *g_bind_hint_label = nullptr;
bool g_bind_popup_visible = false;

void refresh_server_status_widgets();
void stop_voice_bridge();
void on_auth_failed();
void on_authenticated(const DraPacketHeader &header);

template <size_t N>
void copy_cstr(char (&dest)[N], const char *src) {
    if (N == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, N - 1);
    dest[N - 1] = '\0';
}

void copy_fixed_string(char *dest, size_t dest_len, const uint8_t *src, size_t src_len) {
    if (!dest || dest_len == 0 || !src || src_len == 0) return;
    const size_t copy_len = (src_len < (dest_len - 1)) ? src_len : (dest_len - 1);
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    for (size_t i = copy_len; i > 0; --i) {
        if (dest[i - 1] == '\0' || dest[i - 1] == ' ') {
            dest[i - 1] = '\0';
        } else {
            break;
        }
    }
}

uint64_t read_u64_be(const uint8_t *src) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(src[i]);
    }
    return value;
}

uint32_t read_u32_be(const uint8_t *src) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<uint32_t>(src[i]);
    }
    return value;
}

int64_t read_i64_be(const uint8_t *src) {
    return static_cast<int64_t>(read_u64_be(src));
}

bool is_plausible_unix_time_ms(int64_t unix_ms) {
    return unix_ms >= VALID_TIME_SYNC_MIN_MS && unix_ms <= VALID_TIME_SYNC_MAX_MS;
}

bool try_parse_time_sync_ms(const uint8_t *data, size_t data_len, int64_t &unix_ms) {
    if (!data || data_len != 10 || data[0] != CONFIG_CMD_TIME) {
        return false;
    }

    const int64_t parsed_value = read_i64_be(data + 2);
    if (!is_plausible_unix_time_ms(parsed_value)) {
        return false;
    }

    unix_ms = parsed_value;
    return true;
}

void write_u64_be(uint8_t *dest, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        dest[i] = static_cast<uint8_t>(value & 0xFFU);
        value >>= 8;
    }
}

void write_u32_be(uint8_t *dest, uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        dest[i] = static_cast<uint8_t>(value & 0xFFU);
        value >>= 8;
    }
}

float read_float_be(const uint8_t *src) {
    const uint32_t raw = read_u32_be(src);
    float value = 0.0f;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

void write_float_be(uint8_t *dest, float value) {
    uint32_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    write_u32_be(dest, raw);
}

device_config::SubAudioSetting ctcss_setting_from_hz(float hz) {
    if (hz <= 0.0f) {
        return {device_config::SubAudioType::OFF, 0};
    }

    size_t best_index = 0;
    float best_diff = fabsf(CTCSS_TONES[0] - hz);
    for (size_t i = 1; i < CTCSS_TONE_COUNT; ++i) {
        const float diff = fabsf(CTCSS_TONES[i] - hz);
        if (diff < best_diff) {
            best_diff = diff;
            best_index = i;
        }
    }

    return {device_config::SubAudioType::CTCSS, static_cast<uint8_t>(best_index)};
}

float ctcss_hz_from_setting(const device_config::SubAudioSetting &setting) {
    if (setting.type != device_config::SubAudioType::CTCSS) {
        return 0.0f;
    }

    const size_t index = (setting.index < CTCSS_TONE_COUNT) ? setting.index : 0;
    return CTCSS_TONES[index];
}

struct ToneFieldParseState {
    bool mode_present = false;
    bool mode_valid = false;
    uint8_t mode = 0;
    bool value_present = false;
    bool value_valid = false;
    char value_text[9] = {0};
};

bool try_get_cdcss_index(uint16_t code, uint8_t &index) {
    for (size_t i = 0; i < CDCSS_CODE_COUNT; ++i) {
        if (CDCSS_CODES[i] == code) {
            index = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}

uint8_t tone_mode_from_setting(const device_config::SubAudioSetting &setting) {
    switch (setting.type) {
        case device_config::SubAudioType::CTCSS: return 1U;
        case device_config::SubAudioType::CDCSS_N: return 2U;
        case device_config::SubAudioType::CDCSS_I: return 3U;
        case device_config::SubAudioType::OFF:
        default: return 0U;
    }
}

void trim_ascii_inplace(char *text) {
    if (!text) return;

    char *start = text;
    while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && std::isspace(static_cast<unsigned char>(text[len - 1]))) {
        text[--len] = '\0';
    }
}

void parse_ascii_tlv_text(const uint8_t *src, size_t src_len, char *dest, size_t dest_len) {
    if (!src || !dest || dest_len == 0) return;

    size_t write_len = 0;
    for (size_t i = 0; i < src_len && write_len < (dest_len - 1); ++i) {
        const char ch = static_cast<char>(src[i]);
        if (ch == '\0') {
            break;
        }
        dest[write_len++] = ch;
    }
    dest[write_len] = '\0';
    trim_ascii_inplace(dest);
}

void format_tone_value_from_setting(const device_config::SubAudioSetting &setting, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) return;

    switch (setting.type) {
        case device_config::SubAudioType::CTCSS: {
            const float hz = ctcss_hz_from_setting(setting);
            if (hz <= 0.0f) {
                snprintf(buffer, buffer_len, "0");
            } else {
                snprintf(buffer, buffer_len, "%.1f", hz);
            }
            break;
        }
        case device_config::SubAudioType::CDCSS_N:
        case device_config::SubAudioType::CDCSS_I: {
            const size_t index = (setting.index < CDCSS_CODE_COUNT) ? setting.index : 0;
            snprintf(buffer, buffer_len, "%03u", static_cast<unsigned int>(CDCSS_CODES[index]));
            break;
        }
        case device_config::SubAudioType::OFF:
        default:
            snprintf(buffer, buffer_len, "0");
            break;
    }
}

void fill_tone_value_tlv(const device_config::SubAudioSetting &setting, uint8_t (&out)[8]) {
    memset(out, 0, sizeof(out));
    char text[12] = {0};
    format_tone_value_from_setting(setting, text, sizeof(text));
    const size_t text_len = strnlen(text, sizeof(text));
    const size_t copy_len = (text_len < sizeof(out)) ? text_len : sizeof(out);
    memcpy(out, text, copy_len);
}

device_config::SubAudioSetting parse_tone_setting(uint8_t mode, const char *value_text) {
    char text[16] = {0};
    if (value_text && value_text[0] != '\0') {
        strncpy(text, value_text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }
    trim_ascii_inplace(text);

    if (mode == 0) {
        return {device_config::SubAudioType::OFF, 0};
    }

    if (mode == 1) {
        char *end_ptr = nullptr;
        const float hz = strtof(text, &end_ptr);
        while (end_ptr && *end_ptr != '\0' && std::isspace(static_cast<unsigned char>(*end_ptr))) {
            ++end_ptr;
        }
        if (hz <= 0.0f || (end_ptr && *end_ptr != '\0')) {
            return {device_config::SubAudioType::OFF, 0};
        }
        return ctcss_setting_from_hz(hz);
    }

    if (mode == 2 || mode == 3) {
        const size_t text_len = strnlen(text, sizeof(text));
        uint8_t mode_from_suffix = 0;
        if (text_len > 1) {
            const char tail = static_cast<char>(std::toupper(static_cast<unsigned char>(text[text_len - 1])));
            if (tail == 'N' || tail == 'I') {
                mode_from_suffix = (tail == 'I') ? 3U : 2U;
                text[text_len - 1] = '\0';
            }
        }

        char digits[4] = {0};
        size_t digits_len = 0;
        for (size_t i = 0; text[i] != '\0' && digits_len < 3; ++i) {
            if (std::isdigit(static_cast<unsigned char>(text[i]))) {
                digits[digits_len++] = text[i];
            }
        }
        if (digits_len == 0) {
            return {device_config::SubAudioType::OFF, 0};
        }

        const uint16_t code = static_cast<uint16_t>(atoi(digits));
        uint8_t index = 0;
        if (!try_get_cdcss_index(code, index)) {
            return {device_config::SubAudioType::OFF, 0};
        }

        const uint8_t resolved_mode = (mode_from_suffix != 0) ? mode_from_suffix : mode;
        const device_config::SubAudioType type = (resolved_mode == 3U)
                                                     ? device_config::SubAudioType::CDCSS_I
                                                     : device_config::SubAudioType::CDCSS_N;
        return {type, index};
    }

    return {device_config::SubAudioType::OFF, 0};
}

bool apply_tone_field_update(const ToneFieldParseState &state,
                             const device_config::SubAudioSetting &base,
                             device_config::SubAudioSetting &out) {
    if (!state.mode_present && !state.value_present) {
        return false;
    }

    uint8_t mode = tone_mode_from_setting(base);
    char value_text[16] = {0};
    format_tone_value_from_setting(base, value_text, sizeof(value_text));

    if (state.mode_present) {
        if (!state.mode_valid) {
            mode = 0;
            snprintf(value_text, sizeof(value_text), "0");
        } else {
            mode = state.mode;
        }
    }

    if (state.value_present) {
        if (!state.value_valid) {
            mode = 0;
            snprintf(value_text, sizeof(value_text), "0");
        } else {
            strncpy(value_text, state.value_text, sizeof(value_text) - 1);
            value_text[sizeof(value_text) - 1] = '\0';
        }
    }

    out = parse_tone_setting(mode, value_text);
    return true;
}

uint8_t power_level_from_radio_config(const device_config::RadioConfig &config) {
    return config.power_high ? 3U : 1U;
}

bool power_high_from_level(uint8_t power_level) {
    return power_level >= 2;
}

bool radio_config_equals(const device_config::RadioConfig &lhs, const device_config::RadioConfig &rhs) {
    return lhs.tx_frequency_x10000 == rhs.tx_frequency_x10000 &&
           lhs.rx_frequency_x10000 == rhs.rx_frequency_x10000 &&
           lhs.tx_subaudio.type == rhs.tx_subaudio.type &&
           lhs.tx_subaudio.index == rhs.tx_subaudio.index &&
           lhs.rx_subaudio.type == rhs.rx_subaudio.type &&
           lhs.rx_subaudio.index == rhs.rx_subaudio.index &&
           lhs.squelch == rhs.squelch &&
           lhs.wide_band == rhs.wide_band &&
           lhs.power_high == rhs.power_high &&
           lhs.rf_guard_enabled == rhs.rf_guard_enabled &&
           lhs.rf_guard_single_tx_limit_s == rhs.rf_guard_single_tx_limit_s &&
           lhs.rf_guard_window_s == rhs.rf_guard_window_s &&
           lhs.rf_guard_max_tx_in_window_s == rhs.rf_guard_max_tx_in_window_s;
}

bool rf_guard_config_equals(const RfGuardConfig &lhs, const RfGuardConfig &rhs) {
    return lhs.enabled == rhs.enabled &&
           lhs.single_tx_limit_s == rhs.single_tx_limit_s &&
           lhs.window_s == rhs.window_s &&
           lhs.max_tx_in_window_s == rhs.max_tx_in_window_s;
}

void normalize_rf_guard_config(RfGuardConfig &cfg) {
    if (cfg.single_tx_limit_s < device_config::RF_GUARD_SINGLE_TX_LIMIT_MIN_S ||
        cfg.single_tx_limit_s > device_config::RF_GUARD_SINGLE_TX_LIMIT_MAX_S) {
        cfg.single_tx_limit_s = device_config::RF_GUARD_SINGLE_TX_LIMIT_DEFAULT_S;
    }

    if (cfg.window_s < device_config::RF_GUARD_WINDOW_MIN_S ||
        cfg.window_s > device_config::RF_GUARD_WINDOW_MAX_S) {
        cfg.window_s = device_config::RF_GUARD_WINDOW_DEFAULT_S;
    }

    if (cfg.max_tx_in_window_s < device_config::RF_GUARD_MAX_TX_IN_WINDOW_MIN_S ||
        cfg.max_tx_in_window_s > cfg.window_s) {
        cfg.max_tx_in_window_s = device_config::RF_GUARD_MAX_TX_IN_WINDOW_DEFAULT_S;
    }
    if (cfg.max_tx_in_window_s > cfg.window_s) {
        cfg.max_tx_in_window_s = cfg.window_s;
    }
}

RfGuardConfig read_rf_guard_config_from_runtime() {
    device_config::RadioConfig radio_cfg = {};
    edit_controller_get_radio_config(radio_cfg);

    RfGuardConfig cfg = {};
    cfg.enabled = radio_cfg.rf_guard_enabled;
    cfg.single_tx_limit_s = radio_cfg.rf_guard_single_tx_limit_s;
    cfg.window_s = radio_cfg.rf_guard_window_s;
    cfg.max_tx_in_window_s = radio_cfg.rf_guard_max_tx_in_window_s;
    normalize_rf_guard_config(cfg);
    return cfg;
}

float rf_guard_budget_capacity_ms(const RfGuardConfig &cfg) {
    return static_cast<float>(cfg.max_tx_in_window_s) * 1000.0f;
}

float rf_guard_refill_rate_per_ms(const RfGuardConfig &cfg) {
    const float window_ms = static_cast<float>(cfg.window_s) * 1000.0f;
    if (window_ms <= 0.0f) {
        return 0.0f;
    }
    return rf_guard_budget_capacity_ms(cfg) / window_ms;
}

uint32_t rf_guard_recover_threshold_ms(const RfGuardConfig &cfg) {
    const uint32_t capacity_ms = static_cast<uint32_t>(rf_guard_budget_capacity_ms(cfg));
    if (capacity_ms == 0) {
        return 0;
    }
    return (capacity_ms < RF_GUARD_RECOVER_MIN_BUDGET_MS) ? capacity_ms : RF_GUARD_RECOVER_MIN_BUDGET_MS;
}

void set_rf_guard_overload_state(bool active) {
    if (g_rf_guard_overload_active == active) {
        return;
    }

    g_rf_guard_overload_active = active;
    edit_controller_set_rf_overload_active(active);
}

void rf_guard_refill_budget(uint32_t now_ms) {
    if (g_rf_guard_last_refill_at_ms == 0) {
        g_rf_guard_last_refill_at_ms = now_ms;
        return;
    }

    const uint32_t elapsed_ms = now_ms - g_rf_guard_last_refill_at_ms;
    g_rf_guard_last_refill_at_ms = now_ms;
    if (elapsed_ms == 0) {
        return;
    }

    const float capacity_ms = rf_guard_budget_capacity_ms(g_rf_guard_cfg);
    if (capacity_ms <= 0.0f) {
        g_rf_guard_budget_ms = 0.0f;
        return;
    }

    const float refill_ms = static_cast<float>(elapsed_ms) * rf_guard_refill_rate_per_ms(g_rf_guard_cfg);
    g_rf_guard_budget_ms += refill_ms;
    if (g_rf_guard_budget_ms > capacity_ms) {
        g_rf_guard_budget_ms = capacity_ms;
    }
}

void rf_guard_account_tx_time(uint32_t now_ms) {
    if (g_voice_state != VoiceBridgeState::NET_TO_RF_ACTIVE || g_rf_guard_tx_last_account_at_ms == 0) {
        return;
    }

    const uint32_t elapsed_ms = now_ms - g_rf_guard_tx_last_account_at_ms;
    g_rf_guard_tx_last_account_at_ms = now_ms;
    if (elapsed_ms == 0) {
        return;
    }

    g_rf_guard_budget_ms -= static_cast<float>(elapsed_ms);
    if (g_rf_guard_budget_ms < 0.0f) {
        g_rf_guard_budget_ms = 0.0f;
    }
}

void apply_rf_guard_runtime_config(uint32_t now_ms) {
    const RfGuardConfig latest_cfg = read_rf_guard_config_from_runtime();
    if (!rf_guard_config_equals(g_rf_guard_cfg, latest_cfg)) {
        g_rf_guard_cfg = latest_cfg;
        const float capacity_ms = rf_guard_budget_capacity_ms(g_rf_guard_cfg);
        if (g_rf_guard_budget_ms > capacity_ms) {
            g_rf_guard_budget_ms = capacity_ms;
        }
    }

    if (g_rf_guard_last_refill_at_ms == 0) {
        g_rf_guard_last_refill_at_ms = now_ms;
        if (g_rf_guard_budget_ms <= 0.0f) {
            g_rf_guard_budget_ms = rf_guard_budget_capacity_ms(g_rf_guard_cfg);
        }
    }

    if (!g_rf_guard_cfg.enabled) {
        g_rf_guard_budget_ms = rf_guard_budget_capacity_ms(g_rf_guard_cfg);
        set_rf_guard_overload_state(false);
        g_rf_guard_wait_next_burst = false;
        g_rf_guard_budget_recovered = false;
    }
}

void trip_rf_guard(const char *reason, uint32_t now_ms) {
    const bool is_single_timeout = (reason && strcmp(reason, "single_timeout") == 0);
    if (is_single_timeout) {
        ++g_rf_guard_trip_single_count;
    } else {
        ++g_rf_guard_trip_duty_count;
    }
    g_rf_guard_last_trip_reason = reason ? reason : "unknown";

    // Both protections drop current PTT and wait for next independent burst.
    // Duty exhaustion additionally enters overload latch (UI/blocked start) until budget recovers.
    g_rf_guard_wait_next_burst = true;
    g_rf_guard_budget_recovered = false;
    if (is_single_timeout) {
        set_rf_guard_overload_state(false);
    } else {
        set_rf_guard_overload_state(true);
    }
    Serial.printf("[RF_GUARD] trip reason=%s budget_ms=%.1f tx_elapsed_ms=%lu\n",
                  g_rf_guard_last_trip_reason,
                  static_cast<double>(g_rf_guard_budget_ms),
                  static_cast<unsigned long>((g_rf_guard_tx_started_at_ms == 0) ? 0 : (now_ms - g_rf_guard_tx_started_at_ms)));
    stop_voice_bridge();
}

void maybe_recover_rf_guard(uint32_t now_ms) {
    if (!g_rf_guard_cfg.enabled || !g_rf_guard_overload_active) {
        return;
    }
    if (g_voice_state == VoiceBridgeState::NET_TO_RF_ACTIVE) {
        return;
    }

    const uint32_t threshold_ms = rf_guard_recover_threshold_ms(g_rf_guard_cfg);
    if (g_rf_guard_budget_ms < static_cast<float>(threshold_ms)) {
        g_rf_guard_budget_recovered = false;
        return;
    }

    if (!g_rf_guard_budget_recovered) {
        g_rf_guard_budget_recovered = true;
        Serial.printf("[RF_GUARD] recovered budget_ms=%.1f\n", static_cast<double>(g_rf_guard_budget_ms));
    }

    // Keep overload visible/latched while current network burst is still ongoing.
    if (g_rf_guard_wait_next_burst) {
        if (g_rf_guard_last_rx_packet_at_ms == 0) {
            return;
        }
        const uint32_t quiet_gap_ms = now_ms - g_rf_guard_last_rx_packet_at_ms;
        if (quiet_gap_ms <= RF_GUARD_NEXT_BURST_GAP_MS) {
            return;
        }
        g_rf_guard_wait_next_burst = false;
        Serial.printf("[RF_GUARD] burst ended (gap=%lums), overload released\n",
                      static_cast<unsigned long>(quiet_gap_ms));
    }

    set_rf_guard_overload_state(false);
}

bool can_start_voice_bridge(uint32_t now_ms) {
    if (!g_rf_guard_cfg.enabled) {
        return true;
    }
    if (g_rf_guard_overload_active) {
        return false;
    }
    if (g_rf_guard_budget_ms <= 0.0f) {
        trip_rf_guard("duty_exhausted", now_ms);
        return false;
    }
    return true;
}

bool enforce_rf_guard_during_active_tx(uint32_t now_ms) {
    if (g_voice_state != VoiceBridgeState::NET_TO_RF_ACTIVE) {
        return true;
    }
    if (!g_rf_guard_cfg.enabled) {
        return true;
    }

    // Perform immediate budget/time accounting on packet ingress so protection does not lag by one loop.
    rf_guard_refill_budget(now_ms);
    rf_guard_account_tx_time(now_ms);

    if (g_rf_guard_tx_started_at_ms != 0) {
        const uint32_t tx_elapsed_ms = now_ms - g_rf_guard_tx_started_at_ms;
        const uint32_t tx_limit_ms = static_cast<uint32_t>(g_rf_guard_cfg.single_tx_limit_s) * 1000UL;
        if (tx_elapsed_ms > tx_limit_ms) {
            trip_rf_guard("single_timeout", now_ms);
            return false;
        }
    }

    if (g_rf_guard_budget_ms <= 0.0f) {
        trip_rf_guard("duty_exhausted", now_ms);
        return false;
    }

    return true;
}

void on_voice_bridge_started(uint32_t now_ms) {
    g_rf_guard_tx_started_at_ms = now_ms;
    g_rf_guard_tx_last_account_at_ms = now_ms;
}

void on_voice_bridge_stopped(uint32_t now_ms) {
    rf_guard_account_tx_time(now_ms);
    g_rf_guard_tx_started_at_ms = 0;
    g_rf_guard_tx_last_account_at_ms = 0;
}

void update_rf_guard_runtime(uint32_t now_ms) {
    apply_rf_guard_runtime_config(now_ms);
    rf_guard_refill_budget(now_ms);

    if (g_voice_state == VoiceBridgeState::NET_TO_RF_ACTIVE) {
        rf_guard_account_tx_time(now_ms);
        if (!g_rf_guard_cfg.enabled) {
            return;
        }

        if (g_rf_guard_tx_started_at_ms != 0) {
            const uint32_t tx_elapsed_ms = now_ms - g_rf_guard_tx_started_at_ms;
            const uint32_t tx_limit_ms = static_cast<uint32_t>(g_rf_guard_cfg.single_tx_limit_s) * 1000UL;
            if (tx_elapsed_ms > tx_limit_ms) {
                trip_rf_guard("single_timeout", now_ms);
                return;
            }
        }

        if (g_rf_guard_budget_ms <= 0.0f) {
            trip_rf_guard("duty_exhausted", now_ms);
            return;
        }
    } else {
        maybe_recover_rf_guard(now_ms);
    }
}

void schedule_radio_config_sync(uint32_t delay_ms) {
    g_radio_config_sync_pending = true;
    g_next_radio_config_sync_at_ms = millis() + delay_ms;
}

void show_config_sync_widget() {
    g_config_sync_visible = true;
    g_config_sync_hide_at_ms = 0;
    refresh_server_status_widgets();
}

void schedule_hide_config_sync_widget() {
    g_config_sync_visible = true;
    g_config_sync_hide_at_ms = millis() + 1000;
    refresh_server_status_widgets();
}

void hide_config_sync_widget() {
    g_config_sync_visible = false;
    g_config_sync_hide_at_ms = 0;
    refresh_server_status_widgets();
}

const void *server_status_icon() {
    switch (g_state) {
        case LinkState::RUNNING:
            return &ui_img_cloud_done_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png;
        case LinkState::IDLE:
        case LinkState::WAIT_WIFI:
        case LinkState::PRECHECK:
        case LinkState::REQUEST_CODE:
        case LinkState::CONFIRM_BIND:
        case LinkState::RESOLVE_SERVER:
        case LinkState::UDP_AUTH_SEND:
        case LinkState::UDP_AUTH_WAIT:
        default:
            return &ui_img_cloud_alert_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png;
    }
}

void refresh_server_status_widgets() {
    if (!g_main_screen_ready) {
        return;
    }

    if (ui_serverstat) {
        lv_img_set_src(ui_serverstat, server_status_icon());
    }
    if (ui_configsync) {
        lv_img_set_src(ui_configsync, &ui_img_cloud_sync_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png);
        if (g_config_sync_visible) {
            lv_obj_clear_flag(ui_configsync, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_configsync, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void write_padded_field(uint8_t *dest, size_t field_len, const char *src) {
    if (!dest || field_len == 0) return;
    memset(dest, 0, field_len);
    if (!src || src[0] == '\0') return;
    const size_t src_len = strlen(src);
    const size_t copy_len = (src_len < field_len) ? src_len : field_len;
    memcpy(dest, src, copy_len);
}

const char *state_name(LinkState state) {
    switch (state) {
        case LinkState::WAIT_WIFI: return "WAIT_WIFI";
        case LinkState::PRECHECK: return "PRECHECK";
        case LinkState::REQUEST_CODE: return "REQUEST_CODE";
        case LinkState::CONFIRM_BIND: return "CONFIRM_BIND";
        case LinkState::RESOLVE_SERVER: return "RESOLVE_SERVER";
        case LinkState::UDP_AUTH_SEND: return "UDP_AUTH_SEND";
        case LinkState::UDP_AUTH_WAIT: return "UDP_AUTH_WAIT";
        case LinkState::RUNNING: return "RUNNING";
        case LinkState::IDLE:
        default: return "IDLE";
    }
}

void enter_state(LinkState state, uint32_t delay_ms = 0) {
    g_state = state;
    g_next_action_at_ms = millis() + delay_ms;
    refresh_server_status_widgets();
    Serial.printf("[NET] state -> %s (delay=%lu)\n", state_name(state), static_cast<unsigned long>(delay_ms));
}

bool has_wifi_link() {
    return WiFi.status() == WL_CONNECTED;
}

bool has_auth_credentials() {
    return g_config.server.account[0] != '\0' && g_config.server.device_auth_password[0] != '\0';
}

String get_device_mac() {
    String mac = WiFi.macAddress();
    if (mac.length() > 0 && mac != "00:00:00:00:00:00") {
        return mac;
    }

    const uint64_t chip_id = ESP.getEfuseMac();
    char text[24] = {0};
    snprintf(text,
             sizeof(text),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned int>((chip_id >> 40) & 0xFFULL),
             static_cast<unsigned int>((chip_id >> 32) & 0xFFULL),
             static_cast<unsigned int>((chip_id >> 24) & 0xFFULL),
             static_cast<unsigned int>((chip_id >> 16) & 0xFFULL),
             static_cast<unsigned int>((chip_id >> 8) & 0xFFULL),
             static_cast<unsigned int>(chip_id & 0xFFULL));
    return String(text);
}

String normalize_mac_text(String mac) {
    mac.trim();
    mac.toUpperCase();
    if (mac.length() != static_cast<int>(HEARTBEAT_MAC_TEXT_SIZE)) {
        return String();
    }

    for (int i = 0; i < mac.length(); ++i) {
        const char ch = mac[i];
        if ((i % 3) == 2) {
            if (ch != ':') {
                return String();
            }
            continue;
        }

        const bool is_digit = (ch >= '0' && ch <= '9');
        const bool is_hex_upper = (ch >= 'A' && ch <= 'F');
        if (!is_digit && !is_hex_upper) {
            return String();
        }
    }

    return mac;
}

String normalized_api_base_url() {
    String base(g_config.server.http_api_base_url);
    if (base.length() == 0) {
        base = "https://ptt.4l2.cn/";
    }
    if (!base.endsWith("/")) {
        base += "/";
    }
    if (base.endsWith("/api/")) {
        return base;
    }
    if (base.endsWith("/api")) {
        return base + "/";
    }
    return base + "api/";
}

String build_api_url(const char *path) {
    String normalized_path(path ? path : "");
    while (normalized_path.startsWith("/")) {
        normalized_path.remove(0, 1);
    }
    return normalized_api_base_url() + normalized_path;
}

bool post_json(const char *api_path, JsonDocument &request, JsonDocument &response) {
    if (!has_wifi_link()) {
        return false;
    }

    String request_text;
    serializeJson(request, request_text);
    const String url = build_api_url(api_path);

    int status = 0;
    String response_text;

    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        if (!http.begin(client, url)) {
            return false;
        }
        http.setConnectTimeout(3000);
        http.setTimeout(4000);
        http.addHeader("Content-Type", "application/json");
        status = http.POST(request_text);
        if (status > 0) {
            response_text = http.getString();
        }
        http.end();
    } else {
        WiFiClient client;

        HTTPClient http;
        if (!http.begin(client, url)) {
            return false;
        }
        http.setConnectTimeout(3000);
        http.setTimeout(4000);
        http.addHeader("Content-Type", "application/json");
        status = http.POST(request_text);
        if (status > 0) {
            response_text = http.getString();
        }
        http.end();
    }

    if (status <= 0) {
        Serial.printf("[NET][HTTP] %s failed, status=%d\n", url.c_str(), status);
        return false;
    }

    const DeserializationError parse_error = deserializeJson(response, response_text);
    if (parse_error) {
        Serial.printf("[NET][HTTP] parse failed: %s\n", parse_error.c_str());
        return false;
    }
    return true;
}

void ensure_bind_popup() {
    if (g_bind_popup || !g_main_screen_ready) return;

    lv_obj_t *root = lv_layer_top();
    g_bind_popup = lv_obj_create(root);
    lv_obj_remove_style_all(g_bind_popup);
    lv_obj_set_size(g_bind_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_bind_popup, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_bind_popup, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(g_bind_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_bind_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(g_bind_popup);
    lv_obj_set_size(panel, 250, 150);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1A2330), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x506A87), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Device Dynamic Code");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE5F1FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_bind_code_label = lv_label_create(panel);
    lv_label_set_text(g_bind_code_label, "------");
    lv_obj_align(g_bind_code_label, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_text_color(g_bind_code_label, lv_color_hex(0x6EE7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(g_bind_code_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_bind_hint_label = lv_label_create(panel);
    lv_label_set_text(g_bind_hint_label, "Open web console to bind");
    lv_obj_align(g_bind_hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_color(g_bind_hint_label, lv_color_hex(0xA5B8CC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(g_bind_hint_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void show_bind_popup(const char *code, const char *hint) {
    ensure_bind_popup();
    if (!g_bind_popup) return;

    connectivity_manager_hide_ble_popup();
    edit_controller_hide_power_popup();

    if (g_bind_code_label) {
        lv_label_set_text(g_bind_code_label, (code && code[0] != '\0') ? code : "------");
    }
    if (g_bind_hint_label) {
        lv_label_set_text(g_bind_hint_label, (hint && hint[0] != '\0') ? hint : "Open web console to bind");
    }
    lv_obj_clear_flag(g_bind_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_bind_popup);
    g_bind_popup_visible = true;
}

void hide_bind_popup() {
    ensure_bind_popup();
    if (!g_bind_popup) return;

    lv_obj_add_flag(g_bind_popup, LV_OBJ_FLAG_HIDDEN);
    g_bind_popup_visible = false;
}

void stop_voice_bridge() {
    const uint32_t now_ms = millis();
    on_voice_bridge_stopped(now_ms);
    if (g_voice_state == VoiceBridgeState::NET_TO_RF_ACTIVE) {
        sa818_stop_tx();
        app_logic_on_ptt_released();
        edit_controller_set_network_bridge_active(false);
    }
    edit_controller_set_network_bridge_source(nullptr, 0);
    g_voice_state = VoiceBridgeState::IDLE;
    g_last_voice_packet_at_ms = 0;
}

bool mark_voice_packet_received() {
    const uint32_t now_ms = millis();
    uint32_t packet_gap_ms = 0;
    if (g_rf_guard_last_rx_packet_at_ms != 0) {
        packet_gap_ms = now_ms - g_rf_guard_last_rx_packet_at_ms;
    }
    g_rf_guard_last_rx_packet_at_ms = now_ms;

    g_last_voice_packet_at_ms = now_ms;
    if (g_voice_state == VoiceBridgeState::NET_TO_RF_ACTIVE) {
        if (!enforce_rf_guard_during_active_tx(now_ms)) {
            ++g_audio_rx_stats.rf_guard_drop;
            return false;
        }
        return true;
    }

    if (!can_start_voice_bridge(now_ms)) {
        ++g_audio_rx_stats.rf_guard_drop;
        return false;
    }

    if (g_rf_guard_wait_next_burst) {
        // Consider a "new transmission" only after a clearly separated quiet gap.
        if (packet_gap_ms <= RF_GUARD_NEXT_BURST_GAP_MS) {
            ++g_audio_rx_stats.rf_guard_drop;
            return false;
        }
        g_rf_guard_wait_next_burst = false;
        g_rf_guard_budget_recovered = false;
    }

    if (edit_controller_is_editing()) {
        ++g_audio_rx_stats.rf_guard_drop;
        return false;
    }

    app_logic_on_ptt_prepare();
    sa818_start_tx();
    app_logic_on_ptt_started();
    g_voice_state = VoiceBridgeState::NET_TO_RF_ACTIVE;
    edit_controller_set_network_bridge_active(true);
    on_voice_bridge_started(now_ms);
    delay(VOICE_BRIDGE_PTT_SETTLE_MS);
    return true;
}

void check_voice_timeout() {
    if (g_voice_state != VoiceBridgeState::NET_TO_RF_ACTIVE) {
        return;
    }
    const uint32_t now = millis();
    if ((now - g_last_voice_packet_at_ms) > VOICE_TX_TIMEOUT_MS) {
        stop_voice_bridge();
    }
}

void reset_tx_audio_merge() {
    g_tx_merged_payload_len = 0;
    g_tx_merged_frame_count = 0;
}

void close_udp_session() {
    if (g_udp_open) {
        g_udp.stop();
    }
    g_udp_open = false;
    g_server_ip = IPAddress();
    g_server_port = 0;
    g_local_port = 0;
    g_auth_retry = 0;
    g_last_heartbeat_at_ms = 0;
    memset(&g_audio_rx_stats, 0, sizeof(g_audio_rx_stats));
    memset(&g_audio_tx_stats, 0, sizeof(g_audio_tx_stats));
    g_last_audio_stats_log_at_ms = 0;
    g_last_audio_not_ready_log_at_ms = 0;
    g_last_decode_fail_log_at_ms = 0;
    g_last_i2s_fail_log_at_ms = 0;
    g_last_i2s_read_fail_log_at_ms = 0;
    g_last_encode_fail_log_at_ms = 0;
    g_last_udp_send_fail_log_at_ms = 0;
    reset_tx_audio_merge();
    g_rf_guard_last_rx_packet_at_ms = 0;
    g_rf_guard_wait_next_burst = false;
    g_rf_guard_budget_recovered = false;
}

void refresh_runtime_config_from_nvs() {
    device_config::DeviceConfig latest = {};
    device_config::set_defaults(latest);
    device_config::load(latest);
    g_config = latest;
}

bool save_server_config() {
    return device_config::save_server(g_config.server);
}

PrecheckResult perform_precheck() {
    if (!has_auth_credentials()) {
        Serial.println("[NET][PRECHECK] missing stored credentials, need bind.");
        return PrecheckResult::NEED_BIND;
    }

    JsonDocument request;
    JsonDocument response;

    request["mac"] = get_device_mac();
    request["username"] = g_config.server.account;
    request["device_password"] = g_config.server.device_auth_password;

    if (!post_json("/device/pre-check", request, response)) {
        Serial.printf("[NET][PRECHECK] http failed for mac=%s user=%s\n",
                      request["mac"].as<const char *>(),
                      g_config.server.account);
        return PrecheckResult::RETRY;
    }

    const int code = response["code"] | 0;
    const char *status = response["data"]["status"] | "";
    const char *message = response["data"]["message"] | response["message"] | "";
    Serial.printf("[NET][PRECHECK] mac=%s user=%s code=%d status=%s message=%s\n",
                  request["mac"].as<const char *>(),
                  g_config.server.account,
                  code,
                  status,
                  message);

    if (code != 200) {
        return PrecheckResult::NEED_BIND;
    }
    if (strcmp(status, "authenticated") == 0) {
        const char *call_sign = response["data"]["call_sign"] | "";
        if (call_sign[0] != '\0') {
            copy_cstr(g_config.server.callsign, call_sign);
            save_server_config();
        }
        return PrecheckResult::AUTHENTICATED;
    }

    return PrecheckResult::NEED_BIND;
}

bool request_dynamic_code() {
    JsonDocument request;
    JsonDocument response;

    request["mac"] = get_device_mac();
    if (!post_json("/device/request-code", request, response)) {
        Serial.printf("[NET][BIND] request-code http failed for mac=%s\n",
                      request["mac"].as<const char *>());
        show_bind_popup("", "Requesting dynamic code...");
        return false;
    }

    const int code = response["code"] | 0;
    const char *message = response["data"]["message"] | response["message"] | "";
    const char *dynamic_code = response["data"]["dynamic_code"] | "";
    Serial.printf("[NET][BIND] request-code mac=%s code=%d dynamic=%s message=%s\n",
                  request["mac"].as<const char *>(),
                  code,
                  dynamic_code,
                  message);

    if (code != 200) {
        show_bind_popup("", "Request code failed, retrying...");
        return false;
    }

    if (dynamic_code[0] == '\0') {
        show_bind_popup("", "Server returned empty code");
        return false;
    }

    copy_cstr(g_dynamic_code, dynamic_code);
    g_dynamic_code_issued_at_ms = millis();
    show_bind_popup(g_dynamic_code, "Enter this code in web console");
    Serial.printf("[NET] dynamic code: %s\n", g_dynamic_code);
    return true;
}

ConfirmBindResult perform_confirm_bind() {
    JsonDocument request;
    JsonDocument response;

    request["mac"] = get_device_mac();
    if (!post_json("/device/confirm-bind", request, response)) {
        Serial.printf("[NET][BIND] confirm-bind http failed for mac=%s\n",
                      request["mac"].as<const char *>());
        return ConfirmBindResult::RETRY;
    }

    const int code = response["code"] | 0;
    const char *status = response["data"]["status"] | "";
    const char *message = response["data"]["message"] | response["message"] | "";
    Serial.printf("[NET][BIND] confirm-bind mac=%s code=%d status=%s message=%s\n",
                  request["mac"].as<const char *>(),
                  code,
                  status,
                  message);

    if (code != 200) {
        return ConfirmBindResult::RETRY;
    }
    if (strcmp(status, "waiting") == 0) {
        show_bind_popup(g_dynamic_code, (message[0] != '\0') ? message : "Waiting for bind");
        return ConfirmBindResult::WAITING;
    }

    if (strcmp(status, "ready") != 0) {
        return ConfirmBindResult::RETRY;
    }

    const char *username = response["data"]["username"] | "";
    const char *device_password = response["data"]["device_password"] | "";
    const uint8_t ssid = response["data"]["ssid"] | 0;
    const uint32_t dmr_id = response["data"]["dmr_id"] | 0;

    if (username[0] == '\0' || device_password[0] == '\0') {
        return ConfirmBindResult::RETRY;
    }
    if (ssid != 0 && !device_config::is_valid_device_node_ssid(ssid)) {
        return ConfirmBindResult::RETRY;
    }

    copy_cstr(g_config.server.account, username);
    copy_cstr(g_config.server.device_auth_password, device_password);
    g_config.server.node_ssid = ssid;
    g_config.server.dmr_id = (dmr_id <= 0xFFFFFFUL) ? dmr_id : 0;
    save_server_config();

    hide_bind_popup();
    g_dynamic_code[0] = '\0';
    g_dynamic_code_issued_at_ms = 0;
    return ConfirmBindResult::READY;
}

bool resolve_and_open_udp() {
    if (g_config.server.udp_host[0] == '\0' || g_config.server.udp_port == 0) {
        return false;
    }
    if (WiFi.hostByName(g_config.server.udp_host, g_server_ip) != 1) {
        Serial.printf("[NET] resolve failed: %s\n", g_config.server.udp_host);
        return false;
    }

    g_server_port = g_config.server.udp_port;
    if (g_udp_open) {
        return true;
    }

    g_local_port = static_cast<uint16_t>(30000U + (esp_random() % 20000U));
    if (!g_udp.begin(g_local_port)) {
        Serial.printf("[NET] udp begin failed on local port %u\n", static_cast<unsigned int>(g_local_port));
        g_local_port = 0;
        return false;
    }

    g_udp_open = true;
    Serial.printf("[NET] udp ready %s:%u (local %u)\n",
                  g_server_ip.toString().c_str(),
                  static_cast<unsigned int>(g_server_port),
                  static_cast<unsigned int>(g_local_port));
    return true;
}

size_t build_dra_packet(uint8_t type, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len) {
    if (!out || out_len < DRA_HEADER_SIZE + payload_len || (DRA_HEADER_SIZE + payload_len) > DRA_MAX_PACKET_SIZE) {
        return 0;
    }

    const size_t total_len = DRA_HEADER_SIZE + payload_len;
    memset(out, 0, total_len);

    memcpy(out, DRA_VERSION, sizeof(DRA_VERSION));
    out[4] = static_cast<uint8_t>((total_len >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(total_len & 0xFF);

    write_padded_field(out + 6, 32, g_config.server.account);
    write_padded_field(out + 38, 10, g_config.server.device_auth_password);
    out[48] = type;
    out[49] = DEV_MODEL_ESP32;
    out[50] = g_config.server.node_ssid;

    const uint32_t dmr_id = g_config.server.dmr_id & 0xFFFFFFUL;
    out[51] = static_cast<uint8_t>((dmr_id >> 16) & 0xFF);
    out[52] = static_cast<uint8_t>((dmr_id >> 8) & 0xFF);
    out[53] = static_cast<uint8_t>(dmr_id & 0xFF);

    write_padded_field(out + 54, 32, g_config.server.callsign);

    if (payload && payload_len > 0) {
        memcpy(out + DRA_HEADER_SIZE, payload, payload_len);
    }
    return total_len;
}

bool send_udp_packet(uint8_t type, const uint8_t *payload, size_t payload_len) {
    if (!g_udp_open) {
        return false;
    }

    const size_t packet_len = build_dra_packet(type, payload, payload_len, g_rt_packet_buffer, sizeof(g_rt_packet_buffer));
    if (packet_len == 0) {
        return false;
    }

    if (!g_udp.beginPacket(g_server_ip, g_server_port)) {
        return false;
    }
    const size_t written = g_udp.write(g_rt_packet_buffer, packet_len);
    if (written != packet_len) {
        g_udp.endPacket();
        return false;
    }
    return g_udp.endPacket() == 1;
}

size_t build_heartbeat_payload(uint8_t *payload, size_t payload_cap) {
    if (!payload || payload_cap < HEARTBEAT_GPS_PAYLOAD_SIZE) {
        return 0;
    }

    memset(payload, 0, HEARTBEAT_GPS_PAYLOAD_SIZE);

    const String normalized_mac = normalize_mac_text(get_device_mac());
    const size_t mac_len = static_cast<size_t>(normalized_mac.length());
    if ((HEARTBEAT_GPS_PAYLOAD_SIZE + mac_len) > payload_cap) {
        return 0;
    }

    if (mac_len > 0) {
        memcpy(payload + HEARTBEAT_GPS_PAYLOAD_SIZE, normalized_mac.c_str(), mac_len);
    }

    return HEARTBEAT_GPS_PAYLOAD_SIZE + mac_len;
}

bool send_heartbeat_packet() {
    uint8_t payload[HEARTBEAT_GPS_PAYLOAD_SIZE + HEARTBEAT_MAC_TEXT_SIZE] = {0};
    const size_t payload_len = build_heartbeat_payload(payload, sizeof(payload));
    if (payload_len < HEARTBEAT_GPS_PAYLOAD_SIZE) {
        return false;
    }
    return send_udp_packet(TYPE_HEARTBEAT, payload, payload_len);
}

bool append_config_tlv(uint8_t *payload, size_t payload_cap, size_t &offset, uint8_t type, const uint8_t *value, size_t value_len) {
    if (!payload || !value || value_len > 255 || (offset + 2 + value_len) > payload_cap) {
        return false;
    }

    payload[offset++] = type;
    payload[offset++] = static_cast<uint8_t>(value_len);
    memcpy(payload + offset, value, value_len);
    offset += value_len;
    return true;
}

bool build_radio_config_payload(uint8_t *payload, size_t payload_cap, size_t &payload_len) {
    if (!payload || payload_cap < 2) {
        return false;
    }

    device_config::RadioConfig config = {};
    edit_controller_get_radio_config(config);

    size_t offset = 0;
    payload[offset++] = CONFIG_CMD_APPLY;
    const size_t item_count_index = offset++;
    uint8_t item_count = 0;

    uint8_t buffer8[8] = {0};
    uint8_t buffer4[4] = {0};
    uint8_t buffer1[1] = {0};
    uint8_t tone_value_buffer[8] = {0};

    auto append_tlv_with_count = [&](uint8_t type, const uint8_t *value, size_t value_len) -> bool {
        if (!append_config_tlv(payload, payload_cap, offset, type, value, value_len)) {
            return false;
        }
        ++item_count;
        return true;
    };

    write_u64_be(buffer8, static_cast<uint64_t>(config.rx_frequency_x10000) * 100ULL);
    if (!append_tlv_with_count(CONFIG_TLV_RX_FREQ, buffer8, sizeof(buffer8))) return false;

    write_u64_be(buffer8, static_cast<uint64_t>(config.tx_frequency_x10000) * 100ULL);
    if (!append_tlv_with_count(CONFIG_TLV_TX_FREQ, buffer8, sizeof(buffer8))) return false;

    write_float_be(buffer4, ctcss_hz_from_setting(config.rx_subaudio));
    if (!append_tlv_with_count(CONFIG_TLV_RX_CTCSS, buffer4, sizeof(buffer4))) return false;

    write_float_be(buffer4, ctcss_hz_from_setting(config.tx_subaudio));
    if (!append_tlv_with_count(CONFIG_TLV_TX_CTCSS, buffer4, sizeof(buffer4))) return false;

    buffer1[0] = config.squelch;
    if (!append_tlv_with_count(CONFIG_TLV_SQL_LEVEL, buffer1, sizeof(buffer1))) return false;

    buffer1[0] = power_level_from_radio_config(config);
    if (!append_tlv_with_count(CONFIG_TLV_POWER_LEVEL, buffer1, sizeof(buffer1))) return false;

    buffer1[0] = config.wide_band ? 2U : 1U;
    if (!append_tlv_with_count(CONFIG_TLV_TX_BANDWIDTH, buffer1, sizeof(buffer1))) return false;

    buffer1[0] = tone_mode_from_setting(config.rx_subaudio);
    if (!append_tlv_with_count(CONFIG_TLV_RX_TONE_MODE, buffer1, sizeof(buffer1))) return false;

    fill_tone_value_tlv(config.rx_subaudio, tone_value_buffer);
    if (!append_tlv_with_count(CONFIG_TLV_RX_TONE_VALUE, tone_value_buffer, sizeof(tone_value_buffer))) return false;

    buffer1[0] = tone_mode_from_setting(config.tx_subaudio);
    if (!append_tlv_with_count(CONFIG_TLV_TX_TONE_MODE, buffer1, sizeof(buffer1))) return false;

    fill_tone_value_tlv(config.tx_subaudio, tone_value_buffer);
    if (!append_tlv_with_count(CONFIG_TLV_TX_TONE_VALUE, tone_value_buffer, sizeof(tone_value_buffer))) return false;

    payload[item_count_index] = item_count;
    payload_len = offset;
    return true;
}

bool send_radio_config_packet() {
    uint8_t payload[96] = {0};
    size_t payload_len = 0;
    if (!build_radio_config_payload(payload, sizeof(payload), payload_len)) {
        return false;
    }
    return send_udp_packet(TYPE_CONFIG, payload, payload_len);
}

void maybe_send_pending_radio_config() {
    if (!g_radio_config_sync_pending || g_state != LinkState::RUNNING) {
        return;
    }

    const uint32_t now = millis();
    if (now < g_next_radio_config_sync_at_ms) {
        return;
    }

    show_config_sync_widget();

    if (send_radio_config_packet()) {
        g_radio_config_sync_pending = false;
        g_next_radio_config_sync_at_ms = 0;
        schedule_hide_config_sync_widget();
        Serial.println("[NET][CFG] local radio config uploaded.");
    } else {
        g_next_radio_config_sync_at_ms = now + CONFIG_SYNC_RETRY_MS;
        schedule_hide_config_sync_widget();
        Serial.println("[NET][CFG] upload failed, retry scheduled.");
    }
}

bool handle_config_apply_payload(const uint8_t *data, size_t data_len) {
    if (!data || data_len < 2 || data[0] != CONFIG_CMD_APPLY) {
        return false;
    }

    device_config::RadioConfig current = {};
    edit_controller_get_radio_config(current);
    device_config::RadioConfig updated = current;

    const uint8_t item_count = data[1];
    size_t offset = 2;
    bool saw_radio_field = false;
    bool rx_ctcss_present = false;
    bool tx_ctcss_present = false;
    float rx_ctcss_hz = 0.0f;
    float tx_ctcss_hz = 0.0f;
    ToneFieldParseState rx_tone_state = {};
    ToneFieldParseState tx_tone_state = {};

    for (uint8_t i = 0; i < item_count; ++i) {
        if ((offset + 2) > data_len) {
            return false;
        }

        const uint8_t type = data[offset++];
        const size_t value_len = data[offset++];
        if ((offset + value_len) > data_len) {
            return false;
        }

        const uint8_t *value = data + offset;
        switch (type) {
            case CONFIG_TLV_RX_FREQ:
                if (value_len == 8) {
                    updated.rx_frequency_x10000 = static_cast<uint32_t>((read_u64_be(value) + 50ULL) / 100ULL);
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_TX_FREQ:
                if (value_len == 8) {
                    updated.tx_frequency_x10000 = static_cast<uint32_t>((read_u64_be(value) + 50ULL) / 100ULL);
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_RX_CTCSS:
                if (value_len == 4) {
                    rx_ctcss_hz = read_float_be(value);
                    rx_ctcss_present = true;
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_TX_CTCSS:
                if (value_len == 4) {
                    tx_ctcss_hz = read_float_be(value);
                    tx_ctcss_present = true;
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_SQL_LEVEL:
                if (value_len == 1) {
                    updated.squelch = value[0];
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_POWER_LEVEL:
                if (value_len == 1) {
                    updated.power_high = power_high_from_level(value[0]);
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_TX_BANDWIDTH:
                if (value_len == 1) {
                    updated.wide_band = value[0] >= 2;
                    saw_radio_field = true;
                }
                break;
            case CONFIG_TLV_RX_TONE_MODE:
                rx_tone_state.mode_present = true;
                saw_radio_field = true;
                if (value_len == 1 && value[0] <= 3U) {
                    rx_tone_state.mode_valid = true;
                    rx_tone_state.mode = value[0];
                } else {
                    rx_tone_state.mode_valid = false;
                }
                break;
            case CONFIG_TLV_RX_TONE_VALUE:
                rx_tone_state.value_present = true;
                saw_radio_field = true;
                if (value_len == 8) {
                    parse_ascii_tlv_text(value, value_len, rx_tone_state.value_text, sizeof(rx_tone_state.value_text));
                    rx_tone_state.value_valid = true;
                } else {
                    rx_tone_state.value_valid = false;
                }
                break;
            case CONFIG_TLV_TX_TONE_MODE:
                tx_tone_state.mode_present = true;
                saw_radio_field = true;
                if (value_len == 1 && value[0] <= 3U) {
                    tx_tone_state.mode_valid = true;
                    tx_tone_state.mode = value[0];
                } else {
                    tx_tone_state.mode_valid = false;
                }
                break;
            case CONFIG_TLV_TX_TONE_VALUE:
                tx_tone_state.value_present = true;
                saw_radio_field = true;
                if (value_len == 8) {
                    parse_ascii_tlv_text(value, value_len, tx_tone_state.value_text, sizeof(tx_tone_state.value_text));
                    tx_tone_state.value_valid = true;
                } else {
                    tx_tone_state.value_valid = false;
                }
                break;
            case CONFIG_TLV_TIMESTAMP:
                if (value_len == 8) {
                    app_logic_set_time_from_server_ms(read_i64_be(value));
                }
                break;
            default:
                break;
        }

        offset += value_len;
    }

    if (rx_ctcss_present) {
        updated.rx_subaudio = ctcss_setting_from_hz(rx_ctcss_hz);
    }
    if (tx_ctcss_present) {
        updated.tx_subaudio = ctcss_setting_from_hz(tx_ctcss_hz);
    }

    device_config::SubAudioSetting parsed_tone = {};
    if (apply_tone_field_update(rx_tone_state, updated.rx_subaudio, parsed_tone)) {
        updated.rx_subaudio = parsed_tone;
    }
    if (apply_tone_field_update(tx_tone_state, updated.tx_subaudio, parsed_tone)) {
        updated.tx_subaudio = parsed_tone;
    }

    if (!saw_radio_field || radio_config_equals(current, updated)) {
        return true;
    }

    if (!edit_controller_set_radio_config(updated, true)) {
        return false;
    }

    g_config.radio = updated;
    Serial.println("[NET][CFG] server radio config applied.");
    return true;
}

void handle_config_packet(const uint8_t *data, size_t data_len) {
    if (!data || data_len == 0) {
        return;
    }

    switch (data[0]) {
        case CONFIG_CMD_QUERY:
            show_config_sync_widget();
            if (!send_radio_config_packet()) {
                schedule_radio_config_sync(CONFIG_SYNC_RETRY_MS);
                Serial.println("[NET][CFG] query response send failed, queued retry.");
            } else {
                schedule_hide_config_sync_widget();
                Serial.println("[NET][CFG] query response sent.");
            }
            break;
        case CONFIG_CMD_APPLY:
            show_config_sync_widget();
            if (!handle_config_apply_payload(data, data_len)) {
                schedule_hide_config_sync_widget();
                Serial.println("[NET][CFG] apply payload parse/apply failed.");
            } else {
                schedule_hide_config_sync_widget();
            }
            break;
        case CONFIG_CMD_TIME:
            if (data_len == 10) {
                int64_t unix_ms = 0;
                if (try_parse_time_sync_ms(data, data_len, unix_ms)) {
                    app_logic_set_time_from_server_ms(unix_ms);
                    Serial.printf("[NET][CFG] time sync applied. offset=2 ts=%lld\n",
                                  static_cast<long long>(unix_ms));
                } else {
                    Serial.printf("[NET][CFG] time sync parse failed. len=%u\n",
                                  static_cast<unsigned int>(data_len));
                }
            } else {
                Serial.printf("[NET][CFG] time sync ignored. len=%u expected=10\n",
                              static_cast<unsigned int>(data_len));
            }
            break;
        default:
            break;
    }
}

bool parse_dra_packet(const uint8_t *packet, size_t packet_len, DraPacketHeader &header, const uint8_t *&data, size_t &data_len) {
    if (!packet || packet_len < DRA_HEADER_SIZE || packet_len > DRA_MAX_PACKET_SIZE) {
        return false;
    }
    if (memcmp(packet, DRA_VERSION, sizeof(DRA_VERSION)) != 0) {
        return false;
    }

    const uint16_t declared_len = static_cast<uint16_t>((packet[4] << 8) | packet[5]);
    if (declared_len != packet_len) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.type = packet[48];
    header.dev_model = packet[49];
    header.ssid = packet[50];
    header.dmr_id = (static_cast<uint32_t>(packet[51]) << 16) |
                    (static_cast<uint32_t>(packet[52]) << 8) |
                    static_cast<uint32_t>(packet[53]);

    copy_fixed_string(header.username, sizeof(header.username), packet + 6, 32);
    copy_fixed_string(header.call_sign, sizeof(header.call_sign), packet + 54, 32);

    data = packet + DRA_HEADER_SIZE;
    data_len = packet_len - DRA_HEADER_SIZE;
    return true;
}

void maybe_store_call_sign(const char *call_sign) {
    if (!call_sign || call_sign[0] == '\0') {
        return;
    }
    if (strncmp(g_config.server.callsign, call_sign, sizeof(g_config.server.callsign)) == 0) {
        return;
    }
    copy_cstr(g_config.server.callsign, call_sign);
    save_server_config();
}

const char *heartbeat_status_label(uint8_t status) {
    switch (status) {
        case HEARTBEAT_STATUS_SUCCESS: return "success";
        case HEARTBEAT_STATUS_DEVICE_CONFLICT_ONLINE: return "device_conflict_online";
        case HEARTBEAT_STATUS_RESERVED_SSID: return "reserved_ssid";
        case HEARTBEAT_STATUS_AUTH_FAILED: return "auth_failed";
        default: return "unknown";
    }
}

bool is_heartbeat_reject_status(uint8_t status) {
    return status == HEARTBEAT_STATUS_DEVICE_CONFLICT_ONLINE ||
           status == HEARTBEAT_STATUS_RESERVED_SSID ||
           status == HEARTBEAT_STATUS_AUTH_FAILED;
}

void handle_heartbeat_reject(uint8_t status, const uint8_t *data, size_t data_len) {
    char message[48] = {0};
    if (data && data_len > 1) {
        parse_ascii_tlv_text(data + 1, data_len - 1, message, sizeof(message));
    }

    Serial.printf("[NET][HB] rejected: status=%u label=%s message=%s state=%s\n",
                  static_cast<unsigned int>(status),
                  heartbeat_status_label(status),
                  message[0] != '\0' ? message : "-",
                  state_name(g_state));
    on_auth_failed();
}

bool handle_heartbeat_packet(const DraPacketHeader &header, const uint8_t *data, size_t data_len) {
    if (header.call_sign[0] != '\0') {
        if (g_state == LinkState::UDP_AUTH_WAIT) {
            on_authenticated(header);
        } else {
            maybe_store_call_sign(header.call_sign);
        }
        return true;
    }

    if (data && data_len > 0 && is_heartbeat_reject_status(data[0])) {
        handle_heartbeat_reject(data[0], data, data_len);
        return true;
    }

    if (g_state == LinkState::UDP_AUTH_WAIT) {
        on_auth_failed();
        return true;
    }

    return true;
}

void maybe_log_audio_stats() {
    if (!AUDIO_STATS_LOG_ENABLED) {
        return;
    }

    if (g_state != LinkState::RUNNING && g_state != LinkState::UDP_AUTH_WAIT) {
        return;
    }

    const uint32_t now = millis();
    if ((now - g_last_audio_stats_log_at_ms) < AUDIO_STATS_LOG_INTERVAL_MS) {
        return;
    }
    g_last_audio_stats_log_at_ms = now;

    const unsigned long loop_stack_hw_bytes =
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);

    Serial.printf("[NET][AUDIO] p5=%lu p6=%lu bytes=%lu ok=%lu dec_fail=%lu i2s_fail=%lu not_ready=%lu parse_fail=%lu short6=%lu rf_drop=%lu tx_ok=%lu tx_bytes=%lu tx_read_fail=%lu tx_enc_fail=%lu tx_udp_fail=%lu rf_trip_single=%lu rf_trip_duty=%lu rf_overload=%d rf_budget_ms=%.1f stack_hw=%lu\n",
                  static_cast<unsigned long>(g_audio_rx_stats.packets_type5),
                  static_cast<unsigned long>(g_audio_rx_stats.packets_type6),
                  static_cast<unsigned long>(g_audio_rx_stats.audio_payload_bytes),
                  static_cast<unsigned long>(g_audio_rx_stats.decoded_frames_ok),
                  static_cast<unsigned long>(g_audio_rx_stats.decode_fail),
                  static_cast<unsigned long>(g_audio_rx_stats.i2s_fail),
                  static_cast<unsigned long>(g_audio_rx_stats.audio_not_ready),
                  static_cast<unsigned long>(g_audio_rx_stats.merged_parse_fail),
                  static_cast<unsigned long>(g_audio_rx_stats.server_voice_short),
                  static_cast<unsigned long>(g_audio_rx_stats.rf_guard_drop),
                  static_cast<unsigned long>(g_audio_tx_stats.packets_sent_ok),
                  static_cast<unsigned long>(g_audio_tx_stats.payload_bytes_sent),
                  static_cast<unsigned long>(g_audio_tx_stats.i2s_read_fail),
                  static_cast<unsigned long>(g_audio_tx_stats.encode_fail),
                  static_cast<unsigned long>(g_audio_tx_stats.udp_send_fail),
                  static_cast<unsigned long>(g_rf_guard_trip_single_count),
                  static_cast<unsigned long>(g_rf_guard_trip_duty_count),
                  g_rf_guard_overload_active ? 1 : 0,
                  static_cast<double>(g_rf_guard_budget_ms),
                  loop_stack_hw_bytes);
}

void maybe_capture_and_send_rf_audio() {
    if (g_state != LinkState::RUNNING) {
        reset_tx_audio_merge();
        return;
    }
    if (!g_audio_ready || !g_opus_encoder || !g_udp_open) {
        reset_tx_audio_merge();
        return;
    }
    // Half-duplex: while NET->RF is active, do not sample/send RF->NET audio.
    if (g_voice_state == VoiceBridgeState::NET_TO_RF_ACTIVE) {
        reset_tx_audio_merge();
        return;
    }
    // SQL active means RF module is currently receiving a valid signal.
    if (!sa818_is_rx()) {
        reset_tx_audio_merge();
        return;
    }

    const size_t stereo_bytes = static_cast<size_t>(OPUS_TX_FRAME_SAMPLES) * 2U * sizeof(int16_t);
    size_t bytes_read = 0;
    const bool read_ok = i2s_read(g_rt_pcm_work, stereo_bytes, &bytes_read);
    if (!read_ok || bytes_read < stereo_bytes) {
        ++g_audio_tx_stats.i2s_read_fail;
        const uint32_t now = millis();
        if ((now - g_last_i2s_read_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_i2s_read_fail_log_at_ms = now;
            Serial.printf("[NET][AUDIO][TX] i2s read failed: ok=%d bytes=%u need=%u\n",
                          read_ok ? 1 : 0,
                          static_cast<unsigned int>(bytes_read),
                          static_cast<unsigned int>(stereo_bytes));
        }
        return;
    }
    ++g_audio_tx_stats.frames_read_ok;

    // Convert interleaved stereo PCM to mono for Opus 16k/1ch.
    for (int i = 0; i < OPUS_TX_FRAME_SAMPLES; ++i) {
        const int32_t left = static_cast<int32_t>(g_rt_pcm_work[i * 2]);
        const int32_t right = static_cast<int32_t>(g_rt_pcm_work[i * 2 + 1]);
        g_tx_pcm_mono[i] = static_cast<int16_t>((left + right) / 2);
    }

    const int encoded_len = opus_encode(
        g_opus_encoder,
        g_tx_pcm_mono,
        OPUS_TX_FRAME_SAMPLES,
        g_tx_opus_buffer,
        static_cast<opus_int32>(sizeof(g_tx_opus_buffer))
    );
    if (encoded_len <= 0) {
        ++g_audio_tx_stats.encode_fail;
        reset_tx_audio_merge();
        const uint32_t now = millis();
        if ((now - g_last_encode_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_encode_fail_log_at_ms = now;
            Serial.printf("[NET][AUDIO][TX] opus encode failed: ret=%d\n", encoded_len);
        }
        return;
    }

    const size_t encoded_size = static_cast<size_t>(encoded_len);
    const size_t merged_next_len = g_tx_merged_payload_len + 2U + encoded_size;
    if (merged_next_len > sizeof(g_tx_merged_payload)) {
        ++g_audio_tx_stats.udp_send_fail;
        reset_tx_audio_merge();
        const uint32_t now = millis();
        if ((now - g_last_udp_send_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_udp_send_fail_log_at_ms = now;
            Serial.printf("[NET][AUDIO][TX] merged payload overflow: next=%u max=%u\n",
                          static_cast<unsigned int>(merged_next_len),
                          static_cast<unsigned int>(sizeof(g_tx_merged_payload)));
        }
        return;
    }

    g_tx_merged_payload[g_tx_merged_payload_len++] = static_cast<uint8_t>((encoded_size >> 8) & 0xFFU);
    g_tx_merged_payload[g_tx_merged_payload_len++] = static_cast<uint8_t>(encoded_size & 0xFFU);
    memcpy(g_tx_merged_payload + g_tx_merged_payload_len, g_tx_opus_buffer, encoded_size);
    g_tx_merged_payload_len += encoded_size;
    ++g_tx_merged_frame_count;

    if (g_tx_merged_frame_count < OPUS_TX_FRAMES_PER_PACKET) {
        return;
    }

    if (!send_udp_packet(TYPE_OPUS_16K, g_tx_merged_payload, g_tx_merged_payload_len)) {
        ++g_audio_tx_stats.udp_send_fail;
        reset_tx_audio_merge();
        const uint32_t now = millis();
        if ((now - g_last_udp_send_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_udp_send_fail_log_at_ms = now;
            Serial.println("[NET][AUDIO][TX] udp send failed.");
        }
        return;
    }

    ++g_audio_tx_stats.packets_sent_ok;
    g_audio_tx_stats.payload_bytes_sent += static_cast<uint32_t>(g_tx_merged_payload_len);
    reset_tx_audio_merge();
}

bool decode_and_play_frame(const uint8_t *frame, size_t frame_len) {
    if (!frame || frame_len == 0) {
        ++g_audio_rx_stats.audio_not_ready;
        return false;
    }

    if (!mark_voice_packet_received()) {
        return false;
    }

    if (!g_audio_ready || !g_opus_decoder) {
        ++g_audio_rx_stats.audio_not_ready;
        const uint32_t now = millis();
        if ((now - g_last_audio_not_ready_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_audio_not_ready_log_at_ms = now;
            Serial.printf("[NET][AUDIO] drop frame: audio_ready=%d i2c=%d codec=%d i2s=%d decoder=%d frame_len=%u\n",
                          g_audio_ready ? 1 : 0,
                          g_i2c_ready ? 1 : 0,
                          g_codec_ready ? 1 : 0,
                          g_i2s_ready ? 1 : 0,
                          g_opus_decoder ? 1 : 0,
                          static_cast<unsigned int>(frame_len));
        }
        return false;
    }

    const int decoded_samples = opus_decode(
        g_opus_decoder,
        frame,
        static_cast<opus_int32>(frame_len),
        g_rt_pcm_work,
        OPUS_FRAME_SAMPLES_MAX,
        0
    );

    if (decoded_samples <= 0) {
        ++g_audio_rx_stats.decode_fail;
        const uint32_t now = millis();
        if ((now - g_last_decode_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_decode_fail_log_at_ms = now;
            Serial.printf("[NET][AUDIO] opus decode failed: ret=%d frame_len=%u\n",
                          decoded_samples,
                          static_cast<unsigned int>(frame_len));
        }
        return false;
    }

    // In-place mono->stereo expansion on the shared RT PCM workspace.
    for (int i = decoded_samples - 1; i >= 0; --i) {
        const int16_t sample = g_rt_pcm_work[i];
        g_rt_pcm_work[i * 2] = sample;
        g_rt_pcm_work[i * 2 + 1] = sample;
    }

    size_t written = 0;
    const bool i2s_ok = i2s_write(
        g_rt_pcm_work,
        static_cast<size_t>(decoded_samples) * 2U * sizeof(int16_t),
        &written
    );
    if (!i2s_ok || written == 0) {
        ++g_audio_rx_stats.i2s_fail;
        const uint32_t now = millis();
        if ((now - g_last_i2s_fail_log_at_ms) >= AUDIO_ERROR_LOG_INTERVAL_MS) {
            g_last_i2s_fail_log_at_ms = now;
            Serial.printf("[NET][AUDIO] i2s write failed: ok=%d written=%u\n",
                          i2s_ok ? 1 : 0,
                          static_cast<unsigned int>(written));
        }
        return false;
    }

    ++g_audio_rx_stats.decoded_frames_ok;
    return true;
}

void decode_and_play_payload(const uint8_t *payload, size_t payload_len) {
    if (!payload || payload_len == 0) {
        return;
    }

    // Length-prefixed merged frame mode (2-byte BE length + frame bytes).
    if (payload_len >= 4 && payload[0] < 0x80) {
        size_t cursor = 0;
        bool parsed_ok = true;
        bool decoded_any = false;
        while (cursor + 2 <= payload_len) {
            const uint16_t frame_len = static_cast<uint16_t>((payload[cursor] << 8) | payload[cursor + 1]);
            cursor += 2;
            if (frame_len == 0 || (cursor + frame_len) > payload_len) {
                parsed_ok = false;
                ++g_audio_rx_stats.merged_parse_fail;
                break;
            }
            decoded_any = decode_and_play_frame(payload + cursor, frame_len) || decoded_any;
            cursor += frame_len;
        }
        if (parsed_ok && cursor == payload_len && decoded_any) {
            return;
        }
    }

    // Single Opus frame mode.
    decode_and_play_frame(payload, payload_len);
}

void on_auth_failed() {
    stop_voice_bridge();
    close_udp_session();
    enter_state(LinkState::REQUEST_CODE, 200);
}

void on_authenticated(const DraPacketHeader &header) {
    maybe_store_call_sign(header.call_sign);
    g_auth_retry = 0;
    g_last_heartbeat_at_ms = millis();
    if (!g_audio_ready) {
        Serial.printf("[NET][AUDIO] warning: audio chain degraded i2c=%d codec=%d i2s=%d opus=%d\n",
                      g_i2c_ready ? 1 : 0,
                      g_codec_ready ? 1 : 0,
                      g_i2s_ready ? 1 : 0,
                      g_opus_ready ? 1 : 0);
    }
    enter_state(LinkState::RUNNING, 0);
}

void process_udp_packet(const uint8_t *packet, size_t packet_len) {
    DraPacketHeader header = {};
    const uint8_t *data = nullptr;
    size_t data_len = 0;

    if (!parse_dra_packet(packet, packet_len, header, data, data_len)) {
        return;
    }

    if (header.type == TYPE_HEARTBEAT) {
        handle_heartbeat_packet(header, data, data_len);
        return;
    }

    if (g_state == LinkState::UDP_AUTH_WAIT &&
        (header.type == TYPE_OPUS_16K || header.type == TYPE_SERVER_VOICE || header.type == TYPE_CONFIG)) {
        on_authenticated(header);
    }

    if (g_state != LinkState::RUNNING && g_state != LinkState::UDP_AUTH_WAIT) {
        return;
    }

    if (header.type == TYPE_CONFIG) {
        maybe_store_call_sign(header.call_sign);
        handle_config_packet(data, data_len);
        return;
    }

    if (header.type == TYPE_OPUS_16K) {
        ++g_audio_rx_stats.packets_type5;
        g_audio_rx_stats.audio_payload_bytes += static_cast<uint32_t>(data_len);
        edit_controller_set_network_bridge_source(header.call_sign, header.ssid);
        decode_and_play_payload(data, data_len);
        return;
    }

    if (header.type == TYPE_SERVER_VOICE) {
        ++g_audio_rx_stats.packets_type6;
        edit_controller_set_network_bridge_source(header.call_sign, header.ssid);
        if (data_len > 68) {
            g_audio_rx_stats.audio_payload_bytes += static_cast<uint32_t>(data_len - 68);
            decode_and_play_payload(data + 68, data_len - 68);
        } else {
            ++g_audio_rx_stats.server_voice_short;
        }
        return;
    }
}

void flush_large_udp_packet(int packet_size) {
    uint8_t dump[64];
    while (packet_size > 0) {
        const int read_len = (packet_size > static_cast<int>(sizeof(dump)))
            ? static_cast<int>(sizeof(dump))
            : packet_size;
        const int consumed = g_udp.read(dump, read_len);
        if (consumed <= 0) break;
        packet_size -= consumed;
    }
}

void poll_udp_packets() {
    if (!g_udp_open) {
        return;
    }

    int packet_size = g_udp.parsePacket();
    while (packet_size > 0) {
        const IPAddress remote_ip = g_udp.remoteIP();
        const uint16_t remote_port = g_udp.remotePort();

        if (packet_size > static_cast<int>(DRA_MAX_PACKET_SIZE)) {
            flush_large_udp_packet(packet_size);
            packet_size = g_udp.parsePacket();
            continue;
        }

        const int read_len = g_udp.read(g_rt_packet_buffer, packet_size);
        if (read_len > 0 && remote_ip == g_server_ip && remote_port == g_server_port) {
            process_udp_packet(g_rt_packet_buffer, static_cast<size_t>(read_len));
        }

        packet_size = g_udp.parsePacket();
    }
}

void update_state_machine() {
    if (!g_main_screen_ready) {
        return;
    }

    const uint32_t now = millis();
    if (g_config_sync_visible && g_config_sync_hide_at_ms != 0 && now >= g_config_sync_hide_at_ms) {
        hide_config_sync_widget();
    }

    update_rf_guard_runtime(now);

    if (!has_wifi_link()) {
        stop_voice_bridge();
        close_udp_session();
        if (g_state != LinkState::WAIT_WIFI) {
            enter_state(LinkState::WAIT_WIFI, 500);
        }
        return;
    }

    check_voice_timeout();

    if (g_state == LinkState::RUNNING || g_state == LinkState::UDP_AUTH_WAIT) {
        poll_udp_packets();
        maybe_capture_and_send_rf_audio();
        maybe_log_audio_stats();
    }

    if (g_state == LinkState::RUNNING) {
        maybe_send_pending_radio_config();
    }

    if (now < g_next_action_at_ms) {
        if (g_state == LinkState::RUNNING &&
            (now - g_last_heartbeat_at_ms) >= HEARTBEAT_INTERVAL_MS) {
            if (send_heartbeat_packet()) {
                g_last_heartbeat_at_ms = now;
            }
        }
        return;
    }

    switch (g_state) {
        case LinkState::IDLE:
            enter_state(LinkState::WAIT_WIFI, 0);
            break;

        case LinkState::WAIT_WIFI:
            refresh_runtime_config_from_nvs();
            if (has_auth_credentials()) {
                enter_state(LinkState::PRECHECK, 0);
            } else {
                enter_state(LinkState::REQUEST_CODE, 0);
            }
            break;

        case LinkState::PRECHECK: {
            switch (perform_precheck()) {
                case PrecheckResult::AUTHENTICATED:
                    hide_bind_popup();
                    enter_state(LinkState::RESOLVE_SERVER, 0);
                    break;
                case PrecheckResult::NEED_BIND:
                    enter_state(LinkState::REQUEST_CODE, 0);
                    break;
                case PrecheckResult::RETRY:
                default:
                    enter_state(LinkState::PRECHECK, HTTP_RETRY_MS);
                    break;
            }
            break;
        }

        case LinkState::REQUEST_CODE:
            if (request_dynamic_code()) {
                enter_state(LinkState::CONFIRM_BIND, BIND_POLL_MS);
            } else {
                enter_state(LinkState::REQUEST_CODE, REQUEST_CODE_RETRY_MS);
            }
            break;

        case LinkState::CONFIRM_BIND:
            if (g_dynamic_code_issued_at_ms != 0 &&
                (now - g_dynamic_code_issued_at_ms) >= DYNAMIC_CODE_REFRESH_MS) {
                enter_state(LinkState::REQUEST_CODE, 0);
                break;
            }

            switch (perform_confirm_bind()) {
                case ConfirmBindResult::READY:
                    enter_state(LinkState::RESOLVE_SERVER, 0);
                    break;
                case ConfirmBindResult::WAITING:
                    enter_state(LinkState::CONFIRM_BIND, BIND_POLL_MS);
                    break;
                case ConfirmBindResult::RETRY:
                default:
                    enter_state(LinkState::CONFIRM_BIND, HTTP_RETRY_MS);
                    break;
            }
            break;

        case LinkState::RESOLVE_SERVER:
            if (resolve_and_open_udp()) {
                g_auth_retry = 0;
                enter_state(LinkState::UDP_AUTH_SEND, 0);
            } else {
                enter_state(LinkState::RESOLVE_SERVER, RESOLVE_RETRY_MS);
            }
            break;

        case LinkState::UDP_AUTH_SEND:
            if (!send_heartbeat_packet()) {
                enter_state(LinkState::UDP_AUTH_SEND, RESOLVE_RETRY_MS);
                break;
            }
            g_last_heartbeat_at_ms = now;
            if (g_auth_retry < 255) {
                ++g_auth_retry;
            }
            enter_state(LinkState::UDP_AUTH_WAIT, AUTH_TIMEOUT_MS);
            break;

        case LinkState::UDP_AUTH_WAIT:
            if (g_state != LinkState::UDP_AUTH_WAIT) {
                break;
            }
            if (g_auth_retry >= AUTH_RETRY_LIMIT) {
                on_auth_failed();
            } else {
                enter_state(LinkState::UDP_AUTH_SEND, 500);
            }
            break;

        case LinkState::RUNNING:
            if ((now - g_last_heartbeat_at_ms) >= HEARTBEAT_INTERVAL_MS) {
                if (send_heartbeat_packet()) {
                    g_last_heartbeat_at_ms = now;
                }
            }
            check_voice_timeout();
            break;

        default:
            enter_state(LinkState::WAIT_WIFI, 1000);
            break;
    }
}

bool init_audio_chain() {
    bool i2c_ok = i2c_driver_init();
    bool codec_ok = false;
    bool i2s_ok = false;
    bool opus_ok = false;

    if (i2c_ok) {
        i2c_scan_bus();
        codec_ok = es8388_init();
        if (codec_ok) {
            es8388_set_format(ES8388Format::STANDARD_I2S);
            es8388_set_sample_rate(ES8388SampleRate::RATE_16K);
            es8388_set_bits_per_sample(16);
            // Hardware routing:
            // RF module audio OUT -> ES8388 LIN1/RIN1
            // ES8388 ROUT1 -> RF module audio IN
            es8388_set_adc_input(ES8388Input::INPUT1);
            // ROUT1 belongs to the OUT1 output pair.
            es8388_set_dac_output(ES8388Output::OUT1);
            es8388_set_adc_gain(12);
            es8388_set_adc_volume(100);
            es8388_set_dac_volume(100);
            es8388_enable_adc(true);
            es8388_enable_dac(true);
            es8388_set_dac_mute(false);
            es8388_set_adc_mute(false);
        }
    }

    i2s_ok = i2s_driver_init();
    if (i2s_ok) {
        i2s_set_sample_rate(OPUS_SAMPLE_RATE);
        i2s_set_mute(false);
        i2s_set_volume(100);
    }

    int enc_error = OPUS_OK;
    int dec_error = OPUS_OK;
    g_opus_encoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, OPUS_APPLICATION_VOIP, &enc_error);
    g_opus_decoder = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &dec_error);

    if (g_opus_encoder && enc_error == OPUS_OK && g_opus_decoder && dec_error == OPUS_OK) {
        opus_encoder_ctl(g_opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(g_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
        opus_encoder_ctl(g_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_ok = true;
    }

    if (!opus_ok) {
        if (g_opus_encoder) {
            opus_encoder_destroy(g_opus_encoder);
            g_opus_encoder = nullptr;
        }
        if (g_opus_decoder) {
            opus_decoder_destroy(g_opus_decoder);
            g_opus_decoder = nullptr;
        }
    }

    g_i2c_ready = i2c_ok;
    g_codec_ready = codec_ok;
    g_i2s_ready = i2s_ok;
    g_opus_ready = opus_ok;

    Serial.printf("[NET][INIT] i2c=%d codec=%d i2s=%d opus=%d\n",
                  i2c_ok ? 1 : 0,
                  codec_ok ? 1 : 0,
                  i2s_ok ? 1 : 0,
                  opus_ok ? 1 : 0);
    if (!codec_ok && i2s_ok && opus_ok) {
        Serial.println("[NET][INIT] codec control is unavailable, but continuing with I2S/Opus so NET<->RF is not blocked.");
    }
    return i2s_ok && opus_ok;
}
} // namespace

bool net_audio_link_init() {
    if (g_initialized) {
        return g_audio_ready;
    }

    device_config::set_defaults(g_config);
    device_config::load(g_config);
    g_rf_guard_cfg = read_rf_guard_config_from_runtime();
    g_rf_guard_budget_ms = rf_guard_budget_capacity_ms(g_rf_guard_cfg);
    g_rf_guard_last_refill_at_ms = millis();
    g_rf_guard_tx_started_at_ms = 0;
    g_rf_guard_tx_last_account_at_ms = 0;
    g_rf_guard_last_rx_packet_at_ms = 0;
    g_rf_guard_wait_next_burst = false;
    g_rf_guard_budget_recovered = false;
    g_rf_guard_trip_single_count = 0;
    g_rf_guard_trip_duty_count = 0;
    g_rf_guard_last_trip_reason = "none";
    set_rf_guard_overload_state(false);

    g_audio_ready = init_audio_chain();
    g_initialized = true;
    enter_state(LinkState::WAIT_WIFI, 300);
    return g_audio_ready;
}

void net_audio_link_on_main_screen_enter() {
    g_main_screen_ready = true;
    ensure_bind_popup();
    refresh_server_status_widgets();
    if (g_dynamic_code[0] != '\0') {
        show_bind_popup(g_dynamic_code, "Enter this code in web console");
    }
}

void net_audio_link_update() {
    if (!g_initialized) {
        return;
    }
    update_state_machine();
}

void net_audio_link_schedule_radio_config_sync() {
    schedule_radio_config_sync(0);
}

void net_audio_link_hide_bind_popup() {
    hide_bind_popup();
}

bool net_audio_link_is_bind_popup_visible() {
    return g_bind_popup_visible;
}
