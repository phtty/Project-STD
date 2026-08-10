# STD 项目的 A/B 协议链式解析规划

## 1. 背景

STD 项目当前已经具备统一的通道接入层、协议分发层和协议自注册机制。现有实现中，`app_dispatch` 已完成以下能力：

- 通道接收后写入共享 ring buffer
- `frame_dispatch_task()` 从 ring buffer 中探测完整帧
- 每个协议模块通过 `sw_app_initcall(...)` 自注册
- 协议模块通过 `app_proto_register()`、`app_proto_bind_channel()`、`app_proto_set_frame_queue()` 完成接入

在现有架构基础上，可以进一步演进为 **A/B 协议链式解析**：

- 一帧数据到达后，先交给协议 A 探测
- A 判断失败时，再交给协议 B
- 若 A 只是“数据未收完整”，必须继续等待，不能让 B 抢走该帧
- 最终只能有一个协议消费该帧，避免重复执行

该方案适合：

- 同一物理通道上需要兼容多种协议
- 新旧协议过渡期
- 协议帧结构相近，需要逐级尝试匹配
- 希望保留当前通道层、分发层、协议层的分层架构

---

## 2. 目标

### 2.1 功能目标

- 支持同一帧数据按优先级依次尝试多个协议模块
- 支持协议 A 失败后自动切换到协议 B
- 支持“数据未收完整”的等待态，避免误判
- 支持协议模块在 EIDE 中加入链接后，通过各自 `xx_proto.init` 自注册
- 支持每个协议在自注册时声明自己的 `priority`

### 2.2 工程目标

- 尽量少改通道层代码
- 尽量不改原有协议模块的业务执行逻辑
- 在 STM32F407 平台上控制 RAM 占用
- 避免引入过多额外任务和队列

---

## 3. 现有架构现状

结合当前代码，STD 项目已具备 A/B 链式解析的基础：

### 3.1 `app_dispatch` 已有的关键对象

- `g_dispatch.ch_queue`
- `g_dispatch.proto_rb[]`
- `g_dispatch.proto_probe[]`
- `g_dispatch.frame_queue[]`
- `g_dispatch.ch_proto_map[]`
- `g_dispatch.registered_mask`

### 3.2 现有接收与分发链路

从 `Application/Src/app_dispatch.c` 可见，现有接收链路为：

```text
通道任务
  -> app_channel_dispatch()
  -> ring buffer 写入
  -> g_dispatch.ch_queue 通知
  -> frame_dispatch_task()
  -> probe()
  -> frame_queue
  -> 协议处理任务
```

### 3.3 现有协议模块示例

- 青海协议：`Application/Src/ProtocolParser_QingHai/app_qh_proto.c`
- LDI 协议：`Application/Src/LDI/app_ldi.c`

这两个协议模块都已经具备：

- 申请 ring buffer
- 注册 probe
- 绑定通道
- 创建处理任务

因此，A/B 链式解析可以在**协议调度层**实现，而不必重构底层通信栈。

---

## 4. 链式解析的核心定义

### 4.1 基本流程

```text
一帧数据到达
  -> 尝试协议 A
      -> A 成功：结束
      -> A 失败：尝试协议 B
          -> B 成功：结束
          -> B 失败：继续下一协议或丢弃
```

### 4.2 状态语义

协议探测函数必须统一使用以下四种返回值：

| 状态 | 含义 | 对当前协议 | 对后续协议 |
|---|---|---|---|
| `PROTO_PROBE_READY` | 帧完整、属于本协议 | 立即消费，停止链 | 不再尝试 |
| `PROTO_PROBE_WAIT` | 帧头/长度初步匹配，但数据未收完整 | 暂停等待更多数据 | 继续探测（让后续协议也表态） |
| `PROTO_PROBE_FAKE` | 首字节/帧头不匹配，明确不属于本协议 | 放弃本轮 | 继续下一协议 |
| `PROTO_PROBE_SKIP` | 帧结构完整但不属于当前设备（如车道号不匹配） | 跳过整帧，停止链 | 不再尝试 |

**关键语义约束**：

1. **READY 和 SKIP 立即终止链路** — 帧已消费或跳过，不需要再问后续协议。
2. **WAIT 不阻止其他协议表态** — 协议 A 返回 WAIT 后，协议 B 仍然可以被调用。但 A 的 WAIT 意味着"我可能认领这帧，还需要更多数据"，因此 **只要有一个协议返回 WAIT，分发器就不能 skip 字节做重同步**。
3. **FAKE 是唯一的"继续"信号** — 只有所有已注册协议都返回了 FAKE（没有任何一个返回 WAIT 或 READY/SKIP），才说明所有协议都确认"这字节不属于我"，此时可以跳过 1 字节重新同步。
4. **WAIT 与 FAKE 的分离逻辑**：
   - 如果某协议返回 WAIT → 设置 `any_wait = true`，继续探测下一个协议
   - 如果某协议返回 FAKE → 设置 `any_fake = true`，继续探测下一个协议
   - 全部探测完后：若 `any_wait == true` → **退出内循环等待更多数据（不跳过任何字节）**
   - 全部探测完后：若只有 FAKE（无 WAIT/READY/SKIP） → **跳过 1 字节重新同步**

> **注意**：当前 `app_dispatch.c` 中 `frame_dispatch_task()` 的实现存在一个 **WAIT 语义 bug**：代码用 `all_wait` 变量初始化为 `true`，遇到 FAKE 时设为 `false`。如果协议 A 返回 WAIT 而协议 B 返回 FAKE，`all_wait` 被 B 改为 `false`，导致跳过 1 字节，破坏协议 A 的帧同步。必须在实现多协议链式解析**之前**修复此 bug。正确做法是用两个独立变量 `any_wait` 和 `any_fake` 分别追踪。

