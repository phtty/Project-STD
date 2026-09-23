#pragma once

/**
 * @file    dev_dp83848.h
 * @brief   DP83848 以太网 PHY：寄存器定义与驱动接口
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---- 寄存器地址 ---- */
#define DP83848_BCR     ((uint16_t)0x0000U)
#define DP83848_BSR     ((uint16_t)0x0001U)
#define DP83848_PHYI1R  ((uint16_t)0x0002U)
#define DP83848_PHYI2R  ((uint16_t)0x0003U)
#define DP83848_ANAR    ((uint16_t)0x0004U)
#define DP83848_ANLPAR  ((uint16_t)0x0005U)
#define DP83848_ANER    ((uint16_t)0x0006U)
#define DP83848_ANNPTR  ((uint16_t)0x0007U)
#define DP83848_SMR     ((uint16_t)0x0019U)
#define DP83848_ISFR    ((uint16_t)0x0012U)
#define DP83848_IMR     ((uint16_t)0x0011U)
#define DP83848_PHYSCSR ((uint16_t)0x0010U)

/* ---- BCR 位定义 ---- */
#define DP83848_BCR_SOFT_RESET       ((uint16_t)0x8000U)
#define DP83848_BCR_LOOPBACK         ((uint16_t)0x4000U)
#define DP83848_BCR_SPEED_SELECT     ((uint16_t)0x2000U)
#define DP83848_BCR_AUTONEGO_EN      ((uint16_t)0x1000U)
#define DP83848_BCR_POWER_DOWN       ((uint16_t)0x0800U)
#define DP83848_BCR_ISOLATE          ((uint16_t)0x0400U)
#define DP83848_BCR_RESTART_AUTONEGO ((uint16_t)0x0200U)
#define DP83848_BCR_DUPLEX_MODE      ((uint16_t)0x0100U)

/* ---- BSR 位定义 ---- */
#define DP83848_BSR_100BASE_T4       ((uint16_t)0x8000U)
#define DP83848_BSR_100BASE_TX_FD    ((uint16_t)0x4000U)
#define DP83848_BSR_100BASE_TX_HD    ((uint16_t)0x2000U)
#define DP83848_BSR_10BASE_T_FD      ((uint16_t)0x1000U)
#define DP83848_BSR_10BASE_T_HD      ((uint16_t)0x0800U)
#define DP83848_BSR_MF_PREAMBLE      ((uint16_t)0x0040U)
#define DP83848_BSR_AUTONEGO_CPLT    ((uint16_t)0x0020U)
#define DP83848_BSR_REMOTE_FAULT     ((uint16_t)0x0010U)
#define DP83848_BSR_AUTONEGO_ABILITY ((uint16_t)0x0008U)
#define DP83848_BSR_LINK_STATUS      ((uint16_t)0x0004U)
#define DP83848_BSR_JABBER_DETECT    ((uint16_t)0x0002U)
#define DP83848_BSR_EXTENDED_CAP     ((uint16_t)0x0001U)

#define DP83848_PHYI1R_OUI_3_18 ((uint16_t)0xFFFFU)

/* ---- PHYI2R 位定义 ---- */
#define DP83848_PHYI2R_OUI_19_24    ((uint16_t)0xFC00U)
#define DP83848_PHYI2R_MODEL_NBR    ((uint16_t)0x03F0U)
#define DP83848_PHYI2R_REVISION_NBR ((uint16_t)0x000FU)

/* ---- ANAR 位定义 ---- */
#define DP83848_ANAR_NEXT_PAGE            ((uint16_t)0x8000U)
#define DP83848_ANAR_REMOTE_FAULT         ((uint16_t)0x2000U)
#define DP83848_ANAR_PAUSE_OPERATION      ((uint16_t)0x0C00U)
#define DP83848_ANAR_PO_NOPAUSE           ((uint16_t)0x0000U)
#define DP83848_ANAR_PO_SYMMETRIC_PAUSE   ((uint16_t)0x0400U)
#define DP83848_ANAR_PO_ASYMMETRIC_PAUSE  ((uint16_t)0x0800U)
#define DP83848_ANAR_PO_ADVERTISE_SUPPORT ((uint16_t)0x0C00U)
#define DP83848_ANAR_100BASE_TX_FD        ((uint16_t)0x0100U)
#define DP83848_ANAR_100BASE_TX           ((uint16_t)0x0080U)
#define DP83848_ANAR_10BASE_T_FD          ((uint16_t)0x0040U)
#define DP83848_ANAR_10BASE_T             ((uint16_t)0x0020U)
#define DP83848_ANAR_SELECTOR_FIELD       ((uint16_t)0x000FU)

