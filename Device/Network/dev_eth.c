/**
 * @file        dev_eth.c
 * @brief       以太网板级设备驱动（DP83848 PHY）
 *
 * 板级网络配置（IP/掩码/网关）+ DP83848 PHY 初始化在此完成。
 * 依赖方向：Device → Platform（调用 pl_eth_mac_hw_init / pl_net_init）。
 *
 * initcall: dev_eth_init() → pl_eth_mac_hw_init() (ETH MAC/DMA) + DP83848 初始化 (PHY)
 *                          → DP83848 PHY 初始化和链路回调注册
 * RTOS:     dev_eth_start() → pl_net_init(ip, mask, gw) (LwIP + netif)
 */

#include "dev_eth.h"
#include "dev_dp83848.h"
#include "pl_eth.h"
#include "pl_net.h"
#include "pl_dwt.h"
#include "initcall.h"

/* 板级默认网络配置: 192.168.2.100/24, 网关 192.168.1.1 */
// static const uint8_t g_default_ip[4]   = {192, 168, 1, 100};
static const uint8_t g_default_ip[4]   = {192, 168, 2, 100};
static const uint8_t g_default_mask[4] = {255, 255, 255, 0};
// static const uint8_t g_default_gw[4]   = {192, 168, 1, 1};
static const uint8_t g_default_gw[4] = {192, 168, 2, 1};

/* DP83848 PHY 设备对象与 IO 上下文 */
static dev_dp83848_obj_t g_dp83848;
static dev_dp83848_io_ctx_t g_dev_dp83848_io_ctx = {
    pl_eth_phy_io_init,
    pl_eth_phy_io_deinit,
    pl_eth_phy_io_write_reg,
    pl_eth_phy_io_read_reg,
    pl_eth_phy_io_get_tick,
};

/**
 * @brief  DP83848 链路状态 → Platform 层抽象枚举的转换
 */
static pl_eth_link_state_t dev_phy_get_link_state(void)
{
    int32_t state = dev_dp83848_link_state_get(&g_dp83848);

    switch (state) {
        case DP83848_STATUS_100MBITS_FULLDUPLEX:
            return PL_ETH_LINK_100M_FD;
        case DP83848_STATUS_100MBITS_HALFDUPLEX:
            return PL_ETH_LINK_100M_HD;
        case DP83848_STATUS_10MBITS_FULLDUPLEX:
            return PL_ETH_LINK_10M_FD;
        case DP83848_STATUS_10MBITS_HALFDUPLEX:
            return PL_ETH_LINK_10M_HD;
        case DP83848_STATUS_LINK_DOWN:
        case DP83848_STATUS_AUTONEGO_NOTDONE:
        default:
            return PL_ETH_LINK_DOWN;
    }
}

void dev_eth_init(void)
{
    extern ETH_HandleTypeDef heth;

    /* ETH MAC 硬件初始化（Platform 层，含 MDIO 接口） */
    pl_eth_mac_hw_init();

    /* 轮询 PHY 是否就绪（PHYIDR1 寄存器的 OUI 部分不为 0xFFFF/0x0000 即正常） */
    /* 冷启动时 PHY 晶振起振需要时间，固定延时不可靠，轮询保证不迟不早 */
    for (uint32_t i = 0; i < 200; i++) {
        uint32_t phy_id;
        HAL_ETH_ReadPHYRegister(&heth, 1, 2, &phy_id);
        if (phy_id != 0xFFFF && phy_id != 0x0000)
            break;
        pl_delay_ms(50); /* 50ms × 200 = 最多 10s */
    }

    /* DP83848 PHY 初始化（Device 层，板级组件） */
    dev_dp83848_register_bus_io(&g_dp83848, &g_dev_dp83848_io_ctx);
    if (dev_dp83848_init(&g_dp83848) != DP83848_STATUS_OK)
        Error_Handler();

    /* 注册 PHY 链路状态查询回调，供 Platform 层 low_level_init / ethernet_link_thread 使用 */
    pl_eth_set_phy_link_fn(dev_phy_get_link_state);
}
hw_dev_initcall(dev_eth_init);

void dev_eth_start(void)
{
    pl_net_init(g_default_ip, g_default_mask, g_default_gw);
}
