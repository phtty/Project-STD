/**
 * @file    app_sc_etc_proto.c
 * @brief   四川 ETC 费显协议模块（1D）— 注册 / 探测 / 处理任务 / 心跳计时
 *
 * 绑定 RS485 + RS232 双通道（与青海同模式：acquire → register → bind → set_frame_queue）。
 * 心跳机制原由固件实现：独立计时任务，5 分钟未收到任何有效帧（心跳/显示/灯控/亮度）
 * 则显示「ETC车道关闭」；收到有效帧刷新计时。无重发/超时逻辑。
 * 2026-08-17：心跳计时任务已停用（#if 0 + 创建处注释）——「ETC车道关闭」超时显示
 * 与黄闪 10 秒自动关闭（依赖该任务 tick）一并失效，详见下文「心跳超时计时」注释。
 */

#include "app_sc_etc_proto.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"

#include "app_sc_etc_proto_parse.h"
#include "app_sc_etc_proto_cmd.h"

/* 地区协议通道 RB：与青海/RLS 等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);
RB_PROVIDE_WEAK(rb_provide_rs232, RB_SIZE_RS232);

static proto_mask_t s_sc_etc_mask;
static proto_mask_t s_sc_etc_mask_rs232;
static osMessageQueueId_t s_sc_etc_queue;

/* 静态队列：payload 覆盖协议最大帧。
 * 0x0D 定界扫描上限：扫描索引 >148 即拒 → 0x0D 最迟位于索引 148 → 帧总长 ≤149B（头 3 + 数据 ≤145 + 0D）。
 * 全屏数据无固定 56B 长度，变长按收到内容渲染。 */
#define SC_ETC_PAYLOAD_MAX (149U)
#define SC_ETC_MSG_SIZE    (sizeof(frame_msg_t) + SC_ETC_PAYLOAD_MAX)
#define SC_ETC_QUEUE_DEPTH (3U)
_Static_assert(SC_ETC_PAYLOAD_MAX <= RB_SIZE_RS485, "SC_ETC frame must fit RS485 RB");
_Static_assert(SC_ETC_PAYLOAD_MAX <= RB_SIZE_RS232, "SC_ETC frame must fit RS232 RB");
static StaticQueue_t s_sc_etc_queue_cb;
static uint8_t s_sc_etc_queue_buf[SC_ETC_QUEUE_DEPTH * SC_ETC_MSG_SIZE];
static const osMessageQueueAttr_t s_sc_etc_queue_attr = {
    .name    = "sc_etc_queue",
    .cb_mem  = &s_sc_etc_queue_cb,
    .cb_size = sizeof(s_sc_etc_queue_cb),
    .mq_mem  = s_sc_etc_queue_buf,
    .mq_size = sizeof(s_sc_etc_queue_buf),
};

/* ---- 心跳超时计时 ---- */

/** 5 分钟无有效帧 → 显示「ETC车道关闭，请择道行驶」（协议 §7） */
#define SC_ETC_HEARTBEAT_TIMEOUT_S (300U)

/** ETC 黄闪自动关闭时长（10 秒，1s 递减到 0 关黄闪） */
#define SC_ETC_HS_TIMEOUT_S (10U)

/* 2026-08-17：心跳超时显示功能已停用——sc_etc_heartbeat_task 以 #if 0 包裹、
 * sc_etc_proto_init 中任务创建已注释。以下状态与 API 保留但暂无消费方：
 *   - s_etc_uptime_s 不再递增，s_etc_activity_s / s_etc_closed_shown 仅被
 *     sc_etc_activity_refresh 写入、无读取方（刷新调用保留，无害）；
 *   - 黄闪 10 秒自动关闭依赖本任务 tick（s_etc_uptime_s 每秒 +1 后与
 *     s_etc_hs_off_s 比较）→ 一并失效：0A 38 黄闪开启后不再自动关闭，
 *     须由上位机 0A 39 显式关闭（sc_etc_hs_timer_arm/cancel 仍被调用，仅记录状态）。
 * 恢复方法：删除任务函数 #if 0/#endif，并恢复 sc_etc_proto_init 中任务创建
 * （sc_etc_heartbeat_task，栈 256×4=1KB），上述两项功能同时恢复。 */

static volatile uint32_t s_etc_uptime_s;   /**< 计时任务秒计数（递增） */
static volatile uint32_t s_etc_activity_s; /**< 最近一次有效帧时刻（秒） */
static volatile bool s_etc_closed_shown;   /**< 车道关闭画面是否已渲染 */
static volatile uint32_t s_etc_hs_off_s;   /**< 黄闪自动关闭时刻（0=未启用），handle/计时任务交叉读写 */

