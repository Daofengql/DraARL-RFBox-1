#ifndef EC11_DRIVER_H
#define EC11_DRIVER_H

#include <Arduino.h>
#include <functional>
#include <ESP32RotaryEncoder.h>

// 编码器方向
enum class EC11Direction {
    NONE = 0,
    CLOCKWISE,         // 顺时针
    COUNTER_CLOCKWISE  // 逆时针
};

// 事件类型
enum class EC11Event {
    NONE = 0,
    ROTATE_CW,      // 顺时针旋转
    ROTATE_CCW,     // 逆时针旋转
    BUTTON_PRESS,   // 按键按下
    BUTTON_RELEASE, // 按键释放
    BUTTON_CLICK    // 按键单击
};

// 回调函���类型
using EC11Callback = std::function<void(EC11Event event, int32_t value)>;

/**
 * @brief 初始化 EC11 编码器驱动
 * @return true 成功, false 失败
 */
bool ec11_init(void);

/**
 * @brief 卸载 EC11 驱动
 */
void ec11_deinit(void);

/**
 * @brief 设置事件回调函数
 * @param callback 回调函数
 */
void ec11_set_callback(EC11Callback callback);

/**
 * @brief 获取当前计数值
 * @return 计数值
 */
int32_t ec11_get_counter(void);

/**
 * @brief 重置计数值
 * @param value 新计数值 (默认0)
 */
void ec11_reset_counter(int32_t value = 0);

/**
 * @brief 获取按键状态
 * @return true 按下, false 释放
 */
bool ec11_get_button_state(void);

/**
 * @brief 获取当前方向
 * @return 方向
 */
EC11Direction ec11_get_direction(void);

/**
 * @brief 设置旋转加速度 (连续旋转时增量增加)
 * @param enable 是否启用
 */
void ec11_set_acceleration(bool enable);

/**
 * @brief 设置去抖时间 (ms)
 * @param debounce_ms 去抖时间
 */
void ec11_set_debounce(uint32_t debounce_ms);

/**
 * @brief 处理编码器事件 (需在主循环中调用)
 */
void ec11_update(void);

#endif // EC11_DRIVER_H