/* ---- ANLPAR 位定义 ---- */
#define DP83848_ANLPAR_NEXT_PAGE            ((uint16_t)0x8000U)
#define DP83848_ANLPAR_REMOTE_FAULT         ((uint16_t)0x2000U)
#define DP83848_ANLPAR_PAUSE_OPERATION      ((uint16_t)0x0C00U)
#define DP83848_ANLPAR_PO_NOPAUSE           ((uint16_t)0x0000U)
#define DP83848_ANLPAR_PO_SYMMETRIC_PAUSE   ((uint16_t)0x0400U)
#define DP83848_ANLPAR_PO_ASYMMETRIC_PAUSE  ((uint16_t)0x0800U)
#define DP83848_ANLPAR_PO_ADVERTISE_SUPPORT ((uint16_t)0x0C00U)
#define DP83848_ANLPAR_100BASE_TX_FD        ((uint16_t)0x0100U)
#define DP83848_ANLPAR_100BASE_TX           ((uint16_t)0x0080U)
#define DP83848_ANLPAR_10BASE_T_FD          ((uint16_t)0x0040U)
#define DP83848_ANLPAR_10BASE_T             ((uint16_t)0x0020U)
#define DP83848_ANLPAR_SELECTOR_FIELD       ((uint16_t)0x000FU)

/* ---- ANER 位定义 ---- */
#define DP83848_ANER_RX_NP_LOCATION_ABLE    ((uint16_t)0x0040U)
#define DP83848_ANER_RX_NP_STORAGE_LOCATION ((uint16_t)0x0020U)
#define DP83848_ANER_PARALLEL_DETECT_FAULT  ((uint16_t)0x0010U)
#define DP83848_ANER_LP_NP_ABLE             ((uint16_t)0x0008U)
#define DP83848_ANER_NP_ABLE                ((uint16_t)0x0004U)
#define DP83848_ANER_PAGE_RECEIVED          ((uint16_t)0x0002U)
#define DP83848_ANER_LP_AUTONEG_ABLE        ((uint16_t)0x0001U)

/* ---- ANNPTR 位定义 ---- */
#define DP83848_ANNPTR_NEXT_PAGE    ((uint16_t)0x8000U)
#define DP83848_ANNPTR_MESSAGE_PAGE ((uint16_t)0x2000U)
#define DP83848_ANNPTR_ACK2         ((uint16_t)0x1000U)
#define DP83848_ANNPTR_TOGGLE       ((uint16_t)0x0800U)
#define DP83848_ANNPTR_MESSAGE_CODE ((uint16_t)0x07FFU)

/* ---- ANNPRR 位定义 ---- */
#define DP83848_ANNPRR_NEXT_PAGE    ((uint16_t)0x8000U)
#define DP83848_ANNPRR_ACK          ((uint16_t)0x4000U)
#define DP83848_ANNPRR_MESSAGE_PAGE ((uint16_t)0x2000U)
#define DP83848_ANNPRR_ACK2         ((uint16_t)0x1000U)
#define DP83848_ANNPRR_TOGGLE       ((uint16_t)0x0800U)
#define DP83848_ANNPRR_MESSAGE_CODE ((uint16_t)0x07FFU)

/* ---- MMDACR 位定义 ---- */
#define DP83848_MMDACR_MMD_FUNCTION      ((uint16_t)0xC000U)
#define DP83848_MMDACR_MMD_FUNCTION_ADDR ((uint16_t)0x0000U)
#define DP83848_MMDACR_MMD_FUNCTION_DATA ((uint16_t)0x4000U)
#define DP83848_MMDACR_MMD_DEV_ADDR      ((uint16_t)0x001FU)

