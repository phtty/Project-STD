#include "app_ldi_cmd.h"

#include <time.h>

#include "crc_utils.h"
#include "app_ldi_cfg.h"
#include "app_iap_cfg.h"
#include "pl_net.h"
#include "pl_rtc.h"
#include "pl_sys.h"
#include "app_vms_ctrl.h"
#include "app_udp.h"
#include "app_tcp_server.h"

/** 请求帧序号（由 ldi_probe_frame 保存，用于响应帧回显） */

// ============================================================================
// 各命令 DATA 域完整帧结构定义
// ============================================================================
//
// 以下 struct 定义的是 ldi_frame_t.data_crc[] 中 DATA 段的完整布局,
// 均以 ldi_req_head_t 开头 (20字节通用头部), 后续字段视命令而定.
//
// 注: 这些 struct 仅用于描述帧结构, 实际解析时通过指针偏移遍历.

/**
 * 网络配置信息 — set_ip / rep_ip_rsp 共用
 *
 * 共 67 字节，不含 head 和 error_code。
 */
typedef struct [[gnu::packed]] {
    uint8_t host_ip[4];     // 上位机 IP1 (外设控制服务)
    uint8_t host_port[2];   // 上位机端口1
    uint8_t reserve1[39];   // 预留段 (含 ServerIP2~3, MQTT 全套参数)
    uint8_t ntp_ip[4];      // 对时服务器 IP
    uint8_t device_ip[4];   // 设备自身 IP
    uint8_t device_port[2]; // 设备端口号
    uint8_t gateway[4];     // 网关地址
    uint8_t netmask[4];     // 子网掩码
    uint8_t reserve2[2];    // 保留字节
} ldi_network_info_t;

/**
 * 设备 IP 信息设置 (0AH) DATA 域 — 共 87 字节 (含 head)
 * head(20B) + network_info(67B)
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    ldi_network_info_t net;
} cmd_set_ip_t;

/**
 * 设备参数配置 (0BH) DATA 域
 *
 * 对应协议第4.2.1节. 上位机→设备.
 * 帧布局: head(20B) | device_num(1B) | module[0] | ... | module[N-1]
 *
 * module 结构视 device_type 而定, 公共头见 ldi_module_head_t,
 * 各设备 payload 定义见 ldi_cfg_barrier_t / ldi_cfg_display_t 等
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head; // 通用头部, cmd_type=0BH
    uint8_t device_num;  // 功能模块数量 N
    uint8_t module[];    // module 序列 (柔性数组), 遍历时按 device_type 查表推进
} cmd_set_config_t;

/**
 * 设备重启 (0DH) DATA 域
 *
 * 对应协议第4.3.1节. 上位机→设备.
 * 帧布局: head(20B) | device_num(1B) | module[0] | ... | module[N-1]
 *
 * 每个 module = device_type(1B) + device_index(1B) + vendor[10],
 * 所有设备统一使用 ldi_reboot_payload_t
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head; // 通用头部, cmd_type=0DH
    uint8_t device_num;  // 功能模块数量 N
    uint8_t module[];    // module 序列 (柔性数组)
} cmd_reboot_t;

/**
 * 设备 IP 信息采集 (1DH) DATA 域
 *
 * 对应协议第4.4.1节. 上位机→设备, 查询设备当前网络配置.
 * 帧布局: 仅含通用头部 (无额外字段)
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head; // 通用头部, cmd_type=1DH
} cmd_rep_ip_t;

/**
 * 设备参数采集 (1EH) DATA 域
 *
 * 对应协议第4.5.1节. 上位机→设备, 查询设备当前业务参数.
 * 帧布局: 仅含通用头部 (无额外字段)
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head; // 通用头部, cmd_type=1EH
} cmd_rep_config_t;

// ============================================================================
// 响应帧 payload 结构体
// ============================================================================

/** 状态响应（通用 ACK + 可附带 payload）
 *
 *  set_ip_rsp/A0H, init_rsp/A1H 等单设备 ACK 直接使用。
 *  rep_ip/D1H 等查询响应将 ldi_network_info_t 放入 payload[]。
 *  status: 00H=成功, 01H=失败
 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t status;
    uint8_t payload[]; // 柔性数组，查询响应时携带业务数据
} ldi_status_rsp_t;

/** 多设备操作响应（set_config_rsp=B0H, reboot_rsp=D0H） */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t device_num;
    struct {
        uint8_t device_type;
        uint8_t device_index;
        uint8_t status;
    } modules[]; // N × 3 字节
} ldi_multi_rsp_t;

/** 初始化响应帧单个 module 信息（A1H: 设备→上位机）

    device_type(1) + device_index(1) + status(1) + software_version(2) +
    firmware_version(2) + protocol_version(2) + custom_init_len(1) = 定长 10 字节
    custom_init[] 为柔性数组，长度由 custom_init_len 指示。 */
typedef struct [[gnu::packed]] {
    ldi_module_head_t head;      // device_type + device_index (2B)
    uint8_t status;              // 功能模块状态: 00H=正常, 01H=异常
    uint8_t software_version[2]; // 软件版本号
    uint8_t firmware_version[2]; // 固件版本号
    uint8_t protocol_version[2]; // 接口协议版本号, 默认 00H
    uint8_t custom_init_len;     // 个性化初始化内容字节长度 S
    uint8_t custom_init[];       // 个性化初始化内容 (S 字节), 当前为空
} ldi_init_rsp_module_t;

static_assert(sizeof(ldi_init_rsp_module_t) == 10, "ldi_init_rsp_module_t fixed part must be 10 bytes");

/** 初始化响应帧 DATA 域（A1H: head + device_num + N×变长module） */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t device_num;
    uint8_t modules[]; // N × 变长模块
} ldi_init_rsp_t;

