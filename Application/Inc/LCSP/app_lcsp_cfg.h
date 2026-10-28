#pragma once

/**
 * @file    app_lcsp_cfg.h
 * @brief   LCSP 配置持久化：网络配置 + 出厂序列号 + 亮度/屏开关
 *
 * **为什么序列号和亮度也塞进这一条记录**：协议没有"设置序列号"的命令，也没有
 * "亮度掉电保持"以外的写入口 —— 它们只在出厂/现场各写一次，不值得各占一个 4KB 块
 * （配置区一共 12 块，各协议分着用）。网络配置与它们的写入时机也不冲突：`0x02` 是
 * 读-改-写，不会冲掉亮度档位。
 *
 * **记录名 `lcsp_cfg` 是持久化契约**（docs/命名约定.md §13）：改名等于换模块，
 * 旧设备的配置会回落默认值（`app_cfg_sched` 按名字认领块位）。
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_lcsp.h"

/** @brief 默认监听端口（协议 1.1：TCP/UDP 10028）*/
#define LCSP_PORT_DEFAULT (10028U)

/** @brief 序列号字节数（协议 7.1：8 字节 ASCII，不足补 0x00）*/
#define LCSP_SERIAL_LEN (8U)

/** @brief 设备序列号（全系列统一值，协议 7.1 的 8 字节 ASCII）
 *
 *  **所有设备报同一个值**：这是产品口径 —— 序列号只用于标识产品族，不用于区分单台设备。
 *  上位机的设备清单请按**源 IP** 区分（协议 7.3.3 的注意事项已同步这一点）；
 *  若哪天要按台区分，给它加"写序列号"命令并落到 `app_lcsp_cfg_t.serial` 即可
 *  （记录里本来就有这个字段）。 */
#define LCSP_SERIAL_DEFAULT "chuangdi"

/* 序列号在线上是定长 8 字节（没有长度字段），默认值多一个字符就会把后面的字段顶掉 */
static_assert(sizeof(LCSP_SERIAL_DEFAULT) - 1U == LCSP_SERIAL_LEN, "默认序列号必须恰好 8 字节");

/** @brief 亮度"自动光感"取值（协议 7.7：0xFF = 自动）*/
#define LCSP_BRIGHTNESS_AUTO (0xFFU)

/** @brief 亮度/开关的出厂默认：自动光感 + 屏输出开 */
#define LCSP_BRIGHTNESS_DEFAULT LCSP_BRIGHTNESS_AUTO

/** @brief 本模块的配置记录
 *
 *  前 14 字节 = 协议 `0x02` 请求 / `0x20`、`0x30` 应答的线上布局（IP/掩码/网关/端口），
 *  与 `app_lcsp_cfg.c` 的 static_assert 钉住 —— 挪动成员会让回读与线上格式错位，
 *  而两处各自都"看起来对"。 */
typedef struct [[gnu::packed]] {
    uint8_t ip[4];                 /**< 本机 IP（大端）*/
    uint8_t mask[4];               /**< 子网掩码 */
    uint8_t gw[4];                 /**< 默认网关 */
    uint8_t port[2];               /**< 监听端口（大端）；0 = 未设置 */
    uint8_t serial[LCSP_SERIAL_LEN]; /**< 设备序列号（ASCII，不足补 0）*/
    uint8_t brightness;            /**< 亮度：0–100 固定；`0xFF` = 自动光感 */
    uint8_t oe_enabled;            /**< 屏输出：1 = 开，0 = 关 */
} app_lcsp_cfg_t;

/** @brief 填充出厂默认配置（网络参数按协议 1.1 的出厂值）
 *  @param[out] cfg 接收默认值 */
void app_lcsp_cfg_default(app_lcsp_cfg_t *cfg);

/** @brief 读取配置记录（内部缓存一次；保存后缓存失效）
 *  @param[out] cfg 接收配置
 *  @return true = 记录有效并已填充；false = 无记录/记录无效/存储不可用（cfg 被清零）*/
bool app_lcsp_cfg_load(app_lcsp_cfg_t *cfg);

/** @brief 保存配置记录（内容相同则跳过擦写，由配置调度器去重）
 *  @param cfg 待保存配置（只读）
 *  @return 0 成功（含去重跳过），负值失败 */
int32_t app_lcsp_cfg_save(const app_lcsp_cfg_t *cfg);
