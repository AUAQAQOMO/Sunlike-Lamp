#include "esp_now_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "espnow_proto.h"
#include "light_engine.h"

extern LightEngine g_light;

namespace {

const char *NVS_NS  = "esplink";
const char *NVS_KEY = "peer";

uint8_t s_peer[6] = {0};
bool    s_paired  = false;
bool    s_enabled = false;      // ENABLE_ESP_NOW=0 时保持 false，所有接口静默返回

// ESP-NOW 的接收回调运行在 WiFi 任务上下文里，在里面调用
// esp_now_add_peer() / esp_now_del_peer() / esp_now_send() / NVS 写入
// 都会二次加锁，存在死锁与看门狗风险。
// 所以回调只做记录，真正的动作统一放到 espnow_link_poll()（主循环）执行。
volatile bool s_pairPending = false;
uint8_t       s_pairMac[6]  = {0};
volatile bool s_pingPending = false;

// ---------------------------------------------------------------------------
// 命令队列
//   ESP-NOW 回调跑在 WiFi 任务上下文，直接改 LightEngine 会与主循环的 update()
//   竞争，所以回调里只入队，真正的动作放到 espnow_link_poll() 里执行。
// ---------------------------------------------------------------------------
struct PendingCmd {
    uint8_t cmd;
    int8_t  delta;
};

const int QUEUE_LEN = 8;
PendingCmd s_queue[QUEUE_LEN];
volatile int s_head = 0;
volatile int s_tail = 0;

String macToStr(const uint8_t *m) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

bool macIsZero(const uint8_t *m) {
    for (int i = 0; i < 6; ++i) {
        if (m[i] != 0) return false;
    }
    return true;
}

void loadPairing() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) {
        return;
    }
    uint8_t tmp[6] = {0};
    const size_t n = p.getBytes(NVS_KEY, tmp, sizeof(tmp));
    p.end();

    if (n == sizeof(tmp) && !macIsZero(tmp)) {
        memcpy(s_peer, tmp, 6);
        s_paired = true;
        Serial.printf("[NOW] 已配对控制器 %s\n", macToStr(s_peer).c_str());
    }
}

void savePairing() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        return;
    }
    p.putBytes(NVS_KEY, s_peer, 6);
    p.end();
}

bool addPeer(const uint8_t *mac) {
    esp_now_del_peer(mac);                  // 已存在时先删，避免重复添加报错

    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = 0;                       // 0 = 使用当前 WiFi 信道
    info.encrypt = false;
    info.ifidx   = WIFI_IF_STA;

    const esp_err_t r = esp_now_add_peer(&info);
    if (r != ESP_OK) {
        Serial.printf("[NOW] 添加 peer 失败: 0x%x\n", r);
        return false;
    }
    return true;
}

// 发一条消息给控制器；channel 传 0 表示“用本机当前信道”
void sendMsg(const uint8_t *mac, uint8_t msgType, uint8_t cmd, int8_t delta) {
    espnow_msg_t msg = {};
    msg.magic    = ESPNOW_PROTO_MAGIC;
    msg.ver      = ESPNOW_PROTO_VER;
    msg.msg_type = msgType;
    msg.dev_id   = ESPNOW_DEV_LAMP;
    WiFi.macAddress(msg.mac);               // 我方真实 MAC
    msg.channel  = (uint8_t)WiFi.channel(); // 关键：把自己的信道告诉控制器
    msg.cmd      = cmd;
    msg.delta    = delta;
    msg.seq      = (uint8_t)(millis() & 0xFF);

    esp_now_send(mac, (uint8_t *)&msg, sizeof(msg));
}

// ---------------------------------------------------------------------------
// 处理配对请求（运行在主循环，可以安全调用 ESP-NOW 与 NVS）
// ---------------------------------------------------------------------------
void handlePairRequest() {
    if (macIsZero(s_pairMac)) {
        return;                         // 防御：异常的空 MAC 直接忽略
    }

    if (s_paired) {
        if (memcmp(s_peer, s_pairMac, 6) != 0) {
            // 已配对：忽略其他控制器（需长按 5s 清除配对才能重新配）
            static uint32_t lastWarn = 0;
            if (millis() - lastWarn > 5000) {
                lastWarn = millis();
                Serial.println(F("[NOW] 已有配对，忽略其他控制器的配对请求"));
            }
            return;
        }
    } else {
        memcpy(s_peer, s_pairMac, 6);
        s_paired = true;
        savePairing();
        Serial.printf("[NOW] 配对成功：%s\n", macToStr(s_peer).c_str());
    }

    if (addPeer(s_peer)) {
        sendMsg(s_peer, ESPNOW_MSG_PAIR_RESP, 0, 0);    // 回包携带本机信道
    }
}

