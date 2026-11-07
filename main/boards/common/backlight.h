#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include <driver/gpio.h>
#include <esp_timer.h>


class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    // 仅控制物理输出，不修改持久化的正常亮度。默认启用，兼容其他板型。
    void SetEnabled(bool enabled);
    inline uint8_t brightness() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return brightness_;
    }

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
    mutable std::mutex mutex_;
    bool enabled_ = true;
};


class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
