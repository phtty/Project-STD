/**
 * @file    app_cdstd_cmd.c
 * @brief   折叠屏创迪标准协议处理
 */

#include "app_cdstd.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "crc_utils.h"

#include "pl_net.h"
#include "pl_task.h"
#include "pl_mem.h"

#include "app_rs485.h"
#include "app_udp.h"
#include "app_cdstd_cfg.h"
#include "app_cdstd_cmd.h"

/* ---- 队列和缓冲区资源静态分配 ---- */
#define CDSTD_PAYLOAD_MAX (512U)
#define CDSTD_FRAME_MAX   (sizeof(app_cdstd_frame_t) + CDSTD_PAYLOAD_MAX + 2U)
#define CDSTD_MSG_SIZE    (sizeof(app_dispatch_msg_t) + CDSTD_FRAME_MAX)

/*
 * 协议私有资源声明，包括帧消息队列、消息队列缓冲区、队列属性
 */
static StaticQueue_t s_cdstd_queue_cb;
static uint8_t s_cdstd_queue_buf[2 * CDSTD_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_cdstd_queue_attr = {
    .name    = "proto_cdstd_queue",
    .cb_mem  = &s_cdstd_queue_cb,
    .cb_size = sizeof(s_cdstd_queue_cb),
    .mq_mem  = s_cdstd_queue_buf,
    .mq_size = sizeof(s_cdstd_queue_buf),
};

static osMessageQueueId_t s_cdstd_msg_queue;
static osThreadId_t s_cdstd_task_handle;
static const osThreadAttr_t s_cdstd_task_attr = {
    .name       = "app_cdstd_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};
void app_cdstd_task(void *argument);

/*
 * 构造协议上下文资源，包括协议缓冲区、协议探针前向声明、协议控制块声明
 */
RB_DEFINE_ATTR(s_cdstd_rb, 2112, PL_CCMRAM); /**< max(2 × 最长帧 523, 单次最大写入 2048 + 1) */
app_pcb_probe_state_t app_cdstd_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src, uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len, uint8_t *aux);
static const app_pcb_ops_t s_cdstd_ops = {.probe = app_cdstd_probe_frame};
static app_pcb_t s_cdstd_pcb           = {
    .name        = "cdstd",
    .ops         = &s_cdstd_ops,
    .rb          = &s_cdstd_rb,
    .payload_max = CDSTD_FRAME_MAX,
};

static_assert(CDSTD_FRAME_MAX <= FRAME_DATA_MAX_LEN, "CDSTD 最长帧超过框架暂存上限");

/* 上电应用网络配置：记录有效才改，无效保持 pl_net 编译期默认。
   · 只读不写：上电阶段落盘会暂停总线（擦除 1~2s）、损坏 LwIP 时序；
   · 时机：本函数在 sw_post 的 initcall_run_sw 里跑，早于 app_boot 的 app_udp_start()，
     端口才能赶在 UDP 首绑之前生效；
   · 提成独立函数：host 套件可直调覆盖"有效 / 无效 / 端口 0"三条路径（见 test_cdstd）。 */
static void _cdstd_apply_net_cfg(void)
{
    app_cdstd_net_cfg_t net = {0};
    if (!app_cdstd_load_config(&net)) return;

    pl_net_set_ip(net.ip, net.mask, net.gw);

    uint16_t port = (uint16_t)((net.port[0] << 8) | net.port[1]);
    if (0 != port) app_udp_set_port(port);
}

[[maybe_unused]] static void _cdstd_module_init(void)
{
    _cdstd_apply_net_cfg();

    rb_init(&s_cdstd_rb, "cdstd");

    s_cdstd_msg_queue = osMessageQueueNew(2, CDSTD_MSG_SIZE, &s_cdstd_queue_attr);
    s_cdstd_pcb.queue = s_cdstd_msg_queue;

    app_dispatch_bind(&s_cdstd_pcb, app_udp_ccb());
    app_dispatch_bind(&s_cdstd_pcb, app_rs485_ccb());

    s_cdstd_task_handle = pl_task_new(app_cdstd_task, nullptr, &s_cdstd_task_attr);
}
sw_post_initcall(_cdstd_module_init);

/**
 * @brief 协议处理任务
 */
void app_cdstd_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[CDSTD_MSG_SIZE] = {0};
    app_dispatch_msg_t *msg                 = (app_dispatch_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_cdstd_msg_queue, msg, NULL, 100))
            continue;

        app_cdstd_frame_t *frame_data = (app_cdstd_frame_t *)msg->data;

        uint8_t cmd = msg->aux;
        if (cmd >= CDSTD_CMD_COUNT)
            continue;

        uint16_t len = (uint16_t)(frame_data->len[0] << 8) | frame_data->len[1];
        g_cdstd_cmd_table[cmd](msg->ccb, frame_data->data_crc, len - 1); // 长度-1的原因见协议探针内注释
    }
}

