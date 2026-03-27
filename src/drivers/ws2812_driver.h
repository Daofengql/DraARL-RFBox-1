#ifndef WS2812_DRIVER_H
#define WS2812_DRIVER_H

#include <Arduino.h>
#include <FastLED.h>

// LED 数量
#define WS2812_LED_COUNT    1

// 使用 FastLED 的 CRGB 和 CHSV 类型

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
void ws2812_set_pixel(uint16_t index, CRGB color);
void ws2812_set_pixel_hsv(uint16_t index, CHSV color);

/**
 * @brief 设置所有 LED 颜色
 */
void ws2812_set_all(CRGB color);
void ws2812_set_all_hsv(CHSV color);

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

void ws2812_breath(CRGB color, uint32_t duration_ms = 2000);
void ws2812_rainbow(uint32_t duration_ms = 3000);
void ws2812_blink(CRGB color, uint32_t interval_ms = 500);
void ws2812_stop_effect(void);
void ws2812_update(uint32_t current_time);

uint16_t ws2812_get_count(void);

// 获取内部 LED 数组 (用于直接 FastLED 操作)
CRGB* ws2812_get_leds(void);

#endif // WS2812_DRIVER_H
