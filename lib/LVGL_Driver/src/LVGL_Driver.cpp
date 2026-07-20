#include "LVGL_Driver.h"
#include "Display_ST7701.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/semphr.h"

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

void lvgl_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    if (flush_start_ms == 0) flush_start_ms = millis();
    // Direct mode: LVGL renders straight into one of the two PSRAM framebuffers,
    // so px_map is that FB's base. Nothing to copy per-area; only the LAST flush
    // of a frame swaps the scan-out FB — and the RGB driver performs that swap on
    // the next VSYNC, so the panel never scans a buffer being drawn (no tearing,
    // no DMA phase slip on heavy layout rebuilds).
    if (lv_display_flush_is_last(disp) && panel_handle != NULL && vsync_sem != NULL) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, px_map);
        lvgl_render_ms = millis() - flush_start_ms;
        flush_start_ms = 0;
        // Drain any vsync give that arrived during rendering, then block until the
        // real post-swap VSYNC so LVGL won't draw into the FB now being scanned.
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

    // Direct mode: render into the driver's two PSRAM framebuffers (already
    // allocated by the panel with double_fb=true) instead of separate internal
    // draw buffers. Frees ~46 KB internal DMA-SRAM and, by only swapping FBs on
    // VSYNC, eliminates the layout-push vertical shift and tearing under load.
    void *fb0 = NULL, *fb1 = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
        printf("LVGL_Driver: failed to get panel framebuffers!\n");
        return;
    }

    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, lvgl_flush_callback);
    lv_display_set_buffers(disp, fb0, fb1,
                           LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_resolution(disp, LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_physical_resolution(disp, LCD_WIDTH, LCD_HEIGHT);
}
