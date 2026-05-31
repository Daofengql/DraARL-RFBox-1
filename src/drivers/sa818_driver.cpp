#include "sa818_driver.h"
#include "uart_driver.h"
#include "../config.h"
#include <cctype>
#include <cstdio>
#include <cstring>

// 模块配置
static SA818Type module_type = SA818Type::SA818_UHF;
static SA818Power current_power = SA818Power::POWER_HIGH;
static SA818Squelch current_squelch = SA818Squelch::SQ_4;
static uint8_t current_volume = 5;
// SA818 manual: 0 = 12.5KHz (narrow), 1 = 25KHz (wide).
// Default to 12.5KHz to match common amateur narrowband usage.
static bool is_wide_band = false;
static bool is_tx = false;
static bool is_enabled = false;
static bool module_present = false;

// 频率配置
static uint32_t tx_freq = 433500;
static uint32_t rx_freq = 433500;
static char tx_subaudio[8] = "0000";
static char rx_subaudio[8] = "0000";
static SA818Offset offset_type = SA818Offset::OFFSET_NONE;
static uint32_t offset_freq = 0;

// 回调
static SA818RxCallback rx_callback = nullptr;
static bool last_rx_state = false;
static constexpr bool SA818_DEBUG_LOG = true;
static constexpr uint32_t SA818_GROUP_CMD_TIMEOUT_MS = 500;

static bool is_valid_frequency_khz(uint32_t freq_khz) {
    if (module_type == SA818Type::SA818_VHF) {
        return freq_khz >= 134000 && freq_khz <= 174000;
    }
    return freq_khz >= 400000 && freq_khz <= 470000;
}

static void log_uart_payload(const char *tag, const uint8_t *data, size_t len) {
    if (!SA818_DEBUG_LOG || !tag) {
        return;
    }

    if (!data || len == 0) {
        Serial.printf("[SA818][%s] len=0 <empty>\n", tag);
        return;
    }

    char text[196] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 5 < sizeof(text); ++i) {
        const uint8_t c = data[i];
        if (c == '\r') {
            text[pos++] = '\\';
            text[pos++] = 'r';
        } else if (c == '\n') {
            text[pos++] = '\\';
            text[pos++] = 'n';
        } else if (c >= 32 && c <= 126) {
            text[pos++] = static_cast<char>(c);
        } else {
            const int written = snprintf(text + pos, sizeof(text) - pos, "\\x%02X", static_cast<unsigned int>(c));
            if (written <= 0) {
                break;
            }
            pos += static_cast<size_t>(written);
        }
    }

    if (pos >= sizeof(text)) {
        text[sizeof(text) - 1] = '\0';
    } else {
        text[pos] = '\0';
    }

    Serial.printf("[SA818][%s][TXT] len=%u %s\n", tag, static_cast<unsigned int>(len), text);
}

static bool response_has_status_zero(const char *response) {
    if (!response) {
        return false;
    }

    const char *colon = strchr(response, ':');
    if (!colon) {
        return false;
    }

    ++colon;
    while (*colon == ' ' || *colon == '\t') {
        ++colon;
    }

    return *colon == '0' &&
           (colon[1] == '\0' || colon[1] == '\r' || colon[1] == '\n' || colon[1] == ',');
}

static bool is_success_response(const char *response) {
    if (!response) {
        return false;
    }

    // Common success formats on SA818/DRA818 firmware.
    if (strstr(response, "OK") != nullptr) {
        return true;
    }
    if (response[0] == '+' && response_has_status_zero(response)) {
        return true;
    }
    if (strncmp(response, "+VERSION:", 9) == 0) {
        return true;
    }
    if (strncmp(response, "RSSI:", 5) == 0) {
        return true;
    }
    if (strncmp(response, "S=", 2) == 0) {
        return true;
    }

    // Some firmware variants may reply with a bare "1".
    if (strcmp(response, "1") == 0 || strcmp(response, "1\r\n") == 0) {
        return true;
    }
    return false;
}

