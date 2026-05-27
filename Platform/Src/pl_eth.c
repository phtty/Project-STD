#include "main.h"
#include "pl_net_adapt.h"
#include "pl_eth.h"
#include <string.h>
#include "cmsis_os.h"

/* ---- 宏定义 ---- */
#define TIME_WAITING_FOR_INPUT (portMAX_DELAY)  /**< 等待接收数据的超时时间 */
#define ETHIF_TX_TIMEOUT (2000U)                /**< 等待发送完成的超时时间 */
#define INTERFACE_THREAD_STACK_SIZE (256 * 4)   /**< EthIf 线程栈大小 */
#define IFNAME0 's'                              /**< 网络接口名 */
#define IFNAME1 't'

#define ETH_DMA_TRANSMIT_TIMEOUT (20U)          /**< DMA 发送超时 */
#define ETH_TX_BUFFER_MAX        ((ETH_TX_DESC_CNT) * 2U) /**< 最大发送缓冲区数 */

/* ---- 零拷贝实现说明 ----
 * Rx：从 LwIP Rx 内存池分配缓冲区，直接交给 ETH HAL DMA 使用
 * Tx：从 LwIP 内存堆分配缓冲区，直接交给 ETH HAL DMA 使用
 *
 * 约束：
 *   1. ETH DMA 描述符必须连续排列（默认 4 个），
 *      可在 stm32xxxx_hal_conf.h 中重定义 ETH_RX_DESC_CNT / ETH_TX_DESC_CNT
 *   2. Rx 缓冲区数量应在 ETH_RX_DESC_CNT ~ 2*ETH_RX_DESC_CNT 之间
 *      所有 Rx 缓冲区使用相同大小 ETH_RX_BUF_SIZE
 *      地址和大小需对齐到 L1-CACHE 行（32 字节）
 */

/* ---- 类型定义 ---- */
typedef enum {
    RX_ALLOC_OK    = 0x00, /**< Rx 缓冲区分配成功 */
    RX_ALLOC_ERROR = 0x01  /**< Rx 缓冲区分配失败（池耗尽） */
} RxAllocStatusTypeDef;

typedef struct {
    struct pbuf_custom pbuf_custom;
    uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32); /**< 32 字节对齐的 DMA 缓冲区 */
} RxBuff_t;

/* ---- 内存池声明 ---- */
#define ETH_RX_BUFFER_CNT 12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* ---- 变量定义 ---- */
static uint8_t RxAllocStatus;                           /**< Rx 缓冲区分配状态 */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];      /**< ETH Rx DMA 描述符表 */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];      /**< ETH Tx DMA 描述符表 */

osSemaphoreId RxPktSemaphore = NULL;        /**< 收到数据包信号量 */
osSemaphoreId TxPktSemaphore = NULL;        /**< 发送完成信号量 */

ETH_HandleTypeDef heth;                     /**< 全局 ETH 句柄 */
ETH_TxPacketConfig TxConfig;                /**< 发送包配置 */


/* ---- HAL 回调函数 ---- */
void pbuf_free_custom(struct pbuf *p);

/** @brief ETH RX 完成回调：释放信号量通知 ethernetif_input 线程取包 */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
    osSemaphoreRelease(RxPktSemaphore);
}

/** @brief ETH TX 完成回调：释放信号量通知发送完成 */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
    osSemaphoreRelease(TxPktSemaphore);
}

/** @brief ETH DMA 错误回调：RBUS 错误时释放 RX 信号量触发恢复 */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
    if ((HAL_ETH_GetDMAError(handlerEth) & ETH_DMASR_RBUS) == ETH_DMASR_RBUS) {
        osSemaphoreRelease(RxPktSemaphore);
    }
}

/* ================================================================
 *  底层驱动接口（LwIP → ETH MAC）
 * ================================================================ */

