#pragma once
#include <stdint.h>

// ============================================================================
// power_mgmt —— 低功耗管理（light sleep）
//
// 为什么不用 deep sleep：
//   C3 的深睡 GPIO 唤醒依赖 ext1，而 IDF 4.4 的 ext1 只支持
//   ALL_LOW / ANY_HIGH 两种模式；EC11 是“上拉、动作时拉低”，
//   A 相与按键不可能同时为低，只能二选一——选了 A 相就没法用按键唤醒。
//   light sleep 支持"任意引脚低电平唤醒"，唤醒约 1ms、RAM 保持，
//   代价是待机电流比深睡高（约 0.5mA 量级）。
//
// 补充：C3 没有 RTC IO（SOC_RTCIO_PIN_COUNT == 0），rtc_gpio_* 系列 API
//   不存在；但 GPIO 由常电域供电，light sleep 期间 INPUT_PULLUP 的上拉
//   会保持，因此 GPIO4/5/6 都能安全地作为唤醒源。
// ============================================================================

void power_init();                  // 配置唤醒源；须在 setup 里最先调用
bool power_woke_from_sleep();       // 本次启动是否由睡眠唤醒
void power_sleep();                 // 进入 light sleep，返回即已唤醒
