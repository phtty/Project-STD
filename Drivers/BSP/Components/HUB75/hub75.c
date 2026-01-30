#include "hub75.h"
#include "main.h"

void bsp_init_hub75(void)
{
    // 初始化控制信号
    HUB75_OE  = 1;
    HUB75_LAT = 0;
    HUB75_CLK = 0;

    // 初始化地址通道
    HUB75_A = 0;
    HUB75_B = 0;
    HUB75_C = 0;
    HUB75_D = 0;
}