static bool g_eth_hw_ready = false; /**< ETH MAC 硬件初始化完成标志 */
static pl_phy_link_fn_t g_phy_get_link = NULL; /**< Device 层注册的 PHY 链路状态查询回调 */

void pl_eth_set_phy_link_fn(pl_phy_link_fn_t fn)
{
    g_phy_get_link = fn;
}

/**
 * @brief   ETH MAC + PHY 硬件初始化（initcall 阶段调用）
 *
 * 执行 HAL_ETH_Init（触发 MspInit 配置 GPIO/时钟/NVIC），不涉及具体 PHY 型号。
 * 幂等：若已初始化则跳过。low_level_init() 中也会检测此标志跳过硬件的重复初始化。
 *
 * @note  PHY 初始化由 Device 层在 dev_eth_init() 中调用 pl_eth_mac_hw_init 后完成。
 *
 * MAC 地址基于芯片 UID 生成（OUI 本地管理位 + 5 字节 UID）。
 */
void pl_eth_mac_hw_init(void)
{
    if (g_eth_hw_ready) return;

    static uint8_t mac[6];
    uint32_t uid0        = HAL_GetUIDw0();
    uint32_t uid1        = HAL_GetUIDw1();
    mac[0]               = 0x02; /* 本地管理地址 */
    mac[1]               = (uid0 >> 24) & 0xFF;
    mac[2]               = (uid0 >> 16) & 0xFF;
    mac[3]               = (uid0 >> 8) & 0xFF;
    mac[4]               = (uid0 >> 0) & 0xFF;
    mac[5]               = (uid1 >> 24) & 0xFF;

    heth.Instance            = ETH;
    heth.Init.MACAddr        = mac;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.TxDesc         = DMATxDscrTab;
    heth.Init.RxDesc         = DMARxDscrTab;
    heth.Init.RxBuffLen      = 1536;

    HAL_ETH_Init(&heth);

    memset(&TxConfig, 0, sizeof(ETH_TxPacketConfig));
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    TxConfig.CRCPadCtrl   = ETH_CRC_PAD_INSERT;

    g_eth_hw_ready = true;
}

/**
 * @brief   LwIP 网络接口初始化（由 netif_add 回调调用）
 *
 * 仅执行 LwIP/RTOS 相关配置：内存池、netif 参数、信号量、EthIf 线程。
 * 硬件初始化由 pl_eth_mac_hw_init() 在 initcall 阶段提前完成，
 * 此处检测 g_eth_hw_ready 做幂等保护。
 */
