#include "espnow_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "espnow_proto.h"

namespace {

enum LinkState { ST_SCANNING, ST_PAIRED };

uint8_t s_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t   s_peer[6]      = {0};
uint8_t   s_channel      = 1;
uint8_t   s_scan_ch      = ESPNOW_SCAN_START_CH;
uint32_t  s_scan_ms      = 0;
uint32_t  s_last_ping_ms = 0;
uint32_t  s_ping_sent_ms = 0;
bool      s_ping_waiting = false;
uint8_t   s_ping_fail    = 0;
uint16_t  s_seq          = 0;
bool      s_need_save    = false;
LinkState s_state        = ST_SCANNING;

// ESP-NOW 回调运行在 WiFi 任务上下文，里面调用 esp_now_add_peer()/del_peer()/
// esp_wifi_set_channel() 会二次加锁（可能死锁），所以回调只记录，主循环执行。
volatile bool s_pairRespPending = false;
uint8_t       s_pairRespMac[6]  = {0};
uint8_t       s_pairRespCh      = 0;

volatile bool s_ackPending      = false;
uint8_t       s_ackCh           = 0;

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

bool addPeer(const uint8_t *mac, uint8_t channel) {
    esp_now_del_peer(mac);

    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = channel;
    info.encrypt = false;
    info.ifidx   = WIFI_IF_STA;

    const esp_err_t r = esp_now_add_peer(&info);
    if (r != ESP_OK) {
        Serial.printf("[NOW] 添加 peer 失败: 0x%x\n", r);
        return false;
    }
    return true;
}

void loadPairing() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) {
        return;
    }
    uint8_t mac[6] = {0};
    const uint8_t ch = p.getUChar(NVS_KEY_CH, 1);
    const size_t n = p.getBytes(NVS_KEY_MAC, mac, sizeof(mac));
    p.end();

    if (n == sizeof(mac) && !macIsZero(mac)) {
        memcpy(s_peer, mac, 6);
        s_channel = (ch >= 1 && ch <= 13) ? ch : 1;
        Serial.printf("[NOW] 已保存配对：%s 信道 %d\n",
                      macToStr(s_peer).c_str(), s_channel);
    }
}

void savePairing() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        return;
    }
    p.putBytes(NVS_KEY_MAC, s_peer, 6);
    p.putUChar(NVS_KEY_CH, s_channel);
    p.end();
}

// 组包并发送
bool sendRaw(const uint8_t *dst, uint8_t msgType, uint8_t cmd, int8_t delta) {
    espnow_msg_t msg = {};
    msg.magic    = ESPNOW_PROTO_MAGIC;
    msg.ver      = ESPNOW_PROTO_VER;
    msg.msg_type = msgType;
    msg.dev_id   = ESPNOW_DEV_CTRL;
    WiFi.macAddress(msg.mac);           // 真实 MAC，广播时回调源 MAC 不可靠
    msg.channel  = s_channel;
    msg.cmd      = cmd;
    msg.delta    = delta;
    msg.seq      = (uint8_t)(s_seq++ & 0xFF);

    return esp_now_send(dst, (uint8_t *)&msg, sizeof(msg)) == ESP_OK;
}

// 进入扫描；已配对时从“上次成功的信道”开始，802ms 内就能重连，
// 不必每次都从 1 号信道扫起（最坏要等 10 秒）。
void enterScan(uint8_t startCh) {
    s_state        = ST_SCANNING;
    s_scan_ch      = (startCh >= ESPNOW_SCAN_START_CH && startCh <= ESPNOW_SCAN_END_CH)
                         ? startCh
                         : ESPNOW_SCAN_START_CH;
    s_scan_ms      = 0;
    s_ping_waiting = false;
    s_ping_fail    = 0;
}