### 4.3 probe 函数契约（纯函数约束）

探测函数在分发器的持锁上下文中被调用，必须遵守以下契约：

| 约束 | 说明 |
|---|---|
| **只读访问 ring buffer** | probe 只能通过 `rb_peek` / `rb_peekc` 窥视数据，**禁止**调用 `rb_read` / `rb_skip` / `rb_flush` |
| **不修改协议内部状态** | probe 不得修改协议上下文中的任何字段（如序列号、状态机、缓冲区）。帧归属确认后的状态更新应在 READY 消费路径中完成 |
| **不修改 total_len / aux（除非返回 READY/SKIP）** | 返回 WAIT 或 FAKE 时，`*total_len` 的值无意义，分发器不读取 |
| **幂等** | 对同一 RB 状态（相同数据、相同 read_index），多次调用 probe 必须返回相同结果 |
| **无阻塞操作** | probe 中不得调用 `osDelay`、`osMessageQueuePut/Get`、`osMutexAcquire` 等可能阻塞的 API |
| **尽可能快速** | probe 在分发器的持锁临界区内执行，耗时越长阻塞通道写入越久。建议先做快速首字节拒绝，匹配后才做完整校验（CRC 等） |

> **实例**：当前 LDI probe (`app_ldi.c` line 305) 中存在 `g_ldi.rsp_seq = frame->seq` 的副作用写入，违反了纯函数契约。如果 LDI probe 被青海协议帧触发（在链式探测中），LDI 的序列号会被错误覆盖。修复方式：将 `rsp_seq` 的赋值移到 `PROTO_PROBE_READY` 后的帧消费路径中。

---

## 5. 具体类图 / 时序图

### 5.1 类图（逻辑结构图）

```text
+-------------------+          +-------------------------+
|    channel_t      |          |      dispatch_ctx_t     |
|-------------------|          |-------------------------|
| ch_id             |          | ch_queue                |
| state             |          | proto_rb[]              |
| ops --------------+--------->| proto_probe[]           |
+-------------------+          | frame_queue[]           |
                               | ch_proto_map[]          |
                               | registered_mask         |
                               +-----------+-------------+
                                           |
                                           |
                                           v
                               +-------------------------+
                               |   ring_buffer_t         |
                               |-------------------------|
                               | buf                     |
                               | size                    |
                               | mutex                   |
                               +-------------------------+

+-------------------+          +-------------------------+
|  protocol A/B     |          |    frame_msg_t          |
|-------------------|          |-------------------------|
| sw_app_initcall   |          | ch                      |
| init()            |          | data[]                  |
| probe()           |          | data_len                |
| execute()         |          +-------------------------+
| task()            |
+-------------------+
```

### 5.2 时序图（接收路径）

```text
UART/UDP/TCP 通道任务
    -> app_channel_dispatch(ch, data, len)
    -> 写入共享 ring buffer
    -> 唤醒 frame_dispatch_task

frame_dispatch_task
    -> 读取通道对应协议掩码
    -> 按优先级尝试协议 A
        -> A.probe(...)
            -> READY: 读取完整帧，投递 A.frame_queue
            -> WAIT: 保持等待
            -> FAKE: 尝试协议 B
    -> 若 A 失败，再尝试协议 B
        -> B.probe(...)
            -> READY: 读取完整帧，投递 B.frame_queue
            -> WAIT: 保持等待
            -> FAKE: 继续下一个协议

协议处理任务 A/B
    -> osMessageQueueGet(frame_queue)
    -> parse
    -> execute
```

---

## 6. 接口设计稿

本节将 A/B 链式解析从“规划”进一步收敛为“接口设计”。设计目标是：

- 保持现有 `app_dispatch` 框架不大改
- 让协议模块在各自 `xx_proto.init()` 中完成自注册
- 让协议优先级成为一等公民
- 对于相同 priority 的协议，给出明确、可复现的注册解决方案，而不是随机/动态打散

### 6.1 注册接口定义

建议新增注册接口：

```c
proto_mask_t app_proto_register_ex(proto_probe_fn_t probe,
                                   ring_buffer_t *rb,
                                   uint16_t priority,
                                   proto_reg_flags_t flags);
```

#### 参数说明

- `probe`
  - 类型：`proto_probe_fn_t`
  - 说明：协议探测函数指针
  - 要求：返回 `PROTO_PROBE_WAIT` / `PROTO_PROBE_FAKE` / `PROTO_PROBE_READY` / `PROTO_PROBE_SKIP`

- `rb`
  - 类型：`ring_buffer_t *`
  - 说明：协议共享的接收缓冲区
  - 要求：由 `app_proto_acquire_buf()` 返回

- `priority`
  - 类型：`uint16_t`
  - 说明：协议优先级，数值越大优先级越高
  - 取值范围：`0 ~ 65535`
  - 推荐约定：
    - `0`：最低优先级
    - `100`：普通协议
    - `200` 以上：主协议 / 强优先协议

- `flags`
  - 类型：`proto_reg_flags_t`
  - 说明：注册行为标志位
  - 当前建议至少保留：
    - `PROTO_REG_DEFAULT = 0x00`
    - `PROTO_REG_EXCLUSIVE = 0x01`：同优先级下，优先级组内只允许单协议占位；若发生冲突，需显式调整 priority 或注册顺序

### 6.2 priority 的数据类型与排序规则

#### 数据类型

`priority` 建议采用 `uint16_t`，原因如下：

- 足够表达协议优先级分层
- 比 `uint8_t` 更灵活，便于未来扩展
- 比 `uint32_t` 更节省结构体空间

