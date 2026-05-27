/**
 * @file    app_onbon.h
 * @brief   Onbon BX-Y LED 控制卡协议（Application 层）
 *
 * 封装 PHY1 层包头、协议层请求/答复帧、动态区域数据结构。
 * 用于将 LDI 协议的显示屏控制命令转换为 Onbon 串口控制帧。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "dispatch_types.h"

/* ---- 屏参（编译期固定） ---- */
#define ONBON_SCREEN_WIDTH  192
#define ONBON_SCREEN_HEIGHT 32
#define ONBON_DST_ADDR      0xFFFF /* 广播 */
#define ONBON_SRC_ADDR      0x8000 /* 模拟上位机 */
#define ONBON_DEVICE_TYPE   0xFE   /* 通配符 */
#define ONBON_PROTO_VER     0x02

/* ---- 字符转义特殊值 ---- */
#define ONBON_STX      0xA5
#define ONBON_STX_ESC  0xA6
#define ONBON_ETX      0x5A
#define ONBON_ETX_ESC  0x5B
#define ONBON_ESC_MASK 0x01 /* 自身转义时加 0x01 */
#define ONBON_ESC_VAL  0x02 /* 代表 STX/ETX 时加 0x02 */

/* ---- 命令码 ---- */
#define ONBON_CMD_DYNAMIC_AREA 0xA306 /* CmdGroup=0xA3, Cmd=0x06 发送实时显示区域数据 */

/* ---- 字体 ---- */
#define ONBON_FONT_SONG    0
#define ONBON_FONT_KAI     1
#define ONBON_FONT_HEI     2
#define ONBON_FONT_FANG    3
#define ONBON_FONT_YAHEI   4
#define ONBON_FONT_ARIAL   5
#define ONBON_FONT_TIMES   6
#define ONBON_FONT_VERDANA 7
#define ONBON_FONT_TAHOMA  8

/* ---- 颜色 ---- */
#define ONBON_COLOR_RED     1
#define ONBON_COLOR_GREEN   2
#define ONBON_COLOR_YELLOW  3
#define ONBON_COLOR_BLUE    4
#define ONBON_COLOR_CYAN    5
#define ONBON_COLOR_MAGENTA 6
#define ONBON_COLOR_WHITE   7

/* ---- 背景 ---- */
#define ONBON_BG_NONE 0

/* ---- 显示方式 ---- */
#define ONBON_DISP_STILL 1
#define ONBON_DISP_FAST  2
#define ONBON_DISP_LEFT  3
#define ONBON_DISP_RIGHT 4
#define ONBON_DISP_UP    5
#define ONBON_DISP_DOWN  6

/* ---- 运行模式 ---- */
#define ONBON_RUN_LOOP    0
#define ONBON_RUN_HOLD    1
#define ONBON_RUN_TIMEOUT 2
#define ONBON_RUN_SEQ     4

/* ---- 对齐 ---- */
#define ONBON_ALIGN_H_LEFT   0
#define ONBON_ALIGN_H_RIGHT  1
#define ONBON_ALIGN_H_CENTER 2
#define ONBON_ALIGN_V_TOP    0
#define ONBON_ALIGN_V_BOTTOM 1
#define ONBON_ALIGN_V_CENTER 2

// ============================================================================
// PHY1 层包头（14 字节，全部小端序）
// ============================================================================
//
//   PHY1 数据 = header(14B) + data_domain(N) + crc16(2B)
//
typedef struct [[gnu::packed]] {
    uint8_t dst_addr[2];  /* 屏地址, 小端, 0xFFFF=广播 */
    uint8_t src_addr[2];  /* 源地址, 小端, 0x8000=上位机 */
    uint8_t reserved[3];  /* 保留 */
    uint8_t opt;          /* 选项, 保留 */
    uint8_t check_mode;   /* 校验模式: 0=CRC16 */
    uint8_t display_mode; /* 显示模式, 保留 */
    uint8_t device_type;  /* 设备类型: 0xFE=通配符 */
    uint8_t proto_ver;    /* 协议版本: 0x02 */
    uint8_t data_len[2];  /* 数据域长度, 小端, 不含本包头和校验 */
} onbon_header_t;

static_assert(sizeof(onbon_header_t) == 14, "onbon_header_t must be 14 bytes");

// ============================================================================
// 协议层请求帧
// ============================================================================
//
//   DATA = req_head(5B) + payload(N)
//
typedef struct [[gnu::packed]] {
    uint8_t cmd_group; /* 命令分组编号 */
    uint8_t cmd;       /* 命令编号 */
    uint8_t response;  /* 0x01=必须回复, 0x02=不必回复 */
    uint8_t process;   /* 是否清理区域 */
    uint8_t reserved;  /* 保留 */
    uint8_t payload[]; /* 命令特定数据 (柔性数组) */
} onbon_req_t;

