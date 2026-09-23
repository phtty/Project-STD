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

/** @brief 按键/拨码开关 ID
 *
 *  **跨板稳定**：板子没有的按键不在 g_dev_key_board[] 里列出即可，
 *  dev_key_get() 对它返回 NULL，枚举值不变。 */
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
    pl_gpio_port_t port;
    uint8_t    pin;
    bool       active_low;
    osSemaphoreId_t press_sem;    /* EXTI 释放，wait_press 获取 */
    uint32_t        last_edge_ms; /* 上次被采纳的边沿时刻，EXTI 回调据此软件去抖 */
};

/** @brief 板级按键描述（由 boards/&lt;板&gt;/Device/Src/dev_key_board.c 提供）
 *
 *  "本板有哪些按键、各在哪根引脚、有没有 EXTI"是板级事实；而按键实例的存储、
 *  虚表选择、去抖、信号量生命周期都是机制，留在 dev_key.c。
 *
 *  拨码开关（DIPx）走纯轮询：has_exti=false，wait_press 恒为 NULL。 */
typedef struct {
    dev_key_id_t id;
    pl_gpio_port_t port;
    uint8_t pin;
    bool active_low;
    bool has_exti;   /**< true = EXTI 型（wait_press 可用）；false = 纯轮询（拨码） */
    uint16_t exti_pin; /**< 注册 EXTI 回调用的引脚掩码；0 = 不注册 */
} dev_key_board_desc_t;

extern const dev_key_board_desc_t g_dev_key_board[];
extern const uint32_t g_dev_key_board_count;

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
