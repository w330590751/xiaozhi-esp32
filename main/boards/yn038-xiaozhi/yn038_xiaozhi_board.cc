#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "dashboard_display.h"
#include "ht1621_sniffer.h"
#include "mcp_server.h"
#include "wifi_board.h"

#include <lwip/ip6_addr.h>

#ifdef CONFIG_YN038_TFT_DISPLAY
#include "display/lcd_display.h"
#include "st7735s_panel.h"
#include <driver/spi_common.h>
#endif

#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_log.h>

#include <cJSON.h>

class Yn038XiaozhiBoard : public WifiBoard {
private:
#ifdef CONFIG_YN038_TFT_DISPLAY
    DashboardDisplay* display_ = nullptr;

    void InitializeDisplay() {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = GPIO_NUM_11;
        bus.miso_io_num = GPIO_NUM_NC;
        bus.sclk_io_num = GPIO_NUM_12;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = 128 * 160 * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_17;
        io_config.dc_gpio_num = GPIO_NUM_13;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 10 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));
        esp_lcd_panel_dev_config_t config = {};
        config.reset_gpio_num = GPIO_NUM_14;
        config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        config.bits_per_pixel = 16;
        esp_lcd_panel_handle_t panel = nullptr;
        // The 1.8-inch 128x160 module uses ST7735S, which IDF does not ship.
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7735s(io, &config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
        // Status board: air readings + assistant turn + weather.
        display_ = new DashboardDisplay(io, panel);
        ESP_LOGI("ENVLCD", "128x160 ST7735S initialized: CLK=12 MOSI=11 RST=14 DC=13 CS=17; external backlight");
    }
#endif
    Button boot_button_;
    Ht1621Sniffer ht1621_sniffer_;

    void InitializeButton() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

#ifdef CONFIG_YN038_TFT_DISPLAY
    // The panel shows air data plus the assistant's last turn and the weather.
    // Both come from the PC service over the LAN, so the board keeps working while
    // the device sits idle and is not holding a chat session.
    void StartDashboard() {
        xTaskCreate(&Yn038XiaozhiBoard::DashboardTask, "dashboard", 7168, this, 2, nullptr);
    }

    static void DashboardTask(void* arg) {
        auto* board = static_cast<Yn038XiaozhiBoard*>(arg);
        bool online = false;
        for (;;) {
            auto* iface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!iface || !esp_netif_is_netif_up(iface)) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            const bool ok = board->RefreshDashboard();
            if (ok != online) {
                online = ok;
                board->display_->SetOnline(online);
                ESP_LOGI("DASH", "PC dashboard link %s", online ? "up" : "down");
            }
            board->RefreshAir();
            vTaskDelay(pdMS_TO_TICKS(DASHBOARD_REFRESH_MS));
        }
    }

    static std::string HttpGetText(const char* url, int timeout_ms) {
        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = timeout_ms;
        config.disable_auto_redirect = true;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) return {};
        std::string body;
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            char buffer[256];
            int read = 0;
            while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
                body.append(buffer, read);
                if (body.size() > 4096) break;
            }
            const int status = esp_http_client_get_status_code(client);
            if (status < 200 || status >= 300) body.clear();
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return body;
    }

    bool RefreshDashboard() {
        const std::string body = HttpGetText(DASHBOARD_URL, 4000);
        if (body.empty()) return false;
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) return false;

        auto* weather = cJSON_GetObjectItem(root, "weather");
        if (cJSON_IsObject(weather)) {
            auto* text = cJSON_GetObjectItem(weather, "text");
            auto* temp = cJSON_GetObjectItem(weather, "temp");
            auto* high = cJSON_GetObjectItem(weather, "high");
            auto* low = cJSON_GetObjectItem(weather, "low");
            char line[64];
            if (cJSON_IsString(text) && cJSON_IsNumber(temp)) {
                if (cJSON_IsNumber(high) && cJSON_IsNumber(low)) {
                    snprintf(line, sizeof(line), "%s %dC %d/%dC", text->valuestring,
                             temp->valueint, high->valueint, low->valueint);
                } else {
                    snprintf(line, sizeof(line), "%s %dC", text->valuestring, temp->valueint);
                }
                display_->SetWeather(line);
            }
        }

        auto* qa = cJSON_GetObjectItem(root, "qa");
        if (cJSON_IsObject(qa)) {
            auto* q = cJSON_GetObjectItem(qa, "q");
            auto* a = cJSON_GetObjectItem(qa, "a");
            if (cJSON_IsString(q) || cJSON_IsString(a)) {
                display_->SetQa(cJSON_IsString(q) ? q->valuestring : "",
                                cJSON_IsString(a) ? a->valuestring : "");
            }
        }
        cJSON_Delete(root);
        return true;
    }

    void RefreshAir() {
        const std::string body = ht1621_sniffer_.GetReadingJson();
        esp_http_client_config_t cfg = {};
        cfg.url = "http://10.0.0.185:8003/internal/environment/push";
        cfg.method = HTTP_METHOD_POST;
        cfg.timeout_ms = 3500;
        cfg.disable_auto_redirect = true;
        if (auto client = esp_http_client_init(&cfg)) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body.data(), body.size());
            esp_http_client_perform(client);
            esp_http_client_cleanup(client);
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) return;
        auto* available = cJSON_GetObjectItem(root, "available");
        const bool valid = cJSON_IsTrue(available);
        auto num = [root](const char* key, double fallback) {
            auto* item = cJSON_GetObjectItem(root, key);
            return cJSON_IsNumber(item) ? item->valuedouble : fallback;
        };
        display_->UpdateAir(valid && cJSON_IsNumber(cJSON_GetObjectItem(root, "temperature_c")), static_cast<float>(num("temperature_c", 0)),
                            static_cast<int>(num("humidity_percent", -1)),
                            static_cast<int>(num("co2_ppm", -1)),
                            static_cast<float>(num("hcho_mg_m3", -1)),
                            static_cast<float>(num("tvoc_mg_m3", -1)));
        cJSON_Delete(root);
    }