#### 排序规则

排序原则如下：

1. **按 priority 从大到小排序**
2. **priority 相同的协议进入同一组**
3. **同组内按 tie-break 规则决定先后顺序**

### 6.3 同 priority 时的注册解决方案

本方案**不采用随机/动态打散策略**。对于 priority 相同的协议，采用**显式注册顺序 + 唯一注册位**的方式进行解决。

#### 设计原则

- priority 仍然作为第一排序条件，数值越大优先级越高
- priority 相同的协议，不做随机打散
- priority 相同的协议，按注册先后顺序进入稳定排序
- 若同优先级协议必须并存，则应通过显式注册顺序和唯一注册位避免冲突
- 若某个协议组要求“独占优先级段”，则同 priority 的重复注册应视为配置冲突并在初始化阶段告警

#### 推荐实现

在系统启动或协议注册时，为每个协议生成一个递增的 `reg_seq`：

- `reg_seq` 由框架在 `app_proto_register_ex()` 内自动分配
- 排序时先比较 `priority`
- `priority` 相同，再比较 `reg_seq`
- `reg_seq` 越小，注册越早，排序越靠前

这样可以保证：

- priority 高的协议始终优先
- 同优先级协议的顺序可复现、可调试
- 协议模块只需通过 init 顺序控制谁先注册

#### 同优先级冲突处理建议

对于某些需要严格区分的协议组，建议采用以下策略之一：

1. **显式调整 priority**：最推荐，避免同级冲突
2. **固定注册顺序**：由 `xx_proto.init()` 的初始化顺序决定
3. **独占注册位**：若某协议已占据该优先级段，则后注册协议直接返回失败并输出配置错误

#### 不采用的策略

- 不采用启动时随机
- 不采用每帧轮换
- 不采用运行期动态重排

原因是这些策略会增加调试复杂度，不利于协议接管行为的稳定复现。

### 6.4 推荐的注册流程

协议模块在自己的 `xx_proto.init()` 中完成如下流程：

1. `app_proto_acquire_buf()` 获取 ring buffer
2. `app_proto_register_ex()` 注册探测器和 priority
3. `app_proto_bind_channel()` 绑定通道
4. `app_proto_set_frame_queue()` 设置协议队列
5. `osThreadNew()` 创建处理任务

### 6.5 多协议模块共享 ring buffer 的实现规划

A/B 链式解析的核心前提之一，是**多个协议模块共享同一份 ring buffer**，而不是每个协议各自维护一份接收缓存。

#### 设计目标

- 同一物理通道收到的数据，只写入一次
- 多个协议模块可以对同一份 ring buffer 进行 probe
- 避免 A/B 协议各自复制一份数据，节省 RAM
- 允许多个协议绑定同一通道，但只共享一份接收缓存

#### 当前代码基础

结合当前 `app_dispatch.c`，项目已经具备共享 ring buffer 的雏形：

- `app_proto_acquire_buf(id, size)` 会按 `id` 复用同一份静态 ring buffer
- `g_dispatch.proto_rb[]` 保存协议对应的 ring buffer 指针
- `app_channel_dispatch()` 会按通道掩码，把接收到的数据写入匹配的 ring buffer
- `frame_dispatch_task()` 会从 ring buffer 中探测完整帧

因此，实现共享 ring buffer 的关键不是“新增缓存”，而是**让多个协议模块显式绑定同一个 rb_id**。

#### 推荐实现方式

协议模块在各自 `xx_proto.init()` 中调用：

```c
ring_buffer_t *rb = app_proto_acquire_buf(1, 512);
```

其中 `id=1` 表示同一组通用协议共享同一个 ring buffer。注意当前实现中 `size` 参数实际被 `(void)size` 忽略，真正的缓冲区容量由编译期 `RB_DEFINE_CCM` 静态分配决定（当前为 2,048 字节）。这样：

- 青海协议与 LDI 协议如果希望链式兼容，可以绑定同一个 `rb_id`
- `app_channel_dispatch()` 只需要把数据写入一次
- `frame_dispatch_task()` 可在同一份数据上按优先级依次 probe

#### 共享规则

建议约束如下：

- **同一通道、同一兼容组的协议共享同一 `rb_id`**
- 共享同一 `rb_id` 的协议应保证 probe 逻辑严格、不会误吞别人的帧
- 如果两个协议格式差异很大，可分配不同 `rb_id`，避免无效探测开销
- 共享 ring buffer 仅共享“接收数据”，不共享“协议队列”与“处理任务”

#### 内存收益

共享 ring buffer 的直接收益：

- 避免同一帧被复制到多份缓存
- 降低 CCMRAM/SRAM 压力
- 减少缓存一致性和调试复杂度

#### 实施注意事项

- 共享 ring buffer 不能替代协议队列：每个协议的 `frame_queue` 仍应独立
- 共享 ring buffer 不能让多个协议同时“消费”同一帧，最终消费权仍由优先级决定
- 如果共享组中某协议的 probe 太宽松，会影响其他协议的命中率，因此必须严格定义 `WAIT/FAKE/READY`

### 6.8 `app_proto_register_ex()` 的接口草案

为了便于后续编码，这里给出建议接口草案：

```c
/**
 * @brief 注册协议探测器，并指定优先级与注册属性。
 * @param probe     协议探测函数
 * @param rb        协议共享 ring buffer
 * @param priority  协议优先级，数值越大越优先
 * @param flags     注册标志位
 * @return          协议掩码；0 表示注册失败
 */
proto_mask_t app_proto_register_ex(proto_probe_fn_t probe,
                                   ring_buffer_t *rb,
                                   uint16_t priority,
                                   proto_reg_flags_t flags);
```