static void low_level_init(struct netif *netif)
{
    osThreadAttr_t attributes;
    uint32_t duplex, speed = 0;
    int32_t PHYLinkState = 0;
    ETH_MACConfigTypeDef MACConf = {0};

    /* 硬件可能已由 initcall 初始化，幂等保护 */
    if (!g_eth_hw_ready)
        pl_eth_mac_hw_init();

    /* 初始化零拷贝 RX 内存池 */
    LWIP_MEMPOOL_INIT(RX_POOL);

#if LWIP_ARP || LWIP_ETHERNET

    /* 设置 MAC 地址长度 */
    netif->hwaddr_len = ETH_HWADDR_LEN;

    /* 设置 MAC 地址 */
    netif->hwaddr[0] = heth.Init.MACAddr[0];
    netif->hwaddr[1] = heth.Init.MACAddr[1];
    netif->hwaddr[2] = heth.Init.MACAddr[2];
    netif->hwaddr[3] = heth.Init.MACAddr[3];
    netif->hwaddr[4] = heth.Init.MACAddr[4];
    netif->hwaddr[5] = heth.Init.MACAddr[5];

    /* MTU */
    netif->mtu = ETH_MAX_PAYLOAD;

    /* 接受广播和 ARP 流量 */
#if LWIP_ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
#else
    netif->flags |= NETIF_FLAG_BROADCAST;
#endif /* LWIP_ARP */

    /* 创建信号量：RX 用于通知帧到达，TX 用于通知发送完成 */
    RxPktSemaphore = osSemaphoreNew(1, 0, NULL);
    TxPktSemaphore = osSemaphoreNew(1, 0, NULL);

    /* 创建 EthIf 线程处理 MAC 收发 */
    memset(&attributes, 0x0, sizeof(osThreadAttr_t));
    attributes.name       = "EthIf";
    attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
    attributes.priority   = osPriorityRealtime;
    osThreadNew(ethernetif_input, netif, &attributes);

    /* PHY 已在 pl_eth_mac_hw_init() 初始化，此处仅检测链路状态 */
    {
        PHYLinkState = g_phy_get_link();

        /* 根据链路状态配置 MAC 双工/速率 */
        if (PHYLinkState == PL_ETH_LINK_DOWN) {
            netif_set_link_down(netif);
            netif_set_down(netif);
        } else {
            switch (PHYLinkState) {
                case PL_ETH_LINK_100M_FD:
                    duplex = ETH_FULLDUPLEX_MODE;
                    speed  = ETH_SPEED_100M;
                    break;
                case PL_ETH_LINK_100M_HD:
                    duplex = ETH_HALFDUPLEX_MODE;
                    speed  = ETH_SPEED_100M;
                    break;
                case PL_ETH_LINK_10M_FD:
                    duplex = ETH_FULLDUPLEX_MODE;
                    speed  = ETH_SPEED_10M;
                    break;
                case PL_ETH_LINK_10M_HD:
                    duplex = ETH_HALFDUPLEX_MODE;
                    speed  = ETH_SPEED_10M;
                    break;
                default:
                    duplex = ETH_FULLDUPLEX_MODE;
                    speed  = ETH_SPEED_100M;
                    break;
            }

            /* 更新 MAC 配置并启动 */
            HAL_ETH_GetMACConfig(&heth, &MACConf);
            MACConf.DuplexMode = duplex;
            MACConf.Speed      = speed;
            HAL_ETH_SetMACConfig(&heth, &MACConf);

            HAL_ETH_Start_IT(&heth);
            netif_set_up(netif);
            netif_set_link_up(netif);
        }
    }
#endif /* LWIP_ARP || LWIP_ETHERNET */
}

/**
 * @brief   底层发送：将 pbuf 链式数据通过 DMA 发送到 ETH MAC
 * @param   netif  LwIP 网络接口
 * @param   p      待发送的 pbuf 链
 * @return  ERR_OK 发送成功；ERR_BUF DMA 忙时等待重试；ERR_IF 其他错误
 *
 * @note    若 DMA 队列满返回 ERR_MEM 可能导致协议栈行为异常，
 *          本实现通过 TxPktSemaphore 等待描述符可用后重试。
 */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    uint32_t i                                  = 0U;
    struct pbuf *q                              = NULL;
    err_t errval                                = ERR_OK;
    ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};

    memset(Txbuffer, 0, ETH_TX_DESC_CNT * sizeof(ETH_BufferTypeDef));

    /* 遍历 pbuf 链，填充 DMA 发送缓冲区描述符 */
    for (q = p; q != NULL; q = q->next) {
        if (i >= ETH_TX_DESC_CNT)
            return ERR_IF;

        Txbuffer[i].buffer = q->payload;
        Txbuffer[i].len    = q->len;

        if (i > 0) {
            Txbuffer[i - 1].next = &Txbuffer[i];
        }

        if (q->next == NULL) {
            Txbuffer[i].next = NULL;
        }

        i++;
    }

    TxConfig.Length   = p->tot_len;
    TxConfig.TxBuffer = Txbuffer;
    TxConfig.pData    = p;

    pbuf_ref(p);

    do {
        if (HAL_ETH_Transmit_IT(&heth, &TxConfig) == HAL_OK) {
            errval = ERR_OK;
        } else {
            if (HAL_ETH_GetError(&heth) & HAL_ETH_ERROR_BUSY) {
                /* DMA 忙，等待描述符释放后重试 */
                osSemaphoreAcquire(TxPktSemaphore, ETHIF_TX_TIMEOUT);
                HAL_ETH_ReleaseTxPacket(&heth);
                errval = ERR_BUF;
            } else {
                /* 不可恢复的错误 */
                pbuf_free(p);
                errval = ERR_IF;
            }
        }
    } while (errval == ERR_BUF);

    return errval;
}

