#include <Arduino.h>
#include <lvgl.h>

#include <cstring>

#include "config.h"
#include "drivers/display.h"
#include "logic/app_logic.h"
#include "logic/device_config.h"
#include "logic/edit_controller.h"
#include "logic/ota_update.h"
#include "ui/ui.h"

SET_LOOP_TASK_STACK_SIZE(32768);

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

static constexpr uint32_t DRAW_BUF_SIZE = SCREEN_WIDTH * SCREEN_HEIGHT / 10;

static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static float current_brightness = 1.0f;

static void log_boot_identity() {
    Serial.printf("[FW] Boot version=%s built=%s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
    Serial.printf("[MEM] heap_free=%u psram_found=%d psram_free=%u psram_total=%u\n",
                  static_cast<unsigned int>(ESP.getFreeHeap()),
                  psramFound() ? 1 : 0,
                  static_cast<unsigned int>(ESP.getFreePsram()),
                  static_cast<unsigned int>(ESP.getPsramSize()));
}

void updateBacklight(float level) {
    if (level < 0.0f) {
        level = 0.0f;
    }
    if (level > 1.0f) {
        level = 1.0f;
    }

    const int max_duty = (1 << PWM_RESOLUTION) - 1;
    const int duty_cycle = static_cast<int>(level * max_duty);
    ledcWrite(PWM_CHANNEL, duty_cycle);
}

static bool load_pending_update_info(ota_update::FirmwareInfo &info) {
    device_config::DeviceConfig config = {};
    device_config::set_defaults(config);
    device_config::load(config);

    if (!config.ota.has_pending_update || config.ota.download_url[0] == '\0') {
        return false;
    }

    memset(&info, 0, sizeof(info));
    info.has_update = true;
    strncpy(info.version, config.ota.available_version, sizeof(info.version) - 1);
    info.version[sizeof(info.version) - 1] = '\0';
    strncpy(info.download_url, config.ota.download_url, sizeof(info.download_url) - 1);
    info.download_url[sizeof(info.download_url) - 1] = '\0';
    strncpy(info.file_hash, config.ota.file_hash, sizeof(info.file_hash) - 1);
    info.file_hash[sizeof(info.file_hash) - 1] = '\0';
    strncpy(info.file_hash_algorithm, config.ota.file_hash_algorithm, sizeof(info.file_hash_algorithm) - 1);
    info.file_hash_algorithm[sizeof(info.file_hash_algorithm) - 1] = '\0';
    info.file_size = config.ota.file_size;
    return true;
}

static void clear_pending_update_state() {
    device_config::DeviceConfig config = {};
    device_config::set_defaults(config);
    if (!device_config::load(config)) {
        return;
    }

    config.ota.has_pending_update = false;
    config.ota.available_version[0] = '\0';
    config.ota.download_url[0] = '\0';
    config.ota.file_hash[0] = '\0';
    config.ota.file_hash_algorithm[0] = '\0';
    config.ota.file_size = 0;
    if (device_config::save_ota(config.ota)) {
        edit_controller_set_update_available(false);
    }
}

static void on_update_button_clicked(lv_event_t *e) {
    Serial.println("Update button event triggered");

    ota_update::FirmwareInfo info = {};
    if (!load_pending_update_info(info)) {
        if (!ota_update::check_for_update(info)) {
            Serial.println("Failed to check for updates");
            return;
        }
    }

    if (!info.has_update || info.download_url[0] == '\0') {
        Serial.println("No update available");
        clear_pending_update_state();
        return;
    }

    Serial.printf("Starting OTA update to version %s\n", info.version);

    if (ota_update::start_update(info.download_url, info.file_hash, info.file_hash_algorithm)) {
        Serial.println("OTA update successful, restarting...");
        Serial.println("Pending OTA state kept until next boot reconciliation.");
        delay(1000);
        ESP.restart();
    } else {
        Serial.printf("OTA update failed: %s\n", ota_update::get_error_message());
    }
}

void setup() {
    // Keep backlight off at boot until PWM is ready.
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, LOW);

    Serial.begin(115200);
    log_boot_identity();

    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(BACKLIGHT_PIN, PWM_CHANNEL);
    // Keep backlight off until startup flow begins.
    updateBacklight(0.0f);

    initDisplay();
    lv_init();

    buf1 = static_cast<lv_color_t *>(heap_caps_malloc(
        DRAW_BUF_SIZE * sizeof(lv_color_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    ));
    buf2 = static_cast<lv_color_t *>(heap_caps_malloc(
        DRAW_BUF_SIZE * sizeof(lv_color_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    ));

    if (!buf1 || !buf2) {
        Serial.println("LVGL buffer allocation failed.");
        while (true) {
            delay(1000);
        }
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DRAW_BUF_SIZE);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = flushDisplay;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init();

    if (ui_updateBT) {
        lv_obj_add_event_cb(ui_updateBT, on_update_button_clicked, LV_EVENT_CLICKED, nullptr);
    }

    app_logic_init();

    Serial.println("System initialization complete.");
}

void loop() {
    app_logic_update();

    lv_timer_handler();
    delay(5);
}