#### 参数约束建议

- `probe` 不能为空
- `rb` 不能为空
- `priority` 不能越界（`uint16_t` 自然约束）
- `flags` 用于描述注册策略，不参与协议内容解析

#### 建议的内部排序字段

框架内部建议为每个注册协议维护以下信息：

- `priority`：主排序字段
- `reg_seq`：注册序号，保证同优先级时排序稳定
- `flags`：注册行为控制位
- `rb`：共享接收缓存指针
- `probe`：探测函数指针

### 6.9 `app_dispatch` 相关数据结构设计稿

结合当前 `Application/Src/app_dispatch.c` 的实现，若要支持多协议链式解析与共享 ring buffer，建议在 `dispatch_ctx_t` 基础上补充一层“协议注册元数据”，将**协议注册信息**与**运行时分发信息**分离。

#### 6.9.1 设计目标

- 让协议注册信息可排序、可复现、可调试
- 让共享 ring buffer 的分组信息显式化
- 让 `frame_dispatch_task()` 可以按“优先级 + 注册顺序”稳定遍历协议
- 避免把排序逻辑散落到各个协议模块中

#### 6.9.2 建议新增的注册信息结构体

```c
typedef struct {
    proto_mask_t      mask;        /* 协议掩码 */
    proto_probe_fn_t  probe;       /* 探测函数 */
    ring_buffer_t    *rb;          /* 共享 ring buffer */
    osMessageQueueId_t frame_queue;/* 协议帧队列 */
    channel_mask_t    ch_mask;     /* 绑定的通道集合 */
    uint16_t          priority;    /* 协议优先级 */
    uint16_t          reg_seq;     /* 注册序号，用于稳定排序 */
    uint16_t          rb_id;       /* 共享 ring buffer 分组编号 */
    uint8_t           flags;       /* 注册标志位 */
    uint8_t           state;       /* 注册状态：空闲/已注册/已失效 */
} proto_reg_info_t;
```

#### 字段说明

- `mask`
  - 协议的唯一掩码，由框架分配
- `probe`
  - 协议探测函数
- `rb`
  - 当前协议绑定的共享 ring buffer
- `frame_queue`
  - 当前协议的帧队列
- `ch_mask`
  - 当前协议绑定的通道集合
- `priority`
  - 主排序字段，越大越先尝试
- `reg_seq`
  - 注册顺序序号，用于同优先级时稳定排序
- `rb_id`
  - 共享 ring buffer 分组号，用于描述哪些协议共用同一份接收缓存
- `flags`
  - 注册行为控制位，例如是否独占优先级段
- `state`
  - 注册元数据状态，便于调试和动态失效处理

#### 6.9.3 建议的派生排序视图

为了避免每次分发都临时排序，建议在 `dispatch_ctx_t` 内维护一个**排序后的协议索引视图**：

```c
typedef struct {
    uint8_t   order[PROTO_MAX_COUNT]; /* 排序后的协议索引 */
    uint8_t   count;                  /* 当前有效协议数量 */
    bool      dirty;                  /* 注册变更后置脏，需要重建排序视图 */
} proto_order_view_t;
```

用途：

- `app_proto_register_ex()` / `app_proto_bind_channel()` 更新后把 `dirty` 置位
- `frame_dispatch_task()` 在发现 `dirty` 时重建排序视图
- 重建后按照 `priority -> reg_seq` 固定排序

#### 6.9.4 建议扩展 `dispatch_ctx_t`

在当前 `g_dispatch` 的基础上，建议增加以下字段：

```c
typedef struct {
    ring_buffer_t    *buf_pool[RB_CNT_MAX];
    proto_reg_info_t  proto_info[PROTO_MAX_COUNT];
    proto_order_view_t order_view;
    uint32_t          registered_mask;
    uint32_t          ch_proto_map[MAX_CHANNELS];
    osMessageQueueId_t frame_queue[PROTO_MAX_COUNT];
    ring_buffer_t     *proto_rb[PROTO_MAX_COUNT];
    proto_probe_fn_t   proto_probe[PROTO_MAX_COUNT];
    osMessageQueueId_t ch_queue;
} dispatch_ctx_t;
```

说明：

- `proto_info[]` 是协议元数据主表
- `order_view` 是调度阶段读取的排序快照
- `proto_rb[]` / `proto_probe[]` / `frame_queue[]` 可以继续保留，作为对外兼容的快捷索引

#### 6.9.5 共享 ring buffer 分组表

如果需要更清晰地表达“哪些协议共享同一份 ring buffer”，建议增加分组表：

```c
typedef struct {
    uint16_t rb_id;                       /* 分组编号 */
    ring_buffer_t *rb;                    /* 共享 ring buffer */
    uint32_t proto_mask;                  /* 该组内协议掩码集合 */
    uint16_t member_count;                /* 成员数 */
} rb_group_info_t;
```

该结构体的作用是：

- 让 `app_dispatch` 很容易知道哪些协议共享同一份接收缓存
- 帮助 `frame_dispatch_task()` 做“按 rb 分组去重探测”
- 方便调试时打印“当前组内成员”

#### 6.9.6 推荐的排序规则实现

排序建议使用以下比较顺序：

1. `priority` 降序
2. `rb_id` 固定组内优先（可选）
3. `reg_seq` 升序
4. `mask` 升序作为最后稳定项

这样可以保证：

- 高优先级协议先尝试
- 同优先级协议排序稳定
- 共享同一 rb 的协议能在组内按固定规则去重

---

## 7. `app_dispatch` 的伪代码

结合当前 `Application/Src/app_dispatch.c` 的实现，链式解析可以抽象成如下伪代码：

