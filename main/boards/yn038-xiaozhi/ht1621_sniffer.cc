#include "ht1621_sniffer.h"
#include "cjson_utils.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <array>
#include <string>

namespace {

constexpr char kTag[] = "HT1621";
constexpr size_t kQueueLength = 256;
constexpr uint8_t kWriteMode = 0b101;
constexpr uint8_t kCommandMode = 0b100;
constexpr uint8_t kReadMode = 0b110;

struct DigitAnchor {
    uint8_t raw;
    uint8_t digit;
};

// Standard seven-segment masks.  Segment wiring can be permuted independently
// for each LCD position; Hamming distances remain invariant under that
// permutation, which lets confirmed anchors identify additional digits without
// guessing a physical COM/SEG order.
constexpr std::array<uint8_t, 10> kSevenSegment = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

constexpr DigitAnchor kTempTens[] = {{0x16, 1}, {0xDB, 2}};
constexpr DigitAnchor kTempOnes[] = {{0xBF, 0}, {0x16, 1}, {0x7F, 9}};
constexpr DigitAnchor kHumidityTens[] = {{0x7D, 5}};
constexpr DigitAnchor kHumidityOnes[] = {{0x76, 4}, {0xFF, 8}, {0x7F, 9}};
constexpr DigitAnchor kTvocOnes[] = {{0x6C, 4}, {0xBE, 6}, {0xFE, 8}};
constexpr DigitAnchor kTvocTenths[] = {{0xD7, 2}, {0xF5, 3}};
constexpr DigitAnchor kTvocHundredths[] = {{0x61, 1}};
constexpr DigitAnchor kTvocLeading[] = {{0xFB, 0}};
constexpr DigitAnchor kHchoOnes[] = {{0xF5, 2}, {0xBD, 5}, {0xE1, 7}, {0xBF, 9}};
constexpr DigitAnchor kHchoTenths[] = {{0xFB, 0}, {0x61, 1}};
constexpr DigitAnchor kHchoHundredths[] = {{0xFB, 0}};
constexpr DigitAnchor kCo2Ones[] = {{0x60, 0}, {0xBF, 5}};
constexpr DigitAnchor kCo2Tens[] = {{0xFA, 0}, {0xD6, 2}, {0xFD, 9}};
constexpr DigitAnchor kCo2Hundreds[] = {{0x6D, 4}, {0xBD, 5}};
constexpr DigitAnchor kCo2Thousands[] = {{0xFB, 0}};

int Popcount8(uint8_t value) {
    int count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

template <size_t N>
int DecodeDigit(uint8_t raw, const DigitAnchor (&anchors)[N]) {
    for (const auto& anchor : anchors) {
        if (anchor.raw == raw) {
            return anchor.digit;
        }
    }

    int result = -1;
    for (int candidate = 0; candidate <= 9; ++candidate) {
        bool matches = true;
        for (const auto& anchor : anchors) {
            const int raw_distance = Popcount8(raw ^ anchor.raw);
            const int segment_distance =
                Popcount8(kSevenSegment[candidate] ^ kSevenSegment[anchor.digit]);
            if (raw_distance != segment_distance) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (result != -1) {
                return -1;  // Ambiguous: wait for another confirmed anchor.
            }
            result = candidate;
        }
    }
    return result;
}

bool ReadByte(const std::array<uint8_t, 64>& ram,
              const std::array<bool, 64>& valid,
              size_t byte_index,
              uint8_t& value) {
    const size_t high = byte_index * 2;
    const size_t low = high + 1;
    if (low >= ram.size() || !valid[high] || !valid[low]) {
        return false;
    }
    value = static_cast<uint8_t>((ram[high] << 4U) | ram[low]);
    return true;
}

void AddMaybeNumber(cJSON* object, const char* name, bool valid, double value) {
    if (valid) {
        cJSON_AddNumberToObject(object, name, value);
    } else {
        cJSON_AddNullToObject(object, name);
    }
}

char HexNibble(uint8_t value) {
    value &= 0x0F;
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + value - 10);
}

}  // namespace

Ht1621Sniffer::Ht1621Sniffer(gpio_num_t cs_gpio,
                             gpio_num_t wr_gpio,
                             gpio_num_t data_gpio)
    : cs_gpio_(cs_gpio), wr_gpio_(wr_gpio), data_gpio_(data_gpio) {}

bool Ht1621Sniffer::ConfigureInput(gpio_num_t gpio,
                                  gpio_pullup_t pull_up,
                                  gpio_int_type_t interrupt) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = pull_up;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = interrupt;
    const esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "gpio_config failed for GPIO%d: %s", gpio, esp_err_to_name(result));
        return false;
    }
    return true;
}

