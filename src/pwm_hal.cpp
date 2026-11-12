#include "pwm_hal.h"
#include "config.h"
#include <driver/ledc.h>

namespace {

const ledc_mode_t    kMode  = LEDC_LOW_SPEED_MODE;      // ESP32-C3 只有一个 LEDC 组
const ledc_timer_t   kTimer = LEDC_TIMER_0;             // 两路共用同一计时器 => 同频同相
const ledc_channel_t kChannel[2] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };

}  // namespace

bool PwmHal::begin(uint8_t gpio_warm, uint8_t gpio_cool, uint32_t freq_hz, uint8_t res_bits) {
    gpio_[0] = gpio_warm;
    gpio_[1] = gpio_cool;
    duty_limit_ = (1u << res_bits) - 1u;

    // 复位期间 GPIO 为浮空，LGS63042 的 EN 靠内部 800k 下拉保持关断；
    // 先写 LOW 到输出寄存器再置 OUTPUT，彻底消除引脚配置瞬间的微秒级毛刺脉冲。
    digitalWrite(gpio_warm, LOW);
    pinMode(gpio_warm, OUTPUT);
    digitalWrite(gpio_cool, LOW);
    pinMode(gpio_cool, OUTPUT);

    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = kMode;
    tcfg.duty_resolution = static_cast<ledc_timer_bit_t>(res_bits);
    tcfg.timer_num       = kTimer;
    tcfg.freq_hz         = freq_hz;
    tcfg.clk_cfg         = LEDC_USE_XTAL_CLK;           // 关键：锁死 40MHz 晶振
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        return false;                                    // 频率/分辨率组合非法
    }

    for (int i = 0; i < 2; ++i) {
        ledc_channel_config_t ccfg = {};
        ccfg.gpio_num   = gpio_[i];
        ccfg.speed_mode = kMode;
        ccfg.channel    = kChannel[i];
        ccfg.intr_type  = LEDC_INTR_DISABLE;
        ccfg.timer_sel  = kTimer;
        ccfg.duty       = 0;
        ccfg.hpoint     = 0;
        if (ledc_channel_config(&ccfg) != ESP_OK) {
            return false;
        }
    }

    ready_ = true;
    return true;
}

void PwmHal::write(uint8_t idx, uint32_t duty) {
    if (!ready_ || idx > 1) return;
    if (duty > duty_limit_) duty = duty_limit_;

    ledc_set_duty(kMode, kChannel[idx], duty);
    ledc_update_duty(kMode, kChannel[idx]);             // 下一个 PWM 周期生效
}

uint32_t PwmHal::currentDuty(uint8_t idx) const {
    if (!ready_ || idx > 1) return 0;
    return ledc_get_duty(kMode, kChannel[idx]);
}