static constexpr uint16_t CDCSS_CODES[] = {
    23,  25,  26,  31,  32,  43,  47,  51,  54,  65,  71,  72,  73,  74,  114, 115, 116,
    125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244, 245,
    251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413,
    423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546, 565, 606, 612, 624, 627, 631,
    632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754
};

static bool is_valid_cdcss_code(uint16_t code) {
    for (size_t i = 0; i < sizeof(CDCSS_CODES) / sizeof(CDCSS_CODES[0]); ++i) {
        if (CDCSS_CODES[i] == code) {
            return true;
        }
    }
    return false;
}

static bool parse_decimal_digits(const char *text, size_t count, uint16_t &value) {
    if (!text || count == 0) {
        return false;
    }

    uint16_t parsed = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(ch)) {
            return false;
        }
        parsed = static_cast<uint16_t>((parsed * 10U) + static_cast<uint16_t>(ch - '0'));
    }

    value = parsed;
    return true;
}

static bool format_cdcss_string(uint16_t code, bool inverted, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0 || !is_valid_cdcss_code(code)) {
        return false;
    }

    snprintf(buffer, buffer_len, "%03u%c", static_cast<unsigned int>(code), inverted ? 'I' : 'N');
    return true;
}

static bool normalize_subaudio_value(const char *input, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len < 5) {
        return false;
    }

    if (!input || input[0] == '\0') {
        snprintf(buffer, buffer_len, "0000");
        return true;
    }

    char temp[8] = {0};
    size_t len = 0;
    while (*input && len < (sizeof(temp) - 1)) {
        const unsigned char ch = static_cast<unsigned char>(*input++);
        if (std::isspace(ch)) {
            continue;
        }
        temp[len++] = static_cast<char>(std::toupper(ch));
    }
    temp[len] = '\0';

    if (strcmp(temp, "OFF") == 0 || strcmp(temp, "0000") == 0) {
        snprintf(buffer, buffer_len, "0000");
        return true;
    }

    if (temp[0] == 'D' && len == 5) {
        memmove(temp, temp + 1, len);
        len -= 1;
        temp[len] = '\0';
    }

    if (len != 4) {
        return false;
    }

    uint16_t value = 0;
    if (std::isdigit(static_cast<unsigned char>(temp[3]))) {
        if (!parse_decimal_digits(temp, 4, value)) {
            return false;
        }
        if (value > 38U) {
            return false;
        }
        snprintf(buffer, buffer_len, "%04u", static_cast<unsigned int>(value));
        return true;
    }

    if (temp[3] != 'N' && temp[3] != 'I') {
        return false;
    }
    if (!parse_decimal_digits(temp, 3, value) || !is_valid_cdcss_code(value)) {
        return false;
    }

    snprintf(buffer, buffer_len, "%03u%c", static_cast<unsigned int>(value), temp[3]);
    return true;
}

