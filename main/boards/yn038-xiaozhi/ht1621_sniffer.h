#pragma once

#include <driver/gpio.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class Ht1621Sniffer {
public:
    Ht1621Sniffer(gpio_num_t cs_gpio, gpio_num_t wr_gpio, gpio_num_t data_gpio);
    bool Start();
    std::string GetReadingJson() const;

private:
    enum class EventType : uint8_t { kBegin, kBit, kEnd };

    struct RecentWrite {
        uint8_t address;
        uint8_t count;
        uint32_t sequence;
        char data[33];
    };
    static constexpr size_t kRecentWrites = 8;

    struct Event {
        EventType type;
        uint8_t bit;
    };

    static constexpr size_t kMaxTransactionBits = 192;
    static constexpr size_t kRamNibbles = 64;

    static void IRAM_ATTR CsIsr(void* arg);
    static void IRAM_ATTR WrIsr(void* arg);
    static void TaskEntry(void* arg);
    static void SettleTaskEntry(void* arg);

    void Task();
    void SettleLoop();
    void DecodeTransaction();
    void UpdateEnvironmentReading(const std::array<uint8_t, kRamNibbles>& ram,
                                  const std::array<bool, kRamNibbles>& ram_valid);
    bool ConfigureInput(gpio_num_t gpio, gpio_pullup_t pull_up, gpio_int_type_t interrupt);
    void QueueFromIsr(EventType type, uint8_t bit);

    gpio_num_t cs_gpio_;
    gpio_num_t wr_gpio_;
    gpio_num_t data_gpio_;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t settle_task_ = nullptr;
    bool started_ = false;
    volatile bool pending_update_ = false;
    volatile int64_t last_write_us_ = 0;
    volatile int64_t pending_since_us_ = 0;
    bool transaction_active_ = false;
    bool transaction_overflow_ = false;
    size_t bit_count_ = 0;
    uint32_t frame_sequence_ = 0;
    volatile bool capture_dropped_ = false;
    std::array<uint8_t, kMaxTransactionBits> bits_{};
    std::array<uint8_t, kRamNibbles> ram_{};
    std::array<bool, kRamNibbles> ram_valid_{};
    mutable portMUX_TYPE reading_lock_ = portMUX_INITIALIZER_UNLOCKED;
    int temperature_c_ = 0;
    int humidity_percent_ = 0;
    int co2_ppm_ = 0;
    int hcho_thousandths_mg_m3_ = 0;
    int tvoc_thousandths_mg_m3_ = 0;
    bool temperature_valid_ = false;
    bool humidity_valid_ = false;
    bool co2_valid_ = false;
    bool hcho_valid_ = false;
    bool tvoc_valid_ = false;
    int64_t reading_timestamp_us_ = 0;
    std::array<RecentWrite, kRecentWrites> recent_writes_{};
    size_t recent_write_index_ = 0;
};

