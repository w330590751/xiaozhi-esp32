#pragma once

#include "display/lcd_display.h"
#include <cJSON.h>
#include <cstdio>

LV_FONT_DECLARE(font_noto_sans_basic_14_1);

// This small panel is a local sensor dashboard. It does not imply that the
// speaker's wake word or model session is running on the ESP32.
class EnvironmentDisplay : public SpiLcdDisplay {
    lv_obj_t* readings_ = nullptr;
    unsigned updates_ = 0;

public:
    EnvironmentDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
        : SpiLcdDisplay(io, panel, 128, 160, 0, 0, true, true, false) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        if (readings_) return;
        auto* box = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(box, 0, 22);
        lv_obj_set_size(box, 128, 138);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 4, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x081820), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        readings_ = lv_label_create(box);
        lv_obj_set_width(readings_, 120);
        lv_obj_set_style_text_font(readings_, &font_noto_sans_basic_14_1, 0);
        lv_obj_set_style_text_color(readings_, lv_color_hex(0xD7F5EE), 0);
        lv_obj_set_style_text_line_space(readings_, 3, 0);
        lv_label_set_text(readings_, "ENV\nWaiting for\nsensor data");
    }

    void UpdateEnvironment(const std::string& json) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) return;
        auto* age = cJSON_GetObjectItemCaseSensitive(root, "age_ms");
        bool fresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "available")) &&
                     cJSON_IsNumber(age) && age->valuedouble >= 0 && age->valuedouble < 15000;
        std::string text = fresh ? "ENV  LIVE\n" : "ENV  NO DATA\n";
        const char* keys[] = {"temperature_c", "humidity_percent", "co2_ppm", "hcho_mg_m3", "tvoc_mg_m3"};
        const char* names[] = {"T", "RH", "CO2", "HCHO", "TVOC"};
        const char* units[] = {" C", "%", " ppm", "", ""};
        for (int i = 0; i < 5; ++i) {
            auto* value = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
            char line[48];
            if (fresh && cJSON_IsNumber(value))
                snprintf(line, sizeof(line), "%s: %.*f%s\n", names[i], i >= 3 ? 3 : 0, value->valuedouble, units[i]);
            else
                snprintf(line, sizeof(line), "%s: --\n", names[i]);
            text += line;
        }
        text += "HCHO/TVOC: mg/m3";
        cJSON_Delete(root);
        DisplayLockGuard lock(this);
        if (readings_) {
            lv_label_set_text(readings_, text.c_str());
            if (updates_++ % 10 == 0) {
                ESP_LOGI("ENVLCD", "Dashboard updated, fresh sensor frame: %s", fresh ? "yes" : "no");
            }
        }
    }
};
