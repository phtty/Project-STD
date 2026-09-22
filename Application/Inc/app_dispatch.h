/**
 * @file    app_dispatch.h
 * @brief   多协议多通道调度框架（Application 层核心）
 *
 * 框架只持有指针，对象存储由各通道/协议模块静态持有，无堆开销。
 *
 * C 语言下的基类约定（新增通道/协议必须遵守）：
 *   1. 基类实例放在派生结构体的第一个成员 —— container_of 还原时偏移为 0，
 *      派生指针与基类指针同址，向上转型零开销；
 *   2. 回调一律回传对象指针：ccb_ops.send 收 ccb_t*，pcb_ops.probe 收 pcb_t*；
 *      派生实现用 container_of 取回自身属性。
 *
 * 基类首个成员是 name 而不是 ops：本工程排障主要靠 GDB / 内存转储 / RTT，
 * 拿到未知控制块指针时首字即名字，能立刻认出对象；ops 的位置不影响分派开销。
 *
 * 注册方式：
 *   协议在自己的 sw_app_initcall 里 app_proto_bind() 声明承载通道即完成注册；
 *   通道在自己的模块内静态持有控制块，通过导出的访问器（如 app_udp_ccb()）暴露。
 *   新增通道类型或协议均无需改动本文件。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cmsis_os2.h"
#include "container_of.h" /* 派生实现用 container_of 取回自身属性 */
#include "ring_buffer.h"

/* ---- 常量 ---- */
#define CCB_NOTIFY_MAX       (8U)     /**< 通道通知队列深度 */
#define CCB_PROTO_MAX       (4U)     /**< 单个通道最多承载的协议数 */

/** @brief 分发任务暂存上限（**平台约束，非协议知识**）
 *
 *  它是探针 scratch 的容量（`app_dispatch.c` 那份唯一的 `_msg_dispatch_buf`），
 *  **与各协议自己声明的 `payload_max` 无关** —— 帧比它长就投递不上来。
 *
 *  原值 1044 是照 IAP 的最长帧定的；抬到 1440 是为了让级联协议**一帧发完一整块
 *  卡位图**（单卡 224×50 1bpp = 1400，加帧头与载荷头共 1430）。
 *  抬它的代价只有那一份静态缓冲（+396 字节 CCMRAM）；各协议的队列元素按各自的
 *  `payload_max` 分配，不受影响。 */
#define FRAME_DATA_MAX_LEN (1440U)

/* ---- 通道连接状态 ---- */
typedef enum {
    CCB_STATE_DOWN = 0,
    CCB_STATE_UP   = 1,
} ccb_state_t;

/* ---- 帧探测结果 ---- */
typedef enum {
    PCB_PROBE_READY, /**< 完整帧就绪 */
    PCB_PROBE_WAIT,  /**< 数据不足，等待更多字节 */
    PCB_PROBE_FAKE,  /**< 伪帧头（如误匹配的 0x5A/0xFF），跳过 1 字节重试 */
    PCB_PROBE_SKIP,  /**< 帧结构合法但不属于本设备，跳过整帧 */
} pcb_probe_sta_t;

/* ---- 基类前置声明 ---- */
typedef struct ccb ccb_t;
typedef struct pcb pcb_t;

/**
 * @brief 接收来源描述 —— 与 ccb_dst_t 对称
 *
 * 框架不解释语义，各通道只填自己能说明的字段，其余留 nullptr。
 *
 * **所有权**：调用方传进来的指针只在 app_ccb_dispatch 调用期间需要有效 ——
 * 框架会把内容拷进自己的通知元素（ccb_notify_t），探针拿到的是那份副本，
 * 因此不受后续派发覆盖的影响。
 * **归属（实际只有一半保证）**：排空按本次派发的字节数计量，因此一次通知最多
 * 消费自己那批字节 —— 先到的帧不会读到后到的来源。
 *
 * 反方向**没有**保证：先到者未排完的余量（半帧）留在缓冲区里，会由**后到者的
 * 来源**去解析。补齐需要"挂起来源"的额外状态（记住余量属于哪个来源，下次派发
 * 时先用旧来源把它排空再切到新来源），本工程未实现，此处也不假装它有。
 *
 * 影响面：只有 MQTT 通道提供来源，而它每条 PUBLISH 都是完整报文，
 * "半帧跨两条报文且主题不同"本身就不是有效流量 —— 实际不可达。
 */
typedef struct {
    const char *topic; /**< 来源字符串地址，如 MQTT 主题（其余通道为 nullptr）*/
} ccb_src_t;

/** 来源主题的最大长度（含结尾 NUL）。需要更长的通道请一并调大此值。 */
#define CCB_SRC_TOPIC_MAX (48U)


/**
 * @brief 协议帧探测函数（proto_ops 的唯一方法）
 *
 * 调用者已持有 self->rb 的锁，因此内部所有 rb API 必须传 nullptr 跳过二次加锁。
 * 只窥视、不消费 —— 消费由框架根据返回值完成。
 *
 * @param self          协议控制块；派生实现用 container_of 取回自身属性
 * @param ccb            数据来源通道
 * @param src           本帧来源描述（可为 nullptr）；只在本次调用内有效
 * @param scratch       框架提供的暂存区（即入队缓冲），容量 scratch_size
 * @param scratch_size  暂存区容量；帧长超过它时必须返回 SKIP，不得越界写
 * @param total_len     输出：完整帧长度（READY / SKIP 时有效）
 * @param aux           输出：协议层分类（命令码等）。READY 时由框架存进
 *                      frame_msg_t.aux 一并投递给协议任务 —— 探针在投递当下
 *                      能看到的 per-message 信息（如来源地址的分类结果）由此
 *                      传给协议任务，不必事后从通道对象上再捞一次。
 * @return 见 pcb_probe_sta_t
 */
