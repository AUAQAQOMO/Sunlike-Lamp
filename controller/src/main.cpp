// ============================================================================
// Sunlike 无线旋钮控制器 —— 主程序
//
// 职责：采集 EC11 → 合并成增量 → 通过 ESP-NOW 发给台灯；
//       处理配对、断链重扫、无操作休眠。
//
// 与台灯本体完全相同的交互：
//   旋转        调亮度
//   按住 + 旋转 调色温
//   单击        开/关
//   长按 5s     清除配对并重启（灯闪不了，只能靠台灯不再响应来判断）
// ============================================================================
#include <Arduino.h>

#include "config.h"
#include "espnow_proto.h"       // 命令字 ESPNOW_CMD_xxx
#include "rotary.h"
#include "espnow_link.h"
#include "power_mgmt.h"

static Rotary g_rotary;

// 待发送的增量（多格会合并成一次发送，避免手快时刷爆 ESP-NOW）
static int      s_accBright  = 0;
static int      s_accCct     = 0;
static uint32_t s_lastSendMs = 0;

static uint32_t s_lastActiveMs = 0;

static int8_t clampDelta(int d) {
    if (d >  ESPNOW_DELTA_LIMIT) return  (int8_t) ESPNOW_DELTA_LIMIT;
    if (d < -ESPNOW_DELTA_LIMIT) return -(int8_t) ESPNOW_DELTA_LIMIT;
    return (int8_t)d;
}

// 把累计的增量发出去（受最小间隔限制，剩余部分留到下一轮）
static void flushPending() {
    if (!espnow_paired()) {
        s_accBright = 0;
        s_accCct    = 0;
        return;
    }
    if (s_accBright == 0 && s_accCct == 0) {
        return;
    }

    const uint32_t now = millis();
    if (now - s_lastSendMs < ESPNOW_CMD_MIN_INTERVAL_MS) {
        return;
    }
    s_lastSendMs = now;

    if (s_accBright != 0) {
        espnow_send_command(ESPNOW_CMD_NUDGE_BRIGHT, clampDelta(s_accBright));
        s_accBright = 0;
    }
    if (s_accCct != 0) {
        espnow_send_command(ESPNOW_CMD_NUDGE_CCT, clampDelta(s_accCct));
        s_accCct = 0;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.printf(" Sunlike 无线旋钮  v%s\n", FW_VERSION);
    Serial.printf(" 构建 %s %s\n", __DATE__, __TIME__);
    Serial.println(F(" PWM: 无（控制器不驱动 LED）"));
    Serial.println(F("=============================================="));

    power_init();                       // 必须最先做：配置唤醒源

    g_rotary.begin();
    espnow_init();

    s_lastActiveMs = millis();

    Serial.printf("[BOOT] %s / 信道 %d / 唤醒来源 %s / FreeHeap=%u\n",
                  espnow_paired() ? "已配对" : "未配对（扫描中）",
                  (int)espnow_channel(),
                  power_woke_from_sleep() ? "睡眠" : "上电",
                  (unsigned)ESP.getFreeHeap());
}

void loop() {
    espnow_poll();
    g_rotary.poll();

    // ---- 旋转：默认调亮度，按住旋转调色温 ----
    const int steps = g_rotary.takeSteps();
    if (steps != 0) {
        s_lastActiveMs = millis();
        if (g_rotary.keyHeld()) {
            s_accCct += steps;
        } else {
            s_accBright += steps;
        }
    }

    // ---- 单击：开/关 ----
    if (g_rotary.takeClick()) {
        s_lastActiveMs = millis();
        if (espnow_paired()) {
            espnow_send_command(ESPNOW_CMD_TOGGLE, 0);
        }
    }

    // ---- 长按 5s：清除配对并重启（重启后重新扫描）----
    if (g_rotary.takeLongPress()) {
        Serial.println(F("[KEY] 长按 5s -> 清除配对并重启"));
        espnow_clear_pairing();
        delay(300);
        ESP.restart();
    }

    flushPending();

    // ---- 无操作休眠 ----
    static uint32_t lastCheckMs = 0;
    const uint32_t now = millis();
    if (now - lastCheckMs >= SLEEP_CHECK_MS) {
        lastCheckMs = now;
        if (now - s_lastActiveMs >= SLEEP_IDLE_MS) {
            flushPending();             // 睡前把命令发完
            power_sleep();              // 阻塞直到被旋钮/按键唤醒

            espnow_resume();            // 重建 ESP-NOW（light sleep 会暂停 WiFi）
            s_lastActiveMs = millis();
            Serial.println(F("[PWR] 已唤醒"));
        }
    }

    delay(1);
}
