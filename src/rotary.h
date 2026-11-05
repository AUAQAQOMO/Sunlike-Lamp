#pragma once
#include <stdint.h>

// ============================================================================
// Rotary —— EC11 旋钮（A/B 正交 + 按压按键）
//
// 采用“主循环轮询解码”而不是 GPIO 中断：
//   * ESP32-C3 是单核，WiFi/MQTT 会抢占中断，ISR 里调 digitalRead 有风险
//   * 人手最快约 20~60 格/秒，主循环 1ms 轮询（1kHz）远超所需
//   * 正交 4 状态机天然抗抖动，掉一个边沿也能在环回时自动纠正方向
//
// 交互映射：
//   旋转           -> 调亮度
//   按住 + 旋转    -> 调色温
//   单击（<600ms） -> 开/关灯
//   长按 5s        -> 闪烁提示 + 重置配网
// ============================================================================
class Rotary {
public:
    void begin();
    void poll();                    // 主循环每次调用

    int  takeSteps();               // 取走累计的机械格数（正=顺时针）
    bool takeClick();
    bool takeLongPress();

    bool keyHeld() const { return pressed_; }

private:
    // ---- 正交解码 ----
    uint8_t enc_prev_ = 0;          // 上一状态 (A<<1)|B
    int32_t accum_    = 0;          // 累计边沿

    // ---- 按键 ----
    bool     pressed_             = false;
    bool     raw_prev_            = true;   // HIGH = 松开
    uint32_t raw_change_ms_       = 0;
    uint32_t press_ms_            = 0;
    bool     rotated_since_press_ = false;  // 按下期间旋转过 -> 不判单击/长按
    bool     long_fired_          = false;
    bool     click_pending_       = false;
    bool     long_pending_        = false;
};
