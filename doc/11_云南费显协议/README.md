# 11 云南费显协议

> 云南常规费显协议（01 云南常规费显协议-云南LED费显P5，2022.7.5，云南弥玉项目 2022-S134）接入记录。
> 模块：`Application/Src/ProtocolParser_YunNan/` + `Application/Inc/ProtocolParser_YunNan/`（前缀 `yn_`）。

## 1. 协议事实（13 命令）

帧格式：`'{'`(0x7B) + 命令字(1B) + 参数长度(1B 二进制) + 参数(变长) + `'}'`(0x7D)，**无校验**。帧总长 = len + 4，len 为二进制字节值（非 ASCII）。传输：串口（RS232/RS485），9600~115200bps（默认 9600，8N1，本设备由 DIP1 统一选择 9600/115200）。

| 命令字 | 名称 | 参数约束（最终实现） | 行为 |
|---|---|---|---|
| `'1'`(0x31) | 主机查询 | len==0 | 回「正常」应答 `7B 31 01 00 7D` 至**源通道**（恒回正常，仅一种应答） |
| `'2'`(0x32) | 自检 | len==0 | **老化循环显示**（复用出厂测试老化序列）+ 每 5s 语音「系统正在自检」；可被下一帧命令打断 |
| `'3'`(0x33) | 单行显示 | len 3~18；color '0'~'2'（红/绿/黄）；row '1'~'5'（→0~4） | **FONT_24** 渲染（协议 24 点阵），先清行再渲染，GBK 直通，超宽截断；行 5「执行但不落屏」 |
| `'4'`(0x34) | 全屏可编辑 | len 4~86；color '0'~'2'；x/y 1B | 先清屏，按 **X/Y 参数坐标**渲染（word_wrap=true，FONT_24）；0x0A 换行、0x0D 跳过 |
| `'5'`(0x35) | 全屏清除 | len==0 | 整屏黑 + commit |
| `'6'`(0x36) | 单行清除 | len==1；row '1'~'5' | 清第 row 行（行高 24px）；行 5 同「执行但不落屏」 |
| `'7'`(0x37) | 礼貌用语语音 | len==1；idx '0'~'3' | 4 条文明用语 → TTS（协议原文文本） |
| `'8'`(0x38) | 亮度 | len==1；**0x00=NUL=自动亮度** / ASCII '1'~'8'=手动档（8 最亮） | 0x00 → 恢复光敏自动调光；'1'~'8' → 挂起光敏任务 + 硬件档恒等映射 1~8 |
| `'9'`(0x39) | 音量 | len==1；'1'~'5' | 语音板音量 {1,3,5,7,9}（沿贵州实测映射） |
| `'A'`(0x41) | 外设 | len==1；bit0 绿/bit1 红/bit2 黄闪 | **红优先**（沿贵州裁决：红→车道灯红；否则绿→车道灯绿；黄闪独立） |
| `'B'`(0x42) | 收费金额语音 | len≥1；金额 ASCII 串（整数/小数） | **0 元不播**；「您好请交费X元」，小数播小数（末位 0 剔除：123.4→「123.4」）；**不显示任何内容** |
| `0x01` | 全屏点亮（二进制命令字） | len==1；DATA0=0x01~0x07（见 §8 决定 10 扩展取值表） | 整屏 `dev_display_fill` + commit |
| `0x02` | 获取版本号（二进制命令字） | len==1 | 回裸 ASCII **PROGRAM_CODE**（固件程序编码，非硬编码 YN_FX_P5_1.0） |

金额运算整数运算（分），无浮点：`yn_amount_to_fen`（parse.c）整数部分×100 + 小数前两位（`%.2f` 语义）。

## 2. 上电效果

**无**。本协议**不注册默认显示内容**（用户决定 8）——上电使用固件默认显示（`app_default_display` 回退欢迎画面），协议文档 §0 的「上电显示祝您一路平安 + 稍候熄灭」不实现，`app_yn_proto_default.c` 已移除（Makefile / `.eide/eide.yml` 条目同步删除）。

