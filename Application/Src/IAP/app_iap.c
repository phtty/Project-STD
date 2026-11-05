/**
 * @file    app_iap.c
 * @brief   IAP 固件升级协议处理
 *
 * 帧格式: 0x5A5A5A5A (4B) | seq (4B) | cmd (4B) | len (4B) | data | CRC32 (4B)
 * 承载于 RS485 + UDP，配置存储于 Flash 0x08004000。
 */

#include "app_iap.h"

#include "FreeRTOS.h"
#include "initcall.h"

#include "pl_crc.h"
#include "pl_net.h"
#include "pl_task.h"

#include "app_rs485.h"
#include "app_udp.h"
#include "app_iap_cfg.h"
#include "app_iap_cmd.h"

#include "app_screen.h"
#include "pl_rtc.h"
#include "pl_sys.h"
#include "pl_iwdg.h"

#include <string.h>

/* ---- IAP 记录镜像同步 ----
 * IAP 记录里的 net_cfg 唯一职责 = 镜像 main app 当前使用的网络参数（供 Recovery 上报）。
 * 触发有两条：pl_net 的 IP 变更监听（任何协议调 pl_net_set_ip 都会走到），
 * 以及上电对账一次（兜底"本上电周期没有 set_ip 调用"的场景）。
 *
 * 实际写入**延迟到本任务里做**：内部 Flash 擦除会硬停总线，不该阻塞改 IP 的调用方。
 *
 * 上电对账的取源是**运行态**（app_flash_iap_sync_from_runtime → pl_net_get_ip），
 * 因此必须跑在"各协议把自己的地址应用到运行态"之后。这个顺序由启动结构保证、
 * 不依赖任何初值：`_init_task` 以 osPriorityHigh 同步跑完整个 initcall_run_sw，
 * 而 app_iap_task 是 osPriorityNormal —— LDI 的 sw_post(4) initcall
 * （app_ldi_ctx_init，两条分支都调 pl_net_set_ip）必定先于本任务首次取队列超时
 * （100ms）完成，也就先于第一次对账。若把本任务提到 High、或让某个 initcall 让出
 * CPU，上电对账会读到编译期默认值并把记录写歪（现场"LDI 报的 / 实际运行的 /
 * 记录里的"三者漂移），届时正确的修法是"把对账推迟到 LDI 应用之后"（例如改由
 * IP 变更监听被动触发），而不是去调这个初值。 */
static volatile bool s_sync_pending = true; /* 初始 true：上电必须对账一次 */

static void _iap_ip_change_cb(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    (void)ip;
    (void)mask;
    (void)gw;
    s_sync_pending = true;
}
#include "pl_mem.h"

/* ---- proto_iap_queue 静态分配 ---- */
#define IAP_PAYLOAD_MAX (1044U) /* FRAME_MAX_LEN * 4 */
#define IAP_MSG_SIZE    (sizeof(app_dispatch_msg_t) + IAP_PAYLOAD_MAX)