/** 参数采集响应帧 DATA 域（E1H: head + device_num + N×变长module）

    module 结构与 0BH 设置时一致，各设备 vendor 长度不同，
    详见 ldi_cfg_module_size()。构造时需动态拼接，不使用固定 struct 做 modules[] 元素。 */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t device_num;
    uint8_t modules[]; // 变长 module 序列，逐设备用 ldi_cfg_module_size() 推进
} ldi_rep_config_rsp_t;

/** 控制/查询响应 payload（ctrl_rsp=B1H，每个 module 4 字节） */
typedef struct [[gnu::packed]] {
    uint8_t device_type;
    uint8_t device_index;
    uint8_t device_func_type;
    uint8_t status; // 00H=成功, 01H=失败
} ldi_ctrl_rsp_payload_t;

/** 控制/查询响应帧 DATA 域（B1H，24 字节 ctrl_head + N×4 字节 modules） */
typedef struct [[gnu::packed]] {
    ldi_ctrl_head_t head; // 24 字节，8 字节时间戳
    uint8_t device_num;
    ldi_ctrl_rsp_payload_t modules[]; // N × 4 字节
} ldi_ctrl_rsp_t;

/** 设备搜索应答帧 DATA 域（0x12，UDP 广播，16 字节定长） */
typedef struct [[gnu::packed]] {
    uint8_t cmd_type;   // 0x12 = 设备搜索应答
    uint8_t ip[4];      // 设备自身 IP, 大端
    uint8_t port[2];    // 设备端口号, 大端
    uint8_t gateway[4]; // 网关地址, 大端
    uint8_t mask[4];    // 子网掩码, 大端
    uint8_t err_code;   // 00H=成功, 01H=失败
} ldi_search_rsp_t;

// ============================================================================
// module 尺寸查表
// ============================================================================
//
// 复合指令中每个 module 长度 = ldi_module_head_t(2B) + 设备特定 payload
// 以下函数按 device_type 返回 module 总字节数, 用于遍历时推进指针.
//
// 注意: 变长设备 (如 LPR 含图片上传地址) 的 module 长度需从帧内字段
//       推算, 查表返回 0 表示调用方需自行计算.

/**
 * 获取 0BH 参数配置命令中指定设备类型的 module 总长度
 *
 * @param type  device_type (E4H~EBH)
 * @return      module 总字节数 (含 module_head 的 2 字节),
 *              未知类型返回 0
 */
static uint8_t ldi_cfg_module_size(ldi_device_t type)
{
    switch (type) {
        case LDI_DEV_TYPE_BARRIER: // E4H: head(2) + filter_time(1) + vendor(10) = 13
            return sizeof(ldi_module_head_t) + sizeof(ldi_cfg_barrier_t);
        case LDI_DEV_TYPE_DISPLAY: // E6H: head(2) + font_line(1) + vendor(10) = 13
            return sizeof(ldi_module_head_t) + sizeof(ldi_cfg_display_t);
        case LDI_DEV_TYPE_LANE_SIGNAL: // E7H: head(2) + vendor(2) = 4
            [[fallthrough]];
        case LDI_DEV_TYPE_ALARM: // E8H: 同上, 共 4 字节
            [[fallthrough]];
        case LDI_DEV_TYPE_CANOPY_LIGHT: // EAH: 同上, 共 4 字节
            [[fallthrough]];
        case LDI_DEV_TYPE_FOG_LIGHT: // EBH: 同上, 共 4 字节
            return sizeof(ldi_module_head_t) + sizeof(ldi_cfg_signal_t);
        case LDI_DEV_TYPE_VMS: // E9H: head(2) + vendor(10) = 12
            return sizeof(ldi_module_head_t) + sizeof(ldi_cfg_vms_t);
        case LDI_DEV_TYPE_VOICE:
            return sizeof(ldi_module_head_t) + sizeof(ldi_cfg_voice_t);
        default:
            return 0; // 未知设备类型, 调用方应视为错误
    }
}

/**
 * @brief 获取 1BH 控制/查询命令中指定设备类型的 payload 定长部分长度
 *
 * @note 返回值已扣除 DeviceFuncType(1B), 即纯功能参数的长度.
 *       多功能设备 (Display/VMS) 根据 func_type 返回对应长度;
 *       text[] 等变长字段不计入, 由调用方根据 mod_len 推算.
 *
 * @param type      设备类型
 * @param func_type 功能编号 (01H=显示控制, 02H=清屏 等)
 * @return          payload 定长部分字节数 (不含 DeviceFuncType), 未知返回 0
 */
[[maybe_unused]] static uint8_t ldi_ctrl_payload_size(ldi_device_t type, uint8_t func_type)
{
    switch (type) {
        case LDI_DEV_TYPE_BARRIER: // E4H: status(1) = 1B
            return sizeof(ldi_ctrl_barrier_t) - 1;
        case LDI_DEV_TYPE_DISPLAY:                                        // E6H: 01H=4B(union)+text[], 02H=1B(clear_type)
            if (func_type == 0x01) return sizeof(ldi_ctrl_display_t) - 1; // 4
            if (func_type == 0x02) return 1;
            return 0;
        case LDI_DEV_TYPE_LANE_SIGNAL: // E7H: color(1) = 1B
            return sizeof(ldi_ctrl_signal_t) - 1;
        case LDI_DEV_TYPE_ALARM: // E8H: status(1)+work_mode(1)+keep_time(1) = 3B
            return sizeof(ldi_ctrl_alarm_t) - 1;
        case LDI_DEV_TYPE_VMS:                                        // E9H: 01H=5B(union)+text[], 02H=1B(clear_type)
            if (func_type == 0x01) return sizeof(ldi_ctrl_vms_t) - 1; // 5
            if (func_type == 0x02) return 1;
            return 0;
        case LDI_DEV_TYPE_CANOPY_LIGHT: // EAH: color(1) = 1B
            return sizeof(ldi_ctrl_canopy_light_t) - 1;
        case LDI_DEV_TYPE_FOG_LIGHT: // EBH: status(1)+work_mode(1) = 2B
            return sizeof(ldi_ctrl_fog_light_t) - 1;
        case LDI_DEV_TYPE_VOICE: // F4H: type(1)+length(2) = 3B, data[] 由 length 推算
            return sizeof(ldi_ctrl_voice_t) - 1;
        default:
            return 0;
    }
}

