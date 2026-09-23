/**
 * @file    app_cfg_sched.h
 * @brief   配置调度器 — W25Qxx 尾部配置区管理（Application 层）
 *
 * 设计取舍: 简单调用优先于跨版本找回。
 *   - 块位 = **启动时按记录头里的 name 反查**得到 (对照各所有者的注册名认领),
 *     而非注册序号。原先的"块 = 注册序号 (纯计算, 无启动扫描)"隐含假设注册
 *     顺序稳定, 而注册顺序就是同层 initcall 顺序 = 链接顺序 = 构建清单的文件
 *     次序: 移动/重命名源文件、新增一个排得更前的注册者, 都会让所有块号平移,
 *     旧记录仍在原地址却再没人去找, 表现为"配置莫名全部回落默认值"。
 *     改为扫描后位置与任何顺序无关, 代价是启动多 8 次 24 字节头读取。
 *     首次上电(无记录)时按"注册序号那块优先, 否则第一个空块"落位。
 *   - 记录归属 = 注册名直接写入记录头 (dev_cfg_record.name[16], 无魔数概念)
 *   - 改名即视为换模块, 旧记录回落默认 (可接受);
 *     陌生数据绝不会被误读为有效配置 (名字+版本+长度+CRC 全量校验)
 *
 * 调用方式: 各配置所有者在自己的 initcall 中注册 (仿协议自注册),
 * 之后 load/save 全走调度器, 不接触名字填充/地址/组包缓冲。
 * 排除编译的模块不注册 → 不占块、不影响他人; 名字重复的注册被忽略。
 *
 * 编译期契约 (static_assert): 字库总占用 + 预留区 ≤ 容量契约 (W25Q256)。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "app_render.h" /* RENDER_PERSIST_PAYLOAD_MAX */
#include "board.h"      /* BOARD_FONT_LIB_TOTAL_BYTES（板级，两版字库大小不同） */
#include "dev_cfg_record.h" /* dev_cfg_record_state_t */

/* ---- 编译期契约 ---- */
#define CFG_REGION_SECTOR     (4096U)
#define CFG_CAP_CONTRACT      (32U * 1024U * 1024U) /* W25Q256 */
#define CFG_REGION_MAX_BLOCKS (8U)                  /* 尾部预留区: 8 × 4KB */
#define CFG_REGION_BYTES      (CFG_REGION_MAX_BLOCKS * CFG_REGION_SECTOR)

/* 字库与配置区在器件上都必须放得下。器件实配容量由运行期门槛把关
   （见 app_cfg_sched.c 的 s_storage_ready）—— 这里锁的是"设计假定的容量"。
   字库大小是板级量（两版字库不同），取自 board.h。 */
_Static_assert(BOARD_FONT_LIB_TOTAL_BYTES + CFG_REGION_BYTES <= CFG_CAP_CONTRACT,
               "font region overlaps config region");

/* 记录头的定长缓冲（dev_cfg_record 的去重读回缓冲 + 本模块的组包缓冲）必须装得下
   本工程最大的一个载荷。这两处取小了不会越界（save 返回 -1），但会表现为
   "配置存不下去"，且要到现场才发现。 */
_Static_assert(RENDER_PERSIST_PAYLOAD_MAX + DEV_CFG_RECORD_HDR_SIZE <= DEV_CFG_RECORD_MAX_IMAGE,
               "DEV_CFG_RECORD_MAX_IMAGE 装不下最大的记录：调大它，或核对 RENDER_PERSIST_BITMAP_MAX");

/* ---- 注册描述 ---- */
typedef struct {
    const char *name;     /* 唯一标识 (<= 15 字符, 写入记录头作归属校验), 也作调试名 */
    uint16_t version;     /* 本所有者记录格式版本 (不符视为无效, 供格式演进) */
    void (*load)(void);   /* 启动加载回调: 读+校验+应用或回落默认 (调用方实现; 可为 NULL) */
} app_cfg_sched_desc_t;

/**
 * @brief  注册配置所有者 (各模块 sw_dev initcall 中调用)
 * @return 句柄 id (0..CFG_REGION_MAX_BLOCKS-1), 供 load/save 使用;
 *         名字重复/满员/名字过短 返回 0xFF (注册被忽略)
 */
uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *desc);

/** @brief 加载本所有者记录 (内部用注册名 + version + dev_cfg_record 全量校验) */
dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *payload, uint16_t payload_cap, uint16_t *payload_len);

/** @brief 保存本所有者记录 (调度器内部组包缓冲 + 写前对比去重; W25Qxx RMW 自行擦除) */
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *payload, uint16_t payload_len);

/** @brief 存储是否可用 (JEDEC 容量 >= 配置区 32KB 即可; 不足则全部配置回落默认) */
bool app_cfg_sched_ready(void);

/** @brief 启动加载遍: 按注册顺序调用各 load 回调 (sw_app 首字母序最先) */
void app_cfg_sched_load_all(void);
