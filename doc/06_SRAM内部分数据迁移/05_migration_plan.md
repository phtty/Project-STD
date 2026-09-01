# 05 RAM 迁移方案（堆回退 + 1-969）

| 项 | 值 |
|----|-----|
| 角色 | Phase A 回退与 SRAM 瘦身的 **唯一执行蓝本** |
| 状态 | **✅ 已执行**（2026-08-14）：堆 **36KB @ SRAM** + RTT 4→2KB |
| 数字基线 | [04_current_memory_occupancy.md](./04_current_memory_occupancy.md) |
| 宪法 | [02_memory_policy.md](./02_memory_policy.md) |
| RB 行为 | 已完成，见 doc/05；本方案 **不改** 协议绑定 |

---

## §0 审核与执行记录

原审核项（已按授权执行，堆改为 36KB 而非蓝本初稿 40KB）：

- [x] 堆从 CCM 迁回 SRAM（`configAPPLICATION_ALLOCATED_HEAP`→0，删 `freertos_heap_ccm.c`）
- [x] 与瘦身同提交；36KB 后缺口约 **1256B**，用 RTT Up 缓冲 4KB→2KB（约 −2KB）覆盖
- [x] 优先候选：缩 SEGGER RTT（未动 AH_MQTT / LwIP）
- [x] 验收以 EIDE 大屏链接成功（Phase A 验证口径 1-969；现行 1-577 3×3，CCM 同为 38208）+ 更新 04 为准
- [x] 授权执行：用户要求「执行修改，ucHeap 改为 36KB」

---

## §1 目标与收益

| 目标 | 推算 / 结果 |
|------|-------------|
| CCM 去掉 ucHeap | 79168→**38208**（58.3%），余 ≈26.7KB |
| 1-969 可链接 | 消除 CCM 溢出 |
| SRAM 容纳堆 | 95464+36864−RTT≈2KB ≤ 131072 |

全局 192KB 够用；纠正的是 **位置**。堆从 40KB 改为 **36KB** 依赖此前任务栈压缩；须测水位 / `vApplicationMallocFailedHook`。

---

## §2 已落地机械步骤

### ① `Core/Inc/FreeRTOSConfig.h`

- `configAPPLICATION_ALLOCATED_HEAP`：**1 → 0**
- `configTOTAL_HEAP_SIZE`：**40×1024 → 36×1024**，堆位 SRAM

### ② 删除 `Core/Src/freertos_heap_ccm.c`

并从 `.eide/eide.yml` / Makefile 去掉列举。

### ③ `heap_4.c` 零改动

宏为 0 后自动 `static ucHeap[]` → `.bss`。

### ④ 注释清理

- `dev_w25qxx.c` / `app_ldi_cfg.c`：去掉「任务栈必在 CCM」过时表述；**保留** bounce / 禁栈 DMA。

### ⑤ SRAM 瘦身

- `SEGGER_RTT_Conf.h`：`BUFFER_SIZE_UP` **4KB → 2KB**

---

## §3 数字（执行前推算）

```
现状 SRAM     95464
+ ucHeap      36864   (36KB)
− RTT          2048
=            130280  ≤ 131072  ✓
```

未采用：排除 AH_MQTT、动 LwIP。

---

## §4 明确不改（本方案范围外）

- 不改 doc/05 已落地的三槽 / 绑定 / 链式逻辑  
- 不把 RB 迁回 CCM  
- 不把 DMA 池放进 CCM  
- 不在未测情况下同时编入 1-260+1-577+1-969 三套显存  

---

## §5 验收

- [x] 三步机械变更合入  
- [x] RTT 瘦身覆盖 36KB 回退缺口  
- [x] EIDE Debug **链接成功**（RAM 130280 / CCM 38208；Phase A 验证口径 1-969，现行 EIDE 编 1-577 3×3 同量级）  
- [x] 更新 [04](./04_current_memory_occupancy.md)  
- [x] `nm`：`ucHeap` @ SRAM（`0x2000…`，不在 `0x1000…`）  
- [ ] ETH / UART / 字库 / IAP-UDP / LDI-TCP 冒烟  
- [ ] 记录 `xPortGetMinimumEverFreeHeapSize` 新基线  

---

## §6 附录：Phase A 之后的 SRAM 瘦身（2026-08-14，已执行）

与 Phase A 同属 RAM 治理，但**不改** 05 协议绑定语义（仅尺寸/深度）：

| 项 | 变更 |
|----|------|
| RB | 2304/512/512 → **1536/768/768** |
| UART DMA | 2048×3 → **640×2**（删 RS232_1） |
| 帧 queue | LDI **2→4**；青海 payload 259 + 深度 **3**；AH **1→3**；IAP/RLS 深度 **2** |
| RLS probe | `mem_pool` → `RLS_PAYLOAD_MAX` |

占用重采见 [04](./04_current_memory_occupancy.md)；queue/RB 产品说明见 **doc/05**。

---

## 修订

- 2026-08-14：由原 `06_heap_revert_and_1_969` 升为本目录执行蓝本；增加 **待审核门禁**；缺口定为 ≈5.4KB。
- 2026-08-14：授权执行 — 堆 **36KB** 回 SRAM + RTT 4→2KB；删除 CCM 堆文件。
- 2026-08-14：附录记录 RB/DMA/queue 后续瘦身；04 重采。
