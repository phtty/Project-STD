# doc/08 协议模块接入规则、指导

> **状态**：现行　|　2026-08-17 新建
> **角色**：新协议接入本固件的**操作性权威指引**（规则、骨架、串口/网络专项、检查清单）
> **上游权威**：RB 行为/绑定矩阵/兼容矩阵 → `doc/05`；内存占用账 → `doc/06`

---

## 1. 目的

本栏目回答「**我要给这台费显接一个新协议，从哪开始、每一步改什么、怎么验证**」。
所有内容均以本仓库现有代码为事实依据（范例：青海 / 四川 ETC / 四川 MTC / 四川治超 / 山东 / 贵州 / RLS / IAP / LDI），
不是通用嵌入式理论。文档中的宏名、API 签名、行号均为写实引用。

## 2. 适用读者

- 为本固件编写地区费显协议（RS485/RS232）或网络协议（UDP/TCP）的开发者
- 评审新协议模块接入 PR 的维护者
- 需要核对「现有模块为什么这样写」的任何人

## 3. 文档结构

| 文件 | 内容 |
|---|---|
| `01_通用规则与模块骨架.md` | 目录/命名/文件拆分、注册五步、probe PURE 契约、帧队列、执行层、内存纪律、文档义务 |
| `02_串口协议接入.md` | RS485/RS232 通道与 RB、波特率（DIP1）、接收路径、同槽共存判别、三种变长定界模式、调试定位 |
| `03_网络协议接入_UDP与TCP.md` | RJ45 共享 RB、UDP 范例（IAP/LDI 搜索）、TCP 范例（LDI 双通道）、LwIP 约束、串口/网络混合规则 |
| `04_接入检查清单.md` | 从建目录到实机联调的 step-by-step checklist，含测试帧样例 |

## 4. 快速规则摘要（先记住这 10 条）

1. **目录即模块**：`Application/Src|Inc/ProtocolParser_XXX/`，前缀 `app_xxx_`，静态函数 `_` 前缀。
2. **注册五步**：`RB_PROVIDE_WEAK` → `app_proto_acquire_buf` → `app_proto_register`（返回 `proto_mask_t`）→ `app_proto_bind_channel` → `app_proto_set_frame_queue`，再 `sw_app_initcall` + 建任务（栈 `256*4`）。
3. **probe 是纯函数**：只 `rb_peek`，无副作用；`avail==0` 必须返回 `PROTO_PROBE_FAKE`；首字节不匹配立即 FAKE（快拒）。
4. **仅 READY/SKIP 写 `*total_len`**；长度必须有上下界、尾字节必须校验。
5. **链式聚合**：同 RB 上 WAIT/FAKE 继续试下一协议；一轮无一消费时 `any_wait` 停等、否则跳 1 字节重同步（`app_dispatch.c:328-336`）。
6. **帧队列静态分配**（`StaticQueue_t`），深度惯例 2~4；payload 上限必须 ≤ 对应 RB 容量并用 `_Static_assert` 锁死。
7. **一个物理口一个 RB**：RJ45 1536 / RS485 768 / RS232 768；网口逻辑通道共享 RJ45 槽。
8. **`CH_ID_RS232_1`（USART6）语音 TX 专用，禁止任何协议 bind**。
9. **内存纪律**：静态队列不占 ucHeap；任务栈（256×4=1KB/个）从 ucHeap 支出；新增静态内存必须同步 `doc/06-04` 占用账。
10. **文档义务**：新帧头登记 `doc/05-01 §3.3`、冲突组合登记 §6 兼容矩阵、queue 深度登记 §4.1；代码注释不得引用外部参考工程。

## 5. 与 doc/05、doc/06 的分工

| 主题 | 权威在哪里 | 本栏目的角色 |
|---|---|---|
| RB 容量、槽位、绑定矩阵、weak 合并语义 | `doc/05_协议模块多协议兼容优化/01_architecture.md`（RB 行为权威） | 引用，不复制全量；只写「接入时怎么用它」 |
| 帧头事实表（§3.3）、兼容矩阵（§6）、queue 深度表（§4.1） | doc/05-01 | 新协议接入后**必须回来更新**，本栏目只指路 |
| SRAM/CCMRAM 占用账 | `doc/06_SRAM内部分数据迁移/04_current_memory_occupancy.md`（唯一占用实测账本） | 接入新增内存后**必须回来更新** |
| 接入操作步骤、probe 编写约束、构建收录、调试定位 | **本栏目（doc/08）** | 唯一权威 |

原则：**doc/05、doc/06 记「是什么/现状」，doc/08 记「怎么做」**。三者都改时，先改 08 的步骤，再按步骤更新 05/06 的事实表。

## 6. 关键代码入口速查

| 事实 | 位置 |
|---|---|
| 调度框架全部类型与 API | `Application/Inc/app_dispatch.h`（RB 尺寸 :37-39；通道 ID :47-55；probe 状态 :65-70；注册 API :118-124） |
| 链式探测聚合引擎 | `Application/Src/app_dispatch.c`（`frame_dispatch_task` :226；聚合决策 :328-336；`app_channel_dispatch` :376） |
| RB 宏与结构 | `Kernel/Inc/ring_buffer.h`（`RB_PROVIDE_WEAK` :40；`rb_init` :58） |
| 模块模板权威 | `doc/05_协议模块多协议兼容优化/01_architecture.md §5`（:168） |