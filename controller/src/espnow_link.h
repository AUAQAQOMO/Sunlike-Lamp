#pragma once
#include <stdint.h>

// ============================================================================
// espnow_link —— 控制器侧的 ESP-NOW 链路
//
// 状态机：
//   SCANNING：在 1~13 信道轮询广播配对请求（每信道驻留 800ms）
//   PAIRED  ：固定到台灯上报的信道，单播发送增量命令 + 定期 PING
//
// 断链自愈：
//   PING 连续 2 次无应答 -> 回到 SCANNING，但保留已配对的 MAC，
//   因此只会认回原来那台台灯，不会误配到邻居家的灯。
// ============================================================================

void    espnow_init();
void    espnow_poll();                                  // 主循环调用
bool    espnow_paired();
bool    espnow_send_command(uint8_t cmd, int8_t delta);
void    espnow_clear_pairing();                         // 清除 NVS 并回到扫描
void    espnow_resume();                                // light sleep 唤醒后调用
uint8_t espnow_channel();
