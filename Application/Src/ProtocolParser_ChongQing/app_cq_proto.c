/**
 * @file    app_cq_proto.c
 * @brief   重庆高速二代费显协议（CQ）模块 — 注册 / 探测 / 处理 / 定时
 *
 * 帧形态：JSON（'{' 花括号深度定界，上限 1044B）+ 固定 12B 二进制
 * （重启请求 / 搜索请求，CRC16-XMODEM 大端）。协议版本 2.0.1。
 *
 * 通道：业务口 CH_ID_UDP_CQ（PROTO_CHONGQING 读 Sector1 net_cfg.port，默认
 *       20103；dev 共存构建固定 20103，见 app_udp.c _udp_cq_read_port）+
 *       搜索口 CH_ID_UDP（10011 固定，与创迪发现口 / IAP 共享）。
 * 注册五步（doc/08-01）：RJ45 RB provide → acquire → register（两次，两逻辑
 * 通道各一 mask）→ 静态队列（两 mask 共用）→ 处理 + 定时双任务。
 *
 * 心跳：1s 定时任务递增 s_cq_sync_counter；宽松语义（B.7.2）——帧结构合法的
 * JSON（合法命令 / 命令未知 / 参数非法）均视为上位机存活并清零，仅
 * CQ_PARSE_ERR_FRAME 与 BIN 帧（重启/搜索）不复位；≥120s 且工厂测试未激活 →
 * 渲染故障屏后清零；存活帧到达时若故障屏已显示，先恢复默认画面（
 * app_default_display_show）再执行本帧命令。工厂测试激活期间暂停计数。
 *
 * probe 注册序：Makefile 收录序 CQ 位于 LDI 之后（initcall 同层按链接序执行），
 * LDI probe 先于 CQ probe。全协议 dev 构建下 10011 口 12B 二进制帧先过 LDI
 * probe——CQ 12B 帧 len=00 00 00 02 且 CRC16-XMODEM 恰好通过 LDI 校验，
 * 2026-08-21 起 LDI probe 对 len==2 显式 FAKE 放行，帧仍由 CQ probe 认领
 * （「LDI 先探测、FAKE 放行、CQ 收单」，无功能受限；修复前 LDI 曾沿身份
 * 校验路径 SKIP 吞掉 CQ 搜索/重启帧，见 doc/03 PartB Q23）。
 */

#include "app_cq_proto.h"
#include "app_cq_proto_parse.h"
#include "app_cq_proto_cmd.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h" /* taskENTER_CRITICAL/taskEXIT_CRITICAL */
#include "cJSON.h"
#include "initcall.h"
#include "app_default_display.h"
#include "app_factory_test.h"
#include "dev_io_ctrl.h"

/* RJ45 物理通道 RB：与 IAP / LDI / MQTT 等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rj45, RB_SIZE_RJ45);

/* ---- 12B 二进制帧常量（CRC16-XMODEM 大端，值按协议文档 2.0.1 固定）---- */
const uint8_t cq_bin_reboot_req[12] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x07, 0x31, 0x5B, 0xC9};
const uint8_t cq_bin_search_req[12] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x05, 0x00, 0x48, 0x73};

/* ---- cJSON 内存钩子：绑定 FreeRTOS 堆（pvPortMalloc / vPortFree）---- */
static cJSON_Hooks s_cq_json_hooks = {
    .malloc_fn = pvPortMalloc,
    .free_fn   = vPortFree,
};

/* ---- 协议掩码（每逻辑通道独立 mask）---- */
static proto_mask_t s_cq_mask_udp_cq; /* CH_ID_UDP_CQ 业务口 20103 */
static proto_mask_t s_cq_mask_udp;    /* CH_ID_UDP   搜索口 10011 */
static osMessageQueueId_t s_cq_queue;

/* ---- 静态队列：payload 1044 ≤ RB_SIZE_RJ45(1536) ----
 * 队列体与任务帧缓冲置于 CCMRAM（CPU 独占访问、无 DMA，osMessageQueue 静态内存
 * 仅经 CPU memcpy 读写，安全）：SRAM 全协议构建仅余 ~1.9KB，CQ 队列 3236B 无法
 * 入 SRAM（doc/06-04 记账注明该例外）。 */
