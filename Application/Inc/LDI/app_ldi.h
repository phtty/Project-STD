#pragma once

/**
 * @file    app_ldi.h
 * @brief   LDI 车道设备协议：设备类型、命令码、帧格式与协议上下文
 */

#include "cmsis_os2.h"
#include "app_dispatch.h"
#include "app_ldi_cfg.h"
#include "ring_buffer.h"

#define DEVICE_NUM      (2U) // 本设备使用到的功能模块数量

#define LDI_TX_BUF_SIZE (512U) // 响应帧拼装缓冲区上限

/**
 * 外设功能模块类型编码
 * 依据《车道设备接口规范》第2.1节定义
 * 编码范围 E1H~EBH，与协议帧中 DeviceType 字段一一对应
 */
typedef enum : uint8_t {
    APP_LDI_DEVICE_RSU          = 0xE1, // 路侧单元
    APP_LDI_DEVICE_LPR          = 0xE2, // 高清车牌图像识别设备
    APP_LDI_DEVICE_VTR          = 0xE3, // 车型识别设备
    APP_LDI_DEVICE_BARRIER      = 0xE4, // 电动栏杆机
    APP_LDI_DEVICE_VD           = 0xE5, // 车辆检测器
    APP_LDI_DEVICE_DISPLAY      = 0xE6, // 信息显示屏
    APP_LDI_DEVICE_LANE_SIGNAL  = 0xE7, // 通行信号灯
    APP_LDI_DEVICE_ALARM        = 0xE8, // 报警器
    APP_LDI_DEVICE_VMS          = 0xE9, // LED情报板（含引导屏）
    APP_LDI_DEVICE_CANOPY_LIGHT = 0xEA, // 雨棚信号灯
    APP_LDI_DEVICE_FOG_LIGHT    = 0xEB, // 雾灯
    APP_LDI_DEVICE_VOICE        = 0xF4, // 语音播报
    APP_LDI_DEVICE_COUNT        = 13,   // 本项目支持的设备类型数量
} app_ldi_device_t;

/**
 * 通用通讯数据帧
 * 依据《车道设备接口规范》第3.4节定义
 * 服务端与客户端通讯的统一帧格式，所有数据均为大端序
 *
 * 帧布局:
 *   STX(2B) | VER(1B) | SEQ(1B) | LEN(4B) | DATA(N) | CRC(2B)
 *
 * CRC-16/XMODEM 校验范围覆盖 VER ~ DATA 全部字节，多项式 0x8408，初始值 0xFFFF
 */
typedef struct [[gnu::packed]] {
    uint8_t stx[2];     // 帧开始标志, 固定值 0xFFFF
    uint8_t ver;        // 协议版本号, 当前版本 0x00
    uint8_t seq;        // 帧序列号: 服务端主动=0x0X, 客户端主动=0xX0, X=1~9自动递增
    uint8_t len[4];     // DATA域长度, 高2字节保留, 低2字节为实际长度
    uint8_t data_crc[]; // DATA内容 + CRC16校验值(2字节, 帧尾), 柔性数组成员
} app_ldi_frame_t;

/**
 * 数据帧通用头部（标准版，4 字节时间戳）
 * 除 1BH/1CH 等少数命令外，大多数配置类/通用类命令的 DATA 域以此头部开头
 *
 * 字段布局 (共20字节):
 *   CmdType(1B) | UnixTime(4B) | LaneHex(5B) | CertInfo(8B) | Reserve(2B)
 */
typedef struct [[gnu::packed]] {
    uint8_t cmd_type;          // 指令代码, 如 0AH=IP设置请求, B0H=参数配置应答
    uint8_t unix_timestamp[4]; // Unix时间戳, 格林威治1970-01-01起的总秒数, 大端
    uint8_t lane_code[5];      // 车道HEX编号: 网络号(2B,BCD) + 站点号(2B,BCD) + 车道号(1B)
    uint8_t cert_info[8];      // 设备验证信息, 由平台生成的唯一设备验证码
    uint8_t reserve[2];        // 保留字节, 默认填 00H
} app_ldi_req_head_t;

/**
 * 控制/查询命令专用头部（8 字节时间戳，毫秒精度）
 * 仅 1BH/B1H 使用，其他命令使用 app_ldi_req_head_t
 *
 * 字段布局 (共24字节):
 *   CmdType(1B) | UnixTime(8B) | LaneHex(5B) | CertInfo(8B) | Reserve(2B)
 */
typedef struct [[gnu::packed]] {
    uint8_t cmd_type;          // 指令代码, 1BH=控制查询请求, B1H=控制查询应答
    uint8_t unix_timestamp[8]; // Unix时间戳, 毫秒精度, 大端
    uint8_t lane_code[5];      // 车道HEX编号
    uint8_t cert_info[8];      // 设备验证信息
    uint8_t reserve[2];        // 保留字节, 默认填 00H
} app_ldi_ctrl_head_t;

/**
 * LDI 协议命令码枚举
 * 涵盖配置接口(4.x)、通用接口(5.x)、功能接口(6.x) 三类命令
 */
