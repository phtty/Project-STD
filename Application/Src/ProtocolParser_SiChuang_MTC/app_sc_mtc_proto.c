/**
 * @file    app_sc_mtc_proto.c
 * @brief   四川 MTC 费显协议模块（1E 方案二）— 注册 / 探测 / 处理任务
 *
 * 绑定 RS485 + RS232 双通道。探测支持双帧型：
 *   1. '{' (0x7B) 帧族：'1'~'9','A' 命令与 7B 40~45 原始帧族，统一按「'}' 定界
 *      变长」扫描（各命令逐字节扫 '}'，帧长 = '}' 下标 + 1，
 *      无长度字段、无 BCC；上限 SC_MTC_PAYLOAD_MAX=74B 设备约束）；
 *   2. 0A 46 0A / 0A 46 0D 主机查询/清屏。
 * 与青海协议（同为 '{' 开头）的互斥分析见 doc/05_协议模块多协议兼容优化/01_architecture.md §6：
 * qh_proto_init 先于 sc_mtc_proto_init 注册（字母序），QH probe 先被调用，完整青海帧
 * 由 QH 认领；残余巧合见文档说明，量产由 EIDE 目录排除纪律兜底。
 */

#include "app_sc_mtc_proto.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"

#include "app_sc_mtc_proto_parse.h"
#include "app_sc_mtc_proto_cmd.h"

/* '{' 帧族互斥守卫：同一构建只允许编入一个 '{' 帧族协议（青海/山东/贵州/四川MTC）。
 * 多个同时编入 → 链接期 multiple definition 强制报错；
 * Makefile 全协议开发构建定义 STD_ALL_PROTO 跳过守卫（共存由 probe 注册序与文档纪律约束）。 */
#ifndef STD_ALL_PROTO
__attribute__((used)) char g_brace_proto_guard;
#endif

/* 地区协议通道 RB：与青海/ETC/治超等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);
RB_PROVIDE_WEAK(rb_provide_rs232, RB_SIZE_RS232);

static proto_mask_t s_sc_mtc_mask;
static proto_mask_t s_sc_mtc_mask_rs232;
static osMessageQueueId_t s_sc_mtc_queue;

/* 静态队列：payload 覆盖协议最大帧（'}' 定界扫描上限 74B；'4' 全屏 ≤68B / '7'自定义语音 ≤74B） */
#define SC_MTC_PAYLOAD_MAX (74U)
#define SC_MTC_MSG_SIZE    (sizeof(frame_msg_t) + SC_MTC_PAYLOAD_MAX)
#define SC_MTC_QUEUE_DEPTH (3U)
_Static_assert(SC_MTC_PAYLOAD_MAX <= RB_SIZE_RS485, "SC_MTC frame must fit RS485 RB");
_Static_assert(SC_MTC_PAYLOAD_MAX <= RB_SIZE_RS232, "SC_MTC frame must fit RS232 RB");
static StaticQueue_t s_sc_mtc_queue_cb;
static uint8_t s_sc_mtc_queue_buf[SC_MTC_QUEUE_DEPTH * SC_MTC_MSG_SIZE];
static const osMessageQueueAttr_t s_sc_mtc_queue_attr = {
    .name    = "sc_mtc_queue",
    .cb_mem  = &s_sc_mtc_queue_cb,
    .cb_size = sizeof(s_sc_mtc_queue_cb),
    .mq_mem  = s_sc_mtc_queue_buf,
    .mq_size = sizeof(s_sc_mtc_queue_buf),
};

static proto_probe_sta_t sc_mtc_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                            uint32_t *total_len, uint8_t *aux);

/**
 * @brief  四川 MTC 费显协议模块初始化入口。
 * @note   通过 initcall 自动注册：申请 RS485 / RS232 RB → 注册探测器 →
 *         绑定双通道 → 创建队列与任务。
 *
 * @warning CH_ID_RS232_1 (PL_UART6) 为语音板 TTS 专用，任何地区协议禁止绑定。
 */
void sc_mtc_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_sc_mtc_mask = app_proto_register(sc_mtc_probe_frame, rb);
    if (s_sc_mtc_mask == 0)
        return;

    app_proto_bind_channel(s_sc_mtc_mask, CH_ID_RS485);

    ring_buffer_t *rb_rs232 = app_proto_acquire_buf(RB_SLOT_RS232, RB_SIZE_RS232);
    if (rb_rs232 != nullptr) {
        s_sc_mtc_mask_rs232 = app_proto_register(sc_mtc_probe_frame, rb_rs232);
        if (s_sc_mtc_mask_rs232 != 0)
            app_proto_bind_channel(s_sc_mtc_mask_rs232, CH_ID_RS232);
    }

    s_sc_mtc_queue = osMessageQueueNew(SC_MTC_QUEUE_DEPTH, SC_MTC_MSG_SIZE, &s_sc_mtc_queue_attr);
    app_proto_set_frame_queue(s_sc_mtc_mask, s_sc_mtc_queue);
    if (s_sc_mtc_mask_rs232 != 0)
        app_proto_set_frame_queue(s_sc_mtc_mask_rs232, s_sc_mtc_queue);

    static const osThreadAttr_t s_sc_mtc_task_attr = {
        .name       = "sc_mtc_handle_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(sc_mtc_proto_handle_task, NULL, &s_sc_mtc_task_attr);
}
sw_app_initcall(sc_mtc_proto_init);

