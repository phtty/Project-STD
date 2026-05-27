/**
 * @file        app_dispatch.c
 * @brief       协议调度框架实现（Application 层核心）
 *
 * 数据流（接收路径）:
 *   物理接口 → 通道任务 → app_channel_dispatch() → ring buffer + g_ch_queue
 *       → frame_dispatch_task() → 协议探测 → frame_queue → 协议处理任务
 *
 * 通道发送通过 ch_ops 虚表分派（OCP 模式），不依赖具体传输实现。
 * 全部调度状态收敛于 dispatch_ctx_t g_dispatch。
 */

#include "app_dispatch.h"
#include "cmsis_os2.h"
#include "bit_utils.h"
#include "initcall.h"

/* ================================================================
 *  环形缓冲区池 — 编译期静态分配，按 id 复用
 *
 *  app_proto_acquire_buf(id, size) 按 id 返回预分配的 ring buffer。
 *  同一 id 多次调用返回同一指针（分组复用），首次调用时 rb_init。
 *  id 与具体的 RB_DEFINE 通过 if(id==N) 硬绑定，扩容需同步修改。
 * ================================================================ */

RB_DEFINE(g_rb0, 2048); /**< id=0: IAP 协议专用（高可靠性组） */
RB_DEFINE(g_rb1, 2048); /**< id=1: 通用业务协议组（LDI 等共用） */
RB_DEFINE(g_rb2, 2048); /**< id=2: 暂未使用，预留 */
RB_DEFINE(g_rb3, 2048); /**< id=3: 暂未使用，预留 */

/* ================================================================
 *  调度上下文 — 全部运行时的唯一状态聚合
 *
 *  新增调度相关字段只需在此结构体中增加即可，
 *  所有调度函数统一通过 g_dispatch 访问。
 * ================================================================ */

dispatch_ctx_t g_dispatch;                  /**< 全局调度上下文 */
static osThreadId_t g_dispatch_task_handle; /**< 帧分发任务句柄 */

/* ================================================================
 *  工具函数
 * ================================================================ */

/**
 * @brief 协议掩码 → 数组索引
 *
 * 通过 bit_ctz 计算尾零位数，将 bitmask 映射为 0~31 的索引。
 * 例如: PROTO_MASK_IAP(0b0001) → 0, PROTO_MASK_LDI(0b0010) → 1
 *
 * @param mask  协议掩码（必须是 2 的幂或 0）
 * @return      对应的数组索引，mask=0 返回 0xFF
 */
uint8_t proto_index(uint32_t mask)
{
    if (mask == 0) return 0xff;
    return (uint8_t)bit_ctz(mask);
}

/* ================================================================
 *  协议注册 — 协议模块通过 sw_device_initcall 自注册
 *
 *  各协议模块的 init 函数中调用以下函数完成注册：
 *    1. app_proto_acquire_buf() — 获取环形缓冲区
 *    2. app_proto_register()    — 注册探测函数 + 缓冲区指针
 *    3. app_proto_bind_channel() — 声明该协议走哪些通道
 *    4. osThreadNew()           — 创建协议处理任务
 *
 *  proto_count 由 app_proto_register 内部递增，无需手动维护。
 * ================================================================ */

/**
 * @brief 注册协议探测函数和环形缓冲区
 * @param mask   协议掩码（PROTO_MASK_IAP / PROTO_MASK_LDI）
 * @param probe  帧探测函数指针
 * @param rb     环形缓冲区指针（来自 app_proto_acquire_buf）
 */
void app_proto_register(uint32_t mask, proto_probe_fn_t probe, ring_buffer_t *rb)
{
    uint8_t idx = proto_index(mask);
    if (idx >= PROTO_MAX_COUNT) return;

    g_dispatch.proto_rb[idx]    = rb;    /**< 协议 → 环形缓冲区 */
    g_dispatch.proto_probe[idx] = probe; /**< 协议 → 探测函数 */
    g_dispatch.proto_count++;            /**< 运行时自动计数 */
}

/**
 * @brief 设置协议的帧消息队列
 * @param mask   协议掩码
 * @param queue  队列句柄（osMessageQueueId_t，存储为 void *）
 *
 * 协议处理任务创建队列后调用，框架据此将完整帧推入对应队列。
 */
