# 02 存储分区宪法（RAM）

> **状态：冻结**。本仓库 **SRAM/CCM 归属** 的最高决策。  
> **修订**：走 README §1。  
> **协议谁绑哪个 RB**：doc/05；本文件只定 **物理段归属**。

---

## §1 三条原则

### ① SRAM = 固定大小数据

| 主体 | 宪法位置 | 现行 | 动作 |
|------|----------|------|------|
| ucHeap 36KB | SRAM `.bss` | **SRAM** | ✅ Phase A 已回退 |
| 协议帧队列 | SRAM | SRAM | 维持 |
| 协议 RB | SRAM | SRAM（provide **1536/768/768**） | 维持（05 已落地；尺寸以 05 为准） |

### ② CCM = 显存 / BSRR（性能路径）

| 主体 | 宪法位置 | 现行 |
|------|----------|------|
| pixel_map / hub75_buff / g_bsrr / row_dst | CCM | CCM |

### ③ CCM 预留大屏余量

1-969 ≈38208B（58.3%）；堆回退后余 ≈26.7KB。  
堆不得长期占 CCM。

---

## §2 方向方向

1. **Phase A 回退**：✅ `configAPPLICATION_ALLOCATED_HEAP`→0；删 `freertos_heap_ccm.c`；`heap_4` static ucHeap → SRAM（现行 **36KB**）。  
2. **显存留 CCM**：不翻案。  
3. **RB 留 SRAM**：行为归 05；禁止再把调度 RB 迁回 CCM「给堆让位」。  
4. **1-969 不是另案**：堆回退后自然容纳。

配套瘦身：RTT Up 4KB→2KB（见 [05_migration_plan.md](./05_migration_plan.md)）。

---

## §3 必须保留的事实

### DMA/ETH 门禁（永留 SRAM）

ram_heap、RX_POOL、PBUF_POOL、ETH 描述符、UART RX DMA、`s_dma_bounce`；`_ptr_in_ccm` 检测保留。

### 构建隔离

占用权威：EIDE Debug（**1-577 3×3**；exclude 1-260 / 1-969 / RLS / AH）→ [04](./04_current_memory_occupancy.md)。  
Makefile（1-260 + RLS/AH 全编）数字不得与之混用。

---

## §4 决策矩阵

| 决策项 | 裁定 |
|--------|------|
| ucHeap | **SRAM**（36KB） |
| RB | **SRAM**（已到位） |
| 显存 | **CCM** |
| 帧队列 | **SRAM** |

优先级：DMA 门禁 > 数据性质 > 性能。

---

## §5 长期验收（迁移完成后）

- [x] EIDE 大屏链接成功（Phase A 验证口径 1-969；现行 1-577 3×3，CCM 同为 38208）；更新 04  
- [x] `ucHeap` 不在 `0x1000…`  
- [ ] ETH/UART/字库冒烟  
- [ ] `xPortGetMinimumEverFreeHeapSize` 重标定  
