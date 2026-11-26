// ============================================================================
// app_link —— 巴法云(bemfa) MQTT 接入 + ArduinoOTA
//
// 云端链路：米家 App / 小爱同学 -> 巴法云 -> MQTT(bemfa.com:9501) -> 本机
//
// 为什么用巴法云：米家第三方平台接入（点灯科技等）自 2025-07-01 起已下线，
// 个人 DIY 设备只能通过巴法云这类“其他平台设备”通道进入米家。
// 设备类型由主题名后三位决定，002 = 灯泡，详见 config.h 的 BEMFA_TOPIC。
//
// 消息格式（灯泡 002）：
//   on / off                 开关
//   on#80                    开灯 + 亮度 80（1~100）
//   on#60#4000               第三段 2000~7000 视为色温（K）
//   on#100#16711680          第三段为十进制 RGB，本灯无彩色，忽略
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <math.h>

#include "app_link.h"
#include "config.h"
#include "light_engine.h"
#include "esp_now_link.h"
#include "net_provision.h"

extern LightEngine g_light;

namespace {

bool     s_enabled      = false;   // 网络栈是否已启动（含配网）
bool     s_cloud        = false;   // 是否具备云端凭证
bool     s_ota_started  = false;
bool     s_boot_checked = false;
bool     s_subscribed   = false;
uint32_t s_last_report  = 0;
uint32_t s_last_wifi_ms = 0;
uint32_t s_last_mqtt_ms = 0;
uint32_t s_last_sub_ms  = 0;

// MQTT 重连间隔：与 2s 的连接超时配合，把断网时对主循环的占用压到约 20%
const uint32_t MQTT_RETRY_MS = 8000;

// 上一次成功上报的值，用于去重（云端按消息条数计费，能少发就少发）
int32_t  s_last_pw  = -1;
int32_t  s_last_bri = -1;

// 云端凭证：App 配网下发的优先，其次用 config.h 里写死的
String s_uid;
String s_topic;
ProvisionData s_prov;

WiFiClient   s_net;
PubSubClient s_mqtt(s_net);

inline bool topicIsLamp() {
    const size_t n = s_topic.length();
    return n >= 3 && s_topic.substring(n - 3) == "002";
}

// 主题兜底：NVS 里的主题若为空/损坏（例如旧固件遗留数据），用它保证设备仍能工作。
// 与 net_provision 的 defaultTopic() 规则保持一致：lamp + MAC 后 6 位 + 002
String fallbackTopic() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() > 6) mac = mac.substring(mac.length() - 6);
    return String("lamp") + mac + "002";
}

// OTA 密码：config.h 填了就用填的；留空则用 MAC 后 6 位派生，保证每台设备不同，
// 避免“一个密码泄露 = 全部量产机可被局域网刷入任意固件”。
String otaPassword() {
    if (strlen(OTA_PASSWORD) >= 1) return String(OTA_PASSWORD);

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() > 6) mac = mac.substring(mac.length() - 6);
    return String("sl") + mac;
}

// ---------------------------------------------------------------------------
// 解析一条云端指令，直接驱动本地引擎
//   无法识别的消息直接忽略，失败也不影响灯（本地始终可用）
// ---------------------------------------------------------------------------
void applyCommand(const String & cmd) {
    if (cmd.length() == 0) return;

    if (cmd == "off") {
        g_light.setPower(false);
        return;
    }

    if (cmd.startsWith("on")) {
        g_light.setPower(true);

        // on#亮度#RGB：用不到的功能可以省略，所以字段数不固定
        const int p1 = cmd.indexOf('#');
        const int p2 = (p1 >= 0) ? cmd.indexOf('#', p1 + 1) : -1;

        if (p1 >= 0) {
            const int32_t bri = (p2 >= 0) ? cmd.substring(p1 + 1, p2).toInt()
                                          : cmd.substring(p1 + 1).toInt();
            if (bri >= 1 && bri <= 100) {
                g_light.setBrightness((float)bri / 100.0f);
            }
        }

        // 第三段：巴法云文档写的是“rgb 值或色温”。
        //   十进制 RGB 一般 ≥ 65536（如红色 16711680），色温是 2000~7000 的整数，
        //   这里按数值大小区分：小的当色温用，大的当 RGB 忽略（本灯只有冷暖白）。
        if (p2 >= 0) {
            const int32_t third = cmd.substring(p2 + 1).toInt();
            if (third >= 2000 && third <= 7000) {
                g_light.setCct((float)(third - CCT_WARM_K) /
                               (float)(CCT_COOL_K - CCT_WARM_K));
            }
        }
        return;
    }

    // 其他消息（例如云端的自定义主题推送）直接忽略
}

