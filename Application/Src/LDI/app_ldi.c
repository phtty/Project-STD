#include "app_ldi.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "app_ldi_cmd.h"
#include "app_vms_ctrl.h"
#include "crc_utils.h"
#include "app_ldi_cfg.h"
#include "app_iap_cfg.h"
#include "app_tcp_client.h"
#include "app_tcp_server.h"
#include "app_udp.h"
#include "pl_net.h"
#include "pl_rtc.h"
#include "pl_task.h"
#include "pl_mem.h"

/* ---- proto_ldi_queue 静态分配 ---- */
#define LDI_DATA_MAX  (512U)                                     /**< DATA 域最大长度 */
#define LDI_FRAME_MAX (sizeof(ldi_frame_t) + LDI_DATA_MAX + 2U)  /**< 8 + 512 + 2 = 522 */
#define LDI_MSG_SIZE (sizeof(frame_msg_t) + LDI_FRAME_MAX)

static StaticQueue_t s_ldi_queue_cb;
static uint8_t s_ldi_queue_buf[2 * LDI_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_ldi_queue_attr = {
    .name    = "proto_ldi_queue",
    .cb_mem  = &s_ldi_queue_cb,
    .cb_size = sizeof(s_ldi_queue_cb),
    .mq_mem  = s_ldi_queue_buf,
    .mq_size = sizeof(s_ldi_queue_buf),
};

static_assert(sizeof(ldi_device_t) == 1, "ldi_device_t must be 1 byte");
static_assert(sizeof(ldi_cmd_type_t) == 1, "ldi_cmd_type_t must be 1 byte");

static const ldi_cmd_type_t cmd_index_table[] = {
    LDI_CMD_SET_IP_REQ,
    LDI_CMD_SET_PARA_REQ,
    LDI_CMD_REBOOT_REQ,
    LDI_CMD_GET_IP_REQ,
    LDI_CMD_GET_PARA_REQ,
    LDI_CMD_STA_RPT_RSP,
    LDI_CMD_CERT_RSP,
    LDI_CMD_UPDATE_RSP,
    LDI_CMD_INIT_REQ,
    LDI_CMD_CTRL_REQ,
    LDI_CMD_FUNC_RPT_REQ,
    LDI_CMD_SEARCH_REQ,
};

/* ---- 协议上下文 ---- */
ldi_ctx_t g_ldi = {
    .state = LDI_ST_UNINIT,
    .cfg   = {
        .module_count = 2,
        .modules      = {
            {.device_type = LDI_DEV_TYPE_VMS, .device_index = 1},
            // {.device_type = LDI_DEV_TYPE_CANOPY_LIGHT, .device_index = 1},
        },
    },
};

void ldi_ctx_init(ldi_ctx_t *self)
{
    app_flash_ldi_cfg_info_t flash_cfg = {0};

    if (app_flash_ldi_load_config(&flash_cfg)) {
        /* Flash 有有效配置：应用到运行环境 */

        /* 网络参数（device_ip/mask/gw/host_ip/host_port 等） */
        memcpy(self->cfg.device_ip, flash_cfg.device_ip, sizeof(self->cfg.device_ip));
        self->cfg.device_port = flash_cfg.device_port;
        memcpy(self->cfg.netmask, flash_cfg.netmask, sizeof(self->cfg.netmask));
        memcpy(self->cfg.gateway, flash_cfg.gateway, sizeof(self->cfg.gateway));
        memcpy(self->cfg.host_ip, flash_cfg.host_ip, sizeof(self->cfg.host_ip));
        self->cfg.host_port = flash_cfg.host_port;
        memcpy(self->cfg.lane_hex, flash_cfg.lane_hex, sizeof(self->cfg.lane_hex));
        memcpy(self->cfg.cert, flash_cfg.cert, sizeof(self->cfg.cert));

        /* 应用到 LwIP / TCP Server / TCP Client */
        pl_net_set_ip(self->cfg.device_ip, self->cfg.netmask, self->cfg.gateway);
        app_tcp_server_set_port(self->cfg.device_port);
        app_tcp_client_set_remote(self->cfg.host_ip, self->cfg.host_port);

        /* module: 以编译期 type 做键匹配，同步 index */
        for (uint8_t i = 0; i < self->cfg.module_count; i++) {
            for (uint8_t j = 0; j < flash_cfg.module_count; j++) {
                if (flash_cfg.modules[j].device_type == self->cfg.modules[i].device_type) {
                    self->cfg.modules[i].device_index = flash_cfg.modules[j].device_index;
                    break;
                }
            }
        }
        self->cfg_valid = true;

        /* 不再在这里显式同步 IAP 记录：上面第 81 行的 pl_net_set_ip 会触发 IP 变更
           监听，IAP 侧据此做镜像同步（见 app_iap.c 的 iap_ip_change_cb）。
           原先这里那段"仅当 IAP 已有有效配置且不一致时"的守卫，是为了绕开
           update_net_cfg 对空记录会写出永久无效记录的老 bug —— 那个 bug 已在
           app_flash_iap_update_net_cfg 里修掉，守卫连同调用一并去除。 */

    } else {
        /* 外部 flash 无有效配置，尝试从 IAP 内部 flash 读取 */
        if (app_flash_iap_is_config_valid(g_config)) {
            memcpy(self->cfg.device_ip, g_config->net_cfg.ip, 4);
            memcpy(self->cfg.netmask, g_config->net_cfg.mask, 4);
            memcpy(self->cfg.gateway, g_config->net_cfg.gw, 4);
        } else {
            /* IAP 也无有效配置，使用上电默认 IP */
            uint8_t ip[4] = {0}, mask[4] = {0}, gw[4] = {0};
            pl_net_get_ip(ip, mask, gw);
            memcpy(self->cfg.device_ip, ip, sizeof(ip));
            memcpy(self->cfg.netmask, mask, sizeof(mask));
            memcpy(self->cfg.gateway, gw, sizeof(gw));
        }
        self->cfg.device_port = app_tcp_server_get_port();
        memcpy(self->cfg.host_ip, app_tcp_client_get_host_ip(), 4);
        self->cfg.host_port = app_tcp_client_get_host_port();
        self->cfg_valid     = true;
        /* 不在上电阶段写 Flash：擦除会暂停 CPU 总线 1~2s，损坏 LwIP 时序。
           配置由 0AH 命令在出厂配置阶段写入，写入时网络负载低，风险可控。 */
    }
}

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ----
 * RB 容量取「2 × 最长帧」与「传输层单次最大写入 + 1」的较大者：app_ccb_dispatch 一次
 * 投递的是传输层一整段读数（TCP 单段 ≤1460、UDP ≤1472），比它小的 RB 会被 rb_write 截断。
 * **+1 是必须的**：ring buffer 保留一个空槽区分满/空（rb_space = size - avail - 1），
 * 容量取成与单次写入相等时，恰好满的那一次会静默丢掉最后一个字节。 */
RB_DEFINE_ATTR(s_ldi_rb, 2112, PL_CCMRAM); /**< max(2 × 最长帧 522, 单次最大写入 2048 + 1) */

static const pcb_ops_t s_ldi_ops = {.probe = ldi_probe_frame};

static pcb_t s_ldi_pcb = {
    .name        = "ldi",
    .ops         = &s_ldi_ops,
    .rb          = &s_ldi_rb,
    .payload_max = LDI_FRAME_MAX,
};

static_assert(LDI_FRAME_MAX <= FRAME_DATA_MAX_LEN, "LDI 最长帧超过框架暂存上限");

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void ldi_module_init(void)
{
    rb_init(&s_ldi_rb, "ldi");

    /* 队列在 initcall 内建好：通道任务可能早于协议任务首次运行就投递帧，
       晚建会留下"向空队列投递"的窗口 */
    g_ldi_msg_queue = osMessageQueueNew(2, LDI_MSG_SIZE, &s_ldi_queue_attr);
    s_ldi_pcb.queue = g_ldi_msg_queue;

    // 绑定协议使用到的通道
    app_proto_bind(&s_ldi_pcb, app_tcp_server_ccb());
    app_proto_bind(&s_ldi_pcb, app_tcp_client_ccb());
    app_proto_bind(&s_ldi_pcb, app_udp_ccb());

    /* 上下文初始化必须在创建任务之前，保证 IP/端口在通道任务启动前就绪 */
    ldi_ctx_init(&g_ldi);

    /* 保护 tx_buf，ldi_handle_task 和 ldi_timer_task 共享 */
    const osMutexAttr_t tx_lock_attr = {.name = "ldi_tx_lock", .attr_bits = osMutexPrioInherit};
    g_ldi.tx_lock                    = osMutexNew(&tx_lock_attr);

    // 创建协议相关处理任务
    g_ldi_task_handle       = pl_task_new(ldi_handle_task, nullptr, &ldi_task_attr);
    g_ldi_timer_task_handle = pl_task_new(ldi_timer_task, nullptr, &ldi_timer_task_attr);
}
/* sw_post(4)：让"读配置"排在"加载配置"之后（cfg 调度器在 sw_app(3) 执行加载遍）。
   同层 initcall 的相对次序 = 链接顺序 = 构建清单文件次序，不能用它表达依赖。 */
sw_post_initcall(ldi_module_init);

osMessageQueueId_t g_ldi_msg_queue;
osThreadId_t g_ldi_task_handle;
const osThreadAttr_t ldi_task_attr = {
    .name       = "ldi_handle_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

osThreadId_t g_ldi_timer_task_handle;
const osThreadAttr_t ldi_timer_task_attr = {
    .name = "ldi_timer_task",
    /* 1536：实测峰值 768 字节（app_diag 的栈水位），留 2 倍余量。
       曾按 -fstack-usage 的函数帧估成 ~310 而收窄到 1024，实测只剩 256 字节 ——
       那次估算漏了 LwIP 那段（ccb_send → netconn_write 走 mailbox，栈消耗不小）
       与 vms_timer_poll → app_render → draw_bitmap 的渲染链。
       教训：函数帧累加低估库调用，以实测水位为准。 */
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ================================================================
 *  状态门禁
 * ================================================================ */

static bool ldi_cmd_allowed(ldi_state_t state, uint8_t cmd)
{
    /* 配置接口命令不受状态限制，任何时候都可执行 */
    if (cmd == LDI_CMD_SET_IP_REQ || cmd == LDI_CMD_SET_PARA_REQ ||
        cmd == LDI_CMD_REBOOT_REQ || cmd == LDI_CMD_GET_IP_REQ ||
        cmd == LDI_CMD_GET_PARA_REQ || cmd == LDI_CMD_SEARCH_REQ)
        return true;

    switch (state) {
        case LDI_ST_UNINIT:
            return cmd == LDI_CMD_CERT_RSP;
        case LDI_ST_AUTHED:
            return cmd == LDI_CMD_CERT_RSP || cmd == LDI_CMD_INIT_REQ;
        case LDI_ST_READY:
            return true;
        default:
            return false;
    }
}

/* ================================================================
 *  响应帧头部构建（从内部状态，不从请求拷贝）
 * ================================================================ */

/**
 * @brief 从内部状态构建标准响应头（4 字节时间戳）
 *
 * Unix 时间戳取自内部 RTC，lane_code / cert_info 取自 g_ldi.cfg，
 * reserve 填零。不再从请求帧拷贝头部。
 *
 * @param head     待填充的响应头指针
 * @param cmd_type 响应命令码（如 LDI_CMD_SET_IP_RSP = 0xA0）
 */
void ldi_build_rsp_head(ldi_req_head_t *head, uint8_t cmd_type)
{
    uint32_t ts             = pl_rtc_get_timestamp(pl_rtc_get_handle());
    head->cmd_type          = cmd_type;
    head->unix_timestamp[0] = (uint8_t)(ts >> 24);
    head->unix_timestamp[1] = (uint8_t)(ts >> 16);
    head->unix_timestamp[2] = (uint8_t)(ts >> 8);
    head->unix_timestamp[3] = (uint8_t)ts;
    memcpy(head->lane_code, g_ldi.cfg.lane_hex, sizeof(head->lane_code));
    memcpy(head->cert_info, g_ldi.cfg.cert, sizeof(head->cert_info));
    memset(head->reserve, 0, sizeof(head->reserve));
}

/**
 * @brief 从内部状态构建控制/查询响应头（8 字节时间戳，毫秒精度）
 *
 * 与 ldi_build_rsp_head 同理，但时间戳为 8 字节毫秒格式（秒×1000）。
 * 仅 B1H 控制查询应答使用。
 *
 * @param head     待填充的控制查询响应头指针
 * @param cmd_type 响应命令码（LDI_CMD_CTRL_RSP = 0xB1）
 */
void ldi_build_ctrl_rsp_head(ldi_ctrl_head_t *head, uint8_t cmd_type)
{
    uint64_t ts_ms          = (uint64_t)pl_rtc_get_timestamp(pl_rtc_get_handle()) * 1000;
    head->cmd_type          = cmd_type;
    head->unix_timestamp[0] = (uint8_t)(ts_ms >> 56);
    head->unix_timestamp[1] = (uint8_t)(ts_ms >> 48);
    head->unix_timestamp[2] = (uint8_t)(ts_ms >> 40);
    head->unix_timestamp[3] = (uint8_t)(ts_ms >> 32);
    head->unix_timestamp[4] = (uint8_t)(ts_ms >> 24);
    head->unix_timestamp[5] = (uint8_t)(ts_ms >> 16);
    head->unix_timestamp[6] = (uint8_t)(ts_ms >> 8);
    head->unix_timestamp[7] = (uint8_t)ts_ms;
    memcpy(head->lane_code, g_ldi.cfg.lane_hex, sizeof(head->lane_code));
    memcpy(head->cert_info, g_ldi.cfg.cert, sizeof(head->cert_info));
    memset(head->reserve, 0, sizeof(head->reserve));
}

/* ================================================================
 *  协议处理任务
 * ================================================================ */

void ldi_handle_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[LDI_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(g_ldi_msg_queue, msg, NULL, osWaitForever))
            continue;

        ldi_frame_t *ldi_frame   = (ldi_frame_t *)msg->data;
        ldi_req_head_t *req_head = (ldi_req_head_t *)ldi_frame->data_crc;

        /* 序号回显的来源。必须在**本任务内**、于分派之前取：探针不能写它（一次排空里
           会被反复调用），而本任务逐帧串行处理，处理完当前帧才会取下一帧，
           因此不会被后续帧覆盖。 */
        g_ldi.rsp_seq = ldi_frame->seq;

        /* 状态门禁 */
        if (!ldi_cmd_allowed(g_ldi.state, req_head->cmd_type))
            continue;

        /* 查表分派 */
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]); i++)
            if (cmd_index_table[i] == req_head->cmd_type)
                idx = i;

        if (idx < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]))
            g_ldi_cmd_table[idx](msg->ccb, ldi_frame->data_crc);
    }
}

