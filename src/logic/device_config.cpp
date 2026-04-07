#include "device_config.h"

#include <Preferences.h>

#include <cstring>

namespace device_config {
namespace {
constexpr char CONFIG_NS[] = "device_cfg";
constexpr char DEFAULT_SERVER_UDP_HOST[] = "ptt.4l2.cn";
constexpr uint16_t DEFAULT_SERVER_UDP_PORT = 60050;
constexpr char DEFAULT_SERVER_HTTP_API_BASE_URL[] = "https://ptt.4l2.cn/";

void load_optional_string(Preferences &prefs, const char *key, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }

    if (!prefs.isKey(key)) {
        buffer[0] = '\0';
        return;
    }

    prefs.getString(key, buffer, buffer_len);
}

void sanitize_wifi_config(WiFiConfig &config) {
    config.ssid[WIFI_SSID_MAX_LEN] = '\0';
    config.password[WIFI_PASSWORD_MAX_LEN] = '\0';
    config.ip[IPV4_TEXT_MAX_LEN] = '\0';
    config.gateway[IPV4_TEXT_MAX_LEN] = '\0';
    config.subnet[IPV4_TEXT_MAX_LEN] = '\0';
    config.dns1[IPV4_TEXT_MAX_LEN] = '\0';
    config.dns2[IPV4_TEXT_MAX_LEN] = '\0';
}

void sanitize_radio_config(RadioConfig &config) {
    if (config.tx_frequency_x10000 < 4000000UL || config.tx_frequency_x10000 > 4700000UL) {
        config.tx_frequency_x10000 = 4395000UL;
    }
    if (config.rx_frequency_x10000 < 4000000UL || config.rx_frequency_x10000 > 4700000UL) {
        config.rx_frequency_x10000 = 4385000UL;
    }

    if (config.tx_subaudio.type > SubAudioType::CDCSS_N) {
        config.tx_subaudio = {SubAudioType::OFF, 0};
    }
    if (config.rx_subaudio.type > SubAudioType::CDCSS_N) {
        config.rx_subaudio = {SubAudioType::OFF, 0};
    }

    if (config.squelch > 8) {
        config.squelch = 4;
    }
}

void sanitize_server_config(ServerConfig &config) {
    config.callsign[CALLSIGN_MAX_LEN] = '\0';
    config.udp_host[HOST_MAX_LEN] = '\0';
    config.http_api_base_url[URL_MAX_LEN] = '\0';
    config.account[ACCOUNT_MAX_LEN] = '\0';
    config.device_auth_password[PASSWORD_MAX_LEN] = '\0';

    if (config.node_ssid > 15) {
        config.node_ssid = 0;
    }

    if (config.udp_host[0] == '\0') {
        strncpy(config.udp_host, DEFAULT_SERVER_UDP_HOST, sizeof(config.udp_host) - 1);
    }
    if (config.udp_port == 0) {
        config.udp_port = DEFAULT_SERVER_UDP_PORT;
    }
    if (config.http_api_base_url[0] == '\0') {
        strncpy(config.http_api_base_url, DEFAULT_SERVER_HTTP_API_BASE_URL, sizeof(config.http_api_base_url) - 1);
    }
}

void load_wifi_config(Preferences &prefs, WiFiConfig &config) {
    set_defaults(config);
    load_optional_string(prefs, "wifi_ssid", config.ssid, sizeof(config.ssid));
    load_optional_string(prefs, "wifi_pwd", config.password, sizeof(config.password));
    config.dhcp = prefs.getBool("wifi_dhcp", true);
    load_optional_string(prefs, "wifi_ip", config.ip, sizeof(config.ip));
    load_optional_string(prefs, "wifi_gw", config.gateway, sizeof(config.gateway));
    load_optional_string(prefs, "wifi_mask", config.subnet, sizeof(config.subnet));
    load_optional_string(prefs, "wifi_dns1", config.dns1, sizeof(config.dns1));
    load_optional_string(prefs, "wifi_dns2", config.dns2, sizeof(config.dns2));
    sanitize_wifi_config(config);
}

void load_radio_config(Preferences &prefs, RadioConfig &config) {
    set_defaults(config);
    config.tx_frequency_x10000 = prefs.getULong("radio_tx", config.tx_frequency_x10000);
    config.rx_frequency_x10000 = prefs.getULong("radio_rx", config.rx_frequency_x10000);
    config.tx_subaudio.type = static_cast<SubAudioType>(prefs.getUChar("radio_txt", static_cast<uint8_t>(config.tx_subaudio.type)));
    config.tx_subaudio.index = prefs.getUChar("radio_txi", config.tx_subaudio.index);
    config.rx_subaudio.type = static_cast<SubAudioType>(prefs.getUChar("radio_rxt", static_cast<uint8_t>(config.rx_subaudio.type)));
    config.rx_subaudio.index = prefs.getUChar("radio_rxi", config.rx_subaudio.index);
    config.squelch = prefs.getUChar("radio_sql", config.squelch);
    config.wide_band = prefs.getBool("radio_bw", config.wide_band);
    sanitize_radio_config(config);
}

void load_server_config(Preferences &prefs, ServerConfig &config) {
    set_defaults(config);
    load_optional_string(prefs, "svr_call", config.callsign, sizeof(config.callsign));
    config.node_ssid = prefs.getUChar("svr_ssid", 0);
    load_optional_string(prefs, "svr_udp_h", config.udp_host, sizeof(config.udp_host));
    config.udp_port = prefs.getUShort("svr_udp_p", 0);
    load_optional_string(prefs, "svr_http", config.http_api_base_url, sizeof(config.http_api_base_url));
    load_optional_string(prefs, "svr_acc", config.account, sizeof(config.account));
    load_optional_string(prefs, "svr_pwd", config.device_auth_password, sizeof(config.device_auth_password));
    sanitize_server_config(config);
}
} // namespace