// 广播配对请求，并带上“我正在哪个信道”，方便双方尽早对齐
void sendPairReq() {
    espnow_msg_t msg = {};
    msg.magic    = ESPNOW_PROTO_MAGIC;
    msg.ver      = ESPNOW_PROTO_VER;
    msg.msg_type = ESPNOW_MSG_PAIR_REQ;
    msg.dev_id   = ESPNOW_DEV_CTRL;
    WiFi.macAddress(msg.mac);
    msg.channel  = s_scan_ch;
    msg.seq      = (uint8_t)(s_seq++ & 0xFF);

    esp_now_send(s_broadcast, (uint8_t *)&msg, sizeof(msg));
}

// ---------------------------------------------------------------------------
// 处理配对响应（运行在主循环，可安全调用 esp_wifi_set_channel / add_peer / NVS）
// ---------------------------------------------------------------------------
void handlePairResp() {
    const uint8_t ch = (s_pairRespCh >= 1 && s_pairRespCh <= 13) ? s_pairRespCh : s_scan_ch;

    if (memcmp(s_peer, s_pairRespMac, 6) != 0) {
        memcpy(s_peer, s_pairRespMac, 6);
        Serial.printf("[NOW] 配对成功：%s\n", macToStr(s_peer).c_str());
    } else if (ch != s_channel) {
        Serial.printf("[NOW] 台灯信道 %d -> %d，已跟随\n", s_channel, ch);
    }

    s_channel = ch;
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    addPeer(s_peer, s_channel);         // 广播 peer 的 channel=0，会自动跟随当前信道

    if (s_state != ST_PAIRED) {
        s_state        = ST_PAIRED;
        s_last_ping_ms = millis();
        s_ping_waiting = false;
        s_ping_fail    = 0;
    }
    s_need_save = true;
}

// ---------------------------------------------------------------------------
// 处理 PING 应答
// ---------------------------------------------------------------------------
void handleAck() {
    s_ping_waiting = false;
    s_ping_fail    = 0;

    // 应答里带着台灯当前信道：路由器重启换了信道时自动跟随
    if (s_ackCh >= 1 && s_ackCh <= 13 && s_ackCh != s_channel) {
        s_channel = s_ackCh;
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        addPeer(s_peer, s_channel);
        s_need_save = true;
        Serial.printf("[NOW] 跟随台灯切到信道 %d\n", s_channel);
    }
}

// ---------------------------------------------------------------------------
// 接收回调（IDF 4.4 签名）：只记录事件
// ---------------------------------------------------------------------------
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    (void)mac;
    if (len != (int)sizeof(espnow_msg_t)) {
        return;
    }

    const espnow_msg_t *m = (const espnow_msg_t *)data;
    if (m->magic != ESPNOW_PROTO_MAGIC || m->ver != ESPNOW_PROTO_VER) {
        return;
    }
    if (m->dev_id != ESPNOW_DEV_LAMP) {
        return;
    }

    // ---- 配对响应：只记录，主循环处理 ----
    if (m->msg_type == ESPNOW_MSG_PAIR_RESP) {
        // 已有配对记录时，只认原来那台，避免重扫时误配到别的灯
        if (!macIsZero(s_peer) && memcmp(s_peer, m->mac, 6) != 0) {
            return;
        }
        memcpy(s_pairRespMac, m->mac, 6);
        s_pairRespCh      = m->channel;
        s_pairRespPending = true;
        return;
    }

    // ---- PING 应答：只记录 ----
    if (m->msg_type == ESPNOW_MSG_ACK) {
        if (s_state != ST_PAIRED || memcmp(s_peer, m->mac, 6) != 0) {
            return;
        }
        s_ackCh      = m->channel;
        s_ackPending = true;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
void espnow_init() {
    WiFi.mode(WIFI_STA);                // 只做 ESP-NOW，不连 AP
    WiFi.disconnect();

    loadPairing();

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[NOW] ESP-NOW 初始化失败"));
        return;
    }
    esp_now_register_recv_cb(onRecv);

    // 关闭省电，否则收包延迟可达数百毫秒
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 广播地址必须登记为 peer，否则无法发送广播包
    addPeer(s_broadcast, 0);

    if (!macIsZero(s_peer)) {
        // 有配对记录：直接固定到已知信道快速重连，同时发一次配对请求确认在线
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        addPeer(s_peer, s_channel);
        Serial.printf("[NOW] 尝试重连：%s 信道 %d\n",
                      macToStr(s_peer).c_str(), s_channel);
        sendRaw(s_peer, ESPNOW_MSG_PAIR_REQ, 0, 0);
    } else {
        Serial.println(F("[NOW] 未配对，开始扫描信道"));
    }

    // 已配对就从上次的信道开始扫（800ms 内可重连）；未配对从 1 号信道开始
    enterScan(macIsZero(s_peer) ? ESPNOW_SCAN_START_CH : s_channel);
}

