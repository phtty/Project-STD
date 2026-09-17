/**
 * @file    cfg_record.h
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

#define CFG_RECORD_NAME_MAX (16U)

/** @brief 记录头 (24 字节, 紧打包) */
typedef struct [[gnu::packed]] {
    char name[CFG_RECORD_NAME_MAX]; /* 所有者标识 (NUL 填充; 首字节 0xFF = 从未写入) */
    uint16_t version;
    uint16_t len;    /* payload 字节数 */
    uint32_t crc32;
} cfg_record_hdr_t;

#define CFG_RECORD_HDR_SIZE ((uint32_t)sizeof(cfg_record_hdr_t)) /* 24 */

_Static_assert(sizeof(cfg_record_hdr_t) == 24, "cfg_record_hdr_t must be 24 bytes");

/** @brief 记录镜像（头 + 载荷）上限，用于两处静态缓冲的定长
 *
 * 取值由**本工程最大的一个载荷**决定，当前是渲染显存：
 *   render_persist_t(5B) + 最大模组位图 2560B（P10 320×64 的 1bpp 位图）
 *   = 2565B 载荷，+ 24B 头 = 2589B。
 * 取 2589 向上留余量到 2600，两处静态缓冲因此各占 2600B（合计约 5.1KB SRAM）。
 *
 * 这不是可以随手调小的"缓冲大小"：调小于最大载荷时 save 会返回 -1，
 * 表现为"配置存不下去"而不是越界。上界由 app_cfg_sched.h 的 _Static_assert
 * 与渲染侧的运行期检查共同守住。
 *
 * 若以后要省这几 KB，可以改成"分块读回比对"（逐 256B 比较即可判定是否一致），
 * 那样只需一个小缓冲。当前按简单优先。 */
#define CFG_RECORD_MAX_IMAGE (2600U)

/** @brief CRC 计算函数 (默认 crc32_calc, Kernel 纯软件; 可注入其他实现) */
typedef uint32_t (*cfg_record_crc_fn)(const uint8_t *data, size_t len);

/** @brief 读取结果状态 */
typedef enum {
    CFG_REC_OK      = 0, /* 读取成功, payload 有效 */
    CFG_REC_EMPTY,       /* 区域全 0xFF, 从未写入 → 调用方应用默认值 */
    CFG_REC_INVALID,     /* name/version/len/CRC 任一不符 → 调用方应用默认值 */
    CFG_REC_IO_ERR,      /* 底层读写失败 → 调用方应用默认值 */
} cfg_rec_sta_t;

/**
 * @brief  加载配置记录
 *
 * @param  stor        存储设备 (W25Qxx 等, 需 RTOS 已启动)
 * @param  addr        记录起始地址
 * @param  name        期望所有者标识 (<= 15 字符)
 * @param  version     期望版本 (不符视为 INVALID)
 * @param  crc         校验函数, NULL 则用 crc32_calc
 * @param  payload     载荷输出缓冲
 * @param  payload_cap 载荷缓冲容量 (记录 len 超过则 INVALID)
 * @param  payload_len 输出实际载荷长度 (可为 NULL)
 * @return cfg_rec_sta_t 状态; 仅 OK 时 payload 有效
 */
cfg_rec_sta_t cfg_record_load(dev_storage_t *stor, uint32_t addr,
                              const char *name, uint16_t version, cfg_record_crc_fn crc,
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
 * @param  scratch     组包缓冲 (容量 >= 24 + payload_len)
 * @param  scratch_cap scratch 容量
 * @return 0 成功 (含去重跳过); 负值失败
 *
 * @warning **不可并发调用**：去重读回用的是一个文件级静态缓冲，两个任务同时
 *          save 会让后到者覆写先到者的组包内容，落盘记录头/载荷互相混杂，
 *          下次上电校验失败、静默回落默认值。调用方必须自行串行化
 *          （app_cfg_sched_save 持锁后再调本函数，全工程的写路径都经它）。
 */
int32_t cfg_record_save(dev_storage_t *stor, uint32_t addr,
                        const char *name, uint16_t version, cfg_record_crc_fn crc,
                        const uint8_t *payload, uint16_t payload_len,
                        uint8_t *scratch, uint16_t scratch_cap);
