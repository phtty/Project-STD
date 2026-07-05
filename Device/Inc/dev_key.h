/**
 * @file    dev_key.h
 * @brief   按键/拨码开关设备 — OCP 虚表基类
 *
 * 基类提供按键 ID、引脚信息和信号量。派生类通过 ops 虚表注入行为差异：
 * - SW1~SW3: EXTI 下降沿释放信号量 → wait_press 阻塞, get_state 读引脚
 * - KEY_TST: 同 SW
 * - DIP1~2: 无 EXTI, wait_press=NULL, get_state 直接读 GPIO
 *
 * 所有按键 GPIO 内部上拉 + 低有效。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cmsis_os2.h"
#include "pl_gpio.h"

/** @brief 按键/拨码开关 ID */
typedef enum {
    DEV_KEY_SW1  = 0,
    DEV_KEY_SW2  = 1,
    DEV_KEY_SW3  = 2,
    DEV_KEY_TST  = 3,
    DEV_KEY_DIP1 = 4,
    DEV_KEY_DIP2 = 5,
    DEV_KEY_COUNT
} dev_key_id_t;

typedef struct dev_key dev_key_t;

/** @brief 按键操作虚表 */
typedef struct dev_key_ops {
    bool (*get_state)(dev_key_t *key);
    bool (*wait_press)(dev_key_t *key, uint32_t timeout_ms);
} dev_key_ops_t;

/** @brief 按键基类（派生类将其放在第一个成员位置） */
struct dev_key {
    const dev_key_ops_t *ops;
    dev_key_id_t id;
    pl_port_t port;
    uint8_t    pin;
    bool       active_low;
    osSemaphoreId_t press_sem; /* EXTI 释放，wait_press 获取 */
};

/* ---- 初始化 ---- */
void dev_key_init(void);     /* hw_dev_initcall: 注册 EXTI 回调 */
void dev_key_sw_init(void);  /* sw_dev_initcall: 创建信号量 */

/** @brief 获取按键实例 */
dev_key_t *dev_key_get(dev_key_id_t id);

/* ---- 便捷 API ---- */

static inline bool dev_key_get_state(dev_key_id_t id)
{
    dev_key_t *k = dev_key_get(id);
    return k && k->ops->get_state ? k->ops->get_state(k) : false;
}

static inline bool dev_key_wait_press(dev_key_id_t id, uint32_t timeout_ms)
{
    dev_key_t *k = dev_key_get(id);
    return k && k->ops->wait_press ? k->ops->wait_press(k, timeout_ms) : false;
}
