#pragma once
#include <Arduino.h>

// ============================================================================
// net_provision —— 巴法 App / 一键配网小程序的 AP 配网
//
// 协议（官方 AP 配网文档）：
//   设备开 SoftAP，IP 192.168.4.1，UDP 监听 8266
//   手机 → 设备：{"cmdType":1,"ssid":"..","password":"..","token":"私钥","topic":".."}
//   设备 → 手机：{"cmdType":2,"productId":"<主题>","protoVersion":"1.0"}
//   手机 → 设备：{"cmdType":3}
//
// 选第一版协议（protoVersion 1.0）是为了让 App 端创建主题，
// 设备就不用再调 HTTP 接口，少一层失败点。
// ============================================================================

struct ProvisionData {
    String ssid;
    String pswd;
    String uid;      // 巴法云私钥（App 账号的私钥，配网时下发）
    String topic;    // 主题名，后三位 002 = 灯泡

    bool valid() const { return ssid.length() > 0; }
};

// 从 NVS 读取配网结果
bool provision_load(ProvisionData &out);

// 清除 WiFi 与云端凭证（恢复出厂用）
void provision_clear();

// 阻塞式进入 AP 配网：超时或配网成功后重启，不返回失败以外的路径
void provision_runPortal();
