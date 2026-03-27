#include "i2s_driver.h"
#include "../config.h"
#include <string.h>
#include <stdlib.h>

static uint8_t current_volume = 100;
static bool is_muted = false;

bool i2s_driver_init(void) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DMA_BUF_COUNT,
        .dma_buf_len = I2S_DMA_BUF_LEN,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN,
    };

    esp_err_t ret = i2s_driver_install(I2S_PORT_NUM, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        return false;
    }

    ret = i2s_set_pin(I2S_PORT_NUM, &pin_config);
    if (ret != ESP_OK) {
        i2s_driver_uninstall(I2S_PORT_NUM);
        return false;
    }

    // 清空 DMA 缓冲区
    i2s_zero_dma_buffer(I2S_PORT_NUM);

    return true;
}

void i2s_driver_deinit(void) {
    i2s_driver_uninstall(I2S_PORT_NUM);
}

bool i2s_write(const void *data, size_t len, size_t *bytes_written) {
    if (is_muted) {
        *bytes_written = len;
        return true;
    }

    // 应用软件音量
    if (current_volume < 100) {
        int16_t *samples = (int16_t *)malloc(len);
        if (samples) {
            memcpy(samples, data, len);
            float scale = current_volume / 100.0f;
            for (size_t i = 0; i < len / 2; i++) {
                samples[i] = (int16_t)(samples[i] * scale);
            }
            esp_err_t ret = i2s_write_expand(I2S_PORT_NUM, samples, len, 16, 32, bytes_written, portMAX_DELAY);
            free(samples);
            return ret == ESP_OK;
        }
    }

    esp_err_t ret = i2s_write_expand(I2S_PORT_NUM, data, len, 16, 32, bytes_written, portMAX_DELAY);
    return ret == ESP_OK;
}

bool i2s_read(void *data, size_t len, size_t *bytes_read) {
    esp_err_t ret = i2s_read(I2S_PORT_NUM, data, len, bytes_read, portMAX_DELAY);
    return ret == ESP_OK;
}

void i2s_set_sample_rate(uint32_t rate) {
    i2s_set_sample_rates(I2S_PORT_NUM, rate);
}

void i2s_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    current_volume = volume;
}

uint8_t i2s_get_volume(void) {
    return current_volume;
}

void i2s_set_mute(bool mute) {
    is_muted = mute;
}
