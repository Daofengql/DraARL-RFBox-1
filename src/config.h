#ifndef CONFIG_H
#define CONFIG_H

// ===================================================================
//                       显示屏配置
// ===================================================================
#define TFT_SCLK        18      // SPI 时钟引脚
#define TFT_MOSI        23      // SPI 数据引脚
#define TFT_DC          2       // 数据/命令引脚
#define TFT_CS          15      // 片选引脚
#define TFT_RST         4       // 复位引脚

// 屏幕分辨率
#define SCREEN_WIDTH    320
#define SCREEN_HEIGHT   240

// ===================================================================
//                       背光配置
// ===================================================================
#define BACKLIGHT_PIN   5       // 背光引脚
#define PWM_CHANNEL     0       // PWM 通道
#define PWM_FREQ        5000    // PWM 频率
#define PWM_RESOLUTION  8       // PWM 分辨率 (8位 = 0-255)

#endif // CONFIG_H
