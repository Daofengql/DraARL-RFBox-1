#include "ec11_driver.h"
#include "../config.h"

// RotaryEncoder 实例
static RotaryEncoder* encoder = nullptr;

// 状态变量
static int32_t counter = 0;
static EC11Direction last_direction = EC11Direction::NONE;
static bool button_pressed = false;
static bool acceleration_enabled = false;

static EC11Callback event_callback = nullptr;

// 加速相关
static uint32_t last_rotate_time = 0;
static int rotate_speed = 1;
static int32_t last_encoder_value = 0;

// 旋转回调 - 库传递的是编码器累计值
static void on_knob_turned(long value) {
    int32_t diff = value - last_encoder_value;

    if (diff > 0) {
        last_direction = EC11Direction::CLOCKWISE;

        // 计算加速
        if (acceleration_enabled) {
            uint32_t time_diff = millis() - last_rotate_time;
            if (time_diff < 50) {
                rotate_speed = 5;
            } else if (time_diff < 100) {
                rotate_speed = 3;
            } else if (time_diff < 200) {
                rotate_speed = 2;
            } else {
                rotate_speed = 1;
            }
            last_rotate_time = millis();
        } else {
            rotate_speed = 1;
        }

        counter += rotate_speed;

        if (event_callback) {
            event_callback(EC11Event::ROTATE_CW, rotate_speed);
        }
    } else if (diff < 0) {
        last_direction = EC11Direction::COUNTER_CLOCKWISE;

        // 计算加速
        if (acceleration_enabled) {
            uint32_t time_diff = millis() - last_rotate_time;
            if (time_diff < 50) {
                rotate_speed = 5;
            } else if (time_diff < 100) {
                rotate_speed = 3;
            } else if (time_diff < 200) {
                rotate_speed = 2;
            } else {
                rotate_speed = 1;
            }
            last_rotate_time = millis();
        } else {
            rotate_speed = 1;
        }

        counter -= rotate_speed;

        if (event_callback) {
            event_callback(EC11Event::ROTATE_CCW, -rotate_speed);
        }
    }

    last_encoder_value = value;
}

// 按键回调 - 库传递按键按下持续时间(ms)
static void on_button_pressed(unsigned long duration) {
    button_pressed = false;  // 按键已释放

    if (event_callback) {
        event_callback(EC11Event::BUTTON_RELEASE, (int32_t)duration);
        // 短按视为单击
        if (duration < 500) {
            event_callback(EC11Event::BUTTON_CLICK, (int32_t)duration);
        }
    }
}

bool ec11_init(void) {
    // 创建 RotaryEncoder 实例 (CLK=A, DT=B, SW)
    encoder = new RotaryEncoder(EC11_CLK, EC11_DT, EC11_SW);

    if (!encoder) {
        return false;
    }

    // 设置编码器类型 (使用外部上拉电阻)
    encoder->setEncoderType(EncoderType::HAS_PULLUP);

    // 设置边界 (无边界限制)
    encoder->setBoundaries(-1000000, 1000000, false);

    // 设置回调
    encoder->onTurned(&on_knob_turned);
    encoder->onPressed(&on_button_pressed);

    // 初始化
    encoder->begin();

    return true;
}

void ec11_deinit(void) {
    if (encoder) {
        delete encoder;
        encoder = nullptr;
    }
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
    // 这个库内置了去抖，不需要手动设置
}

void ec11_update(void) {
    // 这个库使用中断，不需要在主循环中轮询
}
