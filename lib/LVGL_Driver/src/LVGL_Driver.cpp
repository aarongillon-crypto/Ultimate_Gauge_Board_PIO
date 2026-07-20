#include "LVGL_Driver.h"
#include "Display_ST7701.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/semphr.h"

// Partial render mode. LVGL renders each dirty area into a small internal-SRAM
// buffer, fully composited, then draw_bitmap copies just that area into the PSRAM
// framebuffer. This composites overlaps correctly every flush.
//
// NB: v2.8.0 briefly used DIRECT mode (render straight into the two PSRAM FBs,
// swap on VSYNC) — it freed ~46 KB internal SRAM and killed tearing under load,
// but the two-buffer sync ghosted moving elements where they overlapped static
// ones (needle crossing a ring). Overlap is the common gauge case, so we reverted
// to partial mode: no ghosting; the tradeoff is ~46 KB internal SRAM back and some
// tearing only under heavy layout-rebuild stress (mitigated by the pclk-drop +
// resync window in main.cpp / build_active_scene).
//
// 1/20th screen per SRAM chunk — keeps DMA-capable SRAM available for WiFi.
#define BUF_SIZE (LCD_WIDTH * LCD_HEIGHT / 20)

static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;
static SemaphoreHandle_t vsync_sem = NULL;
static uint32_t flush_start_ms = 0;
uint32_t lvgl_render_ms = 0;

static bool on_vsync(esp_lcd_panel_handle_t panel,
                     const esp_lcd_rgb_panel_event_data_t *edata,
                     void *user_ctx) {
    BaseType_t awoken = pdFALSE;
    if (vsync_sem) xSemaphoreGiveFromISR(vsync_sem, &awoken);
    return awoken == pdTRUE;
}

void lvgl_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    if (flush_start_ms == 0) flush_start_ms = millis();
    if (panel_handle != NULL) {
        esp_lcd_panel_draw_bitmap(panel_handle,
                                  area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                                  color_p);
    }
    if (lv_display_flush_is_last(disp) && vsync_sem != NULL) {
        lvgl_render_ms = millis() - flush_start_ms;
        flush_start_ms = 0;
        // Drain any vsync give that arrived during rendering — if we don't, we
        // return from the second Take immediately (using a stale give from mid-
        // render), which means we skip the actual vsync that commits this frame.
        xSemaphoreTake(vsync_sem, 0);
        xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(100));
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

    vsync_sem = xSemaphoreCreateBinary();

    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_vsync = on_vsync };
    esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL);

    buf1 = (lv_color_t *)heap_caps_aligned_alloc(32, BUF_SIZE * sizeof(lv_color_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = (lv_color_t *)heap_caps_aligned_alloc(32, BUF_SIZE * sizeof(lv_color_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!buf1 || !buf2) {
        printf("LVGL_Driver: Failed to allocate SRAM draw buffers!\n");
        return;
    }

    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, lvgl_flush_callback);
    lv_display_set_buffers(disp, buf1, buf2,
                           BUF_SIZE * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_resolution(disp, LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_physical_resolution(disp, LCD_WIDTH, LCD_HEIGHT);
}
