#include "rotary.h"
#include "config.h"

namespace {

// 标准正交解码表：索引 = (上一状态 << 2) | 当前状态，状态 = (A << 1) | B
// 返回值 +1/-1 表示方向，0 表示无变化或非法跳变（抖动/丢边沿）
const int8_t kEncTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

inline uint8_t readEncState() {
    return (uint8_t)((digitalRead(PIN_EC11_A) << 1) | digitalRead(PIN_EC11_B));
}

}  // namespace

void Rotary::begin() {
    // EC11 为无源开关，启用内部上拉（板上另有 R13/R14 等外部上拉与 RC 滤波，二者并联无妨）
    pinMode(PIN_EC11_A, INPUT_PULLUP);
    pinMode(PIN_EC11_B, INPUT_PULLUP);
    pinMode(PIN_EC11_KEY, INPUT_PULLUP);

    enc_prev_ = readEncState();
    accum_    = 0;

    raw_prev_ = (digitalRead(PIN_EC11_KEY) == HIGH);
    pressed_  = !raw_prev_;
    press_ms_ = millis();
}

void Rotary::poll() {
    const uint32_t now = millis();

    // ------------------------------------------------------------------
    // 1. 正交解码
    // ------------------------------------------------------------------
    const uint8_t cur = readEncState();
    if (cur != enc_prev_) {
        accum_  += kEncTable[(enc_prev_ << 2) | cur];
        enc_prev_ = cur;
    }

    // 关键时序修正：一旦按住期间转满一格，必须“立刻”标记。
    // 若等到 takeSteps() 里再标记，可能出现这种情况：
    //   上一帧累计 3 个边沿（不足一格）→ 本帧先判松手 => 误判成单击（开/关灯），
    //   然后 takeSteps() 才发现转了 1 格。表现为“调完色温手一松，灯被关掉”。
    if (pressed_ && (accum_ / ENC_TICKS_PER_DETENT) != 0) {
        rotated_since_press_ = true;
    }

    // ------------------------------------------------------------------
    // 2. 按键消抖（用“稳定时间”而不是简单的延时采样）
    // ------------------------------------------------------------------
    const bool raw = (digitalRead(PIN_EC11_KEY) == HIGH);   // HIGH = 松开
    if (raw != raw_prev_) {
        raw_change_ms_ = now;
        raw_prev_      = raw;
    }

    const bool stable_pressed = !raw;
    if (stable_pressed != pressed_ && (now - raw_change_ms_) >= KEY_DEBOUNCE_MS) {
        pressed_ = stable_pressed;
        if (pressed_) {
            press_ms_            = now;
            rotated_since_press_ = false;
            long_fired_          = false;
        } else {
            const uint32_t held = now - press_ms_;
            // 只有“短按且按下期间没有旋转”才算单击，避免调色温后误触发开关灯
            if (!long_fired_ && !rotated_since_press_ && held < CLICK_MAX_MS) {
                click_pending_ = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // 3. 长按判定（按住满 5s 立即触发，不必等松开）
    //    若期间旋转过（在调色温），则取消长按
    // ------------------------------------------------------------------
    if (pressed_ && !long_fired_ && !rotated_since_press_ &&
        (now - press_ms_) >= LONGPRESS_MS) {
        long_fired_   = true;
        long_pending_ = true;
    }
}

int Rotary::takeSteps() {
    const int32_t detents = accum_ / ENC_TICKS_PER_DETENT;
    if (detents == 0) return 0;

    accum_ -= detents * ENC_TICKS_PER_DETENT;   // 保留不足一格的部分

    if (pressed_) {
        rotated_since_press_ = true;            // 按住旋转 -> 不算单击/长按
    }
    return (int)(detents * ENC_DIR);
}

bool Rotary::takeClick() {
    if (!click_pending_) return false;
    click_pending_ = false;
    return true;
}

bool Rotary::takeLongPress() {
    if (!long_pending_) return false;
    long_pending_ = false;
    return true;
}
