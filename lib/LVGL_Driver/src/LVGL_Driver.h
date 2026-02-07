 #pragma once

   #include <lvgl.h>
   #include <esp_heap_caps.h>
   #include <esp_lcd_panel_ops.h>
   #include <esp_lcd_panel_rgb.h>

   // If LCD_WIDTH isn't defined yet (because we removed the include), define it here
   // or rely on the .cpp file to have the correct values via its includes.
   #ifndef LCD_WIDTH
   #define LCD_WIDTH 480
   #endif
   #ifndef LCD_HEIGHT
   #define LCD_HEIGHT 480
   #endif

   void lvgl_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p);
   void lvgl_init(void);