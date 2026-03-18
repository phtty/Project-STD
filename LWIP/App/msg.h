#pragma once

#include "main.h"

#include "lwip.h"
#include "cmsis_os2.h"

#include "RingBuff.h"

#define MAX_CHANNELS (8U)

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

typedef struct {
    ChannelType_t type;
    uint32_t session_id;
    union {
        struct {
            char topic[64];
        } mqtt;
        struct {
            struct netconn *conn;
        } tcp;
        struct {
            struct netconn *conn;
        } udp;
        struct {
            UART_HandleTypeDef *huart;
        } uart;
    } handle;
} ChannelContext_t;

// 数据收发任务传递给协议解析的结构体
typedef struct {
    uint8_t channel_index; // 对应gx_Channels的索引
    uint32_t session_id;
    ip_addr_t udp_src_ip;
    uint16_t udp_src_port;
} MsgQueueItem_t;

extern ChannelContext_t gx_Channels[MAX_CHANNELS];
extern osMutexId_t gx_RingBufMutex;
extern osMutexId_t gx_ChannelMutex;
extern osMessageQueueId_t gx_PacketMsg;

void communication_init(void);
int16_t sMsgFindEmptySlot(void);
int16_t Channel_Alloc(ChannelType_t type);
void Channel_Free(int index);
void Channel_FreeNet(void);
void Universal_Send(MsgQueueItem_t *msg, uint8_t *data, uint16_t len);
