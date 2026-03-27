#include "es8388_driver.h"
#include "i2c_driver.h"

static uint8_t current_dac_volume = 50;
static uint8_t current_adc_volume = 50;

// ES8388 寄存器定义
enum ES8388Reg {
    REG_CHIP_CONTROL1      = 0x00,
    REG_CHIP_CONTROL2      = 0x01,
    REG_CHIP_POWER         = 0x02,
    REG_ADC_POWER          = 0x03,
    REG_DAC_POWER          = 0x04,
    REG_ADC_CTRL1          = 0x09,
    REG_ADC_CTRL2          = 0x0A,
    REG_ADC_CTRL3          = 0x0B,
    REG_ADC_CTRL4          = 0x0C,
    REG_DAC_CTRL1          = 0x18,
    REG_DAC_CTRL2          = 0x19,
    REG_DAC_CTRL3          = 0x1A,
    REG_DAC_CTRL6          = 0x1D,
    REG_LDAC_VOL           = 0x30,
    REG_RDAC_VOL           = 0x31,
    REG_LADC_VOL           = 0x32,
    REG_RADC_VOL           = 0x33,
};

// 写寄存器
static bool write_reg(uint8_t reg, uint8_t value) {
    return i2c_write_byte(ES8388_ADDR, reg, value);
}

// 读寄存器
static bool read_reg(uint8_t reg, uint8_t *value) {
    return i2c_read_byte(ES8388_ADDR, reg, value);
}

bool es8388_init(void) {
    // 复位芯片
    es8388_reset();

    // 等待复位完成
    delay(10);

    // 初始化序列
    write_reg(REG_CHIP_POWER, 0xFF);   // 所有部分上电
    write_reg(REG_CHIP_CONTROL1, 0x06); // 正常工作模式
    write_reg(REG_CHIP_CONTROL2, 0x50); // 时钟配置

    // ADC 配置
    write_reg(REG_ADC_CTRL1, 0x00);    // 正常模式
    write_reg(REG_ADC_CTRL2, 0x00);    // 16位 I2S
    write_reg(REG_ADC_CTRL3, 0x0C);    // ADC 使能, LIN1/RIN1
    write_reg(REG_ADC_POWER, 0x09);    // ADC 电源使能

    // DAC 配置
    write_reg(REG_DAC_CTRL1, 0x00);    // 正常模式
    write_reg(REG_DAC_CTRL2, 0x02);    // 16位 I2S
    write_reg(REG_DAC_CTRL3, 0x04);    // DAC 使能
    write_reg(REG_DAC_POWER, 0x0C);    // DAC 电源使能

    // 设置默认音量
    es8388_set_dac_volume(50);
    es8388_set_adc_volume(50);

    return true;
}

void es8388_deinit(void) {
    es8388_enter_low_power();
}

bool es8388_is_present(void) {
    uint8_t value;
    return read_reg(REG_CHIP_CONTROL1, &value);
}

void es8388_set_format(ES8388Format format) {
    uint8_t adc_val = 0, dac_val = 0;
    read_reg(REG_ADC_CTRL2, &adc_val);
    read_reg(REG_DAC_CTRL2, &dac_val);

    adc_val = (adc_val & 0xFC) | ((uint8_t)format & 0x03);
    dac_val = (dac_val & 0xFC) | ((uint8_t)format & 0x03);

    write_reg(REG_ADC_CTRL2, adc_val);
    write_reg(REG_DAC_CTRL2, dac_val);
}

void es8388_set_sample_rate(ES8388SampleRate rate) {
    uint8_t val = 0;
    read_reg(REG_ADC_CTRL1, &val);
    val = (val & 0x1F) | ((uint8_t)rate << 5);
    write_reg(REG_ADC_CTRL1, val);

    read_reg(REG_DAC_CTRL1, &val);
    val = (val & 0x1F) | ((uint8_t)rate << 5);
    write_reg(REG_DAC_CTRL1, val);
}

void es8388_set_bits_per_sample(uint8_t bits) {
    uint8_t adc_val = 0, dac_val = 0;
    read_reg(REG_ADC_CTRL2, &adc_val);
    read_reg(REG_DAC_CTRL2, &dac_val);

    uint8_t bits_val = 0;
    switch (bits) {
        case 16: bits_val = 0x00; break;
        case 24: bits_val = 0x01; break;
        case 32: bits_val = 0x02; break;
        default: bits_val = 0x00; break;
    }

    adc_val = (adc_val & 0x03) | (bits_val << 2);
    dac_val = (dac_val & 0x03) | (bits_val << 2);

    write_reg(REG_ADC_CTRL2, adc_val);
    write_reg(REG_DAC_CTRL2, dac_val);
}

void es8388_set_adc_input(ES8388Input input) {
    uint8_t val = 0;
    read_reg(REG_ADC_CTRL3, &val);

    switch (input) {
        case ES8388Input::INPUT1:
            val = (val & 0x3F) | 0x00;
            break;
        case ES8388Input::INPUT2:
            val = (val & 0x3F) | 0x40;
            break;
        case ES8388Input::INPUT_DIFF:
            val = (val & 0x3F) | 0x80;
            break;
    }
    write_reg(REG_ADC_CTRL3, val);
}