/* ---- ENCTR 位定义 ---- */
#define DP83848_ENCTR_TX_ENABLE             ((uint16_t)0x8000U)
#define DP83848_ENCTR_TX_TIMER              ((uint16_t)0x6000U)
#define DP83848_ENCTR_TX_TIMER_1S           ((uint16_t)0x0000U)
#define DP83848_ENCTR_TX_TIMER_768MS        ((uint16_t)0x2000U)
#define DP83848_ENCTR_TX_TIMER_512MS        ((uint16_t)0x4000U)
#define DP83848_ENCTR_TX_TIMER_265MS        ((uint16_t)0x6000U)
#define DP83848_ENCTR_RX_ENABLE             ((uint16_t)0x1000U)
#define DP83848_ENCTR_RX_MAX_INTERVAL       ((uint16_t)0x0C00U)
#define DP83848_ENCTR_RX_MAX_INTERVAL_64MS  ((uint16_t)0x0000U)
#define DP83848_ENCTR_RX_MAX_INTERVAL_256MS ((uint16_t)0x0400U)
#define DP83848_ENCTR_RX_MAX_INTERVAL_512MS ((uint16_t)0x0800U)
#define DP83848_ENCTR_RX_MAX_INTERVAL_1S    ((uint16_t)0x0C00U)
#define DP83848_ENCTR_EX_CROSS_OVER         ((uint16_t)0x0002U)
#define DP83848_ENCTR_EX_MANUAL_CROSS_OVER  ((uint16_t)0x0001U)

/* ---- MCSR 位定义 ---- */
#define DP83848_MCSR_EDPWRDOWN   ((uint16_t)0x2000U)
#define DP83848_MCSR_FARLOOPBACK ((uint16_t)0x0200U)
#define DP83848_MCSR_ALTINT      ((uint16_t)0x0040U)
#define DP83848_MCSR_ENERGYON    ((uint16_t)0x0002U)

/* ---- SMR 位定义 ---- */
#define DP83848_SMR_MODE     ((uint16_t)0x00E0U)
#define DP83848_SMR_PHY_ADDR ((uint16_t)0x001FU)

/* ---- TPDCR 位定义 ---- */
#define DP83848_TPDCR_DELAY_IN           ((uint16_t)0x8000U)
#define DP83848_TPDCR_LINE_BREAK_COUNTER ((uint16_t)0x7000U)
#define DP83848_TPDCR_PATTERN_HIGH       ((uint16_t)0x0FC0U)
#define DP83848_TPDCR_PATTERN_LOW        ((uint16_t)0x003FU)

/* ---- TCSR 位定义 ---- */
#define DP83848_TCSR_TDR_ENABLE           ((uint16_t)0x8000U)
#define DP83848_TCSR_TDR_AD_FILTER_ENABLE ((uint16_t)0x4000U)
#define DP83848_TCSR_TDR_CH_CABLE_TYPE    ((uint16_t)0x0600U)
#define DP83848_TCSR_TDR_CH_CABLE_DEFAULT ((uint16_t)0x0000U)
#define DP83848_TCSR_TDR_CH_CABLE_SHORTED ((uint16_t)0x0200U)
#define DP83848_TCSR_TDR_CH_CABLE_OPEN    ((uint16_t)0x0400U)
#define DP83848_TCSR_TDR_CH_CABLE_MATCH   ((uint16_t)0x0600U)
#define DP83848_TCSR_TDR_CH_STATUS        ((uint16_t)0x0100U)
#define DP83848_TCSR_TDR_CH_LENGTH        ((uint16_t)0x00FFU)

/* ---- SCSIR 位定义 ---- */
#define DP83848_SCSIR_AUTO_MDIX_ENABLE ((uint16_t)0x8000U)
#define DP83848_SCSIR_CHANNEL_SELECT   ((uint16_t)0x2000U)
#define DP83848_SCSIR_SQE_DISABLE      ((uint16_t)0x0800U)
#define DP83848_SCSIR_XPOLALITY        ((uint16_t)0x0010U)

/* ---- CLR 位定义 ---- */
#define DP83848_CLR_CABLE_LENGTH ((uint16_t)0xF000U)

/* ---- IMR/ISFR 中断位定义 ---- */
#define DP83848_INT_8 ((uint16_t)0x0100U)
#define DP83848_INT_7 ((uint16_t)0x0080U)
#define DP83848_INT_6 ((uint16_t)0x0040U)
#define DP83848_INT_5 ((uint16_t)0x0020U)
#define DP83848_INT_4 ((uint16_t)0x0010U)
#define DP83848_INT_3 ((uint16_t)0x0008U)
#define DP83848_INT_2 ((uint16_t)0x0004U)
#define DP83848_INT_1 ((uint16_t)0x0002U)