## 3. 模块设计与接入点

- 目录/命名：`ProtocolParser_YunNan/`，前缀 `app_yn_`；文件拆分与青海/贵州同构（proto / proto_parse / proto_cmd / proto_voice，**无 proto_default**）。
- 注册五步：`RB_PROVIDE_WEAK`（RS485+RS232 同槽 weak 合并）→ `app_proto_acquire_buf` → `app_proto_register`（双通道各独立 mask）→ `app_proto_bind_channel`（CH_ID_RS485 + CH_ID_RS232）→ `app_proto_set_frame_queue`；`sw_app_initcall(yn_proto_init)`。
- probe：**长度字段定界**（len 字段 + 尾字节 `'}'` 校验），PURE 契约（仅 rb_peek）；命令字预筛 '1'~'9','A','B',0x01,0x02 否则 FAKE；首字节非 `'{'` 快拒。0x01/0x02 控制字符命令字与青海/山东/四川MTC 的 ASCII 命令集互斥，各 probe 首命令字快拒无冲突。
- 队列：`YN_PAYLOAD_MAX 259`、`YN_QUEUE_DEPTH 3`（静态 SRAM 801B + cb 80B）；`_Static_assert` ≤ RB 768。
- 处理任务：`yn_handle_task` 栈 256×4；帧缓冲 static；parse 纯函数 → `yn_execute_cmd` 查表分派。
- 应答：'1' 主机查询、0x02 版本号经 `channel_send` 回源通道（串口通道 `ch->ops->send` → `pl_uart_send`，RS232/RS485 均支持 TX）；其余命令单向不回。
- 自检：一次性任务 `yn_selftest_task`（栈 256×4，`s_yn_selftest_running` 防重入；`s_yn_selftest_abort` 由 `yn_execute_cmd` 在下一帧有效命令到达时置位，任务分片（100ms）检测后退出且**不清屏**）。
- 波特率：无协议改波特率命令，沿用 DIP1（9600/115200）全局选择。

## 4. 与既有模块的差异（对照贵州，同构帧族）

| 项 | 贵州（GZ） | 云南（YN） |
|---|---|---|
| '2' 自检 | 黄色全屏 + 语音（实测裁决） | **老化循环显示序列**（复用出厂测试，字号×字体循环逐字全屏）+ 每 5s 语音「系统正在自检」，可被下一帧打断 |
| '3' 单行字号 | FONT_16（5 行全容纳） | **FONT_24**（协议 24 点阵；96px 屏高只容纳 4 行，行 5 按协议接受、执行但不落屏） |
| '6' | 固定格式显示（客/货行文） | **单行清除**（语义不同） |
| '8' 亮度 | '0'~'5'（0=恢复光敏自动） | **0x00（NUL）=恢复光敏自动、'1'~'8' 手动档**（8 最亮） |
| '7' 文明用语 | 贵州文案（'0' 实测修正） | 协议原文云南文案 |
| 'B' 费额语音 | 金额 ≥0.5 元播报，.00 只播整数 | **0 元不播**；小数播小数（末位 0 剔除） |
| 0x01 全屏点亮 | 01红/02绿/03黄 | **01红~07白 七色**（用户决定 10 扩展） |
| 0x02 版本号 | 回 PROGRAM_CODE + 屏幕红字显示（实测附加行为） | 回 **PROGRAM_CODE**，无屏幕显示 |
| 上电画面 | 不建 default 文件（裁决） | **不建 default 文件**（用户决定 8：不注册默认显示，使用固件默认显示） |

## 5. 帧头冲突纪律

