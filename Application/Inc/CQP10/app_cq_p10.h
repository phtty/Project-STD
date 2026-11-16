/**
 * @file    app_cq_p10.h
 * @brief   CQ_P10 产品协议（重庆P10状态屏固化协议，JSON over UDP）
 *
 * 仅实现协议文档 5.6 图片固化(picset) 与 5.7 修改IP(setip) 两条指令。
 * 协议模块自注册于 sw_post_initcall，接入 app_dispatch 调度框架。
 * 构建中与其他协议（IAP/LDI/AH_MQTT/RLS/CDSTD）互斥：它们共用同一个 UDP
 * 通道与 W25Qxx 尾部配置区。本产品分支（2300001735）直接把这几项从 3833024
 * 的构建中排除；本模块本身也可从构建整体剔除（5006048 已剔除）。
 *
 * 本产品无 IAP/升级，固件从 0x08000000 起：固化图与 setip 的网络配置都存
 * W25Qxx（capacity-16384 起 8KB），不再使用内部 Flash Sector1。
 */

#pragma once

#include <stdint.h>

#define CQ_UDP_PORT_DEFAULT 20102U /* 协议文档默认本机端口 */
#define CQ_PIC_COUNT        3U     /* 固化图片数 nu=0..2 (nu=3 全屏点亮) */
#define CQ_BITMAP_SIZE      2560U  /* 整屏位图字节数 320/8 × 64 */
#define CQ_PACKET_PAYLOAD   320U   /* 每包 tN 数据字节数 (8 行 × 40B) */
#define CQ_FRAME_MAX        1408U  /* 最大 JSON 帧字节数 (须 ≤ FRAME_DATA_MAX_LEN) */
