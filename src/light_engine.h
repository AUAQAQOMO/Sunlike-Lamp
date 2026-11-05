#pragma once
#include <stdint.h>
#include "config.h"
#include "pwm_hal.h"

// ============================================================================
// LightEngine —— 双色温混光引擎 + 渐变插值
//
// 核心约束（来自需求）：
//   任何色温下，两路平均电流之和 <= 单路满电流（666.7mA），
//   即 占空比_warm + 占空比_cool <= 1023，总功率恒定、调色温不改变亮度。
//
// 亮度曲线：gamma 2.2（感知线性）。20kHz/10bit 下暗端档位有限，
//   因此亮度下限限制在 2%，并且旋钮步进做了“保证占空比变化 >= 1 级”的自适应。
//
// 渐变：由本类在主循环里按时间插值（每 TWEEN_TICK_MS 写一次占空比），
//   可以做到秒级的柔和过渡；PWM 波形本身仍由 LEDC 硬件产生。
// ============================================================================
class LightEngine {
public:
    bool begin();

    // ---- 状态设置 ----
    void setPower(bool on, bool animate = true);
    bool power() const { return on_; }

    void setBrightness(float b);       // 0.0 ~ 1.0
    void setCct(float t);              // 0.0=全暖(3000K) ~ 1.0=全冷(5700K)
    float brightness() const { return bright_; }
    float cct() const { return cct_; }

    // ---- 旋钮输入（steps 可正可负）----
    void nudgeBrightness(int steps);
    void nudgeCct(int steps);

    // ---- 主循环调度 ----
    void update();

    // 状态是否有变化（供持久化与云端上报使用），读取后自动清除
    bool takeDirty();

private:
    void     calcTarget(uint32_t &warm, uint32_t &cool) const;
    uint32_t totalDutyOf(float b) const;
    uint32_t fadeMsForDistance(uint32_t warm, uint32_t cool) const;
    void     beginTween(uint32_t warm, uint32_t cool, uint32_t ms);
    void     stepTween();

    PwmHal pwm_;

    float bright_ = 0.50f;      // 上电默认亮度
    float cct_    = 0.40f;      // 上电默认偏暖（约 4080K）
    bool  on_     = false;

    uint32_t target_w_ = 0;     // 期望目标
    uint32_t target_c_ = 0;
    uint32_t out_w_    = 0;     // 当前实际输出
    uint32_t out_c_    = 0;

    // ---- 渐变插值状态 ----
    uint32_t tw_from_w_ = 0;
    uint32_t tw_from_c_ = 0;
    uint32_t tw_to_w_   = 0;
    uint32_t tw_to_c_   = 0;
    uint32_t tw_start_  = 0;
    uint32_t tw_dur_    = 1;
    bool     tw_active_ = false;
    uint32_t last_tick_ = 0;

    uint32_t next_fade_ms_ = 0; // 0 = 按占空比距离自适应；非 0 = 本次指定时长
    bool     state_dirty_  = false;
};
