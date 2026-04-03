#pragma once

#include "main.h"

#include "lwip.h"
#include "cmsis_os2.h"

#include "RingBuff.h"

#define MAX_CHANNELS (32U)
#define PROTOCOL_CNT (2U)

typedef enum channel_type {
    CH_TYPE_NONE,
    CH_TYPE_MQTT,
    CH_TYPE_TCP,
    CH_TYPE_UDP,
    CH_TYPE_UART,
} ChannelType_t;

typedef enum channel_index {
    CH_RS485 = 0,
    CH_RS232_0,
    CH_RS232_1,
} ChannelIndex_t;

typedef enum protocol_type_mask {
    ptcl_NONE    = (uint32_t)0x00000000,
    ptcl_IAP     = (uint32_t)0x00000001,
    ptcl_AH_MQTT = (uint32_t)0x00000002,
    ptcl_ALL     = (uint32_t)0xffffffff,
} ProtocolTypeMask_t;

typedef struct channel_metadata {
    ChannelType_t type;
    ProtocolTypeMask_t protocol;
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
} ch_metadata_t;

extern osMutexId_t gx_RingBufMutex;
extern osMessageQueueId_t gx_MDataQueue;
extern osMessageQueueId_t ParserQueue[32];
extern uint8_t ParserQueueCnt;

void CH_Initialize(void);
void MDataRouterTask(void *argument);
void CH_UniversalSend(ch_metadata_t *mdata, uint8_t *data, uint16_t len);
