# 多协议共享 RB — 设计规范

> **状态**：现行　|　2026-08-14  
> **落地对照**：[02_as_built_status.md](./02_as_built_status.md)  
> **内存位置**：RB 体默认 SRAM、堆/CCM 归属见 [`../06_SRAM内部分数据迁移/02_memory_policy.md`](../06_SRAM内部分数据迁移/02_memory_policy.md)

---

## 1. 验收目标

| 编号 | 验收项 | 判定标准 |
|------|--------|----------|
| A-1 | 单固件多地区协议 | 同一镜像可同编多个兼容解析器（如 LDI + 青海 + RLS） |
| A-2 | 工程目录选编 | EIDE/Makefile 包含/排除协议目录；不以 `APP_PROTOCOL` 宏为主 |
| A-3 | 启动自注册 | `sw_app_initcall` → acquire / register / bind / 任务；`app_boot` 无协议 `#ifdef` |
| A-4 | 同通道链式解析 | 绑定同通道的协议共享物理 RB，链式 probe，匹配者消费 |
| A-5 | USART6 语音专用 | 禁止任何协议 `bind(CH_ID_RS232_1)`；TTS → `PL_UART6` 旁路 |
| A-6 | 单协议退化 | 只编入一个协议目录时行为退化为单 probe（weak 无他提供者） |

非功能：

| 编号 | 需求 |
|------|------|
| NF-1 | 多协议带来的 FreeRTOS 堆压力可控（静态队列/控栈；结构见 06） |
| NF-2 | **不同物理口**不共享可并发写入的 RB |
| NF-3 | 帧头冲突组合不得同通道同 RB 同时启用 |
| NF-4 | 网口逻辑通道默认共享 RJ45；若需真并发高压隔离，再拆逻辑 RB（见 §2.4） |

---

## 2. 核心决策

### 2.1 一物理通道一 RB

| 物理口 | RB 槽 | 容量 | 逻辑通道（可多） |
|--------|-------|------|------------------|
| ETH（RJ45） | `RB_SLOT_RJ45` | **1536** | TCP_SERVER / TCP_CLIENT / UDP / MQTT |
| USART1 RS485 | `RB_SLOT_RS485` | **768** | `CH_ID_RS485` |
| USART3 RS232 | `RB_SLOT_RS232` | **768** | `CH_ID_RS232` |
| USART6 | — | — | **不进 RB**（语音 TX 旁路） |

规则：

- 同物理口多协议 → **共享**该槽 RB，链式 probe 区分帧。
- 不同物理口 → **独立** RB，禁止混写。
- `app_channel_dispatch`：同 RB 指针 `seen[]` 去重，每包只写一次。
- `frame_dispatch_task`：外循环按 RB 去重；内循环同 RB 上协议链式探测。

```
通道 RX → app_channel_dispatch → 写入物理 RB（去重）
       → ch_queue → frame_dispatch_task
       → 链式 probe → READY → frame_queue[协议]
```

### 2.2 编译期按需（`RB_PROVIDE_WEAK`）

- 协议 TU 内：`RB_PROVIDE_WEAK(rb_provide_xxx, RB_SIZE_xxx)`。
- 多 TU 同名 weak → 链接保留一个 getter 及 static 体。
- 未编入任何提供者 → weak 为 0 → `acquire` 返回 nullptr（不占缓冲）。
- 体落默认 `.bss`（SRAM）；**不**再使用调度路径上的 `RB_DEFINE_CCM` 八槽池。

### 2.3 协议模块组织

| 原则 | 说明 |
|------|------|
| 目录即模块 | `LDI/`、`ProtocolParser_QingHai/`、`RLS/`、`IAP/` … |
| 自注册 | `sw_app_initcall`；四步：acquire → register → bind → `osThreadNew` |
| 多逻辑通道 | **每逻辑通道独立 mask + bind**；共享同一物理 RB；禁止「单 mask 绑多通道」 |
| 框架零分支 | `app_dispatch` 无地区 `#ifdef` |

