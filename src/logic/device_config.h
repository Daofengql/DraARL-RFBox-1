#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace device_config {

constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;
constexpr size_t IPV4_TEXT_MAX_LEN = 15;
constexpr size_t HOST_MAX_LEN = 63;
constexpr size_t URL_MAX_LEN = 127;
constexpr size_t ACCOUNT_MAX_LEN = 31;
constexpr size_t PASSWORD_MAX_LEN = 63;
constexpr size_t CALLSIGN_MAX_LEN = 15;
constexpr uint8_t BACKLIGHT_PWM_MIN = 20;
constexpr uint8_t BACKLIGHT_PWM_MAX = 255;
constexpr uint16_t RF_GUARD_SINGLE_TX_LIMIT_MIN_S = 1;
constexpr uint16_t RF_GUARD_SINGLE_TX_LIMIT_MAX_S = 1800;
constexpr uint16_t RF_GUARD_WINDOW_MIN_S = 5;
constexpr uint16_t RF_GUARD_WINDOW_MAX_S = 3600;
constexpr uint16_t RF_GUARD_MAX_TX_IN_WINDOW_MIN_S = 1;
constexpr bool RF_GUARD_ENABLED_DEFAULT = true;
constexpr uint16_t RF_GUARD_SINGLE_TX_LIMIT_DEFAULT_S = 30;
constexpr uint16_t RF_GUARD_WINDOW_DEFAULT_S = 300;
constexpr uint16_t RF_GUARD_MAX_TX_IN_WINDOW_DEFAULT_S = 60;

enum class SubAudioType : uint8_t {
    OFF = 0,
    CTCSS = 1,
    CDCSS_N = 2,
    CDCSS_I = 3,
};

struct SubAudioSetting {
    SubAudioType type;
    uint8_t index;
};

struct WiFiConfig {
    bool dhcp;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
    char ip[IPV4_TEXT_MAX_LEN + 1];
    char gateway[IPV4_TEXT_MAX_LEN + 1];
    char subnet[IPV4_TEXT_MAX_LEN + 1];
    char dns1[IPV4_TEXT_MAX_LEN + 1];
    char dns2[IPV4_TEXT_MAX_LEN + 1];
};

struct RadioConfig {
    uint32_t tx_frequency_x10000;
    uint32_t rx_frequency_x10000;
    SubAudioSetting tx_subaudio;
    SubAudioSetting rx_subaudio;
    uint8_t squelch;
    bool wide_band;
    bool power_high;
    bool rf_guard_enabled;
    uint16_t rf_guard_single_tx_limit_s;
    uint16_t rf_guard_window_s;
    uint16_t rf_guard_max_tx_in_window_s;
};

struct ServerConfig {
    char callsign[CALLSIGN_MAX_LEN + 1];
    uint8_t node_ssid;
    uint32_t dmr_id;
    char udp_host[HOST_MAX_LEN + 1];
    uint16_t udp_port;
    char http_api_base_url[URL_MAX_LEN + 1];
    char account[ACCOUNT_MAX_LEN + 1];
    char device_auth_password[PASSWORD_MAX_LEN + 1];
};

struct DeviceConfig {
    WiFiConfig wifi;
    RadioConfig radio;
    ServerConfig server;
};

void set_defaults(DeviceConfig &config);
void set_defaults(WiFiConfig &config);
void set_defaults(RadioConfig &config);
void set_defaults(ServerConfig &config);

bool load(DeviceConfig &config);
bool save(const DeviceConfig &config);
bool save_wifi(const WiFiConfig &config);
bool save_radio(const RadioConfig &config);
bool save_server(const ServerConfig &config);

bool has_wifi_credentials(const WiFiConfig &config);
bool is_valid_device_node_ssid(uint8_t ssid);
uint8_t sanitize_backlight_pwm(uint8_t pwm);
uint8_t load_backlight_pwm();
bool save_backlight_pwm(uint8_t pwm);

} // namespace device_config

#endif // DEVICE_CONFIG_H
