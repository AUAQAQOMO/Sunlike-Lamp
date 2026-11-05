#pragma once
#include <stdint.h>

// ============================================================================
// PwmHal —— 双通道 LEDC 底层封装
//
//   * 强制使用 XTAL 40MHz 时钟源（最稳定，不受 APB/DFS 影响）
//   * 两路共用同一个 LEDC 定时器 => 严格同频同分辨率
//   * 只负责“把某个占空比写到硬件”，渐变由上层 LightEngine 的主循环插值完成
//
// 为什么不用 LEDC 的硬件 fade（ledc_set_fade_with_time）：
//   该接口用整数 scale 控制“每个 PWM 周期改变几级占空比”，最小为 1。
//   20kHz / 10bit 下 1 级 = 50us，满量程 1023 级最快也要 51ms，
//   因此秒级的开关灯渐变在硬件 fade 上物理上无法实现（会被压缩到 ~51ms）。
//   渐变改由主循环每 16ms 写一次占空比，时长完全可控。
// ============================================================================
class PwmHal {
public:
    // 初始化两个通道，返回 false 表示频率/分辨率组合不被硬件接受
    bool begin(uint8_t gpio_warm, uint8_t gpio_cool, uint32_t freq_hz, uint8_t res_bits);

    // 写入占空比（超出范围自动钳位）
    void write(uint8_t idx, uint32_t duty);

    // 读取硬件当前占空比
    uint32_t currentDuty(uint8_t idx) const;

private:
    bool    ready_   = false;
    uint8_t gpio_[2] = {0, 0};
};
