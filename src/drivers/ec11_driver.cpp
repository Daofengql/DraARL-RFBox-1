#include "ec11_driver.h"
#include "../config.h"

// 状态变量
static int32_t counter = 0;
static EC11Direction last_direction = EC11Direction::NONE;
static bool button_pressed = false;
static bool acceleration_enabled = false;
static uint32_t debounce_ms = 2;

static EC11Callback event_callback = nullptr;

// 去抖和状态跟踪
static int last_clk_state = 1;
static int last_dt_state = 1;
static int last_sw_state = 1;
static uint32_t last_update_time = 0;

// 按键检测
static uint32_t press_time = 0;
static bool waiting_release = false;

// 加速相关
static uint32_t last_rotate_time = 0;
static int rotate_speed = 1;

bool ec11_init(void) {
    // 配置 GPIO 为输入模式 (外部已有上拉)
    pinMode(EC11_CLK, INPUT);
    pinMode(EC11_DT, INPUT);
    pinMode(EC11_SW, INPUT);

    // 读取初始状态
    last_clk_state = digitalRead(EC11_CLK);
    last_dt_state = digitalRead(EC11_DT);
    last_sw_state = digitalRead(EC11_SW);

    return true;
}

void ec11_deinit(void) {
    // Arduino 框架下无需特殊处理
    event_callback = nullptr;
}

void ec11_set_callback(EC11Callback callback) {
    event_callback = callback;
}

int32_t ec11_get_counter(void) {
    return counter;
}

void ec11_reset_counter(int32_t value) {
    counter = value;
}

bool ec11_get_button_state(void) {
    return button_pressed;
}

EC11Direction ec11_get_direction(void) {
    return last_direction;
}

void ec11_set_acceleration(bool enable) {
    acceleration_enabled = enable;
}

void ec11_set_debounce(uint32_t ms) {
    debounce_ms = ms;
}

void ec11_update(void) {
    uint32_t current_time = millis();

    // 去抖检查
    if (current_time - last_update_time < debounce_ms) {
        return;
    }
    last_update_time = current_time;

    // 读取当前状态 (外部上拉, 按下为低电平)
    int clk_state = digitalRead(EC11_CLK);
    int dt_state = digitalRead(EC11_DT);
    int sw_state = digitalRead(EC11_SW);

    // 检测旋转
    if (clk_state != last_clk_state) {
        if (clk_state == 0) {  // CLK 下降沿
            if (dt_state == 1) {
                // 顺时针
                last_direction = EC11Direction::CLOCKWISE;

                // 计算加速
                if (acceleration_enabled) {
                    uint32_t time_diff = current_time - last_rotate_time;
                    if (time_diff < 50) {
                        rotate_speed = 5;
                    } else if (time_diff < 100) {
                        rotate_speed = 3;
                    } else if (time_diff < 200) {
                        rotate_speed = 2;
                    } else {
                        rotate_speed = 1;
                    }
                    last_rotate_time = current_time;
                } else {
                    rotate_speed = 1;
                }

                counter += rotate_speed;

                if (event_callback) {
                    event_callback(EC11Event::ROTATE_CW, rotate_speed);
                }
            } else {
                // 逆时针
                last_direction = EC11Direction::COUNTER_CLOCKWISE;

                if (acceleration_enabled) {
                    uint32_t time_diff = current_time - last_rotate_time;
                    if (time_diff < 50) {
                        rotate_speed = 5;
                    } else if (time_diff < 100) {
                        rotate_speed = 3;
                    } else if (time_diff < 200) {
                        rotate_speed = 2;
                    } else {
                        rotate_speed = 1;
                    }
                    last_rotate_time = current_time;
                } else {
                    rotate_speed = 1;
                }

                counter -= rotate_speed;

                if (event_callback) {
                    event_callback(EC11Event::ROTATE_CCW, -rotate_speed);
                }
            }
        }
        last_clk_state = clk_state;
        last_dt_state = dt_state;
    }

    // 检测按键 (外部上拉, 按下为低电平)
    if (sw_state != last_sw_state) {
        if (sw_state == 0) {
            // 按键按下
            button_pressed = true;
            press_time = current_time;
            waiting_release = true;

            if (event_callback) {
                event_callback(EC11Event::BUTTON_PRESS, 0);
            }
        } else {
            // 按键释放
            button_pressed = false;

            if (event_callback) {
                event_callback(EC11Event::BUTTON_RELEASE, 0);
                // 如果按下时间短于500ms, 触发单击事件
                if (waiting_release && (current_time - press_time < 500)) {
                    event_callback(EC11Event::BUTTON_CLICK, 0);
                }
            }
            waiting_release = false;
        }
        last_sw_state = sw_state;
    }
}
