# 04 当前 SRAM / CCMRAM 占用分布（实测）

> **角色**：本目录 **唯一占用实测账本**  
> **状态**：现行　|　采样：**EIDE Debug（GCC 15.2）** / 编入 **1-577 3×3**、排除 1-260 / 1-969 / RLS / AH_MQTT / **2026-08-14 16:37**（重建复核，数字未漂移）  
> **迁移**：[05_migration_plan.md](./05_migration_plan.md)（Phase A + 后续瘦身已执行）  
> **RB / queue 语义**：doc/05

**结论**：SRAM **≈120820B（92.2%）**；CCM **38208B（58.3%）**。`ucHeap` @ SRAM **36KB**；CCM 仅显存。全局约 159KB / 192KB，空闲约 **37.5KB**。

---

## §0 采样口径

| 项 | 值 |
|----|-----|
| 构建 | **EIDE Debug**（Arm GNU Toolchain 15.2 / GCC）；`outDir=build` → `build/Debug/Project_STD.elf`（对象在 `build/Debug/.obj/`） |
| 模组（CCM） | 编入 **1-577 3×3**（`.eide/eide.yml` exclude 1-260 / 1-969）；CCM **38208B**，与 1-969 同量级 |
| 协议选编 | 编入 **IAP + LDI + 青海**；exclude **RLS / AH_MQTT** |
| 段合计 | `.data` 1612 + `.bss` 115620 + `._user_heap_stack` 3588 = **120820** |
| CCM | `.ccmram` **38208** |
| 结果 | **链接成功** |

与「仅 1-260 小屏」数字 **严禁混用**。**勿用 `make` 重采**：Makefile 现编 1-260 + RLS/AH（见 `03` §1），覆盖 elf 后本表数字不复现。

| 段 | 大小 |
|----|------|
| `.data` | 1612B |
| `.bss`（含 ucHeap 36KB） | 115620B |
| `._user_heap_stack` | 3588B |
| **SRAM≈** | **120820B（92.18%）**；空闲 **≈10252B** |
| `.ccmram` | **38208B（58.30%）**；空闲 **≈27328B** |

---

## §1 CCMRAM（仅显存）

| 对象（1-577 3×3 / 同量级 1-969） | 大小 |
|----------------------------------|------|
| pixel_map + hub75_buff + row_dst + g_bsrr | **38208** |
| **合计** | **38208** |

CCM 中 **0 字节** ucHeap / RB / UART DMA。

---

## §2 SRAM

### 关键符号（nm，2026-08-14 16:37，EIDE Debug 产物）

| 对象 | 大小 | 说明 |
|------|------|------|
| `ucHeap` | **36864（36KB）** | FreeRTOS heap_4 @ SRAM |
| `_acUpBuffer` | 2048 | RTT Up（已 4→2KB） |
| `rb_provide_rj45_buf` | **1536** | RJ45 RB |
| `rb_provide_rs485_buf` | **768** | RS485 RB |
| `rb_provide_rs232_buf` | **768** | RS232 RB |
| `s_rs485_buf` / `s_rs232_0_buf` | **1280** / **1280** | UART DMA 乒乓双缓冲（640×2）；无 RS232_1 |
| `s_iap_queue_buf` | **2104** | 深度 2 |
| `s_ldi_queue_buf` | **2080** | 深度 **4** |
| `s_qh_queue_buf` | **801** | 深度 **3** |
| ETH/LwIP 池 | ram_heap / RX_POOL / PBUF 等 | 永留 SRAM（大户） |
| LwIP memp 池（netconn/udp_pcb） | 8×netconn + 8×udp_pcb（2026-08-21 起 `lwipopts.h` 显式 `MEMP_NUM_NETCONN=8`/`MEMP_NUM_UDP_PCB=8`，原 opt.h 默认 4） | 实测 bss 增量 **304B**；**新增 UDP 端口协议时随通道数核算本行**（每通道常驻 1 netconn + 1 udp_pcb，广播复用常驻 conn 不占池） |

