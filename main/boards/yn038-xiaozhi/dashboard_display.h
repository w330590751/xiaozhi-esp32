#pragma once

#include "display/lcd_display.h"

#include <string>

// 1.8" 128x160 panel used as a standalone status board:
// local air readings on top, the assistant's latest turn in the middle, weather at the bottom.
class DashboardDisplay : public SpiLcdDisplay {
public:
    DashboardDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
        : SpiLcdDisplay(io, panel, 128, 160, 0, 0, true, true, false) {}

    void UpdateAir(bool valid, float celsius, int humidity, int co2, float hcho, float tvoc);
    void SetWeather(const std::string& line);
    void SetQa(const std::string& question, const std::string& answer);
    void SetOnline(bool online);

    void SetChatMessage(const char* role, const char* content) override;

protected:
    void SetupUI() override;

private:
    void RenderAir();
    void RenderQa();

    lv_obj_t* air_label_ = nullptr;
    lv_obj_t* qa_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* link_label_ = nullptr;

    bool ui_ready_ = false;
    bool air_valid_ = false;
    float temp_ = 0;
    int humidity_ = -1;
    int co2_ = -1;
    float hcho_ = -1;
    float tvoc_ = -1;
    std::string question_;
    std::string answer_;
    bool online_ = false;
};