// ============================================================================
// 编译期尺寸校验
// ============================================================================
// 确保 struct 布局与协议定义逐字节一致, 避免因 padding/对齐导致帧解析错位
static_assert(sizeof(ldi_module_head_t) == 2, "ldi_module_head_t must be 2 byte");

static_assert(sizeof(ldi_cfg_barrier_t) == 11, "ldi_cfg_barrier_t must be 11 byte");
static_assert(sizeof(ldi_cfg_display_t) == 11, "ldi_cfg_display_t must be 11 byte");
static_assert(sizeof(ldi_cfg_signal_t) == 2, "ldi_cfg_signal_t must be 3 byte");
static_assert(sizeof(ldi_cfg_vms_t) == 10, "ldi_cfg_vms_t must be 11 byte");

static_assert(sizeof(ldi_reboot_payload_t) == 10, "ldi_reboot_payload_t must be 10 byte");

static_assert(sizeof(ldi_ctrl_barrier_t) == 2, "ldi_ctrl_barrier_t must be 2 byte");
static_assert(sizeof(ldi_ctrl_display_t) == 5, "ldi_ctrl_display_t fixed part must be 5 byte");
static_assert(sizeof(ldi_ctrl_signal_t) == 2, "ldi_ctrl_signal_t must be 2 byte");
static_assert(sizeof(ldi_ctrl_alarm_t) == 4, "ldi_ctrl_alarm_t must be 4 byte");
static_assert(sizeof(ldi_ctrl_vms_t) == 6, "ldi_ctrl_vms_t fixed part must be 6 byte");
static_assert(sizeof(ldi_ctrl_canopy_light_t) == 2, "ldi_ctrl_canopy_light_t must be 2 byte");
static_assert(sizeof(ldi_ctrl_fog_light_t) == 3, "ldi_ctrl_fog_light_t must be 3 byte");
static_assert(sizeof(ldi_ctrl_voice_t) == 4, "ldi_ctrl_voice_t fixed part must be 4 byte");

// ============================================================================
// LDI 响应帧构造器
// ============================================================================
//
// ldi_send_response 处理 STX/VER/SEQ/LEN/CRC 这些共有帧结构，
// 各命令只需准备自己的 DATA 域 payload 即可。
//
// seq 规则：
//   请求-响应 → 回显请求帧的 seq（探头函数已保存到 g_ldi.rsp_seq）
//   主动上报 → 使用 g_ldi.rpt_seq，发送后自增

/** 设备主动上报帧序号计数器 */

/** 获取下一主动上报帧序号（0x10, 0x20, ... ，高半字节递增，协议 §3.2） */
static uint8_t ldi_next_rpt_seq(void)
{
    uint8_t seq = ++g_ldi.rpt_seq;
    return (seq << 4) & 0xF0; /* 0x10, 0x20, ... 0xF0 */
}

/** 响应帧拼装工作缓冲区（所有 LDI 命令复用） */

/**
 * @brief 构造并发送 LDI 响应帧
 * @param ch         回复目标通道
 * @param rsp_cmd    响应命令码（如 LDI_CMD_SET_IP_RSP = 0xA0）
 * @param seq        帧序号（回显请求帧的 seq）
 * @param payload    响应 DATA 域内容（可为 nullptr）
 * @param payload_len DATA 域长度（字节）
 */
static void ldi_send_response(channel_t *ch, uint8_t rsp_cmd, uint8_t seq, const uint8_t *payload, uint16_t payload_len)
{
    osMutexAcquire(g_ldi.tx_lock, osWaitForever);

    ldi_frame_t *frame = (ldi_frame_t *)g_ldi.tx_buf;

    frame->stx[0] = 0xFF;
    frame->stx[1] = 0xFF;
    frame->ver    = 0x00;
    frame->seq    = seq;
    frame->len[0] = (uint8_t)(payload_len >> 24);
    frame->len[1] = (uint8_t)(payload_len >> 16);
    frame->len[2] = (uint8_t)(payload_len >> 8);
    frame->len[3] = (uint8_t)payload_len;

    if (payload_len > 0 && payload != nullptr)
        memcpy(frame->data_crc, payload, payload_len);

    uint16_t crc                     = crc16_xmodem(&frame->ver, sizeof(*frame) - sizeof(frame->stx) + payload_len);
    frame->data_crc[payload_len]     = (uint8_t)(crc >> 8);
    frame->data_crc[payload_len + 1] = (uint8_t)(crc & 0xFF);

    channel_send(ch, g_ldi.tx_buf, sizeof(*frame) + payload_len + 2);
    osMutexRelease(g_ldi.tx_lock);
}

/** @brief 定长 payload 响应（编译期 sizeof，类型安全） */
#define LDI_RESPOND(ch, rsp_cmd, seq, payload) \
    ldi_send_response(ch, rsp_cmd, seq, (const uint8_t *)&(payload), sizeof(payload))

/** @brief 变长 payload 响应（调用方显式传额外长度） */
#define LDI_RESPOND_EX(ch, rsp_cmd, seq, payload, extra_len) \
    ldi_send_response(ch, rsp_cmd, seq, (const uint8_t *)&(payload), sizeof(payload) + (extra_len))

/** @brief 无 payload 响应（仅帧头，如简单 ACK） */
#define LDI_RESPOND_EMPTY(ch, rsp_cmd, seq) \
    ldi_send_response(ch, rsp_cmd, seq, nullptr, 0)

// ============================================================================
// 命令处理函数声明与分发表
// ============================================================================

