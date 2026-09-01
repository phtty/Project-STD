# 05 — 多协议共享 RB（已落地）

> **范围**：一物理通道一环缓、多协议链式解析、协议绑定与兼容门禁、协议帧 queue 深度。  
> **状态**：功能 **已实现**（2026-08-14 对齐代码；含 RB/queue 瘦身修订）。  
> **非本目录**：SRAM/CCM 分区、堆 → [`../06_SRAM内部分数据迁移/`](../06_SRAM内部分数据迁移/README.md)；LDI 依赖 IAP 配置存储 → [`../07_LDI与IAP配置解耦/`](../07_LDI与IAP配置解耦/README.md)。

---

## 文档结构

| 文件 | 角色 |
|------|------|
| [01_architecture.md](./01_architecture.md) | **设计规范**（现行） |
| [02_as_built_status.md](./02_as_built_status.md) | **落地对照**（代码事实 + DoD + queue 深度） |
| [03_freertos_heap_side_effect.md](./03_freertos_heap_side_effect.md) | **副作用归档**：多协议任务挤 FreeRTOS 堆（结构治理在 06，已回退） |

## 与 06 / 07 的分工

| | 05 多协议共享 RB | 06 RAM | 07 LDI↔IAP 配置 |
|--|------------------|--------|-----------------|
| 主题 | 谁写哪个 RB、链式 probe、**帧 queue 深度** | 堆/显存/RB **放哪块物理 RAM**、占用账 | 选编 LDI 时的配置存储依赖 |
| 权威 | 本目录（协议行为） | 内存分区 **唯一权威** | 配置解耦（**已落地**，`app_board_net_cfg`） |
| 代码状态 | **已合入** | Phase A + 后续瘦身 **已合入** | **已落地**；历史选编规则见 07/`03` |

旧版「8 槽 `g_rb0…7` @ CCM、IAP 双槽 RS485+UDP、LDI 绑 485」已废止；若与本目录冲突，以 `01`/`02` 现行稿为准。

## 快速结论

- 三槽：**`RJ45=1536` / `RS485=768` / `RS232=768`**，体在 SRAM，`RB_PROVIDE_WEAK` 按需提供。
- 帧 queue（静态，不占 `ucHeap`）：IAP **2** / LDI **4** / 青海 **3** / RLS **2** / AH_MQTT **3**（RLS/AH 为源码口径；现行 EIDE Debug 未编入，仅 Makefile 全编）。
- 同物理口多协议：包级写入去重 + 持锁链式 probe。
- IAP 仅 UDP；LDI 仅 TCP/UDP（RJ45）；RS232_1 语音旁路禁 bind。
- 目录选编：排除协议目录可裁解析器；现行 **EIDE Debug 已排除 RLS/AH_MQTT**（仅编 IAP+LDI+青海），**Makefile 全量编入**；配置层已抽至 `Application/Config/app_board_net_cfg.c`，**排除整个 IAP/ 目录不影响 LDI 链接**（见 07）。

阅读顺序：`01` → `02`；堆/CCM → `06`；LDI 选编 → `07`。
