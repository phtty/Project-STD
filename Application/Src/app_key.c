/**
 * @file    app_key.c
 * @brief   按键应用层实现 — 20ms 轮询去抖 + 状态缓存
 *
 * 策略:
 *   SW1~SW3: EXTI 下降沿在 dev_key 中直接标记 s_state=true。
 *            key_poll_task 周期性读取 GPIO 实际电平，连续 3 次高电平（释放）
 *            后通过 dev_key_clear_state 置 false。
 *   DIP1~2: key_poll_task 周期性读取 GPIO 电平，连续 3 次一致后更新缓存。
 *   KEY_TST: 同时轮询 GPIO + 监听信号量，去抖后更新缓存。
 */

#include "app_key.h"

#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_gpio.h"

#define DEBOUNCE_CNT 3 /* 连续一致次数 → ~60ms 去抖窗口 (20ms × 3) */

/* ---- 引脚查表 ---- */
typedef struct {
    pl_port_t port;
    uint8_t pin;
} key_pin_t;

static const key_pin_t s_pin[DEV_KEY_COUNT] = {
    [DEV_KEY_SW1]  = {PL_PORT_E, 12},
    [DEV_KEY_SW2]  = {PL_PORT_E, 11},
    [DEV_KEY_SW3]  = {PL_PORT_E, 10},
    [DEV_KEY_TST]  = {PL_PORT_D, 8},
    [DEV_KEY_DIP1] = {PL_PORT_E, 7},
    [DEV_KEY_DIP2] = {PL_PORT_E, 8},
};

/* ---- 去抖状态机 ---- */
typedef struct {
    bool state;         /* 当前去抖后的输出状态 */
    bool last_raw;      /* 上一次原始读数 */
    uint8_t stable_cnt; /* 连续一致的次数 */
} debounce_t;

static debounce_t s_db[DEV_KEY_COUNT];

/* ---- 去抖后的状态缓存（对外暴露） ---- */
static bool s_cache[DEV_KEY_COUNT];

/* ---- 读取原始电平（低有效: true=按下/闭合） ---- */
static inline bool _raw_read(dev_key_id_t id)
{
    return !pl_gpio_read(s_pin[id].port, s_pin[id].pin);
}

/* ---- 通用去抖 ---- */
static bool _debounce(dev_key_id_t id, bool raw)
{
    debounce_t *d = &s_db[id];

    if (raw == d->last_raw) {
        d->stable_cnt++;
        if (d->stable_cnt >= DEBOUNCE_CNT) {
            d->state      = raw;
            d->stable_cnt = 0;
        }
    } else {
        d->stable_cnt = 0;
        d->last_raw   = raw;
    }
    return d->state;
}

/* ---- 轮询任务 ---- */
void app_key_task(void *argument)
{
    (void)argument;

    for (;;) {
        /* SW1~SW3: EXTI 已标记按下，这里只检测释放（GPIO 高电平 = 按键松开） */
        for (uint8_t i = DEV_KEY_SW1; i <= DEV_KEY_SW3; i++) {
            if (dev_key_get_state((dev_key_id_t)i)) {
                bool raw = _raw_read((dev_key_id_t)i);
                if (!raw) { /* GPIO 高电平 → 已释放 */
                    bool released = _debounce((dev_key_id_t)i, false);
                    if (released)
                        dev_key_clear_state((dev_key_id_t)i);
                }
            }
        }

        /* DIP1~2 + KEY_TST: 纯轮询去抖 */
        for (uint8_t i = DEV_KEY_TST; i <= DEV_KEY_DIP2; i++) {
            bool raw   = _raw_read((dev_key_id_t)i);
            s_cache[i] = _debounce((dev_key_id_t)i, raw);
        }

        /* SW1~SW3 的缓存跟随 dev_key 状态（按下由 EXTI 保证即时性） */
        for (uint8_t i = DEV_KEY_SW1; i <= DEV_KEY_SW3; i++)
            s_cache[i] = dev_key_get_state((dev_key_id_t)i);

        osDelay(20);
    }
}

/* ---- 公开 API ---- */

bool app_key_get_state(dev_key_id_t key_id)
{
    if (key_id >= DEV_KEY_COUNT) return false;
    return s_cache[key_id];
}

bool app_key_test_pressed(void)
{
    return dev_key_test_pressed();
}

/* ---- 模块自注册 ---- */
static void app_key_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "key_poll_task",
        .stack_size = 128 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(app_key_task, NULL, &attr);
}
sw_app_initcall(app_key_init);