/**
 * @brief   底层接收：从 ETH HAL 读取已分配的 pbuf
 * @param   netif  LwIP 网络接口
 * @return  指向接收包的 pbuf 指针；NULL 表示无数据或内存分配失败
 */
static struct pbuf *low_level_input(struct netif *netif)
{
    struct pbuf *p = NULL;

    if (RxAllocStatus == RX_ALLOC_OK) {
        HAL_ETH_ReadData(&heth, (void **)&p);
    }

    return p;
}

/**
 * @brief   EthIf 线程：阻塞等待 RX 信号量，取出 pbuf 投递给 LwIP 协议栈
 * @param   argument  指向 netif 结构的指针
 *
 * 循环内：
 *   1. osSemaphoreAcquire 等待帧到达
 *   2. low_level_input 提取 pbuf
 *   3. netif->input 投递到 LwIP 协议栈
 *   4. 如果池耗尽（RxAllocStatus == ERROR），HAL 回调会释放信号量触发本线程
 *      → 调用 HAL_ETH_GetRxDataBuffer 重建描述符
 */
void ethernetif_input(void *argument)
{
    struct pbuf *p      = NULL;
    struct netif *netif = (struct netif *)argument;

    for (;;) {
        if (osSemaphoreAcquire(RxPktSemaphore, TIME_WAITING_FOR_INPUT) == osOK) {
            do {
                p = low_level_input(netif);
                if (p != NULL) {
                    if (netif->input(p, netif) != ERR_OK) {
                        pbuf_free(p);
                    }
                }
            } while (p != NULL);
        }
    }
}

#if !LWIP_ARP
/** @brief ARP 关闭时的自定义输出函数（当前为占位实现） */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
    err_t errval;
    errval = ERR_OK;

    return errval;
}
#endif /* LWIP_ARP */

/**
 * @brief   LwIP 网络接口注册入口（传递给 netif_add 的回调）
 * @param   netif  LwIP 网络接口
 * @return  ERR_OK 初始化成功；ERR_MEM 内存不足
 *
 * 设置 name、output/linkoutput 函数指针，调用 low_level_init 初始化硬件。
 */
err_t ethernetif_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

    // MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;

    /* output 直接使用 etharp_output，省去一次函数调用 */
#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
    netif->output = etharp_output;
#else
    netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

    netif->linkoutput = low_level_output;

    /* 初始化底层硬件及 RTOS 资源 */
    low_level_init(netif);

    return ERR_OK;
}

/**
 * @brief   自定义 pbuf 释放回调：将 Rx 缓冲区归还内存池
 * @param   p  待释放的 pbuf
 *
 * 若此前因 Rx 池耗尽触发错误，归还缓冲区后释放 RX 信号量
 * 通知 ethernetif_input 线程调用 HAL_ETH_GetRxDataBuffer 重建描述符。
 */
void pbuf_free_custom(struct pbuf *p)
{
    struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;
    LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

    if (RxAllocStatus == RX_ALLOC_ERROR) {
        RxAllocStatus = RX_ALLOC_OK;
        osSemaphoreRelease(RxPktSemaphore);
    }
}

/**
 * @brief   获取当前系统毫秒时间戳（供 LwIP 定时器使用）
 * @return  系统启动以来的毫秒数
 */
u32_t sys_now(void)
{
    return HAL_GetTick();
}

/* ================================================================
 *  ETH MSP 初始化与反初始化（HAL 回调）
 * ================================================================ */

