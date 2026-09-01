#include "app_ldi.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "app_ldi_cmd.h"
#include "app_vms_ctrl.h"
#include "app_ldi_device.h"
#include "crc_utils.h"
#include "app_ldi_cfg.h"
#include "app_board_net_cfg.h"
#include "app_tcp_client.h"
#include "app_tcp_server.h"
#include "pl_net.h"
#include "pl_rtc.h"

/* RJ45 物理通道 RB：与 IAP/MQTT 等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rj45, RB_SIZE_RJ45);

/* ---- proto_ldi_queue 静态分配 ---- */
#define LDI_PAYLOAD_MAX (512U) /* 与探头 mem_pool 容量一致 */
#define LDI_MSG_SIZE    (sizeof(frame_msg_t) + LDI_PAYLOAD_MAX)
#define LDI_QUEUE_DEPTH (4U)

static StaticQueue_t s_ldi_queue_cb;
static uint8_t s_ldi_queue_buf[LDI_QUEUE_DEPTH * LDI_MSG_SIZE];
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
        .module_count = 4,
        .modules      = {
            {.device_type = LDI_DEV_TYPE_DISPLAY, .device_index = 1},
            {.device_type = LDI_DEV_TYPE_LANE_SIGNAL, .device_index = 1},
            {.device_type = LDI_DEV_TYPE_ALARM, .device_index = 1},
            {.device_type = LDI_DEV_TYPE_VOICE, .device_index = 1},
        },
    },
};

/**
 * @brief  LDI 上下文装载：Sector1/W25 自愈 + LDI 设备配置装载。
 *
 * 职责（2026-08-21 解耦修订）：只做配置真源裁定与自愈——
 * 网络字段以 Sector1.net_cfg 为唯一真源、W25 为恢复镜像（三分支判定），
 * 非网络字段（host/lane/cert/modules）与 TCP Client 远端装载，Sector1 空/损坏
 * 时从 W25 回写自愈。**不再应用网络**：pl_net_set_ip /
 * app_tcp_server_set_port 移交中立模块 app_net_boot（app_boot.c init_task
 * 在本函数之后调用 app_net_boot_apply 统一应用 netif 与 TCP Server 口）。
 */
