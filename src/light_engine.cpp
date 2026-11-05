#include "light_engine.h"
#include "config.h"
#include <math.h>

namespace {
inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
}  // namespace

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool LightEngine::begin() {
    if (!pwm_.begin(PIN_PWM_WARM, PIN_PWM_COOL, PWM_FREQ_HZ, PWM_RES_BITS)) {
        return false;
    }
    out_w_ = out_c_ = 0;
    target_w_ = target_c_ = 0;
    tw_active_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// 亮度 -> 总占空比
//   为了让最暗档在 20kHz 下仍然能被 LGS63042 的 EN 引脚响应，
//   把可用占空比区间整体抬到底限 DUTY_MIN_USABLE 之上：
//     duty = DUTY_MIN_USABLE + (1023 - DUTY_MIN_USABLE) * brightness^2.2
// ---------------------------------------------------------------------------
uint32_t LightEngine::totalDutyOf(float b) const {
    b = clampf(b, 0.0f, 1.0f);
    if (b <= 0.0f) return 0;

    float d = (float)DUTY_MIN_USABLE
            + (float)(PWM_DUTY_MAX - DUTY_MIN_USABLE) * powf(b, GAMMA_VALUE);
    if (d > (float)PWM_DUTY_MAX) d = (float)PWM_DUTY_MAX;
    if (d < (float)DUTY_MIN_USABLE) d = (float)DUTY_MIN_USABLE;
    return (uint32_t)lroundf(d);
}

// ---------------------------------------------------------------------------
// 亮度 + 色温 -> 两路占空比（总电流守恒）
// ---------------------------------------------------------------------------
void LightEngine::calcTarget(uint32_t &warm, uint32_t &cool) const {
    uint32_t total = on_ ? totalDutyOf(bright_) : 0;
    if (total > PWM_DUTY_MAX) total = PWM_DUTY_MAX;

    // 说明：这里不对“每一路”单独设下限。
    // 极暗 + 中间色温时，较小那一路的占空比会低于 DUTY_MIN_USABLE（例如 total=8、T=0.4 时
    // 冷路只有 3 级 ≈ 150ns 高电平）。这是 10bit/20kHz 的固有代价：
    //   * 若把每路都抬到 8 级，最暗亮度会被抬高 2~8 倍，手感更差；
    //   * 实测若真的出现“暗端偏色/某路不亮”，把 config.h 的 DUTY_MIN_USABLE 或 BRIGHT_MIN 调大。
    uint32_t c = (uint32_t)lroundf((float)total * cct_);
    if (c > total) c = total;

    warm = total - c;
    cool = c;
}

// ---------------------------------------------------------------------------
// 按“距离当前输出多远”决定本次渐变时长
//   取两路变化量的较大值 * FADE_MS_PER_DUTY，再钳到 [MIN, MAX]。
//   两路用同一时长 => 过渡过程中不会偏色。
// ---------------------------------------------------------------------------
uint32_t LightEngine::fadeMsForDistance(uint32_t warm, uint32_t cool) const {
    const uint32_t d0 = (warm > out_w_) ? (warm - out_w_) : (out_w_ - warm);
    const uint32_t d1 = (cool > out_c_) ? (cool - out_c_) : (out_c_ - cool);
    const uint32_t d  = (d0 > d1) ? d0 : d1;

    uint32_t ms = d * FADE_MS_PER_DUTY;
    if (ms < FADE_MS_MIN) ms = FADE_MS_MIN;
    if (ms > FADE_MS_MAX) ms = FADE_MS_MAX;
    return ms;
}

// ---------------------------------------------------------------------------
// 启动一段渐变：从“当前输出”出发，避免中途改变目标时出现跳变
// ---------------------------------------------------------------------------
void LightEngine::beginTween(uint32_t warm, uint32_t cool, uint32_t ms) {
    tw_from_w_ = out_w_;
    tw_from_c_ = out_c_;
    tw_to_w_   = warm;
    tw_to_c_   = cool;
    tw_start_  = millis();
    tw_dur_    = (ms < 1) ? 1 : ms;
    tw_active_ = true;

    // 让下一帧就能推进一次（处理 millis() 从 0 附近起步的回绕也安全）
    last_tick_ = tw_start_ - TWEEN_TICK_MS;
}

// ---------------------------------------------------------------------------
// 按经过时间线性插值，并写入硬件
// ---------------------------------------------------------------------------
void LightEngine::stepTween() {
    const uint32_t now     = millis();
    const uint32_t elapsed = now - tw_start_;

    if (elapsed >= tw_dur_) {
        out_w_     = tw_to_w_;
        out_c_     = tw_to_c_;
        tw_active_ = false;
    } else {
        const float k = (float)elapsed / (float)tw_dur_;
        out_w_ = (uint32_t)lroundf((float)tw_from_w_ +
                                   ((float)tw_to_w_ - (float)tw_from_w_) * k);
        out_c_ = (uint32_t)lroundf((float)tw_from_c_ +
                                   ((float)tw_to_c_ - (float)tw_from_c_) * k);
    }

    pwm_.write(0, out_w_);
    pwm_.write(1, out_c_);
}

// ---------------------------------------------------------------------------
// 主循环调度
// ---------------------------------------------------------------------------
void LightEngine::update() {
    uint32_t w, c;
    calcTarget(w, c);

    // 目标变化 -> 从当前输出重新开始一段渐变（旧的渐变直接被接管，无跳变）
    if (w != target_w_ || c != target_c_) {
        uint32_t ms = next_fade_ms_;
        if (ms == 0) {
            ms = fadeMsForDistance(w, c);       // 普通调节：按距离自适应
        } else {
            next_fade_ms_ = 0;                  // 开关灯指定的时长只生效一次
        }
        beginTween(w, c, ms);
        target_w_ = w;
        target_c_ = c;
    }

    if (!tw_active_) return;

    const uint32_t now = millis();
    if ((now - last_tick_) < TWEEN_TICK_MS) return;
    last_tick_ = now;
    stepTween();
}

// ---------------------------------------------------------------------------
// 开关灯
// ---------------------------------------------------------------------------
void LightEngine::setPower(bool on, bool animate) {
    if (on_ == on) return;
    on_ = on;
    next_fade_ms_ = animate ? (on ? FADE_MS_ON : FADE_MS_OFF) : 1;
    state_dirty_  = true;
}

void LightEngine::setBrightness(float b) {
    b = clampf(b, BRIGHT_MIN, 1.0f);
    if (fabsf(b - bright_) < 0.0005f) return;
    bright_       = b;
    next_fade_ms_ = 0;
    state_dirty_  = true;
}

void LightEngine::setCct(float t) {
    t = clampf(t, 0.0f, 1.0f);
    if (fabsf(t - cct_) < 0.0005f) return;
    cct_          = t;
    next_fade_ms_ = 0;
    state_dirty_  = true;
}

// ---------------------------------------------------------------------------
// 旋钮调亮度：保证每一格都至少让占空比变化 1 个量化级
// （10bit + gamma2.2 下，暗端 1% 的亮度步进可能不足一级，必须自动放大）
// ---------------------------------------------------------------------------
void LightEngine::nudgeBrightness(int steps) {
    if (steps == 0) return;

    const float dir = (steps > 0) ? 1.0f : -1.0f;
    int n = (steps > 0) ? steps : -steps;
    if (n > 20) n = 20;                 // 单次最多合并 20 格，避免跳跃过大

    for (int i = 0; i < n; ++i) {
        const uint32_t before = totalDutyOf(bright_);
        bool moved = false;

        for (int k = 0; k < MAX_STEP_TRIES; ++k) {
            float nb = bright_ + dir * STEP_BRIGHT;
            if (nb >= 1.0f) {
                bright_ = 1.0f;
                moved   = (before != totalDutyOf(bright_));
                break;
            }
            if (nb <= BRIGHT_MIN) {
                bright_ = BRIGHT_MIN;
                moved   = (before != totalDutyOf(bright_));
                break;
            }
            bright_ = nb;
            if (totalDutyOf(bright_) != before) {
                moved = true;
                break;
            }
        }
        if (!moved) break;              // 已经到顶或到底
    }

    next_fade_ms_ = 0;
    state_dirty_  = true;
}

// ---------------------------------------------------------------------------
// 旋钮调色温：直接以 2% 为步进（色温不参与占空比量化，无需自适应）
// ---------------------------------------------------------------------------
void LightEngine::nudgeCct(int steps) {
    if (steps == 0) return;

    float t = cct_ + (float)steps * STEP_CCT;
    t = clampf(t, 0.0f, 1.0f);
    if (fabsf(t - cct_) < 0.0005f) return;

    cct_          = t;
    next_fade_ms_ = 0;
    state_dirty_  = true;
}

bool LightEngine::takeDirty() {
    bool d = state_dirty_;
    state_dirty_ = false;
    return d;
}