/* ================================================================
 *  帧探测
 * ================================================================ */

static const uint8_t ldi_stx[2] = {0xFF, 0xFF};

/**
 * @brief LDI 帧探测（pcb_ops.probe）
 *
 * **只窥视、不写全局**：序号回显所需的 seq 由处理任务从帧里取（见 ldi_handle_task）。
 * 探针在一次排空里会被反复调用（FAKE 时逐字节重试），在此写全局状态会让后续帧
 * 覆盖前一帧的取值，而前一帧可能还排在队列里没被处理。
 *
 * 窥视一律经 rb_peek_capped —— 它按暂存区容量夹紧。此前用 rb_peek(buff, 0, mem_pool,
 * avail, nullptr) 配一个 512 字节的 mem_pool，而 rb 有 2048 字节，avail 超过 512 时
 * 就会写穿栈数组（TCP 合并分段时是常态）。
 */
pcb_probe_sta_t ldi_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux)
{
    (void)src; /* 本协议不区分来源 */
    (void)ccb;
    const ring_buffer_t *buff = self->rb;

    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < sizeof(ldi_frame_t) + sizeof(ldi_req_head_t) + 2)
        return PCB_PROBE_WAIT;

    /* 先窥视帧头（含 4 字节长度域），据此判断整帧是否已到齐。
       rb_peek_capped 按暂存区容量截断；连帧头都放不下时该协议无法工作，整帧丢弃。 */
    if (rb_peek_capped(buff, 0, scratch, scratch_size, nullptr) < sizeof(ldi_frame_t))
        return PCB_PROBE_SKIP;
    ldi_frame_t *frame = (ldi_frame_t *)scratch;

    if (memcmp(ldi_stx, frame->stx, sizeof(ldi_stx)))
        return PCB_PROBE_FAKE;
    if (frame->ver != 0x00)
        return PCB_PROBE_FAKE;

    uint32_t data_len = ((uint32_t)frame->len[0] << 24) | ((uint32_t)frame->len[1] << 16) |
                        ((uint32_t)frame->len[2] << 8) | (uint32_t)frame->len[3];

    /* 长度域上界校验：DATA 域最大 LDI_DATA_MAX，超限即伪帧。
       必须在用 data_len 索引之前校验 —— 否则下面的 data_crc[data_len] 会越界读
       （data_len 是帧内取来的 32 位值，最远可索引到暂存区之外）。 */
    if (data_len > LDI_DATA_MAX)
        return PCB_PROBE_FAKE;

    uint32_t full_len = (uint32_t)sizeof(ldi_frame_t) + data_len + 2U;

    if (avail < full_len)
        return PCB_PROBE_WAIT;

    if (full_len > scratch_size) {
        *total_len = full_len; /* 暂存区装不下 → 无法校验，整帧丢弃 */
        return PCB_PROBE_SKIP;
    }

    /* 取整帧到暂存区（上面已保证 full_len ≤ scratch_size）。
       身份校验最多读到 data_crc[21]，而顶部已保证 avail ≥ 8+20+2 = 30，
       故这些字节必在本次拷入的范围内。 */
    rb_peek_capped(buff, 0, scratch, scratch_size, nullptr);

    uint16_t frame_crc =
        (uint16_t)((frame->data_crc[data_len] << 8) | frame->data_crc[data_len + 1]);
    uint16_t calc_crc = crc16_xmodem(&frame->ver, data_len + sizeof(*frame) - sizeof(frame->stx));
    if (frame_crc != calc_crc)
        return PCB_PROBE_FAKE;

    /* 配置指令 (0AH/0BH/0DH/1DH/1EH) 不校验 lane_code/cert_info，
       直接放行；其余指令需匹配设备身份 */
    uint8_t cmd = frame->data_crc[0];
    if (!(cmd == LDI_CMD_SET_IP_REQ || cmd == LDI_CMD_SET_PARA_REQ ||
          cmd == LDI_CMD_REBOOT_REQ || cmd == LDI_CMD_GET_IP_REQ ||
          cmd == LDI_CMD_GET_PARA_REQ || cmd == LDI_CMD_SEARCH_REQ)) {

        /* 1BH/B1H 使用 ldi_ctrl_head_t (24B, UnixTimestamp 8B)，lane_code 偏移 9，cert_info 偏移 14 */
        uint8_t lane_off, cert_off;
        if (cmd == LDI_CMD_CTRL_REQ || cmd == LDI_CMD_CTRL_RSP) {
            lane_off = 9;
            cert_off = 14;
        } else {
            lane_off = 5; /* ldi_req_head_t: cmd(1) + timestamp(4) */
            cert_off = 10;
        }

        if (g_ldi.cfg_valid) {
            /* 帧结构合法但车道/设备不匹配 → SKIP 整帧 */
            if (memcmp(g_ldi.cfg.lane_hex, frame->data_crc + lane_off, sizeof(g_ldi.cfg.lane_hex)) ||
                memcmp(g_ldi.cfg.cert, frame->data_crc + cert_off, sizeof(g_ldi.cfg.cert))) {
                *total_len = full_len;
                return PCB_PROBE_SKIP;
            }
        }
    }

    *aux       = cmd;
    *total_len = full_len;
    return PCB_PROBE_READY;
}

