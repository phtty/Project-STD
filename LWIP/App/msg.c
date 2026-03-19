#include "msg.h"

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

ChannelContext_t gx_Channels[MAX_CHANNELS] = {
    [CH_RS485] = {
        .type       = CH_TYPE_UART,
        .session_id = 0,
    },
    [CH_RS232_0] = {
        .type       = CH_TYPE_UART,
        .session_id = 0,
    },
    [CH_RS232_1] = {
        .type       = CH_TYPE_UART,
        .session_id = 0,
    },
};
osMutexId_t gx_RingBufMutex;
osMutexId_t gx_ChannelMutex;
osMessageQueueId_t gx_PacketMsg;

/**
 * @brief 创建数据收发使用的线程同步资源
 *
 */
void communication_init(void)
{
    osMutexAttr_t rb_mutex = {
        .name      = "gx_RingBufMutex",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0,
    };
    gx_RingBufMutex = osMutexNew(&rb_mutex);

    osMutexAttr_t ch_mutex = {
        .name      = "gx_ChannelMutex",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0,
    };
    gx_ChannelMutex = osMutexNew(&ch_mutex);

    osMessageQueueAttr_t msg_queue = {
        .name = "gx_PacketMsg",
    };
    gx_PacketMsg = osMessageQueueNew(MAX_CHANNELS, sizeof(MsgQueueItem_t), &msg_queue);
}

/**
 * @brief 在gx_Channels中找到一个未被使用信道
 *
 * @return int16_t 信道索引，若返回-1则是没找到
 */
int16_t sMsgFindEmptySlot(void)
{
    for (uint8_t i = 0; i < MAX_CHANNELS; i++)
        if (gx_Channels[i].type == CH_TYPE_NONE)
            return i;

    return -1;
}

/**
 * @brief 申请空闲信道
 *
 * @param type 通道类型
 * @return int 申请到的索引，若返回-1则申请失败
 */
int16_t Channel_Alloc(ChannelType_t type)
{
    // 防止多个信道同时申请
    osMutexAcquire(gx_ChannelMutex, osWaitForever);

    int16_t index = sMsgFindEmptySlot();

    if (index >= 0) {
        gx_Channels[index].type = type;
        gx_Channels[index].session_id++; // 会话自增，过滤失效的消息
    }

    osMutexRelease(gx_ChannelMutex);

    return index;
}

/**
 * @brief 释放信道
 *
 * @param index 信道索引
 */
void Channel_Free(int index)
{
    if (index < 0 || index >= MAX_CHANNELS)
        return;

    // 防止多个信道同时释放
    osMutexAcquire(gx_ChannelMutex, osWaitForever);

    gx_Channels[index].type = CH_TYPE_NONE;
    // 清空句柄等资源
    memset(&gx_Channels[index].handle, 0, sizeof(gx_Channels[index].handle));

    osMutexRelease(gx_ChannelMutex);
}

/**
 * @brief 清理所有网络相关信道
 *
 */
void Channel_FreeNet(void)
{
    // 防止其他信道同时释放
    osMutexAcquire(gx_ChannelMutex, osWaitForever);

    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (gx_Channels[i].type == CH_TYPE_TCP ||
            gx_Channels[i].type == CH_TYPE_UDP ||
            gx_Channels[i].type == CH_TYPE_MQTT) {

            gx_Channels[i].session_id++; // 增加 session_id，让队列里残余的消息失效

            gx_Channels[i].type = CH_TYPE_NONE; // 标记为空闲
        }
    }

    osMutexRelease(gx_ChannelMutex);
}

/**
 * @brief 通用发送函数
 *
 * @param msg 协议处理收到的消息队列数据
 * @param ch_idx 使用的信道索引
 * @param data 要发送的数据
 * @param len 要发送的数据长度
 */
void Universal_Send(MsgQueueItem_t *msg, uint8_t *data, uint16_t len)
{
    ChannelContext_t *ctx = &gx_Channels[msg->channel_index];

    // 校验当前槽位的session_id和消息产生时的session_id
    if (ctx->session_id != msg->session_id) {
        return; // 数据过期，直接丢弃
    }

    switch (ctx->type) {
        case CH_TYPE_UART:
            HAL_UART_Transmit(ctx->handle.uart.huart, data, len, 100);
            break;

        case CH_TYPE_TCP:
            netconn_write(ctx->handle.tcp.conn, data, len, NETCONN_COPY);
            break;

        case CH_TYPE_UDP:
            struct netbuf *nb = netbuf_new();
            netbuf_ref(nb, data, len);
            netconn_sendto(ctx->handle.udp.conn, nb, &msg->udp_src_ip, msg->udp_src_port);
            netbuf_delete(nb);
            break;

        case CH_TYPE_MQTT:
            break;

        default:
            break;
    }
}
