#pragma once
   #include <inttypes.h>

   #include "freertos/FreeRTOS.h"
   #include "freertos/task.h"
   #include "driver/spi_master.h"
   #include "driver/gpio.h"
   #include "esp_heap_caps.h"
   #include "esp_log.h"
   #include "esp_lcd_panel_ops.h"
   #include "esp_lcd_panel_io.h"
   #include "esp_lcd_panel_rgb.h"

   #include "I2C_Driver.h"
   #include "TCA9554PWR.h"

   #define LCD_CLK_PIN           2
   #define LCD_MOSI_PIN          1
   #define LCD_BACKLIGHT_PIN     6

   // Backlight
   #define pwm_channel           1         // PWM Channel
   #define frequency             20000     // PWM frequencyconst
   #define resolution            10        // PWM resolution ratio     MAX:13
   #define dutyfactor            500       // PWM dutyfactor
   #define backlight_max         100

   #define ESP_PANEL_LCD_WIDTH                       (480)
   #define ESP_PANEL_LCD_HEIGHT                      (480)
   #define ESP_PANEL_LCD_COLOR_BITS                  (16)
   #define ESP_PANEL_LCD_RGB_PIXEL_BITS              (16)    // 24 | 16
   #define ESP_PANEL_LCD_RGB_DATA_WIDTH              (16)
   #define ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ          (12 * 1000 * 1000)
   #define ESP_PANEL_LCD_RGB_TIMING_HPW              (8)
   #define ESP_PANEL_LCD_RGB_TIMING_HBP              (10)
   #define ESP_PANEL_LCD_RGB_TIMING_HFP              (30)
   #define ESP_PANEL_LCD_RGB_TIMING_VPW              (6)
   #define ESP_PANEL_LCD_RGB_TIMING_VBP              (16)
   #define ESP_PANEL_LCD_RGB_TIMING_VFP              (16)
   #define ESP_PANEL_LCD_RGB_FRAME_BUF_NUM           (2)     // 1/2/3
   // Bounce buffer (internal-SRAM prefetch of the PSRAM framebuffer). Fully mapped
   // trade-off on this board:
   //   ON  (small) -> no flicker, but a full-screen rebuild burst can underrun it
   //                  and slip the DMA VSYNC phase = a persistent vertical shift on
   //                  layout reload (magnitude == bounce size; bigger made it WORSE).
   //   OFF (0)     -> no shift, but the LCD FIFO underruns on ANY movement (PSRAM
   //                  latency-bound, not throughput — lowering pclk barely helped)
   //                  = constant flicker. Worse for daily use.
   // Kept ON @ 20 rows: flicker-free normal operation; the reload shift is a design-
   // time-only artifact. Definitive cure = CONFIG_LCD_RGB_RESTART_IN_VSYNC (auto
   // re-align every VSYNC) via a custom sdkconfig rebuild — see the display notes.
   #define ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE         (ESP_PANEL_LCD_WIDTH * 20)

   // ... (Pin definitions remain the same) ...
   #define ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC           (38)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC           (39)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DE              (40)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_PCLK            (41)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA0           (42)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA1           (45)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA2           (48)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA3           (47)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA4           (21)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA5           (14)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA6           (13)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA7           (12)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA8           (11)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA9           (10)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA10          (9)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA11          (46)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA12          (3)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA13          (17)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA14          (18)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA15          (8)
   #define ESP_PANEL_LCD_PIN_NUM_RGB_DISP            (-1)

   #define ESP_PANEL_LCD_BK_LIGHT_ON_LEVEL           (1)
   #define ESP_PANEL_LCD_BK_LIGHT_OFF_LEVEL !ESP_PANEL_LCD_BK_LIGHT_ON_LEVEL

   #define EXAMPLE_ENABLE_PRINT_LCD_FPS            (0)

   extern uint8_t LCD_Backlight;
   extern esp_lcd_panel_handle_t panel_handle;
   void st7701_reset();
   void st7701_init();

   void lcd_init();
   // Re-align RGB DMA to VSYNC (fixes intermittent vertical-shift-at-boot).
   // Call once after the framebuffer has valid content, backlight still off.
   esp_err_t lcd_resync();

   // Change the pixel clock at runtime (applied on the next VSYNC). Used to
   // briefly slow the LCD DMA during a layout rebuild burst so the bounce buffer
   // can't underrun (which would slip the DMA phase = vertical shift), then
   // restore full speed. See LCD_PCLK_* below.
   void lcd_set_pclk(uint32_t hz);
   #define LCD_PCLK_NORMAL_HZ   ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ   // 12 MHz
   #define LCD_PCLK_RELOAD_HZ   (4 * 1000 * 1000)                  // during rebuild burst
   void lcd_add_window(uint16_t Xstart, uint16_t Xend, uint16_t Ystart, uint16_t Yend,
 uint8_t *color);

   // backlight
   void backlight_init();
   void set_backlight(uint8_t light);