void app_proto_set_frame_queue(uint32_t mask, osMessageQueueId_t queue)
{
    uint8_t idx = proto_index(mask);
    if (idx >= PROTO_MAX_COUNT) return;
    g_dispatch.frame_queue[idx] = queue;
}

/**
 * @brief 绑定协议到通道（声明该协议监听哪些通道的数据）
 * @param mask   协议掩码
 * @param ch_id  通道标识（CH_ID_RS485 等）
 *
 * 内部为 OR 累积——多个协议可绑定同一通道，同一协议也可绑定多个通道。
 * ch_proto_map 初始全零，全部由协议模块通过此函数声明。
 */
void app_proto_bind_channel(proto_mask_t mask, channel_id_t ch_id)
{
    g_dispatch.ch_proto_map[ch_id] |= mask;
}

/**
 * @brief 从缓冲区池获取环形缓冲区
 * @param id    缓冲区编号（0 ~ RB_CNT_MAX-1）
 * @param size  容量（当前未使用，预留）
 * @return      环形缓冲区指针，失败返回 nullptr
 *
 * 同一 id 首次调用时 rb_init（互斥锁名 = "rb_N"），
 * 后续调用直接返回已有指针，实现分组复用。
 * 所有缓冲区为编译期静态分配，无堆开销。
 */
ring_buffer_t *app_proto_acquire_buf(uint8_t id, uint16_t size)
{
    if (id >= RB_CNT_MAX) return nullptr;
    (void)size;

    static const char *names[RB_CNT_MAX] = {"rb_0", "rb_1", "rb_2", "rb_3"};
    ring_buffer_t *rb                    = nullptr;

    /* id → 预分配的 RB_DEFINE 硬绑定 */
    if (id == 0) rb = &g_rb0;
    if (id == 1) rb = &g_rb1;
    if (id == 2) rb = &g_rb2;
    if (id == 3) rb = &g_rb3;
    if (rb == nullptr) return nullptr;

    /* 已初始化则直接返回 */
    if (g_dispatch.buf_pool[id] != nullptr)
        return g_dispatch.buf_pool[id];

    /* 首次使用：创建互斥锁 + 缓存指针 */
    rb_init(rb, names[id]);
    g_dispatch.buf_pool[id] = rb;
    return rb;
}

/* ================================================================
 *  调度系统初始化 — sw_subsys_initcall(2)，RTOS 后自动调用
 *
 *  创建 ch_queue → 创建 frame_dispatch_task → 返回。
 *  协议模块的注册由 sw_device_initcall(3) 在之后执行。
 * ================================================================ */