_Static_assert(CQ_PAYLOAD_MAX <= RB_SIZE_RJ45, "CQ frame must fit RJ45 RB");
static StaticQueue_t s_cq_queue_cb __attribute__((section(".ccmram")));
static uint8_t s_cq_queue_buf[CQ_QUEUE_DEPTH * CQ_MSG_SIZE] __attribute__((section(".ccmram")));
static const osMessageQueueAttr_t s_cq_queue_attr = {
    .name    = "cq_queue",
    .cb_mem  = &s_cq_queue_cb,
    .cb_size = sizeof(s_cq_queue_cb),
    .mq_mem  = s_cq_queue_buf,
    .mq_size = sizeof(s_cq_queue_buf),
};

/* ---- 模块状态（心跳计数 / 黄闪倒计时 / 故障屏标志）----
 * 三者跨 timer_task/handle_task 共享；M4 对齐 u32 单指令原子，但读写-判断
 * 序列用临界区保护（递增/递减/置值见下方 taskENTER_CRITICAL 包裹，阈值
 * 判断在临界区外做——值本身 volatile 保证读取可见性）。 */
static volatile uint32_t s_cq_sync_counter; /* 距上次上位机存活帧的秒数 */
static volatile int32_t s_cq_warn_seconds;  /* >0 倒计时 / -1 常开 / 0 关 */
static volatile bool s_cq_fault_shown;      /* 故障屏已渲染：下帧存活信息先恢复默认画面 */

static proto_probe_sta_t cq_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                        uint32_t *total_len, uint8_t *aux);

/**
 * @brief  CQ 协议模块初始化入口（sw_app_initcall 自注册）。
 * @note   cJSON 内存钩子换绑 FreeRTOS 堆必须在处理任务创建之前完成。
 * @note   网络配置应用（netif + 默认落盘）已移交中立模块 app_net_boot
 *         （app_boot.c init_task 调 app_net_boot_apply，2026-08-21 解耦），
 *         本模块不再承担网络启动职责；CQ UDP 业务口仍由 _udp_cq_read_port
 *         读 Sector1 net_cfg.port（现状保持）。
 */
void cq_proto_init(void)
{
    /* cJSON 钩子：parse 只发生在处理任务内，此处先行换绑 */
    cJSON_InitHooks(&s_cq_json_hooks);

    /* 第 2 步：取 RJ45 RB（weak 提供者缺位即放弃注册） */
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RJ45, RB_SIZE_RJ45);
    if (rb == nullptr)
        return;

    /* 第 3 步：两逻辑通道各注册一次拿独立 mask */
    s_cq_mask_udp_cq = app_proto_register(cq_probe_frame, rb);
    if (s_cq_mask_udp_cq == 0)
        return;
    app_proto_bind_channel(s_cq_mask_udp_cq, CH_ID_UDP_CQ);

    s_cq_mask_udp = app_proto_register(cq_probe_frame, rb);
    if (s_cq_mask_udp != 0)
        app_proto_bind_channel(s_cq_mask_udp, CH_ID_UDP);

    /* 第 4 步：静态队列（两 mask 共用同一队列） */
    s_cq_queue = osMessageQueueNew(CQ_QUEUE_DEPTH, CQ_MSG_SIZE, &s_cq_queue_attr);
    app_proto_set_frame_queue(s_cq_mask_udp_cq, s_cq_queue);
    if (s_cq_mask_udp != 0)
        app_proto_set_frame_queue(s_cq_mask_udp, s_cq_queue);

    /* 第 5 步：处理任务 + 1s 定时任务 */
    static const osThreadAttr_t s_cq_handle_attr = {
        .name       = "cq_handle_task",
        .stack_size = 256 * 4, /* 帧缓冲为 static，不放大栈 */
        .priority   = osPriorityNormal,
    };
    static const osThreadAttr_t s_cq_timer_attr = {
        .name       = "cq_timer_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(cq_proto_handle_task, NULL, &s_cq_handle_attr);
    osThreadNew(cq_proto_timer_task, NULL, &s_cq_timer_attr);
}
sw_app_initcall(cq_proto_init);

/**
 * @brief  CQ 帧探测（PURE 契约：只 rb_peek，零副作用；avail==0 必返 FAKE）。
 *
 * 首字节 '{'：花括号深度扫描定界——'"' 内跳过、深度>16 或累计长度>1044 → FAKE；
 * 深度归零 → READY；扫描到 avail 末尾未归零 → WAIT。
 * 已知限制（与裸机同脆弱，接受）：GB2312 低字节域 0x40~0xFE 含 '{'(0x7B)/'}'(0x7D)，
 * 文本字段内 GBK 字符尾字节为花括号值时会破坏深度计数；JSON 转义 '\"' 未按转义
 * 处理（'\' 后紧跟 '"' 会使字符串态提前退出）。
 * 首字节 0xFF：12 字节精确比对重启 / 搜索两帧 → READY；不足 12 字节 → WAIT；
 * 前缀不匹配 → FAKE。
 * 其他首字节 → FAKE。
 */