// 发送 AT 命令
static bool send_at_command(const char *cmd,
                            char *response,
                            size_t resp_len,
                            uint32_t timeout_ms = 100,
                            bool allow_without_present = false) {
    if (!module_present && !allow_without_present) {
        if (response && resp_len > 0) {
            response[0] = '\0';
        }
        if (SA818_DEBUG_LOG) {
            Serial.printf("[SA818][AT>>] skipped, module not present: %s\n", cmd ? cmd : "<null>");
        }
        return false;
    }

    uart_flush_rx();

    char tx_packet[128] = {0};
    int tx_len = snprintf(tx_packet, sizeof(tx_packet), "%s\r\n", cmd ? cmd : "");
    if (tx_len <= 0) {
        if (response && resp_len > 0) {
            response[0] = '\0';
        }
        if (SA818_DEBUG_LOG) {
            Serial.println("[SA818][AT>>] <format failed>");
        }
        return false;
    }
    if (tx_len >= static_cast<int>(sizeof(tx_packet))) {
        tx_len = static_cast<int>(sizeof(tx_packet) - 1);
    }

    const size_t tx_size = static_cast<size_t>(tx_len);
    if (SA818_DEBUG_LOG) {
        Serial.printf("[SA818][AT>>][CMD] %s\n", cmd ? cmd : "<null>");
        log_uart_payload("AT>>", reinterpret_cast<const uint8_t *>(tx_packet), tx_size);
    }

    const int written = uart_send(reinterpret_cast<const uint8_t *>(tx_packet), tx_size);
    if (SA818_DEBUG_LOG) {
        Serial.printf("[SA818][AT>>][WRITE] requested=%u sent=%d\n",
                      static_cast<unsigned int>(tx_size),
                      written);
    }
    if (written <= 0) {
        if (response && resp_len > 0) {
            response[0] = '\0';
        }
        return false;
    }

    if (response && resp_len > 0) {
        int len = uart_receive((uint8_t *)response, resp_len - 1, timeout_ms);
        if (len > 0) {
            response[len] = '\0';
            if (SA818_DEBUG_LOG) {
                log_uart_payload("AT<<", reinterpret_cast<const uint8_t *>(response), static_cast<size_t>(len));
            }
            const bool ok = is_success_response(response);
            if (SA818_DEBUG_LOG) {
                Serial.printf("[SA818][AT<<][PARSE] ok=%d\n", ok ? 1 : 0);
            }
            return ok;
        }
        response[0] = '\0';
        if (SA818_DEBUG_LOG) {
            Serial.printf("[SA818][AT<<] <timeout %lu ms, pending=%d>\n",
                          static_cast<unsigned long>(timeout_ms),
                          uart_available_bytes());
        }
        return false;
    }

    if (SA818_DEBUG_LOG) {
        Serial.println("[SA818][AT<<] <no response buffer>");
    }
    return true;
}

bool sa818_init(SA818Type type) {
    module_type = type;
    module_present = false;

    // 配置 GPIO
    // SA818_EN: 外部下拉, 上拉为启动
    pinMode(SA818_EN, OUTPUT);
    digitalWrite(SA818_EN, 0);  // LOW

    // SA818_SQL: 外部上拉, 低电平为正在接收
    pinMode(SA818_SQL, INPUT);

    // SA818_PTT: 外部上拉, 低电平为控制发射
    pinMode(SA818_PTT, OUTPUT);
    digitalWrite(SA818_PTT, 1);  // HIGH - 默认不发射

    // SA818_PW: 低电平低功率, 高阻态高功率
    // 使用开漏模式实现高阻态，并复用当前运行态的功率设置。
    sa818_set_power(current_power);

    // 初始化 UART
    if (!uart_driver_init()) {
        return false;
    }

    // 默认禁用模块
    sa818_enable(false);

    return true;
}

void sa818_deinit(void) {
    uart_driver_deinit();
    module_present = false;

    pinMode(SA818_EN, INPUT);
    pinMode(SA818_SQL, INPUT);
    pinMode(SA818_PTT, INPUT);
    pinMode(SA818_PW, INPUT);
}

void sa818_enable(bool enable) {
    digitalWrite(SA818_EN, enable ? 1 : 0);
    is_enabled = enable;

    if (enable) {
        delay(100);  // 等待模块启动
    }
}

bool sa818_is_enabled(void) {
    return is_enabled;
}

void sa818_set_power(SA818Power power) {
    current_power = power;
    if (power == SA818Power::POWER_LOW) {
        pinMode(SA818_PW, OUTPUT);
        digitalWrite(SA818_PW, 0);  // LOW
    } else {
        pinMode(SA818_PW, OUTPUT_OPEN_DRAIN);
        digitalWrite(SA818_PW, 1);  // HIGH
    }
}