static void cmd_set_ip(channel_t *ch, void *data);
static void cmd_set_config(channel_t *ch, void *data);
static void cmd_reboot(channel_t *ch, void *data);
static void cmd_rep_ip(channel_t *ch, void *data);
static void cmd_rep_config(channel_t *ch, void *data);
static void cmd_rsp_report(channel_t *ch, void *data);
static void cmd_rsp_cert(channel_t *ch, void *data);
static void cmd_update(channel_t *ch, void *data);
static void cmd_init(channel_t *ch, void *data);
static void cmd_ctrl(channel_t *ch, void *data);
static void cmd_rep_func(channel_t *ch, void *data);
static void cmd_search(channel_t *ch, void *data);

/**
 * LDI 命令处理函数分发表
 *
 * 按命令码数组索引: data[0] 即为 cmd_type, 但实际分发由 protocol.c
 * 根据 cmd_type 查表调用, 此处顺序应与 ldi_cmd_type_t 枚举值对应.
 */
const ldi_cmd_handler_fn_t g_ldi_cmd_table[] = {
    cmd_set_ip,     // 0AH  设备IP信息设置请求
    cmd_set_config, // 0BH  设备参数配置请求
    cmd_reboot,     // 0DH  设备重启请求
    cmd_rep_ip,     // 1DH  设备IP信息采集请求
    cmd_rep_config, // 1EH  设备参数采集请求
    cmd_rsp_report, // C0H  设备状态监控上报返回
    cmd_rsp_cert,   // E0H  设备验证申请返回
    cmd_update,     // 0FH  设备升级包下载请求
    cmd_init,       // 1AH  设备初始化请求
    cmd_ctrl,       // 1BH  控制/查询请求
    cmd_rep_func,   // 1CH  设备功能数据上报
    cmd_search,     // 21H  设备搜索请求 (UDP广播)
};

// ============================================================================
// 命令处理函数实现
// ============================================================================

/**
 * 处理 0AH 设备 IP 信息设置请求
 *
 * 上位机→设备, 用于出厂配置模式下设置设备网络参数.
 * data 指向 cmd_set_ip_t 结构.
 */
void cmd_set_ip(channel_t *ch, void *data)
{
    cmd_set_ip_t *info = data;

    memcpy(g_ldi.cfg.device_ip, info->net.device_ip, sizeof(g_ldi.cfg.device_ip));
    g_ldi.cfg.device_port = ((uint16_t)info->net.device_port[0] << 8) | info->net.device_port[1];
    memcpy(g_ldi.cfg.host_ip, info->net.host_ip, sizeof(g_ldi.cfg.host_ip));
    g_ldi.cfg.host_port = ((uint16_t)info->net.host_port[0] << 8) | info->net.host_port[1];
    memcpy(g_ldi.cfg.netmask, info->net.netmask, sizeof(g_ldi.cfg.netmask));
    memcpy(g_ldi.cfg.gateway, info->net.gateway, sizeof(g_ldi.cfg.gateway));
    g_ldi.cfg_valid = true;

    app_flash_ldi_save_config(&g_ldi.cfg);
    app_flash_iap_update_net_cfg(g_ldi.cfg.device_ip, g_ldi.cfg.netmask, g_ldi.cfg.gateway);

    ldi_status_rsp_t rsp = {.status = 0x00};
    ldi_build_rsp_head(&rsp.head, LDI_CMD_SET_IP_RSP);
    LDI_RESPOND(ch, LDI_CMD_SET_IP_RSP, g_ldi.rsp_seq, rsp);
}

/**
 * 处理 0BH 设备参数配置请求
 *
 * 上位机→设备, 解析复合指令的 module 序列，
 * 将每个模块的 device_type + device_index + vendor[10] 写入 Flash.
 */
static void cmd_set_config(channel_t *ch, void *data)
{
    ldi_req_head_t *head = (ldi_req_head_t *)data;
    uint8_t *ptr         = (uint8_t *)data + sizeof(ldi_req_head_t);
    uint8_t device_num   = *ptr++;

    /* 0BH 请求头部含车道编号和验证信息，与 module 数据一同持久化 */
    memcpy(g_ldi.cfg.lane_hex, head->lane_code, sizeof(g_ldi.cfg.lane_hex));
    memcpy(g_ldi.cfg.cert, head->cert_info, sizeof(g_ldi.cfg.cert));

    if (device_num == 0 || device_num > g_ldi.cfg.module_count) {
        ldi_status_rsp_t rsp = {.status = 0x01};
        ldi_build_rsp_head(&rsp.head, LDI_CMD_SET_PARA_RSP);
        LDI_RESPOND(ch, LDI_CMD_SET_PARA_RSP, g_ldi.rsp_seq, rsp);
        return;
    }

    /* 以编译期 cfg 中的 device_type 做键，匹配请求帧中的 module，同步 index 和 vendor */
    bool result = true;
    for (uint8_t i = 0; i < device_num; i++) {
        ldi_module_head_t *mod = (ldi_module_head_t *)ptr;
        uint8_t mod_size       = ldi_cfg_module_size((ldi_device_t)mod->device_type);

        if (mod_size == 0) {
            result = false;
            break;
        }

        int8_t cfg_idx = -1;
        for (uint8_t j = 0; j < g_ldi.cfg.module_count; j++) {
            if (g_ldi.cfg.modules[j].device_type == mod->device_type) {
                cfg_idx = (int8_t)j;
                break;
            }
        }
        if (cfg_idx < 0) {
            result = false;
            break;
        }

        g_ldi.cfg.modules[cfg_idx].device_index = mod->device_index;
        uint8_t vendor_len                      = mod_size - sizeof(ldi_module_head_t);
        if (vendor_len > sizeof(g_ldi.cfg.modules[cfg_idx].vendor))
            vendor_len = sizeof(g_ldi.cfg.modules[cfg_idx].vendor);
        memcpy(g_ldi.cfg.modules[cfg_idx].vendor, ptr + sizeof(ldi_module_head_t), vendor_len);

        ptr += mod_size;
    }

    if (result)
        app_flash_ldi_save_config(&g_ldi.cfg);

    ldi_status_rsp_t rsp = {.status = result ? 0x00 : 0x01};
    ldi_build_rsp_head(&rsp.head, LDI_CMD_SET_PARA_RSP);
    LDI_RESPOND(ch, LDI_CMD_SET_PARA_RSP, g_ldi.rsp_seq, rsp);
}