void app_dispatch_init(void)
{
    /* 通道指针通知队列：max 32 个槽位，每项 sizeof(channel_t *) = 4 字节 */
    osMessageQueueAttr_t ch_queue_attr = {.name = "g_ch_queue"};
    g_dispatch.ch_queue                = osMessageQueueNew(MAX_CHANNELS, sizeof(channel_t *), &ch_queue_attr);

    /* 帧分发任务：遍历 ring buffer，调用各协议的探测函数 */
    const osThreadAttr_t frame_dispatch_task_attr = {
        .name       = "frame_dispatch_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    g_dispatch_task_handle = osThreadNew(frame_dispatch_task, nullptr, &frame_dispatch_task_attr);
}
sw_subsys_initcall(app_dispatch_init);

/* ================================================================
 *  frame_dispatch_task — 帧分发引擎（核心调度循环）
 *
 *  流程:
 *    1. 阻塞等待 g_dispatch.ch_queue 中的通道指针通知
 *    2. 根据 channel_t->ch_id 查表获得协议掩码 (ch_proto_map)
 *    3. 遍历所有已注册协议 (proto_count)
 *    4. 对每个缓冲区，调用协议的探测函数 (probe) 检测完整帧
 *    5. 探测成功 → 从 ring buffer 读出完整帧 → 推入协议队列
 *    6. 探测失败 → 跳过 1 字节继续尝试
 *
 *  关键设计:
 *    - 一个通知可能触发多帧解析 (while(avail>0))
 *    - 多个协议可能共享同一缓冲区，通过指针去重避免重复遍历
 *    - 持锁跨整轮探测+读取，消除 TOCTOU 窗口
 * ================================================================ */

void frame_dispatch_task(void *argument)
{
    channel_t *ch;          /**< 来源通道指针（从 ch_queue 取出） */
    static frame_msg_t msg; /**< 帧消息（推入协议队列） */
    uint32_t frame_len = 0; /**< 探测到的完整帧长度 */
    uint8_t aux        = 0; /**< 辅助信息（如命令码） */

    for (;;) {
        /* 阻塞等待：任一通道收到数据时唤醒 */
        if (osMessageQueueGet(g_dispatch.ch_queue, &ch, NULL, osWaitForever) != osOK)
            continue;

        /* 通道已销毁或未初始化，跳过 */
        if (ch == nullptr) continue;

        /* 根据通道 ID 查表获得该通道承载的协议掩码 */
        proto_mask_t proto = g_dispatch.ch_proto_map[ch->ch_id];

        /* 外循环：遍历所有已注册协议 */
        for (uint8_t i = 0; i < g_dispatch.proto_count; i++) {
            uint32_t mask     = (1U << i);
            ring_buffer_t *rb = g_dispatch.proto_rb[i];

            /* 跳过：通道不承载此协议 / 空缓冲区 */
            if ((proto & mask) == 0) continue;
            if (rb == nullptr) continue;

            /* 跳过已处理过的缓冲区（多个协议共享同一 RB 时去重） */
            bool dup = false;
            for (uint8_t k = 0; k < i; k++)
                if (g_dispatch.proto_rb[k] == rb) {
                    dup = true;
                    break;
                }
            if (dup) continue;

            /* 持锁跨整轮探测+读取，消除 TOCTOU */
            rb_lock(rb);
            uint16_t avail = rb_avail(rb, nullptr);

            /* 内循环：从同一缓冲区中连续提取多帧 */
            while (avail > 0) {
                bool parsed   = false; /**< 本轮是否成功解析一帧 */
                bool all_wait = true;  /**< 所有协议是否都返回 WAIT */

                /* 按协议优先级顺序探测 */
                for (uint8_t j = 0; j < g_dispatch.proto_count; j++) {
                    uint32_t inner_mask = (1U << j);

                    if ((proto & inner_mask) == 0) continue;
                    if (g_dispatch.proto_rb[j] != rb) continue;
                    if (g_dispatch.proto_probe[j] == nullptr) continue;

                    /* 调用探测函数（调用者持锁，探测内部传 nullptr 跳过锁） */
                    proto_probe_sta_t state = g_dispatch.proto_probe[j](ch, rb, &frame_len, &aux);

                    if (state == PROTO_PROBE_READY) {
                        /* 完整帧就绪：从缓冲区读出 → 推入协议处理队列 */
                        if (avail >= frame_len) {
                            uint16_t actual = rb_read(rb, msg.data, frame_len, nullptr);
                            avail           = rb_avail(rb, nullptr);

                            if (actual == frame_len) {
                                msg.data_len = frame_len;
                                msg.ch       = ch;
                                osMessageQueuePut(g_dispatch.frame_queue[j], &msg, 0, 0);
                            } else {
                                /* 异常：读出字节数不匹配，丢弃已读部分 */
                                rb_skip(rb, actual, nullptr);
                                avail = rb_avail(rb, nullptr);
                            }
                        }
                        parsed   = true;
                        all_wait = false;
                        break; /* 成功解析一帧，回到 while 继续下一帧 */

                    } else if (state == PROTO_PROBE_SKIP) {
                        /* 帧结构合法但不属于本设备，跳过整帧 */
                        if (avail >= frame_len) {
                            avail -= rb_skip(rb, frame_len, nullptr);
                            parsed = true;
                        }
                        /* 数据不足时退化为 WAIT */
                        all_wait = false;

                    } else if (state == PROTO_PROBE_WAIT) {
                        /* 数据不足，协议等待更多字节 —— 继续探测下一个协议 */
                    } else if (state == PROTO_PROBE_FAKE) {
                        /* 伪帧头（如误匹配的 0x5A），跳过 1 字节重试 */
                        all_wait = false;
                    }
                }

                /* 无协议成功解析:
                 *   全部 WAIT → 退出内循环，等待更多数据
                 *   至少一个 FAKE → 跳过 1 字节继续重试
                 */
                if (!parsed) {
                    if (all_wait)
                        break;
                    else
                        avail -= rb_skip(rb, 1, nullptr);
                }
            }
            rb_unlock(rb);
        }
    }
}

/* ================================================================
 *  app_channel_send — 通道发送（OCP：虚表分派）
 *
 *  协议处理任务调用此函数回复数据，通过 ch_ops 虚表分派到
 *  具体通道的 send 实现。新增通道类型无需修改此函数。
 *
 *  安全守卫: ch->ops == nullptr 表示通道已销毁，拒绝发送。
 *            TCP/UDP 连接任务退出前会置 ops = nullptr。
 * ================================================================ */

void channel_send(channel_t *ch, uint8_t *data, uint16_t len)
{
    if (ch == nullptr || ch->ops == nullptr)
        return;
    if (ch->ops->send)
        ch->ops->send(ch, data, len);
}

/* ================================================================
 *  app_channel_dispatch — 通道接收分发
 *
 *  所有通道任务的接收路径统一入口：
 *    1. 根据 ch->ch_id 查 ch_proto_map 获得协议掩码
 *    2. 遍历所有协议，将数据写入对应的 ring buffer
 *    3. 共享同一 RB 的多个协议通过 seen[] 指针去重，避免重复写入
 *    4. 向 ch_queue 发送通道指针通知帧分发任务
 *
 *  注意：通道任务不要直接操作 ring buffer 或 mutex。
 * ================================================================ */

void app_channel_dispatch(const channel_t *ch, const uint8_t *data, uint16_t len)
{
    /* 根据通道 ID 查表获得协议掩码 */
    proto_mask_t proto                   = g_dispatch.ch_proto_map[ch->ch_id];
    ring_buffer_t *seen[PROTO_MAX_COUNT] = {nullptr}; /**< 已写入的 RB 指针集合 */

    /* 遍历所有协议，将数据写入匹配的环形缓冲区 */
    for (uint8_t i = 0; i < g_dispatch.proto_count; i++) {
        uint32_t mask = (1U << i);
        if ((proto & mask) == 0) continue;

        ring_buffer_t *rb = g_dispatch.proto_rb[i];
        if (rb == nullptr) continue;

        /* RB 指针去重：多个协议共享同一缓冲区时只写一次 */
        bool dup = false;
        for (uint8_t k = 0; seen[k] != nullptr; k++)
            if (seen[k] == rb) {
                dup = true;
                break;
            }
        if (dup) continue;

        rb_write(rb, data, len, rb->mutex);
        seen[i] = rb;
    }

    /* 通知帧分发任务：传入通道指针的地址（不是通道结构体的地址）
     * 队列每项大小为 sizeof(channel_t *)，拷贝的是指针值本身。
     * 帧分发任务通过 osMessageQueueGet(&ch, ...) 读出指针。 */
    osMessageQueuePut(g_dispatch.ch_queue, &ch, 0, 0);
}

/* ================================================================
 *  通道注册表 — 通道 init/deinit 时维护，Application 层按 ID 查询
 *
 *  app_channel_register(ch_id, ch)：通道 init 时注册，deinit 时传 nullptr 清空
 *  app_channel_get(ch_id)：返回已注册的 channel_t *（可能为 nullptr）
 *
 *  Device 层调用 register（通知模式，与 app_channel_dispatch 一致），
 *  Application 层调用 get（查询抽象指针，不触碰 Device 层全局变量）。
 * ================================================================ */

void app_channel_register(channel_id_t ch_id, channel_t *ch)
{
    if (ch_id < CH_ID_MAX)
        g_dispatch.channels[ch_id] = ch;
}

channel_t *app_channel_get(channel_id_t ch_id)
{
    if (ch_id < CH_ID_MAX)
        return g_dispatch.channels[ch_id];
    return nullptr;
}