/**
 * @brief  刷新心跳活动计时（收到有效帧时调用）。
 * @note   32 位对齐读写原子；计时任务与处理任务交叉访问，误差 ≤1s 可接受。
 *         2026-08-17：心跳计时任务已停用（#if 0），本函数暂无读取方，
 *         保留调用与实现以便恢复（无害，仅写两个 volatile 状态）。
 */
static void sc_etc_activity_refresh(void)
{
    s_etc_activity_s  = s_etc_uptime_s;
    s_etc_closed_shown = false;
}

/** @brief 0x38 黄闪开：启动 10 秒自动关闭计时。 */
void sc_etc_hs_timer_arm(void)
{
    s_etc_hs_off_s = s_etc_uptime_s + SC_ETC_HS_TIMEOUT_S;
}

/** @brief 0x39 黄闪关：清零自动关闭计时。 */
void sc_etc_hs_timer_cancel(void)
{
    s_etc_hs_off_s = 0U;
}

/**
 * @brief  心跳计时任务：每秒检查一次超时，超时后渲染车道关闭画面一次，
 *         并处理 ETC 黄闪 10 秒自动关闭。
 * @param  argument  任务参数，当前未使用。
 *
 * 2026-08-17 已停用（#if 0）：心跳超时显示功能注释下线。注意本任务同时承担
 * 黄闪 10 秒自动关闭的 tick（s_etc_uptime_s 递增 + s_etc_hs_off_s 判定），
 * 停用后 0A 38 黄闪开启不再 10 秒自动关闭，须由 0A 39 显式关闭。
 * 恢复方法：删除 #if 0/#endif 并恢复 sc_etc_proto_init 中任务创建。
 */
#if 0
static void sc_etc_heartbeat_task(void *argument)
{
    (void)argument;
    for (;;) {
        osDelay(1000);
        s_etc_uptime_s++;

        if (s_etc_hs_off_s != 0U && s_etc_uptime_s >= s_etc_hs_off_s) {
            s_etc_hs_off_s = 0U;
            sc_etc_hs_timeout();
        }

        if (!s_etc_closed_shown && (s_etc_uptime_s - s_etc_activity_s) >= SC_ETC_HEARTBEAT_TIMEOUT_S) {
            s_etc_closed_shown = true;
            sc_etc_show_lane_closed();
        }
    }
}
#endif

static proto_probe_sta_t sc_etc_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                            uint32_t *total_len, uint8_t *aux);

/**
 * @brief  四川 ETC 费显协议模块初始化入口。
 * @note   通过 initcall 自动注册到系统启动流程：
 *         申请 RS485 / RS232 RB → 注册探测器 → 绑定双通道 → 创建队列与任务。
 *
 * @warning CH_ID_RS232_1 (PL_UART6) 为语音板 TTS 专用，任何地区协议禁止绑定。
 */
void sc_etc_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_sc_etc_mask = app_proto_register(sc_etc_probe_frame, rb);
    if (s_sc_etc_mask == 0)
        return;

    app_proto_bind_channel(s_sc_etc_mask, CH_ID_RS485);

    ring_buffer_t *rb_rs232 = app_proto_acquire_buf(RB_SLOT_RS232, RB_SIZE_RS232);
    if (rb_rs232 != nullptr) {
        s_sc_etc_mask_rs232 = app_proto_register(sc_etc_probe_frame, rb_rs232);
        if (s_sc_etc_mask_rs232 != 0)
            app_proto_bind_channel(s_sc_etc_mask_rs232, CH_ID_RS232);
    }

    s_sc_etc_queue = osMessageQueueNew(SC_ETC_QUEUE_DEPTH, SC_ETC_MSG_SIZE, &s_sc_etc_queue_attr);
    app_proto_set_frame_queue(s_sc_etc_mask, s_sc_etc_queue);
    if (s_sc_etc_mask_rs232 != 0)
        app_proto_set_frame_queue(s_sc_etc_mask_rs232, s_sc_etc_queue);

    static const osThreadAttr_t s_sc_etc_task_attr = {
        .name       = "sc_etc_handle_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(sc_etc_proto_handle_task, NULL, &s_sc_etc_task_attr);

    /* 2026-08-17：心跳计时任务已停用（见 sc_etc_heartbeat_task #if 0 注释）——
     * 5 分钟超时「ETC车道关闭」显示与黄闪 10 秒自动关闭均不再生效。
     * 恢复方法：取消本段注释（任务栈 256×4=1KB，自 ucHeap 支出）。 */
    // static const osThreadAttr_t s_sc_etc_timer_attr = {
    //     .name       = "sc_etc_heartbeat_task",
    //     .stack_size = 256 * 4,
    //     .priority   = osPriorityLow,
    // };
    // osThreadNew(sc_etc_heartbeat_task, NULL, &s_sc_etc_timer_attr);
}
sw_app_initcall(sc_etc_proto_init);

