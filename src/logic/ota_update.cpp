#include "ota_update.h"
#include "device_config.h"
#include "http_response_buffer.h"
#include "http_request_gate.h"
#include "version_utils.h"
#include "../config.h"

#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <esp_heap_caps.h>

#include <cctype>
#include <cstring>

namespace ota_update {
namespace {

OTAState current_state = OTAState::IDLE;
uint8_t download_progress = 0;
char error_message[128] = {0};

enum class HashVerificationMode : uint8_t {
    NONE = 0,
    MD5,
    SHA256,
};

constexpr size_t OTA_RESPONSE_PREVIEW_MAX = 192;
constexpr uint32_t OTA_HTTP_GATE_WAIT_MS = 10000;
constexpr size_t OTA_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr size_t OTA_API_BASE_CANDIDATE_MAX = 3;

void set_error(const char *msg) {
    if (msg) {
        strncpy(error_message, msg, sizeof(error_message) - 1);
        error_message[sizeof(error_message) - 1] = '\0';
    } else {
        error_message[0] = '\0';
    }
}

bool append_api_base_candidate(const char *base,
                               const char **candidates,
                               size_t &candidate_count) {
    if (!base || base[0] == '\0' || !candidates || candidate_count >= OTA_API_BASE_CANDIDATE_MAX) {
        return false;
    }

    for (size_t i = 0; i < candidate_count; ++i) {
        if (strcmp(candidates[i], base) == 0) {
            return false;
        }
    }

    candidates[candidate_count++] = base;
    return true;
}

bool equals_ignore_case(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return lhs == rhs;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (std::tolower(static_cast<unsigned char>(*lhs)) !=
            std::tolower(static_cast<unsigned char>(*rhs))) {
            return false;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

bool is_hex_string(const char *value, size_t expected_len) {
    if (!value || strlen(value) != expected_len) {
        return false;
    }

    for (size_t i = 0; i < expected_len; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

const char *infer_hash_algorithm(const char *hash) {
    if (is_hex_string(hash, 32)) {
        return "md5";
    }
    if (is_hex_string(hash, 64)) {
        return "sha256";
    }
    return "";
}

HashVerificationMode resolve_hash_mode(const char *algorithm, const char *hash) {
    if (algorithm && algorithm[0] != '\0') {
        if (equals_ignore_case(algorithm, "md5")) {
            return is_hex_string(hash, 32) ? HashVerificationMode::MD5 : HashVerificationMode::NONE;
        }
        if (equals_ignore_case(algorithm, "sha256")) {
            return is_hex_string(hash, 64) ? HashVerificationMode::SHA256 : HashVerificationMode::NONE;
        }
        return HashVerificationMode::NONE;
    }

    if (is_hex_string(hash, 32)) {
        return HashVerificationMode::MD5;
    }
    if (is_hex_string(hash, 64)) {
        return HashVerificationMode::SHA256;
    }
    return HashVerificationMode::NONE;
}

void bytes_to_hex_lower(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";

    if (!out || out_len == 0) {
        return;
    }

    if (!bytes || out_len < (len * 2U + 1U)) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = HEX_DIGITS[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

void log_response_preview(const char *response, size_t response_len) {
    if (!response) {
        Serial.println("[OTA] Response preview unavailable (null body).");
        return;
    }

    String preview;
    preview.reserve(OTA_RESPONSE_PREVIEW_MAX + 8);
    const size_t preview_len = response_len > OTA_RESPONSE_PREVIEW_MAX ? OTA_RESPONSE_PREVIEW_MAX : response_len;
    for (size_t i = 0; i < preview_len; ++i) {
        const char ch = response[i];
        if (ch == '\r') {
            preview += "\\r";
        } else if (ch == '\n') {
            preview += "\\n";
        } else {
            preview += ch;
        }
    }
    if (response_len > preview_len) {
        preview += "...";
    }

    Serial.printf("[OTA] Response preview (%u bytes): %s\n",
                  static_cast<unsigned int>(response_len),
                  preview.c_str());
}

bool parse_firmware_response(const char *json_str, size_t json_len, FirmwareInfo &info) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_str, json_len);

    if (error) {
        Serial.printf("[OTA] JSON parse failed: %s\n", error.c_str());
        set_error("JSON parse failed");
        return false;
    }

    // 检查是否有 data 字段（服务器返回格式）
    JsonObject data = doc["data"].as<JsonObject>();
    if (data.isNull()) {
        Serial.println("[OTA] Response data is null, server says no newer version.");
        info.has_update = false;
        return true;
    }

    if (data["has_update"].is<bool>() && !data["has_update"].as<bool>()) {
        Serial.println("[OTA] Response has_update=false.");
        info.has_update = false;
        return true;
    }

    if (!data["version"].is<const char*>()) {
        Serial.println("[OTA] Response missing version field, treating as no update.");
        info.has_update = false;
        return true;
    }

    const char *remote_version = data["version"];
    if (version_utils::compare(remote_version, FIRMWARE_VERSION) <= 0) {
        Serial.printf("[OTA] Remote version (%s) is not newer than local version (%s).\n",
                      remote_version,
                      FIRMWARE_VERSION);
        info.has_update = false;
        return true;
    }

    info.has_update = true;
    strncpy(info.version, remote_version, sizeof(info.version) - 1);
    info.version[sizeof(info.version) - 1] = '\0';

    if (data["changelog"].is<const char*>()) {
        strncpy(info.changelog, data["changelog"], sizeof(info.changelog) - 1);
        info.changelog[sizeof(info.changelog) - 1] = '\0';
    } else {
        info.changelog[0] = '\0';
    }

    if (data["download_url"].is<const char*>()) {
        strncpy(info.download_url, data["download_url"], sizeof(info.download_url) - 1);
        info.download_url[sizeof(info.download_url) - 1] = '\0';
    } else {
        set_error("No download URL");
        return false;
    }

    if (data["file_hash"].is<const char*>()) {
        strncpy(info.file_hash, data["file_hash"], sizeof(info.file_hash) - 1);
        info.file_hash[sizeof(info.file_hash) - 1] = '\0';
    } else {
        info.file_hash[0] = '\0';
    }

    if (data["hash_algo"].is<const char*>()) {
        strncpy(info.file_hash_algorithm, data["hash_algo"], sizeof(info.file_hash_algorithm) - 1);
        info.file_hash_algorithm[sizeof(info.file_hash_algorithm) - 1] = '\0';
    } else {
        strncpy(info.file_hash_algorithm, infer_hash_algorithm(info.file_hash), sizeof(info.file_hash_algorithm) - 1);
        info.file_hash_algorithm[sizeof(info.file_hash_algorithm) - 1] = '\0';
    }

    info.file_size = data["file_size"] | 0;

    Serial.printf("[OTA] Parsed update: version=%s size=%lu hash_algo=%s url=%s\n",
                  info.version,
                  static_cast<unsigned long>(info.file_size),
                  info.file_hash_algorithm[0] != '\0' ? info.file_hash_algorithm : "<none>",
                  info.download_url[0] != '\0' ? info.download_url : "<empty>");

    return true;
}

} // anonymous namespace

void init() {
    current_state = OTAState::IDLE;
    download_progress = 0;
    error_message[0] = '\0';
}

bool check_for_update(FirmwareInfo &info) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[OTA] Check skipped, WiFi not connected (status=%d)\n", static_cast<int>(WiFi.status()));
        set_error("WiFi not connected");
        return false;
    }

    current_state = OTAState::CHECKING;
    memset(&info, 0, sizeof(info));

    device_config::DeviceConfig config;
    device_config::load(config);

    const char *api_base_candidates[OTA_API_BASE_CANDIDATE_MAX] = {};
    size_t api_base_candidate_count = 0;
    append_api_base_candidate(config.server.http_api_base_url,
                              api_base_candidates,
                              api_base_candidate_count);
    if (device_config::server_default_migration_available() &&
        device_config::is_default_or_legacy_server_http_api_base(config.server.http_api_base_url)) {
        append_api_base_candidate(device_config::default_server_http_api_base_url(),
                                  api_base_candidates,
                                  api_base_candidate_count);
        append_api_base_candidate(device_config::legacy_server_http_api_base_url(),
                                  api_base_candidates,
                                  api_base_candidate_count);
    }

    Serial.printf("[OTA] Check context: base=%s model=%d current=%s auto=%d pending=%d wifi_ip=%s rssi=%ld\n",
                  config.server.http_api_base_url[0] != '\0' ? config.server.http_api_base_url : "<empty>",
                  DEVICE_MODEL,
                  FIRMWARE_VERSION,
                  config.ota.auto_check_enabled ? 1 : 0,
                  config.ota.has_pending_update ? 1 : 0,
                  WiFi.localIP().toString().c_str(),
                  static_cast<long>(WiFi.RSSI()));

    http_request_gate::ScopedLock http_lock("OTA_CHECK", OTA_HTTP_GATE_WAIT_MS);
    if (!http_lock.locked()) {
        Serial.printf("[OTA] HTTP gate busy for check. owner=%s\n", http_request_gate::current_owner());
        set_error("HTTP gate busy");
        current_state = OTAState::FAILED;
        return false;
    }

    if (api_base_candidate_count == 0) {
        set_error("Empty OTA API base");
        current_state = OTAState::FAILED;
        return false;
    }

    for (size_t i = 0; i < api_base_candidate_count; ++i) {
        FirmwareInfo attempt_info = {};

        char url[256];
        snprintf(url,
                 sizeof(url),
                 "%s/api/public/firmware/latest?dev_model=%d&current_version=%s",
                 api_base_candidates[i],
                 DEVICE_MODEL,
                 FIRMWARE_VERSION);

        Serial.printf("[OTA] GET %s%s\n",
                      url,
                      (api_base_candidate_count > 1) ? " (candidate)" : "");

        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        http.begin(client, url);
        http.setTimeout(10000);

        const int http_code = http.GET();
        Serial.printf("[OTA] HTTP GET finished with code=%d base=%s\n",
                      http_code,
                      api_base_candidates[i]);

        if (http_code == 404) {
            Serial.println("[OTA] Server returned 404 for firmware metadata.");
            http.end();
            if ((i + 1U) < api_base_candidate_count) {
                Serial.println("[OTA] Trying next OTA API base after 404.");
                continue;
            }
            info.has_update = false;
            current_state = OTAState::IDLE;
            Serial.println("[OTA] No firmware published for this model.");
            return true;
        }

        if (http_code != 200) {
            Serial.printf("[OTA] HTTP GET failed: %s\n", http.errorToString(http_code).c_str());
            snprintf(error_message, sizeof(error_message), "HTTP error: %d", http_code);
            http.end();
            if ((i + 1U) < api_base_candidate_count) {
                Serial.println("[OTA] Trying next OTA API base after HTTP failure.");
                continue;
            }
            current_state = OTAState::FAILED;
            return false;
        }

        http_response_buffer::Buffer response = {};
        if (!http_response_buffer::read_all(http, response, "OTA_CHECK")) {
            set_error("Read response failed");
            http.end();
            http_response_buffer::release(response);
            if ((i + 1U) < api_base_candidate_count) {
                Serial.println("[OTA] Trying next OTA API base after read failure.");
                continue;
            }
            current_state = OTAState::FAILED;
            return false;
        }
        http.end();
        log_response_preview(response.data, response.length);

        const bool result = parse_firmware_response(response.data, response.length, attempt_info);
        http_response_buffer::release(response);
        Serial.printf("[OTA] Parse result: ok=%d has_update=%d version=%s base=%s\n",
                      result ? 1 : 0,
                      attempt_info.has_update ? 1 : 0,
                      attempt_info.version[0] != '\0' ? attempt_info.version : "<none>",
                      api_base_candidates[i]);
        if (result) {
            info = attempt_info;
            current_state = OTAState::IDLE;
            return true;
        }

        if ((i + 1U) < api_base_candidate_count) {
            Serial.println("[OTA] Trying next OTA API base after parse failure.");
            continue;
        }

        current_state = OTAState::FAILED;
        return false;
    }

    current_state = OTAState::FAILED;
    return false;
}

bool start_update(const char *url, const char *expected_hash, const char *expected_hash_algorithm) {
    if (!url || strlen(url) == 0) {
        set_error("Invalid URL");
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        set_error("WiFi not connected");
        return false;
    }

    current_state = OTAState::DOWNLOADING;
    download_progress = 0;

    const HashVerificationMode hash_mode = resolve_hash_mode(expected_hash_algorithm, expected_hash);

    http_request_gate::ScopedLock http_lock("OTA_UPDATE", OTA_HTTP_GATE_WAIT_MS);
    if (!http_lock.locked()) {
        Serial.printf("[OTA] HTTP gate busy for update. owner=%s\n", http_request_gate::current_owner());
        set_error("HTTP gate busy");
        current_state = OTAState::FAILED;
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(60000);

    int http_code = http.GET();
    if (http_code != 200) {
        snprintf(error_message, sizeof(error_message), "Download failed: %d", http_code);
        current_state = OTAState::FAILED;
        http.end();
        return false;
    }

    int content_length = http.getSize();
    if (content_length <= 0) {
        set_error("Invalid content length");
        current_state = OTAState::FAILED;
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();

    current_state = OTAState::INSTALLING;

    if (!Update.begin(content_length, U_FLASH)) {
        snprintf(error_message, sizeof(error_message), "Update begin failed: %s", Update.errorString());
        current_state = OTAState::FAILED;
        http.end();
        return false;
    }

    if (hash_mode == HashVerificationMode::MD5) {
        Update.setMD5(expected_hash);
    } else if (expected_hash && expected_hash[0] != '\0' && hash_mode == HashVerificationMode::NONE) {
        set_error("Unsupported firmware hash");
        Update.abort();
        current_state = OTAState::FAILED;
        http.end();
        return false;
    }

    size_t written = 0;
    bool download_buffer_in_psram = false;
    uint8_t *buffer = nullptr;
    if (psramFound()) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(OTA_DOWNLOAD_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        download_buffer_in_psram = (buffer != nullptr);
    }
    if (!buffer) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(OTA_DOWNLOAD_BUFFER_SIZE, MALLOC_CAP_8BIT));
        download_buffer_in_psram = false;
    }
    if (!buffer) {
        set_error("Download buffer alloc failed");
        Update.abort();
        current_state = OTAState::FAILED;
        http.end();
        return false;
    }
    Serial.printf("[OTA] Download buffer size=%u source=%s free_heap=%u free_psram=%u\n",
                  static_cast<unsigned int>(OTA_DOWNLOAD_BUFFER_SIZE),
                  download_buffer_in_psram ? "PSRAM" : "HEAP",
                  static_cast<unsigned int>(ESP.getFreeHeap()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

    bool sha256_active = false;
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);

    if (hash_mode == HashVerificationMode::SHA256) {
        if (mbedtls_sha256_starts_ret(&sha256_ctx, 0) != 0) {
            set_error("SHA256 init failed");
            Update.abort();
            current_state = OTAState::FAILED;
            http.end();
            heap_caps_free(buffer);
            mbedtls_sha256_free(&sha256_ctx);
            return false;
        }
        sha256_active = true;
    }

    while (http.connected() && (written < content_length)) {
        size_t available = stream->available();
        if (available) {
            size_t to_read = (available > OTA_DOWNLOAD_BUFFER_SIZE) ? OTA_DOWNLOAD_BUFFER_SIZE : available;
            size_t read_bytes = stream->readBytes(buffer, to_read);

            if (read_bytes == 0) {
                delay(1);
                continue;
            }

            if (sha256_active && mbedtls_sha256_update_ret(&sha256_ctx, buffer, read_bytes) != 0) {
                set_error("SHA256 update failed");
                Update.abort();
                current_state = OTAState::FAILED;
                http.end();
                heap_caps_free(buffer);
                mbedtls_sha256_free(&sha256_ctx);
                return false;
            }

            if (Update.write(buffer, read_bytes) != read_bytes) {
                set_error("Write failed");
                Update.abort();
                current_state = OTAState::FAILED;
                http.end();
                heap_caps_free(buffer);
                mbedtls_sha256_free(&sha256_ctx);
                return false;
            }

            written += read_bytes;
            download_progress = (written * 100) / content_length;
        }
        delay(1);
    }

    http.end();
    heap_caps_free(buffer);

    if (written != static_cast<size_t>(content_length)) {
        set_error("Incomplete download");
        Update.abort();
        current_state = OTAState::FAILED;
        mbedtls_sha256_free(&sha256_ctx);
        return false;
    }

    if (sha256_active) {
        uint8_t digest[32] = {0};
        char digest_hex[65] = {0};
        if (mbedtls_sha256_finish_ret(&sha256_ctx, digest) != 0) {
            set_error("SHA256 finish failed");
            Update.abort();
            current_state = OTAState::FAILED;
            mbedtls_sha256_free(&sha256_ctx);
            return false;
        }

        bytes_to_hex_lower(digest, sizeof(digest), digest_hex, sizeof(digest_hex));
        mbedtls_sha256_free(&sha256_ctx);

        if (!equals_ignore_case(digest_hex, expected_hash)) {
            snprintf(error_message,
                     sizeof(error_message),
                     "SHA256 mismatch");
            Update.abort();
            current_state = OTAState::FAILED;
            return false;
        }
    } else {
        mbedtls_sha256_free(&sha256_ctx);
    }

    if (!Update.end(true)) {
        snprintf(error_message, sizeof(error_message), "Update end failed: %s", Update.errorString());
        current_state = OTAState::FAILED;
        return false;
    }

    current_state = OTAState::SUCCESS;
    return true;
}

OTAState get_state() {
    return current_state;
}

uint8_t get_progress() {
    return download_progress;
}

const char* get_error_message() {
    return error_message;
}

void reset_state() {
    current_state = OTAState::IDLE;
    download_progress = 0;
    error_message[0] = '\0';
}

} // namespace ota_update
