# 副作用归档：多协议 → FreeRTOS 堆压力

> **状态**：`[已归档]`　|　原 `03_issue_freertos_heap.md` 精简重写  
> **说明**：本问题发生在「多协议同时自注册」之后，根因是 **FreeRTOS `ucHeap`（任务栈/动态 OS 对象）**，不是 RB 静态体。  
> **结构治理**：[`../06_SRAM内部分数据迁移/`](../06_SRAM内部分数据迁移/README.md) — Phase A **已执行**（堆 36KB @ SRAM）。

---

## 1. 现象（历史）

上电进入 `vApplicationMallocFailedHook`：

```
tcp_server_task → osSemaphoreNew → pvPortMalloc 失败
```

当时堆约 36KB @ SRAM，多协议任务栈叠满后在 TCP Server 起步处击穿。

---

## 2. 定性（仍成立）

| 维度 | 结论 |
|------|------|
| 挤爆的是 | FreeRTOS `ucHeap`（任务栈、TCB、动态 Queue/Sem） |
| 不是 | 协议 RB 静态缓冲（现 @ SRAM provide，不占堆） |
| 也不是 | 协议帧 queue 静态缓冲（`s_*_queue_buf`，不占堆） |
| 也不是 | LwIP `MEM_SIZE` 池（独立） |

多协议兼容（05）必然带来 **任务栈线性叠加** → 必须做堆预算；与「一通道一 RB」正交。

---

## 3. 已合入的缓解（保留）

| 措施 | 效果 |
|------|------|
| 青海等帧队列静态化 | 少占运行时堆 |
| 工厂任务栈减半；interrupt 不重建任务 | 少占 / 少碎片 |
| TCP Server 断开信号量静态化 | 启动少一次动态分配 |
| 堆曾扩至 40KB 并迁 CCM | 历史止血；**已按 06 回退** |

排障：区分「RB 太多」误判；用 `xPortGetFreeHeapSize` / 任务栈清单；扩堆前看 06 占用账。

---

## 4. 与 06 的衔接（现行）

| 项 | 现行代码 |
|----|----------|
| `configTOTAL_HEAP_SIZE` | **36×1024** |
| `configAPPLICATION_ALLOCATED_HEAP` | **0**（`heap_4` static `ucHeap` → SRAM `.bss`） |
| CCM | 无 `ucHeap`；仅显存/BSRR |

待办（非本文件）：`xPortGetMinimumEverFreeHeapSize` 标定、IAP 升级冒烟。

- 05 目录不再把「堆留 CCM」写成结构方案。
