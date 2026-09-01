/**
 * @file    app_sd_proto.c
 * @brief   山东车道费额显示器通信协议模块 — 注册 / 探测 / 处理任务
 *
 * 帧格式：'{' + 命令字 + 二进制长度 + 参数 + '}'，与青海协议同构。
 * 绑定 RS485 + RS232 双通道（与青海/四川同模式：acquire → register → bind → set_frame_queue）。
 * 帧头冲突：命令字 '1'~'5','7','8' 全部落入青海 probe 命令集（'1'~'9','A','B'），
 * 全协议构建下青海 probe 先注册（字母序 qh < sd）先认领山东帧；
 * 量产由 EIDE 目录排除纪律保证山东与青海/四川MTC 互斥（doc/05-01 §6 兼容矩阵）。
 */

#include "app_sd_proto.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"

#include "app_sd_proto_parse.h"
#include "app_sd_proto_cmd.h"

/* '{' 帧族互斥守卫：同一构建只允许编入一个 '{' 帧族协议（青海/山东/贵州/四川MTC）。
 * 多个同时编入 → 链接期 multiple definition 强制报错；
 * Makefile 全协议开发构建定义 STD_ALL_PROTO 跳过守卫（共存由 probe 注册序与文档纪律约束）。 */
#ifndef STD_ALL_PROTO
__attribute__((used)) char g_brace_proto_guard;
#endif

/* 地区协议通道 RB：与青海/RLS/四川等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);
RB_PROVIDE_WEAK(rb_provide_rs232, RB_SIZE_RS232);

static proto_mask_t s_sd_mask;
static proto_mask_t s_sd_mask_rs232;
static osMessageQueueId_t s_sd_queue;

/* 静态队列：payload 覆盖协议最大帧（len 字段 1B → 数据 ≤255 → 帧 ≤259，与青海同构） */
#define SD_PAYLOAD_MAX  (259U) /* '{' + cmd + len + data[0..255] + '}' */
#define SD_MSG_SIZE     (sizeof(frame_msg_t) + SD_PAYLOAD_MAX)
#define SD_QUEUE_DEPTH  (3U)
_Static_assert(SD_PAYLOAD_MAX <= RB_SIZE_RS485, "SD frame must fit RS485 RB");
_Static_assert(SD_PAYLOAD_MAX <= RB_SIZE_RS232, "SD frame must fit RS232 RB");
static StaticQueue_t s_sd_queue_cb;
static uint8_t s_sd_queue_buf[SD_QUEUE_DEPTH * SD_MSG_SIZE];
static const osMessageQueueAttr_t s_sd_queue_attr = {
    .name    = "sd_queue",
    .cb_mem  = &s_sd_queue_cb,
    .cb_size = sizeof(s_sd_queue_cb),
    .mq_mem  = s_sd_queue_buf,
    .mq_size = sizeof(s_sd_queue_buf),
};

static proto_probe_sta_t sd_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                        uint32_t *total_len, uint8_t *aux);

/**
 * @brief  山东协议模块初始化入口。
 * @note   通过 initcall 自动注册到系统启动流程：
 *         申请 RS485 / RS232 RB → 注册探测器 → 绑定双通道 → 创建队列与任务。
 *
 * @warning CH_ID_RS232_1 (PL_UART6) 为语音板 TTS 专用，任何地区协议禁止绑定。
 */
void sd_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_sd_mask = app_proto_register(sd_probe_frame, rb);
    if (s_sd_mask == 0)
        return;

    app_proto_bind_channel(s_sd_mask, CH_ID_RS485);

    ring_buffer_t *rb_rs232 = app_proto_acquire_buf(RB_SLOT_RS232, RB_SIZE_RS232);
    if (rb_rs232 != nullptr) {
        s_sd_mask_rs232 = app_proto_register(sd_probe_frame, rb_rs232);
        if (s_sd_mask_rs232 != 0)
            app_proto_bind_channel(s_sd_mask_rs232, CH_ID_RS232);
    }

    s_sd_queue = osMessageQueueNew(SD_QUEUE_DEPTH, SD_MSG_SIZE, &s_sd_queue_attr);
    app_proto_set_frame_queue(s_sd_mask, s_sd_queue);
    if (s_sd_mask_rs232 != 0)
        app_proto_set_frame_queue(s_sd_mask_rs232, s_sd_queue);

    static const osThreadAttr_t s_sd_task_attr = {
        .name       = "sd_handle_task",
        .stack_size = 256 * 4, /* 帧缓冲为 static，不放大栈 */
        .priority   = osPriorityNormal,
    };
    osThreadNew(sd_proto_handle_task, NULL, &s_sd_task_attr);
}
sw_app_initcall(sd_proto_init);

/**
 * @brief  山东协议帧探测（PURE 函数契约：只 rb_peek，无副作用）
 *
 * 首字节快速拒绝：首字节非 '{' (0x7B) → 立即 FAKE。
 * 帧结构（长度字段为二进制字节值）：
 *   '{' + 命令字 + len + data[0..len-1] + '}'，帧总长 = len + 4。
 * 命令字 '1'~'5','7','8'（无 '6'；'9'/'A'/'B' 属青海命令集 → FAKE）。
 */
static proto_probe_sta_t sd_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                        uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);

    /* 首字节快速拒绝：无数据 → FAKE；首字节非 '{' → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;
    uint8_t first_byte;
    rb_peek(rb, 0, &first_byte, 1, NULL);
    if (first_byte != '{')
        return PROTO_PROBE_FAKE;

    /* 需要至少 3 字节：'{' + 命令字 + 长度字段 */
    if (avail < 3U)
        return PROTO_PROBE_WAIT;

    uint8_t head[3];
    rb_peek(rb, 0, head, sizeof(head), NULL);
    bool cmd_ok = (head[1] >= '1' && head[1] <= '5') || head[1] == '7' || head[1] == '8';
    if (!cmd_ok)
        return PROTO_PROBE_FAKE;

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t data_len  = head[2];
    uint32_t frame_len = (uint32_t)data_len + 4U;
    if (avail < frame_len)
        return PROTO_PROBE_WAIT;

    uint8_t tail;
    rb_peek(rb, frame_len - 1U, &tail, 1, NULL);
    if (tail != '}')
        return PROTO_PROBE_FAKE;

    *total_len = frame_len; /* 仅 READY 路径写输出参数 */
    return PROTO_PROBE_READY;
}

/**
 * @brief  山东协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void sd_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[SD_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_sd_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        sd_parsed_cmd_t cmd = sd_parse_frame(msg->data, msg->data_len);
        sd_execute_cmd(msg->ch, &cmd);
    }
}