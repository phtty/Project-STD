#include "app_ldi.h"
#include "initcall.h"

#include "app_ldi_cmd.h"
#include "crc_utils.h"
#include "dev_flash_ldi.h"
#include "app_tcp_client.h"
#include "app_tcp_server.h"
#include "pl_net.h"
#include "pl_rtc.h"

static_assert(sizeof(ldi_device_t) == 1, "ldi_device_t must be 1 byte");
static_assert(sizeof(ldi_cmd_type_t) == 1, "ldi_cmd_type_t must be 1 byte");

const static ldi_cmd_type_t cmd_index_table[] = {
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
            {.device_type = LDI_DEV_TYPE_CANOPY_LIGHT, .device_index = 1},
        },
    },
};

void ldi_ctx_init(ldi_ctx_t *self)
{
    dev_flash_ldi_cfg_info_t flash_cfg = {0};

    if (dev_flash_ldi_load_config(&flash_cfg)) {
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

    } else {
        /* flash 无有效配置，填入网络参数，modules[] 沿用静态初始化中的编译期默认值 */
        uint8_t ip[4] = {0}, mask[4] = {0}, gw[4] = {0};
        pl_net_get_ip(ip, mask, gw);
        memcpy(self->cfg.device_ip, ip, sizeof(ip));
        memcpy(self->cfg.netmask, mask, sizeof(mask));
        memcpy(self->cfg.gateway, gw, sizeof(gw));
        self->cfg.device_port = app_tcp_server_get_port();
        memcpy(self->cfg.host_ip, app_tcp_client_get_host_ip(), 4);
        self->cfg.host_port = app_tcp_client_get_host_port();
        self->cfg_valid     = true;
        /* 不在上电阶段写 Flash：擦除会暂停 CPU 总线 1~2s，损坏 LwIP 时序。
           配置由 0AH 命令在出厂配置阶段写入，写入时网络负载低，风险可控。 */
    }
}

/* ---- 协议自注册 ---- */
static proto_mask_t s_ldi_mask;

[[maybe_unused]] static void ldi_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(1, 2048);
    s_ldi_mask = app_proto_register(ldi_probe_frame, rb);
    if (s_ldi_mask == 0) return;

    // 绑定协议使用的通道
    app_proto_bind_channel(s_ldi_mask, CH_ID_TCP_SERVER);
    app_proto_bind_channel(s_ldi_mask, CH_ID_TCP_CLIENT);
    app_proto_bind_channel(s_ldi_mask, CH_ID_UDP);

    /* 上下文初始化必须在创建任务之前，保证 IP/端口在通道任务启动前就绪 */
    ldi_ctx_init(&g_ldi);

    /* 保护 tx_buf，ldi_handle_task 和 ldi_timer_task 共享 */
    const osMutexAttr_t tx_lock_attr = {.name = "ldi_tx_lock", .attr_bits = osMutexPrioInherit};
    g_ldi.tx_lock                    = osMutexNew(&tx_lock_attr);

    // 创建协议相关处理任务
    g_ldi_task_handle       = osThreadNew(ldi_handle_task, nullptr, &ldi_task_attr);
    g_ldi_timer_task_handle = osThreadNew(ldi_timer_task, nullptr, &ldi_timer_task_attr);
}
sw_app_initcall(ldi_module_init);

osMessageQueueId_t g_ldi_msg_queue;
osThreadId_t g_ldi_task_handle;
const osThreadAttr_t ldi_task_attr = {
    .name       = "ldi_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

osThreadId_t g_ldi_timer_task_handle;
const osThreadAttr_t ldi_timer_task_attr = {
    .name       = "ldi_timer_task",
    .stack_size = 512 * 4,
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
    static frame_msg_t msg;
    const osMessageQueueAttr_t proto_ldi_queue_attr = {.name = "proto_ldi_queue"};
    g_ldi_msg_queue                                 = osMessageQueueNew(2, sizeof(frame_msg_t), &proto_ldi_queue_attr);
    app_proto_set_frame_queue(s_ldi_mask, g_ldi_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_ldi_msg_queue, &msg, NULL, osWaitForever))
            continue;

        ldi_frame_t *ldi_frame   = (ldi_frame_t *)msg.data;
        ldi_req_head_t *req_head = (ldi_req_head_t *)ldi_frame->data_crc;

        /* 状态门禁 */
        if (!ldi_cmd_allowed(g_ldi.state, req_head->cmd_type))
            continue;

        /* 查表分派 */
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < sizeof(cmd_index_table); i++)
            if (cmd_index_table[i] == req_head->cmd_type)
                idx = i;

        if (idx < sizeof(cmd_index_table))
            g_ldi_cmd_table[idx](msg.ch, ldi_frame->data_crc);
    }
}

/* ================================================================
 *  帧探测
 * ================================================================ */

const static uint8_t ldi_stx[2] = {0xFF, 0xFF};

proto_probe_sta_t ldi_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *aux)
{
    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < sizeof(ldi_frame_t) + sizeof(ldi_req_head_t) + 2)
        return PROTO_PROBE_WAIT;

    static uint8_t mem_pool[512] = {0};
    memset(mem_pool, 0, sizeof(mem_pool));
    rb_peek(buff, 0, mem_pool, avail, nullptr);
    ldi_frame_t *frame = (ldi_frame_t *)mem_pool;

    if (memcmp(ldi_stx, frame->stx, sizeof(ldi_stx)))
        return PROTO_PROBE_FAKE;
    if (frame->ver != 0x00)
        return PROTO_PROBE_FAKE;

    g_ldi.rsp_seq = frame->seq; /* 保存序号用于响应回显 */

    uint32_t data_len  = (frame->len[3] & 0xFF) | (frame->len[2] & 0xFF00) | (frame->len[1] & 0xFF0000) | (frame->len[0] & 0xFF000000);
    uint16_t frame_crc = (frame->data_crc[data_len] << 8) | frame->data_crc[data_len + 1];
    uint16_t calc_crc  = crc16_xmodem(&frame->ver, data_len + sizeof(*frame) - sizeof(frame->stx));
    if (frame_crc != calc_crc)
        return PROTO_PROBE_FAKE;

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
                *total_len = (uint32_t)(sizeof(ldi_frame_t) + data_len + 2);
                return PROTO_PROBE_SKIP;
            }
        }
    }

    *aux       = cmd;
    *total_len = sizeof(ldi_frame_t) + data_len + 2;
    return PROTO_PROBE_READY;
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

        channel_t *ch = app_channel_get(CH_ID_TCP_CLIENT);

        if (ch == nullptr || ch->state != CH_STATE_UP) {
            g_ldi.state = LDI_ST_UNINIT;
            continue;
        }

        uint32_t now = osKernelGetTickCount();

        if (g_ldi.state == LDI_ST_UNINIT) {
            if (now - g_ldi.last_cert_tick >= 3000) {
                ldi_send_cert_req(ch);
                g_ldi.last_cert_tick = now;
            }
        }

        if (g_ldi.state == LDI_ST_AUTHED || g_ldi.state == LDI_ST_READY) {
            if (now - g_ldi.last_rpt_tick >= 5000) {
                ldi_send_sta_rpt(ch);
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
