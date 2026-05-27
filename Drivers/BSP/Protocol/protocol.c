#include "protocol.h"

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"
#include "usart.h"

#include "iap.h"
#include "ah_mqtt.h"
#include "mqtt_app.h"

osMutexId_t g_ringbuf_mutex;
osMessageQueueId_t g_meta_queue;
osThreadId_t g_dispatch_task_handle;

// 协议分组对应的缓冲区表
RingBuff_t *g_group_ringbuf[RB_GROUP_COUNT] = {
    [RB_GROUP_IAP]   = &g_iap_ringbuf,
    [RB_GROUP_PROTO] = &xProtocol_RB,
};

// 协议序号对应的分组表，在初始化函数里定义
uint8_t g_proto_to_group[PROTOCOL_CNT];

// 每个协议解析任务对应的消息队列句柄，在对应协议解析任务中创建
osMessageQueueId_t g_frame_queue[PROTOCOL_CNT];

/**
 * @brief 创建数据收发使用的线程同步资源
 *
 */
void channel_init(void)
{
    osMutexAttr_t rb_mutex_attr = {
        .name      = "g_ringbuf_mutex",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0,
    };
    g_ringbuf_mutex = osMutexNew(&rb_mutex_attr);

    // 接收任务向元数据路由任务发送的队列
    osMessageQueueAttr_t meta_queue_attr = {
        .name = "g_meta_queue",
    };
    g_meta_queue = osMessageQueueNew(MAX_CHANNELS, sizeof(ch_meta_t), &meta_queue_attr);

    // 创建元数据路由任务
    const osThreadAttr_t frame_dispatch_task_attr = {
        .name       = "frame_dispatch_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    g_dispatch_task_handle = osThreadNew(frame_dispatch_task, NULL, &frame_dispatch_task_attr);

    // 注册IAP协议任务
    g_iap_task_handle = osThreadNew(iap_handle_task, NULL, &IapTask_attributes);
    // 注册IAP协议相关资源
    g_proto_to_group[proto_index(PROTO_MASK_IAP)] = RB_GROUP_IAP;
    g_proto_probers[proto_index(PROTO_MASK_IAP)]  = iap_probe_frame;
    // 注册安徽mqtt协议任务
    g_ah_mqtt_task_handle = osThreadNew(ah_mqtt_handle_task, NULL, &ProtocolTask_attributes);
    // 注册安徽mqtt协议相关资源
    g_proto_to_group[proto_index(PROTO_MASK_AH_MQTT)] = RB_GROUP_PROTO;
    g_proto_probers[proto_index(PROTO_MASK_AH_MQTT)]  = ah_mqtt_probe_frame;
}

// 协议探测函数注册表（在各协议模块中填充）
proto_probe_fn_t g_proto_probers[PROTOCOL_CNT];
// 元数据路由任务
void frame_dispatch_task(void *argument)
{
    static ch_meta_t meta;
    static frame_msg_t msg;
    uint32_t frame_len;
    uint8_t aux;

    for (;;) {
        if (osMessageQueueGet(g_meta_queue, &meta, NULL, osWaitForever) != osOK) {
            continue;
        }

        bool group_visited[RB_GROUP_COUNT] = {false};

        // 遍历所有协议，按分组批量处理
        for (uint8_t i = 0; i < PROTOCOL_CNT; i++) {
            uint32_t mask  = (1U << i);
            uint8_t gid    = g_proto_to_group[i];
            RingBuff_t *rb = g_group_ringbuf[gid];

            if ((meta.protocol & mask) == 0)
                continue;

            if (group_visited[gid])
                continue; // 该分组已处理过

            if (rb == NULL)
                continue;

            osMutexAcquire(g_ringbuf_mutex, osWaitForever);
            uint16_t avail = BSP_RB_GetAvailable(rb); // 获取缓冲区内可读数据量
            osMutexRelease(g_ringbuf_mutex);
            while (avail > 0) {
                bool parsed   = false;
                bool all_wait = true;

                // 在该分组内遍历所有协议
                for (uint8_t j = 0; j < PROTOCOL_CNT; j++) {
                    uint32_t inner_mask = (1U << j);

                    if ((meta.protocol & inner_mask) == 0)
                        continue;
                    if (g_proto_to_group[j] != gid)
                        continue; // 只探测本分组的协议
                    if (g_proto_probers[j] == NULL)
                        continue;

                    // 调用对应协议的帧探测函数
                    osMutexAcquire(g_ringbuf_mutex, osWaitForever);
                    proto_probe_sta_t state = g_proto_probers[j](&meta, rb, &frame_len, &aux);
                    osMutexRelease(g_ringbuf_mutex);

                    if (state == PROTO_PROBE_READY) { // 处理帧探测成功的情况
                        osMutexAcquire(g_ringbuf_mutex, osWaitForever);

                        if (avail >= frame_len) { // 检查缓冲区内可用数据长度是否匹配帧探测的长度
                            uint16_t actual = BSP_RB_GetByte_Bulk(rb, msg.data, frame_len);
                            avail           = BSP_RB_GetAvailable(rb); // 更新缓冲区内可读数据量
                            osMutexRelease(g_ringbuf_mutex);

                            if (actual == frame_len) { // 检查实际读取的数据长度是否匹配帧探测的长度
                                msg.data_len = frame_len;
                                msg.meta     = meta;
                                osMessageQueuePut(g_frame_queue[j], &msg, 0, 0);

                            } else { // 异常情况，跳过已读字节，避免死锁
                                osMutexAcquire(g_ringbuf_mutex, osWaitForever);
                                BSP_RB_SkipBytes(rb, actual);
                                avail = BSP_RB_GetAvailable(rb); // 更新缓冲区内可读数据量
                                osMutexRelease(g_ringbuf_mutex);
                            }

                        } else {
                            // 数据不足，目前架构理论上不会产生，此操作为增加健壮性
                            osMutexRelease(g_ringbuf_mutex);
                        }

                        parsed   = true;
                        all_wait = false;
                        break; // 成功解析一帧，退出协议遍历，继续尝试解析下一帧

                    } else if (state == PROTO_PROBE_WAIT) { // 处理数据不够的情况
                        // 数据不足，保持 all_wait 状态

                    } else if (state == PROTO_PROBE_FAKE) {
                        all_wait = false; // FAKE 状态，说明数据存在但非法
                    }
                }

                if (!parsed) {
                    if (all_wait) {
                        break; // 分组内所有协议均数据不足，等待新数据

                    } else {
                        osMutexAcquire(g_ringbuf_mutex, osWaitForever);
                        BSP_RB_SkipBytes(rb, 1);
                        avail--; // 更新缓冲区内可读数据量
                        osMutexRelease(g_ringbuf_mutex);
                    }
                }
            }
            group_visited[gid] = true;
        }
    }
}

/**
 * @brief 通用发送函数
 *
 * @param meta 接收到的元数据
 * @param data 要发送的数据
 * @param len 要发送的数据长度
 */
void channel_send(ch_meta_t *meta, uint8_t *data, uint16_t len)
{
    switch (meta->type) {
        case CH_TYPE_UART:
            if (meta->handle.uart.huart == &huart1)
                RS485_RE = 1;
            HAL_UART_Transmit(meta->handle.uart.huart, data, len, 100);
            if (meta->handle.uart.huart == &huart1)
                RS485_RE = 0;
            break;

        case CH_TYPE_TCP:
            netconn_write(meta->handle.tcp.conn, data, len, NETCONN_COPY);
            break;

        case CH_TYPE_UDP:
            struct netbuf *nb = netbuf_new();
            netbuf_ref(nb, data, len);
            netconn_sendto(meta->handle.udp.conn, nb, &(meta->handle.udp.src_ip), meta->handle.udp.src_port);
            netbuf_delete(nb);
            break;

        case CH_TYPE_MQTT:
            mqtt_send_data(meta->handle.mqtt.topic, (char *)data);
            break;

        default:
            break;
    }
}

/**
 * @brief 从协议掩码转换为协议索引
 *
 * @param mask 协议掩码
 *
 * @retval 转换得到的协议索引，若为0xff则为无效协议
 */
uint8_t proto_index(uint32_t mask)
{
    // 若为无协议，则返回FF
    if (mask == 0)
        return 0xff;

    // 返回后置0个数
    return (uint8_t)__builtin_ctz(mask);
}

/**
 * @brief 将数据写入所有可能的分组缓冲区，并发送元数据通知
 *
 * @param meta  信道元数据（需预先填充 type、protocol、handle）
 * @param data  数据指针
 * @param len   数据长度
 */
void channel_dispatch_data(const ch_meta_t *meta, const uint8_t *data, uint16_t len)
{
    bool group_visited[RB_GROUP_COUNT] = {false};

    // 遍历所有协议，找出涉及的分组并写入对应缓冲区
    for (uint8_t i = 0; i < PROTOCOL_CNT; i++) {
        uint32_t mask = (1U << i);
        if ((meta->protocol & mask) == 0)
            continue; // 没有这个协议

        uint8_t gid = g_proto_to_group[i];
        if (group_visited[gid])
            continue; // 该分组已写入过

        RingBuff_t *rb = g_group_ringbuf[gid];
        if (rb != NULL) {
            osMutexAcquire(g_ringbuf_mutex, osWaitForever);
            BSP_RB_PutByte_Bulk(rb, data, len);
            osMutexRelease(g_ringbuf_mutex);
        }
        group_visited[gid] = true;
    }

    // 发送元数据通知帧分发任务
    osMessageQueuePut(g_meta_queue, meta, 0, 0);
}