/**
 * 处理 0DH 设备重启请求
 *
 * 上位机→设备, 可按模块指定重启范围.
 * 本 MCU 不支持部分重启，发送响应帧后执行整机软件复位.
 */
static void cmd_reboot(channel_t *ch, void *data)
{
    (void)data;

    /* D0H 响应: head(20B) + ErrorCode(1B), 简单 ACK */
    ldi_status_rsp_t rsp = {.status = 0x00};
    ldi_build_rsp_head(&rsp.head, LDI_CMD_REBOOT_RSP);
    LDI_RESPOND(ch, LDI_CMD_REBOOT_RSP, g_ldi.rsp_seq, rsp);

    osDelay(100);      // 等待以太网发送完成
    pl_system_reset(); // 整机软件复位
}

/**
 * 处理 1DH 设备 IP 信息采集请求
 *
 * 上位机→设备, DATA 域仅含通用头部, 无额外字段.
 * 设备收到后应回复当前设备的网络配置 (D1H).
 */
static void cmd_rep_ip(channel_t *ch, void *data)
{
    uint8_t buf[sizeof(ldi_status_rsp_t) + sizeof(ldi_network_info_t)] = {0};

    ldi_status_rsp_t *rsp   = (ldi_status_rsp_t *)buf;
    ldi_network_info_t *net = (ldi_network_info_t *)rsp->payload;

    ldi_build_rsp_head(&rsp->head, LDI_CMD_GET_IP_RSP);
    rsp->status = 0x00;

    memcpy(net->device_ip, g_ldi.cfg.device_ip, sizeof(net->device_ip));
    net->device_port[0] = (uint8_t)(g_ldi.cfg.device_port >> 8);
    net->device_port[1] = (uint8_t)(g_ldi.cfg.device_port);
    memcpy(net->gateway, g_ldi.cfg.gateway, sizeof(net->gateway));
    memcpy(net->netmask, g_ldi.cfg.netmask, sizeof(net->netmask));
    memcpy(net->host_ip, g_ldi.cfg.host_ip, sizeof(net->host_ip));
    net->host_port[0] = (uint8_t)(g_ldi.cfg.host_port >> 8);
    net->host_port[1] = (uint8_t)(g_ldi.cfg.host_port);

    LDI_RESPOND_EX(ch, LDI_CMD_GET_IP_RSP, g_ldi.rsp_seq, *rsp, sizeof(ldi_network_info_t));
}

/**
 * 处理 1EH 设备参数采集请求
 *
 * 上位机→设备, DATA 域仅含通用头部.
 * 设备收到后回复 E1H: head(20B) + device_num(1B) + N×module(变长).
 *
 * 各设备 module 结构与 0BH 设置时一致, vendor 长度为 2~10 字节不等,
 * 通过 ldi_cfg_module_size() 查表确定.
 */
static void cmd_rep_config(channel_t *ch, void *data)
{
    (void)data;

    // 计算所有 module 的总字节数，确定局部 buffer 大小
    uint16_t modules_len = 0;
    for (uint8_t i = 0; i < g_ldi.cfg.module_count; i++) {
        uint8_t mod_size = ldi_cfg_module_size((ldi_device_t)g_ldi.cfg.modules[i].device_type);
        // Flash 最多存 10 字节 vendor，超出则按 Flash 实际容量截断
        uint8_t vendor_len = mod_size - sizeof(ldi_module_head_t);
        if (vendor_len > sizeof(g_ldi.cfg.modules[i].vendor))
            vendor_len = sizeof(g_ldi.cfg.modules[i].vendor);
        modules_len += sizeof(ldi_module_head_t) + vendor_len;
    }

    // 用局部 buffer 组装完整 DATA（含 head）
    // 避免与 tx_buf 的 DATA 写入区重叠
    uint8_t buf[sizeof(ldi_rep_config_rsp_t) + modules_len];
    ldi_rep_config_rsp_t *rsp = (ldi_rep_config_rsp_t *)buf;

    ldi_build_rsp_head(&rsp->head, LDI_CMD_GET_PARA_RSP);
    rsp->device_num = g_ldi.cfg.module_count;

    uint8_t *dst = rsp->modules;
    for (uint8_t i = 0; i < g_ldi.cfg.module_count; i++) {
        app_flash_ldi_module_cfg_t *mod = &g_ldi.cfg.modules[i];

        // module 公共头
        *dst++ = mod->device_type;
        *dst++ = mod->device_index;

        // 查表确定 vendor 实际长度
        uint8_t mod_size   = ldi_cfg_module_size((ldi_device_t)mod->device_type);
        uint8_t vendor_len = mod_size - sizeof(ldi_module_head_t);
        if (vendor_len > sizeof(mod->vendor))
            vendor_len = sizeof(mod->vendor);

        memcpy(dst, mod->vendor, vendor_len);
        dst += vendor_len;
    }

    LDI_RESPOND_EX(ch, LDI_CMD_GET_PARA_RSP, g_ldi.rsp_seq, *rsp, modules_len);
}

/**
 * 处理 C0H 设备状态监控上报确认
 *
 * 设备确认上位机收到的上报
 */
static void cmd_rsp_report(channel_t *ch, void *data)
{
    (void)ch;

    /* C0H 是服务器对设备上报的应答，每 5 秒一次，携带服务器 Unix 时间戳，用于定期对钟 */
    ldi_req_head_t *head = (ldi_req_head_t *)data;
    uint32_t ts          = ((uint32_t)head->unix_timestamp[0] << 24) |
                           ((uint32_t)head->unix_timestamp[1] << 16) |
                           ((uint32_t)head->unix_timestamp[2] << 8) |
                           (uint32_t)head->unix_timestamp[3];
    pl_rtc_set_timestamp(pl_rtc_get_handle(), ts);
}

