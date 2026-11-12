// ============================================================================
// Sunlike 双色温台灯 —— 主程序
//
// 职责：初始化外设、跑主循环、把本地交互（旋钮/按键）与云端指令统一到
//       LightEngine，并负责掉电记忆与云端状态回传。
// ============================================================================
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

// ---------------------------------------------------------------------------
// 恢复出厂前的 3 次闪烁提示
//   在调用 app_link_reset() 之前执行；此时主循环即将中断，因此全部使用
//   直接写硬件的接口（forceOff / reapplyNow），不依赖 update() 插值。
// ---------------------------------------------------------------------------
static void indicateReset() {
    if (!g_light.power()) {
        g_light.setPower(true, false);      // 关灯状态下先点亮，否则闪了也看不见
    }
    g_light.reapplyNow();                   // 立刻亮到当前亮度
    delay(150);

    for (int i = 0; i < 3; ++i) {
        g_light.forceOff();
        delay(150);
        g_light.reapplyNow();
        delay(150);
    }
    g_light.forceOff();                     // 提示完毕后关断，等待系统重启恢复默认
}

void setup() {
    Serial.begin(115200);
    delay(50);                             // C3 走 UART0（板上 TX/RX 排针），无需等待 USB 枚举

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.printf(" Sunlike 台灯固件  ESP32-C3  v%s\n", FW_VERSION);
    Serial.printf(" 构建 %s %s\n", __DATE__, __TIME__);
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
    g_light.setPower(st.on, true);      // 直接淡入点亮（1500ms 同时覆盖芯片内部软启动）

    // 注：避免使用 %f（部分 newlib-nano 配置不输出浮点）
    Serial.printf("[BOOT] 亮度 %d/1000 / 色温 %dK / 上次状态 %s\n",
                  (int)(st.brightness * 1000.0f + 0.5f),
                  (int)(CCT_WARM_K + st.cct * (CCT_COOL_K - CCT_WARM_K) + 0.5f),
                  st.on ? "on" : "off");
    Serial.printf("[BOOT] FreeHeap=%u / MinFree=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

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
        // 灯灭时任何旋转都先点亮，否则看不到变化，容易被当成旋钮失灵。
        // 唤醒必须走 1.5s 柔和淡入（FADE_MS_ON），不要换成 setPowerAdaptive()。
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
    // 长按 5s：闪 3 下确认 -> 清空记忆 -> 清除配网 -> 重启
    // ------------------------------------------------------------------
    if (g_rotary.takeLongPress()) {
        Serial.println(F("[KEY] 长按 5s -> 恢复出厂设置并重启"));
        indicateReset();                    // 先闪 3 下给用户确认
        g_store.clear();                    // 清空 NVS 中的亮度/色温/开关记忆
        app_link_reset();                   // 清配网信息后重启（不返回）
    }

    // ------------------------------------------------------------------
    // 渐变调度：按时间插值下发新占空比（PWM 波形由 LEDC 硬件维持）
    // ------------------------------------------------------------------
    g_light.update();

    // ------------------------------------------------------------------
    // 状态变化 -> 延迟写 NVS + 回传云端
    // ------------------------------------------------------------------
    if (g_light.takeDirty()) {
        s_persist_pending = true;
        s_last_change_ms  = millis();
        s_report_pending  = true;
    }

    if (s_persist_pending) {
        // 关灯操作优先保存（500ms），避免用户关灯后立刻拔电导致未记忆；
        // 连续调光维持 2000ms 防抖，保护 Flash
        const uint32_t delay_ms = (!g_light.power()) ? 500 : PERSIST_DELAY_MS;
        if ((millis() - s_last_change_ms) >= delay_ms) {
            LampState cur = { g_light.brightness(), g_light.cct(), g_light.power() };
            g_store.save(cur);
            s_persist_pending = false;
        }
    }

    if (s_report_pending) {
        if (app_link_report(false)) {   // 返回 false 表示被节流，下轮再试
            s_report_pending = false;
        }
    }

    delay(1);
}
