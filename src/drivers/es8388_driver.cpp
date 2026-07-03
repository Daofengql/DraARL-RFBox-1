#include "es8388_driver.h"
#include "i2s_driver.h"
#include "i2c_driver.h"

namespace {
uint8_t current_dac_volume = 50;
uint8_t current_adc_volume = 50;
ES8388Output current_dac_output = ES8388Output::OUT1;
uint8_t active_es8388_addr = ES8388_ADDR;
constexpr uint8_t ES8388_ADDR_ALT = 0x11;

enum ES8388Reg : uint8_t {
    REG_CONTROL1 = 0x00,
    REG_CONTROL2 = 0x01,
    REG_CHIP_POWER = 0x02,
    REG_ADC_POWER = 0x03,
    REG_DAC_POWER = 0x04,
    REG_MASTER_MODE = 0x08,
    REG_ADC_CONTROL1 = 0x09,
    REG_ADC_CONTROL2 = 0x0A,
    REG_ADC_CONTROL4 = 0x0C,
    REG_ADC_CONTROL5 = 0x0D,
    REG_ADC_CONTROL7 = 0x0F,
    REG_LADC_VOL = 0x10,
    REG_RADC_VOL = 0x11,
    REG_DAC_CONTROL1 = 0x17,
    REG_DAC_CONTROL2 = 0x18,
    REG_DAC_CONTROL3 = 0x19,
    REG_LDAC_VOL = 0x1A,
    REG_RDAC_VOL = 0x1B,
    REG_DAC_CONTROL16 = 0x26,
    REG_DAC_CONTROL17 = 0x27,
    REG_DAC_CONTROL18 = 0x28,
    REG_DAC_CONTROL19 = 0x29,
    REG_DAC_CONTROL20 = 0x2A,
    REG_DAC_CONTROL21 = 0x2B,
    REG_LOUT1_VOL = 0x2E,
    REG_ROUT1_VOL = 0x2F,
    REG_LOUT2_VOL = 0x30,
    REG_ROUT2_VOL = 0x31,
};

constexpr uint8_t ES8388_DAC_OUT1_MASK = 0x30;
constexpr uint8_t ES8388_DAC_OUT2_MASK = 0x0C;
constexpr uint8_t ES8388_DAC_OUT_ALL_MASK = ES8388_DAC_OUT1_MASK | ES8388_DAC_OUT2_MASK;
constexpr uint8_t ES8388_ADC_INPUT1_MASK = 0x00;
constexpr uint8_t ES8388_ADC_INPUT2_MASK = 0x50;
constexpr uint8_t ES8388_ADC_INPUT_DIFF_MASK = 0xF0;
constexpr uint8_t ES8388_BITS_16_MASK = 0x03;
constexpr uint8_t ES8388_BITS_24_MASK = 0x00;
constexpr uint8_t ES8388_BITS_32_MASK = 0x04;
constexpr uint8_t ES8388_I2S_FORMAT_MASK = 0x00;
constexpr uint8_t ES8388_LEFT_FORMAT_MASK = 0x01;
constexpr uint8_t ES8388_RIGHT_FORMAT_MASK = 0x02;
constexpr uint8_t ES8388_DSP_FORMAT_MASK = 0x03;
constexpr uint8_t ES8388_MCLK_DIV_1024 = 0x07;
constexpr uint8_t ES8388_ANALOG_0DB = 0x1E;

static_assert(I2S_MCLK_MULTIPLIER == 1024U, "ES8388 MCLK/LRCK register must match I2S_MCLK_MULTIPLIER.");

bool write_reg(uint8_t reg, uint8_t value) {
    const bool ok = i2c_write_byte(active_es8388_addr, reg, value);
    if (!ok) {
        Serial.printf("[ES8388] write failed: addr=0x%02X reg=0x%02X val=0x%02X\n",
                      static_cast<unsigned int>(active_es8388_addr),
                      static_cast<unsigned int>(reg),
                      static_cast<unsigned int>(value));
    }
    return ok;
}

bool read_reg(uint8_t reg, uint8_t *value) {
    const bool ok = i2c_read_byte(active_es8388_addr, reg, value);
    if (!ok) {
        Serial.printf("[ES8388] read failed: addr=0x%02X reg=0x%02X\n",
                      static_cast<unsigned int>(active_es8388_addr),
                      static_cast<unsigned int>(reg));
    }
    return ok;
}

bool detect_es8388_address() {
    const uint8_t candidates[] = {ES8388_ADDR, ES8388_ADDR_ALT};
    for (const uint8_t addr : candidates) {
        if (i2c_device_exists(addr)) {
            active_es8388_addr = addr;
            return true;
        }
    }
    return false;
}

bool update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
    uint8_t value = 0;
    if (!read_reg(reg, &value)) {
        return false;
    }
    value = static_cast<uint8_t>((value & ~clear_mask) | set_mask);
    return write_reg(reg, value);
}