/* ================================================================
 *  主动发送帧（设备 → 上位机）
 *
 *  当前仅在 TCP Client 通道上发送，由 ldi_timer_task 周期驱动。
 *  ldi_next_rpt_seq() 返回 0x10~0xF0 的主动发送序号。
 *  ldi_build_rsp_head() 从内部状态构建头部（RTC + g_ldi.cfg）。
 * ================================================================ */

/**
 * @brief 发送 0EH 设备验证申请
 *
 * DATA = head(20B) + device_num(1B) + N×(device_type + device_index)(2B)
 * 上电后每 3 秒发送一次，直到收到 E0H 认证成功。
 * server 根据 lane_code/cert_info 和设备列表验证设备身份。
 */
void ldi_send_cert_req(channel_t *ch)
{
    uint8_t buf[sizeof(ldi_cert_req_t) + DEVICE_NUM * 2];
    ldi_cert_req_t *req = (ldi_cert_req_t *)buf;

    ldi_build_rsp_head(&req->head, LDI_CMD_CERT_REQ);
    req->device_num = g_ldi.cfg.module_count;

    for (uint8_t i = 0; i < g_ldi.cfg.module_count; i++) {
        req->devices[i].device_type  = g_ldi.cfg.modules[i].device_type;
        req->devices[i].device_index = g_ldi.cfg.modules[i].device_index;
    }

    uint16_t payload_len = sizeof(ldi_cert_req_t) + g_ldi.cfg.module_count * 2;
    uint8_t seq          = ldi_next_rpt_seq();
    ldi_send_response(ch, LDI_CMD_CERT_REQ, seq, (const uint8_t *)req, payload_len);
}

/**
 * @brief 发送 0CH 设备状态监控上报
 *
 * DATA = head(20B) + RunningTime(4B) + RunningStatus(1B) + ConnectStatus(1B) + DeviceNum(1B) + DeviceInfo[N×17B]
 * 认证成功后每 5 秒发送一次。
 */
void ldi_send_sta_rpt(channel_t *ch)
{
    uint8_t buf[sizeof(ldi_sta_rpt_t) + DEVICE_NUM * sizeof(ldi_device_info_t)];
    ldi_sta_rpt_t *rpt = (ldi_sta_rpt_t *)buf;

    ldi_build_rsp_head(&rpt->head, LDI_CMD_STA_RPT_REQ);

    /* RunningTime：设备自 RTOS 启动以来的运行秒数 */
    uint32_t rt          = osKernelGetTickCount() / 1000;
    rpt->running_time[0] = (uint8_t)(rt >> 24);
    rpt->running_time[1] = (uint8_t)(rt >> 16);
    rpt->running_time[2] = (uint8_t)(rt >> 8);
    rpt->running_time[3] = (uint8_t)rt;

    rpt->running_status = 0x00; /* 暂填正常 */
    rpt->connect_status = 0x00; /* 暂填已连接 */
    rpt->device_num     = g_ldi.cfg.module_count;

    /* 从 RTC 反推当前日期，转为 BCD 码（每字节 = 十位<<4 | 个位）
     * 例: 2026-05-13 → {0x20, 0x26, 0x05, 0x13} */
    time_t ts_now = (time_t)pl_rtc_get_timestamp(pl_rtc_get_handle());
    struct tm *tm = gmtime(&ts_now);
    uint16_t yr   = (uint16_t)(tm->tm_year + 1900); // uint8_t 装不下 2026，会溢出
    uint8_t dt[4] = {
        (uint8_t)((yr / 100 / 10 << 4) | (yr / 100 % 10)),                 // 世纪 BCD（如 20→0x20）
        (uint8_t)((yr % 100 / 10 << 4) | (yr % 100 % 10)),                 // 年份 BCD（如 26→0x26）
        (uint8_t)(((tm->tm_mon + 1) / 10 << 4) | ((tm->tm_mon + 1) % 10)), // 月份 BCD
        (uint8_t)((tm->tm_mday / 10 << 4) | (tm->tm_mday % 10)),           // 日期 BCD
    };

    for (uint8_t i = 0; i < g_ldi.cfg.module_count; i++) {
        rpt->devices[i].device_type      = g_ldi.cfg.modules[i].device_type;
        rpt->devices[i].device_index     = g_ldi.cfg.modules[i].device_index;
        rpt->devices[i].available_status = 0x00; /* 暂填可用 */
        rpt->devices[i].error_code       = 0x00;
        rpt->devices[i].running_status   = 0x01;
        rpt->devices[i].vendor_code[0]   = '0'; /* 暂填默认厂商代码 */
        rpt->devices[i].vendor_code[1]   = '7';
        rpt->devices[i].model_code[0]    = '2'; /* 暂填默认型号代码 */
        rpt->devices[i].model_code[1]    = '1';
        memcpy(rpt->devices[i].date_bcd, dt, sizeof(dt));
        rpt->devices[i].version_serial = 0x01; /* 暂填默认版本 */
        memset(rpt->devices[i].reserved, 0, sizeof(rpt->devices[i].reserved));
    }

    uint16_t payload_len = sizeof(ldi_sta_rpt_t) + g_ldi.cfg.module_count * sizeof(ldi_device_info_t);
    uint8_t seq          = ldi_next_rpt_seq();
    ldi_send_response(ch, LDI_CMD_STA_RPT_REQ, seq, (const uint8_t *)rpt, payload_len);
}

/**
 * 处理 E0H 设备验证结果
 *
 * 设备收到上位机回复的验证结果
 */
