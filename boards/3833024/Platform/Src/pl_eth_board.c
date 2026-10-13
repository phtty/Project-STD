/**
 * @file    pl_eth_board.c
 * @brief   3833024 的 ETH 板级引脚表
 *
 * 共享的 Platform/Src/pl_eth.c 只认 g_pl_eth_pin_grps[]，本文件是它对这块板的回答：
 * PHY 挂在哪些端口、哪些脚、什么复用功能。
 *
 * **这些数据原先直接写在 pl_eth.c 的 HAL_ETH_MspInit 里** —— 它是从 CubeMX 的
 * stm32f4xx_hal_msp.c 搬过来的板级数据，却住在共享层。两板恰好都是 F407 的标准
 * RMII 脚位，所以一直没暴露问题；但那属于"碰巧相同"，换一块 PHY 接线不同的板
 * 会静默拿到这几个脚，表现为"网口不通"且没有任何编译期提示。
 *
 * 引脚号在 CubeMX 的 main.h 里没有标签（本工程不再重新生成 CubeMX 代码），
 * 故此处直接写端口与引脚号。分组顺序即初始化顺序，无依赖关系。
 */

#include "pl_eth.h"
#include "main.h" /* stm32f4xx_hal.h：GPIO_PIN_x / GPIO_AF11_ETH */

const pl_eth_pin_grp_t g_pl_eth_pin_grps[] = {
    {PL_PORT_C, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5, GPIO_AF11_ETH},    /* MDC, RXD0, RXD1 */
    {PL_PORT_A, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7, GPIO_AF11_ETH},    /* REF_CLK, MDIO, CRS_DV */
    {PL_PORT_G, GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14, GPIO_AF11_ETH}, /* TX_EN, TXD0, TXD1 */
};

const uint8_t g_pl_eth_pin_grp_count = sizeof(g_pl_eth_pin_grps) / sizeof(g_pl_eth_pin_grps[0]);