typedef enum : uint8_t {
    // ---- 配置接口命令 (出厂配置模式, 设备为服务端) ----
    APP_LDI_CMD_TYPE_SET_IP_REQ = 0x0A, // 设备IP信息设置请求
    APP_LDI_CMD_TYPE_SET_IP_RSP = 0xA0, // 设备IP信息设置应答

    APP_LDI_CMD_TYPE_SET_PARA_REQ = 0x0B, // 设备参数配置请求
    APP_LDI_CMD_TYPE_SET_PARA_RSP = 0xB0, // 设备参数配置应答

    APP_LDI_CMD_TYPE_REBOOT_REQ = 0x0D, // 设备重启请求
    APP_LDI_CMD_TYPE_REBOOT_RSP = 0xD0, // 设备重启应答

    APP_LDI_CMD_TYPE_GET_IP_REQ = 0x1D, // 设备IP信息采集请求
    APP_LDI_CMD_TYPE_GET_IP_RSP = 0xD1, // 设备IP信息采集应答

    APP_LDI_CMD_TYPE_GET_PARA_REQ = 0x1E, // 设备参数采集请求
    APP_LDI_CMD_TYPE_GET_PARA_RSP = 0xE1, // 设备参数采集应答

    // ---- 通用接口命令 (工作模式, 设备为客户端) ----
    APP_LDI_CMD_TYPE_STA_RPT_REQ = 0x0C, // 设备状态监控上报 (间隔5秒)
    APP_LDI_CMD_TYPE_STA_RPT_RSP = 0xC0, // 监控上报应答 (携带软件版本信息)

    APP_LDI_CMD_TYPE_CERT_REQ = 0x0E, // 设备验证申请 (上电后定时3秒重发直至成功)
    APP_LDI_CMD_TYPE_CERT_RSP = 0xE0, // 设备验证应答 (00H=认证成功)

    APP_LDI_CMD_TYPE_UPDATE_REQ = 0x0F, // 设备升级包下载请求
    APP_LDI_CMD_TYPE_UPDATE_RSP = 0xF0, // 设备升级包下载应答 (含MD5+文件流)

    APP_LDI_CMD_TYPE_INIT_REQ = 0x1A, // 设备初始化请求 (验证成功后由服务端发起)
    APP_LDI_CMD_TYPE_INIT_RSP = 0xA1, // 设备初始化应答

    // ---- 功能接口命令 (工作模式) ----
    APP_LDI_CMD_TYPE_CTRL_REQ = 0x1B, // 控制/查询请求 (服务端→设备, 支持多模块组合)
    APP_LDI_CMD_TYPE_CTRL_RSP = 0xB1, // 控制/查询应答

    APP_LDI_CMD_TYPE_FUNC_RPT_REQ = 0x1C, // 设备功能数据上报 (设备→服务端)
    APP_LDI_CMD_TYPE_FUNC_RPT_RSP = 0xC1, // 上报确认应答

    // ---- 设备搜索 (UDP 广播) ----
    APP_LDI_CMD_TYPE_SEARCH_REQ = 0x21, // 设备搜索请求 (上位机→设备, UDP广播)
    APP_LDI_CMD_TYPE_SEARCH_RSP = 0x12, // 设备搜索应答 (设备→上位机, UDP广播)
} app_ldi_cmd_type_t;

/* ---- 协议上下文 ---- */

typedef enum {
    APP_LDI_STATE_UNINIT, // 仅接受 0AH(IP设置)、E0H(认证结果)
    APP_LDI_STATE_AUTHED, // 已认证，可接受 1AH(初始化)
    APP_LDI_STATE_READY,  // 正常运行，全部命令可用
} app_ldi_state_t;

typedef struct {
    app_ldi_state_t state;
    uint8_t rsp_seq; // 响应序号（app_ldi_task 在处理每帧前从帧里取，回显 host 请求帧）
    uint8_t rpt_seq; // 主动上报序号计数器（→ 0x10, 0x20...）

    app_flash_ldi_cfg_info_t cfg; // RAM 镜像 — 唯一配置真源
    bool cfg_valid;

    uint32_t last_cert_tick; // 上次发送 0EH 的 RTOS tick（3 秒间隔）
    uint32_t last_rpt_tick;  // 上次发送 0CH 的 RTOS tick（5 秒间隔）

    osMutexId_t tx_lock;             // 保护 tx_buf，两个任务共享
    uint8_t tx_buf[LDI_TX_BUF_SIZE]; // 响应帧拼装缓冲区
} app_ldi_ctx_t;

extern app_ldi_ctx_t g_ldi_ctx;

void app_ldi_ctx_init(app_ldi_ctx_t *self);

/* ---- API ---- */

extern osMessageQueueId_t g_ldi_msg_queue;
extern osThreadId_t g_ldi_task_handle;
extern const osThreadAttr_t g_ldi_task_attr;
extern osThreadId_t g_ldi_timer_task_handle;
extern const osThreadAttr_t g_ldi_timer_task_attr;

void app_ldi_task(void *argument);
void app_ldi_timer_task(void *argument);
/** @brief LDI 帧探测（pcb_ops.probe）—— 契约见 app_dispatch.h 的 app_pcb_probe_fn_t */
app_pcb_probe_state_t app_ldi_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux);
uint8_t app_ldi_get_device_idx(app_ldi_device_t device_type);
void app_ldi_set_device_idx(app_ldi_device_t device_type, uint8_t device_index);

/* ---- 响应帧头部构建（从内部状态，不从请求拷贝）---- */
void app_ldi_build_rsp_head(app_ldi_req_head_t *head, uint8_t cmd_type);
void app_ldi_build_ctrl_rsp_head(app_ldi_ctrl_head_t *head, uint8_t cmd_type);