void ldi_ctx_init(ldi_ctx_t *self)
{
    app_flash_ldi_cfg_info_t flash_cfg = {0};
    bool flash_valid                   = false;

    if (app_flash_ldi_load_config(&flash_cfg)) {
        /* Flash 记录的 module_count 钳制：超设备类型上限（LDI_DEV_TYPE_COUNT=13）或
         * 存储数组容量（APP_FLASH_LDI_MAX_MODULES=11）视为非法记录，回退默认配置，
         * 防后续按 module_count 遍历 flash_cfg.modules[] 越界读。 */
        if (flash_cfg.module_count <= LDI_DEV_TYPE_COUNT &&
            flash_cfg.module_count <= APP_FLASH_LDI_MAX_MODULES)
            flash_valid = true;
    }

    /* ---- 方案 X（doc/07 §12）：Sector1 为网络配置唯一真源，W25 为恢复镜像 ----
     * Sector1 有效性判定：公开接口 app_board_net_cfg_get 做 magic+CRC 校验
     * （返回 0 = 有效）。内存映射拷贝，上电可安全执行（不擦写内部 Flash）。 */
    app_board_net_cfg_t board_cfg = {0};
    bool board_valid              = (app_board_net_cfg_get(&board_cfg) == 0);

    /* net_cfg 合法性（与 app_net_boot 同口径）：IP 非 0 且端口 1~65535。
     * 2026-08-21 修订：Sector1 记录有效且 net_cfg 合法即采纳（update_sta 不再参与
     * 分支判定，含升级中间态）——app_board_net_cfg_update 自 2026-08-18 起在中间态
     * 也放行 net_cfg 更新（0AH/4B02/CQ setip 改 IP 中间态同样生效），若此处仍按
     * update_sta != updated 落入分支 2，W25 回写会用陈旧镜像覆盖刚写入的 net_cfg，
     * setip 失效。Bootloader 条件 D 对中间态记录本就进 Recovery（不启动主固件），
     * 主固件可运行态下采纳 Sector1 net_cfg 无副作用。 */
    bool net_valid = board_valid &&
        (board_cfg.ip[0] | board_cfg.ip[1] | board_cfg.ip[2] | board_cfg.ip[3]) != 0U &&
        board_cfg.port > 0U && board_cfg.port <= 65535U;

    if (net_valid) {
        /* ---- 分支 1：Sector1 有效（magic+CRC）且 net_cfg 合法 → 以 Sector1 为准 ----
         * 网络字段（device_ip/netmask/gateway/device_port）全部取自 Sector1.net_cfg。
         * 缺陷 B 修复：device_port 必须读 Sector1.net_cfg.port，而非编译期默认
         * （app_tcp_server_get_port），否则 0AH/4B02 改过的端口上电即被默认值覆盖。 */
        memcpy(self->cfg.device_ip, board_cfg.ip, sizeof(self->cfg.device_ip));
        memcpy(self->cfg.netmask, board_cfg.mask, sizeof(self->cfg.netmask));
        memcpy(self->cfg.gateway, board_cfg.gw, sizeof(self->cfg.gateway));
        self->cfg.device_port = (uint16_t)board_cfg.port;

        if (flash_valid) {
            /* W25 已有有效配置：非网络字段（host/lane/cert/modules）沿用 W25，
             * 网络字段仍以 Sector1 为准（Sector1 优先，不覆盖 W25）。 */
            memcpy(self->cfg.host_ip, flash_cfg.host_ip, sizeof(self->cfg.host_ip));
            self->cfg.host_port = flash_cfg.host_port;
            memcpy(self->cfg.lane_hex, flash_cfg.lane_hex, sizeof(self->cfg.lane_hex));
            memcpy(self->cfg.cert, flash_cfg.cert, sizeof(self->cfg.cert));

            /* module: 以编译期 type 做键匹配，同步 index */
            for (uint8_t i = 0; i < self->cfg.module_count; i++) {
                for (uint8_t j = 0; j < flash_cfg.module_count; j++) {
                    if (flash_cfg.modules[j].device_type == self->cfg.modules[i].device_type) {
                        self->cfg.modules[i].device_index = flash_cfg.modules[j].device_index;
                        break;
                    }
                }
            }
            app_tcp_client_set_remote(self->cfg.host_ip, self->cfg.host_port);

            /* 缺陷 C/A 修复（镜像自愈）：Sector1 与 W25 的网络字段逐项比对，
             * 任一不一致 → 用当前 cfg（网络字段取自 Sector1）刷新 W25 镜像。
             * 覆盖两类陈旧场景：① 4B02 只写 Sector1、不写 W25 → 镜像残留旧值，
             * 擦 Sector1 恢复出厂时会回滚旧 IP；② 单次 W25 写失败导致的永久陈旧。
             * W25 为 SPI 4KB 擦写，无内部 Flash 总线停顿，上电执行安全。 */
            if (memcmp(self->cfg.device_ip, flash_cfg.device_ip, sizeof(flash_cfg.device_ip)) != 0 ||
                memcmp(self->cfg.netmask, flash_cfg.netmask, sizeof(flash_cfg.netmask)) != 0 ||
                memcmp(self->cfg.gateway, flash_cfg.gateway, sizeof(flash_cfg.gateway)) != 0 ||
                self->cfg.device_port != flash_cfg.device_port) {
                app_flash_ldi_save_config(&self->cfg);
            }
        } else {
            /* W25 空/无效：host 字段取编译期默认，并把当前 cfg 的网络字段写为 W25
             * 恢复镜像——维护「擦 Sector1 恢复出厂后仍可还原用户配置」能力。
             * W25 为 SPI 外设擦写（4KB 扇区），不暂停 CPU 总线，区别于内部 Flash
             * 擦除（1~2s 停顿），上电执行安全。 */
            memcpy(self->cfg.host_ip, app_tcp_client_get_host_ip(), 4);
            self->cfg.host_port = app_tcp_client_get_host_port();
            app_flash_ldi_save_config(&self->cfg);
        }

        /* 网络应用（pl_net_set_ip / app_tcp_server_set_port）已移交
         * app_net_boot_apply（2026-08-21 解耦），此处仅装载 cfg 与自愈 */
        self->cfg_valid = true;

    } else if (flash_valid) {
        /* ---- 分支 2：Sector1 空/损坏/记录无效或 net_cfg 非法 + W25 有效 → 以 W25 为准
         * 并回写自愈 ----（2026-08-21 修订：升级中间态不再落入本分支——Sector1 有效且
         * net_cfg 合法即走分支 1，见上方 net_valid 判定）
         * cfg 全字段取自 W25；回写 Sector1 由 app_board_net_cfg_update 内部处理
         * 全部分支（空/损坏完整初始化自愈；中间态下 update_sta/app_info 保留、仅
         * net_cfg 写入（2026-08-18 语义修订），见 doc/07 §10 方案 1）。 */
        memcpy(self->cfg.device_ip, flash_cfg.device_ip, sizeof(self->cfg.device_ip));
        self->cfg.device_port = flash_cfg.device_port;
        memcpy(self->cfg.netmask, flash_cfg.netmask, sizeof(self->cfg.netmask));
        memcpy(self->cfg.gateway, flash_cfg.gateway, sizeof(self->cfg.gateway));
        memcpy(self->cfg.host_ip, flash_cfg.host_ip, sizeof(self->cfg.host_ip));
        self->cfg.host_port = flash_cfg.host_port;
        memcpy(self->cfg.lane_hex, flash_cfg.lane_hex, sizeof(self->cfg.lane_hex));
        memcpy(self->cfg.cert, flash_cfg.cert, sizeof(self->cfg.cert));

        /* module: 以编译期 type 做键匹配，同步 index */
        for (uint8_t i = 0; i < self->cfg.module_count; i++) {
            for (uint8_t j = 0; j < flash_cfg.module_count; j++) {
                if (flash_cfg.modules[j].device_type == self->cfg.modules[i].device_type) {
                    self->cfg.modules[i].device_index = flash_cfg.modules[j].device_index;
                    break;
                }
            }
        }

        /* TCP Client 远端应用；netif / TCP Server 口已移交
         * app_net_boot_apply 统一应用（2026-08-21 解耦） */
        app_tcp_client_set_remote(self->cfg.host_ip, self->cfg.host_port);
        self->cfg_valid = true;

        /* 回写 Sector1 自愈（升级中间态仅 net_cfg 写入，update_sta/app_info 保留）。
         * 方案 B（2026-08-21）：W25 device_port 写 TCP 业务口，udp_port 保留现值
         * （Sector1 无效时 get 失败，用出厂默认 20103）。 */
        app_board_net_cfg_t cur_cfg;
        uint32_t udp_port = (app_board_net_cfg_get(&cur_cfg) == 0) ? cur_cfg.udp_port : 20103U;
        (void)app_board_net_cfg_update(self->cfg.device_ip, self->cfg.netmask,
                                       self->cfg.gateway, self->cfg.device_port,
                                       udp_port);

    } else {
        /* ---- 分支 3：Sector1 与 W25 皆空/无效 → 统一出厂默认 ----
         * 默认 IP 取 pl_net 上电值（三固件统一 192.168.114.200/24，doc/07 §11），
         * 端口取 TCP Server 编译期默认。cfg 默认填充仅保证 LDI 上下文自洽，
         * netif 应用由后续 app_net_boot_apply 统一执行（2026-08-21 解耦）。 */
        uint8_t ip[4] = {0}, mask[4] = {0}, gw[4] = {0};
        pl_net_get_ip(ip, mask, gw);
        memcpy(self->cfg.device_ip, ip, sizeof(ip));
        memcpy(self->cfg.netmask, mask, sizeof(mask));
        memcpy(self->cfg.gateway, gw, sizeof(gw));
        self->cfg.device_port = app_tcp_server_get_port();
        memcpy(self->cfg.host_ip, app_tcp_client_get_host_ip(), 4);
        self->cfg.host_port = app_tcp_client_get_host_port();
        self->cfg_valid = true;
        /* 不写任何 Flash：配置由 0AH 命令在出厂配置阶段写入，写入时网络负载低，
         * 风险可控；内部 Flash 擦除 1~2s 的 CPU 停顿不得出现在上电路径。 */
    }
}