```c
void frame_dispatch_task(void *argument)
{
    while (1) {
        ch = wait_channel_notification();
        if (!ch) continue;

        proto_mask = g_dispatch.ch_proto_map[ch->ch_id];

        for each rb in protocol_ring_buffers_covered_by_this_channel:
            lock(rb);

            while (rb_has_data(rb)) {
                parsed   = false;
                any_wait = false;  // 是否有协议因数据不足而等待
                any_fake = false;  // 是否有协议明确说"不是我的帧"

                for each protocol in priority_order:
                    if protocol not bound to this channel:
                        continue;
                    if protocol does not use this rb:
                        continue;

                    state = protocol.probe(ch, rb, &frame_len, &aux);

                    if (state == READY) {
                        // 确认归属，读取完整帧 → 投递队列 → 立即停止链
                        if (rb_available >= frame_len) {
                            read full frame from rb;
                            push frame to protocol.frame_queue;
                            parsed = true;
                            break;  // 退出协议遍历，回到 while 继续下一帧
                        }
                        // frame_len > avail 时退化为 WAIT
                        any_wait = true;

                    } else if (state == SKIP) {
                        // 帧结构完整但不属于当前设备，跳过整帧 → 停止链
                        if (rb_available >= frame_len) {
                            skip full frame bytes;
                            parsed = true;
                            break;
                        }
                        // 数据不足退化为 WAIT
                        any_wait = true;

                    } else if (state == WAIT) {
                        // 帧头初步匹配但数据不足，标记等待，继续探测下一个协议
                        any_wait = true;

                    } else if (state == FAKE) {
                        // 明确不属于本协议，继续探测下一个协议
                        any_fake = true;
                    }

                // 协议遍历结束后的处理
                if (!parsed) {
                    if (any_wait)
                        break;      // 至少一个协议在等待 → 退出内循环等更多数据
                    else
                        skip 1 byte; // 全部返回 FAKE → 伪帧头，跳过 1 字节重同步
                }
            }

            unlock(rb);
        }
    }
}
```

### 7.1 伪代码与当前实现的对应关系

当前 `app_dispatch.c` 中已经具备如下结构：

- `g_dispatch.registered_mask`
- `g_dispatch.proto_rb[]`
- `g_dispatch.proto_probe[]`
- `g_dispatch.frame_queue[]`
- `rb_lock()` / `rb_unlock()`
- `rb_avail()` / `rb_read()` / `rb_skip()`

因此，只需要在分发循环里把“单协议探测”升级成“按优先级链式探测”即可。

---

## 8. A/B 两个协议的注册示例

下面示例以“青海协议 A、LDI 协议 B”为例说明。示例仅体现注册方式，不改动原有架构。

### 8.1 协议 A：青海协议注册示例

文件：

```text
Application/Src/ProtocolParser_QingHai/app_qh_proto.c
```

示例注册流程：

```c
void qh_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(1, 512);
    s_qh_mask = app_proto_register(qh_proto_probe_frame, rb);
    if (s_qh_mask == 0)
        return;

    app_proto_bind_channel(s_qh_mask, CH_ID_RS485);

    s_qh_queue = osMessageQueueNew(2, sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN, &s_qh_queue_attr);
    app_proto_set_frame_queue(s_qh_mask, s_qh_queue);

    osThreadNew(qh_proto_handle_task, NULL, &s_qh_task_attr);
}
```

### 8.2 协议 B：LDI 注册示例

文件：

```text
Application/Src/LDI/app_ldi.c
```

示例注册流程：

```c
[[maybe_unused]] static void ldi_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(1, 512);
    s_ldi_mask = app_proto_register(ldi_probe_frame, rb);
    if (s_ldi_mask == 0)
        return;

    // 注意：当前 LDI 实际绑定 TCP/UDP，不绑定 RS485
    // 若要做 RS485 链式兼容测试，需额外解除此行的注释：
    // app_proto_bind_channel(s_ldi_mask, CH_ID_RS485);
    app_proto_bind_channel(s_ldi_mask, CH_ID_TCP_SERVER);
    app_proto_bind_channel(s_ldi_mask, CH_ID_TCP_CLIENT);
    app_proto_bind_channel(s_ldi_mask, CH_ID_UDP);

    ldi_ctx_init(&g_ldi);
    ldi_device_init();

    s_ldi_queue = osMessageQueueNew(...);
    app_proto_set_frame_queue(s_ldi_mask, s_ldi_queue);

    osThreadNew(ldi_handle_task, NULL, &ldi_task_attr);
    osThreadNew(ldi_timer_task, NULL, &ldi_timer_task_attr);
}
sw_app_initcall(ldi_module_init);
```

### 8.3 A/B 并行时的注册约束

如果要做链式解析，建议协议注册遵循：

- 同一通道可以绑定多个协议
- 协议分发顺序由 priority 决定
- probe 函数必须严格区分 `WAIT` / `FAKE` / `READY`
- 一帧只允许一个协议最终消费

### 8.4 多协议共通道的风险约束

结合现有工程实现，多个协议共同绑定同一个 `RS485/RS232` 通道时，必须额外控制以下风险：

- **协议抢帧**：多个协议同时认为一帧属于自己，必须依赖 priority 和单消费机制避免重复执行。
- **WAIT/FAKE 误判**：半包不能被后续协议抢走；不属于本协议的帧必须明确返回 FAKE，而不是 WAIT。
- **重复接收与缓存膨胀**：同一通道的数据不能为每个协议各自复制一份，应通过共享 ring buffer 只写一次。
- **优先级偏置**：若没有稳定排序规则，某个协议会长期压制其它协议，导致兼容协议形同虚设。
- **调试复杂度增加**：同一帧可能在多个协议 probe 之间切换，需要保留清晰的日志、断点和统计信息。
- **半双工发送冲突**：尤其在 RS485 上，多个协议同时回复可能引起方向控制或总线冲突，因此发送路径仍应统一走通道层。
- **误命中问题**：格式相似的协议必须严格校验帧头、长度、帧尾与命令合法性，避免宽松探测导致抢帧。
- **错误恢复困难**：若帧错位，需要明确是跳过 1 字节还是整帧恢复同步，否则可能影响后续连续帧。