/**
 * @brief   CDSTD 帧探测函数
 *
 * 检测 0xFFFF 帧头 → 校验 len 合法性 → CRC16 验证 → 返回完整帧长度
 * @retval APP_PCB_PROBE_STATE_READY  帧就绪
 * @retval APP_PCB_PROBE_STATE_WAIT   数据不足
 * @retval APP_PCB_PROBE_STATE_FAKE   伪帧头，跳过 1 字节重试
 * @retval APP_PCB_PROBE_STATE_SKIP   帧长超出暂存区，无法校验，交由框架整帧跳过
 */
app_pcb_probe_state_t app_cdstd_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src, uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len, uint8_t *aux)
{
    (void)src;
    (void)ccb;
    const ring_buffer_t *buff = self->rb;
    uint32_t available        = rb_avail(buff, nullptr);

    // 环形缓冲区里的数据不足一帧最小的长度
    if (available < sizeof(app_cdstd_frame_t) + 2)
        return APP_PCB_PROBE_STATE_WAIT;

    // 伪造枕头
    uint16_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 2, nullptr);
    if (head != CDSTD_HEAD)
        return APP_PCB_PROBE_STATE_FAKE;

    /*
     * 这个协议比较奇葩，不知道是哪个傻屌设计的，数据载荷长度部分居然还包含命令字段
     * 不知道为啥非要包含这个命令字段，理论上讲这个协议的所有帧都有命令字段，只有数据载荷是变长的
     * 在变长协议中，长度字段描述变长字段的大小按理说就足够了，非要加入一个定长的字段，完全意义不明
     */
    uint8_t len[2]       = {0};
    uint16_t payload_len = 0;
    rb_peek(buff, 6, len, 2, nullptr);
    payload_len |= (uint16_t)(len[0] << 8);
    payload_len |= len[1];
    if (payload_len > CDSTD_PAYLOAD_MAX + 1)
        return APP_PCB_PROBE_STATE_FAKE;

    // 计算全帧长度
    uint32_t full_bytes = (sizeof(app_cdstd_frame_t) + payload_len + 1);

    // 整帧到齐闸门
    if (available < full_bytes)
        return APP_PCB_PROBE_STATE_WAIT;

    // 全帧长度超过解析缓冲区上限，无法解析
    if (full_bytes > scratch_size) {
        *total_len = full_bytes;
        return APP_PCB_PROBE_STATE_SKIP;
    }

    rb_peek(buff, 0, scratch, scratch_size, nullptr);
    app_cdstd_frame_t *ptemp = (app_cdstd_frame_t *)scratch;

    // 进行crc校验
    uint16_t crc       = crc16_xmodem(scratch + 2, payload_len + 6);
    uint16_t frame_crc = (uint16_t)((uint16_t)ptemp->data_crc[payload_len] | ((uint16_t)ptemp->data_crc[payload_len - 1] << 8));
    if (crc != frame_crc)
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = full_bytes;
    *aux       = ptemp->cmd & 0x0F;
    return APP_PCB_PROBE_STATE_READY;
}