void onMqttMessage(char * topic, byte * payload, unsigned int len) {
    (void)topic;                            // 只订阅了一个主题

    String cmd;
    cmd.reserve(len + 1);
    for (unsigned int i = 0; i < len; ++i) {
        cmd += (char)payload[i];
    }
    cmd.trim();                              // 容忍云端多打的空白/换行

    Serial.printf("[CLOUD] 收到指令: %s\n", cmd.c_str());
    applyCommand(cmd);

    // 这里用非 force 上报：状态与上次相同时直接跳过，避免云端回推造成消息回环
    // （每轮两条消息，既消耗巴法云额度又可能被限流）。
    // 真正的状态变化仍会立即发出；万一被 300ms 节流丢掉，主循环的 takeDirty 会补发。
    app_link_report(false);
}

// ---------------------------------------------------------------------------
// WiFi：优先用 App 配网下发的，其次 config.h 里写死的，最后复用芯片 NVS
// ---------------------------------------------------------------------------
void startWiFi() {
    WiFi.mode(WIFI_STA);
    if (s_prov.valid()) {
        WiFi.begin(s_prov.ssid.c_str(), s_prov.pswd.c_str());
        Serial.printf("[NET] 连接 WiFi：%s（App 配网）\n", s_prov.ssid.c_str());
    } else if (strlen(LOCAL_WIFI_SSID) >= 1) {
        WiFi.begin(LOCAL_WIFI_SSID, LOCAL_WIFI_PASSWORD);
        Serial.printf("[NET] 连接 WiFi：%s\n", LOCAL_WIFI_SSID);
    } else {
        WiFi.begin();                       // 无参调用 = 使用 NVS 中已保存的凭证
        Serial.println(F("[NET] 连接 WiFi：使用芯片内已保存的凭证"));
    }
    s_last_wifi_ms = millis();
}

void mqttReconnect() {
    if (millis() - s_last_mqtt_ms < MQTT_RETRY_MS) return;
    s_last_mqtt_ms = millis();

    // ClientID 必须是私钥本身，否则会回落到账号密码鉴权而失败
    if (s_mqtt.connect(s_uid.c_str())) {
        s_subscribed  = false;              // 交给下面的订阅重试逻辑去订阅
        s_last_sub_ms = 0;
        Serial.printf("[CLOUD] 巴法云已连接，准备订阅 %s\n", s_topic.c_str());
        s_last_pw  = -1;                    // 重新上线后强制补报一次完整状态
        s_last_bri = -1;
        app_link_report(true);
    } else {
        Serial.printf("[CLOUD] 巴法云连接失败 (state=%d)，5s 后重试\n", s_mqtt.state());
    }
}

