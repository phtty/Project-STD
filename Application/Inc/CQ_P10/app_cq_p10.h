/**
 * @file    app_cq_p10.h
 * @brief   CQ_P10 产品协议（重庆P10状态屏固化协议，JSON over UDP）
 *
 * 仅实现协议文档 5.6 图片固化(picset) 与 5.7 修改IP(setip) 两条指令。
 * 协议模块自注册于 sw_app_initcall，接入 app_dispatch 调度框架。
 * 构建中与其他协议(IAP/LDI/AH_MQTT/RLS)互斥，本模块可从构建整体剔除。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"

#define CQ_UDP_PORT_DEFAULT 20102U /* 协议文档默认本机端口 */
#define CQ_PIC_COUNT        3U     /* 固化图片数 nu=0..2 (nu=3 全屏点亮) */
#define CQ_BITMAP_SIZE      2560U  /* 整屏位图字节数 320/8 × 64 */
#define CQ_PACKET_PAYLOAD   320U   /* 每包 tN 数据字节数 (8 行 × 40B) */
#define CQ_FRAME_MAX        1408U  /* 最大 JSON 帧字节数 (须 ≤ FRAME_DATA_MAX_LEN) */

extern osMessageQueueId_t g_cq_msg_queue;
extern osThreadId_t g_cq_task_handle;
