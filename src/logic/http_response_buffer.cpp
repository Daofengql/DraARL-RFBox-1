#include "http_response_buffer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace http_response_buffer {
namespace {

constexpr size_t HTTP_RESPONSE_FALLBACK_CAPACITY = 1024;
constexpr uint32_t HTTP_RESPONSE_READ_IDLE_TIMEOUT_MS = 4000;

char *allocate_buffer(size_t capacity, bool &from_psram) {
    from_psram = false;

    if (psramFound()) {
        char *psram_buffer = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (psram_buffer) {
            from_psram = true;
            return psram_buffer;
        }
    }

    return static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_8BIT));
}

bool ensure_capacity(Buffer &buffer, size_t required_capacity, const char *tag) {
    if (buffer.data && required_capacity <= buffer.capacity) {
        return true;
    }

    bool from_psram = false;
    char *replacement = allocate_buffer(required_capacity, from_psram);
    if (!replacement) {
        Serial.printf("[%s] Response buffer allocation failed. need=%u free_heap=%u free_psram=%u\n",
                      tag ? tag : "HTTP",
                      static_cast<unsigned int>(required_capacity),
                      static_cast<unsigned int>(ESP.getFreeHeap()),
                      static_cast<unsigned int>(ESP.getFreePsram()));
        return false;
    }

    if (buffer.data && buffer.length > 0) {
        memcpy(replacement, buffer.data, buffer.length);
    }
    if (buffer.data) {
        heap_caps_free(buffer.data);
    }

    buffer.data = replacement;
    buffer.capacity = required_capacity;
    buffer.from_psram = from_psram;
    return true;
}

} // namespace

bool read_all(HTTPClient &http, Buffer &buffer, const char *tag) {
    if (buffer.data) {
        release(buffer);
    }
    buffer.data = nullptr;
    buffer.length = 0;
    buffer.capacity = 0;
    buffer.from_psram = false;

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        return false;
    }

    const int reported_length = http.getSize();
    size_t capacity = reported_length > 0
        ? static_cast<size_t>(reported_length) + 1U
        : HTTP_RESPONSE_FALLBACK_CAPACITY;

    if (!ensure_capacity(buffer, capacity, tag)) {
        return false;
    }

    Serial.printf("[%s] Response buffer cap=%u source=%s free_heap=%u free_psram=%u\n",
                  tag ? tag : "HTTP",
                  static_cast<unsigned int>(capacity),
                  buffer.from_psram ? "PSRAM" : "HEAP",
                  static_cast<unsigned int>(ESP.getFreeHeap()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

    const uint32_t started_at = millis();
    uint32_t last_data_at = started_at;

    while (true) {
        const size_t available = stream->available();
        if (available > 0) {
            const size_t required = buffer.length + available + 1U;
            if (required > capacity) {
                size_t new_capacity = capacity;
                while (new_capacity < required) {
                    new_capacity *= 2U;
                }
                if (!ensure_capacity(buffer, new_capacity, tag)) {
                    release(buffer);
                    return false;
                }
                capacity = new_capacity;
            }

            const size_t read_bytes = stream->readBytes(
                reinterpret_cast<uint8_t *>(buffer.data + buffer.length),
                available
            );
            buffer.length += read_bytes;
            buffer.data[buffer.length] = '\0';
            last_data_at = millis();

            if (reported_length > 0 && buffer.length >= static_cast<size_t>(reported_length)) {
                break;
            }
            continue;
        }

        if (reported_length > 0 && buffer.length >= static_cast<size_t>(reported_length)) {
            break;
        }
        if (!http.connected()) {
            break;
        }
        if ((millis() - last_data_at) > HTTP_RESPONSE_READ_IDLE_TIMEOUT_MS) {
            Serial.printf("[%s] Response read idle timeout after %u ms\n",
                          tag ? tag : "HTTP",
                          static_cast<unsigned int>(millis() - started_at));
            break;
        }

        delay(1);
    }

    if (!buffer.data) {
        return false;
    }

    buffer.data[buffer.length] = '\0';
    return true;
}

void release(Buffer &buffer) {
    if (buffer.data) {
        heap_caps_free(buffer.data);
    }
    buffer.data = nullptr;
    buffer.length = 0;
    buffer.capacity = 0;
    buffer.from_psram = false;
}

} // namespace http_response_buffer