`s_rls_queue_buf`（**1076**，深度 2）与 AH_MQTT 队列（~1623，任务内 static）为**源码口径**，**不在本采样 elf 内**（EIDE Debug 排除 RLS/AH；仅 Makefile 构建时才计入 bss）。

### 协议 RB（SRAM）

| 槽 | 容量 | 可用（size−1） |
|----|------|----------------|
| RJ45 | 1536 | 1535 |
| RS485 | 768 | 767 |
| RS232 | 768 | 767 |

### 协议帧 queue（SRAM 静态；2026-08-14 变更）

| 协议 | 深度 | 缓冲约 | 变更记录 | 本采样 |
|------|------|--------|----------|--------|
| IAP | 2 | 2104 | 维持 | ✅ 编入 |
| LDI | 4 | 2080 | **2→4** | ✅ 编入 |
| 青海 | 3 | 801 | payload 259 + **深度 3** | ✅ 编入 |
| RLS | 2 | 1076 | 维持 | — EIDE 排除 |
| AH_MQTT | 3 | ~1623 | **1→3**（任务内；initcall 关则未激活路径） | — EIDE 排除 |
| 山东 | 3 | 801 | 新增（payload 259，与青海同构） | — EIDE 排除 |
| 贵州 | 3 | 801 | 新增（payload 259，与青海同构；13 命令含 0x01/0x02） | — EIDE 排除 |
| 云南 | 3 | 801 | 新增（payload 259，与青海/贵州同构；13 命令含 0x01/0x02） | — EIDE 排除 |
| 重庆CQ | 3 | 3156 | 新增（payload 1044，JSON `{` 定界 + 12B 二进制）；**队列体+任务帧缓冲+JSON/文本缓冲置 CCMRAM**（SRAM 全协议构建仅余 ~1.9KB；CPU 独占访问无 DMA，见文末修订） | ✅ 编入（CCM） |

> **重庆CQ 内存例外（2026-08-20）**：CQ 队列 `s_cq_queue_buf` 3156B + `s_cq_queue_cb` 80B +
> 任务帧缓冲 1052B + JSON 缓冲 1045B + 文本缓冲 1044B = **6377B 置 `.ccmram`**。
> 理由：全协议 dev 构建 SRAM 仅余 ~1.9KB，CQ 帧队列 3236B 无法入 SRAM；
> 这些缓冲为 CPU 独占访问（osMessageQueue 静态内存经 CPU memcpy 读写、无 DMA/ETH），
> CCMRAM 可用（Makefile 1-260 显存 2432B；EIDE 1-577 口径 38208B + 6377B 亦不超 64KB）。
> cJSON 解析树为 **FreeRTOS 堆瞬时分配**（钩子 pvPortMalloc/pvPortFree，ucHeap 36KB
> 内支出，解析后 `cJSON_Delete` 归还）。`cq_proto_handle_task`/`cq_proto_timer_task`
> 各 1KB 栈（ucHeap，地区协议处理任务 +1KB、定时任务 +1KB）。

语义与 Put 丢帧行为 → **doc/05** `01` §4.1 / `02` §2.1。

`size` 的 bss 列含 `.ccmram`，勿直接当 SRAM。

---

## §3 数字链（相对 Phase A 刚完成时）

| 步骤 | SRAM 约 | CCM |
|------|---------|-----|
| Phase A 后（旧账，RB 仍 2304/512） | ~130280 / 99.4% | 38208 / 58.3% |
| + RB/DMA/queue 瘦身与修订（本采样） | **~120820 / 92.2%** | **38208 / 58.3%** |

主要释放：删 RS232_1 DMA、DMA 2048→640、RJ45 RB 2304→1536；RS485/232 RB 512→768 略增；LDI/QH queue 加深略增。**净效果 SRAM 明显留白**。