bool Ht1621Sniffer::Start() {
    if (started_) {
        return true;
    }

    queue_ = xQueueCreate(kQueueLength, sizeof(Event));
    if (queue_ == nullptr) {
        ESP_LOGE(kTag, "failed to create capture queue");
        return false;
    }

    // CS receives a weak pull-up only so an unplugged cable cannot create an
    // interrupt storm. WR and DATA remain completely un-biased inputs.
    if (!ConfigureInput(cs_gpio_, GPIO_PULLUP_ENABLE, GPIO_INTR_ANYEDGE) ||
        !ConfigureInput(wr_gpio_, GPIO_PULLUP_DISABLE, GPIO_INTR_POSEDGE) ||
        !ConfigureInput(data_gpio_, GPIO_PULLUP_DISABLE, GPIO_INTR_DISABLE)) {
        return false;
    }

    esp_err_t result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "failed to install GPIO ISR service: %s", esp_err_to_name(result));
        return false;
    }

    if (xTaskCreate(&Ht1621Sniffer::TaskEntry,
                    "ht1621_sniffer",
                    4096,
                    this,
                    4,
                    &task_) != pdPASS) {
        ESP_LOGE(kTag, "failed to create capture task");
        task_ = nullptr;
        return false;
    }

    result = gpio_isr_handler_add(cs_gpio_, &Ht1621Sniffer::CsIsr, this);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "failed to add CS ISR: %s", esp_err_to_name(result));
        return false;
    }

    result = gpio_isr_handler_add(wr_gpio_, &Ht1621Sniffer::WrIsr, this);
    if (result != ESP_OK) {
        gpio_isr_handler_remove(cs_gpio_);
        ESP_LOGE(kTag, "failed to add WR ISR: %s", esp_err_to_name(result));
        return false;
    }

    started_ = true;

    if (settle_task_ == nullptr) {
        xTaskCreate(&Ht1621Sniffer::SettleTaskEntry, "ht1621_settle", 4096, this, 2,
                    &settle_task_);
    }

    // Synchronize at the next actual CS assertion. A suffix can accidentally
    // look like a valid write prefix if capture begins mid-transaction.

    ESP_LOGI(kTag,
             "read-only sniffer ready: CS=GPIO%d WR=GPIO%d DATA=GPIO%d; RD is not required",
             cs_gpio_,
             wr_gpio_,
             data_gpio_);
    return true;
}

