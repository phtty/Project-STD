# 多协议共享 RB — 落地状态（对照当前代码）

> **基准**：2026-08-14（含 RB/DMA/queue 修订后）  
> **总体结论：RB / 协议绑定 DoD = READY（已落地）**  
> 设计规范：[01_architecture.md](./01_architecture.md)  
> RAM 占用权威：[`../06_SRAM内部分数据迁移/04_current_memory_occupancy.md`](../06_SRAM内部分数据迁移/04_current_memory_occupancy.md)  
> LDI 选编依赖：[`../07_LDI与IAP配置解耦/`](../07_LDI与IAP配置解耦/README.md)

---

## 1. 验收项对照

| 编号 | 判定 | 现状 |
|------|------|------|
| A-1 单固件多协议 | ✅ | Makefile 同编 IAP/LDI/青海/RLS/AH/四川三协议；**EIDE Debug 现编 IAP+LDI+青海+四川三协议**（`.eide/eide.yml` exclude RLS/AH）；各自 `sw_app_initcall` |
| A-2 目录选编 | ⚠️ | 无 `APP_PROTOCOL`；排除协议目录可裁解析器。配置层已抽至 `Application/Config/app_board_net_cfg.c`，**排除整个 `IAP/` 不影响 LDI**（解耦见 doc/07） |
| A-3 自注册 | ✅ | `app_boot` 无协议分支；四步完整 |
| A-4 同通道链式 | ✅ | `any_wait/any_fake/any_parsed`；RS485：青海+RLS；RJ45：IAP+LDI（+可选 MQTT） |
| A-5 语音 USART6 | ✅ | 零处 `bind(CH_ID_RS232_1)`；voice → `PL_UART6` |
| A-6 单协议退化 | ✅ | weak provide；排除目录后无硬编码互斥（LDI 仍依赖 cfg，见 A-2） |
| NF-2 跨物理口隔离 | ✅ | 三物理槽独立 |
| NF-3 兼容矩阵门禁 | ⚠️ | 编译期检查仍缺（青海 vs 重庆）；运行约定见 `01` §6 |
| NF-4 网口共享策略 | ✅ 已采纳默认 | 共享 RJ45 **1536**；高压并发风险见 `01` §2.4 |

---

## 2. 现行 RB / 绑定表（代码事实）

定义：`Application/Inc/app_dispatch.h`（`rb_slot_t` / `RB_SIZE_*`）；池：`app_dispatch.c`（`g_rb_provide[]`）。

| 槽 | 尺寸 | 协议 | bind |
|----|------|------|------|
| `RB_SLOT_RJ45` | **1536** | IAP | `CH_ID_UDP` |
| 同上 | | LDI | `CH_ID_TCP_SERVER` / `TCP_CLIENT` / `UDP`（每通道独立 mask） |
| 同上 | | AH_MQTT | `CH_ID_MQTT`（initcall 可关） |
| `RB_SLOT_RS485` | **768** | 青海、RLS、四川 ETC/MTC/治超 | `CH_ID_RS485` |
| `RB_SLOT_RS232` | **768** | 青海、四川 ETC/MTC/治超 | `CH_ID_RS232` |

关键源文件：

| 文件 | 要点 |
|------|------|
| `app_dispatch.c` | provide 表、acquire 判空、写去重、链式分发；`_Static_assert` 尺寸 1536/768/768 |
| `ring_buffer.h` | `RB_PROVIDE_WEAK`；可用容量 = size−1 |
| `app_iap.c` | provide RJ45；**仅 UDP**；`IAP_QUEUE_DEPTH=2` |
| `app_ldi.c` | provide RJ45；**无 485/232**；`LDI_QUEUE_DEPTH=4` |
| `app_qh_proto.c` | provide 485+232；`QH_PAYLOAD_MAX=259`；`QH_QUEUE_DEPTH=3` |
| `app_sc_etc_proto.c` | provide 485+232；`SC_ETC_PAYLOAD_MAX=149`；`SC_ETC_QUEUE_DEPTH=3`；心跳计时任务（2026-08-17 已停用，见「修订」） |
| `app_sc_mtc_proto.c` | provide 485+232；`SC_MTC_PAYLOAD_MAX=74`；`SC_MTC_QUEUE_DEPTH=3`；'{' '}' 定界变长 probe（上限 74B） |
| `app_sc_ol_proto.c` | provide 485+232；`SC_OL_PAYLOAD_MAX=249`；`SC_OL_QUEUE_DEPTH=3`；FF+len probe（07~FF，0xFE 排除） |
| `app_uart_baud.c` | DIP1 波特率选择 + `pl_uart_set_baud` 运行态切换（RS232+RS485） |
| `app_rls.c` | provide 485；`mem_pool[RLS_PAYLOAD_MAX]`；`RLS_QUEUE_DEPTH=2` |
| `ah_mqtt.c` | provide RJ45；`AH_MQTT_QUEUE_DEPTH=3`；initcall 可关 |
| `app_rs232.c` / `app_boot.c` | RS232_1 旁路；无 DMA RX 缓冲 |