/* ---- 协议自注册：仅 RJ45（TCP/UDP 逻辑通道共享同一物理 RB，链式探测）---- */
static proto_mask_t s_ldi_mask_tcp_server;
static proto_mask_t s_ldi_mask_tcp_client;
static proto_mask_t s_ldi_mask_udp;

static void ldi_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RJ45, RB_SIZE_RJ45);
    if (rb == nullptr)
        return;

    /* 每逻辑通道独立 mask，共享同一 RJ45 RB（禁止单 mask 绑多通道） */
    s_ldi_mask_tcp_server = app_proto_register(ldi_probe_frame, rb);
    if (s_ldi_mask_tcp_server != 0)
        app_proto_bind_channel(s_ldi_mask_tcp_server, CH_ID_TCP_SERVER);

    s_ldi_mask_tcp_client = app_proto_register(ldi_probe_frame, rb);
    if (s_ldi_mask_tcp_client != 0)
        app_proto_bind_channel(s_ldi_mask_tcp_client, CH_ID_TCP_CLIENT);

    s_ldi_mask_udp = app_proto_register(ldi_probe_frame, rb);
    if (s_ldi_mask_udp != 0)
        app_proto_bind_channel(s_ldi_mask_udp, CH_ID_UDP);

    if (s_ldi_mask_tcp_server == 0 && s_ldi_mask_tcp_client == 0 && s_ldi_mask_udp == 0)
        return;

    /* 上下文初始化必须在创建任务之前，保证 IP/端口在通道任务启动前就绪 */
    ldi_ctx_init(&g_ldi);
    ldi_device_init();

    /* 保护 tx_buf，ldi_handle_task 和 ldi_timer_task 共享 */
    const osMutexAttr_t tx_lock_attr = {.name = "ldi_tx_lock", .attr_bits = osMutexPrioInherit};
    g_ldi.tx_lock                    = osMutexNew(&tx_lock_attr);

    g_ldi_task_handle       = osThreadNew(ldi_handle_task, nullptr, &ldi_task_attr);
    g_ldi_timer_task_handle = osThreadNew(ldi_timer_task, nullptr, &ldi_timer_task_attr);
}
sw_app_initcall(ldi_module_init);

