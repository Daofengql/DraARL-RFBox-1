/****************************************************************
 * 最小化 LVGL 启动程序
 ****************************************************************/

#include <Arduino.h>
#include <lvgl.h>
#include "config.h"
#include "drivers/display.h"
#include "ui/ui.h"

// LVGL 显示缓冲区
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
const uint32_t buf_size = SCREEN_WIDTH * SCREEN_HEIGHT / 10;
static lv_color_t *buf1;
static lv_color_t *buf2;

// 背光亮度
float currentBrightness = 1.0f;

/**
 * @brief 设置背光亮度
 */
void updateBacklight(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    int maxDuty = (1 << PWM_RESOLUTION) - 1;
    int dutyCycle = (int)(level * maxDuty);
    ledcWrite(PWM_CHANNEL, dutyCycle);
}

void setup() {
    Serial.begin(115200);
    Serial.println("LVGL 启动中...");

    // 背光初始化
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(BACKLIGHT_PIN, PWM_CHANNEL);
    updateBacklight(currentBrightness);

    // 显示初始化
    initDisplay();

    // LVGL 初始化
    lv_init();

    // 分配显示缓冲区
    buf1 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = flushDisplay;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // 初始化 UI
    ui_init();

    Serial.println("LVGL 初始化完成");
}

void loop() {
    lv_timer_handler();
    delay(5);
}