因此，多协议共通道模式必须建立在“共享缓存 + 优先级仲裁 + 单协议消费 + 严格探测”四个前提之上。

---

## 9. RAM 约束分析

STM32F407 项目内存资源：

- **SRAM**：128KB（`0x20000000`），其中 FreeRTOS heap = 32KB (`heap_4.c`)
- **CCMRAM**：64KB（`0x10000000`），零等待状态

当前已分配的关键内存（单协议模式，以 LDI 为例）：

### 9.1 单协议 vs 双协议 RAM 对比

| 组件 | 单协议 (LDI) | 双协议 (LDI + QingHai) | 增量 | 位置 |
|---|---|---|---|---|
| `frame_dispatch_task` 栈 | 1,024 B | 1,024 B | 0 | SRAM heap |
| `_msg_dispatch_buf` (帧缓冲) | 1,052 B | 1,052 B | 0 | SRAM .bss |
| LDI 协议任务栈 (×2) | 4,096 B | 4,096 B | 0 | SRAM heap |
| 第二个协议任务栈 (×1) | — | 2,048 B | +2,048 B | SRAM heap |
| LDI 消息队列 (静态) | 1,040 B | 1,040 B | 0 | SRAM .bss |
| 第二个协议消息队列 | — | ~2,200 B | +2,200 B | SRAM heap (动态) |
| 协议上下文 (`g_ldi` 等) | ~700 B | ~700 B | 0 | SRAM .bss |
| 第二个协议 `msg_buf` | — | 1,052 B | +1,052 B | SRAM .bss |
| 环形缓冲区池 (4×2,048) | 8,192 B | 8,192 B | 0 | CCMRAM |
| `g_dispatch` 上下文 | ~400 B | ~400 B | 0 | SRAM .bss |
| **SRAM 合计** | **~8,300 B** | **~13,600 B** | **+5,300 B** | /128KB = 10.6% |
| **FreeRTOS heap 占用** | **~5,100 B** | **~9,350 B** | **+4,250 B** | /32KB = 29.2% |

> **结论**：双协议模式 SRAM 总占用约 13.6KB（占总 SRAM 的 10.6%），FreeRTOS heap 占用约 29.2%。RAM 完全在预算内，即使再增加第三个协议也绰绰有余。

### 9.2 优化方向

- 第二个协议的消息队列如果改为静态分配（像 LDI 一样使用 `StaticQueue_t` + `osMessageQueueNew` 的 `cb_mem`/`mq_mem`），可节省约 2KB heap，将 heap 占用降至 ~23%
- 环形缓冲区池（4×2KB=8KB CCMRAM）由所有协议共享，不随协议数量增加

### 9.3 多协议共享通道时的内存原则

- **接收缓存共享**：同一兼容组使用同一 `rb_id`，避免重复分配 ring buffer
- **处理任务独立**：每个协议保留自己的 `frame_queue` 与处理任务，避免业务逻辑互相干扰
- **协议初始化轻量化**：初始化阶段仅做注册、绑定、队列创建与任务创建
- **共享通道但不共享执行上下文**：共享的是输入字节流，不共享协议状态机、业务上下文或输出动作

---

## 10. 任务调度考虑

### 10.1 推荐调度模型

```text
通道任务
  -> 写入环形缓冲区
  -> 唤醒 frame_dispatch_task
  -> frame_dispatch_task 按优先级尝试协议 A/B
  -> 成功协议进入各自执行路径
```

### 10.2 调度特点

- 协议尝试在一个任务中串行完成，避免竞态
- A 失败后 B 继续尝试，逻辑清晰
- 适合帧长度有限、协议探测成本较低的场景

### 10.3 风险

- 若 probe 过重，可能增加单帧分发延迟
- 若 A 的判定过宽，可能抢走 B 的帧
- 若 `WAIT` / `FAKE` 语义混乱，容易误解析

---

## 11. 推荐的落地步骤

### 核心多协议启用机制

**不引入条件编译、不修改 Makefile、不增加 `PROTO=BOTH` 构建目标。**

多协议兼容的启用完全依赖 EIDE 工程的文件夹包含/排除：

- **单协议模式**（现状）：EIDE 中只保留一个协议文件夹（如 `Application/Src/LDI`），排除其他协议文件夹。只有一个协议的 `initcall` 编译进固件，行为与现在完全一致。
- **多协议模式**（目标）：EIDE 中同时保留两个或多个协议文件夹（如 `Application/Src/LDI` + `Application/Src/ProtocolParser_QingHai`），排除其他不需要的。所有被包含的协议模块的 `sw_app_initcall` 都会在启动时执行，各自向 `app_dispatch` 自注册。`frame_dispatch_task` 自动按优先级链式探测所有已注册协议。

**工作机制**：
1. EIDE 工程中包含了 N 个协议文件夹 → N 个 `sw_app_initcall(proto_init)` 被编译
2. RTOS 启动后，`init_task` → `sw_board_init()` 按 `sw_app_initcall(3)` 层依次调用所有协议 init
3. 每个协议的 init 调用 `app_proto_register_ex(probe, rb, priority, flags)` 注册自身
4. 框架自动按 priority 排序，构建探测顺序
5. `frame_dispatch_task` 按排序后的顺序链式探测
6. 无需任何条件编译宏、无需 Makefile 改动、无需 `protocol_select.h` 参与决策

