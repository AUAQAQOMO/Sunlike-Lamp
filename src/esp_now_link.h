#pragma once
#include <stdint.h>

// ============================================================================
// esp_now_link —— 台灯端的 ESP-NOW 从机
//
// 职责：
//   * 响应无线旋钮控制器的配对请求（未配对时；已配对则忽略其他控制器）
//   * 接收增量命令并驱动 LightEngine
//   * 回应 PING，把自己的 WiFi 信道告知控制器（路由器换信道后控制器据此重扫）
//
// 与巴法云 MQTT 并存：ESP-NOW 与 STA 共用射频，不需额外网络。
// ============================================================================

void espnow_link_begin();       // 在 WiFi 初始化（app_link_begin）之后调用
void espnow_link_poll();        // 主循环调用：执行回调里排队的命令
void espnow_link_resume();      // WiFi 模式被外部改动后（如 AP 配网）重建链路
bool espnow_link_paired();
void espnow_link_get_peer(uint8_t mac_out[6]);   // 未配对时填 0
void espnow_link_clear();       // 清除配对（长按恢复出厂时调用）