/**
 * @brief   ETH 外设 MSP 初始化：使能时钟、配置 GPIO 复用、使能 NVIC
 * @param   ethHandle  ETH 句柄（HAL_ETH_Init 自动传入）
 *
 * RMII 引脚映射：
 *   PC1  → ETH_MDC       PA1  → ETH_REF_CLK
 *   PA2  → ETH_MDIO      PA7  → ETH_CRS_DV
 *   PC4  → ETH_RXD0      PC5  → ETH_RXD1
 *   PG11 → ETH_TX_EN     PG13 → ETH_TXD0
 *   PG14 → ETH_TXD1
 */
void HAL_ETH_MspInit(ETH_HandleTypeDef *ethHandle)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if (ethHandle->Instance == ETH) {
        /* 使能外设时钟 */
        __HAL_RCC_ETH_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOG_CLK_ENABLE();

        /* GPIOC：MDC、RXD0、RXD1 */
        GPIO_InitStruct.Pin       = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
        HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

        /* GPIOA：REF_CLK、MDIO、CRS_DV */
        GPIO_InitStruct.Pin       = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* GPIOG：TX_EN、TXD0、TXD1 */
        GPIO_InitStruct.Pin       = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
        HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

        /* NVIC 中断配置：ETH IRQ 优先级 5 */
        HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(ETH_IRQn);
    }
}

/** @brief ETH 外设 MSP 反初始化：关闭时钟、回收 GPIO、关闭 NVIC */
void HAL_ETH_MspDeInit(ETH_HandleTypeDef *ethHandle)
{
    if (ethHandle->Instance == ETH) {
        /* 关闭外设时钟 */
        __HAL_RCC_ETH_CLK_DISABLE();

        /* 回收 ETH 相关 GPIO */
        HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
        HAL_GPIO_DeInit(GPIOG, GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14);

        /* 关闭 NVIC */
        HAL_NVIC_DisableIRQ(ETH_IRQn);
    }
}

/* ================================================================
 *  PHY MDIO 接口函数
 * ================================================================ */

/** @brief 初始化 MDIO 接口（配置 MDIO 时钟分频） */
int32_t pl_eth_phy_io_init(void)
{
    /* MDIO GPIO 配置已在 HAL_ETH_MspInit 中完成 */
    HAL_ETH_SetMDIOClockRange(&heth);
    return 0;
}

/** @brief 反初始化 MDIO 接口（当前为空操作） */
int32_t pl_eth_phy_io_deinit(void)
{
    return 0;
}

/** @brief 通过 MDIO 读取 PHY 寄存器 */
int32_t pl_eth_phy_io_read_reg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
    if (HAL_ETH_ReadPHYRegister(&heth, DevAddr, RegAddr, pRegVal) != HAL_OK) {
        return -1;
    }
    return 0;
}

/** @brief 通过 MDIO 写入 PHY 寄存器 */
int32_t pl_eth_phy_io_write_reg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
    if (HAL_ETH_WritePHYRegister(&heth, DevAddr, RegAddr, RegVal) != HAL_OK) {
        return -1;
    }
    return 0;
}

/** @brief 获取系统时间戳（供 Device 层 PHY 驱动内部定时使用） */
int32_t pl_eth_phy_io_get_tick(void)
{
    return HAL_GetTick();
}

/**
 * @brief   链路状态监控线程：每 100ms 轮询 PHY 链路状态，更新 MAC 和 netif
 * @param   argument  指向 netif 结构的指针
 *
 * 检测链路变化：
 *   - 链路断开 → 停止 ETH、netif down
 *   - 链路恢复 → 根据 PHY 协商结果配置 MAC 速率/双工、启动 ETH、netif up
 */