void espnow_poll() {
    const uint32_t now = millis();

    // 回调记录的事件在这里落地（加 peer / 切信道 / 写 NVS 全在主循环做）
    if (s_pairRespPending) {
        s_pairRespPending = false;
        handlePairResp();
    }
    if (s_ackPending) {
        s_ackPending = false;
        handleAck();
    }

    if (s_need_save) {
        s_need_save = false;
        savePairing();
    }

    // ---------------- 扫描配对 ----------------
    if (s_state == ST_SCANNING) {
        if (now - s_scan_ms < ESPNOW_SCAN_DWELL_MS) {
            return;
        }
        s_scan_ms = now;

        esp_wifi_set_channel(s_scan_ch, WIFI_SECOND_CHAN_NONE);
        sendPairReq();

        s_scan_ch = (s_scan_ch >= ESPNOW_SCAN_END_CH)
                        ? ESPNOW_SCAN_START_CH
                        : (uint8_t)(s_scan_ch + 1);
        return;
    }

    // ---------------- 已配对：在线探测 ----------------
    if (s_ping_waiting) {
        if (now - s_ping_sent_ms >= ESPNOW_PING_TIMEOUT_MS) {
            s_ping_waiting = false;
            if (++s_ping_fail >= ESPNOW_PING_FAIL_LIMIT) {
                Serial.println(F("[NOW] 台灯无响应，开始重扫信道"));
                enterScan(ESPNOW_SCAN_START_CH);
            }
        }
        return;
    }

    if (now - s_last_ping_ms >= ESPNOW_PING_INTERVAL_MS) {
        s_last_ping_ms = now;
        s_ping_sent_ms = now;
        s_ping_waiting = true;
        sendRaw(s_peer, ESPNOW_MSG_CMD, ESPNOW_CMD_PING, 0);
    }
}

bool espnow_paired() {
    return s_state == ST_PAIRED;
}

bool espnow_send_command(uint8_t cmd, int8_t delta) {
    if (s_state != ST_PAIRED) {
        return false;
    }
    return sendRaw(s_peer, ESPNOW_MSG_CMD, cmd, delta);
}

void espnow_clear_pairing() {
    if (!macIsZero(s_peer)) {
        esp_now_del_peer(s_peer);
    }
    memset(s_peer, 0, 6);

    Preferences p;
    if (p.begin(NVS_NS, false)) {
        p.remove(NVS_KEY_MAC);
        p.remove(NVS_KEY_CH);
        p.end();
    }

    // 防御：即便调用方没有立即重启，也要回到扫描状态，否则会卡在
    // ST_PAIRED 却没有任何 peer，永远发不出去、也不会重新配对。
    enterScan(ESPNOW_SCAN_START_CH);

    Serial.println(F("[NOW] 配对信息已清除"));
}

void espnow_resume() {
    // light sleep 会暂停 WiFi/ESP-NOW，唤醒后最稳妥的做法是整体重建一次
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_now_deinit();
    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[NOW] 唤醒后重建 ESP-NOW 失败"));
        return;
    }
    esp_now_register_recv_cb(onRecv);

    addPeer(s_broadcast, 0);
    if (!macIsZero(s_peer)) {
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        addPeer(s_peer, s_channel);
    }

    // 睡眠前那次 PING 的应答肯定收不到了，重置计时避免误判掉线
    s_ping_waiting = false;
    s_ping_fail    = 0;
    s_last_ping_ms = millis();
    s_scan_ms      = 0;
}

uint8_t espnow_channel() {
    return s_channel;
}