/* ---- PHYSCSR 位定义 ---- */
#define DP83848_PHYSCSR_AUTONEGO_DONE ((uint16_t)0x010U)
#define DP83848_PHYSCSR_HCDSPEEDMASK  ((uint16_t)0x006U)
#define DP83848_PHYSCSR_10BT_HD       ((uint16_t)0x002U)
#define DP83848_PHYSCSR_10BT_FD       ((uint16_t)0x006U)
#define DP83848_PHYSCSR_100BTX_HD     ((uint16_t)0x000U)
#define DP83848_PHYSCSR_100BTX_FD     ((uint16_t)0x004U)

/* ---- 状态码 ---- */
#define DP83848_STATUS_READ_ERROR          ((int32_t)-5)
#define DP83848_STATUS_WRITE_ERROR         ((int32_t)-4)
#define DP83848_STATUS_ADDRESS_ERROR       ((int32_t)-3)
#define DP83848_STATUS_RESET_TIMEOUT       ((int32_t)-2)
#define DP83848_STATUS_ERROR               ((int32_t)-1)
#define DP83848_STATUS_OK                  ((int32_t) 0)
#define DP83848_STATUS_LINK_DOWN           ((int32_t) 1)
#define DP83848_STATUS_100MBITS_FULLDUPLEX ((int32_t) 2)
#define DP83848_STATUS_100MBITS_HALFDUPLEX ((int32_t) 3)
#define DP83848_STATUS_10MBITS_FULLDUPLEX  ((int32_t) 4)
#define DP83848_STATUS_10MBITS_HALFDUPLEX  ((int32_t) 5)
#define DP83848_STATUS_AUTONEGO_NOTDONE    ((int32_t) 6)

/* ---- 中断标志 ---- */
#define DP83848_WOL_IT                      DP83848_INT_8
#define DP83848_ENERGYON_IT                 DP83848_INT_7
#define DP83848_AUTONEGO_COMPLETE_IT        DP83848_INT_6
#define DP83848_REMOTE_FAULT_IT             DP83848_INT_5
#define DP83848_LINK_DOWN_IT                DP83848_INT_4
#define DP83848_AUTONEGO_LP_ACK_IT          DP83848_INT_3
#define DP83848_PARALLEL_DETECTION_FAULT_IT DP83848_INT_2
#define DP83848_AUTONEGO_PAGE_RECEIVED_IT   DP83848_INT_1

/* ---- 类型定义 ---- */

/** @brief PHY 初始化回调（配置 GPIO/时钟等底层资源） */
typedef int32_t (*dev_dp83848_init_fn_t)(void);

/** @brief PHY 反初始化回调（释放底层资源） */
typedef int32_t (*dev_dp83848_deinit_fn_t)(void);

/** @brief PHY 寄存器读取回调；读到的值写入 *reg_val */
typedef int32_t (*dev_dp83848_read_reg_fn_t)(uint32_t dev_addr, uint32_t reg_addr, uint32_t *reg_val);

/** @brief PHY 寄存器写入回调 */
typedef int32_t (*dev_dp83848_write_reg_fn_t)(uint32_t dev_addr, uint32_t reg_addr, uint32_t reg_val);

/** @brief 毫秒节拍获取回调（供驱动内部超时使用） */
typedef int32_t (*dev_dp83848_get_tick_fn_t)(void);

/** @brief DP83848 的 IO 操作函数集（按所用总线由调用方实现并注册） */
typedef struct {
    dev_dp83848_init_fn_t      init;       /**< 初始化回调；可为 NULL */
    dev_dp83848_deinit_fn_t    deinit;     /**< 反初始化回调；可为 NULL */
    dev_dp83848_write_reg_fn_t write_reg;  /**< 写寄存器回调；必填 */
    dev_dp83848_read_reg_fn_t  read_reg;   /**< 读寄存器回调；必填 */
    dev_dp83848_get_tick_fn_t  get_tick;   /**< 取节拍回调；必填 */
} dev_dp83848_io_ctx_t;

/** @brief DP83848 设备对象（由调用方分配，驱动只持有指针） */
typedef struct {
    uint32_t             dev_addr;       /**< 探测到的 PHY 地址 */
    uint32_t             is_initialized; /**< 初始化完成标志，非 0 表示已初始化 */
    dev_dp83848_io_ctx_t io;             /**< IO 操作函数集 */
    void                *p_data;         /**< 调用方私有数据指针，驱动不解释 */
} dev_dp83848_obj_t;

/* ---- API ---- */

