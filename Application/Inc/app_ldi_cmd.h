#pragma once

#include "main.h"

#include "app_ldi.h"
#include "app_dispatch.h"
#include "app_tcp_server.h"
#include "app_tcp_client.h"

/**
 * LDI 命令处理函数指针类型
 * @param meta  通道元信息（来源通道类型、编号等）
 * @param data  指向帧 DATA 域首字节，布局视具体命令而定
 */
typedef void (*ldi_cmd_handler_fn_t)(channel_t *, void *);

/** LDI 命令处理函数表，按命令码索引 */
extern const ldi_cmd_handler_fn_t g_ldi_cmd_table[];

// ============================================================================
// 复合指令 module 公共结构
// ============================================================================
//
// 复合指令 (0BH/0DH/1AH/1BH/1CH/0CH) 的 DATA 域通用布局:
//
//   ┌────────────┬─────────────┬───────────────────────────────────┐
//   │   head     │ device_num  │ module[0] | module[1] | ...        │
//   │ (20 bytes) │  (1 byte)   │ (变长, 按 device_type 区分)         │
//   └────────────┴─────────────┴───────────────────────────────────┘
//
// 每个 module 前 2 字节固定为 device_type + device_index,
// 后续 payload 的结构和长度由 device_type 决定。

/**
 * 复合指令 module 公共头
 * 所有复合指令的每个 module 均以此 2 字节开头
 */
typedef struct [[gnu::packed]] {
    uint8_t device_type;  // 外设功能模块类型编码, 对应 ldi_device_t 枚举值, 如 E4H=电动栏杆机
    uint8_t device_index; // 功能模块序号, 同类型多实例时区分, 从 01H 开始编号
} ldi_module_head_t;

// ============================================================================
// 0BH 设备参数配置 — 各设备 module payload
// ============================================================================
//
// payload 定义 = module 中除 device_type/device_index 之外的剩余字节
// 帧偏移: payload[0] 对应 module 内偏移 2 (跳过公共头)
//
// 参数设置成功后自动生效，无须重启设备。

/**
 * 电动栏杆机 (E4H) 参数配置 payload — 共 11 字节
 *
 *   [0] FilterTime  |  [1..10] Vendor
 *       1 byte       |     10 bytes
 */
typedef struct [[gnu::packed]] {
    uint8_t filter_time; // 杂波信号过滤时间阈值, 单位毫秒, 用于消抖
    uint8_t vendor[10];  // 厂商自定义参数段
} ldi_cfg_barrier_t;

/**
 * 信息显示屏 (E6H) 参数配置 payload — 共 11 字节
 *
 *   [0] FontLine  |  [1..10] Vendor
 *      1 byte     |     10 bytes
 */
typedef struct [[gnu::packed]] {
    uint8_t font_line;  // 默认显示行数: 01H~06H, 00H=自适应
    uint8_t vendor[10]; // 厂商自定义参数段
} ldi_cfg_display_t;

/**
 * 通行信号灯(E7H) / 报警器(E8H) / 雨棚信号灯(EAH) / 雾灯(EBH) 共用参数配置 payload — 共 2 字节
 *
 *    [0] 保留    |  [1..2] Vendor
 *     1 byte     |    2 bytes
 *
 * 这四类设备在 0BH 命令下无独立业务参数, 仅含 2 字节厂商自定义段
 */
typedef struct [[gnu::packed]] {
    uint8_t vendor[2]; // 厂商自定义参数段
} ldi_cfg_signal_t;

/**
 * LED情报板 (E9H) 参数配置 payload — 共 10 字节
 *
 *    [0] 保留    |  [1..10] Vendor
 *     1 byte     |     10 bytes
 */
typedef struct [[gnu::packed]] {
    uint8_t vendor[10]; // 厂商自定义参数段
} ldi_cfg_vms_t;

typedef ldi_cfg_vms_t ldi_cfg_voice_t;
// ============================================================================
// 0DH 设备重启 — module payload
// ============================================================================

/**
 * 设备重启 (0DH) 各设备通用 payload — 共 10 字节
 *
 * 重启命令的 module 结构: device_type(1) + device_index(1) + vendor[10]
 * 除公共头外仅含厂商自定义段, 无设备特定参数
 */
