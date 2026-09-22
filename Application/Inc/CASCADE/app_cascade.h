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
 *     +7  idx        保留（分片已废弃，恒 0 —— 帧头长度不动，免得连带动探针与测试构造器）
 *     +8  frag_n     保留（同上）
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

/** @brief 本板跑不跑级联 —— **由网格形状推导**（单卡 = COLS×ROWS == 1 = 不跑）
 *
 *  单卡板上整个 `app_cascade.c` **编译成空**：不占 Flash、不占那 7KB CCMRAM
 *  （协议环 4096 + 帧队列 2×1435）、不发每 10 秒一次的 PING、也不读身份记录与拨码。
 *  与"没有级联之前的那套功能"逐字一致。
 *
 *  **刻意不走"从构建清单里删文件"那条路**：Makefile 的应用源清单是两块板共享的，
 *  删掉会连 5006048 一起失去级联；而 EIDE 那份清单要用户手动维护。编译成空则两边
 *  清单都不用动。
 *
 *  测试套件要跑级联：在 include 本头之前 `#define BOARD_CASCADE_ENABLED 1`。 */
#ifndef BOARD_CASCADE_ENABLED
#define BOARD_CASCADE_ENABLED (((BOARD_CASCADE_COLS) * (BOARD_CASCADE_ROWS)) > 1)
#endif

#include <stdint.h>
#include <stdbool.h>

#include "board.h"        /* BOARD_CASCADE_BAND_MAX —— 单帧上限由它推出 */
#include "app_dispatch.h" /* FRAME_DATA_MAX_LEN —— 上限断言；pcb_t —— 协议控制块 */

/* ---- 帧定界与固定开销 ---- */
#define CASC_SOF0 (0xA5U)
#define CASC_SOF1 (0x5AU)

/** @brief 帧头 + 尾 CRC 的固定开销：头 11 字节（2 sof + 1 ver_type + 1 dst + 1 src
 *         + 2 seq + 1 idx + 1 frag_n + 2 len）+ CRC32 4 字节 */
#define CASC_OVERHEAD (15U)

/** @brief 协议版本（ver_type 的高 2 位）
 *
 *  **3 = 一帧一轮 + 从卡可持久化 + 主卡格随识别帧下发**（本轮形态）；
 *  2 = 无主卡格字段；1 = 一帧一轮（无 persist）；0 = 已废弃的分片形态。
 *  `PRESENT` 回带本值，主卡那行 `[casc] PRESENT … ver=N` 于是能一眼看出两块卡是不是
 *  同批固件 —— 混烧时的表现是"从卡静默不响应"，只看日志很难往固件版本上想。 */
#define CASC_PROTO_VER (3U)

/* ---- 地址 ---- */
#define CASC_ADDR_MASTER (0x00U)
#define CASC_ADDR_BCAST  (0xFFU)

/* ---- 帧类型（ver_type 的低 6 位）----
 *
 * 0x05 / 0x08 两个值**空出来了**（原 SET_COLOR / SET_BLANK 的占位，2026-09-22 裁掉）：
 * 颜色本来就随每轮 IMAGE 一起下发（见 `casc_image_t.color`），"清屏"就是"发一块全黑的
 * 位图"（主卡每轮都发整块位图）—— 两条都是冗余的命令，留着只会让人以为有别的语义。
 */
typedef enum {
    /* 主 → 从 */
    CASC_T_IMAGE      = 0x01, /**< 一轮就是这一条：本卡那一块整块位图 + 本轮参数 */
    CASC_T_PING       = 0x06, /**< 探活/枚举，广播 */
    CASC_T_SET_ADDR   = 0x07, /**< 识别帧：主 → 卡，"我是主卡，你的地址改成 X" */
    CASC_T_SET_BRIGHT = 0x09, /**< 整屏调光，广播 */

    /* 从 → 主 —— 0x20 位是方向标记。
       类型只有 6 位（ver_type 的高 2 位是协议版本），所以方向不能另占字段；
       用 0x20 位区分，全部落在 0..0x3F 内。
       （曾用过 0x81/0x82/0x83，而 CASC_TYPE_OF 只取低 6 位 —— 0x81 掩完变成 0x01，
        与当时的 SYNC_BEGIN 撞车，从卡的 PRESENT 会被当主卡命令静默丢掉。） */
    CASC_T_PRESENT = 0x21, /**< 应答 PING：本卡身份与几何 */
    CASC_T_ACK     = 0x22, /**< 本轮收下并落屏了（**无载荷**） */
    CASC_T_NACK    = 0x23, /**< 本轮拒收（配置错，重发没用） */
} casc_type_t;

#define CASC_TYPE_MASK (0x3FU)
#define CASC_TYPE_OF(vt) ((uint8_t)((vt) & CASC_TYPE_MASK))

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