- 云南 ASCII 命令集 `'1'~'9','A','B'` 与青海 probe 命令集**完全重叠**（同 `{` 帧族、len 字段同构）：全协议构建下青海 probe 先注册（源码收录序 qh 在前）先认领，云南 probe 仅承接青海 FAKE 的帧（与贵州处境一致）。
- **量产 EIDE 目录排除纪律**：云南与青海/山东/贵州/四川MTC 互斥（`.eide/eide.yml` Debug 目标已启用四川MTC、排除青海/山东/贵州/云南；云南量产目标须启用云南并排除其余 `{` 帧族）。
- **编译期互斥守卫**：云南主文件携带 `g_brace_proto_guard`（`#ifndef STD_ALL_PROTO` 包裹、`__attribute__((used))`）——EIDE 量产构建多个 `{` 帧族编入即链接报 `multiple definition`，无法绕过；Makefile 全协议开发构建经 `-DSTD_ALL_PROTO` 豁免共存（probe 注册序纪律约束）。
- 0x01/0x02 二进制命令字不属于青海/山东/四川MTC 的 ASCII 命令集，各 probe 首命令字快拒 → 云南 probe 可无冲突认领。

## 6. 构建口径

- **Makefile PROTO=ALL（dev）**：编入（`SRC_APPLICATION` 收录序在贵州之后、`app_uart_baud.c` 之前，probe 注册序为 `{` 帧族末位）。
- **Makefile PROTO=CQ**：**仍编入**——现有纪律 PROTO=CQ 仅剔除 LDI 目录并追加 `-DPROTO_CHONGQING`，`{` 帧族（青海/山东/贵州/四川MTC）均保留 + `-DSTD_ALL_PROTO` 豁免，云南同属 `{` 帧族随同编入（不改「排除 LDI ↔ PROTO_CHONGQING」不变量）。
- **EIDE Debug（PROTO_CHONGQING 量产口径）**：云南**排除**（excludeList 加 `Application/protocol/ProtocolParser_YunNan`）——Debug 已启用四川MTC（`{` 帧族守卫生效），云南若编入即 `multiple definition`。Inc 目录已入 incList（与青海/山东/贵州同列，供后续云南量产目标启用）。
- **EIDE Release**：已知错配未修，本次不动。
- 无新增编译开关 → `doc/构建开关总表.md` 不动。

## 7. as-built 状态

- **构建验证（2026-08-24 修订，用户决定 1~10 后重采）**：
  - `make clean && make -j8`（PROTO=ALL）：通过，零告警（仅 HAL flash_ex 3 条既有 unused-parameter）。text **352860** / data **1656** / bss **138196**。
  - `make clean && make -j8 PROTO=CQ`：通过（同上 3 条既有告警）。text **338996** / data **852** / bss **134720**。
  - 对照初版（含 default 文件）增量变化：bss **+256**（自检单字缓冲 static buf）、text **-56/-64**（default 文件代码移除、自检序列代码加入抵消）。ucHeap 常驻任务栈 +1KB（yn_handle_task）；自检任务（1KB）为瞬态。CCM 增量 0。SRAM 未超 124KB 上限。
- EIDE 构建未执行（EIDE 需 GUI「重新加载项目」后构建；本模块在 Debug 目标已排除，不影响现有 EIDE Debug 构建）。**注意**：`.eide/eide.yml` 已外部编辑（移除 default 文件条目），须在 EIDE 中「Reload Project」后再做任何会触发保存的 GUI 操作。
- 未烧录。

## 8. 已确认决定（原待确认清单，2026-08-24 用户裁定）

