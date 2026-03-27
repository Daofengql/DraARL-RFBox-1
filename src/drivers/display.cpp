#include "display.h"
#include "config.h"

// 定义LGFX类，用于显示面板初始化
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789 _panel_instance;  // 使用 ST7789 面板驱动 (兼容 GC9A01)
  lgfx::Bus_SPI _bus_instance;         // 使用SPI总线

public:
  LGFX(void)
  {
    auto cfg = _bus_instance.config();
    // SPI总线设置
    cfg.spi_host = SPI2_HOST;    
    cfg.spi_mode = 0;            
    cfg.freq_write = 80000000;   
    cfg.freq_read = 80000000;    
    cfg.spi_3wire = true;        
    cfg.use_lock = true;       
    cfg.dma_channel = SPI_DMA_CH_AUTO; 
    cfg.pin_sclk = TFT_SCLK;            
    cfg.pin_mosi = TFT_MOSI;            
    cfg.pin_miso = -1;           
    cfg.pin_dc = TFT_DC; 
    
    _bus_instance.config(cfg);   
    _panel_instance.setBus(&_bus_instance); 

    auto panel_cfg = _panel_instance.config(); 
    panel_cfg.pin_cs = TFT_CS;       
    panel_cfg.pin_rst = TFT_RST;       
    panel_cfg.pin_busy = -1;     

    panel_cfg.memory_width  = SCREEN_WIDTH;  
    panel_cfg.memory_height = SCREEN_HEIGHT; 
    panel_cfg.panel_width   = SCREEN_WIDTH; 
    panel_cfg.panel_height  = SCREEN_HEIGHT;
    panel_cfg.offset_rotation = 0;           // 不旋转
    

    panel_cfg.offset_x = 0;       
    panel_cfg.offset_y = 0;

    panel_cfg.dummy_read_pixel = 8; 
    panel_cfg.dummy_read_bits = 1;  
    panel_cfg.readable = false;
    panel_cfg.invert = true;      // 如果颜色不对，尝试改为 true
    panel_cfg.rgb_order = false;   
    panel_cfg.dlen_16bit = false;  
    panel_cfg.bus_shared = false;  

    _panel_instance.config(panel_cfg);  
    setPanel(&_panel_instance);   
  }
};

LGFX tft; 

void initDisplay() {
  tft.init();       
  tft.initDMA();    
  tft.startWrite(); 
  // PVGAMCTRL (Positive Voltage Gamma Control)
  tft.writecommand(0xE0);
  tft.writedata(0xD0); tft.writedata(0x08); tft.writedata(0x11); tft.writedata(0x08);
  tft.writedata(0x0C); tft.writedata(0x15); tft.writedata(0x39); tft.writedata(0x33);
  tft.writedata(0x50); tft.writedata(0x36); tft.writedata(0x13); tft.writedata(0x14);
  tft.writedata(0x29); tft.writedata(0x2D);

  // NVGAMCTRL (Negative Voltage Gamma Control)
  tft.writecommand(0xE1);
  tft.writedata(0xD0); tft.writedata(0x08); tft.writedata(0x10); tft.writedata(0x08);
  tft.writedata(0x06); tft.writedata(0x06); tft.writedata(0x39); tft.writedata(0x44);
  tft.writedata(0x51); tft.writedata(0x0B); tft.writedata(0x16); tft.writedata(0x14);
  tft.writedata(0x2F); tft.writedata(0x31);
  
}

void flushDisplay(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  if (tft.getStartCount() == 0)
  {
    tft.endWrite();
  }

  tft.pushImageDMA(area->x1, area->y1, 
                   area->x2 - area->x1 + 1, 
                   area->y2 - area->y1 + 1, 
                   (lgfx::swap565_t *)&color_p->full);

  lv_disp_flush_ready(disp);
}