typedef struct [[gnu::packed]] {
    uint8_t vendor[10]; // 厂商自定义参数段
} ldi_reboot_payload_t;

// ============================================================================
// 1BH 控制/查询 — 各设备 function 子帧 payload
// ============================================================================
//
// 每个 module 子帧结构:
//   DeviceType(1) | DeviceIndex(1)  | DeviceFuncType(1) | 功能特定 payload
//   └── 公共头 ──┘                   └────── 以下 struct 定义的 payload ──────┘
//
// 控制查询帧由服务端主动发起, 设备端返回控制/查询结果

/**
 * 电动栏杆机 (E4H) 栏杆控制 (01H) payload — 共 1 字节 (不含 DeviceFuncType)
 *
 *   [0] Status
 *      1 byte
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (栏杆控制)
    uint8_t status;           // 00H = 落杆, 01H = 抬杆
} ldi_ctrl_barrier_t;

/**
 * 信息显示屏 (E6H) 控制/查询 payload — 定长 4 字节 + 变长 text
 *
 *   [0] DeviceFuncType | [1..4] 功能参数 (union) | [5..] Text
 *        1 byte        |        4 bytes         |  N bytes
 *
 * 功能 01H (显示控制): FontColor + FontSize + FontLine + KeepTime + Text
 * 功能 02H (清屏):     ClearType (仅 [1] 有效, [2..4] 保留)
 *
 * Text 编码为 GBK, 行间以 '_' 分隔, 每行最多 10 个中文字符 (仅 01H 功能有效)
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号: 01H=显示控制, 02H=清屏
    union {
        struct {
            uint8_t font_color; // 字体颜色: 01H=绿色, 02H=红色, 03H=黄色
            uint8_t font_size;  // 字体大小: 01H~08H, 00H=自适应
            uint8_t font_line;  // 显示行数: 01H~06H, 00H=自适应
            uint8_t keep_time;  // 显示停留时间: 00H=永久停留, 01H~FFH=秒
        };
        uint8_t clear_type; // 清屏类型: 00H=文字清屏, 01H=全红, 02H=全绿
    };
    uint8_t text[]; // 显示内容 (柔性数组), GBK 编码, 行间 '_' 分隔
} ldi_ctrl_display_t;

/**
 * 通行信号灯 (E7H) 显示控制 (01H) payload — 共 1 字节 (不含 DeviceFuncType)
 *
 *   [0] Color
 *      1 byte
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (显示控制)
    uint8_t color;            // 信号灯颜色: 01H=绿色(通行), 02H=红色(禁行), 03H=黄色(过渡)
} ldi_ctrl_signal_t;

/**
 * 报警器 (E8H) 报警控制 (01H) payload — 共 3 字节 (不含 DeviceFuncType)
 *
 *   [0] Status | [1] WorkMode | [2] KeepTime
 *     1 byte   |    1 byte    |    1 byte
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (报警控制)
    uint8_t status;           // 报警开关: 00H=停止报警, 01H=开始报警
    uint8_t work_mode;        // 工作模式: 00H=持续报警, 01H~FFH=频率报警
    uint8_t keep_time;        // 报警保持时长: 00H=一直报警, 01H~FFH=秒
} ldi_ctrl_alarm_t;

/**
 * LED情报板 (E9H) 控制/查询 payload — 定长 5 字节 + 变长 text
 *
 *   [0] DeviceFuncType | [1..5] 功能参数 (union) | [6..] Text
 *        1 byte        |        5 bytes         |  N bytes
 *
 * 功能 01H (显示控制): FontColor + FontSize + FontLine + KeepTime + Format + Text
 * 功能 02H (清屏):     ClearType (仅 [1] 有效, [2..5] 保留)
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号: 01H=显示控制, 02H=清屏
    union {
        struct {
            uint8_t font_color; // 字体颜色: 01H=绿色, 02H=红色, 03H=黄色
            uint8_t font_size;  // 字体大小: 01H~08H, 00H=自适应
            uint8_t font_line;  // 显示行数: 01H~FFH, 00H=自适应
            uint8_t keep_time;  // 显示停留时间: 00H=永久停留, 01H~FFH=秒
            uint8_t format;     // 对齐方式: 01H=居中, 02H=左对齐, 03H=右对齐
        };
        uint8_t clear_type; // 清屏类型: 00H=文字清屏, 01H=全红, 02H=全绿
    };
    uint8_t text[]; // 显示内容 (柔性数组), GBK 编码, 行间 '_' 分隔
} ldi_ctrl_vms_t;

/**
 * 雨棚信号灯 (EAH) 显示控制 (01H) payload — 共 1 字节 (不含 DeviceFuncType)
 *
 *   [0] Color
 *      1 byte
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (显示控制)
    uint8_t color;            // 信号灯颜色: 01H=绿色(通行), 02H=红色(禁行), 03H=黄色
} ldi_ctrl_canopy_light_t;

/**
 * 雾灯 (EBH) 显示控制 (01H) payload — 共 2 字节 (不含 DeviceFuncType)
 *
 *   [0] Status | [1] WorkMode
 *     1 byte   |    1 byte
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (显示控制)
    uint8_t status;           // 雾灯开关: 00H=关闭, 01H=开启
    uint8_t work_mode;        // 工作模式: 00H=长亮, 01H~FFH=频率闪烁
} ldi_ctrl_fog_light_t;

/**
 * 语音播报设备 (F4H) 播放提示音 (01H) payload — 定长 4 字节 + 变长 data
 *
 *   [0] DeviceFuncType | [1] Type | [2..3] Length(大端) | [4..] Data
 *        1 byte        |   1 byte |       2 bytes       |  N bytes
 *
 * Type: 00H=按文字播报, 01H=按指定语音序列播报, 02H~FFH=其他语音格式
 * Length: 语音内容字节长度 N, 最大 FFFFH, 大端序
 * Data: 语音内容, GBK 编码
 */