通道启动（`app_boot.c`）：TCP Server/Client、UDP、RS485、RS232；不启 RS232_1 协议 RX。

### 2.1 协议帧 queue 深度（2026-08-14 修订）

| 协议 | 深度 | 静态缓冲约 | 变更 |
|------|------|------------|------|
| IAP | 2 | 2104 | 维持 |
| LDI | **4** | 2080 | 2→4（粘包多帧） |
| 青海 | **3** | 801 | payload 259 + 深度 3 |
| RLS | 2 | 1076 | 维持 |
| AH_MQTT | **3** | ~1623（任务启动后） | 1→3 |
| 四川 ETC | **3** | 471 | 新增（payload 64→149，0x0D 定界对齐 9K1F212701） |
| 四川 MTC | **3** | 246 | 新增（payload 74） |
| 四川治超 | **3** | 120 | 新增（payload 32） |

构建口径：IAP/LDI/青海两套构建均编入；**RLS/AH 仅 Makefile 编入**，EIDE Debug 排除（采样 elf 中无其队列缓冲，见 06/`04`）。

`osMessageQueuePut(..., timeout=0)`：满则丢整帧。深度与 RB/DMA 无强制联调。

---

## 3. 相对历史方案的变更关闭表

| ID | 历史问题 / 旧规划 | 状态 | 说明 |
|----|-------------------|------|------|
| ARCH-1 | 8×2KB @ CCM | ✅ 取代 | 三槽 provide @ SRAM |
| ARCH-2 | IAP RS485+UDP 双槽 | ✅ 关闭 | 仅 UDP |
| ARCH-3 | LDI 绑 RS485 | ✅ 关闭 | 仅网口 |
| ARCH-4 | 每网口独占 RB | ✅ 产品决策 | 改共享 RJ45（NF-4） |
| CR-1 | IAP 盲 WAIT | ✅ | 首字节快拒 |
| CR-2 | RLS peek/长度越界 | ✅ | `mem_pool`≥`RLS_PAYLOAD_MAX`；RB 768 |
| CR-4 / MA-5 | AH_MQTT probe 过松 | ⚠️ 例外 | initcall 默认关 |
| MI-3 | 兼容矩阵编译门禁 | ❌ 可选后续 | |
| MI-CFG | LDI→`app_iap_cfg` 强依赖 | ✅ 已解耦 | `app_board_net_cfg`（Application/Config），见 doc/07 |

---

## 4. DoD（本目录：多协议共享 RB）

1. [x] `RB_CNT_MAX==3`，尺寸 **1536/768/768**，`_Static_assert` 在册  
2. [x] 无调度路径 `RB_DEFINE_CCM(g_rb*)` 八槽池  
3. [x] IAP 无 `CH_ID_RS485`；LDI 无 RS485/RS232 bind  
4. [x] 无协议 bind `CH_ID_RS232_1`  
5. [x] 同物理口链式 + 写去重仍在  
6. [x] EIDE/Makefile 无 `APP_PROTOCOL`  
7. [x] 帧 queue 深度：IAP2 / LDI4 / QH3 / RLS2 / AH3 / SC_ETC3 / SC_MTC3 / SC_OL3（静态缓冲）  

**不在本 DoD**：堆水位标定、IAP 升级实机验证、doc/07 解耦落地。

---

## 5. 建议回归（协议行为）

