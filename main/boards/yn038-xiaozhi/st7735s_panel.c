#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdlib.h>

// Minimal ST7735S panel driver. IDF 6.1 only ships ST7789/SSD1306, and the
// 1.8-inch 128x160 module on this board uses ST7735S, so reuse the panel
// interface with the ST7735 power/gamma tables instead of the ST7789 ones.

static const char *TAG = "lcd_panel.st7735s";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
} st7735s_panel_t;

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_st7735s_sleep(esp_lcd_panel_t *panel, bool sleep);

static esp_err_t tx(esp_lcd_panel_io_handle_t io, int cmd, const uint8_t *param, size_t size)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(io, cmd, param, size);
    ESP_RETURN_ON_ERROR(err, TAG, "cmd 0x%02x failed", cmd);
    return ESP_OK;
}

static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(tx(p->io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Frame rate control: normal / idle / partial
    ESP_RETURN_ON_ERROR(tx(io, 0xB1, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "frmctr1");
    ESP_RETURN_ON_ERROR(tx(io, 0xB2, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "frmctr2");
    ESP_RETURN_ON_ERROR(tx(io, 0xB3, (const uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6), TAG, "frmctr3");
    ESP_RETURN_ON_ERROR(tx(io, 0xB4, (const uint8_t[]){0x07}, 1), TAG, "invctr");

    // Power control
    ESP_RETURN_ON_ERROR(tx(io, 0xC0, (const uint8_t[]){0xA2, 0x02, 0x84}, 3), TAG, "pwctr1");
    ESP_RETURN_ON_ERROR(tx(io, 0xC1, (const uint8_t[]){0xC5}, 1), TAG, "pwctr2");
    ESP_RETURN_ON_ERROR(tx(io, 0xC2, (const uint8_t[]){0x0A, 0x00}, 2), TAG, "pwctr3");
    ESP_RETURN_ON_ERROR(tx(io, 0xC3, (const uint8_t[]){0x8A, 0x2A}, 2), TAG, "pwctr4");
    ESP_RETURN_ON_ERROR(tx(io, 0xC4, (const uint8_t[]){0x8A, 0xEE}, 2), TAG, "pwctr5");
    ESP_RETURN_ON_ERROR(tx(io, 0xC5, (const uint8_t[]){0x0E}, 1), TAG, "vmctr1");

    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_INVOFF, NULL, 0), TAG, "invoff");
    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_MADCTL, (const uint8_t[]){p->madctl_val}, 1), TAG, "madctl");
    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_COLMOD, (const uint8_t[]){0x05}, 1), TAG, "colmod");

    // Gamma
    ESP_RETURN_ON_ERROR(tx(io, 0xE0, (const uint8_t[]){
        0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 16), TAG, "gmctrp1");
    ESP_RETURN_ON_ERROR(tx(io, 0xE1, (const uint8_t[]){
        0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 16), TAG, "gmctrn1");

    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_DISPON, NULL, 0), TAG, "dispon");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_CASET, (const uint8_t[]){
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1)}, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(tx(io, LCD_CMD_RASET, (const uint8_t[]){
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1)}, 4), TAG, "raset");

    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * 2;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "ramwr");
    return ESP_OK;
}

static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    return tx(p->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (mirror_x) {
        p->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        p->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return tx(p->io, LCD_CMD_MADCTL, (const uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (swap_axes) {
        p->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return tx(p->io, LCD_CMD_MADCTL, (const uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    return tx(p->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_st7735s_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    ESP_RETURN_ON_ERROR(tx(p->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    esp_err_t ret = ESP_OK;
    st7735s_panel_t *p = (st7735s_panel_t *)calloc(1, sizeof(st7735s_panel_t));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {};
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "reset gpio config failed");
    }

    // Rotation 0 for the 128x160 ST7735S module: MX + MY + BGR.
    p->madctl_val = LCD_CMD_MX_BIT | LCD_CMD_MY_BIT;
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        p->madctl_val |= LCD_CMD_BGR_BIT;
    }

    p->io = io;
    p->reset_gpio_num = panel_dev_config->reset_gpio_num;
    p->reset_level = panel_dev_config->flags.reset_active_high;
    p->base.del = panel_st7735s_del;
    p->base.reset = panel_st7735s_reset;
    p->base.init = panel_st7735s_init;
    p->base.draw_bitmap = panel_st7735s_draw_bitmap;
    p->base.invert_color = panel_st7735s_invert_color;
    p->base.mirror = panel_st7735s_mirror;
    p->base.swap_xy = panel_st7735s_swap_xy;
    p->base.set_gap = panel_st7735s_set_gap;
    p->base.disp_on_off = panel_st7735s_disp_on_off;
    p->base.disp_sleep = panel_st7735s_sleep;
    *ret_panel = &p->base;
    ESP_LOGD(TAG, "new st7735s panel @%p", p);
    return ESP_OK;

err:
    if (p) {
        free(p);
    }
    return ESP_ERR_INVALID_STATE;
}