/* ================================================================
 *  定时任务 — 周期发送 0EH（验证申请）和 0CH（状态上报）
 *
 *  每秒检查一次，根据状态机决定发送：
 *    UNINIT/AUTHED → 每 3 秒发 0EH
 *    READY        → 每 5 秒发 0CH
 *  通道断开时自动重置状态到 UNINIT。
 * ================================================================ */

void ldi_timer_task(void *argument)
{
    (void)argument;

    for (;;) {
        osDelay(1000);

        vms_timer_poll(); /* VMS 定时清屏 — 不受通道状态影响 */

        /* 主动上报固定走 TCP 客户端通道。控制块是静态对象、永不悬空，
           断线只置 state，所以这里可以直接持有指针而不必每次重新查找。 */
        ccb_t *ccb = app_tcp_client_ccb();

        if (ccb == nullptr || ccb->state != CCB_STATE_UP) {
            g_ldi.state = LDI_ST_UNINIT;
            continue;
        }

        uint32_t now = osKernelGetTickCount();

        if (g_ldi.state == LDI_ST_UNINIT) {
            if (now - g_ldi.last_cert_tick >= 3000) {
                ldi_send_cert_req(ccb);
                g_ldi.last_cert_tick = now;
            }
        }

        if (g_ldi.state == LDI_ST_AUTHED || g_ldi.state == LDI_ST_READY) {
            if (now - g_ldi.last_rpt_tick >= 5000) {
                ldi_send_sta_rpt(ccb);
                g_ldi.last_rpt_tick = now;
            }
        }
    }
}

/* ================================================================
 *  设备索引查表
 * ================================================================ */

uint8_t ldi_get_device_index(ldi_device_t device_type)
{
    for (uint8_t n = 0; n < g_ldi.cfg.module_count; n++)
        if (g_ldi.cfg.modules[n].device_type == (uint8_t)device_type)
            return g_ldi.cfg.modules[n].device_index;
    return 0xFF;
}

void ldi_set_device_index(ldi_device_t device_type, uint8_t device_index)
{
    for (uint8_t n = 0; n < g_ldi.cfg.module_count; n++)
        if (g_ldi.cfg.modules[n].device_type == (uint8_t)device_type)
            g_ldi.cfg.modules[n].device_index = device_index;
}
