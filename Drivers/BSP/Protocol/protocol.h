#pragma once

#include "main.h"

#include "lwip.h"
#include "cmsis_os2.h"

#include "ring_buffer.h"

#define MAX_CHANNELS       (32U)
#define PROTOCOL_CNT       (2U)    // 协议总个数（需与枚举位数匹配）
#define FRAME_DATA_MAX_LEN (1044U) // 取所有协议里的最大帧长度

typedef enum {
    CH_TYPE_NONE,
    CH_TYPE_MQTT,
    CH_TYPE_TCP,
    CH_TYPE_UDP,
    CH_TYPE_UART,
} ChannelType_t;

typedef enum {
    CH_RS485 = 0,
    CH_RS232_0,
    CH_RS232_1,
} ChannelIndex_t;

// 协议掩码枚举
typedef enum {
    PROTO_MASK_NONE    = (uint32_t)0x00000000,
    PROTO_MASK_IAP     = (uint32_t)0x00000001,
    PROTO_MASK_AH_MQTT = (uint32_t)0x00000002,
    // 新增协议掩码在此添加
    PROTO_MASK_ALL = (uint32_t)0xffffffff,
} proto_mask_t;

// 环形缓冲区分组枚举
typedef enum {
    RB_GROUP_IAP = 0, // 升级协议组（高可靠）
    RB_GROUP_PROTO,   // 普通业务协议组（可接受丢包）
    // 新增分组在此添加
    RB_GROUP_COUNT,
} rb_group_id_t;

// 帧探测结果
typedef enum {
    PROTO_PROBE_READY,
    PROTO_PROBE_WAIT,
    PROTO_PROBE_FAKE,
} proto_probe_sta_t;

typedef struct {
    ChannelType_t type;
    proto_mask_t protocol;
    union discription {
        struct mqtt_topic {
            char topic[64];
            uint16_t payload_len;
        } mqtt;
        struct tcp_netconn {
            struct netconn *conn;
        } tcp;
        struct udp_handle {
            struct netconn *conn;
            ip_addr_t src_ip;
            uint16_t src_port;
        } udp;
        struct uart_handle {
            UART_HandleTypeDef *huart;
        } uart;
    } handle;
} ch_meta_t;

typedef struct {
    ch_meta_t meta;                   // 信道元数据
    uint16_t data_len;                // 帧数据有效长度
    uint8_t data[FRAME_DATA_MAX_LEN]; // 帧数据副本
} frame_msg_t;

// 协议探测函数类型
typedef proto_probe_sta_t (*proto_probe_fn_t)(const ch_meta_t *, const ring_buffer_t *, uint32_t *, uint8_t *);

// 环形缓冲区实例
extern ring_buffer_t g_iap_rb;
extern ring_buffer_t g_proto_rb;
// 全局分组缓冲区指针数组
extern ring_buffer_t *g_group_ringbuf[RB_GROUP_COUNT];
// 协议 → 分组映射表（每个协议属于哪个分组）
extern uint8_t g_proto_to_group[PROTOCOL_CNT];
// 帧探测函数跳转表
extern proto_probe_fn_t g_proto_probers[PROTOCOL_CNT];
// 协议队列映射表
extern osMessageQueueId_t g_frame_queue[PROTOCOL_CNT];

extern osMessageQueueId_t g_meta_queue;

void channel_init(void);
void frame_dispatch_task(void *argument);
void channel_send(ch_meta_t *meta, uint8_t *data, uint16_t len);
uint8_t proto_index(uint32_t mask);