/** @brief 识别帧：主 → 其余每张卡，"我（自称）是主卡，主卡格是 M，你该是 X 号"
 *
 *  语义是**单向**的：只有"按下按键的那张卡"是发起者，**接收方从不自封主卡**
 *  （`yours` 不为 0，`0xFF` 也不行）—— 所以一条被重放、迟到、或半双工回声回来的帧，
 *  都不可能造出第二张主卡。
 *
 *  `master_cell` 是**主卡格**：整张切分表 = 它 + 网格形状（主卡格编 addr 0，其余按
 *  格序编 1..N）。它必须随识别帧一起走 —— 只改地址不改格的话，被按的那张卡会按老
 *  规矩渲染**另一块屏**那一格，两块屏的上下半幅当场对调（见 app_screen.h）。
 *
 *  `yours` 用两个值区分两种指令：
 *   · `0xFF` —— **自算**："按你自己的格 + 新的主卡格，重算你该是几号"。广播用，
 *     因为发起方未必知道总线上还有谁（两张卡都自称主卡时，它压根没枚举到对方）。
 *   · 具体地址 —— "改成这个号"。逐卡单播用，发起方按新表算好每张卡该是几号。
 *
 *  `claim` 是发送方**按下按键那一刻的内核 tick**。用途有二：
 *   · 裁决"两张卡都自称主卡"：**更晚按下者胜**（= 最近按的那张卡作数）；
 *   · **分辨自己的回声**：`src == 我的地址` 且 `claim == 我自己的 claim` = 是我刚发出去的。
 *     两块都自称主卡时两张卡的地址都是 0，光看 src 分不开 —— 这是唯一能分开的东西。 */
typedef struct [[gnu::packed]] {
    uint8_t mine;        /**< 发送方自称的地址；**只有 0 会被采纳** */
    uint8_t yours;       /**< 接收方应改成的地址；**0xFF = 按自己的格自算** */
    uint8_t master_cell; /**< 发送方认定的主卡格（哪一格编 addr 0） */
    uint8_t claim[4];    /**< 认领时刻（内核 tick），大端 */
} casc_set_addr_t;

_Static_assert(sizeof(casc_set_addr_t) == 7, "SET_ADDR 载荷必须是 7 字节");

/** @brief `yours` 的这个值 = "按自己的格自算地址"（广播识别帧用） */
#define CASC_ADDR_SELF_CALC (0xFFU)

/* ================================================================
 *  图传：一帧一轮
 *
 *  一轮 = **一条 IMAGE 帧**：它那一块矩形 + 整块位图，从卡收下即落屏。
 *
 *  **为什么不再分片**：分片只是为了绕开框架暂存上限（`FRAME_DATA_MAX_LEN` 原是 1044
 *  = IAP 的最长帧），而单卡 224×50 的 1bpp 位图是 1400 字节。现在那个上限抬到了
 *  1440，一帧装得下 —— 于是三阶段握手（BEGIN/DATA×N/COMMIT）、`idx`/`frag_n`、
 *  缺片位图、定向重传、帧间 `osDelay(1)` 全部消失。代价是每帧长 400 字节、一轮多占
 *  总线约 20ms；换来的是**每卡每轮只一次空闲中断、一次交付**（旧形态是五次），
 *  而"连发多帧"恰是接收路径那些 bug 的暴露条件。
 * ================================================================ */

/** @brief 主 → 从：本卡这一块（整块位图 + 本轮参数）
 *
 *  矩形是**整屏逻辑坐标**，其尺寸必须等于该卡自己的屏几何 —— 本工程的部署形态是
 *  「每张卡各带一整块屏，几块屏拼起来是整屏」（见 app_screen.h 的 screen_card_t）。
 *  从卡把 `x/y/w/h` 与**自己本地切分表里那一项逐字段核对**，不符回 NACK 而不是将就：
 *  将就的后果是错位画面（甚至两块屏内容互换），而从卡自己不知道错了。
 *
 *  `bright` **每一轮都重新断言**：`SET_BRIGHT` 是单次广播、没有重传，而本帧是每轮必发、
 *  丢一轮下一轮就自愈的一字节 —— 这是"从卡永久停在旧亮度上"的唯一防线。
 *
 *  `bitmap[]` 紧跟在头后面（帧内偏移 `sizeof(casc_hdr_t) + sizeof(casc_image_t)` = 24），
 *  长度由 `bmp_len` 给出：1bpp、行优先、MSB-first、bit=1 上色、末字节补位归零 ——
 *  与 `app_screen_extract` 的输出、与从卡 `app_screen_commit_bitmap` 的输入**逐位一致**，
 *  所以两侧零转码。
 *
 *  整帧 = 11 帧头 + 13 本头 + bmp_len + 4 CRC；本板满幅即 **1428** 字节。 */
