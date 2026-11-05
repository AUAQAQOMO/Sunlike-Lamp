// ============================================================================
// 注意：blinker 的宏必须在 #include <Blinker.h> 之前定义，
//       且整个工程只能在这一个 .cpp 里 include Blinker.h（它定义了全局对象）。
// ============================================================================
#include "app_link.h"

#define BLINKER_WIFI
#define BLINKER_MIOT_LIGHT
// AP 配网：首次上电（或长按 5s 重置后）设备开热点，手机连上后用点灯 App 配置 WiFi。
// 注意：只有定义了 BLINKER_APCONFIG / BLINKER_ESP_SMARTCONFIG，blinker 才提供
// begin(auth) 这个单参数重载，否则必须写成 begin(auth, ssid, pswd)。
#define BLINKER_APCONFIG

#include <Arduino.h>
#include <WiFi.h>
#include <Blinker.h>
#include <ArduinoOTA.h>
#include <math.h>

#include "config.h"
#include "light_engine.h"

extern LightEngine g_light;

namespace {

bool     s_enabled     = false;
bool     s_ota_started = false;
uint32_t s_last_report = 0;

// 上一次成功上报的值，用于去重（米家免费额度按消息条数计）
int32_t  s_last_pw  = -1;
int32_t  s_last_bri = -1;
int32_t  s_last_k   = -1;

inline int32_t kelvinFromCct(float t) {
    return (int32_t)lroundf((float)CCT_WARM_K + t * (float)(CCT_COOL_K - CCT_WARM_K));
}

inline float cctFromKelvin(int32_t k) {
    float t = (float)(k - CCT_WARM_K) / (float)(CCT_COOL_K - CCT_WARM_K);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// ---------------------------------------------------------------------------
// 米家回调：把云端指令直接映射到本地引擎，然后立刻回传状态
// ---------------------------------------------------------------------------
void onMiotPower(const String & state) {
    if (state == BLINKER_CMD_ON) {
        g_light.setPower(true);
    } else if (state == BLINKER_CMD_OFF) {
        g_light.setPower(false);
    }
    app_link_report(true);
}

void onMiotBrightness(const String & bright) {
    int32_t v = bright.toInt();
    if (v < 1)   v = 1;
    if (v > 100) v = 100;

    g_light.setBrightness((float)v / 100.0f);
    // 米家上拖亮度滑块通常隐含“开灯”，这里跟随该习惯
    if (!g_light.power()) {
        g_light.setPower(true);
    }
    app_link_report(true);
}

// 注意：blinker 里色温回调是 int32 参数（开尔文值），不是 String
void onMiotColorTemperature(int32_t clrTemp) {
    g_light.setCct(cctFromKelvin(clrTemp));
    if (!g_light.power()) {
        g_light.setPower(true);
    }
    app_link_report(true);
}

void onMiotQuery(int32_t queryCode) {
    // 无论查询哪一项，都统一回传完整状态，避免米家 App 显示不同步
    (void)queryCode;
    app_link_report(true);
}

}  // namespace

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void app_link_begin() {
    // 1) 有 AuthKey -> 走点灯科技（米家/小爱 + OTA）
    if (strlen(BLINKER_AUTH_KEY) >= 8) {
        Blinker.begin(BLINKER_AUTH_KEY);

        BlinkerMIOT.attachPowerState(onMiotPower);
        BlinkerMIOT.attachBrightness(onMiotBrightness);
        BlinkerMIOT.attachColorTemperature(onMiotColorTemperature);
        BlinkerMIOT.attachQuery(onMiotQuery);

        s_enabled = true;
        Serial.println(F("[APP] 点灯科技初始化完成；首次使用请用点灯 App 给设备配网"));
        return;
    }

    // 2) 没有 AuthKey 但填了固定 WiFi -> 只为了 OTA 联网，不接米家
    if (strlen(LOCAL_WIFI_SSID) >= 1) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(LOCAL_WIFI_SSID, LOCAL_WIFI_PASSWORD);
        Serial.printf("[APP] 本地 WiFi 模式（仅 OTA）：%s\n", LOCAL_WIFI_SSID);
        return;
    }