/**
 * @brief  MTC 协议帧探测（PURE 函数契约：只 rb_peek，无副作用）
 *
 * 首字节快速拒绝：非 '{' 且非 0x0A → 立即 FAKE。
 *
 * '{' 帧族（'}' 定界变长，无长度字段、无 BCC）：
 *   - '1'~'9','A' 与 0x40~45 原始帧族统一：从下标 2 起扫描 '}'，
 *     帧长 = '}' 下标 + 1；上限 SC_MTC_PAYLOAD_MAX(74B，帧队列 payload
 *     设备约束；协议上限 228)。未闭合 → 缓冲不足 WAIT（帧跨 DMA 块），
 *     达到上限仍无 '}' → FAKE 逐字节重同步。
 *   - BCC 变体不单独判别：数据段内字节一律视为内容（带 BCC 帧的
 *     BCC 字节会成为文本尾部一并渲染；BCC 不校验）。
 *   - 0x41='A' 双义（4B 点阵大小 / 5B 颜色）由 parse 按帧长细分。
 *     青海 'A'/'B' 命令字重叠：qh_proto_init 先于 sc_mtc_proto_init 注册
 *     （字母序 q < s），同 RB 上 QH probe 先被调用，详见 doc/05 兼容矩阵。
 * 0x0A 帧族：仅 0A 46 0A / 0A 46 0D 两种，其余（含 ETC 0A 帧）→ FAKE。
 */
static proto_probe_sta_t sc_mtc_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                            uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);

    /* 首字节快速拒绝：无数据 → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(rb, 0, &first_byte, 1, NULL);

    if (first_byte == 0x0A) {
        /* 主机查询 0A 46 0A / 清屏 0A 46 0D；其余 0A 帧属 ETC → FAKE */
        if (avail < 3U)
            return PROTO_PROBE_WAIT;
        uint8_t b[2];
        rb_peek(rb, 1, b, sizeof(b), NULL);
        if (b[0] == 0x46 && (b[1] == 0x0A || b[1] == 0x0D)) {
            *total_len = 3U;
            return PROTO_PROBE_READY;
        }
        return PROTO_PROBE_FAKE;
    }

    if (first_byte != '{')
        return PROTO_PROBE_FAKE;

    if (avail < 2U)
        return PROTO_PROBE_WAIT;

    uint8_t cmd_byte;
    rb_peek(rb, 1, &cmd_byte, 1, NULL);

    /* ---- '{' 帧族：'}' 定界变长扫描 ----
     * '{' 后取命令字，各命令逐字节扫 '}' 定帧尾，
     * 帧长 = '}' 下标 + 1。'1'~'9','A' 与
     * 7B 40~45 原始帧族统一走本扫描；长度/参数合法性由 parse 按命令校验。 */
    if (!((cmd_byte >= '1' && cmd_byte <= '9') ||
          (cmd_byte >= 0x40 && cmd_byte <= 0x45))) /* 'A'=0x41 已含于 40~45 */
        return PROTO_PROBE_FAKE;

    uint32_t scan_limit = (avail < SC_MTC_PAYLOAD_MAX) ? avail : SC_MTC_PAYLOAD_MAX;
    for (uint32_t e = 2U; e < scan_limit; e++) { /* e = '}' 下标（最小 2：{x}） */
        uint8_t b;
        rb_peek(rb, e, &b, 1, NULL);
        if (b != '}')
            continue;
        *total_len = e + 1U;
        return PROTO_PROBE_READY;
    }
    /* 未找到 '}'：缓冲不足 → WAIT（帧跨 DMA 块）；达到上限仍无 → FAKE 重同步 */
    if (avail >= SC_MTC_PAYLOAD_MAX)
        return PROTO_PROBE_FAKE;
    return PROTO_PROBE_WAIT;
}

/**
 * @brief  MTC 协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void sc_mtc_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[SC_MTC_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_sc_mtc_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        sc_mtc_parsed_cmd_t cmd = sc_mtc_parse_frame(msg->data, msg->data_len);
        sc_mtc_execute_cmd(msg->ch, &cmd);
    }
}