static proto_probe_sta_t cq_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                        uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first;
    rb_peek(rb, 0, &first, 1, NULL);

    if (first == '{') {
        /* JSON：花括号深度扫描。probe 栈受限（frame_dispatch_task 1KB），
         * 分 64B 小块 peek，不放大栈帧。 */
        uint8_t buf[64];
        uint32_t pos       = 0;
        uint32_t depth     = 0;
        uint32_t scanned   = 0;
        bool in_str        = false;

        while (pos < avail) {
            uint32_t n = avail - pos;
            if (n > sizeof(buf))
                n = sizeof(buf);
            rb_peek(rb, (uint16_t)pos, buf, (uint16_t)n, NULL);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t b = buf[i];
                if (in_str) {
                    if (b == '"')
                        in_str = false;
                } else if (b == '"') {
                    in_str = true;
                } else if (b == '{') {
                    depth++;
                    if (depth > 16)
                        return PROTO_PROBE_FAKE;
                } else if (b == '}') {
                    if (depth == 0)
                        return PROTO_PROBE_FAKE; /* 无匹配 '{' 的孤立 '}' */
                    depth--;
                    if (depth == 0) {
                        *total_len = pos + i + 1U;
                        if (*total_len > CQ_PAYLOAD_MAX)
                            return PROTO_PROBE_FAKE;
                        return PROTO_PROBE_READY;
                    }
                }
                scanned++;
                if (scanned > CQ_PAYLOAD_MAX)
                    return PROTO_PROBE_FAKE;
            }
            pos += n;
        }
        return PROTO_PROBE_WAIT; /* 扫描到 avail 末尾未归零，等更多字节 */
    }

    if (first == 0xFF) {
        /* 12B 二进制：精确匹配重启 / 搜索两帧 */
        if (avail < 12U)
            return PROTO_PROBE_WAIT;
        uint8_t buf[12];
        rb_peek(rb, 0, buf, sizeof(buf), NULL);
        if (memcmp(buf, cq_bin_reboot_req, sizeof(buf)) == 0 ||
            memcmp(buf, cq_bin_search_req, sizeof(buf)) == 0) {
            *total_len = 12;
            return PROTO_PROBE_READY;
        }
        return PROTO_PROBE_FAKE; /* 前缀不匹配 */
    }

    return PROTO_PROBE_FAKE;
}

/**
 * @brief  CQ 帧处理任务：判别 JSON / BIN，JSON 查表分派执行。
 *
 *  宽松心跳语义（doc/03 PartB B.7.2）：帧结构合法的 JSON——命令未知
 *  （CQ_PARSE_ERR_CMD）/ 参数非法（CQ_PARSE_ERR_PARAM）与合法命令同样视为
 *  上位机存活并复位心跳；仅 CQ_PARSE_ERR_FRAME 静默丢弃且继续计数。BIN 帧
 *  （重启/搜索）不复位。存活帧到达时若故障屏已显示，先恢复默认画面。
 */
void cq_proto_handle_task(void *argument)
{
    (void)argument;
    /* 帧接收缓冲置 CCMRAM（CPU 独占访问，无 DMA） */
    static uint8_t msg_buf[CQ_MSG_SIZE] __attribute__((section(".ccmram")));
    frame_msg_t *msg = (frame_msg_t *)msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_cq_queue, msg, NULL, osWaitForever))
            continue;

        cq_parsed_cmd_t cmd = cq_parse_frame(msg->data, msg->data_len);
        if (cmd.sta != CQ_PARSE_OK) {
            /* 宽松心跳语义（doc/03 PartB B.7.2）：帧结构合法的 JSON（命令未知/
             * 参数非法）仍视为上位机存活——只不计入超时、不执行；仅 ERR_FRAME
             * 静默丢弃且计数（JSON 无应答帧约定）。 */
            if (cmd.sta != CQ_PARSE_ERR_FRAME) {
                /* 故障屏恢复：存活信息到达，先恢复默认画面再清标志、复位心跳 */
                if (s_cq_fault_shown) {
                    app_default_display_show();
                    s_cq_fault_shown = false;
                }
                cq_proto_sync_reset();
            }
            continue;
        }

        /* 故障屏恢复（仅 JSON 存活命令；BIN 帧不复位心跳也不触发恢复）：
         * 先恢复默认画面、再执行本帧命令——text1/full 等随后覆盖渲染，
         * 同帧同步提交（app_render 内 commit）不闪烁。 */
        if (cmd.cmd != CQ_PCMD_BIN_REBOOT && cmd.cmd != CQ_PCMD_BIN_SEARCH &&
            s_cq_fault_shown) {
            app_default_display_show();
            s_cq_fault_shown = false;
        }

        cq_execute_cmd(msg->ch, &cmd);

        /* 任意合法 JSON 命令成功解析即清心跳（二进制帧不复位：重启帧自身复位，
         * 搜索帧非业务心跳） */
        if (cmd.cmd != CQ_PCMD_BIN_REBOOT && cmd.cmd != CQ_PCMD_BIN_SEARCH)
            cq_proto_sync_reset();
    }
}