uint8_t format_mask(ES8388Format format) {
    switch (format) {
        case ES8388Format::STANDARD_I2S:
            return ES8388_I2S_FORMAT_MASK;
        case ES8388Format::LEFT_JUSTIFIED:
            return ES8388_LEFT_FORMAT_MASK;
        case ES8388Format::RIGHT_JUSTIFIED:
            return ES8388_RIGHT_FORMAT_MASK;
        case ES8388Format::DSP_MODE_A:
        case ES8388Format::DSP_MODE_B:
            return ES8388_DSP_FORMAT_MASK;
        default:
            return ES8388_I2S_FORMAT_MASK;
    }
}

uint8_t bits_mask(uint8_t bits) {
    switch (bits) {
        case 24:
            return ES8388_BITS_24_MASK;
        case 32:
            return ES8388_BITS_32_MASK;
        case 16:
        default:
            return ES8388_BITS_16_MASK;
    }
}

uint8_t adc_input_mask(ES8388Input input) {
    switch (input) {
        case ES8388Input::INPUT2:
            return ES8388_ADC_INPUT2_MASK;
        case ES8388Input::INPUT_DIFF:
            return ES8388_ADC_INPUT_DIFF_MASK;
        case ES8388Input::INPUT1:
        default:
            return ES8388_ADC_INPUT1_MASK;
    }
}

uint8_t dac_output_mask(ES8388Output output) {
    switch (output) {
        case ES8388Output::OUT2:
            return ES8388_DAC_OUT2_MASK;
        case ES8388Output::OUT_BOTH:
            return ES8388_DAC_OUT_ALL_MASK;
        case ES8388Output::OUT1:
        default:
            return ES8388_DAC_OUT1_MASK;
    }
}

uint8_t analog_output_volume_reg(uint8_t volume) {
    if (volume >= 100) {
        return ES8388_ANALOG_0DB;
    }
    return static_cast<uint8_t>((static_cast<uint32_t>(volume) * ES8388_ANALOG_0DB) / 100U);
}

uint8_t digital_attenuation_reg(uint8_t volume) {
    if (volume >= 100) {
        return 0x00;
    }
    return static_cast<uint8_t>(((100U - volume) * 0xC0U) / 100U);
}
} // namespace