/**
 * @brief  ETC 协议帧探测（PURE 函数契约：只 rb_peek，无副作用）
 *
 * 首字节快速拒绝：首字节非 0x0A → 立即 FAKE。
 * 帧结构（无长度字段，以 0x0D 结尾）：
 *   - 灯控/心跳 0A 36/37/38/39/50 0D           → 固定 3 字节
 *   - 亮度     0A 40 XX YY 0D                  → 固定 5 字节
 *   - 显示     0A 00/01 <行号 0~6> 数据 0D      → 变长，扫描 0x0D
 *     （≤149 字节；0x0D 定界扫描索引 >148 拒帧）
 * 0A 46 0A/0A 46 0D（MTC 主机查询/清屏）命令位 0x46 不在本协议命令集 → FAKE，交由 MTC probe。
 */
static proto_probe_sta_t sc_etc_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                            uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);

    /* 首字节快速拒绝：无数据 → FAKE；首字节非 0x0A → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(rb, 0, &first_byte, 1, NULL);
    if (first_byte != 0x0A)
        return PROTO_PROBE_FAKE;

    if (avail < 2U)
        return PROTO_PROBE_WAIT;

    uint8_t cmd_byte;
    rb_peek(rb, 1, &cmd_byte, 1, NULL);

    switch (cmd_byte) {
        case 0x36: /* 交通灯红 */
        case 0x37: /* 交通灯绿 */
        case 0x38: /* 黄闪开 */
        case 0x39: /* 黄闪关 */
        case 0x50: /* 心跳 */
            if (avail < 3U)
                return PROTO_PROBE_WAIT;
            {
                uint8_t tail;
                rb_peek(rb, 2, &tail, 1, NULL);
                if (tail != 0x0D)
                    return PROTO_PROBE_FAKE;
            }
            *total_len = 3U;
            return PROTO_PROBE_READY;

        case 0x40: /* 亮度 0A 40 XX YY 0D */
            if (avail < 5U)
                return PROTO_PROBE_WAIT;
            {
                uint8_t tail;
                rb_peek(rb, 4, &tail, 1, NULL);
                if (tail != 0x0D)
                    return PROTO_PROBE_FAKE;
            }
            *total_len = 5U;
            return PROTO_PROBE_READY;

        case 0x00: /* 静态显示 */
        case 0x01: { /* 滚屏显示 */
            if (avail < 3U)
                return PROTO_PROBE_WAIT;
            uint8_t row;
            rb_peek(rb, 2, &row, 1, NULL);
            if (row > 6U) /* 单行行号 1~6，0=全屏 */
                return PROTO_PROBE_FAKE;

            /* 变长数据：扫描 0x0D 结尾；上限 149 字节
             * （0x0D 定界扫描索引 >148 即拒 → 0x0D 最迟索引 148 → 帧 ≤149B；
             *   数据段无固定 56B 长度）。
             * 滚屏帧（协议 §9）头 6 字节固定（0A 01 00 md rt st），从索引 6 起扫描，
             * 避免 rt/st 控制字节恰为 0x0D 时提前误定界。 */
            const uint32_t max_total = SC_ETC_PAYLOAD_MAX;
            const uint32_t scan_from = (cmd_byte == 0x01U) ? 6U : 3U;
            for (uint32_t off = scan_from; off < avail && off < max_total; off++) {
                uint8_t b;
                rb_peek(rb, off, &b, 1, NULL);
                if (b == 0x0D) {
                    *total_len = off + 1U;
                    return PROTO_PROBE_READY;
                }
            }
            /* 已到上限仍未见 0x0D → 超长/错帧，交重同步 */
            if (avail >= max_total)
                return PROTO_PROBE_FAKE;
            return PROTO_PROBE_WAIT;
        }

        default: /* 0x46 等不属于 ETC 命令集（MTC 的 0A 46 帧）→ FAKE */
            return PROTO_PROBE_FAKE;
    }
}

/**
 * @brief  ETC 协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void sc_etc_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[SC_ETC_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_sc_etc_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        sc_etc_parsed_cmd_t cmd = sc_etc_parse_frame(msg->data, msg->data_len);
        if (cmd.sta == SC_ETC_PARSE_OK) {
            sc_etc_activity_refresh(); /* 心跳计时任务已停用，刷新暂无消费方（保留待恢复） */
        }
        sc_etc_execute_cmd(msg->ch, &cmd);
    }
}