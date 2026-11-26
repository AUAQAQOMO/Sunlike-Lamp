#pragma once
#include <stdint.h>

// ============================================================================
// 无线旋钮控制器 <-> 台灯 的 ESP-NOW 协议
//
//   控制器 -> 台灯：PAIR_REQ（广播）、CMD（单播）
//   台灯 -> 控制器：PAIR_RESP、ACK
//
// 约定：
//   * 所有消息首字节为魔数，收到无关 ESP-NOW 包时直接丢弃，不解析；
//   * 消息体里带发送方真实 MAC —— ESP-NOW 广播时回调给的源 MAC 可能是临时值，
//     不能用来做配对判断（参考项目踩过这个坑）；
//   * 协议用「增量命令」而不是绝对状态：手机/小爱随时可能改灯，
//     控制器不保存状态就不会出现"转一下灯跳回旧值"。
// ============================================================================

#define ESPNOW_PROTO_MAGIC      0x53    // 'S'
#define ESPNOW_PROTO_VER        1

#define ESPNOW_DEV_CTRL         1       // 控制器
#define ESPNOW_DEV_LAMP         10      // 台灯

// ---- 消息类型 ----
#define ESPNOW_MSG_PAIR_REQ     0x01    // 控制器 -> 台灯（广播，扫描信道时发）
#define ESPNOW_MSG_PAIR_RESP    0x02    // 台灯 -> 控制器（携带台灯当前信道）
#define ESPNOW_MSG_CMD          0x03    // 控制器 -> 台灯（单播）
#define ESPNOW_MSG_ACK          0x04    // 台灯 -> 控制器（回应 PING，携带台灯信道）

// ---- 命令字 ----
#define ESPNOW_CMD_NUDGE_BRIGHT 1       // delta > 0 调亮，< 0 调暗
#define ESPNOW_CMD_NUDGE_CCT    2       // delta > 0 更冷，< 0 更暖
#define ESPNOW_CMD_TOGGLE       3       // 开/关翻转
#define ESPNOW_CMD_PING         4       // 探活（台灯回 ACK，控制器据此校信道）

typedef struct __attribute__((packed)) {
    uint8_t magic;      // 固定 ESPNOW_PROTO_MAGIC
    uint8_t ver;        // 固定 ESPNOW_PROTO_VER
    uint8_t msg_type;   // ESPNOW_MSG_xxx
    uint8_t dev_id;     // 发送方设备类型
    uint8_t mac[6];     // 发送方 STA MAC（真实 MAC）
    uint8_t channel;    // 发送方当前 WiFi 信道
    uint8_t cmd;        // ESPNOW_CMD_xxx（仅 CMD 消息有效）
    int8_t  delta;      // 增量（仅 NUDGE 命令有效）
    uint8_t seq;        // 序号，便于排查丢包
} espnow_msg_t;

// 编译期确认结构体大小，避免两端不一致
#if defined(__cplusplus)
static_assert(sizeof(espnow_msg_t) == 14, "espnow_msg_t 大小必须为 14 字节");
#endif