| # | 问题 | 用户决定（已实现） |
|---|---|---|
| 1 | 绑定通道 | RS232/RS485 **双绑**（现状已确认） |
| 2 | 0x33 行号 1~5 与 24 点阵的映射（96px 屏高只容 4 行） | FONT_24 不变；行号范围**以协议为准 '1'~'5' 全接受**。行 5 物理不显示但解析不丢弃不拒绝——渲染调用照常发出，越界由渲染层（dev_display_fill / draw_bitmap 起点越界早退）自然处理，即「执行但不落屏」（parse.c 注释 + cmd.c `_yn_clear_row`/`_yn_exec_one_line` 注释） |
| 3 | '2' 自检显示序列/语音 | 复用 `app_factory_test.c` 263-299 行「老化循环显示」逻辑：整屏单字居中，字号 {FONT_16/24/32}×字体 {ST/FS/KT/HT} 循环「重庆创迪科技发展有限公司设备老化测试」；**每 5s 播报「系统正在自检」**（与循环显示并行）；可被下一帧命令打断（`s_yn_selftest_abort`，退出不清屏）；防重入保留；不切换车道灯、不破坏出厂测试任务/状态 |
| 4 | 'B' 费额语音文案 | 「您好请交费X元」+ 小数播小数、末位 0 剔除（核对确认：123.4→「123.4」、123.40→「123.4」、123→「123」，`yn_voice_fee_amount` 已实现） |
| 5 | '8' 亮度参数语义 | 参数 1B：**0x00（NUL）=自动亮度**（恢复光敏任务 `osThreadResume`）；**ASCII '1'~'8'（0x31~0x38）=手动档 1~8**（挂起光敏任务 + 硬件档恒等映射，8 最亮）。probe/parse 按二进制 len 定界，0x00 参数不会被当字符串终结符或错误 |
| 6 | 'A' 外设红绿同置优先级 | **红优先**（现状已确认，沿贵州裁决） |
| 7 | 0x02 版本号应答 | 回固件 **PROGRAM_CODE**（`app_boot.h` 编译期常量，与山东/贵州同模式 `channel_send(ch, PROGRAM_CODE, ...)`），不再硬编码 YN_FX_P5_1.0（宏 `YN_FW_VERSION` 已删除） |
| 8 | 上电效果 | **删除**「稍候熄灭」与默认显示注册——不注册默认显示内容（使用固件默认显示）。`app_yn_proto_default.c` 已移除，Makefile `SRC_APPLICATION` / `.eide/eide.yml` 条目同步删除（yml 外部编辑后须 EIDE Reload Project） |
| 9 | '1' 主机查询异常应答触发条件 | **恒回正常 00**（现状已确认，设备无故障检测条件） |
| 10 | 0x01 全屏点亮颜色范围 | 扩展到显示屏支持的全部颜色（保留 01红/02绿/03黄 兼容）。P5 户外全彩屏 display_color_t 枚举共 8 色，扩展取值表： |

**决定 10 扩展取值表**（DATA0 与 `display_color_t` 枚举值恒等，执行层直接按枚举渲染）：

| DATA0 | 颜色 | 枚举 |
|---|---|---|
| 0x01 | 红 | COLOR_RED（协议原文，兼容） |
| 0x02 | 绿 | COLOR_GREEN（协议原文，兼容） |
| 0x03 | 黄 | COLOR_YELLOW（协议原文，兼容） |
| 0x04 | 蓝 | COLOR_BLUE（新增） |
| 0x05 | 紫 | COLOR_PURPLE（新增） |
| 0x06 | 青 | COLOR_CYAN（新增） |
| 0x07 | 白 | COLOR_WHITE（新增） |
| 0x00 | （拒绝） | COLOR_BLACK——全屏黑等效 '5' 全屏清除，非「点亮」 |

## 9. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-24 | 新建：云南常规费显协议模块（5 源文件 + 4 头文件）接入 STD 分发框架；Makefile 全协议/CQ 口径编入；EIDE Debug 目标排除；doc/05-01、doc/06-04、doc/CLAUDE.md、doc/08-01 同步 |
| 2026-08-24 | 修订（用户决定 1~10）：'2' 自检改老化循环显示 + 5s 语音「系统正在自检」+ 下一帧打断；'3'/'6' 行 5 按协议接受（执行但不落屏）；'8' 亮度 0x00 自动档 + '1'~'8' 手动档；'B' 费额语音核对确认；'A' 红优先确认；0x02 应答改 PROGRAM_CODE；移除上电效果 default 文件（不注册默认显示）；'1' 恒回正常确认；0x01 全屏点亮扩 01红~07白 七色。构建验证：PROTO=ALL text 352860/data 1656/bss 138196、PROTO=CQ text 338996/data 852/bss 134720，均链接通过 |