> 注（2026-08-18）：UART RX 已改乒乓双缓冲（s_rs485_buf/s_rs232_0_buf 640→1280×2），Makefile 口径实测 bss 130036（原 128724，+1312 = 双缓冲 1280 + 静态通道实例 32）、text 334256（+1184，框架/协议防御代码）。EIDE 口径总量以 EIDE 重采为准。

---

## §5 四川三协议增量账（Makefile 口径实测）

采样：**Makefile GCC Debug**（`make -B -j8`，2026-08-14；编 1-260 + 全协议含 RLS/AH）。
注意与 §0 口径不同（EIDE Debug 编 1-577 3×3 且排除 RLS/AH），两口径数字**严禁混用**。

| 段 | 基线（未编三协议） | 编入三协议后 | 增量 |
|----|--------------------|--------------|------|
| text | 319664 | **328280** | **+8616** |
| data | 1604 | **1620** | **+16** |
| bss（含 .ccmram 2432） | 123868 | **126396** | **+2528** |

SRAM 合计（`.data + .bss(不含 ccmram) + ._user_heap_stack`）：**≈125363B（95.7%）**，未超 124KB（126976B）上限。

增量构成（nm 实测）：
- 帧 queue 静态体：`s_sc_etc_queue_buf` **471**（2026-08-15 ETC payload 64→149，0x0D 定界上限对齐 9K1F212701） + `s_sc_mtc_queue_buf` 246 + `s_sc_ol_queue_buf` **789**（2026-08-17 治超 payload 249→255 = 长度字段上限 FF，消除 250~255 长帧队列截断越界读） = **1506**；`*_queue_cb` 3×80=240
- 任务内帧缓冲（static bss）：ETC 157 + MTC 82 + OL **263** = **502**
- 状态：`s_ol_lines` **200**（8 行 ×24B + `s_ol_line_len` 8B；2026-08-15 治超行数据改变长 ≤24B，原 8×16B=128B）、`s_etc_uptime/activity_s` 8、各 mask/句柄 36、杂项 ~20
- 任务栈（ucHeap，非 bss）：3 处理任务 = **3×1KB**（2026-08-17 ETC 心跳计时任务停用 -1KB，原 4×1KB；从 36KB ucHeap 支出）
- 数据段 +8：`s_mtc_color`/`s_mtc_font_size` 等初值静态；+8：`s_etc_color`/`s_ol_color`（显示颜色跟随通行灯状态）

CCM 增量 **0**（无新 CCM 对象）。

---

## §6 山东协议增量账（Makefile 口径实测）

采样：**Makefile GCC Debug**（`make -j8`，2026-08-17；编 1-260 + 全协议含 RLS/AH + 山东）。
基线为 §5 末行口径（text 327408 / data 1620 / bss 126396），两次数值精确衔接（增量 = 山东模块贡献）。

| 段 | 基线（§5 末行口径） | 编入山东后 | 增量 |
|----|---------------------|------------|------|
| text | 327408 | **328872** | **+1464** |
| data | 1620 | **1620** | **+0** |
| bss（size 工具口径，含 .ccmram 2432） | 126396 | **127556** | **+1160** |

SRAM 合计（相对 §5 基线增量 +1160）：**≈126523B（96.5%）**，未超 124KB（126976B）上限。

增量构成（nm 实测）：
- 帧 queue 静态体：`s_sd_queue_buf` **801**（3×267，payload 259）+ `s_sd_queue_cb` **80** = 881
- 任务内帧缓冲（static bss）：**267**
- 状态：`s_sd_mask`/`s_sd_mask_rs232`/队列句柄 **12**
- 任务栈（ucHeap，非 bss）：`sd_handle_task` **1×1KB**（地区协议处理任务 3→4）
- text +1464：probe/parse/cmd/注册代码 + 版本串 rodata 10B

CCM 增量 **0**（无新 CCM 对象）。

2026-08-17（默认显示注册机制 + 版本应答 PROGRAM_CODE）追加采样（同口径 `make -B -j8`）：