static StaticQueue_t s_iap_queue_cb;
static uint8_t s_iap_queue_buf[2 * IAP_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_iap_queue_attr = {
    .name    = "proto_iap_queue",
    .cb_mem  = &s_iap_queue_cb,
    .cb_size = sizeof(s_iap_queue_cb),
    .mq_mem  = s_iap_queue_buf,
    .mq_size = sizeof(s_iap_queue_buf),
};

static osMessageQueueId_t s_iap_msg_queue;
static osThreadId_t s_iap_task_handle;
static const osThreadAttr_t s_iap_task_attr = {
    .name       = "app_iap_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ----
 * RB 容量必须**严格大于**传输层单次最大写入：ring buffer 保留一个空槽来区分满/空
 * （rb_space = size - avail - 1），所以装下 N 字节需要 size ≥ N+1。取相等的值会
 * 让 rb_write 静默截掉尾巴那一字节，帧尾被切 → CRC 失败 → 整帧丢。
 * 本协议承载 RS485/RS232（DMA 单次可达 2048）与 UDP，故取 2112 = 2048 + 余量。 */
RB_DEFINE_ATTR(s_iap_rb, 2112, PL_CCMRAM); /**< max(2 × 最长帧 1044, 单次最大写入 2048 + 1) */

static const app_pcb_ops_t s_iap_ops = {.probe = app_iap_probe_frame, .cmd_of = app_iap_frame_cmd};

static app_pcb_t s_iap_pcb = {
    .name        = "iap",
    .ops         = &s_iap_ops,
    .rb          = &s_iap_rb,
    .payload_max = IAP_PAYLOAD_MAX,
};

static_assert(IAP_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "IAP 最长帧超过框架暂存上限");

app_pcb_t *app_iap_pcb(void)
{
    return &s_iap_pcb;
}

/* ---- 协议模块自注册 ---- */
[[maybe_unused]] static void _iap_module_init(void)
{
    rb_init(&s_iap_rb, "iap");

    /* 队列在 initcall 内建好：通道任务可能早于协议任务首次运行就投递帧，
       晚建会留下"向空队列投递"的窗口 */
    s_iap_msg_queue = osMessageQueueNew(2, IAP_MSG_SIZE, &s_iap_queue_attr);
    s_iap_pcb.queue = s_iap_msg_queue;

    /* 绑定两块板都有的通道。板级特有的通道（如 3833024 的两路 RS232）不在这里绑：
       由各板的板级文件调 app_iap_pcb() 自行绑定，否则共享文件要认识每块板的外设。 */
    app_dispatch_bind(&s_iap_pcb, app_rs485_ccb());
    app_dispatch_bind(&s_iap_pcb, app_udp_ccb());

    /* 注册 IP 变更监听：任何协议改 IP 都触发 IAP 记录的镜像同步 */
    pl_net_register_ip_listener(_iap_ip_change_cb);

    /* 创建协议处理任务 */
    s_iap_task_handle = pl_task_new(app_iap_task, nullptr, &s_iap_task_attr);
}
/* sw_post(4)：让"读配置"排在"加载配置"之后（cfg 调度器在 sw_app(3) 执行加载遍）。
   同层 initcall 的相对次序 = 链接顺序 = 构建清单文件次序，移动源文件即改变，
   不能用同层顺序表达依赖。 */
sw_post_initcall(_iap_module_init);

/* ================================================================
 *  任务实现
 * ================================================================ */

uint32_t app_iap_frame_cmd(const uint8_t *frame, uint16_t len)
{
    if (len < sizeof(app_iap_frame_t)) return 0U;
    return ((const app_iap_frame_t *)frame)->cmd;
}

void app_iap_send_internal(uint32_t cmd, uint32_t seq, uint32_t len, const uint32_t *data)
{
    static uint32_t tx[FRAME_MAX_LEN] = {0};

    app_iap_frame_t *f = (app_iap_frame_t *)tx;
    f->head            = 0x5A5A5A5A;
    f->seq             = seq;
    f->cmd             = cmd;
    f->len             = len;
    if (len != 0U && data != NULL)
        memcpy(f->data_crc, data, len * sizeof(uint32_t));
    f->data_crc[len] = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)tx, sizeof(app_iap_frame_t) + len * sizeof(uint32_t));

    app_ccb_send_to(app_rs485_ccb(), NULL, (uint8_t *)tx,
                    sizeof(app_iap_frame_t) + len * sizeof(uint32_t) + sizeof(uint32_t));
}

/** @brief 内部 485 请求（0x4Cxx）：main app 只支持 0x10 查询与 0x14 进 Recovery */
static void _iap_handle_internal(app_iap_frame_t *f)
{
    app_flash_iap_sys_info_t cfg = *g_iap_sys_info;
    uint32_t id                  = f->cmd & 0xFFU;

    switch (id) {
    case 0x10U: {
        /* 主卡不由本机应答：避免主/从两张卡都回 0x10 */
        if (app_screen_is_master()) return;
        uint32_t out[4] = {0U /* main app */, cfg.app_info.size, cfg.app_info.crc32, cfg.update_status};
        app_iap_send_internal(0x0000BC10U, f->seq, 4U, out);
        break;
    }
    case 0x14U: {
        if (app_screen_is_master()) {
            uint32_t st = 2U;
            app_iap_send_internal(0x0000BC14U, f->seq, 1U, &st);
            return;
        }
        cfg.role = APP_FLASH_IAP_ROLE_SLAVE;
        app_flash_iap_edit_config(&cfg);
        pl_rtc_bkup_write(pl_rtc_get_handle(), 0, FLAG_FORCE_UPDATE);
        uint32_t st = 0U;
        app_iap_send_internal(0x0000BC14U, f->seq, 1U, &st);
        osDelay(50);
        pl_iwdg_refresh(pl_iwdg_get_handle());
        pl_system_reset();
        break;
    }
    default: {
        uint32_t st = 2U;
        app_iap_send_internal(0x0000BC00U | id, f->seq, 1U, &st);
        break;
    }
    }
}