typedef struct [[gnu::packed]] {
    uint8_t device_func_type; // 功能编号, 此处取值 01H (播放提示音)
    uint8_t type;             // 语音控制类型
    uint8_t length[2];        // 语音内容字节长度 N, 大端序
    uint8_t data[];           // 语音内容, GBK 编码
} ldi_ctrl_voice_t;

// ============================================================================
// 主动上报帧结构（设备 → 服务端）
// ============================================================================

/** 设备状态信息（0CH 上报帧中每个功能模块 17 字节） */
typedef struct [[gnu::packed]] {
    uint8_t device_type;      // 功能模块类型编码 (E1H~EBH)
    uint8_t device_index;     // 功能模块序号, 从 01H 开始
    uint8_t available_status; // 可用状态
    uint8_t error_code;       // 异常编码 (00H=正常, 见附录A.2)
    uint8_t running_status;   // 运行状态 (00H=正常, 见附录A.1)
    uint8_t vendor_code[2];   // 厂商代码
    uint8_t model_code[2];    // 型号代码
    uint8_t date_bcd[4];      // 日期 BCD 码 (如 2026-05-12 → 0x20 0x26 0x05 0x12)
    uint8_t version_serial;   // 版本序列号
    uint8_t reserved[3];      // 保留, 填 00H
} ldi_device_info_t;

static_assert(sizeof(ldi_device_info_t) == 17, "ldi_device_info_t must be 17 bytes");

/** 设备验证申请帧 DATA 域（0EH） */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t device_num;
    struct {
        uint8_t device_type;  // 功能模块类型编码 (E1H~EBH)
        uint8_t device_index; // 功能模块序号, 从 01H 开始
    } devices[];              // N × 2 字节
} ldi_cert_req_t;

/** 设备状态监控上报帧 DATA 域（0CH） */
typedef struct [[gnu::packed]] {
    ldi_req_head_t head;
    uint8_t running_time[4];     // 设备运行时长(秒), 大端
    uint8_t running_status;      // 00H=正常, 01H=异常
    uint8_t connect_status;      // 与服务端连接状态
    uint8_t device_num;          // 功能模块数量 N
    ldi_device_info_t devices[]; // N × 17 字节
} ldi_sta_rpt_t;

/* ---- 主动发送 API ---- */
void ldi_send_cert_req(channel_t *ch);
void ldi_send_sta_rpt(channel_t *ch);