| 段 | 山东基线 | 本任务后 | 增量 |
|----|----------|----------|------|
| text | 328872 | **329112** | **+240** |
| data | 1620 | **1620** | **+0** |
| bss（size 工具口径，含 .ccmram 2432） | 127556 | **127564** | **+8** |

增量构成（nm 实测）：
- `s_default_fn`（`app_default_display.c` 注册槽）**4B bss** + 链接尾部对齐 4B → .bss 净 **+8**（.ccmram 2432 / ._user_heap_stack 3588 不变）
- 新增 `s_sd_default_text` 22B rodata + `_sd_default_show` / `app_default_display_register` / 回退渲染代码 + initcall 条目 8B + 名称字符串 + PROGRAM_CODE 在 `app_sd_proto_cmd.c` 的 rodata 副本；删除 `s_sd_version_text` 10B → text 净 **+240**

SRAM 合计（§5 基线 +1160 +8）：**≈126531B（96.5%）**，未超 124KB（126976B）上限。
CCM 增量 **0**（无新 CCM 对象）。

2026-08-17（文本常量字节数组 → UTF-8 字符串字面量 + 山东上电画面改黄字）追加采样（同口径 `make -B -j8`）：

| 段 | 山东基线 | 本任务后 | 增量 |
|----|----------|----------|------|
| text | 329112 | **329456** | **+344** |
| data | 1620 | **1620** | **+0** |
| bss（size 工具口径，含 .ccmram 2432） | 127564 | **127564** | **+0** |

增量构成（nm 实测）：
- rodata（GBK 字节数组 → UTF-8 字面量，汉字 2B→3B）：`s_sd_default_text` **22→33B（+11，0x21）**；MTC 固定标签 7 个 **+24**（34→58）；MTC 初始化「祝您一路平安」**+7**、自检「系统正在加电自检」**+9**；MTC 固定语音表（`s_mtc_fixed_voices` 指针表 32B + 8 串 206B）**+58**；青海文明用语表（`s_civil_texts` 指针表 16B + 4 串 130B）**+40**（'0' 同时修正为 doc/04 裸机文本「贵州高速公路」，原数组字节有损）→ 合计 **+149**
- code（约 **+195**）：MTC/QH 语音播报前 UTF8ToGBK 运行时转换调用点（`UTF8ToGBK` 本体早已随 `app_render` 链接，无新增）；MTC 初始化/自检的静态 GBK 数组改为栈缓冲 + 运行时转换；`_sc_mtc_render_center` 改 `FONT_ENC_UTF8`
- `s_sc_etc_lane_closed_text`（ETC「车道关闭」23B→34B）：调用方已停用，符号被 --gc-sections 丢弃，**0 增量**
- bss 无变化：转换缓冲全部为函数内栈/局部，未引入静态存储

SRAM 合计不变：**≈126531B（96.5%）**，未超上限。
CCM 增量 **0**。

2026-08-17（dev_display_fill 越界下溢修复）追加采样（同口径 `make -j8`，插桩已清理）：

| 段 | 山东基线 | 本任务后 | 增量 |
|----|----------|----------|------|
| text | 329456 | **329472** | **+16** |
| data | 1620 | **1620** | **+0** |
| bss（size 工具口径，含 .ccmram 2432） | 127564 | **127564** | **+0** |

增量构成：`dev_display_fill` 起点越界早退分支（`x >= screen_rows || y >= screen_cols` 丢弃）≈ +16B code；无新增静态存储。SRAM / CCM 不变。

2026-08-17（贵州协议接入）追加采样（同口径 `make -j8`）：

| 段 | 贵州基线 | 本任务后 | 增量 |
|----|----------|----------|------|
| text | 329472 | **333072** | **+3600** |
| data | 1620 | **1620** | **+0** |
| bss（size 工具口径，含 .ccmram 2432） | 127564 | **128724** | **+1160** |

