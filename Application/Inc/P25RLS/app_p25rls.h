#pragma once

/**
 * @file    app_p25rls.h
 * @brief   P25RLS 协议（P25 可变信息板简易通讯协议）：帧格式、命令字与任务接口
 *
 * 帧格式与 **RLS 完全相同**（`FF FE` + 大端整帧长度 + 2 字节命令 + 数据 + BCC +
 * `0D 0C`），但命令集与 `SV` 的载荷不同。两者**不能编进同一块板**：同格式的帧会
 * 被两边的探针同时认领、两个协议任务都会处理一遍。按产品二选一（与 RLS/LDI 的
 * 取舍方式相同）。
 *
 * 多字节口径：
 *   · 长度域 = **整帧字节数**（含帧头/命令/BCC/帧尾），**大端**；
 *   · 命令字两个字节就是 ASCII 字符本身（`"SV"` = 0x53 0x56），枚举按**小端拼成
 *     uint16** 取值（与 RLS 同一读法：`"BM"` → 0x4d42 同理）；
 *   · 接收端**不校验 BCC**：与 RLS 一致 —— 现场上位机的 BCC 算法与文档对不上，
 *     文档里那个 BCC 例子本身就是坏的；发送侧按"帧头 → BCC 前所有字节异或"算。
 *   · **无回显**：RLS 的"收到什么指令反馈什么指令"是它文档 §5 的规定，P25 文档
 *     没有这条，所以只有 `SC` 回一帧 `RB`，其余命令都不回。
 */

#include <stdint.h>

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief 帧头 */
#define P25RLS_HEAD_BYTE0 (0xFFU)
/** @brief 帧头第二字节 */
#define P25RLS_HEAD_BYTE1 (0xFEU)

/** @brief 帧尾第一字节 */
#define P25RLS_TAIL_BYTE0 (0x0DU)
/** @brief 帧尾第二字节 */
#define P25RLS_TAIL_BYTE1 (0x0CU)

/** @brief P25RLS 帧 —— 帧头 6 字节 + DATA/BCC + 帧尾 */
typedef struct [[gnu::packed]] {
    uint8_t head[2];         /**< 帧头，固定 0xFF 0xFE */
    uint8_t length[2];       /**< **整帧**字节数，大端（含帧头与帧尾） */
    uint8_t cmd[2];          /**< 命令字，线上是 ASCII 两字节（大端序即字符序） */
    uint8_t data_bcc_tail[]; /**< DATA + BCC(1) + 帧尾 0x0D 0x0C */
} app_p25rls_frame_t;

/** @brief 最短帧字节数：帧头 6 + BCC 1 + 帧尾 2（`CL` 的 DATA 为空） */
#define P25RLS_FRAME_MIN_BYTES (sizeof(app_p25rls_frame_t) + 3U)

/** @brief 最长帧字节数 = 框架暂存上限（`HZ` 的文本长度不设协议上限） */
#define P25RLS_FRAME_MAX_BYTES (FRAME_DATA_MAX_LEN)

/** @brief 命令字（线上两字节按小端拼成的 uint16；见文件头说明） */
typedef enum {
    APP_P25RLS_CMD_TYPE_DISPLAY_SAVE = 0x5653, /**< 全屏图片显示并保存（线上 "SV"） */
    APP_P25RLS_CMD_TYPE_SET_IP       = 0x5049, /**< 修改 IP（线上 "IP"） */
    APP_P25RLS_CMD_TYPE_AQUIRY_IP    = 0x4353, /**< 查询 IP（线上 "SC"） */
    APP_P25RLS_CMD_TYPE_REPORT_IP    = 0x4252, /**< 查询应答（线上 "RB"）—— 仅设备→主机 */
    APP_P25RLS_CMD_TYPE_FILL         = 0x5346, /**< 全屏纯色（线上 "FS"） */
    APP_P25RLS_CMD_TYPE_TEXT         = 0x5A48, /**< 文字显示（线上 "HZ"） */
    APP_P25RLS_CMD_TYPE_CLEAR        = 0x4C43, /**< 清屏（线上 "CL"） */
    APP_P25RLS_CMD_TYPE_BAUD         = 0x4442, /**< 修改波特率（线上 "BD"） */
} app_p25rls_cmd_type_t;

/** @brief 本协议的协议控制块（测试与将来的板级绑定用）
 *  @return 协议控制块，恒非 nullptr */
app_pcb_t *app_p25rls_pcb(void);

/** @brief 处理一帧完整 P25RLS 帧：命令字分派
 *
 *  帧的**结构**校验（帧头/长度域/帧尾）已在探针里做过，本函数只做命令字与载荷
 *  长度检查。从任务里拆出来是为了能在 host 上直接喂帧测。
 *
 *  @param ccb   本帧来源通道（应答发往它）
 *  @param frame 整帧首字节（只读）
 *  @param len   整帧字节数（= 帧内长度域） */
void app_p25rls_handle_frame(app_ccb_t *ccb, const uint8_t *frame, uint16_t len);

/** @brief P25RLS 协议任务入口
 *  @param argument 未使用（单例任务） */
void app_p25rls_task(void *argument);

/** @brief P25RLS 帧探测（pcb_ops.probe）
 *  契约见 `app_dispatch.h` 的 `app_pcb_probe_fn_t`。
 *
 *  只判"是不是本协议的完整帧"：帧头 → 长度域上下界 → 整帧到齐 → 帧尾。
 *  **不校验命令字与 BCC**（前者是处理层的事，后者现场算不对）。
 *
 *  @param self         协议控制块
 *  @param ccb          数据来源通道（本协议不使用）
 *  @param src          本帧来源描述（本协议不使用）
 *  @param[out] scratch 框架暂存区；本函数只写入帧头 6 字节
 *  @param scratch_size 暂存区容量；不足一个帧头时返回 SKIP
 *  @param[out] total_len 输出：整帧字节数（READY 时有效）
 *  @param aux          协议层分类输出（本协议不使用，不写）
 *  @return 探测状态，取值见 app_pcb_probe_state_t */
app_pcb_probe_state_t app_p25rls_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                             const app_ccb_src_t *src, uint8_t *scratch,
                                             uint16_t scratch_size, uint32_t *total_len,
                                             uint8_t *aux);