void es8388_set_adc_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    current_adc_volume = volume;

    uint8_t reg_val = (volume * 192) / 100;
    write_reg(REG_LADC_VOL, reg_val);
    write_reg(REG_RADC_VOL, reg_val);
}

void es8388_set_adc_gain(uint8_t gain_db) {
    if (gain_db > 24) gain_db = 24;

    uint8_t val = 0;
    read_reg(REG_ADC_CTRL4, &val);
    val = (val & 0xF8) | (gain_db / 3);
    write_reg(REG_ADC_CTRL4, val);
}

void es8388_enable_adc(bool enable) {
    uint8_t val = 0;
    read_reg(REG_ADC_POWER, &val);
    if (enable) {
        val |= 0x09;
    } else {
        val &= ~0x09;
    }
    write_reg(REG_ADC_POWER, val);
}

void es8388_set_adc_mute(bool mute) {
    uint8_t val = 0;
    read_reg(REG_ADC_CTRL3, &val);
    if (mute) {
        val |= 0x04;
    } else {
        val &= ~0x04;
    }
    write_reg(REG_ADC_CTRL3, val);
}

void es8388_set_dac_output(ES8388Output output) {
    uint8_t val = 0;
    read_reg(REG_DAC_CTRL3, &val);

    switch (output) {
        case ES8388Output::OUT1:
            val = (val & 0x3F) | 0x00;
            break;
        case ES8388Output::OUT2:
            val = (val & 0x3F) | 0x40;
            break;
        case ES8388Output::OUT_BOTH:
            val = (val & 0x3F) | 0x80;
            break;
    }
    write_reg(REG_DAC_CTRL3, val);
}

void es8388_set_dac_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    current_dac_volume = volume;

    uint8_t reg_val = (volume * 192) / 100;
    write_reg(REG_LDAC_VOL, reg_val);
    write_reg(REG_RDAC_VOL, reg_val);
}

void es8388_set_dac_mute(bool mute) {
    uint8_t val = 0;
    read_reg(REG_DAC_CTRL3, &val);
    if (mute) {
        val |= 0x04;
    } else {
        val &= ~0x04;
    }
    write_reg(REG_DAC_CTRL3, val);
}

void es8388_enable_dac(bool enable) {
    uint8_t val = 0;
    read_reg(REG_DAC_POWER, &val);
    if (enable) {
        val |= 0x0C;
    } else {
        val &= ~0x0C;
    }
    write_reg(REG_DAC_POWER, val);
}

void es8388_set_dac_volume_left(uint8_t volume) {
    if (volume > 100) volume = 100;
    uint8_t reg_val = (volume * 192) / 100;
    write_reg(REG_LDAC_VOL, reg_val);
}

void es8388_set_dac_volume_right(uint8_t volume) {
    if (volume > 100) volume = 100;
    uint8_t reg_val = (volume * 192) / 100;
    write_reg(REG_RDAC_VOL, reg_val);
}

void es8388_set_stereo_balance(int8_t balance) {
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;

    if (balance < 0) {
        es8388_set_dac_volume_left(100);
        es8388_set_dac_volume_right(100 + balance);
    } else if (balance > 0) {
        es8388_set_dac_volume_left(100 - balance);
        es8388_set_dac_volume_right(100);
    } else {
        es8388_set_dac_volume_left(100);
        es8388_set_dac_volume_right(100);
    }
}

void es8388_enable_3d(bool enable) {
    uint8_t val = 0;
    read_reg(REG_DAC_CTRL6, &val);
    if (enable) {
        val |= 0x01;
    } else {
        val &= ~0x01;
    }
    write_reg(REG_DAC_CTRL6, val);
}

void es8388_enable_mono_mix(bool enable) {
    uint8_t val = 0;
    read_reg(REG_DAC_CTRL3, &val);
    if (enable) {
        val |= 0x02;
    } else {
        val &= ~0x02;
    }
    write_reg(REG_DAC_CTRL3, val);
}

void es8388_enter_low_power(void) {
    write_reg(REG_CHIP_POWER, 0x00);
    write_reg(REG_ADC_POWER, 0x00);
    write_reg(REG_DAC_POWER, 0x00);
}

void es8388_exit_low_power(void) {
    write_reg(REG_CHIP_POWER, 0xFF);
    write_reg(REG_ADC_POWER, 0x09);
    write_reg(REG_DAC_POWER, 0x0C);
}

void es8388_reset(void) {
    write_reg(REG_CHIP_CONTROL1, 0x3F);
    write_reg(REG_CHIP_CONTROL1, 0x06);
}

uint8_t es8388_get_dac_volume(void) {
    return current_dac_volume;
}

uint8_t es8388_get_adc_volume(void) {
    return current_adc_volume;
}
