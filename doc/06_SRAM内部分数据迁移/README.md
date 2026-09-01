# 06 — RAM 规划与迁移

> **范围**：SRAM / CCM 分区宪法、占用实测、堆回退与大屏容纳、后续 SRAM 瘦身。  
> **状态**：规划 **现行**；**Phase A 已执行**（堆 36KB @ SRAM + RTT 4→2KB）；**后续瘦身已执行**（RB/DMA/queue，2026-08-14）。  
> **非本目录**：多协议 RB/绑定/queue 深度语义 → [`../05_协议模块多协议兼容优化/`](../05_协议模块多协议兼容优化/README.md)；LDI↔IAP 配置解耦 → [`../07_LDI与IAP配置解耦/`](../07_LDI与IAP配置解耦/README.md)（暂缓改码）。

---

## §0 总判（2026-08-14 修订）

| 维度 | 结论 |
|------|------|
| 多协议共享 RB | **已完成**（见 05）；体在 SRAM；现行 **1536/768/768** |
| 分区宪法 | SRAM=堆/队列/RB/DMA；CCM=显存/BSRR |
| 堆 | **36KB @ SRAM**；CCM 无 ucHeap |
| 链接 | 无阻塞；EIDE Debug 编 1-577(3×3)、排除 1-260/1-969/RLS/AH；与 1-969 显存同为 CCM 38208 |
| 占用账 | 见 [04](./04_current_memory_occupancy.md)（采样口径 **EIDE Debug**；须按构建/模组重采） |
| **下一步** | 冒烟 + 堆水位；**IAP 升级实机**待验 |

---

## 文档结构

| 文件 | 角色 |
|------|------|
| [01_background_and_problem.md](./01_background_and_problem.md) | 背景与需求（历史 CCM 溢出） |
| [02_memory_policy.md](./02_memory_policy.md) | **分区宪法**（冻结） |
| [03_as_built_baseline.md](./03_as_built_baseline.md) | 链接/构建架构事实 |
| [04_current_memory_occupancy.md](./04_current_memory_occupancy.md) | **占用实测账本** |
| [05_migration_plan.md](./05_migration_plan.md) | Phase A 执行记录 + 后续瘦身附录 |

---

## §1 全局原则（最高约束）

1. **分区**：SRAM = 固定大小（FreeRTOS 堆、帧队列、协议 RB、UART DMA）；CCM = 显存/BSRR + 大屏余量。  
2. **CCM 判据**：DMA/ETH 可达 → 永留 SRAM。  
3. **Phase A**：`ucHeap` 不得留 CCM（已满足）。  
4. **RB 位置**：默认 SRAM；尺寸/行为归 05，本目录约束段属性与占用。  
5. **数字隔离**：引用必须标注 `构建/模组/日期`。  
6. **改码后**：更新 04 实测。

---

## §2 状态表

| 事项 | 状态 |
|------|------|
| RB @ SRAM（三槽 provide） | ✅；尺寸 1536/768/768 |
| 显存 @ CCM | ✅ |
| 堆回 SRAM 36KB | ✅ |
| RTT Up 4→2KB | ✅ |
| UART DMA 瘦身 / 删 RS232_1 DMA | ✅（640 / 无 index1） |
| 协议帧 queue 深度修订 | ✅（见 05；占 SRAM 静态） |
| EIDE 大屏链接 | ✅ CCM ~58% |
| 堆水位 / IAP 实机 | ☐ 待测 |

---

## §3 阅读顺序

| 目的 | 文档 |
|------|------|
| 占多少 | **04** |
| 该放哪 | **02** |
| 怎么迁（执行记录） | **05** |
| 协议 RB / queue | → **doc/05** |

通读：01 → 02 → 03 → 04 → 05。