### 2.4 网口共享 RJ45 的风险与门禁（NF-4）

TCP 为流、UDP 为报；多通道任务均可对 **同一** RJ45 RB 调用 `dispatch`。互斥保证单次 `rb_write` 原子，但**不同套接字的包会在环缓中交错排列**。

| 场景 | 建议 |
|------|------|
| 产品以单网口业务为主（常见：UDP IAP/LDI 或 TCP 二选一） | **维持共享 1536**（省 RAM） |
| TCP 与 UDP/MQTT **同时高压**收包 | 评估拆逻辑 RB，或产品互斥启通道 |

本阶段默认维持共享；变更须同步改 `02` 绑定表与 `06` 占用账。

### 2.5 硬件映射

| PL | 口 | CH_ID | 用途 |
|----|-----|-------|------|
| UART1 | RS485 | `CH_ID_RS485` | 地区协议链式 |
| UART3 | RS232 | `CH_ID_RS232` | 地区协议链式 |
| UART6 | TTL | `CH_ID_RS232_1` | **仅语音 TTS**；禁止协议 bind |
| ETH | — | TCP_S / TCP_C / UDP / MQTT | 共享 `RB_SLOT_RJ45` |

---

## 3. 链式探测契约（A-4）

### 3.1 WAIT / FAKE 决策

```
READY / SKIP → 消费后结束本轮，继续取下一帧
WAIT         → any_wait，继续试下一协议
FAKE         → any_fake，继续试下一协议

无一消费时：
  if (any_wait)       break;       /* 禁止 skip */
  else if (any_fake)  rb_skip(1);  /* 重同步 */
```

### 3.2 probe 强制规则

1. 只 `rb_peek`；禁止 probe 内 `rb_read` / `rb_skip` / 写业务全局  
2. **首字节快拒**：不匹配立刻 FAKE；禁止「长度不够就一律 WAIT」  
3. 仅 READY/SKIP 写 `*total_len` / `*aux`  
4. 无阻塞、无副作用、幂等  
5. **长度/缓冲边界**：对 length 做上下界；`rb_peek` 按目标缓冲截断  

### 3.3 帧头事实

| 协议 | 特征 |
|------|------|
| IAP | `0x5A5A5A5A` + CRC32 |
| LDI | STX `0xFF 0xFF` + CRC16-XMODEM |
| RLS | `0xFF 0xFE` … `0x0D 0x0C` |
| 青海 | `{` … `}`（弱校验） |
| 山东 | `{` + 命令字('1'~'5','7','8') + 二进制 len + `}`（与青海同构、无 BCC；'3'/'4'/'5' 语义与青海一致，'1'/'2'/'7'/'8' 语义分歧） |
| 贵州 | `{` + 命令字('1'~'9','A','B',0x01,0x02) + 二进制 len + `}`（与青海完全同构、无 BCC；13 命令含 0x01 全屏点亮 / 0x02 版本号两个二进制命令字） |
| 云南 | `{` + 命令字('1'~'9','A','B',0x01,0x02) + 二进制 len + `}`（与青海/贵州完全同构、无 BCC；13 命令，'6'=单行清除（贵州为固定格式）、0x02 版本号应答 PROGRAM_CODE） |
| 四川 ETC | `0x0A` + 命令位(00/01/36~39/40/50) … `0x0D`（46 归属 MTC） |
| 四川 MTC | `{`(0x7B) + 命令 + 参数 + `}`（**'}' 定界变长**，无长度字段、无 BCC，扫描上限 74B，9K1F212701 语义）；`0A 46 0A` / `0A 46 0D` 双帧型 |
| 四川治超 | `0xFF` + 长度(07~FF，0xFE 排除) … `0xFF`（BCC 异或）；长度上限对齐 9K1F212701 容纳 0x80 全屏长数据段，0xFE 显式排除与 RLS `FF FE` 区分；81~88 行数据变长（=总长-6，≤24B 截断） |
| 重庆CQ | JSON `{` 花括号深度定界（'"' 内跳过，深度>16/累计>1044 → FAKE）+ `FF FF` 12B 二进制精确匹配（重启/搜索两帧），CRC16-XMODEM（大端，帧内校验）；业务口 UDP_CQ(20103) + 搜索口 UDP(10011) |

