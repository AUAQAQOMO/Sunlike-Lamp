#pragma once
#include <stdint.h>

// ============================================================================
// app_link —— 点灯科技(blinker) 米家灯组件 + ArduinoOTA
//
// 设计原则：本地旋钮/按键是主路径，云端只是增强。
//   * BLINKER_AUTH_KEY 为空 -> 完全不启动 WiFi，纯本地运行
//   * 断网时灯照常可用，只损失米家/小爱控制
//   * 状态双向同步：本地操作后主动回传，米家指令直接驱动本地引擎
// ============================================================================

// 初始化（连接 WiFi / 注册米家灯组件）
void app_link_begin();

// 主循环调用（处理 blinker 消息与 OTA）
void app_link_run();

// 回传状态到米家。force=true 忽略节流。返回 true 表示本次确实发送了
bool app_link_report(bool force = false);

// 清除配网信息并重启（长按 5s 触发）
void app_link_reset();

// 是否已启用云端（填了 AuthKey 且 Blinker 连接正常）
bool app_link_online();