/**
 * @brief 向 PHY 设备对象注册 IO 操作函数
 * @param[out] obj    待填充的设备对象；本函数写入其 io 成员
 * @param io_ctx      IO 操作函数集（读/写寄存器、取节拍等）
 * @return DP83848_STATUS_OK 注册成功；DP83848_STATUS_ERROR 缺失必要回调
 */
int32_t dev_dp83848_register_bus_io(dev_dp83848_obj_t *obj, dev_dp83848_io_ctx_t *io_ctx);

/**
 * @brief 初始化 DP83848 PHY 并扫描设备地址
 * @param[in,out] obj  设备对象；读出 io 回调，写入 dev_addr 与 is_initialized
 * @return 成功返回 DP83848_STATUS_OK；地址未找到或寄存器读取失败返回负错误码
 */
int32_t dev_dp83848_init(dev_dp83848_obj_t *obj);

/**
 * @brief 反初始化 DP83848，释放硬件资源
 * @param[in,out] obj  设备对象；读/写其 is_initialized 标志
 * @return DP83848_STATUS_OK 成功；DP83848_STATUS_ERROR 反初始化失败
 */
int32_t dev_dp83848_deinit(dev_dp83848_obj_t *obj);

/**
 * @brief 退出 PHY 省电模式
 * @param obj  设备对象
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_power_down_disable(dev_dp83848_obj_t *obj);

/**
 * @brief 进入 PHY 省电模式
 * @param obj  设备对象
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_power_down_enable(dev_dp83848_obj_t *obj);

/**
 * @brief 启动自动协商
 * @param obj  设备对象
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_autonego_start(dev_dp83848_obj_t *obj);

/**
 * @brief 获取 DP83848 当前链路状态（速率、双工模式）
 * @param obj  设备对象
 * @return 100M/10M × 全/半双工之一；链路断开、自协商未完成或读失败返回
 *         对应错误码
 */
int32_t dev_dp83848_link_state_get(dev_dp83848_obj_t *obj);

/**
 * @brief 手动设置 DP83848 链路速率和双工模式（关闭自动协商）
 * @param obj        设备对象
 * @param link_state 目标链路状态：DP83848_STATUS_100MBITS_FULLDUPLEX、
 *                   DP83848_STATUS_100MBITS_HALFDUPLEX、
 *                   DP83848_STATUS_10MBITS_FULLDUPLEX、
 *                   DP83848_STATUS_10MBITS_HALFDUPLEX 之一
 * @return DP83848_STATUS_OK 成功；DP83848_STATUS_ERROR 链路状态参数无效；
 *         负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_link_state_set(dev_dp83848_obj_t *obj, uint32_t link_state);

/**
 * @brief 使能 PHY 环回模式（调试用）
 * @param obj  设备对象
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_loopback_enable(dev_dp83848_obj_t *obj);

/**
 * @brief 关闭 PHY 环回模式
 * @param obj  设备对象
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_loopback_disable(dev_dp83848_obj_t *obj);

/**
 * @brief 使能 PHY 中断源
 * @param obj        设备对象
 * @param interrupt  中断源掩码，可为多个 DP83848_*_IT 的组合
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_it_enable(dev_dp83848_obj_t *obj, uint32_t interrupt);

/**
 * @brief 关闭 PHY 中断源
 * @param obj        设备对象
 * @param interrupt  中断源掩码，可为多个 DP83848_*_IT 的组合
 * @return DP83848_STATUS_OK 成功；负错误码表示读/写寄存器失败
 */
int32_t dev_dp83848_it_disable(dev_dp83848_obj_t *obj, uint32_t interrupt);

/**
 * @brief 清除 PHY 中断标志（读 ISFR 即清除）
 * @param obj        设备对象
 * @param interrupt  中断标志掩码（未使用，读 ISFR 即清除标志）
 * @return DP83848_STATUS_OK 成功；DP83848_STATUS_READ_ERROR 寄存器读取失败
 */
int32_t dev_dp83848_it_clear(dev_dp83848_obj_t *obj, uint32_t interrupt);

/**
 * @brief 获取 PHY 中断标志状态
 * @param obj        设备对象
 * @param interrupt  待检查的中断标志掩码，可为多个 DP83848_*_IT 的组合
 * @return 1 中断标志置位；0 未置位；DP83848_STATUS_READ_ERROR 寄存器读取失败
 */
int32_t dev_dp83848_it_status_get(dev_dp83848_obj_t *obj, uint32_t interrupt);

#ifdef __cplusplus
}
#endif