static void cmd_rsp_cert(channel_t *ch, void *data)
{
    (void)ch;

    /* E0H 响应：head(20B) + error_code(1B)，同步时间戳并检查认证结果 */
    ldi_req_head_t *head = (ldi_req_head_t *)data;
    uint32_t ts          = ((uint32_t)head->unix_timestamp[0] << 24) |
                           ((uint32_t)head->unix_timestamp[1] << 16) |
                           ((uint32_t)head->unix_timestamp[2] << 8) |
                           (uint32_t)head->unix_timestamp[3];
    pl_rtc_set_timestamp(pl_rtc_get_handle(), ts); // 调试：排除 RTC 并发写

    uint8_t error_code = *((uint8_t *)data + sizeof(ldi_req_head_t));
    if (error_code == 0x00)
        g_ldi.state = LDI_ST_AUTHED;
}

/**
 * 处理 0FH 设备升级包下载请求
 *
 * 升级不在本协议中实现，这里只定义桩函数
 */
static void cmd_update(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
}

/**
 * 处理 1AH 设备初始化请求
 *
 * 上位机→设备, 在设备验证成功后发起.
 * 请求帧: head(20B) | device_num(1B) | module[0] | ... | module[N-1]
 * 每个 module: device_type(1B) | device_index(1B) | version(1B) | custom_init(n)
 *
 * 响应 A1H: head(20B) | device_num(1B) | module[0] | ... | module[N-1]
 * 每个 module: device_type(1B) | device_index(1B) | protocol_version(2B) |
 *              custom_init_len(1B) | custom_init(变长)
 *
 * 当前无个性化初始化内容, 每个响应 module 固定 5 字节.
 */
static void cmd_init(channel_t *ch, void *data)
{
    uint8_t *ptr       = (uint8_t *)data + sizeof(ldi_req_head_t);
    uint8_t device_num = *ptr++;

    // 每个响应 module 定长 10 字节 (custom_init_len=0)
    uint8_t buf[sizeof(ldi_init_rsp_t) + device_num * sizeof(ldi_init_rsp_module_t)];
    ldi_init_rsp_t *rsp = (ldi_init_rsp_t *)buf;
    ldi_build_rsp_head(&rsp->head, LDI_CMD_INIT_RSP);
    rsp->device_num = device_num;

    bool all_ok                    = true;
    ldi_init_rsp_module_t *dst_mod = (ldi_init_rsp_module_t *)rsp->modules;
    for (uint8_t i = 0; i < device_num; i++) {
        ldi_module_head_t *req_mod = (ldi_module_head_t *)ptr;

        // 响应 module 头部
        dst_mod->head.device_type  = req_mod->device_type;
        dst_mod->head.device_index = req_mod->device_index;

        // 验证 device_type 是否在设备注册表中
        bool found = false;
        for (uint8_t j = 0; j < g_ldi.cfg.module_count; j++) {
            if (g_ldi.cfg.modules[j].device_type == (ldi_device_t)req_mod->device_type) {
                found = true;
                break;
            }
        }
        dst_mod->status = found ? 0x00 : 0x01;
        if (!found) all_ok = false;

        // 版本号（暂填默认值）
        dst_mod->software_version[0] = 0x00;
        dst_mod->software_version[1] = 0x01;
        dst_mod->firmware_version[0] = 0x00;
        dst_mod->firmware_version[1] = 0x01;
        dst_mod->protocol_version[0] = 0x00;
        dst_mod->protocol_version[1] = 0x00;
        dst_mod->custom_init_len     = 0x00; // 当前无个性化内容

        // 推进: 请求帧 module (最小 3B) → 响应 module (10B)
        ptr += sizeof(ldi_module_head_t) + 1;
        dst_mod = (ldi_init_rsp_module_t *)((uint8_t *)dst_mod + sizeof(ldi_init_rsp_module_t));
    }

    uint16_t modules_len = (uint16_t)((uint8_t *)dst_mod - rsp->modules);
    LDI_RESPOND_EX(ch, LDI_CMD_INIT_RSP, g_ldi.rsp_seq, *rsp, modules_len);

    if (all_ok)
        g_ldi.state = LDI_ST_READY;
}

/**
 * 处理 1BH 控制/查询请求
 *
 * 上位机→设备, 支持单次对多个功能模块进行组合控制或查询.
 * 请求: ctrl_head(24B) | device_num(1B) | Σ[mod_len(2B大端) | mod子帧]
 * 每个 mod 子帧 = DeviceType | DeviceIndex | DeviceFuncType | 功能payload
 * 响应 B1H: ctrl_head(24B) | device_num(1B) | N × ldi_ctrl_rsp_payload_t(4B)
 */
