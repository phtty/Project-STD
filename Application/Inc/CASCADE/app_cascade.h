/**
 * @file    app_cascade.h
 * @brief   多控制卡级联同步显示协议 —— 帧结构与本机地址
 *
 * **背景**：有些屏幕产品的 HUB75 接口数超出单张控制卡的能力，必须多张卡拼一块屏。
 * 对上位机而言它仍是一个设备：只有主卡接网口，从卡只跟主卡通信（RS485 总线）。
 *
 * **本文件只是协议层**：帧怎么排、地址怎么认、谁该应答。整屏内容与切分由
 * `app_screen` 管，本协议只认它、不认识 `dev_display_t`。
 *
 * 前人做过一版（MSL，`origin/wire_p20`）：它**没有帧序号、没有应答、没有重发**，
 * 同步靠"整片推帧 + 片间 osDelay(50)"，且切分几何写死。本实现重新设计。
 *
 * ---- 帧格式 ----
 *
 *     +0  sof[2]     A5 5A          与 RLS(FFFE)/LDI(FFFF)/IAP(5A5A5A5A) 都不同
 *     +2  ver_type   b7..b6=版本(0) b5..b0=帧类型
 *     +3  dst        00=主卡 01..1F=从卡 FF=广播
 *     +4  src        主卡恒 00；从卡填自身地址
 *     +5  seq[2]     轮次序号，**大端**；回绕比较用 (int16_t)(a-b)
 *     +7  idx        分片号；非分片命令为 0
 *     +8  frag_n     本轮分片总数；非分片为 0
 *     +9  len[2]     **整帧**字节数（含头含 CRC），大端
 *     +11 payload[]
 *     +n  crc[4]     CRC32（**硬件单元**），覆盖 [2, len-4)，大端
 *
 * 多字节字段一律 `uint8_t arr[]` + 手工存取，**不写 uint16_t 成员** —— 本工程既有
 * 做法（`rls_frame_t.length[2]`、`ldi_frame_t.len[4]`），避开打包结构的对齐与字节序陷阱。
 *
 * `len` 取**整帧长度**而非 payload 长度：探针一次 `avail < len` 即可判 WAIT，
 * 一次 `len > CASC_FRAME_MAX` 即可覆盖全部越界风险，不必再算偏移。
 *
 * ---- 校验用硬件 CRC32 ----
 * payload 长达 1KB，异或 BCC 对随机错误的漏检率约 1/256，而 RS485 长线上的典型错误
 * 是突发错 —— 必须用 CRC。
 *
 * 选硬件 CRC32 而不是软件 CRC16：那个单元本来就在（IAP 在用），算 1KB 约 9µs，
 * 而软件 CRC16 约 71µs。代价是每帧多 2 字节（一轮 15 帧 = 30 字节 = 2.6ms @115200），
 * 无关紧要。
 *
 * 注意 `pl_crc32_calc` 原先对**非对齐输入只算前 256 字节**（静默截断），已修 ——
 * 而本协议的 CRC 区段起点是 `scratch + 2`（暂存区 4 字节对齐），正是那条路。
 * 见提交 `fix: 硬件 CRC32 对非对齐输入静默截断`。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---- 帧定界与长度 ---- */
#define CASC_SOF0 (0xA5U)
#define CASC_SOF1 (0x5AU)

/** @brief 帧头 + 尾 CRC 的固定开销：头 11 字节（2 sof + 1 ver_type + 1 dst + 1 src
 *         + 2 seq + 1 idx + 1 frag_n + 2 len）+ CRC32 4 字节 */
#define CASC_OVERHEAD (15U)

/** @brief 单帧上限 —— **等于框架的暂存上限**（`FRAME_DATA_MAX_LEN`）。
 *
 *  这不是本协议随便定的：`app_dispatch.c` 给探针的 scratch 恒为
 *  `FRAME_DATA_MAX_LEN`，与协议自己的 payload_max 无关。所以**更大的帧在本框架下
 *  根本投递不上来** —— 1KB 的位图必须分片，这是硬约束不是优化。 */
#define CASC_FRAME_MAX   (1044U)
#define CASC_FRAME_MIN   (CASC_OVERHEAD)
#define CASC_PAYLOAD_MAX (CASC_FRAME_MAX - CASC_OVERHEAD)

/** @brief 协议版本（ver_type 的高 2 位） */
#define CASC_PROTO_VER (0U)

/* ---- 地址 ---- */
#define CASC_ADDR_MASTER (0x00U)
#define CASC_ADDR_BCAST  (0xFFU)

