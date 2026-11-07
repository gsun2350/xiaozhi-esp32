#pragma once
#include <atomic>
#include <functional>
#include <vector>

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include "assets/lang_config.h"
#include "board.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "sdkconfig.h"

class PowerManager {
private:
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_timer_handle_t volume_timer_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    std::atomic<uint32_t> battery_level_{0};
    std::atomic<bool> is_charging_{false};
    std::atomic<bool> is_low_battery_{false};
    int ticks_ = 0;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 20;

    void Volumecontrol() {
        int VolumeButton_adc_value;
        ESP_ERROR_CHECK(
            adc_oneshot_read(adc_handle_, POWER_VOLUME_ADC_CHANNEL, &VolumeButton_adc_value));
        // ESP_LOGI("VolumeButton_adc_value", "VolumeButton ADC value: %d ",
        // VolumeButton_adc_value);
        if (1100 < VolumeButton_adc_value && VolumeButton_adc_value < 2800) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            auto codec = board.GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        }

        if (50 < VolumeButton_adc_value && VolumeButton_adc_value < 1000) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            auto codec = board.GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        }
    }

    void CheckBatteryStatus() {
        int usb_usb_adc_value0;
        ESP_ERROR_CHECK(
            adc_oneshot_read(adc_handle_, POWER_USBIN_ADC_CHANNEL, &usb_usb_adc_value0));
        bool new_charging_status = (1500 < usb_usb_adc_value0 && usb_usb_adc_value0 < 4000);
        // ESP_LOGE("powercontrol", "USB ADC VALUE 1: %d", usb_usb_adc_value0);

        if (new_charging_status != is_charging_.load()) {
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_.load());
            }
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, POWER_BATTERY_ADC_CHANNEL, &adc_value));

        // 将 ADC 值添加到队列中
        adc_values_.push_back(adc_value);
        if (adc_values_.size() > kBatteryAdcDataCount) {
            adc_values_.erase(adc_values_.begin());
        }
        uint32_t average_adc = 0;
        for (auto value : adc_values_) {
            average_adc += value;
        }
        average_adc /= adc_values_.size();

        // 定义电池电量区间
        const struct {
            uint16_t adc;
            uint8_t level;
        } levels[] = {{1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100}};

        // 低于最低值时
        if (average_adc < levels[0].adc) {
            battery_level_ = 0;
        }
        // 高于最高值时
        else if (average_adc >= levels[5].adc) {
            battery_level_ = 100;
        } else {
            // 线性插值计算中间值
            for (int i = 0; i < 5; i++) {
                if (average_adc >= levels[i].adc && average_adc < levels[i + 1].adc) {
                    float ratio = static_cast<float>(average_adc - levels[i].adc) /
                                  (levels[i + 1].adc - levels[i].adc);
                    battery_level_ =
                        levels[i].level + ratio * (levels[i + 1].level - levels[i].level);
                    break;
                }
            }
        }

        // Check low battery status
        if (adc_values_.size() >= kBatteryAdcDataCount) {
            bool new_low_battery_status = battery_level_.load() <= kLowBatteryLevel;
            if (new_low_battery_status != is_low_battery_.load()) {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(is_low_battery_.load());
                }
            }
        }

        ESP_LOGI("PowerManager", "ADC value: %d average: %lu level: %lu", adc_value,
                 static_cast<unsigned long>(average_adc),
                 static_cast<unsigned long>(battery_level_.load()));
    }

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        // 初始化电源控制引脚
        gpio_config_t powercontgpio_conf = {};
        powercontgpio_conf.intr_type = GPIO_INTR_DISABLE;
        powercontgpio_conf.mode = GPIO_MODE_OUTPUT;
        powercontgpio_conf.pin_bit_mask = (1ULL << Power_Control);
        powercontgpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        powercontgpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&powercontgpio_conf);
        // vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(Power_Control, 1);
        ESP_LOGI("powercontrol", "turnded on ...");

        // 实机日志显示参考固件的 GPIO16 电源键判定会误触发重启/关机。
        // 保持电源锁存脚为高电平，仅轮询音量 ADC；电源键行为待硬件验证后再启用。
        esp_timer_create_args_t volume_timer_args = {
            .callback =
                [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->Volumecontrol();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "volume_control_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&volume_timer_args, &volume_timer_handle_));
        // 初始化充电引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->CheckBatteryStatus();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = POWER_CBS_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_BATTERY_ADC_CHANNEL,
                                                   &chan_config));  // 电池电量
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_VOLUME_ADC_CHANNEL,
                                                   &chan_config));  // 音量键
        ESP_ERROR_CHECK(
            adc_oneshot_config_channel(adc_handle_, POWER_USBIN_ADC_CHANNEL, &chan_config));  // usb
        // ADC 和定时器句柄全部准备好后再启动回调，避免初始化竞态。
        ESP_ERROR_CHECK(esp_timer_start_periodic(volume_timer_handle_, 200000));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (volume_timer_handle_) {
            esp_timer_stop(volume_timer_handle_);
            esp_timer_delete(volume_timer_handle_);
        }
        if (adc_handle_ != nullptr) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() {
        // 如果电量已经满了，则不再显示充电中
        if (battery_level_.load() == 100) {
            return false;
        }
        return is_charging_.load();
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_.load();
    }

    uint8_t GetBatteryLevel() { return static_cast<uint8_t>(battery_level_.load()); }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
};
