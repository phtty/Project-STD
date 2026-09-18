/**
 * @file    pl_tim.h
 * @brief   定时器抽象（Platform 层）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/** @brief 定时器实例 ID
 *
 *  **跨板稳定**：枚举值不随板子增删定时器而变，板子没有的那项在
 *  g_pl_tim_board[] 里留空（.init = NULL, .handle = NULL）即可。
 *  这样按名字引用某个定时器的共享代码（如 dev_display.c）不需要条件编译。 */
enum {
    PL_TIM2 = 0,
    PL_TIM3,
    PL_TIM4,
    PL_TIM7,
    PL_TIM_MAX,
};

typedef void *pl_tim_handle_t;

/** @brief 显示子系统占用的两个定时器角色
 *
 *  **这是 MCU 级常量，不是板级数据** —— 定时器是片上资源，两块板用的是同一颗
 *  STM32F407ZG，没有理由各选一套。统一之前 3833024 是"扫描 TIM3 / 调光 TIM4"、
 *  5006048 是"扫描 TIM2 / 调光 TIM3"，两者错位，看板级表根本看不出谁干什么，
 *  还容易把 5006048 的"PL_TIM4 留空"误读成"调光定时器缺失"。
 *
 *  约定：扫描 = TIM3，调光 PWM = TIM4，HAL 时基 = TIM7，**TIM2 不用**。
 *  两块板的 Core/Src/tim.c 都只配这三个（注意周期是**板级**的——模组不同，
 *  扫描率与 PWM 率各异，那部分留在各板自己的 tim.c 里）。 */
#define PL_TIM_DISPLAY_SCAN PL_TIM3
#define PL_TIM_DISPLAY_PWM  PL_TIM4

/** @brief 板级定时器表项（由 boards/<板>/Src/pl_tim_board.c 提供）
 *
 *  共享的 pl_tim.c 只认这张表，不认具体是哪几个定时器——哪些定时器存在、
 *  各自怎么初始化、句柄是谁、IRQ 号是多少，全是板级事实。 */
typedef struct {
    void (*init)(void);    /**< MX_TIMx_Init，NULL 表示本板无此定时器 */
    pl_tim_handle_t handle; /**< &htimx，NULL 表示本板无此定时器 */
    uint8_t irq;           /**< TIMx_IRQn，0 表示无 */
} pl_tim_board_entry_t;

extern const pl_tim_board_entry_t g_pl_tim_board[PL_TIM_MAX];

void pl_tim_init(void);
pl_tim_handle_t pl_tim_get_handle(uint8_t id);

/** @brief 启动定时器中断 */
void pl_tim_start_it(pl_tim_handle_t h);

/** @brief NVIC 中断开关（用于 OE 原子操作等） */
void pl_tim_irq_disable(uint8_t irq);
void pl_tim_irq_enable(uint8_t irq);

/** @brief 按定时器 ID 取 NVIC 中断号；本板无此定时器时返回 0 */
uint8_t pl_tim_irq_of(uint8_t id);

/** @brief 调试冻结定时器：调试器 halt 时让该定时器一并停住
 *
 *  之前只处理 TIM3、其余静默空操作，而调用点对 TIM3/TIM4 各调一次——
 *  TIM4 那次等于没调：halt 时行扫描停了、亮度 PWM 还在跑，屏会瞬间全亮。
 *  现按 Instance 通用处理 TIM2~TIM7。 */
void pl_tim_dbg_freeze(pl_tim_handle_t h);

/** @brief TIM 周期回调函数类型 */
typedef void (*pl_tim_period_cb_t)(void);

/** @brief 注册 TIM 周期回调（在 hw_dev_initcall 阶段调用） */
void pl_tim_register_period_cb(uint8_t tim_id, pl_tim_period_cb_t cb);