#endif

    void StartEnvironmentPush() {
        xTaskCreate(&Yn038XiaozhiBoard::EnvironmentPushTask, "env_push", 6144, this, 2,
                    nullptr);
    }

    static void EnvironmentPushTask(void* arg) {
        auto* board = static_cast<Yn038XiaozhiBoard*>(arg);
        unsigned int attempts = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(ENVIRONMENT_PUSH_INTERVAL_MS));
            const bool report = (attempts++ % 10) == 0;
            auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t info = {};
            if (!netif || !esp_netif_is_netif_up(netif) ||
                esp_netif_get_ip_info(netif, &info) != ESP_OK || !info.ip.addr) {
                if (report) {
                    ESP_LOGW("ENVPUSH", "netif not ready");
                }
                continue;
            }
            esp_ip6_addr_t addr6[3] = {};
            const int addr_count = esp_netif_get_all_ip6(netif, addr6);
            bool has_global6 = false;
            for (int i = 0; i < addr_count; ++i) {
                const uint8_t top = addr6[i].addr[0];
                // Only a globally routable address can reach the speaker;
                // link-local (fe80::/10) and ULA (fc00::/7) cannot.
                if (top != 0xfe && (top & 0xfe) != 0xfc) {
                    has_global6 = true;
                }
            }
            if (report) {
                ESP_LOGI("ENVPUSH", "ipv4=" IPSTR " ipv6_count=%d global=%d", IP2STR(&info.ip),
                         addr_count, has_global6);
                for (int i = 0; i < addr_count; ++i) {
                    ESP_LOGI("ENVPUSH", "  addr6[%d]=%s", i,
                             ip6addr_ntoa((const ip6_addr_t*)&addr6[i]));
                }
            }
            board->PushEnvironmentOnce(has_global6, report);
        }
    }

    void PushEnvironmentOnce(bool has_ipv6, bool report) {
        const std::string body = ht1621_sniffer_.GetReadingJson();
        const bool available = body.find("\"available\":true") != std::string::npos;
        if (!available && !report) return;
        static const char* const kPushUrls[] = {ENVIRONMENT_PUSH_URL, ENVIRONMENT_PUSH_URL_IPV6};
        for (const char* push_url : kPushUrls) {
            if (!has_ipv6 && push_url == kPushUrls[0]) continue;
            esp_http_client_config_t config = {};
            config.url = push_url;
            config.method = HTTP_METHOD_POST;
            config.timeout_ms = 4000;
            config.disable_auto_redirect = true;
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client == nullptr) {
                continue;
            }
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
            const esp_err_t err = esp_http_client_perform(client);
            const int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
            if (report) ESP_LOGI("ENVPUSH", "frame=%s via=%s status=%d result=%s",
                available ? "yes" : "no", push_url == kPushUrls[0] ? "IPv6" : "IPv4",
                status, esp_err_to_name(err));
            if (err == ESP_OK && status >= 200 && status < 300) {
                return;
            }
        }
    }

    void InitializeEnvironmentTool() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.environment.get_reading",
            "读取本机连接的空气质量检测器。返回温度、湿度、CO2、甲醛(HCHO)、"
            "TVOC、数据新鲜度和HT1621原始帧。字段为null表示该位尚未完成可靠标定，"
            "不要猜测null字段。",
            PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                return ht1621_sniffer_.GetReadingJson();
            });
    }

public:
    Yn038XiaozhiBoard()
        : boot_button_(BOOT_BUTTON_GPIO),
          ht1621_sniffer_(HT1621_SNIFFER_GPIO_CS,
                          HT1621_SNIFFER_GPIO_WR,
                          HT1621_SNIFFER_GPIO_DATA) {
#ifdef CONFIG_YN038_TFT_DISPLAY
        InitializeDisplay();
#endif
        InitializeButton();
#if HT1621_SNIFFER_ENABLED
        ht1621_sniffer_.Start();
#ifdef CONFIG_YN038_TFT_DISPLAY
        StartDashboard();
#endif
        InitializeEnvironmentTool();
#if ENVIRONMENT_PUSH_ENABLED
        StartEnvironmentPush();
#endif
#endif
    }

#ifdef CONFIG_YN038_TFT_DISPLAY
    Display* GetDisplay() override { return display_; }
#endif

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }
};

DECLARE_BOARD(Yn038XiaozhiBoard);
