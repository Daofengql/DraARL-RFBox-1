#include "ws2812_driver.h"
#include "../config.h"
#include <esp32-hal-rgb-led.h>

static uint16_t led_count = 0;
static RGB *led_buffer = NULL;
static uint8_t brightness = 255;

// 效果相关
enum class EffectType {
    NONE = 0,
    BREATH,
    RAINBOW,
    BLINK
};
static EffectType current_effect = EffectType::NONE;
static RGB effect_color;
static uint32_t effect_duration = 0;
static uint32_t effect_start_time = 0;

// HSV 转 RGB
RGB HSV::to_rgb() const {
    RGB rgb;

    if (s == 0) {
        rgb.r = rgb.g = rgb.b = v;
        return rgb;
    }

    uint16_t hue = h % 360;
    uint8_t region = hue / 60;
    uint16_t remainder = (hue - (region * 60)) * 256 / 60;

    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0: rgb.r = v; rgb.g = t; rgb.b = p; break;
        case 1: rgb.r = q; rgb.g = v; rgb.b = p; break;
        case 2: rgb.r = p; rgb.g = v; rgb.b = t; break;
        case 3: rgb.r = p; rgb.g = q; rgb.b = v; break;
        case 4: rgb.r = t; rgb.g = p; rgb.b = v; break;
        default: rgb.r = v; rgb.g = p; rgb.b = q; break;
    }

    return rgb;
}

bool ws2812_init(uint16_t count) {
    led_count = count;

    led_buffer = (RGB *)calloc(led_count, sizeof(RGB));
    if (!led_buffer) {
        return false;
    }

    return true;
}

void ws2812_deinit(void) {
    if (led_buffer) {
        free(led_buffer);
        led_buffer = NULL;
    }
    led_count = 0;
}

void ws2812_set_pixel(uint16_t index, RGB color) {
    if (index < led_count && led_buffer) {
        led_buffer[index] = color;
    }
}

void ws2812_set_pixel_hsv(uint16_t index, HSV color) {
    ws2812_set_pixel(index, color.to_rgb());
}

void ws2812_set_all(RGB color) {
    if (led_buffer) {
        for (uint16_t i = 0; i < led_count; i++) {
            led_buffer[i] = color;
        }
    }
}

void ws2812_set_all_hsv(HSV color) {
    ws2812_set_all(color.to_rgb());
}

void ws2812_clear(void) {
    ws2812_set_all(RGB::black());
    ws2812_show();
}

void ws2812_show(void) {
    if (!led_buffer || led_count == 0) {
        return;
    }

    // 使用 Arduino 内置的 neopixelWrite 函数
    for (uint16_t i = 0; i < led_count; i++) {
        uint8_t r = (led_buffer[i].r * brightness) >> 8;
        uint8_t g = (led_buffer[i].g * brightness) >> 8;
        uint8_t b = (led_buffer[i].b * brightness) >> 8;

        // 简单的 WS2812 时序控制
        // 使用 ESP32 的 RMT 或 bit-bang
        uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;

        noInterrupts();
        for (int bit = 23; bit >= 0; bit--) {
            if (grb & (1 << bit)) {
                // 1码: 高电平约 0.8us, 低电平约 0.45us
                digitalWrite(WS2812_PIN, HIGH);
                delayMicroseconds(1);
                digitalWrite(WS2812_PIN, LOW);
                delayMicroseconds(1);
            } else {
                // 0码: 高电平约 0.4us, 低电平约 0.85us
                digitalWrite(WS2812_PIN, HIGH);
                delayMicroseconds(1);
                digitalWrite(WS2812_PIN, LOW);
                delayMicroseconds(2);
            }
        }
        interrupts();
    }

    // 复位时间
    delayMicroseconds(50);
}

void ws2812_set_brightness(uint8_t value) {
    brightness = value;
}

uint8_t ws2812_get_brightness(void) {
    return brightness;
}

void ws2812_breath(RGB color, uint32_t duration_ms) {
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

void ws2812_blink(RGB color, uint32_t interval_ms) {
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

            ws2812_set_all(RGB(r, g, b));
            ws2812_show();
            break;
        }

        case EffectType::RAINBOW: {
            float phase = (float)(elapsed % effect_duration) / effect_duration;
            uint16_t hue = (uint16_t)(phase * 360);

            ws2812_set_all_hsv(HSV(hue, 255, 255));
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