void IRAM_ATTR Ht1621Sniffer::QueueFromIsr(EventType type, uint8_t bit) {
    if (queue_ == nullptr) {
        return;
    }

    const Event event = {type, bit};
    BaseType_t high_task_woken = pdFALSE;
    if (xQueueSendFromISR(queue_, &event, &high_task_woken) != pdTRUE) {
        capture_dropped_ = true;
    }
    if (high_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void IRAM_ATTR Ht1621Sniffer::CsIsr(void* arg) {
    auto* self = static_cast<Ht1621Sniffer*>(arg);
    if (self == nullptr) {
        return;
    }
    const bool selected = gpio_get_level(self->cs_gpio_) == 0;
    self->QueueFromIsr(selected ? EventType::kBegin : EventType::kEnd, 0);
}

void IRAM_ATTR Ht1621Sniffer::WrIsr(void* arg) {
    auto* self = static_cast<Ht1621Sniffer*>(arg);
    if (self == nullptr || gpio_get_level(self->cs_gpio_) != 0) {
        return;
    }
    self->QueueFromIsr(EventType::kBit,
                       static_cast<uint8_t>(gpio_get_level(self->data_gpio_) != 0));
}

void Ht1621Sniffer::TaskEntry(void* arg) {
    auto* self = static_cast<Ht1621Sniffer*>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    self->Task();
}

void Ht1621Sniffer::Task() {
    Event event = {};
    while (xQueueReceive(queue_, &event, portMAX_DELAY) == pdTRUE) {
        switch (event.type) {
            case EventType::kBegin:
                transaction_active_ = true;
                transaction_overflow_ = false;
                bit_count_ = 0;
                break;
            case EventType::kBit:
                if (!transaction_active_) {
                    break;
                }
                if (bit_count_ < bits_.size()) {
                    bits_[bit_count_++] = event.bit & 1U;
                } else {
                    transaction_overflow_ = true;
                }
                break;
            case EventType::kEnd:
                if (transaction_active_) {
                    DecodeTransaction();
                }
                transaction_active_ = false;
                break;
        }
    }
}

void Ht1621Sniffer::DecodeTransaction() {
    const bool capture_dropped = capture_dropped_;
    capture_dropped_ = false;
    if (capture_dropped || transaction_overflow_) {
        ESP_LOGW(kTag,
                 "capture discarded: bits=%u overflow=%d dropped_events=%lu",
                 static_cast<unsigned>(bit_count_),
                 transaction_overflow_ ? 1 : 0,
                 capture_dropped ? 1UL : 0UL);
        return;
    }
    if (bit_count_ < 3) {
        return;
    }

    const uint8_t mode =
        static_cast<uint8_t>((bits_[0] << 2) | (bits_[1] << 1) | bits_[2]);
    ++frame_sequence_;

    if (mode == kCommandMode) {
        if (bit_count_ < 12) {
            ESP_LOGD(kTag,
                     "frame=%lu incomplete command bits=%u",
                     static_cast<unsigned long>(frame_sequence_),
                     static_cast<unsigned>(bit_count_));
            return;
        }
        uint16_t command = 0;
        for (size_t i = 3; i < 12; ++i) {
            command = static_cast<uint16_t>((command << 1) | bits_[i]);
        }
        ESP_LOGI(kTag,
                 "frame=%lu command=0x%03X",
                 static_cast<unsigned long>(frame_sequence_),
                 command);
        return;
    }

    if (mode == kReadMode) {
        ESP_LOGD(kTag,
                 "frame=%lu read request observed; connect RD only if readback capture is needed",
                 static_cast<unsigned long>(frame_sequence_));
        return;
    }

    if (mode != kWriteMode || bit_count_ < 13) {
        ESP_LOGD(kTag,
                 "frame=%lu unsupported mode=0b%u%u%u bits=%u",
                 static_cast<unsigned long>(frame_sequence_),
                 bits_[0],
                 bits_[1],
                 bits_[2],
                 static_cast<unsigned>(bit_count_));
        return;
    }

    uint8_t start_address = 0;
    for (size_t i = 3; i < 9; ++i) {
        start_address = static_cast<uint8_t>((start_address << 1) | bits_[i]);
    }

    const size_t payload_bits = bit_count_ - 9;
    const size_t nibble_count = payload_bits / 4;
    const size_t trailing_bits = payload_bits % 4;
    if (trailing_bits != 0) {
        return;  // A missed WR edge shifts data; leave the last complete write intact.
    }
    char data_hex[(kMaxTransactionBits - 9) / 4 + 1] = {};
    bool changed = false;

    portENTER_CRITICAL(&reading_lock_);
    for (size_t nibble_index = 0; nibble_index < nibble_count; ++nibble_index) {
        uint8_t value = 0;
        // HT1621 sends display data D0, D1, D2, D3 (least significant bit first).
        for (size_t bit_index = 0; bit_index < 4; ++bit_index) {
            value |= static_cast<uint8_t>(
                bits_[9 + nibble_index * 4 + bit_index] << bit_index);
        }

        const size_t address =
            static_cast<size_t>((start_address + nibble_index) & 0x3F);
        if (!ram_valid_[address] || ram_[address] != value) {
            changed = true;
        }
        ram_[address] = value;
        ram_valid_[address] = true;
        data_hex[nibble_index] = HexNibble(value);
    }
    data_hex[nibble_count] = '\0';

    char ram_hex[kRamNibbles + 1] = {};
    for (size_t address = 0; address < kRamNibbles; ++address) {
        ram_hex[address] = ram_valid_[address] ? HexNibble(ram_[address]) : '?';
    }
    ram_hex[kRamNibbles] = '\0';

    RecentWrite& recent = recent_writes_[recent_write_index_ % kRecentWrites];
    recent.address = start_address;
    recent.count = static_cast<uint8_t>(nibble_count > 32 ? 32 : nibble_count);
    recent.sequence = frame_sequence_;
    for (size_t i = 0; i < sizeof(recent.data) - 1; ++i) {
        recent.data[i] = i < nibble_count && i < 32 ? HexNibble(ram_[static_cast<size_t>((start_address + i) & 0x3F)]) : '\0';
    }
    recent.data[sizeof(recent.data) - 1] = '\0';
    recent_write_index_ = (recent_write_index_ + 1) % kRecentWrites;

    // 屏幕是逐位写入的：写入过程中镜像常是“半新半旧”。这里只标记待更新，
    // 由 SettleLoop 在总线静默 200ms 后再整体解码，避免发布撕裂帧。
    pending_update_ = true;
    last_write_us_ = esp_timer_get_time();
    if (pending_since_us_ == 0) {
        pending_since_us_ = last_write_us_;
    }
    portEXIT_CRITICAL(&reading_lock_);

    ESP_LOGD(kTag,
             "frame=%lu write addr=0x%02X count=%u data=%s changed=%d trailing=%u ram=%s",
             static_cast<unsigned long>(frame_sequence_),
             start_address,
             static_cast<unsigned>(nibble_count),
             data_hex,
             changed ? 1 : 0,
             static_cast<unsigned>(trailing_bits),
             ram_hex);
}

void Ht1621Sniffer::SettleTaskEntry(void* arg) {
    static_cast<Ht1621Sniffer*>(arg)->SettleLoop();
}

void Ht1621Sniffer::SettleLoop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (!pending_update_) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        const bool quiet = now - last_write_us_ >= 200000;
        const bool stale = now - pending_since_us_ >= 1500000;
        if (!quiet && !stale) {
            continue;
        }
        std::array<uint8_t, kRamNibbles> ram_snapshot;
        std::array<bool, kRamNibbles> valid_snapshot;
        portENTER_CRITICAL(&reading_lock_);
        ram_snapshot = ram_;
        valid_snapshot = ram_valid_;
        pending_update_ = false;
        pending_since_us_ = 0;
        portEXIT_CRITICAL(&reading_lock_);
        UpdateEnvironmentReading(ram_snapshot, valid_snapshot);
    }
}