// ---------------------------------------------------------------------------
// 接收回调（IDF 4.4 签名）：只记录事件，不做任何加锁操作
// ---------------------------------------------------------------------------
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    (void)mac;                              // 广播时源 MAC 可能是临时值，不用它
    if (len != (int)sizeof(espnow_msg_t)) {
        return;
    }

    const espnow_msg_t *m = (const espnow_msg_t *)data;
    if (m->magic != ESPNOW_PROTO_MAGIC || m->ver != ESPNOW_PROTO_VER) {
        return;                             // 不是本协议的包，直接丢弃
    }
    if (m->dev_id != ESPNOW_DEV_CTRL) {
        return;
    }

    // ---- 配对请求：只记录，主循环里处理 ----
    if (m->msg_type == ESPNOW_MSG_PAIR_REQ) {
        memcpy(s_pairMac, m->mac, 6);
        s_pairPending = true;
        return;
    }

    // ---- 其余消息只接受已配对控制器 ----
    if (!s_paired || memcmp(s_peer, m->mac, 6) != 0) {
        return;
    }

    if (m->msg_type == ESPNOW_MSG_CMD) {
        if (m->cmd == ESPNOW_CMD_PING) {
            s_pingPending = true;           // 应答同样交给主循环发送
            return;
        }

        const int next = (s_head + 1) % QUEUE_LEN;
        if (next != s_tail) {               // 队列满则丢弃（手速远达不到）
            s_queue[s_head].cmd   = m->cmd;
            s_queue[s_head].delta = m->delta;
            s_head = next;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
void espnow_link_begin() {
#if !ENABLE_ESP_NOW
    // 不配无线旋钮的批次：完全不碰 ESP-NOW，也不关闭 WiFi 省电，
    // 台灯的本地旋钮/巴法云/OTA/记忆全部照常。
    Serial.println(F("[NOW] 无线旋钮功能已禁用（ENABLE_ESP_NOW=0）"));
    return;
#else
    s_enabled = true;
    loadPairing();

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[NOW] ESP-NOW 初始化失败"));
        return;
    }

    esp_now_register_recv_cb(onRecv);

    // 关闭 WiFi 省电：默认 modem sleep 会让 ESP-NOW 收包延迟 100~300ms，
    // 旋钮会明显"发涩"。台灯是插电设备，不需要省这点电。
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (s_paired) {
        addPeer(s_peer);
    }

    Serial.printf("[NOW] 就绪，信道 %d，%s\n",
                  (int)WiFi.channel(), s_paired ? "已配对" : "等待配对");
#endif  // ENABLE_ESP_NOW
}

// ---------------------------------------------------------------------------
// WiFi 模式被外部改动后重建链路
//   AP 配网会切到 WIFI_AP，配网超时返回后 ESP-NOW 已经失效；
//   不重建的话无线旋钮要等下次重启才能用。
// ---------------------------------------------------------------------------
void espnow_link_resume() {
#if !ENABLE_ESP_NOW
    return;
#else
    if (!s_enabled) {
        return;
    }

    esp_now_deinit();
    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[NOW] 重建 ESP-NOW 失败"));
        return;
    }
    esp_now_register_recv_cb(onRecv);
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (s_paired) {
        addPeer(s_peer);
    }
    Serial.printf("[NOW] 已重建，信道 %d\n", (int)WiFi.channel());
#endif
}

void espnow_link_poll() {
    // 配对请求：回调只置位，这里才做加 peer / 回包 / 写 NVS
    if (s_pairPending) {
        s_pairPending = false;
        handlePairRequest();
    }

    // PING 应答
    if (s_pingPending) {
        s_pingPending = false;
        if (s_paired) {
            sendMsg(s_peer, ESPNOW_MSG_ACK, ESPNOW_CMD_PING, 0);
        }
    }

    while (s_tail != s_head) {
        const PendingCmd p = s_queue[s_tail];
        s_tail = (s_tail + 1) % QUEUE_LEN;

        switch (p.cmd) {
            case ESPNOW_CMD_NUDGE_BRIGHT:
                if (!g_light.power()) g_light.setPower(true);   // 灭灯时转旋钮先点亮
                g_light.nudgeBrightness(p.delta);
                break;

            case ESPNOW_CMD_NUDGE_CCT:
                if (!g_light.power()) g_light.setPower(true);
                g_light.nudgeCct(p.delta);
                break;

            case ESPNOW_CMD_TOGGLE:
                g_light.setPower(!g_light.power());
                break;

            default:
                break;
        }
    }
}

bool espnow_link_paired() {
    return s_paired;
}

void espnow_link_get_peer(uint8_t mac_out[6]) {
    memcpy(mac_out, s_peer, 6);
}

void espnow_link_clear() {
    if (s_paired) {
        esp_now_del_peer(s_peer);
    }
    memset(s_peer, 0, 6);
    s_paired = false;

    Preferences p;
    if (p.begin(NVS_NS, false)) {
        p.remove(NVS_KEY);
        p.end();
    }
    Serial.println(F("[NOW] 配对已清除，等待新的控制器"));
}