---

## 4. RB / 绑定规划（与代码一致）

| 槽 | 尺寸 | 提供方（weak） | bind 通道 | 协议 |
|----|------|----------------|-----------|------|
| RJ45 | 1536 | IAP / LDI / AH_MQTT | UDP | IAP |
| RJ45 | 同上（共享） | 同上 | TCP_S / TCP_C / UDP | LDI |
| RJ45 | 同上 | 同上 + CQ | MQTT | AH_MQTT（initcall 可关） |
| RJ45 | 同上 | CQ（weak 合并） | UDP_CQ / UDP | 重庆CQ（双 mask 共用一队列；CQ 源收录序在 LDI 之后） |
| RS485 | 768 | QH / RLS / SC_ETC / SC_MTC / SC_OL / SD / GZ / YN | RS485 | 青海、RLS、四川三协议、山东、贵州、云南 |
| RS232 | 768 | QH / SC_ETC / SC_MTC / SC_OL / SD / GZ / YN | RS232 | 青海、四川三协议、山东、贵州、云南 |

**已取消（相对历史 8 槽方案）**：

- IAP 占用 RS485 / 「系统独立 RB」双写  
- LDI 绑定 RS485 / RS232  
- 每逻辑网口独占 2KB CCM 槽  

### 4.1 协议帧 queue 深度（静态，SRAM）

一协议一队列（LDI 三逻辑通道 mask 共用 `g_ldi_msg_queue`）。与 RB/DMA 正交；加深 queue 不必联调 RB。

| 协议 | 宏 | 深度 | 单槽约 | 静态体约 | 说明 |
|------|-----|------|--------|----------|------|
| IAP | `IAP_QUEUE_DEPTH` | **2** | ~1052 | ~2104 | 停等升级模型够用 |
| LDI | `LDI_QUEUE_DEPTH` | **4** | ~520 | ~2080 | 一包多帧粘包（≥3） |
| 青海 | `QH_QUEUE_DEPTH` | **3** | ~267 | ~801 | payload 上限 259 |
| RLS | `RLS_QUEUE_DEPTH` | **2** | ~538 | ~1076 | |
| AH_MQTT | `AH_MQTT_QUEUE_DEPTH` | **3** | ~541 | ~1623 | initcall 可关；任务内 static |
| 四川 ETC | `SC_ETC_QUEUE_DEPTH` | **3** | 157 | **471** | payload 149（帧 ≤149，0x0D 定界，参考 9K1F212701） |
| 四川 MTC | `SC_MTC_QUEUE_DEPTH` | **3** | 82 | **246** | payload 74（'}' 定界扫描上限；'4' 全屏 ≤68B / '78' 语音 ≤74B） |
| 四川治超 | `SC_OL_QUEUE_DEPTH` | **3** | 263 | **789** | payload 255（=长度字段上限 FF，覆盖 0x80 全屏长数据段） |
| 山东 | `SD_QUEUE_DEPTH` | **3** | ~267 | ~801 | payload 259（len 字段 1B 上限，与青海同构） |
| 贵州 | `GZ_QUEUE_DEPTH` | **3** | ~267 | ~801 | payload 259（与青海同构，13 命令含 0x01/0x02） |
| 云南 | `YN_QUEUE_DEPTH` | **3** | ~267 | ~801 | payload 259（与青海/贵州同构，13 命令含 0x01/0x02；'3' 单行 FONT_24 渲染） |
| 重庆CQ | `CQ_QUEUE_DEPTH` | **3** | 1052 | **3156** | payload 1044（JSON 花括号定界上限）；队列体+任务帧缓冲+JSON/文本缓冲**置 CCMRAM**（SRAM 全协议构建仅余 ~1.9KB，CPU 独占访问无 DMA） |

