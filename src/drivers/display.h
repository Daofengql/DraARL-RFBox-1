#ifndef DISPLAY_H
#define DISPLAY_H

#include <LovyanGFX.hpp>
#include <lvgl.h>

extern class LGFX tft; // 在display.cpp定义

void initDisplay();
void flushDisplay(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

// 编码器输入设备
void initEncoderInput();
extern lv_indev_t *encoder_indev;
extern lv_group_t *encoder_group;

#endif // DISPLAY_H
