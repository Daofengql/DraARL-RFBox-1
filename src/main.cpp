#include <Arduino.h>
#include <lvgl.h>

#include "config.h"
#include "drivers/display.h"
#include "logic/app_logic.h"
#include "ui/ui.h"

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

static constexpr uint32_t DRAW_BUF_SIZE = SCREEN_WIDTH * SCREEN_HEIGHT / 10;

static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static float current_brightness = 1.0f;

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

void setup() {
    Serial.begin(115200);

    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(BACKLIGHT_PIN, PWM_CHANNEL);
    updateBacklight(current_brightness);

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
    app_logic_init();

    Serial.println("System initialization complete.");
}

void loop() {
    app_logic_update();

    lv_timer_handler();
    delay(5);
}