void Ht1621Sniffer::UpdateEnvironmentReading(
    const std::array<uint8_t, kRamNibbles>& ram,
    const std::array<bool, kRamNibbles>& ram_valid) {
    std::array<uint8_t, 16> bytes{};
    std::array<bool, 16> valid{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        valid[i] = ReadByte(ram, ram_valid, i, bytes[i]);
    }

    const int temp_tens = valid[1] ? DecodeDigit(bytes[1], kTempTens) : -1;
    const int temp_ones = valid[2] ? DecodeDigit(bytes[2], kTempOnes) : -1;
    const int humidity_tens = valid[3] ? DecodeDigit(bytes[3], kHumidityTens) : -1;
    const int humidity_ones = valid[4] ? DecodeDigit(bytes[4], kHumidityOnes) : -1;

    const int tvoc_d0 = valid[5] ? DecodeDigit(bytes[5], kTvocOnes) : -1;
    const int tvoc_d1 = valid[6] ? DecodeDigit(bytes[6], kTvocTenths) : -1;
    const int tvoc_d2 = valid[7] ? DecodeDigit(bytes[7], kTvocHundredths) : -1;
    const int tvoc_d3 = valid[8] ? DecodeDigit(bytes[8], kTvocLeading) : -1;

    const int hcho_d0 = valid[9] ? DecodeDigit(bytes[9], kHchoOnes) : -1;
    const int hcho_d1 = valid[10] ? DecodeDigit(bytes[10], kHchoTenths) : -1;
    const int hcho_d2 = valid[11] ? DecodeDigit(bytes[11], kHchoHundredths) : -1;

    const int co2_d0 = valid[12] ? DecodeDigit(bytes[12], kCo2Ones) : -1;
    const int co2_d1 = valid[13] ? DecodeDigit(bytes[13], kCo2Tens) : -1;
    const int co2_d2 = valid[14] ? DecodeDigit(bytes[14], kCo2Hundreds) : -1;
    const int co2_d3 = valid[15] ? DecodeDigit(bytes[15], kCo2Thousands) : -1;

    const bool temperature_valid = temp_tens >= 0 && temp_ones >= 0;
    const bool humidity_valid = humidity_tens >= 0 && humidity_ones >= 0;
    const bool tvoc_valid = tvoc_d0 >= 0 && tvoc_d1 >= 0 && tvoc_d2 >= 0 && tvoc_d3 >= 0;
    const bool hcho_valid = hcho_d0 >= 0 && hcho_d1 >= 0 && hcho_d2 >= 0;
    const bool co2_valid = co2_d0 >= 0 && co2_d1 >= 0 && co2_d2 >= 0 && co2_d3 >= 0;

    portENTER_CRITICAL(&reading_lock_);
    temperature_valid_ = temperature_valid;
    humidity_valid_ = humidity_valid;
    tvoc_valid_ = tvoc_valid;
    hcho_valid_ = hcho_valid;
    co2_valid_ = co2_valid;
    if (temperature_valid) {
        temperature_c_ = temp_tens * 10 + temp_ones;
    }
    if (humidity_valid) {
        humidity_percent_ = humidity_tens * 10 + humidity_ones;
    }
    if (tvoc_valid) {
        tvoc_thousandths_mg_m3_ =
            tvoc_d3 * 1000 + tvoc_d2 * 100 + tvoc_d1 * 10 + tvoc_d0;
    }
    if (hcho_valid) {
        hcho_thousandths_mg_m3_ = hcho_d2 * 100 + hcho_d1 * 10 + hcho_d0;
    }
    if (co2_valid) {
        co2_ppm_ = co2_d3 * 1000 + co2_d2 * 100 + co2_d1 * 10 + co2_d0;
    }
    reading_timestamp_us_ = esp_timer_get_time();
    portEXIT_CRITICAL(&reading_lock_);

    if (temperature_valid || humidity_valid || tvoc_valid || hcho_valid || co2_valid) {
        ESP_LOGD(kTag,
                 "environment decoded: temp=%s humidity=%s co2=%s hcho=%s tvoc=%s",
                 temperature_valid ? "ok" : "?",
                 humidity_valid ? "ok" : "?",
                 co2_valid ? "ok" : "?",
                 hcho_valid ? "ok" : "?",
                 tvoc_valid ? "ok" : "?");
    }
}