bool es8388_init(void) {
    if (!detect_es8388_address()) {
        Serial.printf("[ES8388] device not found at 0x%02X/0x%02X\n",
                      static_cast<unsigned int>(ES8388_ADDR),
                      static_cast<unsigned int>(ES8388_ADDR_ALT));
        return false;
    }

    Serial.printf("[ES8388] detected at 0x%02X\n", static_cast<unsigned int>(active_es8388_addr));

    es8388_reset();
    delay(10);

    bool ok = true;
    // Follow the datasheet playback/record startup order closely so DAC output
    // is actually routed into the output mixer and line drivers.
    ok &= write_reg(REG_MASTER_MODE, 0x00);
    ok &= write_reg(REG_CHIP_POWER, 0xF3);
    ok &= write_reg(REG_CONTROL1, 0x05);
    ok &= write_reg(REG_CONTROL2, 0x40);
    ok &= write_reg(REG_ADC_POWER, 0x00);
    ok &= write_reg(REG_DAC_POWER, ES8388_DAC_OUT_ALL_MASK);
    ok &= write_reg(REG_ADC_CONTROL1, 0x00);
    ok &= write_reg(REG_ADC_CONTROL2, ES8388_ADC_INPUT1_MASK);
    ok &= write_reg(REG_ADC_CONTROL7, 0x30);
    ok &= write_reg(REG_DAC_CONTROL3, 0x32);
    ok &= write_reg(REG_DAC_CONTROL16, 0x00);
    ok &= write_reg(REG_DAC_CONTROL17, 0xB8);
    ok &= write_reg(REG_DAC_CONTROL18, 0x38);
    ok &= write_reg(REG_DAC_CONTROL19, 0x38);
    ok &= write_reg(REG_DAC_CONTROL20, 0xB8);
    ok &= write_reg(REG_DAC_CONTROL21, 0x80);
    ok &= write_reg(REG_LDAC_VOL, 0x00);
    ok &= write_reg(REG_RDAC_VOL, 0x00);
    ok &= write_reg(REG_LOUT1_VOL, ES8388_ANALOG_0DB);
    ok &= write_reg(REG_ROUT1_VOL, ES8388_ANALOG_0DB);
    ok &= write_reg(REG_LOUT2_VOL, ES8388_ANALOG_0DB);
    ok &= write_reg(REG_ROUT2_VOL, ES8388_ANALOG_0DB);
    ok &= write_reg(REG_CHIP_POWER, 0x00);

    es8388_set_format(ES8388Format::STANDARD_I2S);
    es8388_set_bits_per_sample(16);
    es8388_set_sample_rate(ES8388SampleRate::RATE_16K);
    es8388_set_adc_input(ES8388Input::INPUT1);
    es8388_set_dac_output(ES8388Output::OUT1);
    es8388_set_adc_volume(100);
    es8388_set_dac_volume(100);
    es8388_set_adc_gain(12);
    es8388_set_adc_mute(false);
    es8388_set_dac_mute(false);

    return ok;
}

void es8388_deinit(void) {
    es8388_enter_low_power();
}

bool es8388_is_present(void) {
    return detect_es8388_address();
}

void es8388_set_format(ES8388Format format) {
    const uint8_t mask = format_mask(format);
    update_reg(REG_ADC_CONTROL4, 0x03, mask);
    update_reg(REG_DAC_CONTROL1, 0x03, mask);
}

void es8388_set_sample_rate(ES8388SampleRate rate) {
    (void)rate;
    write_reg(REG_ADC_CONTROL5, ES8388_MCLK_DIV_1024);
    write_reg(REG_DAC_CONTROL2, ES8388_MCLK_DIV_1024);
    write_reg(REG_DAC_CONTROL21, 0x80);
}

void es8388_set_bits_per_sample(uint8_t bits) {
    const uint8_t mask = bits_mask(bits);
    update_reg(REG_ADC_CONTROL4, 0x1C, static_cast<uint8_t>(mask << 2));
    update_reg(REG_DAC_CONTROL1, 0x38, static_cast<uint8_t>(mask << 3));
}

void es8388_set_adc_input(ES8388Input input) {
    update_reg(REG_ADC_CONTROL2, 0xF0, adc_input_mask(input));
}

void es8388_set_adc_volume(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    current_adc_volume = volume;

    const uint8_t reg_val = digital_attenuation_reg(volume);
    write_reg(REG_LADC_VOL, reg_val);
    write_reg(REG_RADC_VOL, reg_val);
}

