#include "LVGL_Driver.h"
#include "Display_ST7701.h"

// Buffer Size: 1/20th of the screen (~23KB per buffer)
// Stable, low memory footprint, safe for SRAM.
#define BUF_SIZE (LCD_WIDTH * LCD_HEIGHT / 20)

static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

void lvgl_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    if (panel_handle != NULL) {
        // Copy SRAM -> PSRAM (LCD Framebuffer)
        esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, (const void*)color_p);
    }
    lv_display_flush_ready(disp);
}

void lvgl_init(void) {
  lv_init();
  lv_tick_set_cb(xTaskGetTickCount);
  
  if (panel_handle == NULL) {
      printf("LVGL_Driver: panel_handle is NULL! Check lcd_init() errors.\n");
      return;
  }
  
  // Allocate buffers in INTERNAL SRAM (Fastest for rendering & DMA)
  buf1 = (lv_color_t *)heap_caps_aligned_alloc(32, BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  buf2 = (lv_color_t *)heap_caps_aligned_alloc(32, BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  
  if (!buf1 || !buf2) {
    printf("LVGL_Driver: Failed to allocate SRAM buffers!\n");
    return;
  }

  lv_display_t *disp_drv = lv_display_create(LCD_WIDTH, LCD_HEIGHT);

  // PARTIAL MODE: Render small chunks in SRAM, copy to display
  lv_display_set_buffers(disp_drv, buf1, buf2, BUF_SIZE * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_display_set_resolution(disp_drv, LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_physical_resolution(disp_drv, LCD_WIDTH, LCD_HEIGHT);

  lv_display_set_flush_cb(disp_drv, lvgl_flush_callback);
}