/**
 * @brief  1s 定时任务：心跳计数、warn1 倒计时（归零关黄闪）、120s 故障屏。
 *
 *  心跳计数与故障屏渲染受编译开关 CQ_FAULT_SCREEN 控制
 *  （app_cq_proto.h）：PROTO_CHONGQING 默认开启、共存/其他构建默认关闭
 *  （他省上位机不发重庆 syn1，关闭避免误触发故障屏），可用
 *  -DCQ_FAULT_SCREEN=1/0 强制覆盖。
 *  warn1 倒计时不受工厂测试影响：移植口径按裸机 hsCnt 与 SyncTimeOutCounter
 *  同 ISR 递减、testMode 仅屏蔽故障屏渲染对齐，倒计时在工厂测试激活期间
 *  继续递减（黄闪到期即关，避免测试残留常亮）；裸机源码不在本仓库，无法
 *  逐行核对，行为以本注释与 doc/03 PartB Q17 为准。
 *  心跳计数在工厂测试激活期间暂停（STD 口径，doc/03 PartB Q17）：测试
 *  占用显示资源期间不累积超时，退出测试后重新从 0 计。
 */
void cq_proto_timer_task(void *argument)
{
    (void)argument;
    for (;;) {
        osDelay(1000);

        /* warn1 倒计时：>0 每秒递减，归零关黄闪；-1 常开不自动关。
         * 位于工厂测试判定之前：测试期间不暂停（对齐裸机同 ISR 口径）。
         * 阈值判断在临界区外（volatile 读），递减与 handle_task 侧
         * cq_proto_warn_set 的写并发 → 读-改-写序列用临界区保护。 */
        if (s_cq_warn_seconds > 0) {
            taskENTER_CRITICAL();
            s_cq_warn_seconds--;
            taskEXIT_CRITICAL();
            if (s_cq_warn_seconds == 0)
                dev_io_flash_light(false);
        }

#if CQ_FAULT_SCREEN
        /* 工厂测试激活期间暂停心跳计数 */
        if (app_factory_test_active())
            continue;

        taskENTER_CRITICAL();
        s_cq_sync_counter++;
        taskEXIT_CRITICAL();
        if (s_cq_sync_counter >= CQ_SYNC_FAULT_S) {
            s_cq_sync_counter = 0; /* 渲染后清零，不重复刷屏 */
            s_cq_fault_shown = true;
            cq_render_fault_screen();
        }
#else
        /* 故障屏开关关闭（CQ_FAULT_SCREEN=0，app_cq_proto.h）：
         * 通用版固件多协议共存时，他省上位机不发重庆 syn1 心跳，120s
         * 超时故障屏会误触发——关闭后不计数、不渲染，s_cq_sync_counter
         * 恒 0、s_cq_fault_shown 恒 false（handle_task 侧
         * cq_proto_sync_reset 清零无副作用）。强制开启：
         * -DCQ_FAULT_SCREEN=1。 */
#endif
    }
}

/* ---- 心跳计数器 / 黄闪倒计时访问（命令执行层调用）----
 * 写侧与 timer_task 的递增/递减并发 → 修改用临界区保护（任务上下文安全）。 */
void cq_proto_sync_reset(void)
{
    taskENTER_CRITICAL();
    s_cq_sync_counter = 0;
    taskEXIT_CRITICAL();
}

void cq_proto_warn_set(int32_t seconds)
{
    taskENTER_CRITICAL();
    s_cq_warn_seconds = seconds;
    taskEXIT_CRITICAL();

    if (seconds > 0) {
        dev_io_flash_light(true);
    } else if (seconds == 0) {
        dev_io_flash_light(false);
    } else {
        /* -1 常开：映射 0xFFFF 语义（协议文档 warn1 的 level 参数未实现，
         * 本实现同样忽略，仅注释说明） */
        dev_io_flash_light(true);
    }
}