static void cmd_ctrl(channel_t *ch, void *data)
{
    // 1BH 使用 24 字节 ctrl_head (UnixTimestamp 8B)，不沿用 ldi_req_head_t
    uint8_t *ptr       = (uint8_t *)data + sizeof(ldi_ctrl_head_t);
    uint8_t device_num = *ptr++;

    uint8_t buf[sizeof(ldi_ctrl_rsp_t) + device_num * sizeof(ldi_ctrl_rsp_payload_t)];
    ldi_ctrl_rsp_t *rsp = (ldi_ctrl_rsp_t *)buf;
    ldi_build_ctrl_rsp_head(&rsp->head, LDI_CMD_CTRL_RSP);
    rsp->device_num = device_num;

    for (uint8_t i = 0; i < device_num; i++) {
        // mod_len: module 子帧总长 (含 DeviceType + DeviceIndex + payload)，大端序
        uint16_t mod_len = ((uint16_t)ptr[0] << 8) | ptr[1];
        ptr += 2;

        ldi_module_head_t *mod = (ldi_module_head_t *)ptr;
        uint8_t *payload       = ptr + sizeof(ldi_module_head_t);

        // 填充响应 module 公共字段
        rsp->modules[i].device_type      = mod->device_type;
        rsp->modules[i].device_index     = mod->device_index;
        rsp->modules[i].device_func_type = *payload;

        // 校验 device_type 是否在设备注册表中
        bool found = false;
        for (uint8_t j = 0; j < g_ldi.cfg.module_count; j++) {
            if (g_ldi.cfg.modules[j].device_type == (ldi_device_t)mod->device_type) {
                found = true;
                break;
            }
        }
        rsp->modules[i].status = found ? 0x00 : 0x01;

        // 解析设备特定 payload，当前仅做结构校验，硬件控制后续实现
        switch (mod->device_type) {
            case LDI_DEV_TYPE_BARRIER: { // E4H 栏杆控制 (01H)
                ldi_ctrl_barrier_t *ctrl = (ldi_ctrl_barrier_t *)payload;
                (void)ctrl; // TODO: dev_barrier_ctrl(ctrl->status)
                break;
            }
            case LDI_DEV_TYPE_VD: { // E5H 车检 (02H)，无额外 payload
                (void)payload;      // TODO: dev_vd_defog()
                break;
            }
            case LDI_DEV_TYPE_DISPLAY: { // E6H 显示控制(01H) / 清屏(02H)
                ldi_ctrl_display_t *ctrl = (ldi_ctrl_display_t *)payload;
                (void)ctrl; // TODO: dev_display_ctrl(ctrl)
                break;
            }
            case LDI_DEV_TYPE_LANE_SIGNAL: { // E7H 信号灯控制 (01H)
                ldi_ctrl_signal_t *ctrl = (ldi_ctrl_signal_t *)payload;
                (void)ctrl; // TODO: dev_signal_ctrl(ctrl->color)
                break;
            }
            case LDI_DEV_TYPE_ALARM: { // E8H 报警控制 (01H)
                ldi_ctrl_alarm_t *ctrl = (ldi_ctrl_alarm_t *)payload;
                (void)ctrl; // TODO: dev_alarm_ctrl(ctrl)
                break;
            }
            case LDI_DEV_TYPE_VMS: { // E9H → VMS (01H)
                ldi_ctrl_vms_t *ctrl = (ldi_ctrl_vms_t *)payload;
                vms_ctrl(ctrl, mod_len - sizeof(ldi_ctrl_vms_t) - sizeof(ldi_module_head_t));
                break;
            }
            case LDI_DEV_TYPE_CANOPY_LIGHT: { // EAH 雨棚灯控制 (01H)
                ldi_ctrl_canopy_light_t *ctrl = (ldi_ctrl_canopy_light_t *)payload;
                (void)ctrl; // TODO: dev_canopy_light_ctrl(ctrl->color)
                break;
            }
            case LDI_DEV_TYPE_FOG_LIGHT: { // EBH 雾灯控制 (01H)
                ldi_ctrl_fog_light_t *ctrl = (ldi_ctrl_fog_light_t *)payload;
                (void)ctrl; // TODO: dev_fog_light_ctrl(ctrl)
                break;
            }
            case LDI_DEV_TYPE_VOICE: { // F4H 语音播报 (01H)
                ldi_ctrl_voice_t *ctrl = (ldi_ctrl_voice_t *)payload;
                (void)ctrl; // TODO: dev_voice_ctrl(ctrl)
                break;
            }
            default:
                break;
        }

        ptr += mod_len;
    }

    LDI_RESPOND_EX(ch, LDI_CMD_CTRL_RSP, g_ldi.rsp_seq, *rsp, device_num * sizeof(ldi_ctrl_rsp_payload_t));
}

/**
 * 处理 1CH 设备功能数据上报
 *
 * 设备→上位机, 主动上报含 N 个功能模块信息的数据帧.
 * 上位机处理完成后回复 C1H (上报确认).
 */
static void cmd_rep_func(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
}

/**
 * 处理 21H 设备搜索请求（UDP 广播）
 *
 * 广播回复 0x12: CmdType(1) + IP(4大端) + Port(2大端) + Gateway(4大端) + Mask(4大端) + ErrCode(1)
 */
static void cmd_search(channel_t *ch, void *data)
{
    (void)data;

    ldi_search_rsp_t rsp;
    rsp.cmd_type = LDI_CMD_SEARCH_RSP;
    rsp.err_code = 0x00;

    memcpy(rsp.ip, g_ldi.cfg.device_ip, sizeof(rsp.ip));
    rsp.port[0] = (uint8_t)(g_ldi.cfg.device_port >> 8);
    rsp.port[1] = (uint8_t)(g_ldi.cfg.device_port);
    memcpy(rsp.gateway, g_ldi.cfg.gateway, sizeof(rsp.gateway));
    memcpy(rsp.mask, g_ldi.cfg.netmask, sizeof(rsp.mask));

    /* 构造 LDI 帧 + 广播发送 */
    osMutexAcquire(g_ldi.tx_lock, osWaitForever);
    ldi_frame_t *frame   = (ldi_frame_t *)g_ldi.tx_buf;
    frame->stx[0]        = 0xFF;
    frame->stx[1]        = 0xFF;
    frame->ver           = 0x00;
    frame->seq           = ldi_next_rpt_seq();
    uint16_t payload_len = sizeof(ldi_search_rsp_t);
    frame->len[0]        = (uint8_t)(payload_len >> 24);
    frame->len[1]        = (uint8_t)(payload_len >> 16);
    frame->len[2]        = (uint8_t)(payload_len >> 8);
    frame->len[3]        = (uint8_t)payload_len;
    memcpy(frame->data_crc, &rsp, payload_len);
    uint16_t crc                     = crc16_xmodem(&frame->ver, sizeof(*frame) - sizeof(frame->stx) + payload_len);
    frame->data_crc[payload_len]     = (uint8_t)(crc >> 8);
    frame->data_crc[payload_len + 1] = (uint8_t)(crc & 0xFF);
    osMutexRelease(g_ldi.tx_lock);

    app_udp_broadcast(g_ldi.tx_buf, sizeof(ldi_frame_t) + payload_len + 2);
}