增量构成（nm 实测）：
- 帧 queue 静态体：`s_gz_queue_buf` **801**（3×267，payload 259）+ `s_gz_queue_cb` **80** = 881
- 任务内帧缓冲（static bss）：`msg_buf` **267**
- 状态：`s_gz_mask`/`s_gz_mask_rs232`/队列句柄 **12**
- 任务栈（ucHeap，非 bss）：`gz_handle_task` **1×1KB**（地区协议处理任务 4→5）
- text +3600：probe/parse/cmd/voice 四文件代码（`gz_parse_frame` 0x20C、`_gz_exec_fixed` 0x268、`gz_execute_cmd` 0xAC 等）+ rodata（文明用语 4 串 UTF-8、颜色/亮度/音量映射表、队列/任务 attr）

SRAM 合计（贵州基线 +1160）：**≈127691B（97.4%）**，未超 124KB（126976B）上限。
CCM 增量 **0**（无新 CCM 对象）。

2026-08-24（云南协议接入）追加采样（同口径 `make clean && make -j8`，对照构建同树剔除云南实测增量；数字为同日用户决定 1~10 修订后重采）：

| 段 | 云南基线（剔除云南） | 修订后 | 增量 |
|----|----------------------|----------|------|
| text | 349796 | **352860** | **+3064** |
| data | 1656 | **1656** | **+0** |
| bss（size 工具口径，含 .ccmram） | 136772 | **138196** | **+1424** |

增量构成（对照构建实测；初版 +3120/+1168，修订后 自检单字缓冲 256B 入 bss、default 文件代码移除）：
- 帧 queue 静态体：`s_yn_queue_buf` **801**（3×267，payload 259）+ `s_yn_queue_cb` **80** = 881
- 任务内帧缓冲（static bss）：`msg_buf` **267**
- 自检单字全屏缓冲（static bss）：`_yn_selftest_fill_char` 内 `buf` **256**
- 状态：`s_yn_mask`/`s_yn_mask_rs232`/队列句柄 **12** + 尾部对齐 8
- 任务栈（ucHeap，非 bss）：`yn_handle_task` **1×1KB**（地区协议处理任务 5→6）；`yn_selftest_task`（1KB）为瞬态任务（上电熄灭任务 `yn_bootoff_task` 已随修订移除）
- text +3064：probe/parse/cmd/voice 四文件代码（含自检老化循环显示序列）+ rodata（文明用语 4 串、PROGRAM_CODE 应答、映射表、队列/任务 attr）

SRAM 合计（云南基线 +1424）：**≈129115B（98.5%）**，未超 124KB（126976B）上限。
CCM 增量 **0**（无新 CCM 对象）。

---

## §4 复现

**前提**：`build/Debug/Project_STD.elf` 须为 **EIDE Debug** 产物。若先执行 `make`（编 1-260 + RLS/AH）会覆盖该 elf，本表数字不复现。

```bash
arm-none-eabi-size -A build/Debug/Project_STD.elf | grep -E '^\.(data|bss|ccmram|_user)'
arm-none-eabi-nm -S build/Debug/Project_STD.elf | grep -E 'ucHeap|rb_provide_.*_buf|s_.*_queue_buf|s_rs485_buf|s_rs232'
# ucHeap 须在 0x2000…，勿在 0x1000…
```

---

## 修订

