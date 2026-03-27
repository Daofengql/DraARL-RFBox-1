#include "ws2812_driver.h"
#include "../config.h"

// FastLED LED 数组
static CRGB* leds = nullptr;
static uint16_t led_count = 0;
static uint8_t brightness = 255;

// 效果相关
enum class EffectType {
    NONE = 0,
    BREATH,
    RAINBOW,
    BLINK
};
static EffectType current_effect = EffectType::NONE;
static CRGB effect_color;
static uint32_t effect_duration = 0;
static uint32_t effect_start_time = 0;

bool ws2812_init(uint16_t count) {
    led_count = count;

    // 分配 LED 数组
    leds = new CRGB[led_count];
    if (!leds) {
        return false;
    }

    // 初始化 FastLED
    FastLED.addLeds<WS2812, WS2812_PIN, GRB>(leds, led_count);
    FastLED.setBrightness(brightness);
    FastLED.clear(true);

    return true;
}

void ws2812_deinit(void) {
    if (leds) {
        delete[] leds;
        leds = nullptr;
    }
    led_count = 0;
}

void ws2812_set_pixel(uint16_t index, CRGB color) {
    if (index < led_count && leds) {
        leds[index] = color;
    }
}

void ws2812_set_pixel_hsv(uint16_t index, CHSV color) {
    if (index < led_count && leds) {
        leds[index] = color;
    }
}

void ws2812_set_all(CRGB color) {
    if (leds) {
        fill_solid(leds, led_count, color);
    }
}

void ws2812_set_all_hsv(CHSV color) {
    if (leds) {
        fill_solid(leds, led_count, color);
    }
}

void ws2812_clear(void) {
    if (leds) {
        FastLED.clear(true);
    }
}

void ws2812_show(void) {
    FastLED.show();
}

void ws2812_set_brightness(uint8_t value) {
    brightness = value;
    FastLED.setBrightness(brightness);
}

uint8_t ws2812_get_brightness(void) {
    return brightness;
}

void ws2812_breath(CRGB color, uint32_t duration_ms) {
    current_effect = EffectType::BREATH;
    effect_color = color;
    effect_duration = duration_ms;
    effect_start_time = millis();
}

void ws2812_rainbow(uint32_t duration_ms) {
    current_effect = EffectType::RAINBOW;
    effect_duration = duration_ms;
    effect_start_time = millis();
}

void ws2812_blink(CRGB color, uint32_t interval_ms) {
    current_effect = EffectType::BLINK;
    effect_color = color;
    effect_duration = interval_ms;
    effect_start_time = millis();
}

void ws2812_stop_effect(void) {
    current_effect = EffectType::NONE;
}

void ws2812_update(uint32_t current_time) {
    if (current_effect == EffectType::NONE) {
        return;
    }

    uint32_t elapsed = current_time - effect_start_time;

    switch (current_effect) {
        case EffectType::BREATH: {
            float phase = (float)(elapsed % effect_duration) / effect_duration;
            float sine = (sin(phase * 2 * 3.14159f) + 1) / 2;

            uint8_t r = (uint8_t)(effect_color.r * sine);
            uint8_t g = (uint8_t)(effect_color.g * sine);
            uint8_t b = (uint8_t)(effect_color.b * sine);

            ws2812_set_all(CRGB(r, g, b));
            ws2812_show();
            break;
        }

        case EffectType::RAINBOW: {
            uint8_t hue = (uint8_t)((elapsed % effect_duration) * 255 / effect_duration);
            fill_rainbow(leds, led_count, hue, 255 / led_count);
            ws2812_show();
            break;
        }

        case EffectType::BLINK: {
            bool on = (elapsed / effect_duration) % 2 == 0;

            if (on) {
                ws2812_set_all(effect_color);
            } else {
                ws2812_clear();
            }
            ws2812_show();
            break;
        }

        default:
            break;
    }
}

uint16_t ws2812_get_count(void) {
    return led_count;
}

CRGB* ws2812_get_leds(void) {
    return leds;
}
