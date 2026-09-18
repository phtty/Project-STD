/**
 * @file    pl_hub75_board.c
 * @brief   std_b 的 HUB75 端口基址表与初始化
 *
 * 共享代码只用控制信号（见 boards/std_b/Inc/pl_hub75.h 的契约说明）；
 * 数据通道的端口索引表是板级事实，落在这里，由面板驱动
 * （dev_p10_112x10_1000000661.c 的 channel_map）消费。
 */

#include "pl_hub75.h"
#include "initcall.h"
#include <stddef.h> /* NULL */

/* ---- 端口基址表 (B 工程 channel_port[]) ---- */
static GPIO_TypeDef *const s_port_table[] = {
    GPIOA, /* 0 */
    GPIOB, /* 1 */
    GPIOC, /* 2 */
    GPIOD, /* 3 */
    GPIOE, /* 4 */
    GPIOF, /* 5 */
    GPIOG, /* 6 */
};

GPIO_TypeDef *pl_hub75_port_by_idx(uint8_t idx)
{
    return (idx < sizeof(s_port_table) / sizeof(s_port_table[0])) ? s_port_table[idx] : NULL;
}

/* ---- 初始化 ---- */
void pl_hub75_init(void)
{
    /* OE 默认关断（高电平 = 输出禁止），LAT/CLK/行地址 低电平 */
    HUB75_OE  = 1;
    HUB75_LAT = 0;
    HUB75_CLK = 0;
    HUB75_A = HUB75_B = HUB75_C = HUB75_D = 0;
}
hw_pl_initcall(pl_hub75_init);
