#include "connectivity_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "../ui/ui.h"
#include "device_config.h"
#include "edit_controller.h"
#include "net_audio_link.h"

namespace {
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_DELAY_MS = 3000;
constexpr uint8_t WIFI_BOOT_FAILURE_LIMIT = 3;
constexpr uint32_t BLE_IDLE_TIMEOUT_MS = 10 * 60 * 1000UL;
constexpr uint8_t BLE_AUTH_CODE_LEN = 6;
constexpr size_t BLE_RPC_CHUNK_PAYLOAD = 19;
constexpr uint32_t WIFI_SCAN_WAIT_BUDGET_MS = 3000;
constexpr uint32_t WIFI_SCAN_POLL_INTERVAL_MS = 120;
constexpr uint8_t WIFI_SCAN_CACHE_MAX = 32;
constexpr int WIFI_SCAN_STATUS_RUNNING = -1;
constexpr int WIFI_SCAN_STATUS_FAILED = -2;

constexpr char BLE_SERVICE_UUID[] = "6d22f67d-7287-4f4e-8548-b362f9b1f001";
constexpr char BLE_STATUS_UUID[] = "6d22f67d-7287-4f4e-8548-b362f9b1f002";
constexpr char BLE_AUTH_UUID[] = "6d22f67d-7287-4f4e-8548-b362f9b1f003";
constexpr char BLE_RPC_TX_UUID[] = "6d22f67d-7287-4f4e-8548-b362f9b1f004";
constexpr char BLE_RPC_RX_UUID[] = "6d22f67d-7287-4f4e-8548-b362f9b1f005";

constexpr uint8_t BLE_CHUNK_FLAG_START = 0x01;
constexpr uint8_t BLE_CHUNK_FLAG_END = 0x02;

enum class WiFiState : uint8_t {
    UNINITIALIZED = 0,
    NO_CONFIG = 1,
    CONNECTING = 2,
    CONNECTED = 3,
    FAILED = 4,
};

enum class BleState : uint8_t {
    BT_OFF = 0,
    BT_ADVERTISING = 1,
    BT_CONNECTED = 2,
};

device_config::DeviceConfig g_config = {};

bool g_manager_initialized = false;
bool g_main_screen_ready = false;
bool g_wifi_attempt_active = false;
bool g_wifi_manual_disconnect = false;
bool g_wifi_connected_once = false;
bool g_wifi_retry_suspended = false;
uint8_t g_wifi_boot_failures = 0;
uint32_t g_wifi_attempt_started_at_ms = 0;
uint32_t g_wifi_next_retry_at_ms = 0;
WiFiState g_wifi_state = WiFiState::UNINITIALIZED;
int32_t g_wifi_rssi = 0;

struct WiFiScanCacheEntry {
    char ssid[33];
    int32_t rssi;
    int32_t auth;
};

WiFiScanCacheEntry g_wifi_scan_cache[WIFI_SCAN_CACHE_MAX] = {};
uint8_t g_wifi_scan_cache_count = 0;
uint32_t g_wifi_scan_cache_updated_at_ms = 0;
bool g_wifi_scan_in_progress = false;

bool g_ble_stack_initialized = false;
bool g_ble_enabled = false;
bool g_ble_transport_connected = false;
bool g_ble_authenticated = false;
uint16_t g_ble_conn_id = 0;
uint32_t g_ble_idle_since_ms = 0;
char g_ble_auth_code[BLE_AUTH_CODE_LEN + 1] = {0};
char g_ble_device_name[32] = {0};
String g_ble_rpc_rx_buffer;

BLEServer *g_ble_server = nullptr;
BLEService *g_ble_service = nullptr;
BLECharacteristic *g_ble_status_char = nullptr;
BLECharacteristic *g_ble_auth_char = nullptr;
BLECharacteristic *g_ble_rpc_tx_char = nullptr;
BLECharacteristic *g_ble_rpc_rx_char = nullptr;
BLEAdvertising *g_ble_advertising = nullptr;

lv_obj_t *g_ble_popup = nullptr;
lv_obj_t *g_ble_popup_code_label = nullptr;
bool g_ble_popup_visible = false;

// ==========================================
// 【线程安全设计】后台事件标志位
// ==========================================
// 以下标志位由 WiFi / BLE 的后台任务置位，主线程进行消费清零
volatile bool g_evt_wifi_connected = false;
volatile bool g_evt_wifi_disconnected = false;
volatile bool g_evt_ble_connected = false;
volatile bool g_evt_ble_disconnected = false;
volatile bool g_evt_ble_auth_success = false;
volatile bool g_evt_ble_auth_failed = false;
volatile bool g_evt_rpc_ready = false;

const char *wifi_state_label(WiFiState state) {
    switch (state) {
        case WiFiState::NO_CONFIG:   return "no-config";
        case WiFiState::CONNECTING:  return "connecting";
        case WiFiState::CONNECTED:   return "connected";
        case WiFiState::FAILED:      return "failed";
        case WiFiState::UNINITIALIZED:
        default:                     return "idle";
    }
}

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

BleState current_ble_state() {
    if (!g_ble_enabled) return BleState::BT_OFF;
    if (g_ble_authenticated) return BleState::BT_CONNECTED;
    return BleState::BT_ADVERTISING;
}

const void *current_wifi_icon() {
    switch (g_wifi_state) {
        case WiFiState::CONNECTED:
            if (g_wifi_rssi >= -60) return &ui_img_wifi_4_png;
            if (g_wifi_rssi >= -70) return &ui_img_wifi_3_png;
            if (g_wifi_rssi >= -80) return &ui_img_wifi_2_png;
            return &ui_img_wifi_1_png;
        case WiFiState::NO_CONFIG:
            return &ui_img_wifi_off_png;
        case WiFiState::CONNECTING:
        case WiFiState::FAILED:
        case WiFiState::UNINITIALIZED:
        default:
            return &ui_img_wifi_bad_png;
    }
}

const void *current_ble_icon() {
    switch (current_ble_state()) {
        case BleState::BT_ADVERTISING:
            return &ui_img_bluetooth_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png;
        case BleState::BT_CONNECTED:
            return &ui_img_bluetooth_connected_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png;
        case BleState::BT_OFF:
        default:
            return &ui_img_bluetooth_disabled_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png;
    }
}

void refresh_status_icons() {
    if (!g_main_screen_ready) return;
    
    if (ui_wifistat) {
        lv_img_set_src(ui_wifistat, current_wifi_icon());
    }
    if (ui_wifistat1) {
        lv_img_set_src(ui_wifistat1, current_wifi_icon());
    }
    if (ui_wifistat2) {
        lv_img_set_src(ui_wifistat2, current_wifi_icon());
    }
    if (ui_wifistat3) {
        lv_img_set_src(ui_wifistat3, current_wifi_icon());
    }
    if (ui_bluestat) {
        lv_img_set_src(ui_bluestat, current_ble_icon());
    }
    if (ui_bluestat1) {
        lv_img_set_src(ui_bluestat1, current_ble_icon());
    }
    if (ui_bluestat2) {
        lv_img_set_src(ui_bluestat2, current_ble_icon());
    }
    if (ui_bluestat3) {
        lv_img_set_src(ui_bluestat3, current_ble_icon());
    }
}

void ensure_popup() {
    if (g_ble_popup || !g_main_screen_ready) return;

    lv_obj_t *root = lv_layer_top();
    g_ble_popup = lv_obj_create(root);
    lv_obj_remove_style_all(g_ble_popup);
    lv_obj_set_size(g_ble_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_ble_popup, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_ble_popup, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(g_ble_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ble_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(g_ble_popup);
    lv_obj_set_size(panel, 240, 140);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16212B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3F5872), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "BLE GATT Provisioning");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE4F0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_ble_popup_code_label = lv_label_create(panel);
    lv_obj_align(g_ble_popup_code_label, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_text_color(g_ble_popup_code_label, lv_color_hex(0x57D7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(g_ble_popup_code_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "Short press EC11 to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x95A9BF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void hide_ble_popup() {
    ensure_popup();
    if (!g_ble_popup) return;

    lv_obj_add_flag(g_ble_popup, LV_OBJ_FLAG_HIDDEN);
    g_ble_popup_visible = false;
}

void show_ble_popup(const char *reason) {
    (void)reason;
    ensure_popup();
    if (!g_ble_popup) return;

    lv_label_set_text(g_ble_popup_code_label, g_ble_auth_code);
    lv_obj_clear_flag(g_ble_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_ble_popup);
    g_ble_popup_visible = true;
}

String build_status_payload() {
    char payload[64] = {0};
    snprintf(payload,
             sizeof(payload),
             "w%u;b%u;a%u;r%ld;f%u",
             static_cast<unsigned int>(g_wifi_state),
             static_cast<unsigned int>(current_ble_state()),
             g_ble_authenticated ? 1U : 0U,
             static_cast<long>(g_wifi_rssi),
             static_cast<unsigned int>(g_wifi_boot_failures));
    return String(payload);
}

void notify_status() {
    refresh_status_icons();

    if (!g_ble_status_char) return;

    String payload = build_status_payload();
    g_ble_status_char->setValue(payload.c_str());
    g_ble_status_char->notify();
}

void notify_rpc_json(const String &json) {
    if (!g_ble_rpc_rx_char) return;

    const size_t total_len = json.length();
    const char *raw = json.c_str();
    size_t offset = 0;

    while (offset < total_len) {
        const size_t chunk_len = std::min(BLE_RPC_CHUNK_PAYLOAD, total_len - offset);
        uint8_t frame[1 + BLE_RPC_CHUNK_PAYLOAD] = {0};
        uint8_t flags = 0;
        if (offset == 0) flags |= BLE_CHUNK_FLAG_START;
        if (offset + chunk_len >= total_len) flags |= BLE_CHUNK_FLAG_END;

        frame[0] = flags;
        memcpy(frame + 1, raw + offset, chunk_len);
        g_ble_rpc_rx_char->setValue(frame, chunk_len + 1);
        g_ble_rpc_rx_char->notify();

        offset += chunk_len;
        delay(8);
    }
}

void send_rpc_error(JsonVariantConst request, const char *error_message) {
    JsonDocument response;
    if (!request.isNull() && request["id"].is<int>()) {
        response["id"] = request["id"].as<int>();
    }
    response["ok"] = false;
    response["error"] = error_message ? error_message : "unknown";

    String json;
    serializeJson(response, json);
    notify_rpc_json(json);
}

String subaudio_type_to_string(device_config::SubAudioType type) {
    switch (type) {
        case device_config::SubAudioType::CTCSS: return "CTCSS";
        case device_config::SubAudioType::CDCSS_N: return "CDCSS_N";
        case device_config::SubAudioType::CDCSS_I: return "CDCSS_I";
        case device_config::SubAudioType::OFF:
        default: return "OFF";
    }
}

device_config::SubAudioType subaudio_type_from_string(const String &value) {
    if (value.equalsIgnoreCase("CTCSS")) return device_config::SubAudioType::CTCSS;
    if (value.equalsIgnoreCase("CDCSS_I") || value.equalsIgnoreCase("DCS_I")) return device_config::SubAudioType::CDCSS_I;
    if (value.equalsIgnoreCase("CDCSS_N") || value.equalsIgnoreCase("DCS") || value.equalsIgnoreCase("DCS_N")) {
        return device_config::SubAudioType::CDCSS_N;
    }
    return device_config::SubAudioType::OFF;
}

void format_frequency_mhz(uint32_t frequency_x10000, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) return;
    const uint32_t mhz = frequency_x10000 / 10000UL;
    const uint32_t decimals = frequency_x10000 % 10000UL;
    snprintf(buffer, buffer_len, "%03lu.%04lu", static_cast<unsigned long>(mhz), static_cast<unsigned long>(decimals));
}

bool parse_frequency_value(JsonVariantConst value, uint32_t &out_frequency_x10000) {
    double mhz = 0.0;
    if (value.is<const char *>()) {
        mhz = atof(value.as<const char *>());
    } else if (value.is<double>()) {
        mhz = value.as<double>();
    } else if (value.is<float>()) {
        mhz = value.as<float>();
    } else if (value.is<uint32_t>()) {
        const uint32_t raw = value.as<uint32_t>();
        if (raw > 100000UL) {
            out_frequency_x10000 = raw;
            return true;
        }
        mhz = static_cast<double>(raw);
    } else {
        return false;
    }
    out_frequency_x10000 = static_cast<uint32_t>(std::lround(mhz * 10000.0));
    return true;
}

bool parse_ipv4_text(const char *value, IPAddress &out_ip) {
    if (!value || value[0] == '\0') return false;
    return out_ip.fromString(value);
}

bool apply_wifi_network_config() {
    if (g_config.wifi.dhcp) {
        return WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    IPAddress local_ip, gateway, subnet, dns1, dns2;
    if (!parse_ipv4_text(g_config.wifi.ip, local_ip) ||
        !parse_ipv4_text(g_config.wifi.gateway, gateway) ||
        !parse_ipv4_text(g_config.wifi.subnet, subnet)) {
        return false;
    }
    const bool has_dns1 = parse_ipv4_text(g_config.wifi.dns1, dns1);
    const bool has_dns2 = parse_ipv4_text(g_config.wifi.dns2, dns2);
    if (has_dns1 && has_dns2) return WiFi.config(local_ip, gateway, subnet, dns1, dns2);
    if (has_dns1) return WiFi.config(local_ip, gateway, subnet, dns1);
    return WiFi.config(local_ip, gateway, subnet);
}

void schedule_wifi_retry(uint32_t delay_ms) {
    g_wifi_next_retry_at_ms = millis() + delay_ms;
}

// 提前声明部分内部函数，确保主循环内可顺利调用
void begin_wifi_connect_attempt(bool reset_failures);
void start_ble_advertising();
void process_rpc_request(const String &json_text);
void enable_ble(const char *reason, bool show_popup);
void disable_ble();
void maybe_start_ble_for_boot_failure(const char *reason);
void handle_wifi_attempt_failure(const char *reason);
void on_wifi_event(arduino_event_id_t event);
void suspend_wifi_retry_for_ble_provisioning();

void begin_wifi_connect_attempt(bool reset_failures) {
    if (!g_main_screen_ready) return;

    if (!device_config::has_wifi_credentials(g_config.wifi)) {
        g_wifi_state = WiFiState::NO_CONFIG;
        g_wifi_attempt_active = false;
        notify_status();
        return;
    }

    if (reset_failures) {
        g_wifi_boot_failures = 0;
        g_wifi_connected_once = false;
        g_wifi_retry_suspended = false;
    } else if (g_wifi_retry_suspended) {
        // BLE provisioning fallback is active, suppress background Wi-Fi churn.
        return;
    }

    if (!apply_wifi_network_config()) {
        g_wifi_state = WiFiState::FAILED;
        notify_status();
        return;
    }

    WiFi.begin(g_config.wifi.ssid, g_config.wifi.password);
    g_wifi_state = WiFiState::CONNECTING;
    g_wifi_attempt_active = true;
    g_wifi_attempt_started_at_ms = millis();
    g_wifi_next_retry_at_ms = 0;
    notify_status();
    Serial.printf("[WIFI] Connecting to %s\n", g_config.wifi.ssid);
}

void generate_auth_code() {
    const uint32_t value = static_cast<uint32_t>(esp_random()) % 1000000UL;
    snprintf(g_ble_auth_code, sizeof(g_ble_auth_code), "%06lu", static_cast<unsigned long>(value));
}

void build_ble_device_name() {
    const uint64_t chip_id = ESP.getEfuseMac();
    snprintf(g_ble_device_name,
             sizeof(g_ble_device_name),
             "DraARL-%06llX",
             static_cast<unsigned long long>(chip_id & 0xFFFFFFULL));
}

void start_ble_advertising() {
    if (!g_ble_advertising) return;
    g_ble_advertising->stop();
    g_ble_advertising->setScanResponse(true);
    g_ble_advertising->setMinPreferred(0x06);
    g_ble_advertising->setMinPreferred(0x12);
    g_ble_advertising->start();
}

// -----------------------------------------------------
// 异步事件回调：只负责更改全局标志位，绝不直接操作UI
// -----------------------------------------------------
class ProvisioningServerCallbacks final : public BLEServerCallbacks {
public:
    void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override {
        (void)server;
        g_ble_conn_id = desc ? desc->conn_handle : 0;
        g_evt_ble_connected = true; // 发送事件标志给主线程
    }

    void onDisconnect(BLEServer *server) override {
        (void)server;
        g_evt_ble_disconnected = true; // 发送事件标志给主线程
    }
};

class AuthCharacteristicCallbacks final : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *characteristic) override {
        if (!characteristic) return;

        // Receiving writes proves an active link, even if connect event was delayed.
        g_ble_transport_connected = true;

        const std::string value = characteristic->getValue();
        String code(value.c_str());
        code.trim();

        if (code.equals(g_ble_auth_code)) {
            characteristic->setValue("OK");
            g_evt_ble_auth_failed = false;
            g_evt_ble_auth_success = true; // 密码正确，交由主线程处理
        } else {
            characteristic->setValue("ERR");
            g_evt_ble_auth_success = false;
            g_evt_ble_auth_failed = true; // 密码错误，交由主线程处理
        }
    }
};

class RpcTxCharacteristicCallbacks final : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *characteristic) override {
        if (!characteristic) return;

        // Keep transport state in sync to avoid dropping RPC responses.
        g_ble_transport_connected = true;

        const std::string value = characteristic->getValue();
        if (value.empty()) return;

        const uint8_t *raw = reinterpret_cast<const uint8_t *>(value.data());
        const size_t len = value.length();
        const uint8_t flags = raw[0];

        if (flags & BLE_CHUNK_FLAG_START) {
            g_ble_rpc_rx_buffer = "";
        }

        if (len > 1) {
            g_ble_rpc_rx_buffer.reserve(g_ble_rpc_rx_buffer.length() + len - 1);
            for (size_t i = 1; i < len; ++i) {
                g_ble_rpc_rx_buffer += static_cast<char>(raw[i]);
            }
        }

        if (flags & BLE_CHUNK_FLAG_END) {
            g_evt_rpc_ready = true; // 组包完毕，唤醒主线程解析
        }
    }
};

// -----------------------------------------------------
// 以下为业务处理函数，在主线程环境下运行是完全安全的
// -----------------------------------------------------
void add_config_to_response(JsonObject root) {
    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["ssid"] = g_config.wifi.ssid;
    wifi["password"] = g_config.wifi.password;
    wifi["dhcp"] = g_config.wifi.dhcp;
    wifi["ip"] = g_config.wifi.ip;
    wifi["gateway"] = g_config.wifi.gateway;
    wifi["subnet"] = g_config.wifi.subnet;
    wifi["dns1"] = g_config.wifi.dns1;
    wifi["dns2"] = g_config.wifi.dns2;

    device_config::RadioConfig radio_config = {};
    edit_controller_get_radio_config(radio_config);

    char tx_freq[16] = {0};
    char rx_freq[16] = {0};
    format_frequency_mhz(radio_config.tx_frequency_x10000, tx_freq, sizeof(tx_freq));
    format_frequency_mhz(radio_config.rx_frequency_x10000, rx_freq, sizeof(rx_freq));

    JsonObject radio = root["radio"].to<JsonObject>();
    radio["tx_frequency_mhz"] = tx_freq;
    radio["rx_frequency_mhz"] = rx_freq;
    radio["tx_tone_type"] = subaudio_type_to_string(radio_config.tx_subaudio.type);
    radio["tx_tone_index"] = radio_config.tx_subaudio.index;
    radio["rx_tone_type"] = subaudio_type_to_string(radio_config.rx_subaudio.type);
    radio["rx_tone_index"] = radio_config.rx_subaudio.index;
    radio["squelch"] = radio_config.squelch;
    radio["wide_band"] = radio_config.wide_band;
    radio["power_high"] = radio_config.power_high;
    radio["power_level"] = radio_config.power_high ? 3 : 1;

    JsonObject server = root["server"].to<JsonObject>();
    server["callsign"] = g_config.server.callsign;
    server["node_ssid"] = g_config.server.node_ssid;
    server["dmr_id"] = g_config.server.dmr_id;
    server["udp_host"] = g_config.server.udp_host;
    server["udp_port"] = g_config.server.udp_port;
    server["http_api_base_url"] = g_config.server.http_api_base_url;
    server["account"] = g_config.server.account;
    server["device_auth_password"] = g_config.server.device_auth_password;
}

bool parse_wifi_config(JsonObjectConst input, device_config::WiFiConfig &config, String &error) {
    device_config::set_defaults(config);
    copy_cstr(config.ssid, input["ssid"] | "");
    copy_cstr(config.password, input["password"] | "");
    config.dhcp = input["dhcp"] | true;
    copy_cstr(config.ip, input["ip"] | "");
    copy_cstr(config.gateway, input["gateway"] | "");
    copy_cstr(config.subnet, input["subnet"] | "");
    copy_cstr(config.dns1, input["dns1"] | "");
    copy_cstr(config.dns2, input["dns2"] | "");

    if (config.ssid[0] == '\0') {
        error = "WiFi SSID required";
        return false;
    }
    if (!config.dhcp) {
        IPAddress test_ip;
        if (!parse_ipv4_text(config.ip, test_ip) ||
            !parse_ipv4_text(config.gateway, test_ip) ||
            !parse_ipv4_text(config.subnet, test_ip)) {
            error = "Static IP fields invalid";
            return false;
        }
    }
    return true;
}

bool parse_radio_config(JsonObjectConst input, device_config::RadioConfig &config, String &error) {
    device_config::set_defaults(config);
    if (!parse_frequency_value(input["tx_frequency_mhz"], config.tx_frequency_x10000) ||
        !parse_frequency_value(input["rx_frequency_mhz"], config.rx_frequency_x10000)) {
        error = "Radio frequency invalid";
        return false;
    }
    config.tx_subaudio.type = subaudio_type_from_string(String(input["tx_tone_type"] | "OFF"));
    config.tx_subaudio.index = input["tx_tone_index"] | 0;
    config.rx_subaudio.type = subaudio_type_from_string(String(input["rx_tone_type"] | "OFF"));
    config.rx_subaudio.index = input["rx_tone_index"] | 0;
    config.squelch = input["squelch"] | 4;
    config.wide_band = input["wide_band"] | false;
    if (!input["power_high"].isNull()) {
        config.power_high = input["power_high"] | false;
    } else {
        const uint8_t power_level = input["power_level"] | (config.power_high ? 3 : 1);
        config.power_high = power_level >= 2;
    }

    if (config.tx_frequency_x10000 < 4000000UL || config.tx_frequency_x10000 > 4700000UL ||
        config.rx_frequency_x10000 < 4000000UL || config.rx_frequency_x10000 > 4700000UL) {
        error = "Frequency out of range";
        return false;
    }
    if (config.squelch > 8) {
        error = "Squelch must be 0-8";
        return false;
    }
    return true;
}

bool parse_server_config(JsonObjectConst input, device_config::ServerConfig &config, String &error) {
    device_config::set_defaults(config);
    copy_cstr(config.callsign, input["callsign"] | "");
    config.node_ssid = input["node_ssid"] | 0;
    config.dmr_id = input["dmr_id"] | 0;
    const char *udp_host = input["udp_host"] | "";
    if (udp_host[0] != '\0') {
        copy_cstr(config.udp_host, udp_host);
    }
    const uint16_t udp_port = input["udp_port"] | 0;
    if (udp_port != 0) {
        config.udp_port = udp_port;
    }
    const char *http_api_base_url = input["http_api_base_url"] | "";
    if (http_api_base_url[0] != '\0') {
        copy_cstr(config.http_api_base_url, http_api_base_url);
    }
    copy_cstr(config.account, input["account"] | "");
    copy_cstr(config.device_auth_password, input["device_auth_password"] | "");

    if (config.node_ssid != 0 && !device_config::is_valid_device_node_ssid(config.node_ssid)) {
        error = "Node SSID must be 1-99 or 106-235";
        return false;
    }
    if (config.dmr_id > 0xFFFFFFUL) {
        error = "DMR ID must be <= 16777215";
        return false;
    }
    return true;
}

void send_rpc_success(JsonVariantConst request, const std::function<void(JsonObject)> &fill_data = nullptr) {
    JsonDocument response;
    if (!request.isNull() && request["id"].is<int>()) {
        response["id"] = request["id"].as<int>();
    }
    response["ok"] = true;
    JsonObject data = response["data"].to<JsonObject>();
    if (fill_data) {
        fill_data(data);
    }

    String json;
    serializeJson(response, json);
    notify_rpc_json(json);
}

void cache_wifi_scan_results(int network_count) {
    if (network_count < 0) return;

    const int capped_count = std::min(network_count, static_cast<int>(WIFI_SCAN_CACHE_MAX));
    g_wifi_scan_cache_count = static_cast<uint8_t>(capped_count);

    for (int i = 0; i < capped_count; ++i) {
        const String ssid = WiFi.SSID(i);
        strncpy(g_wifi_scan_cache[i].ssid, ssid.c_str(), sizeof(g_wifi_scan_cache[i].ssid) - 1);
        g_wifi_scan_cache[i].ssid[sizeof(g_wifi_scan_cache[i].ssid) - 1] = '\0';
        g_wifi_scan_cache[i].rssi = WiFi.RSSI(i);
        g_wifi_scan_cache[i].auth = static_cast<int32_t>(WiFi.encryptionType(i));
    }

    for (int i = capped_count; i < static_cast<int>(WIFI_SCAN_CACHE_MAX); ++i) {
        g_wifi_scan_cache[i].ssid[0] = '\0';
        g_wifi_scan_cache[i].rssi = 0;
        g_wifi_scan_cache[i].auth = 0;
    }

    g_wifi_scan_cache_updated_at_ms = millis();
}

void append_cached_wifi_scan_results(JsonArray items) {
    for (uint8_t i = 0; i < g_wifi_scan_cache_count; ++i) {
        if (g_wifi_scan_cache[i].ssid[0] == '\0') continue;
        JsonObject item = items.add<JsonObject>();
        item["ssid"] = g_wifi_scan_cache[i].ssid;
        item["rssi"] = g_wifi_scan_cache[i].rssi;
        item["auth"] = g_wifi_scan_cache[i].auth;
    }
}

void scan_wifi_networks(JsonVariantConst request) {
    bool partial = false;
    bool timed_out = false;
    bool scan_in_progress = false;
    bool used_cache = false;
    bool fresh = false;

    int scan_count = -1;
    if (!g_wifi_scan_in_progress) {
        const int start_result = WiFi.scanNetworks(true, true);
        if (start_result >= 0) {
            scan_count = start_result;
        } else if (start_result == WIFI_SCAN_STATUS_RUNNING) {
            g_wifi_scan_in_progress = true;
        } else {
            partial = true;
            used_cache = true;
        }
    }

    const uint32_t wait_started_at_ms = millis();
    while (scan_count < 0 && g_wifi_scan_in_progress) {
        const int status = WiFi.scanComplete();
        if (status >= 0) {
            scan_count = status;
            g_wifi_scan_in_progress = false;
            break;
        }

        if (status == WIFI_SCAN_STATUS_FAILED) {
            g_wifi_scan_in_progress = false;
            break;
        }

        if ((millis() - wait_started_at_ms) >= WIFI_SCAN_WAIT_BUDGET_MS) {
            timed_out = true;
            scan_in_progress = true;
            break;
        }

        delay(WIFI_SCAN_POLL_INTERVAL_MS);
    }

    if (scan_count >= 0) {
        cache_wifi_scan_results(scan_count);
        WiFi.scanDelete();
        fresh = true;
        partial = false;
    } else {
        partial = true;
        used_cache = true;
    }

    const int32_t cache_age_ms = (g_wifi_scan_cache_updated_at_ms == 0)
        ? -1
        : static_cast<int32_t>(millis() - g_wifi_scan_cache_updated_at_ms);

    send_rpc_success(request, [partial, timed_out, scan_in_progress, used_cache, fresh, scan_count, cache_age_ms](JsonObject data) {
        data["partial"] = partial;
        data["timed_out"] = timed_out;
        data["scan_in_progress"] = scan_in_progress;
        data["used_cache"] = used_cache;
        data["fresh"] = fresh;
        data["total"] = scan_count >= 0 ? scan_count : static_cast<int>(g_wifi_scan_cache_count);
        data["cache_age_ms"] = cache_age_ms;
        JsonArray items = data["networks"].to<JsonArray>();
        append_cached_wifi_scan_results(items);
    });
}

void disconnect_ble_client() {
    if (g_ble_server && g_ble_transport_connected) {
        g_ble_server->disconnect(g_ble_conn_id);
    }
}

void process_rpc_request(const String &json_text) {
    JsonDocument request;
    const DeserializationError parse_result = deserializeJson(request, json_text);
    if (parse_result) {
        send_rpc_error(JsonVariantConst(), "Invalid JSON");
        return;
    }

    JsonVariantConst request_variant = request.as<JsonVariantConst>();
    const char *cmd = request["cmd"] | "";
    if (cmd[0] == '\0') {
        send_rpc_error(request_variant, "Missing cmd");
        return;
    }

    const bool auth_not_required = strcmp(cmd, "get_status") == 0;

    if (!g_ble_authenticated && !auth_not_required) {
        send_rpc_error(request_variant, "Not authenticated");
        return;
    }

    if (strcmp(cmd, "get_status") == 0) {
        send_rpc_success(request_variant, [](JsonObject data) {
            data["wifi_state"] = wifi_state_label(g_wifi_state);
            data["ble_state"] = static_cast<unsigned int>(current_ble_state());
            data["authenticated"] = g_ble_authenticated;
            data["rssi"] = g_wifi_rssi;
            data["device_name"] = g_ble_device_name;
        });
        return;
    }

    if (strcmp(cmd, "get_config") == 0) {
        send_rpc_success(request_variant, [](JsonObject data) {
            add_config_to_response(data);
        });
        return;
    }

    if (strcmp(cmd, "scan_wifi") == 0) {
        scan_wifi_networks(request_variant);
        return;
    }

    if (strcmp(cmd, "set_wifi") == 0) {
        device_config::WiFiConfig config = {};
        String parse_error;
        if (!parse_wifi_config(request["data"].as<JsonObjectConst>(), config, parse_error)) {
            send_rpc_error(request_variant, parse_error.c_str());
            return;
        }

        g_config.wifi = config;
        if (!device_config::save_wifi(g_config.wifi)) {
            send_rpc_error(request_variant, "Failed to save WiFi config");
            return;
        }

        begin_wifi_connect_attempt(true);
        send_rpc_success(request_variant, [](JsonObject data) {
            data["wifi_state"] = wifi_state_label(g_wifi_state);
        });
        return;
    }

    if (strcmp(cmd, "set_radio") == 0) {
        device_config::RadioConfig config = {};
        String parse_error;
        if (!parse_radio_config(request["data"].as<JsonObjectConst>(), config, parse_error)) {
            send_rpc_error(request_variant, parse_error.c_str());
            return;
        }

        if (!edit_controller_set_radio_config(config, true)) {
            send_rpc_error(request_variant, "Failed to apply radio config");
            return;
        }

        g_config.radio = config;
        net_audio_link_schedule_radio_config_sync();
        send_rpc_success(request_variant);
        return;
    }

    if (strcmp(cmd, "set_server") == 0) {
        device_config::ServerConfig config = {};
        String parse_error;
        if (!parse_server_config(request["data"].as<JsonObjectConst>(), config, parse_error)) {
            send_rpc_error(request_variant, parse_error.c_str());
            return;
        }

        g_config.server = config;
        if (!device_config::save_server(g_config.server)) {
            send_rpc_error(request_variant, "Failed to save server config");
            return;
        }

        send_rpc_success(request_variant);
        return;
    }

    if (strcmp(cmd, "disconnect") == 0) {
        send_rpc_success(request_variant);
        disconnect_ble_client();
        return;
    }

    send_rpc_error(request_variant, "Unknown cmd");
}


void init_ble_stack() {
    if (g_ble_stack_initialized) return;

    build_ble_device_name();

    BLEDevice::init(g_ble_device_name);

    g_ble_server = BLEDevice::createServer();
    g_ble_server->setCallbacks(new ProvisioningServerCallbacks());

    g_ble_service = g_ble_server->createService(BLE_SERVICE_UUID);

    g_ble_status_char = g_ble_service->createCharacteristic(
        BLE_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    g_ble_auth_char = g_ble_service->createCharacteristic(
        BLE_AUTH_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    g_ble_auth_char->setCallbacks(new AuthCharacteristicCallbacks());
    g_ble_auth_char->setValue("READY");

    g_ble_rpc_tx_char = g_ble_service->createCharacteristic(
        BLE_RPC_TX_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    g_ble_rpc_tx_char->setCallbacks(new RpcTxCharacteristicCallbacks());

    g_ble_rpc_rx_char = g_ble_service->createCharacteristic(
        BLE_RPC_RX_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    g_ble_service->start();
    g_ble_advertising = BLEDevice::getAdvertising();
    g_ble_advertising->addServiceUUID(BLE_SERVICE_UUID);
    g_ble_stack_initialized = true;
}

void disable_ble() {
    hide_ble_popup();

    if (g_ble_server && g_ble_transport_connected) {
        g_ble_server->disconnect(g_ble_conn_id);
    }
    if (g_ble_advertising) {
        g_ble_advertising->stop();
    }
    g_ble_enabled = false;
    g_ble_transport_connected = false;
    g_ble_authenticated = false;
    g_ble_idle_since_ms = 0;
    g_ble_rpc_rx_buffer = "";
    notify_status();
    Serial.println("[BLE] Disabled.");
}

void enable_ble(const char *reason, bool show_popup) {
    generate_auth_code();
    init_ble_stack();
    start_ble_advertising();

    g_ble_enabled = true;
    g_ble_transport_connected = false;
    g_ble_authenticated = false;
    g_ble_idle_since_ms = millis();
    if (g_ble_auth_char) {
        g_ble_auth_char->setValue("READY");
    }
    if (show_popup) {
        show_ble_popup(reason);
    } else {
        hide_ble_popup();
    }
    notify_status();
    Serial.printf("[BLE] Enabled with auth code %s\n", g_ble_auth_code);
}

void maybe_start_ble_for_boot_failure(const char *reason) {
    suspend_wifi_retry_for_ble_provisioning();

    if (g_ble_enabled) {
        show_ble_popup(reason);
        notify_status();
        return;
    }
    enable_ble(reason, true);
}

void suspend_wifi_retry_for_ble_provisioning() {
    g_wifi_attempt_active = false;
    g_wifi_next_retry_at_ms = 0;
    g_wifi_retry_suspended = true;

    if (WiFi.status() != WL_CONNECTED) {
        g_wifi_manual_disconnect = true;
        WiFi.disconnect(false, false);
    }
}


void handle_wifi_attempt_failure(const char *reason) {
    g_wifi_attempt_active = false;
    g_wifi_state = WiFiState::FAILED;
    g_wifi_rssi = 0;

    if (!g_wifi_connected_once) {
        if (g_wifi_boot_failures < 255) {
            ++g_wifi_boot_failures;
        }

        if (g_wifi_boot_failures >= WIFI_BOOT_FAILURE_LIMIT) {
            maybe_start_ble_for_boot_failure("WiFi link failed repeatedly. BLE GATT provisioning is active.");
            notify_status();
            Serial.printf("[WIFI] Boot connect failed: %s\n", reason ? reason : "unknown");
            return;
        }
    }

    schedule_wifi_retry(WIFI_RETRY_DELAY_MS);
    notify_status();
    Serial.printf("[WIFI] Connect failure: %s\n", reason ? reason : "unknown");
}

void on_wifi_event(arduino_event_id_t event) {
    // 仅仅改变标志位，绝不直接操作带 LVGL 渲染的业务逻辑！
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            g_evt_wifi_connected = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            g_evt_wifi_disconnected = true;
            break;
        default:
            break;
    }
}
} // namespace

void connectivity_manager_init() {
    if (g_manager_initialized) return;

    device_config::set_defaults(g_config);
    device_config::load(g_config);
    edit_controller_get_radio_config(g_config.radio);

    // Initialize BLE once before Wi-Fi starts to avoid late BT controller enable
    // aborting in coexistence setup on ESP32-S3.
    init_ble_stack();

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    // ESP32-S3 requires Wi-Fi modem sleep to stay enabled while BLE is active.
    WiFi.setSleep(true);
    WiFi.onEvent(on_wifi_event);

    g_wifi_state = device_config::has_wifi_credentials(g_config.wifi) ? WiFiState::FAILED : WiFiState::NO_CONFIG;
    g_wifi_rssi = 0;
    g_manager_initialized = true;
}

void connectivity_manager_on_main_screen_enter() {
    if (!g_manager_initialized) {
        connectivity_manager_init();
    }

    g_main_screen_ready = true;
    refresh_status_icons();

    if (!device_config::has_wifi_credentials(g_config.wifi)) {
        maybe_start_ble_for_boot_failure("WiFi configuration missing. BLE GATT provisioning is active.");
        g_wifi_state = WiFiState::NO_CONFIG;
        notify_status();
        return;
    }

    begin_wifi_connect_attempt(true);
}

void connectivity_manager_update() {
    if (!g_manager_initialized) return;

    // ===============================================
    // 处理所有后台触发的异步事件（100% 在主线程下进行）
    // ===============================================

    // 处理 WiFi 连接成功
    if (g_evt_wifi_connected) {
        g_evt_wifi_connected = false;
        g_wifi_attempt_active = false;
        g_wifi_state = WiFiState::CONNECTED;
        g_wifi_connected_once = true;
        g_wifi_boot_failures = 0;
        g_wifi_rssi = WiFi.RSSI();
        notify_status();
        Serial.printf("[WIFI] Connected. IP=%s\n", WiFi.localIP().toString().c_str());
    }

    // 处理 WiFi 断开事件
    if (g_evt_wifi_disconnected) {
        g_evt_wifi_disconnected = false;
        g_wifi_rssi = 0;
        if (g_wifi_manual_disconnect) {
            g_wifi_manual_disconnect = false;
            notify_status();
        } else if (g_wifi_retry_suspended) {
            notify_status();
        } else if (!g_wifi_attempt_active &&
                   g_wifi_state != WiFiState::CONNECTING &&
                   g_wifi_state != WiFiState::CONNECTED) {
            // Ignore unrelated/disordered disconnect events while not attempting a link.
            notify_status();
        } else {
            handle_wifi_attempt_failure("station disconnected");
        }
    }

    // 处理 BLE 客户端连接
    if (g_evt_ble_connected) {
        g_evt_ble_connected = false;
        g_ble_transport_connected = true;
        g_ble_authenticated = false;
        g_ble_idle_since_ms = millis();
        notify_status();
        Serial.println("[BLE] Client connected.");
    }

    // 处理 BLE 客户端断开
    if (g_evt_ble_disconnected) {
        g_evt_ble_disconnected = false;
        g_ble_transport_connected = false;
        g_ble_authenticated = false;
        g_ble_idle_since_ms = millis();
        notify_status();
        if (g_ble_enabled) {
            start_ble_advertising();
        }
        Serial.println("[BLE] Client disconnected.");
    }

    // 处理 BLE 配对码校验通过
    if (g_evt_ble_auth_success) {
        g_evt_ble_auth_success = false;
        g_ble_authenticated = true;
        g_ble_idle_since_ms = millis();
        hide_ble_popup();
        notify_status();
        Serial.println("[BLE] Dynamic code authenticated.");
    }

    // 处理 BLE 配对码校验失败
    if (g_evt_ble_auth_failed) {
        g_evt_ble_auth_failed = false;
        g_ble_authenticated = false;
        notify_status();
        Serial.println("[BLE] Dynamic code authentication failed.");
    }

    // 处理完整的 RPC 指令请求
    if (g_evt_rpc_ready) {
        g_evt_rpc_ready = false;
        process_rpc_request(g_ble_rpc_rx_buffer);
        g_ble_rpc_rx_buffer = "";
    }

    // ===============================================
    // 常规主循环更新逻辑
    // ===============================================
    if (g_wifi_state == WiFiState::CONNECTED) {
        g_wifi_rssi = WiFi.RSSI();
    }

    const uint32_t now_ms = millis();

    if (g_wifi_attempt_active && (now_ms - g_wifi_attempt_started_at_ms) >= WIFI_CONNECT_TIMEOUT_MS) {
        handle_wifi_attempt_failure("timeout");
    } else if (!g_wifi_retry_suspended && !g_wifi_attempt_active &&
               g_wifi_next_retry_at_ms != 0 && now_ms >= g_wifi_next_retry_at_ms) {
        begin_wifi_connect_attempt(false);
    }

    if (g_ble_enabled && !g_ble_authenticated && g_ble_idle_since_ms != 0 && (now_ms - g_ble_idle_since_ms) >= BLE_IDLE_TIMEOUT_MS) {
        disable_ble();
    }

    refresh_status_icons();
}

bool connectivity_manager_handle_encoder_event(EC11Event event, int32_t value) {
    (void)value;

    if (!g_ble_popup_visible) {
        return false;
    }

    if (event == EC11Event::BUTTON_CLICK) {
        hide_ble_popup();
        return true;
    }

    return false;
}

bool connectivity_manager_set_ble_enabled(bool enable, bool show_popup) {
    if (!g_manager_initialized) {
        connectivity_manager_init();
    }

    if (enable) {
        if (g_ble_enabled) {
            if (show_popup) {
                show_ble_popup("Manual BLE provisioning is active.");
            } else {
                hide_ble_popup();
            }
            notify_status();
            return true;
        }

        enable_ble(show_popup ? "Manual BLE provisioning is active." : nullptr, show_popup);
        return true;
    }

    if (!g_ble_enabled) {
        hide_ble_popup();
        notify_status();
        return true;
    }

    disable_ble();
    return true;
}

bool connectivity_manager_is_ble_enabled() {
    return g_ble_enabled;
}

const char *connectivity_manager_get_ble_auth_code() {
    return g_ble_auth_code;
}

const char *connectivity_manager_get_ble_device_name() {
    if (g_ble_device_name[0] == '\0') {
        build_ble_device_name();
    }
    return g_ble_device_name;
}