osMessageQueueId_t g_ldi_msg_queue;
osThreadId_t g_ldi_task_handle;
const osThreadAttr_t ldi_task_attr = {
    .name       = "ldi_handle_task",
    .stack_size = 256 * 4, /* 帧缓冲为 static；原 2KB 偏大 */
    .priority   = (osPriority_t)osPriorityNormal,
};

osThreadId_t g_ldi_timer_task_handle;
const osThreadAttr_t ldi_timer_task_attr = {
    .name       = "ldi_timer_task",
    .stack_size = 256 * 4, /* 周期上报路径；原 2KB 偏大 */
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
    g_ldi_msg_queue = osMessageQueueNew(LDI_QUEUE_DEPTH, LDI_MSG_SIZE, &s_ldi_queue_attr);
    if (s_ldi_mask_tcp_server != 0)
        app_proto_set_frame_queue(s_ldi_mask_tcp_server, g_ldi_msg_queue);
    if (s_ldi_mask_tcp_client != 0)
        app_proto_set_frame_queue(s_ldi_mask_tcp_client, g_ldi_msg_queue);
    if (s_ldi_mask_udp != 0)
        app_proto_set_frame_queue(s_ldi_mask_udp, g_ldi_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_ldi_msg_queue, msg, NULL, osWaitForever))
            continue;

        ldi_frame_t *ldi_frame   = (ldi_frame_t *)msg->data;
        ldi_req_head_t *req_head = (ldi_req_head_t *)ldi_frame->data_crc;

        /* 从已读帧中保存序号用于响应回显（从 probe 移至此处，消除 probe 副作用） */
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
            g_ldi_cmd_table[idx](msg->ch, ldi_frame->data_crc);
    }
}

/* ================================================================
 *  帧探测
 * ================================================================ */

