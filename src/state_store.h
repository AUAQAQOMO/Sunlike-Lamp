#pragma once
#include <stdint.h>

// 需要掉电记忆的台灯状态
struct LampState {
    float brightness;   // 0.0 ~ 1.0
    float cct;          // 0.0(3000K) ~ 1.0(5700K)
    bool  on;           // 上次是否处于点亮状态
};

// ============================================================================
// StateStore —— 基于 NVS(Preferences) 的掉电记忆
//   * 亮度/色温以 0~1000 的千分比存储，精度 0.1%
//   * 只在数值真正变化时写 Flash（主循环还做了 2s 防抖）
// ============================================================================
class StateStore {
public:
    void begin();
    LampState load();
    void save(const LampState &st);

private:
    LampState cache_     = { 0.5f, 0.4f, true };
    bool      has_cache_ = false;
};
