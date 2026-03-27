#ifndef WS2812_DRIVER_H
#define WS2812_DRIVER_H

#include <Arduino.h>

// LED 数量
#define WS2812_LED_COUNT    1

// RGB 颜色结构
struct RGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    RGB() : r(0), g(0), b(0) {}
    RGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

    static RGB black()   { return RGB(0, 0, 0); }
    static RGB white()   { return RGB(255, 255, 255); }
    static RGB red()     { return RGB(255, 0, 0); }
    static RGB green()   { return RGB(0, 255, 0); }
    static RGB blue()    { return RGB(0, 0, 255); }
    static RGB yellow()  { return RGB(255, 255, 0); }
    static RGB cyan()    { return RGB(0, 255, 255); }
    static RGB magenta() { return RGB(255, 0, 255); }
    static RGB orange()  { return RGB(255, 165, 0); }
    static RGB purple()  { return RGB(128, 0, 128); }
    static RGB pink()    { return RGB(255, 192, 203); }
};

// HSV 颜色结构
struct HSV {
    uint16_t h;  // 色相 0-360
    uint8_t s;   // 饱和度 0-255
    uint8_t v;   // 明度 0-255

    HSV() : h(0), s(255), v(255) {}
    HSV(uint16_t hue, uint8_t sat, uint8_t val) : h(hue), s(sat), v(val) {}

    RGB to_rgb() const;
};

/**
 * @brief 初始化 WS2812 LED 驱动
 * @param led_count LED 数量 (默认为1)
 * @return true 成功, false 失败
 */
bool ws2812_init(uint16_t led_count = WS2812_LED_COUNT);

/**
 * @brief 卸载 WS2812 驱动
 */
void ws2812_deinit(void);

/**
 * @brief 设置单个 LED 颜色
 */
void ws2812_set_pixel(uint16_t index, RGB color);
void ws2812_set_pixel_hsv(uint16_t index, HSV color);

/**
 * @brief 设置所有 LED 颜色
 */
void ws2812_set_all(RGB color);
void ws2812_set_all_hsv(HSV color);

/**
 * @brief 清除所有 LED (关闭)
 */
void ws2812_clear(void);

/**
 * @brief 更新 LED 显示
 */
void ws2812_show(void);

/**
 * @brief 设置亮度 (0-255)
 */
void ws2812_set_brightness(uint8_t brightness);
uint8_t ws2812_get_brightness(void);

// ==================== 预设效果 ====================

void ws2812_breath(RGB color, uint32_t duration_ms = 2000);
void ws2812_rainbow(uint32_t duration_ms = 3000);
void ws2812_blink(RGB color, uint32_t interval_ms = 500);
void ws2812_stop_effect(void);
void ws2812_update(uint32_t current_time);

uint16_t ws2812_get_count(void);

#endif // WS2812_DRIVER_H