/**
 * @brief LDI 帧探测（PURE 函数契约：只 peek，无副作用，不修改全局状态）
 *
 * 首字节快速拒绝：avail 为 0 时直接 FAKE；>= 1 字节时 peek 首字节判断 STX[0]。
 * g_ldi.rsp_seq 的赋值已移至 ldi_handle_task（读帧之后）。
 */
proto_probe_sta_t ldi_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    uint32_t avail = rb_avail(buff, nullptr);

    /* 首字节快速拒绝：无数据或首字节非 0xFF → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(buff, 0, &first_byte, 1, nullptr);
    if (first_byte != 0xFF)
        return PROTO_PROBE_FAKE;

    /* 等到定长帧头（STX[2]/VER/SEQ/LEN[4] = 8 字节），再按 LEN 等完整帧 */
    if (avail < sizeof(ldi_frame_t))
        return PROTO_PROBE_WAIT;

    static uint8_t mem_pool[512] = {0};
    memset(mem_pool, 0, sizeof(mem_pool));
    rb_peek(buff, 0, mem_pool, avail > sizeof(mem_pool) ? sizeof(mem_pool) : avail, nullptr);
    ldi_frame_t *frame = (ldi_frame_t *)mem_pool;

    /* STX[1] 校验 */
    if (frame->stx[1] != 0xFF)
        return PROTO_PROBE_FAKE;
    if (frame->ver != 0x00)
        return PROTO_PROBE_FAKE;

    /* 注意：此处不再写 g_ldi.rsp_seq，移到 ldi_handle_task 读帧后 */

    uint32_t data_len = ((uint32_t)frame->len[0] << 24) | ((uint32_t)frame->len[1] << 16) |
                        ((uint32_t)frame->len[2] << 8) | (uint32_t)frame->len[3];
    /* CQ 12B 二进制重启/搜索帧（FF FF 00 00 00 00 00 02 + 2B DATA + CRC16-XMODEM，
     * len 恒为 2）与 LDI 共占 10011（CH_ID_UDP），且其 CRC 恰好通过 LDI 校验——
     * 若按 LDI 帧认领会吞掉 CQ 搜索/重启请求（CQ probe 链序在 LDI 之后失收）。
     * LDI 合法 DATA：21H 搜索 = 1B、其余命令 ≥ 20B 头，len==2 无合法命令
     * （doc/03 PartB Q23 实态修正：原「CQ 帧 len 域全 0 → FAKE」与实态不符）。
     * 明确 FAKE 放行给链序靠后的 CQ probe。 */
    if (data_len == 2U)
        return PROTO_PROBE_FAKE;

    /* DATA 至少要有 CmdType；上限保护，避免脏 LEN 越界 */
    if (data_len < 1 || data_len > (sizeof(mem_pool) - sizeof(ldi_frame_t) - 2))
        return PROTO_PROBE_FAKE;

    uint32_t need = sizeof(ldi_frame_t) + data_len + 2;
    if (avail < need)
        return PROTO_PROBE_WAIT;
    if (need > sizeof(mem_pool))
        return PROTO_PROBE_FAKE;

    /* 完整帧已齐，再 peek 一次确保 CRC 落在 mem_pool 内 */
    rb_peek(buff, 0, mem_pool, need, nullptr);
    frame = (ldi_frame_t *)mem_pool;

    uint16_t frame_crc = ((uint16_t)frame->data_crc[data_len] << 8) | frame->data_crc[data_len + 1];
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
            /* data_len 下限：身份字段必须完整落在帧内（标准头 cert 偏移 10 + 8 = 18B；
             * 1BH 头 cert 偏移 14 + 8 = 22B）。不足视为畸形帧直接 SKIP 整帧，
             * 不再对帧尾之外的垃圾字节做 memcmp 身份校验。 */
            if (data_len < (uint32_t)cert_off + sizeof(g_ldi.cfg.cert)) {
                *total_len = (uint32_t)(sizeof(ldi_frame_t) + data_len + 2);
                return PROTO_PROBE_SKIP;
            }

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

        vms_timer_poll(); /* VMS 定时清屏 — 不受通道状态影响 */
        ldi_device_timer_poll(); /* E6 KeepTime / E8 KeepTime */

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
