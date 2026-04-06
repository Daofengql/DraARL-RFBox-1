#include "uart_driver.h"
#include "../config.h"
#include <cstring>

bool uart_driver_init(void) {
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,  // 使用 XTAL 时钟源
    };

    if (uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0) != ESP_OK) {
        return false;
    }

    if (uart_param_config(UART_PORT_NUM, &uart_cfg) != ESP_OK) {
        uart_driver_delete(UART_PORT_NUM);
        return false;
    }

    // Cross wiring: ESP TX -> SA818_RXD, ESP RX -> SA818_TXD
    if (uart_set_pin(UART_PORT_NUM, SA818_RXD, SA818_TXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(UART_PORT_NUM);
        return false;
    }

    return true;
}

void uart_driver_deinit(void) {
    uart_driver_delete(UART_PORT_NUM);
}

int uart_send(const uint8_t *data, size_t len) {
    return uart_write_bytes(UART_PORT_NUM, (const char *)data, len);
}

int uart_send_string(const char *str) {
    return uart_write_bytes(UART_PORT_NUM, str, strlen(str));
}

int uart_receive(uint8_t *data, size_t len, uint32_t timeout_ms) {
    return uart_read_bytes(UART_PORT_NUM, data, len, pdMS_TO_TICKS(timeout_ms));
}

void uart_flush_rx(void) {
    uart_flush_input(UART_PORT_NUM);
}

int uart_available_bytes(void) {
    size_t size = 0;
    uart_get_buffered_data_len(UART_PORT_NUM, &size);
    return (int)size;
}
