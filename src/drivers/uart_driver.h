#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <Arduino.h>
#include <driver/uart.h>

// UART 配置参数
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      9600
#define UART_BUF_SIZE       256
#define UART_READ_TIMEOUT   100  // ms

/**
 * @brief 初始化 UART 驱动
 * @return true 成功, false 失败
 */
bool uart_driver_init(void);

/**
 * @brief 卸载 UART 驱动
 */
void uart_driver_deinit(void);

/**
 * @brief 发送数据
 * @param data 数据指针
 * @param len 数据长度
 * @return 实际发送字节数, -1 表示失败
 */
int uart_send(const uint8_t *data, size_t len);

/**
 * @brief 发送字符串
 * @param str 字符串
 * @return 实际发送字节数, -1 表示失败
 */
int uart_send_string(const char *str);

/**
 * @brief 接收数据
 * @param data 数据缓冲区
 * @param len 最大接收长度
 * @param timeout_ms 超时时间(ms)
 * @return 实际接收字节数, -1 表示失败
 */
int uart_receive(uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief 清空接收缓冲区
 */
void uart_flush_rx(void);

/**
 * @brief 检查可读数据长度
 * @return 可读数据长度
 */
int uart_available_bytes(void);

#endif // UART_DRIVER_H
