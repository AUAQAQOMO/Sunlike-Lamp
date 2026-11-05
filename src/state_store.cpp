#include "state_store.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

namespace {

Preferences s_prefs;

// 首次上电的默认值：亮度 50%，色温 40%（约 4080K），点亮
constexpr uint16_t kDefaultBri = 500;
constexpr uint16_t kDefaultCct = 400;

inline uint16_t toPermille(float v) {
    return (uint16_t)lroundf(constrain(v, 0.0f, 1.0f) * 1000.0f);
}

}  // namespace

void StateStore::begin() {
    // 命名空间 "lamp"，读写模式（false = 可写）
    s_prefs.begin("lamp", false);
}

LampState StateStore::load() {
    LampState st;

    const uint16_t b = s_prefs.getUShort("bri", kDefaultBri);
    const uint16_t c = s_prefs.getUShort("cct", kDefaultCct);

    // 亮度下限与引擎保持一致，避免存进去的极值在启动时被判为非法
    st.brightness = constrain(b / 1000.0f, BRIGHT_MIN, 1.0f);
    st.cct        = constrain(c / 1000.0f, 0.0f, 1.0f);
    st.on         = s_prefs.getBool("on", true);

    cache_     = st;
    has_cache_ = true;
    return st;
}

void StateStore::save(const LampState &st) {
    const uint16_t b = toPermille(st.brightness);
    const uint16_t c = toPermille(st.cct);

    // 只写真正变化的键，减少 Flash 擦写次数
    if (!has_cache_ || b != toPermille(cache_.brightness)) {
        s_prefs.putUShort("bri", b);
    }
    if (!has_cache_ || c != toPermille(cache_.cct)) {
        s_prefs.putUShort("cct", c);
    }
    if (!has_cache_ || st.on != cache_.on) {
        s_prefs.putBool("on", st.on);
    }

    cache_     = st;
    has_cache_ = true;
}
