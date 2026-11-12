#pragma once
#include <stdint.h>

// ============================================================================
// app_link —— 网络接入层：WiFi + 巴法云 MQTT + AP 配网 + ArduinoOTA
//
// 云端链路：米家 App / 小爱同学 -> 巴法云 -> MQTT(bemfa.com:9501) -> 本机
//   * 无云端凭证时仍会启动 WiFi 与配网（凭证正是靠配网下发的）
//   * 断网时灯照常可用，只损失云端控制
//   * 状态双向同步：本地操作后主动回传，云端指令直接驱动本地引擎
// ============================================================================

// 初始化（连接 WiFi / 连接巴法云）
void app_link_begin();

// 主循环调用（处理配网、MQTT 消息与 OTA）
void app_link_run();

// 回传状态到巴法云。force=true 忽略节流。返回 true 表示本次确实发送了
bool app_link_report(bool force = false);

// 恢复出厂并重启（长按 5s 触发）
void app_link_reset();

// 云端是否在线（已配网 + WiFi 已连 + MQTT 已连）
bool app_link_online();