/* ---- 帧类型（ver_type 的低 6 位）---- */
typedef enum {
    /* 主 → 从 */
    CASC_T_SYNC_BEGIN = 0x01, /**< 开启一轮（P3） */
    CASC_T_SYNC_DATA  = 0x02, /**< 位图分片（P3） */
    CASC_T_SYNC_COMMIT = 0x03,/**< 请该卡应用并应答（P3） */
    CASC_T_SYNC_ABORT = 0x04, /**< 主卡放弃本轮（P3） */
    CASC_T_SET_COLOR  = 0x05, /**< 单独改某卡颜色（P5） */
    CASC_T_PING       = 0x06, /**< 探活/枚举，广播 */
    CASC_T_SET_LAYOUT = 0x07, /**< 下发切分表（P5） */
    CASC_T_BLANK      = 0x08, /**< 令该卡清屏 */
    CASC_T_SET_BRIGHT = 0x09, /**< 整屏调光，广播 */

    /* 从 → 主 —— 0x20 位是方向标记。
       类型只有 6 位（ver_type 的高 2 位是协议版本），所以方向不能另占字段；
       用 0x20 位区分，全部落在 0..0x3F 内。
       （曾用过 0x81/0x82/0x83，而 CASC_TYPE_OF 只取低 6 位 —— 0x81 掩完变成 0x01，
        与 SYNC_BEGIN 撞车，从卡的 PRESENT 会被当主卡命令静默丢掉。） */
    CASC_T_PRESENT = 0x21, /**< 应答 PING：本卡身份与几何 */
    CASC_T_ACK     = 0x22, /**< 本轮结果（P3） */
    CASC_T_NACK    = 0x23, /**< 结构/几何不符（P3） */
} casc_type_t;

#define CASC_TYPE_MASK (0x3FU)
#define CASC_TYPE_OF(vt) ((uint8_t)((vt) & CASC_TYPE_MASK))

/** @brief 该类型是否由从卡发出（0x20 位） */
#define CASC_TYPE_FROM_SLAVE(t) (((t) & 0x20U) != 0U)

/* ---- 帧头 ---- */
typedef struct [[gnu::packed]] {
    uint8_t sof[2];
    uint8_t ver_type;
    uint8_t dst;
    uint8_t src;
    uint8_t seq[2];
    uint8_t idx;
    uint8_t frag_n;
    uint8_t len[2];
} casc_hdr_t;

_Static_assert(sizeof(casc_hdr_t) == 11, "级联帧头必须是 11 字节");
_Static_assert(CASC_FRAME_MAX <= 1044U, "级联帧超过框架暂存上限（FRAME_DATA_MAX_LEN）");

/* ---- 大端存取（帧内多字节字段一律走这两个）---- */
static inline uint16_t casc_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline void casc_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline uint32_t casc_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void casc_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- 命令载荷 ---- */

/** @brief 从 → 主：应答 PING（枚举用）
 *
 *  字段只服务于**枚举与校验** —— 主卡据此核对"表里写的那张卡是不是真在那儿、
 *  几何对不对"，那是同步显示的前提，不是设备状态上报。本协议不做状态回读
 *  （温度/面板故障这类由各产品自己的协议另走）。 */
typedef struct [[gnu::packed]] {
    uint8_t addr;      /**< 本卡总线地址 */
    uint8_t w[2];      /**< 本卡屏宽，大端 */
    uint8_t h[2];      /**< 本卡屏高，大端 */
    uint8_t bright;    /**< 本卡当前亮度等级（主卡据此确认调光下发到位） */
    uint8_t proto_ver; /**< 级联协议版本 */
} casc_present_t;

_Static_assert(sizeof(casc_present_t) == 7, "PRESENT 载荷必须是 7 字节");

/** @brief 主 → 全：整屏调光 */
typedef struct [[gnu::packed]] {
    uint8_t level; /**< 0..7 */
} casc_set_bright_t;

/* ---- 本机身份见 app_screen.h ----
 * 本卡地址与主/从角色放在 app_screen 而不是这里：身份属于"整屏"，不属于某个协议。
 * 否则光传感器、以及将来任何"只有主卡该做"的事，都得去依赖级联协议。 */

/* ---- 协议控制块（供板级 initcall 追加绑定用）---- */
#include "app_dispatch.h"
pcb_t *app_cascade_pcb(void);

/** @brief 枚举：主卡广播一次 PING。上电初始化后与运行期都可以调。 */
int32_t app_cascade_ping(void);

/** @brief 整屏调光：主卡广播一次亮度等级。从卡收到后写自己的 light_level。 */
int32_t app_cascade_broadcast_bright(uint8_t level);
