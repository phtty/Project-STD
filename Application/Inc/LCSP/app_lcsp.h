#pragma once

/**
 * @file    app_lcsp.h
 * @brief   外贸车道屏以太网协议（LCSP）：帧格式、协议控制块与任务入口
 *
 * 协议正文：`docs/protocol/外贸车道屏协议.md`（V1.3）。帧 =
 * 帧头 `55 AA` + SEQ(2,大端) + CMD(1) + LEN(2,大端) + DATA + CRC16(2,低字节在前)。
 *
 * **改本模块前先看这三条**（都是与工程内其它协议不一样、错了又不会报错的地方）：
 *
 *   1. **应答命令字 = 请求命令字高低半字节互换**（`0x03` ↔ `0x30`）。应答码在
 *      `app_lcsp_cmd.h` 里逐个写死成 `APP_LCSP_REPLY_xxx`，**不要**改成现场换位 ——
 *      换位函数在加新命令时漏改一行，症状是"上位机收不到应答"，而设备这边帧是
 *      合法发出的，抓包只看得出"没回"。
 *   2. **SEQ 原样回显**（协议 4.3.2）：屏没有自己的序号计数器。上位机靠它把应答
 *      与请求配对，序号错一位就被当成噪声 —— 重传、判链路故障全跟着来。
 *   3. **CRC 覆盖 SEQ 起至 DATA 末**（不含帧头、不含 CRC 自身），算法是
 *      CRC-16/X-25（反射 0x8408 / 初值 0xFFFF / 结果取反），即 `crc16_xmodem()`。
 *
 * **端口归属**：本协议独占设备监听端口（默认 10028，可由 `0x02` 改写、重启生效）。
 * 这不是"抢占"，而是产品口径：**LCSP 与 LDI / CDSTD / RLS 不同时启用**（同一台设备只跑
 * 一种上位机协议），所以"设备监听哪个端口"由本模块一家说了算。同一份固件里若真把两者
 * 同时编进去，后跑的那个 `sw_post` 会覆盖前者的端口 —— 那时要查的是产品配置，不是这里
 * （见 `app_lcsp.c` 的 `_lcsp_apply_cfg`）。
 *
 * **一帧一幅图**（协议 7.5）：整屏点阵一帧传完，没有分片、没有会话状态。因此
 * 帧长上限由**整屏位图**决定，见 `LCSP_PAYLOAD_MAX_LEN`。
 */

#include <stdint.h>

#include "cmsis_os2.h"

#include "app_dispatch.h"
#include "ring_buffer.h"

/* ---- 帧头与帧长 ---- */

#define LCSP_SOF_FIRST_BYTE  (0x55U) /**< 帧头首字节 */
#define LCSP_SOF_SECOND_BYTE (0xAAU) /**< 帧头次字节 */

/** @brief 协议允许的 DATA 上限（协议 1.1：DATA ≤ 8192，单帧 ≤ 8201） */
#define LCSP_DATA_MAX_LEN (8192U)

/** @brief 本工程能承载的最长整屏位图（字节，1bpp）
 *
 *  按本工程最大整屏取：5006048 的 1×2 级联是逻辑屏 224×100 →
 *  `ceil(224/8) × 100 = 2800`。`0x05` 的点阵数据必须**恰好**是这个长度。
 *
 *  更大的屏（如 4 卡横排 896×50）会超过它 —— 那种情况下 `0x05/0x06` 回 `0x01`
 *  （不改屏），并且要把这里与 `FRAME_DATA_MAX_LEN`（app_dispatch.h）一起抬。 */
#define LCSP_BITMAP_MAX_BYTES (2800U)

/** @brief 本协议最长 DATA：`0x05` = 槽位(1) + 颜色(1) + 整屏点阵 */
#define LCSP_PAYLOAD_MAX_LEN (2U + LCSP_BITMAP_MAX_BYTES)

/** @brief 本协议最长整帧：帧头/SEQ/CMD/LEN(7) + DATA + CRC(2) */
#define LCSP_FRAME_MAX_LEN (9U + LCSP_PAYLOAD_MAX_LEN)

/** @brief 应答帧上限：DATA = 状态(1) + 应答数据(15) —— 最大的是设备信息的 15 字节 RDATA
 *  （序列号 8 + 屏宽 2 + 屏高 2 + 槽位数 1 + 亮度 1 + 屏幕状态 1，协议 7.1）*/