typedef pcb_probe_sta_t (*pcb_probe_fn_t)(pcb_t *self, const ccb_t *ccb,
                                              const ccb_src_t *src, uint8_t *scratch,
                                              uint16_t scratch_size, uint32_t *total_len,
                                              uint8_t *aux);

/** @brief 协议虚表 —— 追加新方法时不影响已有协议 */
typedef struct pcb_ops {
    pcb_probe_fn_t probe;
} pcb_ops_t;

/** @brief 协议控制块（基类）— 协议模块静态持有 */
struct pcb {
    const char        *name;        /**< 首成员：排障时用于识别对象 */
    const pcb_ops_t *ops;
    ring_buffer_t     *rb;          /**< 协议自有缓冲区（须先 rb_init）*/
    osMessageQueueId_t queue;       /**< 协议自有帧队列，在 sw initcall 中创建 */
    uint16_t           payload_max; /**< 本协议最长帧，须 ≤ FRAME_DATA_MAX_LEN */
};

/* ---- 通道抽象（OCP 虚表）---- */

/**
 * @brief 发送目的地
 *
 * 框架不解释语义：各通道只取自己能理解的字段，其余忽略并退化为默认行为。
 * 传 nullptr 表示"回复到本帧来源"，其含义由各通道自行定义
 * （TCP = 本连接、UDP = 最近一次来源、RS485 = 总线对端、MQTT = 本帧来源主题）。
 *
 * 这是"随消息走的寻址信息"，不是通道配置：broker 地址/端口/凭据才是通道配置。
 * 协议负责表达意图，通道负责翻译成自己的机制。
 */
typedef struct {
    bool        broadcast; /**< 请求组播/广播投递（UDP 支持；RS485/TCP 忽略）*/
    const char *topic;     /**< 目的字符串地址，如 MQTT 主题（MQTT 支持；其余忽略）*/
} ccb_dst_t;

typedef struct ccb_ops {
    /** 发送；dst 为 nullptr 表示回复到本帧来源 */
    int32_t (*send)(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len);
} ccb_ops_t;

/** @brief 通道控制块（基类）— 通道模块静态持有 */
struct ccb {
    const char     *name;                /**< 首成员：排障时用于识别对象 */
    const ccb_ops_t *ops;
    uint8_t         state;               /**< ccb_state_t */
    pcb_t          *protos[CCB_PROTO_MAX];/**< 本通道承载的协议（绑定顺序）*/
    uint8_t         proto_cnt;
};

/* ---- 帧消息（协议队列元素）---- */

typedef struct {
    ccb_t *ccb;
    uint16_t   data_len;
    uint8_t    aux; /**< 探针给出的协议层分类（命令码等），框架原样带给协议任务 */
    /* 4 字节对齐：探针会把该暂存区直接 cast 成含 uint32 字段的帧结构体
     * （如 iap_frame_t）访问，未对齐的字访问在 Cortex-M4 上要么变慢要么触发异常。
     * 加该属性后 offsetof(data) == 8 == sizeof(frame_msg_t)；
     * aux 落在 data_len 之后的填充字节里，不增加 sizeof。 */
    uint8_t data[] __attribute__((aligned(4)));
} frame_msg_t;

/* ---- 接收事件监听：框架以注册方式通知，不直接依赖任何业务模块 ---- */

typedef void (*dispatch_rx_listener_t)(void);

/* ---- 调度上下文 ---- */

typedef struct {
    osMessageQueueId_t ccb_queue; /**< 通道通知队列（元素为 ccb_t *）*/
} dispatch_ctx_t;

extern dispatch_ctx_t g_dispatch;
extern osThreadId_t g_dispatch_task_handle;

/* ---- 调度 API ---- */

/** @brief 声明协议承载的通道 —— 即完成注册（须在任何通道任务启动前调用）*/
void app_proto_bind(pcb_t *pcb, ccb_t *ccb);

/** @brief 通道接收入口：写入本通道各协议的缓冲区并唤醒分发任务
 *  @param src 本帧来源描述，无来源概念的通道传 nullptr */
void app_ccb_dispatch(const ccb_t *ccb, const ccb_src_t *src, const uint8_t *data,
                          uint16_t len);

/** @brief 发送到显式目的地（虚表分派）；dst 为 nullptr 等同回复到本帧来源
 *
 *  @return 通道 ops->send 的结果；<0 表示**没发出去**（通道未 UP、参数被拒等）。
 *  **调用方该看的就看** —— 尤其是"发得对不对"会决定现场怎么查。
 *  历史上这两条返回 void，把结果就地丢了，于是"帧没发出去"与"发出去了但对方没收"
 *  完全分不开。已有的调用点忽略返回值也不受影响。 */
int32_t ccb_send_to(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len);

/** @brief 回复到本帧来源（ccb_send_to(ccb, nullptr, ...) 的便捷形式） */
int32_t ccb_send(ccb_t *ccb, const uint8_t *data, uint16_t len);

/** @brief 注册接收事件监听（每收到一段数据触发一次）*/
void app_dispatch_register_rx_listener(dispatch_rx_listener_t fn);

void frame_dispatch_task(void *argument);
void app_dispatch_init(void);