void es8388_set_adc_gain(uint8_t gain_db) {
    if (gain_db > 24) {
        gain_db = 24;
    }
    write_reg(REG_ADC_CONTROL1, static_cast<uint8_t>(gain_db / 3));
}

void es8388_enable_adc(bool enable) {
    write_reg(REG_ADC_POWER, enable ? 0x00 : 0xFF);
}

void es8388_set_adc_mute(bool mute) {
    write_reg(REG_ADC_CONTROL7, mute ? 0x34 : 0x30);
}

void es8388_set_dac_output(ES8388Output output) {
    current_dac_output = output;
    update_reg(REG_DAC_POWER, ES8388_DAC_OUT_ALL_MASK, dac_output_mask(output));
}

void es8388_set_dac_volume(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    current_dac_volume = volume;

    const uint8_t digital_reg_val = digital_attenuation_reg(volume);
    const uint8_t analog_reg_val = analog_output_volume_reg(volume);
    write_reg(REG_LDAC_VOL, digital_reg_val);
    write_reg(REG_RDAC_VOL, digital_reg_val);
    write_reg(REG_LOUT1_VOL, analog_reg_val);
    write_reg(REG_ROUT1_VOL, analog_reg_val);
    write_reg(REG_LOUT2_VOL, analog_reg_val);
    write_reg(REG_ROUT2_VOL, analog_reg_val);
}

void es8388_set_dac_mute(bool mute) {
    write_reg(REG_DAC_CONTROL3, mute ? 0x36 : 0x32);
}

void es8388_enable_dac(bool enable) {
    update_reg(REG_DAC_POWER, ES8388_DAC_OUT_ALL_MASK, enable ? dac_output_mask(current_dac_output) : 0x00);
}

void es8388_set_dac_volume_left(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    write_reg(REG_LOUT1_VOL, analog_output_volume_reg(volume));
    write_reg(REG_LOUT2_VOL, analog_output_volume_reg(volume));
}

void es8388_set_dac_volume_right(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    write_reg(REG_ROUT1_VOL, analog_output_volume_reg(volume));
    write_reg(REG_ROUT2_VOL, analog_output_volume_reg(volume));
}

void es8388_set_stereo_balance(int8_t balance) {
    if (balance < -100) {
        balance = -100;
    }
    if (balance > 100) {
        balance = 100;
    }

    if (balance < 0) {
        es8388_set_dac_volume_left(100);
        es8388_set_dac_volume_right(static_cast<uint8_t>(100 + balance));
    } else if (balance > 0) {
        es8388_set_dac_volume_left(static_cast<uint8_t>(100 - balance));
        es8388_set_dac_volume_right(100);
    } else {
        es8388_set_dac_volume_left(100);
        es8388_set_dac_volume_right(100);
    }
}

void es8388_enable_3d(bool enable) {
    update_reg(REG_DAC_CONTROL3, 0x10, enable ? 0x10 : 0x00);
}

void es8388_enable_mono_mix(bool enable) {
    update_reg(REG_DAC_CONTROL16, 0x07, enable ? 0x02 : 0x00);
}

void es8388_enter_low_power(void) {
    write_reg(REG_ADC_POWER, 0xFF);
    write_reg(REG_DAC_POWER, 0x00);
    write_reg(REG_CHIP_POWER, 0xFF);
}

void es8388_exit_low_power(void) {
    write_reg(REG_CHIP_POWER, 0x00);
    write_reg(REG_ADC_POWER, 0x00);
    write_reg(REG_DAC_POWER, dac_output_mask(current_dac_output));
}

void es8388_reset(void) {
    write_reg(REG_CONTROL1, 0x80);
    delay(5);
    write_reg(REG_CONTROL1, 0x00);
}

uint8_t es8388_get_dac_volume(void) {
    return current_dac_volume;
}

uint8_t es8388_get_adc_volume(void) {
    return current_adc_volume;
}
