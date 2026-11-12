#include "net_provision.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include "config.h"
#include "light_engine.h"

extern LightEngine g_light;

namespace {

const char * NVS_NS    = "prov";
const uint16_t UDP_PORT = 8266;     // 官方协议固定端口
const IPAddress AP_IP(192, 168, 4, 1);

Preferences s_prefs;
WiFiUDP     s_udp;

// AP 热点名：bemfa_ + MAC 后 6 位（去掉冒号）
String apSsid() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() > 6) mac = mac.substring(mac.length() - 6);
    return String(AP_SSID_PREFIX) + mac;
}

// 从 JSON 里取字符串字段，缺省返回空
String jsonStr(JsonObjectConst obj, const char * key) {
    const char * v = obj[key] | "";
    return String(v);
}

// App 没下发主题时用的兜底主题：lamp + MAC 后 6 位 + 002
// 后三位必须是 002，巴法云才会把它当成“灯泡”同步到米家。
// 用 6 位（24bit）而不是 4 位：4 位时 1000 台设备出现主题碰撞的期望对数约 7.6 对，
// 一旦撞车两台灯会互相串控；6 位时降到 0.03 对。
String defaultTopic() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() > 6) mac = mac.substring(mac.length() - 6);
    return String("lamp") + mac + "002";
}

// 返回是否真正写进了 NVS：写失败却提示“配网成功”会导致用户反复配网
bool saveAll(const ProvisionData & d) {
    if (!s_prefs.begin(NVS_NS, false)) {
        return false;
    }

    // ssid 与 topic 必须写入成功；pswd 允许为空（开放网络），uid 允许为空（留给下次配网）
    bool ok = s_prefs.putString("ssid", d.ssid) > 0;
    s_prefs.putString("pswd", d.pswd);
    s_prefs.putString("uid", d.uid);
    ok = (s_prefs.putString("topic", d.topic) > 0) && ok;

    s_prefs.end();
    return ok;
}

void reply(int cmdType, const String & topic) {
    StaticJsonDocument<192> doc;
    doc["cmdType"] = cmdType;
    if (cmdType == 2) {
        if (topic.length()) doc["productId"] = topic;
        doc["protoVersion"] = "1.0";     // 1.0 = MQTT 云，且由 App 创建主题
    }

    char buf[192];
    const size_t n = serializeJson(doc, buf, sizeof(buf));

    s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
    s_udp.write((const uint8_t *)buf, n);
    s_udp.endPacket();
}

}  // namespace

bool provision_load(ProvisionData & out) {
    s_prefs.begin(NVS_NS, true);
    out.ssid  = s_prefs.getString("ssid",  "");
    out.pswd  = s_prefs.getString("pswd",  "");
    out.uid   = s_prefs.getString("uid",   "");
    out.topic = s_prefs.getString("topic", "");
    s_prefs.end();
    return out.valid();
}

void provision_clear() {
    s_prefs.begin(NVS_NS, false);
    s_prefs.clear();
    s_prefs.end();

    // 连芯片里保存的 WiFi 一起删掉，否则下次上电还会拿旧账号重试
    WiFi.disconnect(true, true);
}

void provision_runPortal() {
    // 进配网前把灯固定到当前目标亮度：配网是阻塞的，主循环不再插值
    g_light.reapplyNow();

    ProvisionData saved;                     // 开热点前先读出已有值做兜底
    provision_load(saved);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    const String ssid = apSsid();
    WiFi.softAP(ssid.c_str());               // 无密码，和巴法官方示例一致

    s_udp.begin(UDP_PORT);

    Serial.println();
    Serial.println(F("=============== AP 配网模式 ==============="));
    Serial.printf("[AP] 热点：%s（无密码）\n", ssid.c_str());
    Serial.println(F("[AP] 手机连上后，用巴法 App 或“一键配网”小程序，协议切到 AP 配网"));
    Serial.printf("[AP] 最多等待 %lu 秒\n", (unsigned long)(AP_PORTAL_TIMEOUT_MS / 1000));
    Serial.println(F("==========================================="));

    const uint32_t start = millis();
    ProvisionData pending;
    bool     gotWifi  = false;
    uint32_t wifiTime = 0;

    // 配网期间慢闪用的状态
    uint32_t blink_ms = 0;
    bool     blink_on = true;

    while (millis() - start < AP_PORTAL_TIMEOUT_MS) {
        // 配网期间让灯慢闪（1s 周期），否则用户完全看不出设备处在配网状态
        if (millis() - blink_ms >= 500) {
            blink_ms = millis();
            blink_on = !blink_on;
            if (blink_on) {
                g_light.reapplyNow();
            } else {
                g_light.forceOff();
            }
        }

        // 已收到 WiFi 信息但没等到 App 的第三步：给 15s 宽限后直接保存
        if (gotWifi && millis() - wifiTime >= 15000) {
            if (saveAll(pending)) {
                Serial.println(F("[AP] 未收到确认指令，直接保存并重启"));
                Serial.flush();
                delay(300);
                ESP.restart();
            }
            Serial.println(F("[AP] 保存失败（NVS 写入异常），继续等待重新下发"));
            gotWifi = false;
        }

        const int len = s_udp.parsePacket();
        if (len <= 0) {
            delay(10);
            continue;
        }

        char buf[512];
        const int n = s_udp.read(buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = 0;

        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, buf)) {
            Serial.println(F("[AP] 收到无法解析的数据，忽略"));
            continue;
        }

        const int cmdType = doc["cmdType"] | 0;

        if (cmdType == 1) {
            pending.ssid  = jsonStr(doc.as<JsonObjectConst>(), "ssid");
            pending.pswd  = jsonStr(doc.as<JsonObjectConst>(), "password");
            pending.uid   = jsonStr(doc.as<JsonObjectConst>(), "token");
            pending.topic = jsonStr(doc.as<JsonObjectConst>(), "topic");

            // App 只带了 WiFi 没带主题时：先沿用上次保存的，再不行用默认的 002 主题
            if (pending.topic.length() < 3) {
                pending.topic = saved.topic.length() >= 3 ? saved.topic : defaultTopic();
            }

            Serial.printf("[AP] 收到 WiFi：%s\n", pending.ssid.c_str());
            Serial.printf("[AP] 私钥长度：%d，主题：%s\n",
                          pending.uid.length(), pending.topic.c_str());
            if (pending.topic.substring(pending.topic.length() - 3) != "002") {
                Serial.println(F("[AP] 警告：主题后三位不是 002，米家不会识别成灯泡"));
            }

            reply(2, pending.topic);         // 第二步：回设备信息
            gotWifi  = true;
            wifiTime = millis();
        } else if (cmdType == 3 && gotWifi) {
            // 第三步：手机确认，保存并重启
            if (saveAll(pending)) {
                Serial.println(F("[AP] 配网完成，保存并重启"));
                Serial.flush();
                delay(300);
                ESP.restart();
            }
            Serial.println(F("[AP] 保存失败（NVS 写入异常），请重新配网"));
            gotWifi = false;
        }
    }

    // 超时：如果期间收到过 WiFi 信息，也照样保存（App 有时不发第三步）
    if (gotWifi && pending.ssid.length()) {
        if (saveAll(pending)) {
            Serial.println(F("[AP] 超时但已收到 WiFi 信息，保存并重启"));
            Serial.flush();
            delay(300);
            ESP.restart();
        }
        Serial.println(F("[AP] 保存失败（NVS 写入异常）"));
    }

    Serial.println(F("[AP] 配网超时，回到正常模式"));
    s_udp.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}
