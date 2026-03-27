#ifndef DISPLAY_H
#define DISPLAY_H

#include <LovyanGFX.hpp>
#include <lvgl.h>

extern class LGFX tft; // 在display.cpp定义

void initDisplay();
void flushDisplay(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

#endif // DISPLAY_H
