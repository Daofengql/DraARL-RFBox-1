#ifndef SA818_DRIVER_H
#define SA818_DRIVER_H

#include <Arduino.h>
#include <functional>

// SA818 类型
enum class SA818Type {
    SA818_VHF,  // VHF 版本 (134-174MHz)
    SA818_UHF   // UHF 版本 (400-470MHz)
};

// 功率模式
enum class SA818Power {
    POWER_LOW = 0,    // 低功率 (约0.5W)
    POWER_HIGH         // 高功率 (约1W)
};

// 静噪等级
enum class SA818Squelch {
    SQ_OFF = 0,
    SQ_1, SQ_2, SQ_3, SQ_4, SQ_5, SQ_6, SQ_7, SQ_8
};

// 频偏类型
enum class SA818Offset {
    OFFSET_NONE = 0,
    OFFSET_PLUS,
    OFFSET_MINUS
};

// 接收状态回调
using SA818RxCallback = std::function<void(bool is_receiving)>;

// ==================== 初始化 ====================

bool sa818_init(SA818Type type = SA818Type::SA818_UHF);
void sa818_deinit(void);

// ==================== 模块控制 ====================

void sa818_enable(bool enable);
bool sa818_is_enabled(void);
void sa818_set_power(SA818Power power);
SA818Power sa818_get_power(void);

// ==================== PTT 控制 ====================

void sa818_start_tx(void);
void sa818_stop_tx(void);
bool sa818_is_tx(void);
bool sa818_is_rx(void);

// ==================== 频率配置 ====================

bool sa818_set_tx_frequency(uint32_t freq_khz);
bool sa818_set_rx_frequency(uint32_t freq_khz);
bool sa818_set_frequency(uint32_t tx_freq_khz, uint32_t rx_freq_khz);
uint32_t sa818_get_tx_frequency(void);
uint32_t sa818_get_rx_frequency(void);

// ==================== 音频配置 ====================

bool sa818_set_squelch(SA818Squelch squelch);
SA818Squelch sa818_get_squelch(void);
bool sa818_set_volume(uint8_t volume);
uint8_t sa818_get_volume(void);

// ==================== 亚音配置 ====================

bool sa818_set_ctcss_tx(uint16_t freq_hz);
bool sa818_set_ctcss_rx(uint16_t freq_hz);
bool sa818_set_cdcss_tx(uint16_t code);
bool sa818_set_cdcss_rx(uint16_t code);
bool sa818_set_offset(SA818Offset offset, uint32_t freq_khz);

// ==================== 带宽配置 ====================

bool sa818_set_bandwidth(bool wide);
bool sa818_get_bandwidth(void);

// ==================== 批量配置（一次 AT 命令）====================

struct SA818GroupConfig {
    uint32_t tx_freq_khz;
    uint32_t rx_freq_khz;
    const char *tx_subaudio;  // e.g. "0000", "0008", "023N"
    const char *rx_subaudio;
    SA818Squelch squelch;
    bool wide_band;
};

// 将所有配置写入驱动内部状态并发送一次 AT+DMOSETGROUP。
bool sa818_apply_group(const SA818GroupConfig &config);

// ==================== 状态查询 ====================

bool sa818_get_version(char *buffer, size_t len);
bool sa818_is_connected(void);
void sa818_set_rx_callback(SA818RxCallback callback);
void sa818_update(void);

// ==================== 低功耗模式 ====================

void sa818_sleep(void);
void sa818_wakeup(void);

#endif // SA818_DRIVER_H
