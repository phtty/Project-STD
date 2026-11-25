#pragma once

/**
 * @file    app_gzzh.h
 * @brief   GZZH 协议（贵州地标情报板精简协议）：帧格式、命令字与任务接口
 *
 * 线上帧（多字节字段**除端口外**一律小端）：
 *
 * ```
 *   引导串 4 | 包序号 4 | 长度 2 | 保留 2 | 命令字 4 | 基本数据包 N | 结束符 1
 *   54 43 4C 59                        （"TCLY"）
 * ```
 *
 * **长度域 = 整帧字节数**（含引导串与结束符，= 17 + N）——这是本协议与
 * IAP/LDI/RLS 最容易照抄错的地方：那几个协议的长度域语义各不相同，只能逐协议看文档。
 *
 * 结束符恒 0x00；它是**合法性检查**，不是定界手段（定界一律靠长度域），
 * 所以文本内容里出现 0x00 不影响解析。
 *
 * 现场行为以参考实现（`netHandler`）为准，文档没写或写岔的地方按参考实现：
 *   · 命令字 4 字节小端，**高 3 字节必须为 0**；
 *   · 包序号不参与判定，应答固定回 `00 00 00 00`；
 *   · 0x10 的应答是**裸 2 字节**（没有帧头帧尾），与其它应答帧不同；
 *   · 0x40 的端口字段是**大端**（全帧唯一的例外）。
 */

#include <stdint.h>

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief 引导串字节数（ASCII "TCLY"） */
#define GZZH_HEAD_BYTES (4U)

/* ---- 逐命令日志（联调开关，**默认关**）----
 *
 * 裸板没有屏时，靠 RTT 把"上位机发了什么、我们解析成什么"打出来对照。
 * 每条命令 1~2 行；RTT 上行只有 1 KB 且是 NO_BLOCK_SKIP（满了丢新数据），
 * 所以开的时候一次别连发太多帧，最好一条一条发。
 *
 * 2026-11-25 首轮上机联调就是开着它做的，验完按约定置 0。
 * 需要再查线上字段（长度域/字体名/字号/颜色/端口）时把它置 1 重编即可。 */
#ifndef GZZH_TRACE
#define GZZH_TRACE (0)
#endif

#if GZZH_TRACE
#define GZZH_TRACE_PRINTF(...) printf(__VA_ARGS__)
#else
#define GZZH_TRACE_PRINTF(...) ((void)0)
#endif

/** @brief 结束符（合法性检查用） */
#define GZZH_TAIL_BYTE (0x00U)

/** @brief 引导串；定义在 `app_gzzh.c`，命令层回帧时也用它
 *
 *  `const` 能力表不适用"跨模块只能走 API"的限制（`docs/命名约定.md` §4.7 的例外）。 */
extern const uint8_t g_gzzh_head[GZZH_HEAD_BYTES];

/** @brief GZZH 帧 —— 16 字节引导段 + 基本数据包 + 结束符
 *
 *  `payload` 是**基本数据包**的首字节；结束符在 `payload[len - 17]`，
 *  由 `app_gzzh_handle_frame()` 从整帧长度换算，结构体里不单列。 */
typedef struct [[gnu::packed]] {
    uint8_t head[GZZH_HEAD_BYTES]; /**< 引导串，固定 54 43 4C 59 */
    uint8_t sequence[4];           /**< 包序号（本机不判定，应答回全 0） */
    uint8_t length[2];             /**< **整帧**字节数，小端（含引导串与结束符） */
    uint8_t reserved[2];           /**< 保留位，恒 00 00 */
    uint8_t cmd[4];                /**< 命令字，小端，高 3 字节恒 0 */
    uint8_t payload[];             /**< 基本数据包（变长）+ 结束符 0x00 */
} app_gzzh_frame_t;

