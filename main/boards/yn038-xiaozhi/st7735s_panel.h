#pragma once

#include "esp_lcd_panel_dev.h"
#include "esp_lcd_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ST7735S is not provided by ESP-IDF 6.1; this is a minimal driver for the
// 1.8-inch 128x160 SPI module used as the environment dashboard.
esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
