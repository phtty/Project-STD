# 03 项目架构基线（RAM 相关）

> **角色**：链接/构建与堆配置的代码级事实  
> **占用数字** → [04](./04_current_memory_occupancy.md)  
> **RB / queue 行为与深度** → doc/05

---

## §1 两套构建

| | Makefile | EIDE Debug（现行常见） |
|--|----------|------------------------|
| 显示 | 默认可含 `1_260` | **仅 1-577**（exclude 1-260/1-969；宏 3×3 → CCM **38208**） |
| 协议 | **全量**：IAP / LDI / 青海 / RLS / AH_MQTT | **IAP + LDI + 青海**（exclude RLS / AH） |
| Kernel | 同（initcall / ring_buffer / bit_utils / crc_utils / text_cvt / `bcc_utils.c`） | 同左 |
| 堆 | 同：`heap_4` static ucHeap @ SRAM **36KB** | 同 |
| RB | 同：三槽 provide @ SRAM **1536/768/768** | 同 |
| 占用权威 | 对照用 | **现行 04**（须标注模组） |

> 1-577 3×3 与 1-969 默认拼屏的 CCM 显存合计均为 **38208B**，勿仅凭 CCM 大小反推模组名。

---

## §2 链接与启动

- `Compiler/STM32F407XX_FLASH.ld`：FLASH 0x08040000/768K；RAM 128K；CCMRAM 64K；`.ccmram (NOLOAD)`  
- `startup.c`：清 `_sccmram.._eccmram`  
- `_Min_Heap_Size=0x400` / `_Min_Stack_Size=0xA00`（合计 0xE00）→ `._user_heap_stack` 实测 **3588B**  

---

## §3 FreeRTOS 堆（已回退）

```
configTOTAL_HEAP_SIZE            = 36*1024
configAPPLICATION_ALLOCATED_HEAP = 0
ucHeap ∈ heap_4.c static → .bss (SRAM)
```

`freertos_heap_ccm.c` 已删除。执行记录见 [05_migration_plan.md](./05_migration_plan.md)。

---

## §4 显存尺寸（CCM）

| 模组 | CCM 合计 | 口径 |
|------|----------|------|
| 1-260 | ~2432B | 小屏 |
| 1-577 1×1 | 较小 | 改宏 |
| 1-577 **3×3** | **38208B** | EIDE 现行常见（与 1-969 同量级） |
| 1-969 | **38208B** | map 实测 |

刷新路径无 DMA → 可留 CCM。

---

## §5 UART DMA（SRAM）

| 缓冲 | 尺寸 | 说明 |
|------|------|------|
| `s_rs485_buf` | **640** | ≥ RLS 530；≤ RB−1 |
| `s_rs232_0_buf` | **640** | ≥ QH 259；≤ RB−1 |
| `s_rs232_1_buf` | **已删除** | USART6 仅 TTS TX |

[待更新: 2026-08-18 UART RX 改乒乓双缓冲 circular，两缓冲均为 640→1280（块 640×2）；本表为历史基线，现行账见 04]

---

## §6 与 doc/05 的接口

调度层 RB 槽、provide、**帧 queue 深度**在 Application；本目录只要求其 **段在 SRAM**，并在 04 记账。