// 订阅可能因为主题还没在控制台创建而失败，所以失败 3s 重试、成功 30s 刷新一次
void subscribeTick() {
    const bool needRetry   = !s_subscribed && (millis() - s_last_sub_ms >= 3000);
    const bool needRefresh =  s_subscribed && (millis() - s_last_sub_ms >= 30000);
    if (!needRetry && !needRefresh) return;

    s_last_sub_ms = millis();
    const bool ok = s_mqtt.subscribe(s_topic.c_str());

    if (ok) {
        if (!s_subscribed) {
            s_subscribed = true;
            Serial.printf("[CLOUD] 已订阅 %s\n", s_topic.c_str());
            app_link_report(true);
        }
    } else {
        s_subscribed = false;
        Serial.printf("[CLOUD] 订阅 %s 失败（主题是否已在巴法云控制台创建？）\n",
                      s_topic.c_str());
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void app_link_begin() {
    // App 配网下发的凭证优先于 config.h 里写死的
    const bool hasProv = provision_load(s_prov);
    s_uid   = hasProv && s_prov.uid.length()   >= 8 ? s_prov.uid   : String(BEMFA_UID);
    s_topic = hasProv && s_prov.topic.length() >= 3 ? s_prov.topic : String(BEMFA_TOPIC);

    s_mqtt.setServer(BEMFA_HOST, BEMFA_PORT);
    s_mqtt.setCallback(onMqttMessage);
    s_mqtt.setKeepAlive(60);
    s_mqtt.setBufferSize(256);              // 默认 128，留点余量

    // 连接超时直接决定“断网时旋钮有多卡”：PubSubClient::connect() 是阻塞的，
    // 每重试一次主循环就停这么久。2s + 8s 重试间隔 => 最坏占用约 20% 的时间。
    s_mqtt.setSocketTimeout(2);

    // 上电后第一次连接立即发起，不用白等一个完整重试窗口
    s_last_mqtt_ms = millis() - MQTT_RETRY_MS;

    // 没有云端凭证也要把 WiFi 和配网跑起来：凭证正是通过配网下发的
    s_cloud   = s_uid.length() >= 8;
    s_enabled = true;
    startWiFi();

    // 兜底：主题为空（旧固件遗留 / NVS 损坏）会导致设备永远订阅失败。
    // 必须放在 startWiFi() 之后：WiFi 未初始化时 macAddress() 返回全 0，
    // 会生成 "lamp000000002" 这种所有设备共用的主题，反而造成串控。
    if (s_topic.length() < 3) {
        s_topic = fallbackTopic();
        Serial.printf("[CLOUD] 主题缺失，改用 MAC 兜底主题 %s\n", s_topic.c_str());
    }

    if (!s_cloud) {
        Serial.println(F("[CLOUD] 无云端凭证 -> 等待配网（30s 后自动开热点）"));
        return;
    }
    if (!topicIsLamp()) {
        Serial.println(F("[CLOUD] 主题后三位不是 002，米家不会识别成灯泡！"));
    }
    Serial.printf("[CLOUD] 巴法云主题 %s（%s:%d，凭证来自%s，私钥长度 %d）\n",
                  s_topic.c_str(), BEMFA_HOST, BEMFA_PORT,
                  hasProv ? "App 配网" : "config.h", (int)s_uid.length());
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void app_link_run() {
    if (s_enabled) {
        // 上电 30s 仍连不上 WiFi -> 自动进 AP 配网（否则设备就彻底失联了）
        if (!s_boot_checked && millis() >= AP_PORTAL_BOOT_WAIT_MS) {
            s_boot_checked = true;
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println(F("[NET] 超时未联网，进入配网模式"));
                provision_runPortal();
                // 配网期间切换到过 WIFI_AP，mDNS 已失效；重置标志让主循环重新注册 OTA
                s_ota_started = false;
                startWiFi();                // 配网超时返回：回到正常流程继续重试
                espnow_link_resume();       // AP 模式下 ESP-NOW 已失效，重建
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            if (s_cloud) {
                if (!s_mqtt.connected()) {
                    s_subscribed = false;
                    mqttReconnect();
                }
                s_mqtt.loop();
                if (s_mqtt.connected()) subscribeTick();
            }
        } else if (millis() - s_last_wifi_ms >= 30000) {
            // WiFi 掉了：先断开 MQTT（否则会拿着失效的 socket 一直重连），再让 WiFi 重连。
            // 这里刻意不用 WiFi.disconnect() + startWiFi()：那样会打断正在进行的连接尝试，
            // 弱信号环境下握手本来就要十几秒，被打断后会永远连不上。
            if (s_mqtt.connected()) s_mqtt.disconnect();
            Serial.println(F("[NET] WiFi 30s 未连上，重新发起连接"));
            WiFi.reconnect();
            s_last_wifi_ms = millis();
        }
    }

    // OTA 只要有 WiFi 就启用，与云端解耦（纯本地模式下 WiFi 未启动，自然不会进来）。
    // OTA 与 MQTT 各跑各的；LEDC 的 PWM 由硬件维持，上传期间灯也不会闪。
    if (!s_ota_started && WiFi.status() == WL_CONNECTED) {
        // 主机名加 MAC 后缀：同一局域网里有多台同型号灯时，mDNS 名字冲突会让后启动的
        // 设备抢不到 sunlike-lamp.local，售后就不知道该刷哪一台
        String otaHost = String(OTA_HOSTNAME);
        {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            if (mac.length() >= 4) {
                otaHost += "-";
                otaHost += mac.substring(mac.length() - 4);
            }
        }
        ArduinoOTA.setHostname(otaHost.c_str());

        const String otaPwd = otaPassword();
        ArduinoOTA.setPassword(otaPwd.c_str());

        ArduinoOTA.onStart([]() {
            // 注意：进入 OTA 后 ArduinoOTA.handle() 会独占 CPU，主循环的 update() 不再执行，
            // 所以只调用 setPower() 是不够的（它只改状态位），必须直接写硬件占空比。
            // 只关硬件、不改变逻辑开关状态：若 OTA 中途失败，主循环恢复后会淡入回原亮度
            g_light.forceOff();
            Serial.println(F("[OTA] 开始接收固件，LED 已立即关断"));
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
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("[OTA] 接收错误 (代码 %u)\n", error);
        });

        ArduinoOTA.begin();
        s_ota_started = true;
        Serial.printf("[OTA] 就绪：%s.local（密码 %s）\n", otaHost.c_str(), otaPwd.c_str());
    }

    if (s_ota_started) {
        ArduinoOTA.handle();
    }
}

// ---------------------------------------------------------------------------
// 状态回传
// ---------------------------------------------------------------------------
bool app_link_report(bool force) {
    if (!s_cloud) return true;              // 还没配网，视为“无需上报”
    if (!s_mqtt.connected()) return false;  // 云端还没连上，返回 false 让调用方稍后重试

    const uint32_t now = millis();
    if (!force && (now - s_last_report) < REPORT_MIN_INTERVAL) {
        return false;                       // 被节流，调用方应该稍后重试
    }

    // 与下发严格互逆：下发 v/100，上报 b*100
    int32_t bri = (int32_t)lroundf(g_light.brightness() * 100.0f);
    if (bri < 1)   bri = 1;
    if (bri > 100) bri = 100;

    const int32_t pw = g_light.power() ? 1 : 0;

    // 状态没变化就不发（force 用于响应云端查询，必须发）
    if (!force && pw == s_last_pw && bri == s_last_bri) {
        return true;
    }

    s_last_report = now;
    s_last_pw     = pw;
    s_last_bri    = bri;

    char payload[16];
    if (!pw) {
        strcpy(payload, "off");
    } else {
        snprintf(payload, sizeof(payload), "on#%d", (int)bri);
    }

    // 发到 <topic>/set：推送给订阅者（米家 App），且不会回推给自己，无回环风险
    char pub_topic[48];
    snprintf(pub_topic, sizeof(pub_topic), "%s/set", s_topic.c_str());

    if (!s_mqtt.publish(pub_topic, payload)) {
        s_last_pw  = -1;                    // 发布失败，下一轮重发
        s_last_bri = -1;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 恢复出厂
//   清掉 App 配网存下的 WiFi/私钥/主题，并删除芯片里保存的 WiFi，
//   重启后连不上会自动进 AP 配网。灯光记忆由调用方（main.cpp）清空。
// ---------------------------------------------------------------------------
void app_link_reset() {
    Serial.println(F("[CLOUD] 清除配网信息，重启后将进入配网模式"));
    if (s_mqtt.connected()) {
        s_mqtt.disconnect();
    }
    provision_clear();
    delay(200);
    ESP.restart();
}

bool app_link_online() {
    return s_cloud && WiFi.status() == WL_CONNECTED && s_mqtt.connected();
}