SA818Power sa818_get_power(void) {
    return current_power;
}

void sa818_start_tx(void) {
    if (!is_tx) {
        digitalWrite(SA818_PTT, 0);  // LOW
        is_tx = true;
    }
}

void sa818_stop_tx(void) {
    if (is_tx) {
        digitalWrite(SA818_PTT, 1);  // HIGH
        is_tx = false;
    }
}

bool sa818_is_tx(void) {
    return is_tx;
}

bool sa818_is_rx(void) {
    return digitalRead(SA818_SQL) == 0;  // LOW
}

bool sa818_set_tx_frequency(uint32_t freq_khz) {
    if (!is_valid_frequency_khz(freq_khz)) {
        return false;
    }

    tx_freq = freq_khz;

    char cmd[96];
    char response[32];
    snprintf(cmd, sizeof(cmd), "AT+DMOSETGROUP=%d,%.4f,%.4f,%s,%d,%s",
             is_wide_band ? 1 : 0,
             tx_freq / 1000.0,
             rx_freq / 1000.0,
             tx_subaudio,
             (int)current_squelch,
             rx_subaudio);

    return send_at_command(cmd, response, sizeof(response), SA818_GROUP_CMD_TIMEOUT_MS);
}

bool sa818_set_rx_frequency(uint32_t freq_khz) {
    if (!is_valid_frequency_khz(freq_khz)) {
        return false;
    }

    rx_freq = freq_khz;
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_frequency(uint32_t tx_freq_khz, uint32_t rx_freq_khz) {
    if (!is_valid_frequency_khz(tx_freq_khz) || !is_valid_frequency_khz(rx_freq_khz)) {
        return false;
    }

    tx_freq = tx_freq_khz;
    rx_freq = rx_freq_khz;
    return sa818_set_tx_frequency(tx_freq);
}

uint32_t sa818_get_tx_frequency(void) {
    return tx_freq;
}

uint32_t sa818_get_rx_frequency(void) {
    return rx_freq;
}

bool sa818_set_squelch(SA818Squelch squelch) {
    current_squelch = squelch;
    return sa818_set_tx_frequency(tx_freq);
}

SA818Squelch sa818_get_squelch(void) {
    return current_squelch;
}

bool sa818_set_volume(uint8_t volume) {
    if (volume > 8) volume = 8;
    current_volume = volume;

    char cmd[32];
    char response[16];
    snprintf(cmd, sizeof(cmd), "AT+DMOSETVOLUME=%d", volume);
    return send_at_command(cmd, response, sizeof(response));
}

uint8_t sa818_get_volume(void) {
    return current_volume;
}

bool sa818_set_ctcss_tx(uint16_t freq_hz) {
    if (freq_hz > 38) {
        return false;
    }

    snprintf(tx_subaudio, sizeof(tx_subaudio), "%04u", static_cast<unsigned int>(freq_hz));
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_ctcss_rx(uint16_t freq_hz) {
    if (freq_hz > 38) {
        return false;
    }

    snprintf(rx_subaudio, sizeof(rx_subaudio), "%04u", static_cast<unsigned int>(freq_hz));
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_cdcss_tx(uint16_t code, bool inverted) {
    if (!format_cdcss_string(code, inverted, tx_subaudio, sizeof(tx_subaudio))) {
        return false;
    }

    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_cdcss_rx(uint16_t code, bool inverted) {
    if (!format_cdcss_string(code, inverted, rx_subaudio, sizeof(rx_subaudio))) {
        return false;
    }

    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_offset(SA818Offset offset, uint32_t freq_khz) {
    offset_type = offset;
    offset_freq = freq_khz;

    uint32_t actual_rx = rx_freq;
    switch (offset) {
        case SA818Offset::OFFSET_PLUS:
            actual_rx = rx_freq + offset_freq;
            break;
        case SA818Offset::OFFSET_MINUS:
            actual_rx = rx_freq - offset_freq;
            break;
        default:
            break;
    }

    return sa818_set_frequency(tx_freq, actual_rx);
}

bool sa818_set_bandwidth(bool wide) {
    is_wide_band = wide;
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_get_bandwidth(void) {
    return is_wide_band;
}

bool sa818_apply_group(const SA818GroupConfig &config) {
    // 校验频率范围
    if (!is_valid_frequency_khz(config.tx_freq_khz) || !is_valid_frequency_khz(config.rx_freq_khz)) {
        return false;
    }

    // 更新内部状态
    tx_freq = config.tx_freq_khz;
    rx_freq = config.rx_freq_khz;
    is_wide_band = config.wide_band;
    current_squelch = config.squelch;

    const char *tx_sub = config.tx_subaudio ? config.tx_subaudio : "0000";
    const char *rx_sub = config.rx_subaudio ? config.rx_subaudio : "0000";
    char normalized_tx[8] = {0};
    char normalized_rx[8] = {0};
    if (!normalize_subaudio_value(tx_sub, normalized_tx, sizeof(normalized_tx)) ||
        !normalize_subaudio_value(rx_sub, normalized_rx, sizeof(normalized_rx))) {
        return false;
    }

    strncpy(tx_subaudio, normalized_tx, sizeof(tx_subaudio) - 1);
    tx_subaudio[sizeof(tx_subaudio) - 1] = '\0';
    strncpy(rx_subaudio, normalized_rx, sizeof(rx_subaudio) - 1);
    rx_subaudio[sizeof(rx_subaudio) - 1] = '\0';

    char cmd[96];
    char response[32];
    snprintf(cmd, sizeof(cmd), "AT+DMOSETGROUP=%d,%.4f,%.4f,%s,%d,%s",
             is_wide_band ? 1 : 0,
             tx_freq / 1000.0,
             rx_freq / 1000.0,
             tx_subaudio,
             static_cast<int>(current_squelch),
             rx_subaudio);

    return send_at_command(cmd, response, sizeof(response), SA818_GROUP_CMD_TIMEOUT_MS);
}

bool sa818_read_group(SA818GroupState &state) {
    char response[96] = {0};
    if (!send_at_command("AT+DMOREADGROUP", response, sizeof(response), 200)) {
        return false;
    }

    unsigned int band = 0;
    unsigned int sql = 0;
    float parsed_tx_mhz = 0.0f;
    float parsed_rx_mhz = 0.0f;
    char parsed_tx_sub[8] = {0};
    char parsed_rx_sub[8] = {0};
    const int matched = sscanf(response,
                               "+DMOREADGROUP: %u,%f,%f,%7[^,],%u,%7s",
                               &band,
                               &parsed_tx_mhz,
                               &parsed_rx_mhz,
                               parsed_tx_sub,
                               &sql,
                               parsed_rx_sub);
    if (matched != 6) {
        return false;
    }

    const uint32_t parsed_tx_khz = static_cast<uint32_t>((parsed_tx_mhz * 1000.0f) + 0.5f);
    const uint32_t parsed_rx_khz = static_cast<uint32_t>((parsed_rx_mhz * 1000.0f) + 0.5f);
    if (!is_valid_frequency_khz(parsed_tx_khz) || !is_valid_frequency_khz(parsed_rx_khz) || sql > 8U) {
        return false;
    }
    if (!normalize_subaudio_value(parsed_tx_sub, state.tx_subaudio, sizeof(state.tx_subaudio)) ||
        !normalize_subaudio_value(parsed_rx_sub, state.rx_subaudio, sizeof(state.rx_subaudio))) {
        return false;
    }

    state.tx_freq_khz = parsed_tx_khz;
    state.rx_freq_khz = parsed_rx_khz;
    state.squelch = static_cast<SA818Squelch>(sql);
    state.wide_band = band != 0U;

    tx_freq = state.tx_freq_khz;
    rx_freq = state.rx_freq_khz;
    current_squelch = state.squelch;
    is_wide_band = state.wide_band;
    strncpy(tx_subaudio, state.tx_subaudio, sizeof(tx_subaudio) - 1);
    tx_subaudio[sizeof(tx_subaudio) - 1] = '\0';
    strncpy(rx_subaudio, state.rx_subaudio, sizeof(rx_subaudio) - 1);
    rx_subaudio[sizeof(rx_subaudio) - 1] = '\0';
    return true;
}

bool sa818_set_filters(bool pre_emphasis_enabled, bool high_pass_enabled, bool low_pass_enabled) {
    char cmd[40] = {0};
    char response[32] = {0};
    snprintf(cmd,
             sizeof(cmd),
             "AT+SETFILTER=%d,%d,%d",
             pre_emphasis_enabled ? 0 : 1,
             high_pass_enabled ? 0 : 1,
             low_pass_enabled ? 0 : 1);
    return send_at_command(cmd, response, sizeof(response), 200);
}

bool sa818_set_tail(bool enable) {
    char cmd[24] = {0};
    char response[32] = {0};
    snprintf(cmd, sizeof(cmd), "AT+SETTAIL=%d", enable ? 1 : 0);
    return send_at_command(cmd, response, sizeof(response), 200);
}

bool sa818_get_rssi(uint8_t &rssi) {
    char response[32] = {0};
    if (!send_at_command("RSSI?", response, sizeof(response), 200)) {
        return false;
    }

    unsigned int parsed = 0;
    if (sscanf(response, "RSSI:%u", &parsed) != 1 || parsed > 255U) {
        return false;
    }

    rssi = static_cast<uint8_t>(parsed);
    return true;
}

bool sa818_scan_frequency(uint32_t freq_khz, bool &has_signal) {
    if (!is_valid_frequency_khz(freq_khz)) {
        return false;
    }

    char cmd[24] = {0};
    char response[32] = {0};
    snprintf(cmd, sizeof(cmd), "S+%.4f", freq_khz / 1000.0);
    if (!send_at_command(cmd, response, sizeof(response), 500)) {
        return false;
    }

    unsigned int scan_result = 0;
    if (sscanf(response, "S=%u", &scan_result) != 1 || scan_result > 1U) {
        return false;
    }

    has_signal = (scan_result == 0U);
    return true;
}

bool sa818_get_version(char *buffer, size_t len) {
    if (!buffer || len == 0) return false;
    return send_at_command("AT+VERSION", buffer, len, 200);
}

bool sa818_is_connected(void) {
    char response[32] = {0};
    bool connected = false;

    // SA818 / DRA818 系列通常使用 AT+DMOCONNECT 进行握手。
    if (send_at_command("AT+DMOCONNECT", response, sizeof(response), 500, true)) {
        connected = true;
    } else if (strstr(response, "+DMOCONNECT") != nullptr) {
        connected = true;
    }

    // 兼容部分固件：普通 "AT" 可能返回 +DMOERROR，但这也能证明串口链路是通的。
    if (!connected) {
        response[0] = '\0';
        if (send_at_command("AT", response, sizeof(response), 500, true)) {
            connected = true;
        } else if (strstr(response, "+DMOERROR") != nullptr) {
            connected = true;
        }
    }

    module_present = connected;
    return module_present;
}

void sa818_set_rx_callback(SA818RxCallback callback) {
    rx_callback = callback;
}

void sa818_update(void) {
    bool current_rx = sa818_is_rx();

    if (current_rx != last_rx_state) {
        last_rx_state = current_rx;
        if (rx_callback) {
            rx_callback(current_rx);
        }
    }
}

void sa818_sleep(void) {
    char response[16];
    send_at_command("AT+DMOSLEEP", response, sizeof(response));
}

void sa818_wakeup(void) {
    uart_send_string("AT");
    delay(10);
}