    // 3) 都不填 -> 纯本地，完全不启动 WiFi
    Serial.println(F("[APP] 未配置 WiFi -> 纯本地模式（旋钮/记忆照常工作，无 OTA）"));
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void app_link_run() {
    if (s_enabled) {
        Blinker.run();

        // 重新上线时补报一次完整状态，避免米家 App 一直显示断线前的旧值
        static bool was_online = false;
        const bool online = Blinker.connected();
        if (online && !was_online) {
            app_link_report(true);
        }
        was_online = online;
    }

    // OTA 只要有 WiFi 就启用，与点灯云解耦（纯本地模式下 WiFi 未启动，自然不会进来）。
    // OTA 与 blinker 各跑各的；LEDC 的 PWM 由硬件维持，上传期间灯也不会闪。
    if (!s_ota_started && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname(OTA_HOSTNAME);
        ArduinoOTA.setPassword(OTA_PASSWORD);

        ArduinoOTA.onStart([]() {
            g_light.setPower(false, false);     // 升级期间关断 LED（1ms 内完成），避免功率干扰
            Serial.println(F("[OTA] 开始接收固件，LED 已关断"));
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            static uint8_t last_pct = 255;
            uint8_t pct = (uint8_t)(progress * 100 / total);
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                Serial.printf("[OTA] %u%%\n", pct);
            }
        });
        ArduinoOTA.onEnd([]() {
            Serial.println(F("[OTA] 接收完成，设备即将重启"));
        });

        ArduinoOTA.begin();
        s_ota_started = true;
        Serial.printf("[OTA] 就绪：%s.local（密码 %s）\n", OTA_HOSTNAME, OTA_PASSWORD);
    }

    if (s_ota_started) {
        ArduinoOTA.handle();
    }
}

// ---------------------------------------------------------------------------
// 状态回传
// ---------------------------------------------------------------------------
bool app_link_report(bool force) {
    if (!s_enabled) return true;            // 纯本地模式下视为“无需上报”

    const uint32_t now = millis();
    if (!force && (now - s_last_report) < REPORT_MIN_INTERVAL) {
        return false;                       // 被节流，调用方应该稍后重试
    }
    s_last_report = now;

    int32_t bri = (int32_t)lroundf(1.0f + g_light.brightness() * 99.0f);   // 1 ~ 100
    if (bri < 1)   bri = 1;
    if (bri > 100) bri = 100;

    const int32_t pw = g_light.power() ? 1 : 0;
    const int32_t k  = kelvinFromCct(g_light.cct());

    // 状态没变化就不发（force 用于响应米家查询，必须发）
    if (!force && pw == s_last_pw && bri == s_last_bri && k == s_last_k) {
        return true;
    }

    s_last_pw  = pw;
    s_last_bri = bri;
    s_last_k   = k;

    BlinkerMIOT.powerState(pw ? BLINKER_CMD_ON : BLINKER_CMD_OFF);
    BlinkerMIOT.brightness(bri);
    BlinkerMIOT.colorTemp(k);                   // 库里的方法名是 colorTemp
    BlinkerMIOT.print();

    return true;
}

// ---------------------------------------------------------------------------
// 重置配网
// ---------------------------------------------------------------------------
void app_link_reset() {
    if (!s_enabled) {
        Serial.println(F("[APP] 未启用点灯科技，没有云端配网信息可重置"));
        return;
    }
    Serial.println(F("[APP] 清除配网信息，重启后请用点灯 App 重新配网"));
    Blinker.reset();                        // 内部会擦除 WiFi 并重启，不返回
}

bool app_link_online() {
    return s_enabled && Blinker.connected();
}