- 2026-08-14：Phase A 后首测（130280 / 38208）。  
- 2026-08-14 14:45：对齐 RB 1536/768/768、DMA 640、queue 深度表；重采 SRAM ≈120820。  
- 2026-08-14 16:37：更正采样口径为 **EIDE Debug**（原误标 Makefile/GCC Debug）；标注 RLS/AH 未编入与 Makefile 重采警告。
- 2026-08-14：新增 §5 四川三协议增量账（Makefile 口径：text +7624 / data +8 / bss +1144；SRAM 124192B=94.7%，未超 124KB 上限；CCM 增量 0；ucHeap 多 4 任务栈 4KB）。
- 2026-08-15：四川三协议显示语义对齐参考项目 9K1F212701（MTC 帧长双格式/治超 8 行与 0x80 全屏/ETC 6 行与 0x30 复位）+ TEST 键修复；重采 Makefile 口径 text +8264 / data +16 / bss +1208（治超行状态 64→128B、颜色状态 +2B）；SRAM 合计 **124264B（94.8%）**，未超上限。
- 2026-08-15（同日修订）：治超帧长度上限 0x1E→FF（对齐 9K1F212701，0xFE 显式排除防 RLS 吞帧）+ `SC_OL_PAYLOAD_MAX` 32→249 + MTC 单行行号 '1'~'4'→'1'~'6'；bss 重采 **125948（+2080）**、SRAM ≈**124915B（95.3%）**；build/Debug 曾发现陈旧 ELF（仅链四川协议），`make clean` 全量重建后恢复。
- 2026-08-15（ETC 全屏修复）：ETC 显示帧变长 0x0D 定界对齐 9K1F212701 etc.c（payload 64→149、全屏数据上限 56→145、全屏渲染先清屏再按屏宽换行）；bss 增量 +344（queue_buf +255、任务帧缓冲 +85、对齐 +4），重采 bss **126292（+2424）**、SRAM ≈**125259B（95.6%）**，未超上限。
- 2026-08-15（治超行帧变长修复）：81~88 行数据改变长（=总长-6，≤24B 截断、不足不补空格、先清行再渲染，对齐 9K1F212701 makefonttolatt_oneline）；`s_ol_lines` 8×16→8×24 + `s_ol_line_len[8]`（+72B）；亮度映射改 (val+1)/32；修复应答帧 BCC 计算 off-by-one；重采 bss **126364（+2496）**、SRAM ≈**125331B（95.6%）**，未超上限。
- 2026-08-17（发布前审查修复）：治超 payload 249→255 消除长帧队列截断越界读（bss +18）；MTC '8' 亮度兼容二进制/ASCII、'A' 4B/5B 判别加 b2 门限；ETC 黄闪 10 秒自动关闭计时（+4B）+ 车道关闭文案补全「请择道行驶」（rodata）+ 滚屏帧 0x0D 扫描起点改索引 6（rt/st 恰为 0x0D 不再误定界）；ETC/MTC 行渲染与 MTC '4'/治超 80 全屏渲染补先清后画（对齐 9K1F212701 MakeSixteenLattAll/OneLine 清屏语义）；ETC 0x40 亮度改 00~07→硬件 1~8（协议文档 §6，00 最暗）；重采 text **328280（+8616）**、bss **126396（+2528）**、SRAM ≈**125363B（95.7%）**，未超上限。
- 2026-08-17（MTC '}' 定界变长修复）：'{' 帧族废除定长双长度与 '78' BCC 校验（删除 BCC 函数与定长 switch），probe 改 '}' 扫描；修复上位机 `{3 1 1234 } 3 }` 单行乱码。queue payload 仍 74B、深度 3 → bss **不变**；text 重采 **327624（-656）**；SRAM ≈**125363B（95.7%）**，未超上限。宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/sc_mtc_frame_sim.py`。
- 2026-08-17（ETC 心跳停用 + MTC '4' 全屏换行修复）：ETC 心跳计时任务 #if 0 停用、不再创建 → ucHeap 任务栈 **-1KB**（4→3）；「ETC车道关闭」5 分钟超时显示与黄闪 10 秒自动关闭一并失效（0A 38 开启后须 0A 39 显式关闭；0A 50 帧解析保留）；MTC '4' 全屏改整屏 word_wrap 渲染（原 16B/行切 ≤4 行，超宽溢出被裁、超 64B 丢弃）；data/bss **不变**（1620/126396），text 重采 **327408（-216）**；SRAM ≈**125363B（95.7%）**，未超上限。
- 2026-08-17（新增山东协议）：ProtocolParser_ShanDong 六文件接入 Makefile（EIDE Debug 目标排除）；§6 增量账：text **+1464**、bss **+1160**（queue 801 + cb 80 + 帧缓冲 267 + 状态 12）、data 不变；ucHeap 任务栈 +1KB（3→4）；SRAM ≈**126523B（96.5%）**，未超 124KB（126976B）上限。
- 2026-08-18（框架/驱动层缺陷修复）：UART RX 单缓冲 IDLE → **乒乓双缓冲 circular**（HT/TC+IDLE 增量冲刷，s_rs485_buf/s_rs232_0_buf 640→1280×2，bss +1280B）；channel_send 与 frame_dispatch_task 增加通道指针回验；W25Qxx 加 SPI 互斥；TCP Server 改串行单客户端、UDP/TCP 通道静态化（+32B）。重采（Makefile 口径）：bss **130036（+1312）**、text **334256（+1184）**、data 不变。
- 2026-08-17（默认显示注册机制 + 山东版本应答 PROGRAM_CODE）：新增 `app_default_display.c`（注册制默认显示，回退欢迎画面）+ `app_sd_proto_default.c`（山东上电画面注册）+ `app_boot.h`（PROGRAM_CODE 共用，开机画面与版本应答）；`'2'` 版本应答改裸 ASCII PROGRAM_CODE（删除占位 `SD_FX_1.0`）；重采 text **329112（+240）**、bss **127564（+8：s_default_fn 4B + 尾部对齐 4B）**、data 不变；SRAM ≈**126531B（96.5%）**，未超上限。
- 2026-08-17（文本常量字节数组 → UTF-8 字符串字面量）：山东/四川ETC 显示文本、四川MTC 固定标签与语音表、青海文明用语表全部改为 UTF-8 字面量（语音发送前 UTF8ToGBK 运行时转 GBK；显示改 FONT_ENC_UTF8）；山东上电画面 COLOR_GREEN→COLOR_YELLOW；青海文明用语 '0' 同时修正为 doc/04 裸机文本「贵州高速公路」（原数组字节有损）；青海自检/费额语音补 UTF8ToGBK 转换（原以 UTF-8 字节直送 GBK 语音板）；重采 text **329456（+344，rodata +149 + 转换代码 ~+195）**、bss **127564（+0）**、data 不变；SRAM ≈**126531B（96.5%）**，未超上限。
- 2026-08-17（dev_display_fill 越界下溢修复）：`dev_display_fill` 起点越界（x≥rows / y≥cols）早退丢弃，消除 `screen_* - x/y` uint16 下溢巨值写穿 pixel_map（CCMRAM）→ HardFault 的缺陷（山东 '3' 行号 ≥3 在小屏构建触发：y=row*16 ≥ 32 屏高；故障地址 0x10010000 = CCMRAM 末尾+1）；该修复同时保护青海/ETC/MTC 行渲染与 app_render 字形擦除路径；重采 text **329472（+16）**、bss/data 不变；SRAM ≈**126531B（96.5%）**，未超上限。
- 2026-08-17（新增贵州协议）：ProtocolParser_GuiZhou 四文件接入 Makefile（EIDE Debug 目标排除，与青海同列）；§6 增量账：text **+3600**、bss **+1160**（queue 801 + cb 80 + 帧缓冲 267 + 状态 12）、data 不变；ucHeap 任务栈 +1KB（4→5）；SRAM ≈**127691B（97.4%）**，未超 124KB（126976B）上限。宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/gz_frame_sim.py`。
- 2026-08-24（新增云南协议）：ProtocolParser_YunNan 五文件接入 Makefile（EIDE Debug 目标排除，与青海/山东/贵州同列；PROTO=CQ 口径保留——`{` 帧族随既有纪律编入）；§6 对照构建增量账：text **+3120**、bss **+1168**（queue 801 + cb 80 + 帧缓冲 267 + 状态 12 + 对齐 8）、data 不变；ucHeap 常驻任务栈 +1KB（5→6），自检/熄灭任务瞬态；SRAM ≈**128891B（98.3%）**，未超 124KB（126976B）上限。构建验证：PROTO=ALL text 352916/data 1656/bss 137940；PROTO=CQ text 339060/data 852/bss 134464，均链接通过。
- 2026-08-24（云南协议修订，用户决定 1~10）：移除 `app_yn_proto_default.c`（上电效果 default 文件，Makefile/eide.yml 同步删除，不再注册默认显示）；'2' 自检改老化循环显示 + 每 5s 语音「系统正在自检」（可被下一帧打断，新增自检单字缓冲 static buf 256B）；'8' 亮度 0x00 恢复光敏自动；0x01 全屏点亮扩 01红~07白 七色；0x02 应答改 PROGRAM_CODE（删硬编码 YN_FX_P5_1.0）。重采：**PROTO=ALL text 352860（-56）/ data 1656 / bss 138196（+256）**；**PROTO=CQ text 338996（-64）/ data 852 / bss 134720（+256）**，均链接通过。
- 2026-08-20（新增重庆CQ协议）：ProtocolParser_ChongQing 四文件 + cJSON 接入 Makefile（恒定编入，CQ 源在 LDI 之后）；`PROTO=CQ` 剔除 LDI 目录 + `-DPROTO_CHONGQING`。Makefile 全协议口径重采：**text 348240（+12056，含 cJSON ~8.5K 代码）**、**data 1652（+32，cJSON hooks/全局指针）**、**bss 136456（+6436 = CCM +6377 + SRAM ~59）**。CQ 静态体构成（**全部置 `.ccmram`**）：queue_buf 3156 + queue_cb 80 + 任务帧缓冲 1052 + JSON 缓冲 1045 + 文本缓冲 1044 = **6377B CCM**；SRAM 净增 ~59B（mask/计数器/hooks/UDP_CQ 通道实例），全协议构建 SRAM 余量仍 >1.7KB，链接通过。ucHeap 任务栈 +2KB（cq_proto_handle_task / cq_proto_timer_task 各 1KB）。cJSON 解析树为 ucHeap 瞬时分配（钩子 pvPortMalloc/pvPortFree）。**CCM 例外说明**：本项目 CCM 惯例为「仅显存」，CQ 帧队列因 SRAM 余量不足（全协议构建仅余 ~1.9KB）破例入 CCM——缓冲为 CPU 独占访问（无 DMA/ETH），Makefile 1-260 口径 CCM 2432+6377=8809B / EIDE 1-577 口径 38208+6377=44585B，均不超 64KB。
- 2026-08-20（cJSON 升级官方 v1.7.18）：`Middlewares/Third_Party/cJSON/` 由裸机拷贝 v1.x 老版（编译 3 条 -Wmisleading-indentation 警告）替换为 DaveGamble/cJSON **v1.7.18** 官方源（GitHub codeload tarball）；parse 层类型判定改 `cJSON_IsString/IsNumber/IsObject` 辅助函数（`app_cq_proto_parse.c`），`cJSON_InitHooks` 换绑 FreeRTOS 堆不变（hooks 字段名仍 malloc_fn/free_fn），`cJSON_Parse`/GBK 字节直传用法不变。重采（Makefile 口径，cJSON 编译 warning 清零，仅余 HAL flash_ex 3 条既有 unused-parameter）：全协议 text **350004（+1764，cJSON 代码 ~8.5K→~10.2K）**、data **1656（+4）**、bss **136460（+4）**；`PROTO=CQ` text **335796（+1772）**、data **852（+4）**、bss **132984（+4）**。cJSON 解析树瞬时堆仍 ucHeap 内 2~4KB 量级（cJSON 结构体尺寸不变，钩子仍 pvPortMalloc/pvPortFree，解析后 cJSON_Delete 归还）。