std::string Ht1621Sniffer::GetReadingJson() const {
    int temperature = 0;
    int humidity = 0;
    int co2 = 0;
    int hcho = 0;
    int tvoc = 0;
    bool temperature_valid = false;
    bool humidity_valid = false;
    bool co2_valid = false;
    bool hcho_valid = false;
    bool tvoc_valid = false;
    int64_t timestamp_us = 0;
    std::array<uint8_t, kRamNibbles> ram;
    std::array<bool, kRamNibbles> ram_valid;

    portENTER_CRITICAL(&reading_lock_);
    temperature = temperature_c_;
    humidity = humidity_percent_;
    co2 = co2_ppm_;
    hcho = hcho_thousandths_mg_m3_;
    tvoc = tvoc_thousandths_mg_m3_;
    temperature_valid = temperature_valid_;
    humidity_valid = humidity_valid_;
    co2_valid = co2_valid_;
    hcho_valid = hcho_valid_;
    tvoc_valid = tvoc_valid_;
    timestamp_us = reading_timestamp_us_;
    ram = ram_;
    ram_valid = ram_valid_;
    portEXIT_CRITICAL(&reading_lock_);

    char raw[33] = {};
    for (size_t i = 0; i < 32; ++i) {
        raw[i] = ram_valid[i] ? HexNibble(ram[i]) : '?';
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{\"error\":\"out of memory\"}";
    }
    cJSON_AddBoolToObject(root.get(), "available", timestamp_us > 0);
    cJSON_AddBoolToObject(root.get(), "complete",
                          temperature_valid && humidity_valid && co2_valid && hcho_valid &&
                              tvoc_valid);
    AddMaybeNumber(root.get(), "temperature_c", temperature_valid, temperature);
    AddMaybeNumber(root.get(), "humidity_percent", humidity_valid, humidity);
    AddMaybeNumber(root.get(), "co2_ppm", co2_valid, co2);
    AddMaybeNumber(root.get(), "hcho_mg_m3", hcho_valid, hcho / 1000.0);
    AddMaybeNumber(root.get(), "tvoc_mg_m3", tvoc_valid, tvoc / 1000.0);
    const int64_t now_us = esp_timer_get_time();
    cJSON_AddNumberToObject(root.get(), "age_ms",
                            timestamp_us > 0 ? (now_us - timestamp_us) / 1000.0 : -1);
    cJSON_AddStringToObject(root.get(), "raw_ht1621", raw);
    cJSON_AddStringToObject(root.get(), "decoder",
                            "confirmed anchors plus segment-distance inference");
    cJSON* writes = cJSON_AddArrayToObject(root.get(), "writes");
    if (writes != nullptr) {
        for (size_t i = 0; i < kRecentWrites; ++i) {
            const RecentWrite& item = recent_writes_[(recent_write_index_ + i) % kRecentWrites];
            if (item.count == 0) {
                continue;
            }
            cJSON* entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "addr", item.address);
            cJSON_AddNumberToObject(entry, "count", item.count);
            cJSON_AddNumberToObject(entry, "seq", item.sequence);
            cJSON_AddStringToObject(entry, "data", item.data);
            cJSON_AddItemToArray(writes, entry);
        }
    }
    CJsonStringUniquePtr json(cJSON_PrintUnformatted(root.get()));
    return json != nullptr ? std::string(json.get()) : std::string("{}");
}

