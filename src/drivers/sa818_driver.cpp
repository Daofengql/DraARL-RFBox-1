#include "sa818_driver.h"
#include "uart_driver.h"
#include "../config.h"
#include <cstdio>
#include <cstring>

// 模块配置
static SA818Type module_type = SA818Type::SA818_UHF;
static SA818Power current_power = SA818Power::POWER_HIGH;
static SA818Squelch current_squelch = SA818Squelch::SQ_4;
static uint8_t current_volume = 5;
static bool is_wide_band = true;
static bool is_tx = false;
static bool is_enabled = false;

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

// 发送 AT 命令
static bool send_at_command(const char *cmd, char *response, size_t resp_len, uint32_t timeout_ms = 100) {
    uart_flush_rx();

    uart_send_string(cmd);
    uart_send_string("\r\n");

    if (response && resp_len > 0) {
        int len = uart_receive((uint8_t *)response, resp_len - 1, timeout_ms);
        if (len > 0) {
            response[len] = '\0';
            return strstr(response, "OK") != nullptr || strstr(response, "1") != nullptr;
        }
        response[0] = '\0';
    }

    return true;
}

bool sa818_init(SA818Type type) {
    module_type = type;

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
    // 使用开漏模式实现高阻态
    pinMode(SA818_PW, OUTPUT_OPEN_DRAIN);
    digitalWrite(SA818_PW, 1);  // HIGH - 默认高功率

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
    if (module_type == SA818Type::SA818_VHF) {
        if (freq_khz < 134000 || freq_khz > 174000) return false;
    } else {
        if (freq_khz < 400000 || freq_khz > 470000) return false;
    }

    tx_freq = freq_khz;

    char cmd[96];
    char response[32];
    snprintf(cmd, sizeof(cmd), "AT+DMOSETGROUP=%d,%.4f,%.4f,%d,%d,%s,%s",
             is_wide_band ? 0 : 1,
             tx_freq / 1000.0,
             rx_freq / 1000.0,
             (int)current_squelch,
             current_volume,
             tx_subaudio,
             rx_subaudio);

    return send_at_command(cmd, response, sizeof(response));
}

bool sa818_set_rx_frequency(uint32_t freq_khz) {
    rx_freq = freq_khz;
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_frequency(uint32_t tx_freq_khz, uint32_t rx_freq_khz) {
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

bool sa818_set_cdcss_tx(uint16_t code) {
    if (!is_valid_cdcss_code(code)) {
        return false;
    }

    snprintf(tx_subaudio, sizeof(tx_subaudio), "%03uN", static_cast<unsigned int>(code));
    return sa818_set_tx_frequency(tx_freq);
}

bool sa818_set_cdcss_rx(uint16_t code) {
    if (!is_valid_cdcss_code(code)) {
        return false;
    }

    snprintf(rx_subaudio, sizeof(rx_subaudio), "%03uN", static_cast<unsigned int>(code));
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

bool sa818_get_version(char *buffer, size_t len) {
    if (!buffer || len == 0) return false;
    return send_at_command("AT+VERSION", buffer, len, 200);
}

bool sa818_is_connected(void) {
    char response[16];
    return send_at_command("AT", response, sizeof(response));
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