变更记录（2026-08-14）：LDI 2→4、青海对齐协议后深度 3、AH 1→3；IAP/RLS 维持 2。
变更记录（2026-08-14）：新增四川三协议（ETC/MTC/治超）各深度 3。
变更记录（2026-08-15）：四川 ETC payload 64→149（0x0D 定界上限对齐 9K1F212701 etc.c，全屏数据无固定 56B）。
变更记录（2026-08-17）：四川治超 payload 249→255（=长度字段上限 FF，消除 250~255 长帧被队列截断的越界读）；MTC '8' 亮度参数兼容二进制 0~8 与 ASCII '0'~'8' 双格式；MTC 'A' 4B/5B 判别加 b2≤2 门限，消除颜色帧 BCC='}' 误判。
变更记录（2026-08-17）：MTC '{' 帧族改「'}' 定界变长」扫描（对齐 9K1F212701 mtc.c 逐字节扫 '}'），废除每命令定长双长度（'3'→20/21B 等）与 '78' BCC 校验；字段偏移按变长重算（'3' 文本=raw_len-4、'4'=raw_len-3、'6' 数字串=raw_len-4、'78' 文本=raw_len-4）；修复上位机 `{3 1 1234 } 3 }`（10B）单行乱码（定长 probe 跨帧拼凑 20B 认领含 '}'/'{' 垃圾文本）；payload 上限仍 74B（队列约束）。宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/sc_mtc_frame_sim.py`。
变更记录（2026-08-17）：新增山东协议（SD）深度 3、payload 259（与青海同构；命令字 '1'~'5','7','8'，无 '6'）。
变更记录（2026-08-17）：新增贵州协议（GZ）深度 3、payload 259（与青海同构；命令字 '1'~'9','A','B' + 0x01/0x02 二进制命令字）。
变更记录（2026-08-24）：新增云南协议（YN）深度 3、payload 259（与青海/贵州同构；命令字 '1'~'9','A','B' + 0x01/0x02；'6'=单行清除、'3'/'4' 按 24 点阵 FONT_24 渲染、0x02 应答 PROGRAM_CODE、无上电效果 default 文件）。
变更记录（2026-08-24 修订，用户决定 1~10）：YN 语义定稿——'2' 自检改老化循环显示 + 每 5s 语音「系统正在自检」（可被下一帧打断）；'3'/'6' 行号 '1'~'5' 全接受（行 5 执行但不落屏）；'8' 亮度 0x00=恢复光敏自动、'1'~'8' 手动档；0x01 全屏点亮扩至 01红~07白 七色；0x02 应答 PROGRAM_CODE；移除上电效果 default 文件（不注册默认显示）。
变更记录（2026-08-20）：新增重庆CQ（JSON `{` + 12B 二进制）深度 3、payload 1044、静态体 3156B **置 CCMRAM**；与 IAP/LDI 共享 RJ45 RB，CQ 源收录序在 LDI 之后（probe 注册序）。
变更记录（2026-08-21）：CQ 业务端口语义修复——`PROTO_CHONGQING` 才读 Sector1 net_cfg.port（无效/0 回退 20103），dev 共存构建固定 20103（不读 LDI 语义的 net_cfg.port）；搜索应答 port 字段改报 `app_udp_cq_get_port()`；10011 口 12B 帧「LDI 先探测、FAKE 放行、CQ 收单」实态修正（见 §6 兼容矩阵与 doc/03 PartB B.11）。
变更记录（2026-08-21 方案 B）：Sector1.net_cfg 新增 `udp_port`（CQ UDP 业务口专有），TCP/UDP 端口彻底分离——`PROTO_CHONGQING` 改读 net_cfg.udp_port（setip 写入，默认 20103），dev 共存构建固定 20103；net_cfg.port 恒为 TCP 业务口不再被 setip 污染（见 doc/03 PartB B.7.1/B.11、doc/07 §15）。
---

## 5. 协议模块模板

```c
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);