---

### Phase 0：修复 WAIT 语义 bug（必须最先做）

> **优先级: CRITICAL** — 不改此 bug，后续所有多协议工作都建立在错误基础上

**范围**：`app_dispatch.c` 的 `frame_dispatch_task()` 内部循环

**当前问题**：`all_wait` 变量初始化为 `true`，被任何协议的 FAKE 改为 `false`。若协议 A 返回 WAIT、协议 B 返回 FAKE，`all_wait = false`，导致跳过 1 字节 → 破坏协议 A 的帧同步。

**修复思路**：
- 引入两个独立变量 `any_wait` 和 `any_fake` 替代单一的 `all_wait`
- WAIT 设置 `any_wait = true`，FAKE 设置 `any_fake = true`
- 全部协议探测完后：若 `any_wait == true` → 退出内循环等待更多数据；若只有 `any_fake` → 跳过 1 字节重同步
- 详细伪代码见 Section 7

**验证方式**：
- 模拟场景：协议 A 对半包数据返回 WAIT，协议 B 对同一数据返回 FAKE → `frame_dispatch_task` 必须退出等待，不能跳过字节
- 模拟场景：两个协议都返回 FAKE → 必须跳过 1 字节

---

### Phase 1：优先级基础设施

> **优先级: HIGH** — 多协议链式解析的排序基础

**新增数据**：

在 `dispatch_ctx_t` 中增加（思路级）：
- 每个协议槽位的 `priority`（`uint16_t`，数值越大越优先）
- 每个协议槽位的 `reg_seq`（`uint16_t`，注册序号，同优先级时稳定排序）
- 排序后的索引视图 `sorted_order[]`（`uint8_t` 数组，存放按 priority 降序排列的协议索引）
- 一个 `dirty` 标志位，注册变更时置位，分发循环中重建排序视图

**新增接口**（思路级）：
- `app_proto_register_ex(probe, rb, priority, flags)` — 带优先级和标志位的注册函数
  - `priority`: `uint16_t`，推荐约定：`200` 以上 = 主协议/强优先，`100` = 普通协议，`0` = 最低优先级
  - `flags`: 注册行为标志位，第一期至少保留 `PROTO_REG_DEFAULT = 0x00`
  - 内部自动分配 `mask` 和 `reg_seq`，设置 `dirty = true`
- 保留旧的 `app_proto_register(probe, rb)` 作为兼容接口（内部调用 `register_ex`，priority=0）

**排序规则**（在 `frame_dispatch_task` 中或注册变更时触发重建）：
1. 按 `priority` 降序
2. 相同 `priority` 按 `reg_seq` 升序（先注册的排前面）
3. 排序在 `dirty == true` 时触发重建，重建完成后 `dirty = false`

**修改范围**：
- `app_dispatch.h`：新增字段到 `dispatch_ctx_t`，新增 `app_proto_register_ex` 声明
- `app_dispatch.c`：新增 `register_ex` 实现，`frame_dispatch_task` 内循环改用 `sorted_order[]` 遍历

---

### Phase 2：修复 probe 函数副作用 + 建立契约

> **优先级: HIGH** — 确保链式探测时各协议 probe 互不污染

**范围**：`app_ldi.c` 的 `ldi_probe_frame()`、`app_qh_proto.c` 的 `qh_proto_probe_frame()`

**问题**：LDI probe 中存在 `g_ldi.rsp_seq = frame->seq` 的副作用写入。在链式探测中，若 LDI probe 被青海协议的帧触发（数据碰巧以 `0xFF 0xFF` 开头），LDI 的 `rsp_seq` 会被错误覆盖。

**修复思路**：
- 将 `g_ldi.rsp_seq = frame->seq` 从 probe 函数移到帧消费路径中（`ldi_handle_task` 解析完整帧后）
- 或者在 READY 返回前才写入（此时已确认帧归属 LDI）

**probe 函数契约**（写入文档和代码注释）：
- 只读访问 ring buffer（`rb_peek` / `rb_peekc`）
- 不修改协议内部状态
- 不阻塞（无 `osDelay`、无队列操作）
- 幂等（相同 RB 状态 → 相同返回值）
- 尽可能快速（先做 1 字节快速拒绝，匹配后才做完整校验）

**验证方式**：
- 代码审查确认 probe 中无状态写入
- 在 `qh_proto_probe_frame` 的 READY 路径打断点，确认 LDI 的 `rsp_seq` 未被修改

---

### Phase 3：协议模块适配新注册接口

> **优先级: MEDIUM** — 让两个协议模块使用新的优先级注册

**范围**：`app_qh_proto.c`、`app_ldi.c`

**修改思路**：
- `qh_proto_init()`：将 `app_proto_register` 替换为 `app_proto_register_ex(qh_proto_probe_frame, rb, 200, PROTO_REG_DEFAULT)`
  - 青海协议设为较高优先级（200），因为其帧格式包含明确帧头帧尾，探测更可靠
- `ldi_module_init()`：将 `app_proto_register` 替换为 `app_proto_register_ex(ldi_probe_frame, rb, 100, PROTO_REG_DEFAULT)`
  - LDI 设为较低优先级（100），由青海协议优先尝试
- 青海协议消息队列改为静态分配（参照 LDI 的 `StaticQueue_t` 模式），节省约 2KB heap

**注意**：此阶段不要求两个协议绑定同一通道。青海协议仍然只绑定 `CH_ID_RS485`，LDI 绑定 TCP/UDP。通道共享在 Phase 4 验证。

