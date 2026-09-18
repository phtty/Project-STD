/**
 * @file    dev_key.c
 * @brief   按键设备实现 — OCP 虚表模式
 *
 * 两种行为，由板级描述表的 has_exti 选择：
 * - exti: EXTI 下降沿释放信号量，wait_press 阻塞，get_state 读引脚（SW1~SW3、KEY_TST）
 * - dip:  无 EXTI，wait_press=NULL，get_state 直接读 GPIO（DIP1~DIP2）
 *
 * 本文件只有机制；"本板有哪些按键、各在哪根引脚"来自板级的 g_dev_key_board[]
 * （见 boards/<板>/Src/dev_key_board.c）。板上没有的按键 id 不在表里，
 * dev_key_get() 返回 NULL，因此下面所有遍历都要容忍空槽。
 */

#include "dev_key.h"

#include "initcall.h"
#include "pl_exti.h"

/* ================================================================
 *  虚表实现
 * ================================================================ */

static bool _exti_get_state(dev_key_t *key)
{
    return !pl_gpio_read(key->port, key->pin);
}

static bool _exti_wait_press(dev_key_t *key, uint32_t timeout_ms)
{
    if (!key->press_sem) return false;
    return osSemaphoreAcquire(key->press_sem, timeout_ms) == osOK;
}

/** 去抖窗口（毫秒）。
 *
 * 机械按键一次按下会产生多个下降沿，而 EXTI 回调是**每个边沿都释放一次**信号量；
 * 信号量上限为 1，多出来的那次就变成"残留令牌"，会被后续某次
 * `dev_key_wait_press(..., timeout)` 立刻消费掉。
 *
 * 实测后果：工厂测试的衰老轮播在显示第一个字之后，第一次带超时的等待返回 true，
 * 于是立刻退出轮播并清屏 —— 表现为"只显示第一个字就黑屏"。
 *
 * 在源头去抖，比在每个调用点做防御干净。 */
#define KEY_DEBOUNCE_MS (50U)

static const dev_key_ops_t exti_key_ops = {
    .get_state  = _exti_get_state,
    .wait_press = _exti_wait_press,
};

static bool _dip_get_state(dev_key_t *key)
{
    return !pl_gpio_read(key->port, key->pin);
}

static const dev_key_ops_t dip_key_ops = {
    .get_state  = _dip_get_state,
    .wait_press = NULL,
};

/* ================================================================
 *  实例存储（参数由板级表填充）
 * ================================================================ */

static dev_key_t s_keys[DEV_KEY_COUNT];

dev_key_t *dev_key_get(dev_key_id_t id)
{
    if (id >= DEV_KEY_COUNT) return NULL;
    /* ops 为空 = 本板没有这个按键 */
    return s_keys[id].ops ? &s_keys[id] : NULL;
}

/* ================================================================
 *  EXTI 回调 — ISR 上下文，释放对应按键的信号量
 * ================================================================ */

static void _exti_cb(uint16_t pin, void *ctx)
{
    (void)ctx;
    for (uint8_t i = 0; i < DEV_KEY_COUNT; i++) {
        dev_key_t *k = &s_keys[i];
        if (!k->ops || !k->press_sem) continue;

        if (pin == (uint16_t)(1U << k->pin)) {
            /* 软件去抖：抖动产生的重复边沿不再各释放一次。
               osKernelGetTickCount 在 ISR 上下文可用。 */
            uint32_t now = osKernelGetTickCount();
            if (now - k->last_edge_ms >= KEY_DEBOUNCE_MS) {
                k->last_edge_ms = now;
                osSemaphoreRelease(k->press_sem);
            }
            break;
        }
    }
}

/* ================================================================
 *  初始化
 * ================================================================ */

void dev_key_init(void)
{
    for (uint32_t i = 0; i < g_dev_key_board_count; i++) {
        const dev_key_board_desc_t *d = &g_dev_key_board[i];
        if (d->id >= DEV_KEY_COUNT) continue;

        dev_key_t *k    = &s_keys[d->id];
        k->id           = d->id;
        k->port         = d->port;
        k->pin          = d->pin;
        k->active_low   = d->active_low;
        k->ops          = d->has_exti ? &exti_key_ops : &dip_key_ops;

        if (d->exti_pin) pl_exti_register_cb(d->exti_pin, _exti_cb, NULL);
    }
}
hw_dev_initcall(dev_key_init);

void dev_key_sw_init(void)
{
    for (uint8_t i = 0; i < DEV_KEY_COUNT; i++) {
        dev_key_t *k = &s_keys[i];
        if (k->ops && k->ops->wait_press)
            k->press_sem = osSemaphoreNew(1, 0, NULL);
    }
}
sw_dev_initcall(dev_key_sw_init);
