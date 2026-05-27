/**
 * @file    pl_hub75.c
 * @brief   HUB75 平台抽象 — 引脚表和初始化
 */

#include "pl_hub75.h"
#include "initcall.h"

/* ---- 引脚映射表（从 main.h 的 CubeMX 引脚定义组装） ---- */

const hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX] = {
    {HUB75_R1_GPIO_Port,  HUB75_R1_Pin},
    {HUB75_R2_GPIO_Port,  HUB75_R2_Pin},
    {HUB75_R3_GPIO_Port,  HUB75_R3_Pin},
    {HUB75_R4_GPIO_Port,  HUB75_R4_Pin},
    {HUB75_R5_GPIO_Port,  HUB75_R5_Pin},
    {HUB75_R6_GPIO_Port,  HUB75_R6_Pin},
    {HUB75_R7_GPIO_Port,  HUB75_R7_Pin},
    {HUB75_R8_GPIO_Port,  HUB75_R8_Pin},
    {HUB75_R9_GPIO_Port,  HUB75_R9_Pin},
    {HUB75_R10_GPIO_Port, HUB75_R10_Pin},
};

const hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX] = {
    {HUB75_G1_GPIO_Port,  HUB75_G1_Pin},
    {HUB75_G2_GPIO_Port,  HUB75_G2_Pin},
    {HUB75_G3_GPIO_Port,  HUB75_G3_Pin},
    {HUB75_G4_GPIO_Port,  HUB75_G4_Pin},
    {HUB75_G5_GPIO_Port,  HUB75_G5_Pin},
    {HUB75_G6_GPIO_Port,  HUB75_G6_Pin},
    {HUB75_G7_GPIO_Port,  HUB75_G7_Pin},
    {HUB75_G8_GPIO_Port,  HUB75_G8_Pin},
    {HUB75_G9_GPIO_Port,  HUB75_G9_Pin},
    {HUB75_G10_GPIO_Port, HUB75_G10_Pin},
};

const hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX] = {
    {HUB75_B1_GPIO_Port,  HUB75_B1_Pin},
    {HUB75_B2_GPIO_Port,  HUB75_B2_Pin},
    {HUB75_B3_GPIO_Port,  HUB75_B3_Pin},
    {HUB75_B4_GPIO_Port,  HUB75_B4_Pin},
    {HUB75_B5_GPIO_Port,  HUB75_B5_Pin},
    {HUB75_B6_GPIO_Port,  HUB75_B6_Pin},
    {HUB75_B7_GPIO_Port,  HUB75_B7_Pin},
    {HUB75_B8_GPIO_Port,  HUB75_B8_Pin},
    {HUB75_B9_GPIO_Port,  HUB75_B9_Pin},
    {HUB75_B10_GPIO_Port, HUB75_B10_Pin},
};

/* ---- 初始化 ---- */
void pl_hub75_init(void)
{
    /* OE 默认关断（高电平 = 输出禁止），LAT/CLK 低电平 */
    HUB75_OE  = 1;
    HUB75_LAT = 0;
    HUB75_CLK = 0;
    HUB75_A = HUB75_B = HUB75_C = HUB75_D = 0;
}
hw_device_initcall(pl_hub75_init);
