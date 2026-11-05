#include <Arduino.h>

#include "config.h"
#include "light_engine.h"
#include "rotary.h"
#include "state_store.h"
#include "app_link.h"

// app_link.cpp 里通过 extern 引用这个对象
LightEngine g_light;

static Rotary     g_rotary;
static StateStore g_store;

static bool     s_persist_pending = false;   // 待写入 NVS
static uint32_t s_last_change_ms  = 0;
static bool     s_report_pending  = false;   // 待回传云端（被节流时为 true）

void setup() {
    Serial.begin(115200);
    delay(50);

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.println(F(" Sunlike 双色温台灯  ESP32-C3 + 2xLGS63042EP"));
    Serial.println(F(" PWM: XTAL 40MHz / 20kHz / 10bit (1024 级)"));
    Serial.println(F("=============================================="));

    // 1) LEDC 初始化（内部会先把两个 EN 拉低，确保上电不闪）
    if (!g_light.begin()) {
        Serial.println(F("[FATAL] LEDC 初始化失败：检查 PWM 频率与分辨率组合"));
    }

    // 2) 恢复掉电记忆
    g_store.begin();
    const LampState st = g_store.load();

    g_light.setBrightness(st.brightness);
    g_light.setCct(st.cct);
    g_light.setPower(st.on, true);      // 直接淡入点亮（400ms 同时覆盖芯片内部软启动）

    // 注：避免使用 %f（部分 newlib-nano 配置不输出浮点）
    Serial.printf("[BOOT] 亮度 %d/1000 / 色温 %dK / 上次状态 %s\n",
                  (int)(st.brightness * 1000.0f + 0.5f),
                  (int)(CCT_WARM_K + st.cct * (CCT_COOL_K - CCT_WARM_K) + 0.5f),
                  st.on ? "on" : "off");

    // 3) 旋钮与云端
    g_rotary.begin();
    app_link_begin();
}

void loop() {
    // 云端消息 + OTA
    app_link_run();

    // 旋钮采样（主循环约 1kHz，足够覆盖手转 EC11 的全部边沿）
    g_rotary.poll();

    // ------------------------------------------------------------------
    // 旋转：默认调亮度；按住旋转调色温
    // ------------------------------------------------------------------
    const int steps = g_rotary.takeSteps();
    if (steps != 0) {
        // 参考 desktop_smart_lamp 的做法：灯灭时任何旋转都先点亮。
        // 否则灯灭状态下转旋钮看不到任何变化，很容易被当成旋钮失灵。
        if (!g_light.power()) {
            g_light.setPower(true);
        }
        if (g_rotary.keyHeld()) {
            g_light.nudgeCct(steps);
        } else {
            g_light.nudgeBrightness(steps);
        }
    }

    // ------------------------------------------------------------------
    // 单击：开 / 关
    // ------------------------------------------------------------------
    if (g_rotary.takeClick()) {
        g_light.setPower(!g_light.power());
        s_report_pending = true;
    }

    // ------------------------------------------------------------------
    // 长按 5s：清除配网并重启（重启后灯会熄灭再重新淡入，即为操作反馈）
    // ------------------------------------------------------------------
    if (g_rotary.takeLongPress()) {
        Serial.println(F("[KEY] 长按 5s -> 清除配网信息并重启"));
        app_link_reset();
    }

    // ------------------------------------------------------------------
    // 渐变调度：推进 LEDC 硬件 fade + 下发新目标
    // ------------------------------------------------------------------
    g_light.update();

    // ------------------------------------------------------------------
    // 状态变化 -> 延迟写 NVS + 回传米家
    // ------------------------------------------------------------------
    if (g_light.takeDirty()) {
        s_persist_pending = true;
        s_last_change_ms  = millis();
        s_report_pending  = true;
    }

    if (s_persist_pending && (millis() - s_last_change_ms) >= PERSIST_DELAY_MS) {
        LampState cur = { g_light.brightness(), g_light.cct(), g_light.power() };
        g_store.save(cur);
        s_persist_pending = false;
    }

    if (s_report_pending) {
        if (app_link_report(false)) {   // 返回 false 表示被节流，下轮再试
            s_report_pending = false;
        }
    }

    delay(1);
}