---

### Phase 4：EIDE 多文件夹 + 通道共享测试

> **优先级: MEDIUM** — 验证多协议自动注册和链式解析端到端工作

**EIDE 工程配置**：
1. 在 EIDE 中同时保留 `Application/Src/LDI` 和 `Application/Src/ProtocolParser_QingHai` 两个文件夹
2. 两个协议的 `sw_app_initcall` 同时编译进固件
3. 无需修改任何宏定义、Makefile 或条件编译

**预期行为**：
- 固件启动后，`sw_board_init()` 依次调用 `app_dispatch_init` → `ldi_module_init` → `qh_proto_init`
- 两个协议各自注册，框架自动按优先级排序
- LDI 在 TCP/UDP 通道上正常工作
- 青海协议在 RS485 通道上正常工作
- 互不干扰

**通道共享验证**：
- 临时将 LDI 也绑定到 `CH_ID_RS485`（仅用于测试）
- 从 RS485 发送混合帧：先发青海协议帧，再发 LDI 帧
- 验证：
  - 青海帧被青海协议优先消费（LDI 返回 FAKE）
  - LDI 帧被 LDI 协议消费（青海返回 FAKE 后 LDI 接管）
  - 半包场景：发送不完整的青海帧 → 青海返回 WAIT → 分发器不跳过字节
  - 全部 FAKE 场景：发送随机数据 → 逐字节跳过重同步

---

### Phase 5：加固与优化

> **优先级: LOW-MEDIUM** — 生产就绪前的打磨

| 项目 | 思路 |
|---|---|
| **per-channel send mutex** | 在 `channel_send()` 或通道层增加发送互斥锁，防止两个协议同时通过同一 RS485 通道回复导致总线冲突 |
| **probe 快速拒绝优化** | LDI probe 当前在每次调用时都执行 `memset(mem_pool, 0, 512)` + `CRC16`。优化：先 `rb_peekc(rb, 0)` 检查首字节是否为 `0xFF`，不匹配则立即返回 FAKE，跳过昂贵的 CRC 计算 |
| **协议冲突检测** | 注册时检测是否有两个协议声明了相同的 `priority` + `PROTO_REG_EXCLUSIVE` 标志，在初始化阶段输出告警日志 |
| **调试/统计信息** | 可选：记录每个协议的命中次数、WAIT 次数、FAKE 次数，方便排查协议帧冲突问题 |
| **probe 超时保护** | 可选：如果某个协议的 probe 函数执行时间异常长（如 > 500µs），通过 DWT 计时器检测并告警 |

---

### Phase 6：（远期）三协议及以上扩展

当前架构（32 槽位、4 个 RB 池）在设计上就已支持 2 个以上协议。新增第三种协议时：
1. 在 EIDE 中添加新协议文件夹
2. 协议模块实现 `sw_app_initcall` 自注册
3. 指定合适的 `priority`（避免与现有协议冲突）
4. 框架自动纳入链式探测

无需修改 `app_dispatch` 框架代码。

---

## 12. 适用与不适用场景

### 12.1 适用场景

- 青海协议与 LDI 协议共存
- 旧设备兼容新协议
- 调试期需要同时支持多种上位机帧格式
- 一个通道可能接收到不同来源、不同版本协议的数据

### 12.2 不适用场景

如果系统只会长期固定一种协议，且永远不会共存，那么 A/B 链式解析会略显复杂。

在这种情况下，直接用工程排除方式切换协议，维护成本更低。

---

## 13. 总结

A/B 协议链式解析是可行的，而且与 STD 项目现有的分层架构高度兼容。经过对实际代码的深入审计，关键发现如下：

**有利基础**（已在生产代码中存在）：
- 共享 ring buffer 机制已实装（两个协议都使用 `rb_id=1`，`app_channel_dispatch` 有 `seen[]` 去重）
- `proto_probe_sta_t` 四状态枚举 (READY/WAIT/FAKE/SKIP) 已正确定义并在所有 probe 中使用
- 协议自注册 (`sw_app_initcall` + `app_proto_register` + `app_proto_bind_channel`) 已成熟
- 多协议绑定同一通道的 `|=` OR 累积机制已完备

**需要在落地前修复**：
1. **WAIT 语义 bug**（Phase 0）：`frame_dispatch_task` 的内部循环用单一 `all_wait` 变量无法正确处理”协议 A=WAIT + 协议 B=FAKE”的场景
2. **LDI probe 副作用**（Phase 2）：`g_ldi.rsp_seq` 在 probe 中被赋值，违反纯函数契约

**多协议启用机制**（核心设计决策）：
- **不使用条件编译、Makefile 改动或 `PROTO=BOTH` 构建目标**
- 完全依赖 EIDE 工程中协议文件夹的包含/排除来决定哪些协议参与编译
- 包含 N 个文件夹 → N 个协议自动注册 → 框架按优先级链式探测
- 与当前单协议模式 100% 兼容：只保留一个文件夹时行为完全不变

**推荐的最终形态**：
- 多个协议共享一个公共接收缓存（同一 `rb_id`）
- 一个帧分发任务按优先级链式调用协议 probe
- WAIT 继续等待（不阻止其他协议表态），FAKE 交给下一个协议，READY 立即接管
- 一帧只允许一个协议最终消费
- 优先级 + 注册序号确保探测顺序稳定、可复现、可调试

从 RAM 与任务调度角度看，这种模式比”每个协议一个独立解析链路”更适合 STM32F407 这类资源有限平台。双协议模式增加约 5.3KB SRAM 占用（占总 SRAM 的 10.6%），FreeRTOS heap 占用约 29.2%，完全在预算内。