typedef struct [[gnu::packed]] {
    uint8_t x[2], y[2];  /**< 本卡矩形左上角（整屏坐标），大端 */
    uint8_t w[2], h[2];  /**< 本卡矩形尺寸，大端 */
    uint8_t bmp_len[2];  /**< 位图字节数 = ceil(w/8)*h，大端 */
    uint8_t bright;      /**< 本轮亮度断言 0..7 */
    uint8_t color;       /**< 本卡颜色（display_color_t）*/
    /** @brief 本轮内容要不要在**从卡**落盘（掉电再上电自动恢复）。0/1。
     *
     *  由主卡决定：它那一轮的渲染带了 `persist` 就置 1 —— 这样上位机
     *  "这次内容要长期保留"的意图会原样传到每张从卡，各卡各存自己那一块。
     *  **用整字节不用位域**：本文件开头就写明"不写 uint16_t 成员、避开打包结构的
     *  对齐与字节序陷阱"，位域是同一类陷阱且收益只有 1 字节。 */
    uint8_t persist;
    uint8_t bitmap[];    /**< bmp_len 字节 */
} casc_image_t;

_Static_assert(sizeof(casc_image_t) == 13, "IMAGE 载荷头必须是 13 字节");

/* ---- 帧长上限 ----
 *
 * **由板级带缓冲推出，不写裸字面量**：一帧必须装得下**一整块本卡位图**，
 * 也就是 `BOARD_CASCADE_BAND_MAX`。换模组只改 board.h 一处，这里自动跟上；
 * 装不下时下面这条断言会在编译期就把话说清楚（而不是运行期静默跳过那张卡）。 */
#define CASC_FRAME_MAX (CASC_OVERHEAD + sizeof(casc_image_t) + BOARD_CASCADE_BAND_MAX)
#define CASC_FRAME_MIN (CASC_OVERHEAD)

_Static_assert(CASC_FRAME_MAX <= FRAME_DATA_MAX_LEN,
               "本卡位图一帧装不下 —— 抬 FRAME_DATA_MAX_LEN（app_dispatch.h）");

/* ================================================================
 *  图传：一轮一条 IMAGE（载荷结构见上面的 casc_image_t）
 *
 *  **历史**：这里原先是 BEGIN → DATA×frag_n → COMMIT 三阶段握手，加上分片号、
 *  缺片位图、定向重传。那一整套只为绕开当时 1044 的框架暂存上限；上限抬到 1440
 *  之后（见 app_dispatch.h 的说明），一帧就装得下整块位图，三阶段随之全部删掉。
 *
 *  ACK 也从一个 `{sta, miss_mask}` 的 2 字节结构缩成**无载荷**：它唯一的意思是
 *  "这一轮我收下并落屏了"；拒收走 NACK 那条类型，于是"缺片"这个状态不存在了。
 * ================================================================ */

/** @brief 从 → 主：这张卡参与不了（配置错，重发也没用）
 *
 *  **与"超时"分开是有意的**：超时的排查方向是链路（线、供电、卡死），
 *  拒绝的排查方向是配置（格号写重、两块板的切分表不一致）。
 *  混成一个静默超时，现场只能靠猜。 */
typedef enum {
    CASC_NACK_GEOM = 1, /**< 矩形与本卡切分表不符，或本卡地址不在切分表里 */
    CASC_NACK_LEN  = 2, /**< 帧长与 bmp_len 自相矛盾（多半是两端固件版本不同） */
    CASC_NACK_ADDR = 3, /**< 识别帧与本板定址冲突（本板有拨码、以拨码为准） */
} casc_nack_err_t;

typedef struct [[gnu::packed]] {
    uint8_t err; /**< casc_nack_err_t */
} casc_nack_t;

/* ---- 本机身份见 app_screen.h ----
 * 本卡地址与主/从角色放在 app_screen 而不是这里：身份属于"整屏"，不属于某个协议。
 * 否则光传感器、以及将来任何"只有主卡该做"的事，都得去依赖级联协议。 */

/* ---- 协议控制块（供板级 initcall 追加绑定用）---- */
#if BOARD_CASCADE_ENABLED

pcb_t *app_cascade_pcb(void);

/** @brief 枚举：主卡广播一次 PING。上电初始化后与运行期都可以调。 */
int32_t app_cascade_ping(void);

/** @brief 整屏调光：主卡广播一次亮度等级。从卡收到后写自己的 light_level。 */

int32_t app_cascade_broadcast_bright(uint8_t level);

#endif /* BOARD_CASCADE_ENABLED */

/** @brief **认领主卡**：本卡成为主卡、写记录、并把识别帧发给其余每一张卡
 *
 *  由按键（TEST）触发。**只投递请求、立刻返回** —— 真正的动作（写 flash + 发帧 +
 *  等 ACK 最长约 1s）由级联任务做：`s_tx` 与轮次都是它的，按键所在的工厂测试任务
 *  不能碰。
 *
 *  **单卡板上是空函数**（定义在 `app_cascade.c` 的禁用分支里）：调用点因此不必带
 *  条件编译，而且符号**永远存在** —— 增量构建只按 `.c` 的时间戳重编，改了 `.h`
 *  时调用方的旧目标文件可能没重编、还在引用它，有定义就不会变成"未定义引用"。 */
void app_cascade_claim_master(void);