#define LCSP_REPLY_MAX_LEN (9U + 1U + 15U)

/** @brief 重启命令"先应答、后复位"的间隔（协议 7.9.1 要求 ≥200ms）*/
#define LCSP_REBOOT_DELAY_MS (300U)

static_assert(LCSP_FRAME_MAX_LEN <= FRAME_DATA_MAX_LEN, "LCSP 最长帧超过框架暂存上限");

/* ---- 帧结构（结构体即线格式，改动前看 docs/命名约定.md §13）---- */

/** @brief LCSP 帧（`data` 之后紧跟 CRC16，长度 = LEN + 2）
 *
 *  CRC 放在柔性数组尾巴上而不是单独字段：它是变长的，只有按 LEN 才能定位。
 *  访问方式见 `app_lcsp_probe_frame` 与本文件的 `app_lcsp_build_frame`。 */
typedef struct [[gnu::packed]] {
    uint8_t sof[2];  /**< 帧头 `55 AA` */
    uint8_t seq[2];  /**< 包序号（大端）；应答原样回显 */
    uint8_t cmd;     /**< 命令字（请求 0x01–0x0F / 应答高低半字节互换） */
    uint8_t len[2];  /**< DATA 字节数（大端，0 表示无数据） */
    uint8_t data[];  /**< DATA（len 字节）后紧跟 CRC16（低字节在前，2 字节） */
} app_lcsp_frame_t;

/* ---- 协议接口 ---- */

/** @brief LCSP 帧探测：帧头 → LEN 合法性 → 整帧到齐 → CRC16
 *
 *  只窥视不消费，符合 `app_pcb_probe_fn_t` 的契约（调用者已持有缓冲区锁）。
 *
 *  @param self         协议控制块
 *  @param ccb          数据来源通道（本探针不看通道）
 *  @param src          来源描述（本协议不用）
 *  @param[out] scratch 框架暂存区，READY 时整帧拷进来
 *  @param scratch_size 暂存区容量
 *  @param[out] total_len 完整帧长度（READY / SKIP 时有效）
 *  @param[out] aux     命令字（READY 时由框架带进任务）
 *  @return 见 `app_pcb_probe_state_t` */
app_pcb_probe_state_t app_lcsp_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                           uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                           uint8_t *aux);

/** @brief 组一帧（帧头 + SEQ + CMD + LEN + DATA + CRC16）
 *
 *  应答与测试都走这一条，别在别处手搓字节序 —— CRC 覆盖范围与低字节在前这两个
 *  约定各写一遍就有两套，迟早对不上。
 *
 *  @param cmd      命令字（应答码）
 *  @param seq      要回显的 2 字节序号（取自请求帧 `seq[]`，大端原样）
 *  @param data     DATA 内容，可为 nullptr（此时 data_len 必须为 0）
 *  @param data_len DATA 字节数
 *  @param[out] out 接收整帧的缓冲
 *  @param out_cap  out 容量；不足时不写并返回 0
 *  @return 写入 out 的整帧字节数；0 = 参数非法或容量不足 */
uint16_t app_lcsp_build_frame(uint8_t cmd, const uint8_t seq[2], const uint8_t *data, uint16_t data_len,
                              uint8_t *out, uint16_t out_cap);

/** @brief 协议任务：取帧 → 判重 → 命令表分派 → 发应答（必要时重启）
 *  @param argument 未使用（单例任务） */
void app_lcsp_task(void *argument);

/** @brief 协议亮度百分比（0–100）→ 本工程亮度档位（1–7）
 *
 *  下限夹到 1 而不是 0：本工程的档位 0 与"关屏"共用硬件口径（`dev_display` 施加 0），
 *  而协议 7.7 说的是"0 最暗"，"关屏"是 `0x08` 的职责 —— 亮度命令不该顺手把屏关掉。
 *  上限 100 → 7；50 → 4（四舍五入到最近档）。
 *
 *  @param percent 协议亮度值 0–100
 *  @return 本工程亮度档位 1–7 */
static inline uint8_t app_lcsp_level_from_percent(uint8_t percent)
{
    uint16_t level = (uint16_t)(((uint16_t)percent * 7U + 99U) / 100U);

    if (level < 1U)
        level = 1U;
    if (level > 7U)
        level = 7U;

    return (uint8_t)level;
}
