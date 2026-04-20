#include "http_request_gate.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace http_request_gate {
namespace {

SemaphoreHandle_t g_http_mutex = nullptr;
portMUX_TYPE g_http_owner_lock = portMUX_INITIALIZER_UNLOCKED;
char g_http_owner[32] = {0};

void ensure_mutex_created() {
    if (g_http_mutex) {
        return;
    }

    SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
    if (!new_mutex) {
        Serial.println("[HTTP_GATE] Failed to create mutex.");
        return;
    }

    taskENTER_CRITICAL(&g_http_owner_lock);
    if (!g_http_mutex) {
        g_http_mutex = new_mutex;
        new_mutex = nullptr;
    }
    taskEXIT_CRITICAL(&g_http_owner_lock);

    if (new_mutex) {
        vSemaphoreDelete(new_mutex);
    }
}

void set_owner(const char *owner) {
    taskENTER_CRITICAL(&g_http_owner_lock);
    if (!owner) {
        g_http_owner[0] = '\0';
    } else {
        strncpy(g_http_owner, owner, sizeof(g_http_owner) - 1);
        g_http_owner[sizeof(g_http_owner) - 1] = '\0';
    }
    taskEXIT_CRITICAL(&g_http_owner_lock);
}

} // namespace

void init() {
    ensure_mutex_created();
}

bool acquire(const char *owner, uint32_t timeout_ms) {
    ensure_mutex_created();
    if (!g_http_mutex) {
        return false;
    }

    if (xSemaphoreTake(g_http_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        Serial.printf("[HTTP_GATE] busy. requester=%s owner=%s timeout=%lu ms\n",
                      owner ? owner : "<null>",
                      current_owner(),
                      static_cast<unsigned long>(timeout_ms));
        return false;
    }

    set_owner(owner);
    return true;
}

void release(const char *owner) {
    (void)owner;
    if (!g_http_mutex) {
        return;
    }

    set_owner(nullptr);
    xSemaphoreGive(g_http_mutex);
}

const char *current_owner() {
    return (g_http_owner[0] != '\0') ? g_http_owner : "<none>";
}

ScopedLock::ScopedLock(const char *owner, uint32_t timeout_ms)
    : owner_(owner),
      locked_(acquire(owner, timeout_ms)) {}

ScopedLock::~ScopedLock() {
    if (locked_) {
        release(owner_);
    }
}

bool ScopedLock::locked() const {
    return locked_;
}

} // namespace http_request_gate
