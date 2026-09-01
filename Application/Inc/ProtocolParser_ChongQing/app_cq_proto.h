/**
 * @file    app_cq_proto.h
 * @brief   重庆高速二代费显协议（CQ）公共定义
 *
 * 传输：UDP 业务口（PROTO_CHONGQING 下 Sector1 net_cfg.port 可配，默认 20103；
 * dev 共存构建固定 20103，见 app_udp.c _udp_cq_read_port）+ 搜索口 10011 固定
 * （与创迪发现口 / IAP 共用 CH_ID_UDP）。
 * 帧形态：JSON（'{' 花括号定界，文本字段为 GB2312 字节）+ 固定 12B 二进制
 * （重启请求 / 搜索请求，CRC16-XMODEM 校验）。协议版本 2.0.1。
 * JSON 命令无应答帧。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_dispatch.h"

/* 心跳超时故障屏开关（1 开 / 0 关）：
 * - 默认跟随构建口径：PROTO_CHONGQING（重庆量产）开启；共存/其他构建关闭
 *   （通用版固件多协议共存时，他省上位机不发重庆心跳，不能误显示故障内容）；
 * - 构建入口可显式覆盖：-DCQ_FAULT_SCREEN=1 强制开 / =0 强制关（Makefile/EIDE
 *   defineList）。
 * - 旧名已废弃（2026-08-24 改名，不保留兼容别名）：
 *   CQ_FAULT_SCREEN_ENABLED → CQ_FAULT_SCREEN；
 *   -DCQ_FAULT_SCREEN_FORCE_ON=1/0 覆盖 → -DCQ_FAULT_SCREEN=1/0。 */
#ifndef CQ_FAULT_SCREEN
#ifdef PROTO_CHONGQING
#define CQ_FAULT_SCREEN 1
#else
#define CQ_FAULT_SCREEN 0
#endif
#endif

/* ---- 帧 / 队列容量 ---- */
#define CQ_PAYLOAD_MAX (1044U) /* 与 FRAME_DATA_MAX_LEN 对齐；JSON 帧扫描上限 \
                                */
#define CQ_MSG_SIZE (sizeof(frame_msg_t) + CQ_PAYLOAD_MAX)
#define CQ_QUEUE_DEPTH (3U)

/* ---- 网络默认值（出厂默认 192.168.1.5/255.255.255.0/192.168.1.1:20103）----
 */
#define CQ_DEFAULT_PORT (20103U)

/* ---- 心跳故障阈值（秒）---- */
#define CQ_SYNC_FAULT_S (120U)

/* ---- 命令类型 ---- */
typedef enum {
  CQ_PCMD_TEXT1 = 0,      /* text1         多行文本 */
  CQ_PCMD_TRA1,           /* tra1          红叉 / 绿箭 */
  CQ_PCMD_PIC1,           /* pic1          图片（stub） */
  CQ_PCMD_LIGHT1,         /* light1        亮度 0~15 */
  CQ_PCMD_VOICE_CONTROL1, /* voice_control1 音量 0~15 */
  CQ_PCMD_VOICE1,         /* voice1        语音播报文本 */
  CQ_PCMD_VOICE_PLAY1,    /* voice_play1 语音库播放（stub） */
  CQ_PCMD_WARN1,          /* warn1         黄闪倒计时 */
  CQ_PCMD_SYN1,           /* syn1          心跳 */
  CQ_PCMD_SETIP,          /* setip         改 IP 落盘重启 */
  CQ_PCMD_SCREEN,         /* screen        单元板选择（仅记录） */
  CQ_PCMD_FULL,           /* full          全屏单色 */
  CQ_PCMD_BIN_REBOOT,     /* 12B 二进制：重启请求 */
  CQ_PCMD_BIN_SEARCH,     /* 12B 二进制：IP 搜索请求 */
  CQ_PCMD_INVALID,
} cq_pcmd_t;

/* ---- 解析结果状态 ---- */
typedef enum {
  CQ_PARSE_OK = 0,
  CQ_PARSE_ERR_FRAME, /* 帧结构非法（非 '{' / 非 12B 二进制 / 截断） */
  CQ_PARSE_ERR_CMD,   /* cmd 字段不在命令集 */
  CQ_PARSE_ERR_PARAM, /* 参数非法 */
} cq_parse_sta_t;

/* ---- 文本行参数（text1 ln0~ln7）---- */
typedef struct {
  bool valid;
  uint8_t font;  /* 16 / 24 / 32（默认 24） */
  uint8_t color; /* 0 红 / 1 绿 / 2 黄（默认绿） */
  const char *text;
  uint16_t text_len;
} cq_line_t;

/* ---- setip 参数 ---- */
typedef struct {
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gw[4];
  uint32_t port;
} cq_setip_t;

/* ---- 12B 二进制帧（帧内 CRC 为 CRC16-XMODEM 大端；定义于 app_cq_proto.c）----
 */
extern const uint8_t cq_bin_reboot_req[12];
extern const uint8_t cq_bin_search_req[12];

/* ---- 跨文件状态访问 ---- */

/** @brief 心跳计数器清零（宽松语义 B.7.2：帧结构合法的 JSON——合法命令 /
 *  命令未知 / 参数非法——到达即调用；CQ_PARSE_ERR_FRAME 与 BIN 帧不复位）。 */
void cq_proto_sync_reset(void);

/** @brief 设置黄闪状态：0 关闭并清倒计时；>0 开启并倒计时（每秒递减，归零关）；
 *  -1 常开（映射 0xFFFF 语义，不自动关）。 */
void cq_proto_warn_set(int32_t seconds);

/* ---- 任务入口（cq_proto_init 创建）---- */
void cq_proto_handle_task(void *argument);
void cq_proto_timer_task(void *argument);