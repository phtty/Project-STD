/**
 * @file    dev_key.c
 * @brief   按键设备实现 — OCP 虚表模式
 *
 * 两种派生类型:
 * - exti_key: SW1~SW3 + KEY_TST — EXTI 下降沿释放信号量，wait_press 阻塞
 * - dip_key:  DIP1~DIP2         — 无 EXTI，get_state 直接读 GPIO，wait_press=NULL
 */

#include "dev_key.h"

#include "main.h"
#include "initcall.h"
#include "pl_exti.h"

/* ================================================================
 *  exti_key 派生类型 — EXTI 信号量
 * ================================================================ */

typedef struct {
    dev_key_t me;
} exti_key_t;

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

static bool _exti_get_state(dev_key_t *key)
{
    return !pl_gpio_read(key->port, key->pin);
}

static bool _exti_wait_press(dev_key_t *key, uint32_t timeout_ms)
{
    if (!key->press_sem) return false;
    return osSemaphoreAcquire(key->press_sem, timeout_ms) == osOK;
}

static const dev_key_ops_t exti_key_ops = {
    .get_state  = _exti_get_state,
    .wait_press = _exti_wait_press,
};

/* ================================================================
 *  dip_key 派生类型 — 纯 GPIO 读取
 * ================================================================ */

typedef struct {
    dev_key_t me;
} dip_key_t;

static bool _dip_get_state(dev_key_t *key)
{
    return !pl_gpio_read(key->port, key->pin);
}

static const dev_key_ops_t dip_key_ops = {
    .get_state  = _dip_get_state,
    .wait_press = NULL,
};

/* ================================================================
 *  实例
 * ================================================================ */

static exti_key_t g_sw1  = {.me = {.id = DEV_KEY_SW1,  .port = PL_PORT_E, .pin = 12, .active_low = true}};
static exti_key_t g_sw2  = {.me = {.id = DEV_KEY_SW2,  .port = PL_PORT_E, .pin = 11, .active_low = true}};
static exti_key_t g_sw3  = {.me = {.id = DEV_KEY_SW3,  .port = PL_PORT_E, .pin = 10, .active_low = true}};
static exti_key_t g_tst  = {.me = {.id = DEV_KEY_TST,  .port = PL_PORT_D, .pin =  8, .active_low = true}};
static dip_key_t  g_dip1 = {.me = {.id = DEV_KEY_DIP1, .port = PL_PORT_E, .pin =  7, .active_low = true}};
static dip_key_t  g_dip2 = {.me = {.id = DEV_KEY_DIP2, .port = PL_PORT_E, .pin =  8, .active_low = true}};

static dev_key_t *s_keys[DEV_KEY_COUNT];

dev_key_t *dev_key_get(dev_key_id_t id)
{
    return (id < DEV_KEY_COUNT) ? s_keys[id] : NULL;
}

/* ================================================================
 *  EXTI 回调 — ISR 上下文，释放对应按键的信号量
 * ================================================================ */

static void _exti_cb(uint16_t pin, void *ctx)
{
    (void)ctx;
    for (uint8_t i = 0; i < DEV_KEY_COUNT; i++) {
        dev_key_t *k = s_keys[i];
        uint16_t k_pin_mask = (uint16_t)(1U << k->pin);

        if (pin == k_pin_mask && k->press_sem) {
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
    s_keys[DEV_KEY_SW1]  = &g_sw1.me;
    s_keys[DEV_KEY_SW2]  = &g_sw2.me;
    s_keys[DEV_KEY_SW3]  = &g_sw3.me;
    s_keys[DEV_KEY_TST]  = &g_tst.me;
    s_keys[DEV_KEY_DIP1] = &g_dip1.me;
    s_keys[DEV_KEY_DIP2] = &g_dip2.me;

    g_sw1.me.ops  = &exti_key_ops;
    g_sw2.me.ops  = &exti_key_ops;
    g_sw3.me.ops  = &exti_key_ops;
    g_tst.me.ops  = &exti_key_ops;
    g_dip1.me.ops = &dip_key_ops;
    g_dip2.me.ops = &dip_key_ops;

    pl_exti_register_cb(SW1_Pin, _exti_cb, NULL);
    pl_exti_register_cb(SW2_Pin, _exti_cb, NULL);
    pl_exti_register_cb(SW3_Pin, _exti_cb, NULL);
    pl_exti_register_cb(KEY_TST_Pin, _exti_cb, NULL);
}
hw_dev_initcall(dev_key_init);

void dev_key_sw_init(void)
{
    for (uint8_t i = 0; i < DEV_KEY_COUNT; i++) {
        dev_key_t *k = s_keys[i];
        if (k->ops->wait_press)
            k->press_sem = osSemaphoreNew(1, 0, NULL);
    }
}
sw_dev_initcall(dev_key_sw_init);