void set_defaults(DeviceConfig &config) {
    set_defaults(config.wifi);
    set_defaults(config.radio);
    set_defaults(config.server);
}

void set_defaults(WiFiConfig &config) {
    memset(&config, 0, sizeof(config));
    config.dhcp = true;
}

void set_defaults(RadioConfig &config) {
    memset(&config, 0, sizeof(config));
    config.tx_frequency_x10000 = 4395000UL;
    config.rx_frequency_x10000 = 4385000UL;
    config.tx_subaudio = {SubAudioType::CTCSS, 7};
    config.rx_subaudio = {SubAudioType::CTCSS, 7};
    config.squelch = 4;
    config.wide_band = false;
}

void set_defaults(ServerConfig &config) {
    memset(&config, 0, sizeof(config));
    config.node_ssid = 0;
    strncpy(config.udp_host, DEFAULT_SERVER_UDP_HOST, sizeof(config.udp_host) - 1);
    config.udp_port = DEFAULT_SERVER_UDP_PORT;
    strncpy(config.http_api_base_url, DEFAULT_SERVER_HTTP_API_BASE_URL, sizeof(config.http_api_base_url) - 1);
}

bool load(DeviceConfig &config) {
    set_defaults(config);

    Preferences prefs;
    if (!prefs.begin(CONFIG_NS, true)) {
        if (prefs.begin(CONFIG_NS, false)) {
            prefs.end();
        }
        return false;
    }

    load_wifi_config(prefs, config.wifi);
    load_radio_config(prefs, config.radio);
    load_server_config(prefs, config.server);
    prefs.end();
    return true;
}

bool save(const DeviceConfig &config) {
    Preferences prefs;
    if (!prefs.begin(CONFIG_NS, false)) {
        return false;
    }

    prefs.putString("wifi_ssid", config.wifi.ssid);
    prefs.putString("wifi_pwd", config.wifi.password);
    prefs.putBool("wifi_dhcp", config.wifi.dhcp);
    prefs.putString("wifi_ip", config.wifi.ip);
    prefs.putString("wifi_gw", config.wifi.gateway);
    prefs.putString("wifi_mask", config.wifi.subnet);
    prefs.putString("wifi_dns1", config.wifi.dns1);
    prefs.putString("wifi_dns2", config.wifi.dns2);

    prefs.putULong("radio_tx", config.radio.tx_frequency_x10000);
    prefs.putULong("radio_rx", config.radio.rx_frequency_x10000);
    prefs.putUChar("radio_txt", static_cast<uint8_t>(config.radio.tx_subaudio.type));
    prefs.putUChar("radio_txi", config.radio.tx_subaudio.index);
    prefs.putUChar("radio_rxt", static_cast<uint8_t>(config.radio.rx_subaudio.type));
    prefs.putUChar("radio_rxi", config.radio.rx_subaudio.index);
    prefs.putUChar("radio_sql", config.radio.squelch);
    prefs.putBool("radio_bw", config.radio.wide_band);

    prefs.putString("svr_call", config.server.callsign);
    prefs.putUChar("svr_ssid", config.server.node_ssid);
    prefs.putString("svr_udp_h", config.server.udp_host);
    prefs.putUShort("svr_udp_p", config.server.udp_port);
    prefs.putString("svr_http", config.server.http_api_base_url);
    prefs.putString("svr_acc", config.server.account);
    prefs.putString("svr_pwd", config.server.device_auth_password);

    prefs.end();
    return true;
}

bool save_wifi(const WiFiConfig &config) {
    DeviceConfig full_config;
    load(full_config);
    full_config.wifi = config;
    sanitize_wifi_config(full_config.wifi);
    return save(full_config);
}

bool save_radio(const RadioConfig &config) {
    DeviceConfig full_config;
    load(full_config);
    full_config.radio = config;
    sanitize_radio_config(full_config.radio);
    return save(full_config);
}

bool save_server(const ServerConfig &config) {
    DeviceConfig full_config;
    load(full_config);
    full_config.server = config;
    sanitize_server_config(full_config.server);
    return save(full_config);
}

bool has_wifi_credentials(const WiFiConfig &config) {
    return config.ssid[0] != '\0';
}

} // namespace device_config