/** @brief 最短帧字节数：引导段 16 + 结束符 1（0x30 允许零载荷） */
#define GZZH_FRAME_MIN_BYTES (sizeof(app_gzzh_frame_t) + 1U)

/** @brief 最长帧字节数 = 框架暂存上限
 *
 *  协议本身不给文本长度上限；超过它的帧**投递不上来**（`app_dispatch.h` 的
 *  `FRAME_DATA_MAX_LEN` 是平台约束），探针会按伪帧逐字节重跳。 */
#define GZZH_FRAME_MAX_BYTES (FRAME_DATA_MAX_LEN)

/** @brief GZZH 命令字
 *
 *  线上 4 字节小端；本枚举只占低 8 位（高 3 字节恒 0，探针之后的处理层会再核一次）。 */
typedef enum {
    APP_GZZH_CMD_TYPE_STATUS    = 0x10, /**< 获取故障信息，应答**裸 2 字节**状态 */
    APP_GZZH_CMD_TYPE_TEXT      = 0x20, /**< 立即显示文字（整屏替换） */
    APP_GZZH_CMD_TYPE_CLEAR     = 0x30, /**< 清屏 */
    APP_GZZH_CMD_TYPE_SET_IP    = 0x40, /**< 设置 IP / 掩码 / 网关 / 端口，应答 0x50 */
    APP_GZZH_CMD_TYPE_REPORT_IP = 0x50, /**< 设备→主机：0x40 的应答（**仅发送**，不进处理表） */
    APP_GZZH_CMD_TYPE_FILL      = 0x80, /**< 全屏纯色显示（3 字节 RGB） */
} app_gzzh_cmd_type_t;

/** @brief 本协议的协议控制块（板级通道绑定时用）
 *
 *  共享层只绑 UDP；RS232 那一路是板级事实（3833024 有 RS232-1），
 *  由 `boards/<板>/Application/Src/app_rs232.c` 取本指针去绑。
 *  @return 协议控制块，恒非 nullptr */
app_pcb_t *app_gzzh_pcb(void);

/** @brief 处理一帧完整 GZZH 帧：校验命令字并分派给命令表
 *
 *  从任务里拆出来的可测入口（套件 `test_gzzh` 直接喂帧）。帧的**结构**校验
 *  （引导串/长度域/结束符）已经在探针里做过，本函数只做命令字与载荷长度检查。
 *
 *  @param ccb   本帧来源通道（应答发往它）
 *  @param frame 整帧首字节（只读）
 *  @param len   整帧字节数（= 帧内长度域） */
void app_gzzh_handle_frame(app_ccb_t *ccb, const uint8_t *frame, uint16_t len);

/** @brief GZZH 协议任务入口
 *  @param argument 未使用（单例任务） */
void app_gzzh_task(void *argument);

/** @brief GZZH 帧探测（pcb_ops.probe）
 *  契约见 `app_dispatch.h` 的 `app_pcb_probe_fn_t`。
 *
 *  只看"是不是本协议的完整帧"：引导串 → 长度域上下界 → 整帧到齐 → 结束符。
 *  **不校验命令字**（那是处理层的事）：命令字不认识也是本协议的合法帧，
 *  整帧丢弃即可，不该让它变成伪帧逐字节重跳。
 *
 *  @param self         协议控制块
 *  @param ccb          数据来源通道（本协议不使用）
 *  @param src          本帧来源描述（本协议不使用）
 *  @param[out] scratch 框架暂存区；本函数只写入引导段 16 字节
 *  @param scratch_size 暂存区容量；不足一个引导段时返回 SKIP
 *  @param[out] total_len 输出：整帧字节数（READY 时有效）
 *  @param aux          协议层分类输出（本协议不使用，不写）
 *  @return 探测状态，取值见 app_pcb_probe_state_t */
app_pcb_probe_state_t app_gzzh_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                           const app_ccb_src_t *src, uint8_t *scratch,
                                           uint16_t scratch_size, uint32_t *total_len, uint8_t *aux);