| 用例 | 期望 |
|------|------|
| RS485 青海 / RLS 帧 | 各自 READY；RLS 满长 ≤530 可 READY |
| 四川 MTC '{' 帧（'}' 定界变长） | '1','2','5'→3B；'3' 单行行号[2]+变长文本[3..]；'4' 全屏变长文本[2..]；'6' 客车 ≥15B/货车 ≥24B（数字串 ≥11/20B）；'7'固定→4B、'78' 自定义文本变长；'8','9'→4B 全部 READY（无 BCC，9K1F212701 语义） |
| 四川 ETC 0A 帧 | 行号 0~6；单行 ≤24B；0x30 → 软件复位（9K1F212701 语义） |
| 四川治超 FF 帧 | 80 全屏 + 81~88 八行；亮度 00=自动调光（9K1F212701 语义） |
| TEST 键 | 任意业务数据到达仅中止当前检测序列，monitor 回 IDLE；TEST 键始终可用（9K1F212701：收包清 testMode，按键扫描持续） |
| UDP IAP 与 LDI | 共享 RJ45，互不永久阻塞 |
| TCP LDI 一包 ≥3 帧 | 深度 4 下应均可入队（处理任务跟得上时） |
| TTS | USART6 出帧；协议口无语音字节 |
| 排除青海目录 Rebuild | Makefile 口径：RS485 provide 仍可由 RLS 提供；EIDE 口径：RLS 亦被排除，RS485 槽无提供者 → `acquire` 判空安全退化 |
| 排除整个 IAP/ 仅留 LDI | 链接成功（配置层在 `Application/Config`，不属 IAP 协议目录） |

---

## 6. 可选后续（非阻塞）

| 项 | 说明 |
|----|------|
| MI-3 编译期兼容检查 | 禁帧头冲突同编 |
| AH_MQTT 产品启用 | 收紧 probe + 堆预算 |
| doc/07 解耦 | 抽出 board_net_cfg（暂缓） |
| IAP 升级实机 | 停等模型下 queue=2；验证 ACK 节奏 |

---

## 修订

- 2026-08-14：首版落地对照。  
- 2026-08-14：对齐 RB 1536/768/768、queue 深度表、A-2/doc/07、DoD。  
- 2026-08-14：A-1 与 §2.1 补充 EIDE/Makefile 协议选编差异（EIDE exclude RLS/AH）。
- 2026-08-14：新增四川 ETC/MTC/治超三协议绑定 RS485+RS232 双通道、queue 深度 3；MTC vs 青海 '{' 帧头冲突以 EIDE 目录排除纪律兜底（01 §6）。
- 2026-08-15：修复四川三协议显示语义与 TEST 键失效（见 doc/06 §5 修订）：
  - MTC '{' 帧族改为无 BCC（9K1F212701）/带 BCC 双格式兼容，BCC 不再校验 → MTC vs 青海 '{' 帧头重叠的判别力由「BCC 校验」降为「帧长+尾字节」；青海 '3' 帧恰 20B 且尾字节 '}' 时会被 MTC 认领的残留风险上升（≈1/256 → 长度巧合概率），量产二选一纪律不变；
  - MTC '6' 固定格式字段整体错位修正（X1 起偏移）；
  - ETC 行号 1~6、单行 ≤24B、0x30 软件复位、显示颜色跟随通行灯；
  - 治超 80 全屏显示 + 81~88 八行、亮度 00 自动调光、颜色跟随通行灯；
  - TEST 键：`app_factory_mode_interrupt` 由销毁 factory_monitor 任务改为置中止标志，monitor 分片等待回 IDLE。
- 2026-08-17：MTC '{' 帧族改「'}' 定界变长」（对齐 9K1F212701 mtc.c 逐字节扫 '}'），废除定长双长度与 '78' BCC 校验；修复上位机 `{3 1 1234 } 3 }`（10B）单行乱码（旧定长 probe 跨帧拼凑 20B 认领含 '}'/'{' 垃圾文本）；字段偏移按变长重算；payload 仍 74B；宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/sc_mtc_frame_sim.py`（`--old` 复现）。MTC vs 青海残余风险更新为「青海帧数据段含 0x7D 且半帧到达被截断认领」（01 §6、doc/CLAUDE.md 帧头冲突纪律）。
- 2026-08-17（ETC 心跳停用 + MTC '4' 全屏换行修复）：
  - ETC 心跳计时任务 #if 0 停用、任务不再创建（ucHeap 任务栈 -1KB）：5 分钟超时「ETC车道关闭」显示不再生效；黄闪 0A 38 开启后的 10 秒自动关闭依赖同一任务 tick 一并失效，须 0A 39 显式关闭；0A 50 心跳帧解析保留（识别后丢弃）；`sc_etc_show_lane_closed`/`sc_etc_hs_timeout`/`sc_etc_activity_refresh` 保留待恢复（无引用，链接期 gc-sections 丢弃）。恢复方法见 `app_sc_etc_proto.c` 注释；
  - MTC '4' 全屏渲染改整屏 word_wrap（w=屏宽、h=整屏高、word_wrap=true，先清屏后渲染）：原 16B/行固定切 ≤4 行且单行 word_wrap=false，第一行超宽溢出被裁、超 64B 文本被丢弃；'3' 单行保持 word_wrap=false + h=当前字号（截断语义，未变）。
