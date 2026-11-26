#include "dashboard_display.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr const char* TAG = "DASH";

constexpr uint32_t kBgHex = 0x000000;
constexpr uint32_t kAirHex = 0x9CF5D8;
constexpr uint32_t kQaHex = 0xFFFFFF;
constexpr uint32_t kWeatherHex = 0x8FC8FF;
constexpr uint32_t kLinkHex = 0xFFE38A;

lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, int width, uint32_t hex,
                    lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(hex), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_label_set_text(label, "");
    return label;
}
}  // namespace

void DashboardDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    if (ui_ready_) {
        return;
    }
    // Own the visible background: the base display also creates an opaque
    // theme-colored container and can reapply its theme when fonts load.
    auto* panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 128, 160);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kBgHex), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(panel, lv_color_white(), 0);

    air_label_ = MakeLabel(panel, 3, 2, 96, kAirHex);
    lv_obj_set_height(air_label_, 40);
    link_label_ = MakeLabel(panel, 100, 2, 26, kLinkHex, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(link_label_, "--");
    qa_label_ = MakeLabel(panel, 3, 44, 122, kQaHex);
    lv_obj_set_height(qa_label_, 84);
    weather_label_ = MakeLabel(panel, 3, 130, 122, kWeatherHex);
    lv_obj_set_height(weather_label_, 28);

    // A visible placeholder means the panel works; blank output means a render fault.
    lv_label_set_text(air_label_, "--C --%\nCO2 ----");
    lv_label_set_text(qa_label_, "Waiting for\nthe assistant...");
    lv_label_set_text(weather_label_, "Weather --");
    ui_ready_ = true;
    ESP_LOGI(TAG, "dashboard UI ready");
}

void DashboardDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) return;
    if (strcmp(role, "user") == 0) {
        question_ = content;
        answer_.clear();
    } else if (strcmp(role, "assistant") == 0) {
        answer_ = content;
    } else {
        return;
    }
    RenderQa();
}

void DashboardDisplay::RenderAir() {
    if (!ui_ready_) return;
    char temp_buf[8] = "--C";
    if (air_valid_) snprintf(temp_buf, sizeof(temp_buf), "%.0fC", temp_);
    char hum_buf[16] = "--";
    if (humidity_ >= 0) snprintf(hum_buf, sizeof(hum_buf), "%d%%", humidity_);
    char co2_buf[16] = "----";
    if (co2_ >= 0) snprintf(co2_buf, sizeof(co2_buf), "%d", co2_);
    char line[64];
    snprintf(line, sizeof(line), "%s %s\nCO2 %s", temp_buf, hum_buf, co2_buf);
    DisplayLockGuard lock(this);
    lv_label_set_text(air_label_, line);
}

void DashboardDisplay::RenderQa() {
    if (!ui_ready_) return;
    std::string text;
    if (!question_.empty()) {
        text = "Q " + question_ + "\n";
    }
    if (!answer_.empty()) {
        text += "A " + answer_;
    }
    if (text.empty()) {
        text = "Waiting for\nthe assistant...";
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(qa_label_, text.c_str());
}

void DashboardDisplay::UpdateAir(bool valid, float celsius, int humidity, int co2, float hcho,
                                 float tvoc) {
    air_valid_ = valid;
    temp_ = celsius;
    humidity_ = humidity;
    co2_ = co2;
    hcho_ = hcho;
    tvoc_ = tvoc;
    RenderAir();
}

void DashboardDisplay::SetWeather(const std::string& line) {
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(weather_label_, line.c_str());
}

void DashboardDisplay::SetQa(const std::string& question, const std::string& answer) {
    question_ = question;
    answer_ = answer;
    RenderQa();
}

void DashboardDisplay::SetOnline(bool online) {
    online_ = online;
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(link_label_, online_ ? "OK" : "--");
}
