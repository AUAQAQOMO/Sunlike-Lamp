#include "power_mgmt.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "config.h"

namespace {
bool s_wokeFromSleep = false;
}

void power_init() {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_wokeFromSleep = (cause == ESP_SLEEP_WAKEUP_GPIO);

    // 三个引脚都做低电平唤醒源。
    //   * C3 没有 RTC IO（SOC_RTCIO_PIN_COUNT = 0），rtc_gpio_* 系列 API 不存在；
    //     但 GPIO 由常电域供电，light sleep 期间 INPUT_PULLUP 的上拉是保持的，
    //     所以非 RTC 引脚（GPIO6）也能安全地当唤醒源。
    //   * 不用 deep sleep 的原因见 power_mgmt.h。
    gpio_wakeup_enable((gpio_num_t)PIN_EC11_A,   GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_EC11_B,   GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_EC11_KEY, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

bool power_woke_from_sleep() {
    return s_wokeFromSleep;
}

void power_sleep() {
    Serial.flush();
    delay(20);                          // 让串口最后一帧发完
    esp_light_sleep_start();            // 阻塞直到被 GPIO 唤醒
}