// ============================================================================
// 动态区域数据头（A3 06 命令 AreaData 部分，非 PHY1 包头）
// ============================================================================
//
//   此结构为区域数据的定长部分（不含语音字段）。
//   屏参硬编码: 宽=192px=24字节, 高=32px, 坐标原点(0,0).
//
typedef struct [[gnu::packed]] {
    uint8_t area_type;    /* 区域类型 */
    uint8_t area_x[2];    /* X 坐标, 小端, 以字节(8像素)为单位 */
    uint8_t area_y[2];    /* Y 坐标, 小端, 以像素为单位 */
    uint8_t area_w[2];    /* 宽度, 24(字节) = 192 像素 */
    uint8_t area_h[2];    /* 高度, 32(像素) */
    uint8_t area_id;      /* 动态区编号, 0 开始 */
    uint8_t line_spacing; /* 行间距 */
    uint8_t run_mode;     /* 0=循环, 1=静止末页, 2=循环超时消隐 */
    uint8_t timeout[2];   /* 超时秒数, 小端 */
    uint8_t sound_mode;   /* 0=不使能语音 */
    uint8_t ext_para_len; /* 拓展位个数, 0=无拓展 */
    uint8_t text_align;   /* Bit1-0=水平, Bit3-2=垂直 */
    uint8_t single_line;  /* 0x02=多行 */
    uint8_t new_line;     /* 0x02=自动换行 */
    uint8_t display_mode; /* 显示方式: 0x01=静止, 0x03=左移... */
    uint8_t exit_mode;    /* 退出方式, 保留 */
    uint8_t speed;        /* 速度 0x00~0x18 */
    uint8_t stay_time;    /* 特技停留, 单位 0.5s */
    uint8_t data_len[4];  /* Data 长度, 小端 */
    uint8_t data[];       /* 显示内容 (柔性数组) */
} onbon_area_data_t;

static_assert(sizeof(onbon_area_data_t) == 27, "onbon_area_data_t fixed part must be 27 bytes");

// ============================================================================
// 转义
// ============================================================================

/**
 * @brief  封帧转义 — 扫描输入，将特殊字节替换为双字节转义序列
 *
 * 转义规则: 0xA5→0xA6 0x02, 0xA6→0xA6 0x01, 0x5A→0x5B 0x02, 0x5B→0x5B 0x01
 *
 * @note   广播模式下控制器不回复，无需解帧反转义。
 *
 * @param  dst      输出缓冲区（应预分配 src_len*2 空间）
 * @param  src      待转义的原始数据
 * @param  src_len  原始数据长度
 * @return 转义后的字节数
 */
size_t onbon_escape(uint8_t *dst, const uint8_t *src, size_t src_len);

// ============================================================================
// 上下文 + 发送接口
// ============================================================================

/**
 * @brief  Onbon 发送上下文（调用方只需修改感兴趣的值）
 *
 *   区域坐标以字节(8像素)为单位（X/W）或像素为单位（Y/H）。
 *   屏参固定 192×32: area_w=24(字节), area_h=32(像素)。
 */
typedef struct {
    /* 区域几何 */
    uint16_t area_x; /* X 坐标, 字节(8像素), 默认 0 */
    uint16_t area_y; /* Y 坐标, 像素, 默认 0 */
    uint16_t area_w; /* 宽度, 字节(8像素), 默认 24 (=192px) */
    uint16_t area_h; /* 高度, 像素, 默认 32 */
    uint8_t area_id; /* 动态区编号, 默认 0 */

    /* 显示行为 */
    uint8_t run_mode;     /* 默认 ONBON_RUN_LOOP */
    uint16_t timeout;     /* 超时秒数, 小端, 默认 0 */
    uint8_t display_mode; /* 默认 ONBON_DISP_STILL */
    uint8_t exit_mode;    /* 退出方式, 默认 0 */
    uint8_t speed;        /* 速度 0~24, 默认 0 */
    uint8_t stay_time;    /* 特技停留, 单位 0.5s, 默认 0 */

    /* 文本样式 */
    uint8_t font;      /* 默认 ONBON_FONT_SONG */
    uint8_t font_size; /* 默认 16 */
    uint8_t color;     /* 默认 ONBON_COLOR_RED */
    uint8_t bg_color;  /* 默认 ONBON_BG_NONE */
    uint8_t h_align;   /* 水平对齐, 默认 ONBON_ALIGN_H_CENTER */
    uint8_t v_align;   /* 垂直对齐, 默认 ONBON_ALIGN_V_CENTER */
} onbon_ctx_t;

void onbon_ctx_init(onbon_ctx_t *ctx);
void onbon_send_text(const onbon_ctx_t *ctx, const char *text);