void ethernet_link_thread(void *argument)
{
    ETH_MACConfigTypeDef MACConf = {0};
    int32_t PHYLinkState         = 0;
    uint32_t linkchanged = 0U, speed = 0U, duplex = 0U;

    struct netif *netif = (struct netif *)argument;

    for (;;) {
        PHYLinkState = g_phy_get_link();

        if (netif_is_link_up(netif) && (PHYLinkState == PL_ETH_LINK_DOWN)) {
            /* 链路断开 */
            HAL_ETH_Stop_IT(&heth);
            netif_set_down(netif);
            netif_set_link_down(netif);
        } else if (!netif_is_link_up(netif) && (PHYLinkState > PL_ETH_LINK_DOWN)) {
            /* 链路恢复，获取协商结果 */
            switch (PHYLinkState) {
                case PL_ETH_LINK_100M_FD:
                    duplex      = ETH_FULLDUPLEX_MODE;
                    speed       = ETH_SPEED_100M;
                    linkchanged = 1;
                    break;
                case PL_ETH_LINK_100M_HD:
                    duplex      = ETH_HALFDUPLEX_MODE;
                    speed       = ETH_SPEED_100M;
                    linkchanged = 1;
                    break;
                case PL_ETH_LINK_10M_FD:
                    duplex      = ETH_FULLDUPLEX_MODE;
                    speed       = ETH_SPEED_10M;
                    linkchanged = 1;
                    break;
                case PL_ETH_LINK_10M_HD:
                    duplex      = ETH_HALFDUPLEX_MODE;
                    speed       = ETH_SPEED_10M;
                    linkchanged = 1;
                    break;
                default:
                    break;
            }

            if (linkchanged) {
                HAL_ETH_GetMACConfig(&heth, &MACConf);
                MACConf.DuplexMode = duplex;
                MACConf.Speed      = speed;
                HAL_ETH_SetMACConfig(&heth, &MACConf);
                HAL_ETH_Start_IT(&heth);
                    netif_set_up(netif);
                netif_set_link_up(netif);
            }
        }

        osDelay(100);
    }
}

/**
 * @brief   ETH HAL Rx 缓冲区分配回调（零拷贝模式）
 *
 * 从 RX_POOL 内存池分配一个含 pbuf 的 RxBuff_t，
 * 设置自定义释放函数 pbuf_free_custom。
 * 若池耗尽，标记 RxAllocStatus 为 ERROR，HAL_ETH_RxCpltCallback 会触发恢复。
 */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
    if (p) {
        *buff                   = (uint8_t *)p + offsetof(RxBuff_t, buff);
        p->custom_free_function = pbuf_free_custom;
        /* pbuf_alloced_custom 必须在每次分配后调用，因为 LwIP/app 可能修改 ref 等字段 */
        pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);
    } else {
        RxAllocStatus = RX_ALLOC_ERROR;
        *buff         = NULL;
    }
}

/**
 * @brief   ETH HAL Rx 链接回调：将零拷贝缓冲区链接为 pbuf 链
 *
 * 每次 DMA 完成一个分片时调用，将 buffer 转为 pbuf 并链接到链尾。
 * 最后遍历整个链更新 tot_len。
 */
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
    struct pbuf **ppStart = (struct pbuf **)pStart;
    struct pbuf **ppEnd   = (struct pbuf **)pEnd;
    struct pbuf *p        = NULL;

    /* 从 buff 地址反推 pbuf 指针 */
    p          = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
    p->next    = NULL;
    p->tot_len = 0;
    p->len     = Length;

    /* 链接到链尾 */
    if (!*ppStart) {
        *ppStart = p;        /* 首帧 */
    } else {
        (*ppEnd)->next = p;  /* 追加到链尾 */
    }
    *ppEnd = p;

    /* 更新链上所有 pbuf 的 tot_len */
    for (p = *ppStart; p != NULL; p = p->next) {
        p->tot_len += Length;
    }
}

/** @brief ETH HAL Tx 释放回调：发送完成后释放 pbuf */
void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
    pbuf_free((struct pbuf *)buff);
}
