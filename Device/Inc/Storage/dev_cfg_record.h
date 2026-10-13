/**
 * @file    dev_cfg_record.h
 * @brief   配置记录通用服务（Device 层, 基于 dev_storage_t 存储接口）
 *
 * 为各模块的持久化配置提供带完整性校验的记录读写。
 * 记录布局: name[16] | version(2) | len(2) | crc32(4) | payload[len]
 *   - name   所有者标识 (NUL 填充, 擦除态全 0xFF 表示从未写入): 归属校验
 *   - version 记录格式版本: 格式演进
 *   - crc32  覆盖 payload 全部字节: 完整性校验
 *   - EMPTY(从未写入) / INVALID(name/version/len/CRC 任一不符) / IO_ERR
 *     统一由调用方回落默认值
 *   - save 先读回对比, 内容完全一致则跳过写 (NOR 擦写磨损保护)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "dev_storage.h"

#define DEV_CFG_RECORD_NAME_MAX (16U) /**< 所有者标识最大长度（含结尾 NUL）*/

/** @brief 记录头 (24 字节, 紧打包) */
typedef struct [[gnu::packed]] {
    char name[DEV_CFG_RECORD_NAME_MAX]; /**< 所有者标识 (NUL 填充; 首字节 0xFF = 从未写入) */
    uint16_t version;                   /**< 记录格式版本 */
    uint16_t len;                       /**< payload 字节数 */
    uint32_t crc32;                     /**< payload 全部字节的 CRC32 */
} dev_cfg_record_hdr_t;

#define DEV_CFG_RECORD_HDR_SIZE ((uint32_t)sizeof(dev_cfg_record_hdr_t)) /**< 记录头字节数 (24) */

_Static_assert(sizeof(dev_cfg_record_hdr_t) == 24, "dev_cfg_record_hdr_t must be 24 bytes");

/** @brief 记录镜像（头 + 载荷）上限，用于两处静态缓冲的定长
 *
 * 取值由**本工程最大的一个载荷**决定，当前是渲染显存：
 *   app_render_persist_t(5B) + 最大模组位图 2560B（P10 320×64 的 1bpp 位图）
 *   = 2565B 载荷，+ 24B 头 = 2589B。
 * 取 2589 向上留余量到 2600，两处静态缓冲因此各占 2600B（合计约 5.1KB SRAM）。
 *
 * 这不是可以随手调小的"缓冲大小"：调小于最大载荷时 save 会返回 -1，
 * 表现为"配置存不下去"而不是越界。上界由 app_cfg_sched.h 的 _Static_assert
 * 与渲染侧的运行期检查共同守住。
 *
 * 若以后要省这几 KB，可以改成"分块读回比对"（逐 256B 比较即可判定是否一致），
 * 那样只需一个小缓冲。当前按简单优先。 */
#define DEV_CFG_RECORD_MAX_IMAGE (2600U)

/** @brief CRC 计算函数 (默认 crc32_calc, Kernel 纯软件; 可注入其他实现) */
typedef uint32_t (*dev_cfg_record_crc_fn_t)(const uint8_t *data, size_t len);

/** @brief 读取结果状态 */
typedef enum {
    DEV_CFG_RECORD_STATE_OK      = 0, /**< 读取成功, payload 有效 */
    DEV_CFG_RECORD_STATE_EMPTY,       /**< 区域全 0xFF, 从未写入 → 调用方应用默认值 */
    DEV_CFG_RECORD_STATE_INVALID,     /**< name/version/len/CRC 任一不符 → 调用方应用默认值 */
    DEV_CFG_RECORD_STATE_IO_ERR,      /**< 底层读写失败 → 调用方应用默认值 */
} dev_cfg_record_state_t;

/**
 * @brief  加载配置记录
 *
 * @param  stor        存储设备 (W25Qxx 等, 需 RTOS 已启动)
 * @param  addr        记录起始地址
 * @param  name        期望所有者标识 (<= 15 字符)
 * @param  version     期望版本 (不符视为 INVALID)
 * @param  crc         校验函数, NULL 则用 crc32_calc
 * @param[out] payload 载荷输出缓冲
 * @param  payload_cap 载荷缓冲容量 (记录 len 超过则 INVALID)
 * @param[out] payload_len 实际载荷长度输出 (可为 NULL)
 * @return dev_cfg_record_state_t 状态; 仅 OK 时 payload 有效
 */
dev_cfg_record_state_t dev_cfg_record_load(dev_storage_t *stor, uint32_t addr,
                              const char *name, uint16_t version, dev_cfg_record_crc_fn_t crc,
                              uint8_t *payload, uint16_t payload_cap, uint16_t *payload_len);

/**
 * @brief  保存配置记录 (scratch 组包 → 读回对比去重 → 单次写入)
 *
 * W25Qxx 的 write 本身按 4KB 扇区 RMW, 覆盖非 0xFF 区间时自动擦除,
 * 故无需显式 erase; 去重可避免重复写入同一内容时的无谓擦写。
 *
 * @param  stor        存储设备
 * @param  addr        记录起始地址 (调用方保证该块归其所有)
 * @param  name        所有者标识
 * @param  version     版本
 * @param  crc         校验函数, NULL 则用 crc32_calc
 * @param  payload     载荷数据
 * @param  payload_len 载荷长度
 * @param[out] scratch 组包缓冲 (容量 >= 24 + payload_len)
 * @param  scratch_cap scratch 容量
 * @return 0 成功 (含去重跳过); 负值失败
 *
 * @note 本函数**可重入**：不持有任何文件级可变状态（去重读回是分块比对，
 *       用栈上的小缓冲）。但底层存储设备未必可重入 —— 本工程 W25Qxx 驱动
 *       自带串行化锁，故这一层不必再包。
 */
int32_t dev_cfg_record_save(dev_storage_t *stor, uint32_t addr,
                        const char *name, uint16_t version, dev_cfg_record_crc_fn_t crc,
                        const uint8_t *payload, uint16_t payload_len,
                        uint8_t *scratch, uint16_t scratch_cap);