void xxx_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr) return;
    proto_mask_t m = app_proto_register(xxx_probe_frame, rb);
    if (m == 0) return;
    app_proto_bind_channel(m, CH_ID_RS485);
    /* 多逻辑通道 → 再 register + bind；禁止 CH_ID_RS232_1 */
    osThreadNew(xxx_handle_task, ...);
}
sw_app_initcall(xxx_proto_init);
```

语音：`dev_rs232_voice_*` → `pl_uart_send(PL_UART6)`，旁路 dispatch。

---

## 6. 兼容矩阵（合入门禁）

| 组合 | 同物理 RB | 建议 |
|------|-----------|------|
| LDI + 青海 | 不同物理口（RJ45 vs RS485） | 允许 |
| LDI + RLS + 青海（RS485 上后两者） | RS485 链式 | 允许（帧头可分） |
| 青海 + 重庆 JSON | **禁止**同 RS485 | 同分 `{` |
| LDI + 重庆 BIN | 高风险 | 同 `FFFF`，须更强 probe 或互斥 |
| 任意协议 + `CH_ID_RS232_1` | **禁止** | 语音专用 |
| 四川 MTC + 青海（`{` 帧头重叠） | RS485/RS232 链式 | ⚠️ **EIDE 目录排除纪律**：完整青海帧由 QH probe 先认领（qh_proto_init 先注册，源码收录序 QH 在前）；MTC probe '}' 定界变长扫描（上限 74B，2026-08-17 对齐 9K1F212701）对「青海帧数据段含 0x7D（GBK 尾字节可命中）且半帧到达」存在截断认领残余风险，依赖排他编译，量产必须二选一 |
| 四川治超 + RLS（`FF` 帧头重叠） | RS485 链式 | 允许：治超第二字节=长度 07~FF 但**显式排除 0xFE(254)**，RLS 固定 0xFE，双向第二字节快拒成立（2026-08-15 长度上限对齐 9K1F212701 后必须显式排除，否则治超 probe 会把 RLS 帧当长帧 WAIT 卡死） |
| 四川 ETC + 四川 MTC（`0A` 帧头重叠） | RS485/RS232 链式 | 允许：ETC 命令位 ∈ {00,01,36~39,40,50}，MTC 仅 46（0A 46 0A/0D），命令位互斥 |
| 山东 + 青海（`{` 帧族完全同构） | RS485/RS232 链式 | ⚠️ **EIDE 目录排除纪律**：山东命令字 '1'~'5','7','8' 全部落入青海 probe 命令集（'1'~'9','A','B'），全协议构建下青海 probe 先认领（源码收录序 qh 在前）；'3'/'4'/'5' 语义巧合一致，'1'/'2'/'7'/'8' 语义分歧（山东 '1'=全屏单色 vs 青海 '1'=主机查询、'2'=版本 vs 自检、'7'=亮度 vs 文明语音、'8'=外设 vs 亮度），量产必须二选一 |
| 山东 + 四川 MTC（`{` 帧头重叠） | RS485/RS232 链式 | ⚠️ 同青海行纪律：MTC '}' 定界变长扫描会认领山东帧，量产互斥 |
| 贵州 + 青海（`{` 帧族完全重叠） | RS485/RS232 链式 | ⚠️ **EIDE 目录排除纪律**：贵州命令字 '1'~'9','A','B' 全部落入青海 probe 命令集，全协议构建下青海 probe 先认领（源码收录序 qh 在前）；'1'/'3'/'4'/'5'/'7'/'8'/'9'/'B' 语义基本一致（'7'=文明语音、'8'=亮度、'9'=音量、'B'=费额语音），'2'/'6'/'A' 细节差异（贵州 '2'=自检黄屏+语音、'6'=固定格式行文与青海不同、'A'=红优先 vs 青海同红优先但贵州语义自协议文档），量产必须二选一 |
| 贵州 0x01/0x02（二进制命令字） | RS485/RS232 链式 | 允许：0x01 全屏点亮 / 0x02 版本号为二进制命令字，不属于青海/山东/四川MTC 的 ASCII 命令集，各 probe 首命令字快拒，贵州 probe 无冲突认领 |
| 云南 + 青海（`{` 帧族完全重叠） | RS485/RS232 链式 | ⚠️ **EIDE 目录排除纪律**：云南命令字 '1'~'9','A','B' 全部落入青海 probe 命令集，全协议构建下青海 probe 先认领（源码收录序 qh 在前）；与贵州同构但 '6' 单行清除（贵州固定格式）、'8' 0x00自动/1~8 档（贵州 0~5 档）语义差异，量产必须二选一 |
| 云南 0x01/0x02（二进制命令字） | RS485/RS232 链式 | 允许：0x01 全屏点亮 / 0x02 版本号为二进制命令字，不属于青海/山东/四川MTC 的 ASCII 命令集，各 probe 首命令字快拒，云南 probe 无冲突认领 |
| `{` 帧族任意两两（QH/SD/GZ/SC_MTC/YN） | RS485/RS232 链式 | ⚠️ **编译期互斥守卫**：五个协议主文件各定义同名强符号 `g_brace_proto_guard`（`__attribute__((used))` 防 `--gc-sections` 丢弃，`#ifndef STD_ALL_PROTO` 包裹）——EIDE 量产构建多个编入即链接报 `multiple definition of 'g_brace_proto_guard'`，无法绕过；Makefile 全协议开发构建经 `-DSTD_ALL_PROTO` 豁免共存（probe 注册序与本文纪律约束）。EIDE excludeList 目录排除纪律仍为操作层首选 |
| 重庆CQ + LDI | RJ45（UDP/UDP_CQ/TCP 共享） | ⚠️ **产品互斥**（Makefile `PROTO=CQ` 剔除 LDI 目录 / EIDE excludeList 目录排除）；全协议 dev 构建下二者共存于 RJ45：CQ JSON `{` 与 LDI `FF FF` 首字节互斥可分；**10011 口 12B 二进制帧（CQ 重启/搜索请求）先经 LDI probe：CQ 12B 帧 len=00 00 00 02 且 CRC16-XMODEM 恰好通过 LDI 校验，LDI probe 对 `data_len==2` 显式 FAKE 放行（2026-08-21 修复，此前沿身份校验路径 SKIP 吞帧致 CQ 12B 在 10011 无响应），CQ probe 仍认领——「LDI 先探测、FAKE 放行、CQ 收单」，无功能受限**。另：dev 共存构建 CQ 业务口固定 20103、不读 Sector1 net_cfg.udp_port（dev 构建无写入方；方案 B 2026-08-21 起 CQ 构建读 udp_port、TCP 口 port 不再被 setip 污染），见 doc/03 PartB B.11 |
| 重庆CQ + IAP | RJ45（UDP 共享） | 允许：CQ 首字节 `{` 或 `FF FF`+12B 精确匹配，IAP 首字节 `0x5A` 快拒；CQ probe 对非匹配 `FF` 前缀 FAKE，互不误伤 |
| 重庆CQ + `{` 帧族（QH/SD/GZ/SC_MTC） | 不同物理口（RJ45 vs RS485/RS232） | 允许：不同 RB 槽不冲突；CQ 非串口 '{' 帧族协议，**不定义** `g_brace_proto_guard`（守卫仅约束串口 '{' 帧族四协议） |

> 说明：`{` 帧族互斥由编译期守卫在链接期兜底强制；excludeList 目录排除为操作层首选（编译更早失败、意图明确）。Makefile 全协议构建（`-DSTD_ALL_PROTO`）下守卫失效，共存风险按上表各行的 probe 注册序纪律管控。

---

## 7. 与 RAM 文档的边界

- RB **行为**以本文件为准；RB **放 SRAM**、堆回退、显存占 CCM → **仅 06**。  
- 多协议任务栈挤堆的历史问题 → [03](./03_freertos_heap_side_effect.md)，结构方案不在本目录改代码。

---

## 8. 非目标

- 不刷机动态下载未编入的解析器  
- 不保证帧头冲突协议在同一字节流上零误判  
- 不把 `APP_PROTOCOL` 作为主切换手段  