/** @brief IAP 协议处理任务：阻塞等待帧队列 → 按 cmd 字段查表分派到命令处理函数 */
void app_iap_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[IAP_MSG_SIZE];
    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)_msg_buf;

    for (;;) {
        /* 带超时地取帧：空闲窗口用来做 IAP 记录的镜像同步（延迟写，不阻塞改 IP 的调用方） */
        if (osOK != osMessageQueueGet(s_iap_msg_queue, msg, NULL, 100)) {
            if (s_sync_pending) {
                s_sync_pending = false;
                app_flash_iap_sync_from_runtime(); /* 内部内容去重，无变化零擦写 */
            }
            continue;
        }

        app_iap_frame_t *frame_data = (app_iap_frame_t *)msg->data;

        /* 按完整命令码的方向字节路由：
         *   0x00004Bxx 上位机 → 命令表（低字节索引，必须先做范围检查）
         *   0x00014Cxx 主卡→从卡 → 内部请求（0x10/0x14 在 main app 也要应答）
         *   其余（含 0x0000BCxx）忽略 */
        if ((frame_data->cmd & 0xFFFF0000U) != 0U) continue;

        uint32_t dir = (frame_data->cmd >> 8) & 0xFFU;
        if (dir == 0x4BU) {
            uint8_t cmd = (uint8_t)(frame_data->cmd & 0xFFU);
            if (cmd >= IAP_CMD_COUNT) continue;
            g_iap_cmd_table[cmd](msg->ccb, frame_data);
        } else if (dir == 0x4CU) {
            /* 内部命令只认 RS485 通道：网口上来的 0x4C 一律当伪造，避免被一条
               UDP 包骗去改角色/复位 */
            if (msg->ccb == app_rs485_ccb())
                _iap_handle_internal(frame_data);
        }
    }
}

/**
 * @brief   IAP 帧探测函数
 *
 * 检测 0x5A5A5A5A 帧头 → 校验 len 合法性 (<=256) → CRC32 验证 → 返回完整帧长度
 * @retval APP_PCB_PROBE_STATE_READY  帧就绪
 * @retval APP_PCB_PROBE_STATE_WAIT   数据不足
 * @retval APP_PCB_PROBE_STATE_FAKE   伪帧头，跳过 1 字节重试
 * @retval APP_PCB_PROBE_STATE_SKIP   帧长超出暂存区，无法校验，交由框架整帧跳过
 */
app_pcb_probe_state_t app_iap_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                          uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                          uint8_t *aux)
{
    (void)src; /* 本协议不区分来源 */
    (void)ccb;
    const ring_buffer_t *buff = self->rb;
    uint32_t available        = rb_avail(buff, nullptr);

    /* min size check */
    if (available < 4) return APP_PCB_PROBE_STATE_WAIT;

    /* frame header check */
    uint32_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 4, nullptr);
    if (head != FRAME_HEAD)
        return APP_PCB_PROBE_STATE_FAKE;

    /* payload len sanity check (protocol max 256) */
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        rb_peek(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4, nullptr);
        if (payload_len > 256)
            return APP_PCB_PROBE_STATE_FAKE;
    } else {
        return APP_PCB_PROBE_STATE_WAIT;
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    /* consecutive header check: if insufficient data but another 0x5A in range, skip */
    if (available < full_bytes) {
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            rb_peek(buff, i, (uint8_t *)&next_head, 4, nullptr);
            if (next_head == FRAME_HEAD)
                return APP_PCB_PROBE_STATE_FAKE;
        }
        return APP_PCB_PROBE_STATE_WAIT;
    }

    /* 暂存区容量校验：payload_len 已被限制在 256，full_bytes 最大 1044 = 框架暂存上限。
       此处显式拦截是为了暂存区一旦被调小即在此拦下，而不是越界写。 */
    if (full_bytes > scratch_size) {
        *total_len = full_bytes;
        return APP_PCB_PROBE_STATE_SKIP;
    }

    /* CRC32 validation（scratch 由框架提供且 4 字节对齐，可直接按 app_iap_frame_t 访问）*/
    rb_peek(buff, 0, scratch, scratch_size, nullptr);
    app_iap_frame_t *ptemp = (app_iap_frame_t *)scratch;

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), scratch, (ptemp->len + 4) * 4);
    if (crc != ptemp->data_crc[ptemp->len])
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = full_bytes;
    *aux       = ptemp->cmd & 0xFF;
    return APP_PCB_PROBE_STATE_READY;
}
