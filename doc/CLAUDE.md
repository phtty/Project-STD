# CLAUDE.md

此文件为 Claude Code (claude.ai/code) 在此仓库中工作时提供指导。

## 构建系统

STM32F407ZGTx 嵌入式项目，工具链 `arm-none-eabi-gcc`，C23 标准。

- **构建**：`make -j8`（根目录 Makefile，`TOOLCHAIN=gcc|clang`，`CONFIG=Debug|Release`，`PROTO=ALL|CQ`）
  - GCC Debug：`make -j8`（默认，`-Og -g`）
  - GCC Release：`make -j8 CONFIG=Release`
  - Clang Debug：`make -j8 TOOLCHAIN=clang`
  - Clang Release：`make -j8 TOOLCHAIN=clang CONFIG=Release`
  - **`PROTO ?= ALL`**：默认全协议 dev 构建（CQ 恒定编入，位于 LDI 源之后）。
    **`PROTO=CQ`**：重庆量产口径——从源码列表剔除 LDI 目录文件、追加
    `-DPROTO_CHONGQING`（`{` 帧族仍编入并保留 `-DSTD_ALL_PROTO` 豁免守卫。
    **注（2026-08-24 修订）**：TCP Server/Client 通道两口径**均启动**——
    `app_boot.c` 实际无 `#ifndef PROTO_CHONGQING` 口径裁剪，此前「CQ 构建不
    启动 TCP 通道」表述与代码不符，已按代码实态更正）。
    **网络配置应用（两口径统一，2026-08-21 解耦，2026-08-24 端口应用两口径化）**：
    中立模块 `app_net_boot`（`app_net_boot_apply`，`app_boot.c` init_task 在
    `app_board_net_cfg_fw_version_update` 之后、splash 之前调用——fw_version_update
    空扇区初始化 net_cfg=0，app_net_boot 随后判无效写默认）读 Sector1 net_cfg →
    `pl_net_set_ip` + `app_tcp_server_set_port`（口取 `net_cfg.port` = TCP 业务口，
    两口径统一应用）；Sector1 空/损坏/非法 → accept_write 写本构建默认记录落盘并
    应用（方案 B 2026-08-21：两口径默认均 **port=9528（TCP 口）+ udp_port=20103
    （CQ UDP 口）**，IP 各自口径）：CQ 192.168.1.5/255.255.255.0/192.168.1.1，
    dev 192.168.114.200/255.255.255.0/192.168.114.1。
    LDI `ldi_ctx_init` 与 CQ `cq_proto_init` 不再直接改 netif（ldi 保留 Sector1/W25
    自愈与配置装载）。
    **CQ setip 双构建语义**（doc/03 PartB B.7.1，2026-08-21 方案 B）：setip 命令
    端口写入 Sector1 `net_cfg.udp_port`（CQ UDP 业务口专有字段），TCP 口
    `net_cfg.port` **保留现值不再被 setip 污染**；改 IP 两构建重启后均生效（均走
    `app_net_boot_apply`）；改端口仅 PROTO_CHONGQING 构建对 CQ 业务口生效
    （`_udp_cq_read_port` 读 Sector1 net_cfg.udp_port，无效/0 回退 20103），dev
    构建 CQ 业务口固定 20103（`#else` 分支）、setip 的 port 对 dev 构建任何口都
    不生效——端口修改验证须用 `make PROTO=CQ` 或 EIDE `PROTO_CHONGQING` 目标。
    **CQ 心跳超时故障屏开关**（`app_cq_proto.h` `CQ_FAULT_SCREEN`，1 开 / 0 关，
    doc/03 PartB B.7.2，2026-08-21 新增、2026-08-24 改名单层宏）：默认跟随构建
    口径——PROTO_CHONGQING 开启、共存/其他构建（通用版多协议固件）关闭（他省
    上位机不发重庆 syn1，120s 故障屏会误触发）；可显式覆盖 `-DCQ_FAULT_SCREEN=1`
    强制开 / `=0` 强制关（旧名 CQ_FAULT_SCREEN_ENABLED / CQ_FAULT_SCREEN_FORCE_ON
    已废弃，不保留兼容别名）。
    关闭时 `cq_proto_timer_task` 不计数、不渲染（`s_cq_sync_counter` 恒 0、
    `s_cq_fault_shown` 恒 false），warn1 倒计时不受开关影响。
    **不变量：排除 LDI 与定义 `PROTO_CHONGQING` 必须成对**（app_net_boot 兜底后
    中间态不再致命，但 PROTO_CHONGQING 仍决定默认口径与端口应用对象，不成对 =
    口径错配）。EIDE Debug 目标当前为 CQ 量产口径（excludeList 排除 ldi +
    defineList 含 PROTO_CHONGQING）。
- **`{` 帧族编译期互斥守卫**：Makefile 默认定义 `-DSTD_ALL_PROTO`（全协议共存开发构建，`g_brace_proto_guard` 守卫失效）；EIDE 量产构建不定义该宏——多个 `{` 帧族协议（青海/山东/贵州/四川MTC）同时编入即链接报 `multiple definition of 'g_brace_proto_guard'`，强制互斥。
- **EIDE 持有 eide.yml（外部编辑纪律）**：EIDE 运行时用自己的内存模型回写 `.eide/eide.yml`，会冲掉外部直接编辑（曾两次发生：CQ 文件夹收录、app_net_boot.c 收录被回写丢失，导致 EIDE 构建 undefined reference）。**外部改完 yml 后必须立刻在 EIDE 中「重新加载项目」（EIDE: Reload Project）让它读入新条目，期间不要先做任何会触发保存的 GUI 操作**；构建前先确认 EIDE 编译列表含新增源文件。
- **编译数据库**：`bear --output build/Debug/compile_commands.json -- make -B -j8`（`-B` 强制全量重编译）
- **编译开关总表**：`doc/构建开关总表.md`（2026-08-24 新增，收录 PROTO_CHONGQING / STD_ALL_PROTO / CQ_FAULT_SCREEN / g_brace_proto_guard 等全部编译期开关及各构建入口默认值）
- **烧录**：`openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; program ./build/Debug/Project_STD.hex verify reset exit"`
- **整片擦除**：`openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; stm32f4x unlock 0; stm32f4x mass_erase 0; shutdown"`
- **J-Link 一键烧录**：`bash tool/flash_all.sh`（Bootloader + Recovery + 主固件一次 J-Link 会话烧完，**默认烧后擦除 Sector1（0x08004000~0x08007FFF）恢复出厂配置态**；`--keep-config` 保留板级配置；`--erase` 整片擦除；`--verify` 校验；`--dry-run` 预览命令不烧录）
- **调试期烧录陷阱（已规避）**：Bootloader 条件 D（非 debug 模式）校验 Sector1 记录 `app_info.size/crc32` 与 `0x08040000` 实机固件 CRC 是否一致，`app_info.size/crc32` 真实值只由 Recovery 升级流程写入（`app_info.version` 由主固件启动时从 PROGRAM_CODE 落库，供 IAP 0x03 上报）。一旦 Recovery 成功升级过一次，再用 J-Link/EIDE 直接重烧主固件而不更新 Sector1 → 条件 D 校验失配 → Bootloader 判「App 损坏」→ 设备永远进 Recovery。**纪律：任何烧录器（J-Link/OpenOCD/EIDE）烧完主固件后必须擦除 Sector1 恢复出厂态**——Sector1 空 → Bootloader 走条件 C（出厂初始化）→ 正常跳主固件；net_cfg 丢失副作用可接受，主固件上电 `ldi_ctx_init` 从 W25Qxx 外部 Flash 同步回写（空扇区自动完整初始化）。`tool/flash_all.sh` 已默认执行该纪律；手动烧录/单固件烧录同样遵守「烧完擦 Sector1」。
- **三固件同步烧录纪律（方案 B 布局变更，2026-08-21）**：方案 B 把 Sector1 记录从 68B 扩到 72B（`NetConfig_t` 新增 `udp_port`，`config_crc` 偏移 64→68，Bootloader/Recovery 的 `config_info.h` 已同步修改）。**设备上 Bootloader/Recovery 若仍是旧 68B 布局（未随主固件重烧）**：旧 Bootloader 按旧偏移读 `config_crc`（读到新记录的 `udp_port` 字段）→ 判 CRC 失配 → 重建**旧布局**出厂记录；主固件按 72B 读又失配 → 反复自愈改写，两布局交替重建、Sector1 永不稳定（网络配置回出厂态；残留升级中间态还可能被旧 Bootloader 条件 A/B/D 误判 → 设备陷 Recovery、主固件不运行 → UDP 10011/20103 全部无响应，LDI 搜索/CQ 业务均失效）。**纪律：方案 B 后三固件必须同步烧录（`tool/flash_all.sh` 一次 J-Link 会话）**，单固件重烧后按惯例擦 Sector1。
- **0x03 版本为空判断路径**：现场 IAP 0x03 上报 `app_info.version` 全 0 时，按序判断：
  ① 看 RTT 是否有 `[fwver]` 日志——**无日志 = 主固件从未运行，设备落在 Recovery 态**（旧
  Sector1 记录 size/crc32 与新烧固件失配 → 条件 D 判 App 损坏），补做「擦 Sector1」后重启
  即可；有 `[fwver] write ok` 仍为空 = 应答来自 Recovery（0x03 应答 ReData[10] `update_sta`
  ==2 且 ReData[0] `size`≠0 为 Recovery 态特征，主固件态 update_sta==0）；有 `erase/write
  failed` = 擦写路径问题；② 确认烧录物为最新构建（`arm-none-eabi-strings
  build/Debug/Project_STD.elf | grep 9K10212482`，EIDE 与 make 共用 `build/Debug` 同名产物，
  注意陈旧覆盖）。
- **清理**：`make clean`（仅删除 `build/$(CONFIG)` 目录）

### 构建产物

| 产物 | 格式 | 用途 |
|---|---|---|
| `Project_STD.elf` | ELF（带调试符号） | 链接器输出，含完整符号表，调试器（probe-rs / cortex-debug）直接使用。`arm-none-eabi-size` 输出各段大小 |
| `Project_STD.hex` | Intel HEX（ASCII 文本） | 烧录文件。OpenOCD 通过 `program ... verify` 写入并回读比对。文本格式，每行含地址+数据+校验和 |
| `Project_STD.bin` | 纯二进制 | 裸二进制映像，无地址信息。适用于 IAP bootloader 直接写入 Flash |

**链接优化**：`--gc-sections` + `-ffunction-sections -fdata-sections`，未引用的函数/数据自动丢弃。`--specs=nano.specs` 链接精简版 newlib。`-u _printf_float` 强制链接 printf 浮点格式化支持（newlib-nano 默认关闭）。`-fshort-enums` 按最小宽度打包枚举类型。

## 文档地图

| 目录 | 角色 |
|---|---|
| `doc/01_显示系统` | 显示层：1-260/1-577/1-969/1-263 模组驱动、自适应字号 |
| `doc/02_LDI协议与外设接口` | LDI 协议与光敏/字库等外设接口 |
| `doc/03_重庆高速二代费显协议` | RLS 重庆协议接入记录 |
| `doc/04_青海高速费显协议` | 青海协议接入记录 |
| `doc/05_协议模块多协议兼容优化` | 多协议 RB/绑定/queue 深度权威（`01_architecture.md`） |
| `doc/06_SRAM内部分数据迁移` | 内存分区唯一权威；`04_current_memory_occupancy.md` 为占用账 |
| `doc/07_LDI与IAP配置解耦` | LDI↔IAP 配置解耦（方案 A 已落地，`app_board_net_cfg`） |
| `doc/08_协议模块接入规则` | 新协议接入权威指引（规则/串口/网络/检查清单） |
| `doc/09_山东费显协议` | 山东车道费额显示器协议接入记录 |
| `doc/10_贵州费显协议` | 贵州常规费显协议接入记录（13 命令/裁决差异表/帧头冲突纪律） |
| `doc/11_云南费显协议` | 云南常规费显协议接入记录（13 命令/24 点阵渲染/已确认决定） |
| `doc/12_缅甸费显项目适配` | 缅甸费显 8051→STD 替代方案设计（方案 B 接口板：TB62726×6 12 位 7 段灯板、3.3V→5V 电平转换、位带 WT588D、FF/E0 20 字节帧 10 命令接入；**方案设计，未实施**） |

## 硬件架构

**MCU**：STM32F407ZGTx（Cortex-M4F，168MHz，FPU，1024KB Flash，128KB SRAM + 64KB CCMRAM）

**时钟**：HSE 8MHz → PLL (M=4, N=168, P=2, Q=7) → SYSCLK 168MHz / APB1 42MHz / APB2 84MHz

**外设**：ADC1、SPI1（W25Qxx）、TIM2/3/4/7、USART1/USART3/USART6、DMA1/2、CRC、IWDG、RTC、Ethernet MAC + DP83848 PHY

## 内存布局

链接脚本：`Compiler/STM32F407XX_FLASH.ld`

```
Flash (1024KB, 起始 0x08000000)
├─ Sector 0:    0x08000000  16KB   Bootloader（独立工程，`ORIGIN 0x08000000 LENGTH 16K`）
├─ Sector 1:    0x08004000  16KB   板级系统配置 (app_board_net_cfg.c)
├─ Sector 2~5:  0x08008000  224KB  Recovery 固件（独立工程，`ORIGIN 0x08008000 LENGTH 224K`）
├─ Sector 6~11: 0x08040000  768KB  主固件 .text/.rodata/.initcall（本工程，FLASH ORIGIN 0x08040000）
└─ (Sector 11 已释放 — LDI 配置已迁移至 W25Qxx，不占内部 Flash)

SRAM (128KB, 0x20000000) — Debug 链接约用 **92%（≈120820B）**（2026-08-14；以 06/04 为准）
├─ .data / .bss     LwIP ram_heap + RX_POOL + PBUF_POOL 等（ETH 大户，永留 SRAM）
├─ ucHeap           **36KB** FreeRTOS heap_4（任务栈/TCB/动态 OS 对象）
├─ 协议 RB          RJ45 **1536** / RS485 **768** / RS232 **768**（三槽 provide）
├─ UART DMA         RS485/RS232 各 **640**（无 RS232_1 DMA）
├─ 帧 queue 静态    IAP2 / LDI4 / QH3 / RLS2 / AH3 / SC_ETC3 / SC_MTC3 / SC_OL3 / SD3 / GZ3 / YN3（不占 ucHeap）
├─ RTT Up           2KB；W25 sec、s_dma_bounce 等
└─ _user_heap_stack newlib + MSP 预留

CCMRAM (64KB, 0x10000000, NOLOAD) — 约用 **58%（38208B）**（1-577 3×3 / 同量级 1-969）
├─ pixel_map / hub75_buff / row_dst / g_bsrr   **仅显存与 BSRR**
├─ 例外（2026-08-20）：CQ 协议帧队列/缓冲 **6377B**（s_cq_queue_buf 3156 + queue_cb 80 +
│   任务帧缓冲 1052 + JSON/文本缓冲 2089；SRAM 全协议构建余量不足，CPU 独占访问无 DMA）
└─ （无 ucHeap、无协议 RB、无 UART DMA）
```

**CCMRAM 特性**：零等待、D-bus 单端口；`startup.c` 清零。仅数据、不可执行；**DMA/ETH 不可达**。占用以 `doc/06_SRAM内部分数据迁移/04_current_memory_occupancy.md` 为准；分区宪法见同目录 `02_memory_policy.md`。多协议 RB / queue 深度见 `doc/05_协议模块多协议兼容优化/`。

## 闪存扇区职能地图

> **当前状态**：Bootloader 与 Recovery 已作为独立工程部署（workspace 内 `STM32F407-Bootloader-master` / `STM32F407-Recovery-master`）。主固件已从 `0x08040000` 链接（Sector 6~11，768K，`Compiler/STM32F407XX_FLASH.ld`），并在 `Core/Src/main.c` 中 `SCB->VTOR = FLASH_BASE | 0x40000` 重定位向量表。

| 扇区 | 地址 | 大小 | 内容 | 读写方式 |
|---|---|---|---|---|
| 0 | `0x08000000` | 16KB | Bootloader（独立工程，`ORIGIN 0x08000000 LENGTH 16K`） | 烧录时写入 |
| 1 | `0x08004000` | 16KB | 板级系统配置 (magic + update_sta + FWInfo + NetConfig + CRC32；记录 72B，NetConfig 20B 含 port/udp_port 双端口，方案 B 2026-08-21) | `app_board_net_cfg.c` 内存映射读 + 擦写 |
| 2~5 | `0x08008000`~`0x08040000` | 224KB | Recovery 固件（独立工程，`ORIGIN 0x08008000 LENGTH 224K`） | IAP 升级时写入 |
| 6~11 | `0x08040000`~`0x080FFFFF` | 768KB | 主固件 (.text/.rodata/.initcall) | 烧录时写入 |
| 11 注 | `0x080E0000` | 128KB | 已释放 — LDI 配置已迁移至 W25Qxx 最后一个 4KB 扇区（不再占内部 Flash） | — |

**外部 Flash**（W25Qxx，SPI1，~8MB）：字库数据（5 字号×4 字型×2 编码的 40 个三元组） + LDI 设备配置（最后一个 4KB 扇区），通过 `dev_storage_read` 接口访问。LDI 配置绑定在 `Application/Src/LDI/app_ldi_cfg.c`（`sw_dev_initcall` 中 `s_ldi_base = dev_storage_capacity(w25) - 4096`）。

**出厂默认网络配置（三固件统一）**：192.168.114.200/24（网关 192.168.114.1、TCP 口 9528；CQ UDP 口 20103，方案 B 2026-08-21）。Recovery 上电 `MX_LWIP_Init` 直读 Sector1.net_cfg 配 netif（空/无效回退统一默认）；主固件 `ldi_ctx_init` 上电同步三分支：**Sector1 为网络配置真源、W25 为恢复镜像**（Sector1 有效且 net_cfg 合法即优先采纳，含升级中间态，`update_sta` 不参与分支判定（2026-08-21 修订）；Sector1 空/损坏/记录无效或 net_cfg 非法时以 W25 自愈回写；皆空用统一出厂默认，详见 `doc/07_LDI与IAP配置解耦/02_decoupling_solutions.md` §11/§12）。**netif 应用由中立模块 `app_net_boot`（`app_net_boot_apply`）统一执行**——`ldi_ctx_init` 只做自愈与配置装载（2026-08-21 解耦）；Sector1 空/损坏/非法时 `app_net_boot` accept_write 写本构建默认落盘并应用（两口径默认均 port=9528/udp_port=20103，IP 各自口径：CQ 192.168.1.5、dev 114.200，详见 §14/§15）。**所有改 IP 接口（LDI 0AH / IAP 4B02 / Recovery IAP）统一重启生效**（运行时不即时改 netif）；Bootloader 对 Sector1 损坏态（magic 不匹配或 CRC 错）自动重建出厂记录自愈（详见 §12）。UDP 发现口 10011 固定（宏 `LDI_DISCOVERY_PORT`）。**搜索/上报端口字段语义**：LDI 12H 应答 port 与 IAP 0x01 上报 port 均为配置功能端口（TCP 业务口，出厂默认 9528，宏 `LDI_DEFAULT_CONFIG_PORT`），搜索/上报的传输渠道才是 UDP 10011——勿混淆（2026-08-18 修复 12H 误报 10011 的污染循环）。

## 软件分层架构

依赖方向严格单向（上层可见下层，反之不可）：

```
┌──────────────────────────────────────────────────────────────────┐
│  Application/  (app_*)                                           │
│  业务逻辑：boot编排、协议处理(IAP/LDI/AH_MQTT)、渲染引擎、       │
│  传感器管理、网络通道(UDP/TCP/MQTT/RS232/RS485任务)              │
│                                                                  │
│  依赖：Device/ + Kernel/ + Platform/ (不直接碰 HAL 地址)         │
├──────────────────────────────────────────────────────────────────┤
│  Device/  (dev_*)                                                │
│  设备抽象：OCP 虚表封装硬件差异                                  │
│  Display/  Storage/  Comm/  Network/  IO/  Config/               │
│                                                                  │
│  依赖：Platform/ + Kernel/ (通过 pl_* 接口操作硬件)              │
├──────────────────────────────────────────────────────────────────┤
│  Kernel/                                                         │
│  纯软件工具：ring_buffer、initcall、container_of、text_cvt、     │
│  bit_utils、crc_utils、bcc_utils                                 │
│                                                                  │
│  依赖：无（零硬件依赖，仅 stdint/stddef/stdbool）                │
├──────────────────────────────────────────────────────────────────┤
│  Platform/  (pl_*)                                               │
│  HAL 薄封装：不透明句柄 (void*)，HAL 类型对外不可见              │
│  pl_spi  pl_tim  pl_uart  pl_gpio  pl_hub75  pl_eth  pl_net      │
│  pl_flash  pl_crc  pl_rtc  pl_iwdg  pl_dma  pl_dwt  pl_sys       │
│                                                                  │
│  依赖：Core/ + HAL 库 (仅 .c 文件 include Core 头文件)           │
├──────────────────────────────────────────────────────────────────┤
│  Core/                                                           │
│  STM32CubeMX 生成：MX_xxx_Init()、ISR 声明、HAL 配置             │
│  main.c  system_stm32f4xx.c  stm32f4xx_it.c  FreeRTOSConfig.h    │
│                                                                  │
│  依赖：HAL 库 + CMSIS                                            │
├──────────────────────────────────────────────────────────────────┤
│  Compiler/                                                       │
│  startup.c (C 语言 Reset_Handler + 自定义向量表)                 │
│  STM32F407XX_FLASH.ld (链接脚本，含 initcall 段定义)             │
└──────────────────────────────────────────────────────────────────┘
```

### Platform 层内部依赖

```
pl_sys (SystemClock_Config, delay, reset)
 ├─ pl_dma     (DMA 流初始化，需在 SPI/UART 之前)
 ├─ pl_gpio    (GPIO 时钟，几乎所有模块依赖)
 ├─ pl_tim     (TIM2/3/4/7)
 ├─ pl_spi     (SPI1，依赖 DMA)
 ├─ pl_uart    (USART1/3/6，依赖 DMA)
 ├─ pl_crc     (硬件 CRC)
 ├─ pl_iwdg    (独立看门狗)
 ├─ pl_rtc     (RTC 备份寄存器)
 ├─ pl_adc     (ADC1)
 ├─ pl_exti    (外部中断)
 ├─ pl_hub75   (HUB75 GPIO 位拆裂)
 ├─ pl_flash   (内部 Flash 编程)
 ├─ pl_dwt     (DWT 周期计数器)
 ├─ pl_rtt     (SEGGER RTT)
 ├─ pl_eth     (ETH MAC + MDIO)
 └─ pl_net     (LwIP 初始化，依赖 pl_eth)
```

各模块用不透明句柄（`typedef void *pl_xxx_handle_t`），Device 层只传句柄、不感知 HAL 类型。

## 启动流程与 initcall

### 完整启动序列

```
硬件上电 → Reset_Handler (Compiler/startup.c)
  ├─ .data 拷贝 (Flash→SRAM), .bss 清零, CCMRAM 清零
  ├─ SystemInit() — FPU 使能（VTOR 在 main() 中由 `SCB->VTOR` 重定位到 0x08040000）
  └─ main() (Core/Src/main.c)
       ├─ SCB->VTOR = FLASH_BASE | 0x40000  ← 向量表重定位（主固件入口 0x08040000）
       ├─ HAL_Init()                     ← HAL 库基础
       ├─ SystemClock_Config()           ← 168MHz
       ├─ initcall_run(__hw_initcall)    ← RTOS 前硬件初始化
       └─ app_boot()                     ← RTOS 编排
            ├─ osKernelInitialize()
            ├─ osThreadNew(init_task, prio=High, stack=512×4)
            └─ osKernelStart()
                 ├─ [FreeRTOS 接管: SysTick→PendSV 任务切换]
                 └─ init_task:
                      ├─ dev_eth_start()          ← LwIP + netif
                      ├─ sw_board_init()          ← initcall_run(__sw_initcall)
                      ├─ app_board_net_cfg_fw_version_update()  ← PROGRAM_CODE 落库 Sector1（空扇区 net_cfg 置 0）
                      ├─ app_net_boot_apply()     ← Sector1 net_cfg → netif + TCP Server 口（CQ 构建只应用 netif；空/损坏写默认落盘）
                      ├─ app_splash_display()     ← 开机画面 (FW/MD 版本, 5s)
                      ├─ osThreadNew(half_sec_task)
                      ├─ app_tcp_server_start() / app_tcp_client_start()  (#ifndef PROTO_CHONGQING)
                      │   / app_udp_start()
                      ├─ app_rs485_start() / app_rs232_start()  (app_rs232_1_start 注释)
                      ├─ app_default_display()    ← 自注册默认显示界面（协议注册回调则用之，否则默认欢迎画面）
                      └─ osThreadExit()           ← 自我销毁
```

### initcall 初始化顺序表

**hw_initcall (RTOS 前，main() 中执行)**：

| 顺序 | 层级 | 函数 | 文件 | 职责 |
|---|---|---|---|---|
| 0-pre | — | （空） | — | 预留 |
| 1-pl | pl | `pl_dma_init` | `pl_dma.c` | DMA 流初始化 |
| 1-pl | pl | `pl_gpio_init` | `pl_gpio.c` | GPIO 时钟使能 |
| 1-pl | pl | `pl_spi_init` | `pl_spi.c` | SPI1 初始化 |
| 1-pl | pl | `pl_tim_init` | `pl_tim.c` | TIM2/3/4/7 初始化 |
| 1-pl | pl | `pl_uart_init` | `pl_uart.c` | USART1/3/6 初始化 |
| 1-pl | pl | `pl_crc_init` | `pl_crc.c` | 硬件 CRC |
| 1-pl | pl | `pl_iwdg_init` | `pl_iwdg.c` | 看门狗 |
| 1-pl | pl | `pl_rtc_init` | `pl_rtc.c` | RTC |
| 1-pl | pl | `pl_adc_init` | `pl_adc.c` | ADC1 |
| 1-pl | pl | `pl_exti_init` | `pl_exti.c` | 外部中断 |
| 1-pl | pl | `pl_hub75_init` | `pl_hub75.c` | HUB75 GPIO |
| 1-pl | pl | `pl_rtt_init` | `pl_rtt.c` | SEGGER RTT |
| 2-dev | dev | `dev_display_init` | `dev_display.c` | HUB75 引脚 + DBG 冻结 |
| 2-dev | dev | `dev_display_1_577_init` | `dev_display_1_577.c` | 1-577 模组实例注册（备用） |
| 2-dev | dev | `dev_display_1_260_init` | `dev_display_1_260.c` | 1-260 模组实例注册（备用，Makefile/EIDE 均已排除） |
| 2-dev | dev | `dev_display_1_969_init` | `dev_display_1_969.c` | 1-969 模组实例注册（备用） |
| 2-dev | dev | `dev_display_1_263_init` | `dev_display_1_263.c` | 1-263 模组实例注册（P6 32x32；Makefile 与 EIDE Debug 现行） |
| 2-dev | dev | `dev_display_p20_init` | `dev_display_p20.c` | P20 模组实例注册（未编入任何构建） |
| 2-dev | dev | `dev_w25qxx_init` | `dev_w25qxx.c` | SPI Flash JEDEC 识别 |
| 2-dev | dev | `dev_eth_init` | `dev_eth.c` | ETH MAC + DP83848 PHY |
| 2-dev | dev | `dev_rs485_init` | `dev_rs485.c` | RS485 RE 方向回调注入 |
| 2-dev | dev | `_app_board_net_cfg_storage_init` | `Application/Src/Config/app_board_net_cfg.c` | 内部 Flash ops 绑定 (Sector 1) |
| 2-dev | dev | `dev_io_ctrl_init` | `dev_io_ctrl.c` | 车道灯/闪光灯 GPIO 初始化 |
| 2-dev | dev | `dev_key_init` | `dev_key.c` | EXTI 中断回调注册 (SW1~SW3, TEST) |
| 3-post | — | （空） | — | 预留 |

**sw_initcall (RTOS 后，init_task → sw_board_init 中执行)**：

| 顺序 | 层级 | 函数 | 文件 | 职责 |
|---|---|---|---|---|
| 0-pre | — | （空） | — | 预留 |
| 2-dev | dev | `dev_display_start` | `dev_display.c` | 创建 scan_task + 启动 TIM3/4 |
| 2-dev | dev | `dev_key_sw_init` | `dev_key.c` | 创建 TEST 按键信号量 |
| 2-dev | dev | `_app_flash_ldi_storage_init` | `Application/Src/LDI/app_ldi_cfg.c` | LDI 配置 W25Qxx 地址计算 |
| 3-app | app | `app_dispatch_init` | `app_dispatch.c` | 创建 ch_queue + frame_dispatch_task |
| 3-app | app | `_render_init` | `app_render.c` | 绑定 display + storage 句柄 |
| 3-app | app | `_app_key_init` | `app_key.c` | 创建 key_poll_task (20ms) |
| 3-app | app | `app_light_sensor_init` | `app_light_sensor.c` | 创建光传感器自动调节任务 |
| 3-app | app | `iap_module_init` | `Application/Src/IAP/app_iap.c` | IAP 协议自注册（UDP） |
| 3-app | app | `ldi_module_init` | `Application/Src/LDI/app_ldi.c` | LDI 协议自注册（TCP/UDP 三通道） |
| 3-app | app | `qh_proto_init` | `Application/Src/ProtocolParser_QingHai/app_qh_proto.c` | 青海协议自注册（RS485+RS232） |
| 3-app | app | `sc_etc_proto_init` | `Application/Src/ProtocolParser_SiChuang_ETC/app_sc_etc_proto.c` | 四川 ETC 费显协议自注册（RS485+RS232） |
| 3-app | app | `sc_mtc_proto_init` | `Application/Src/ProtocolParser_SiChuang_MTC/app_sc_mtc_proto.c` | 四川 MTC 费显协议自注册（RS485+RS232） |
| 3-app | app | `sc_ol_proto_init` | `Application/Src/ProtocolParser_SiChuang_Overload/app_sc_ol_proto.c` | 四川治超屏协议自注册（RS485+RS232） |
| 3-app | app | `sd_proto_init` | `Application/Src/ProtocolParser_ShanDong/app_sd_proto.c` | 山东费显协议自注册（RS485+RS232） |
| 3-app | app | `gz_proto_init` | `Application/Src/ProtocolParser_GuiZhou/app_gz_proto.c` | 贵州费显协议自注册（RS485+RS232） |
| 3-app | app | `yn_proto_init` | `Application/Src/ProtocolParser_YunNan/app_yn_proto.c` | 云南费显协议自注册（RS485+RS232；不注册默认显示——使用固件默认显示，用户决定 8） |
| 3-app | app | `cq_proto_init` | `Application/Src/ProtocolParser_ChongQing/app_cq_proto.c` | 重庆CQ协议自注册（UDP_CQ 业务口 + UDP 搜索口双 mask；cJSON 钩子换绑 FreeRTOS 堆；网络配置应用移交 app_net_boot） |
| 3-app | app | `app_uart_baud_init` | `Application/Src/app_uart_baud.c` | DIP1 波特率选择（RS232+RS485 同步切换） |
| 3-app | app | `rls_module_init` | `Application/Src/RLS/app_rls.c` | RLS 协议自注册（RS485） |
| 3-app | app | `_factory_test_init` | `Application/Src/app_factory_test.c` | 出厂检测 monitor |
| 3-app | app | `ah_mqtt_module_init` | `Application/Src/AH_MQTT/ah_mqtt.c` | 已注释，未启用 |
| 4-post | — | （空） | — | 预留 |

### initcall 实现原理

链接脚本中定义两个特殊段：
```
.hw_initcall : { KEEP(*(SORT(.hw_initcall.0))) ... KEEP(*(SORT(.hw_initcall.3))) }
.sw_initcall : { KEEP(*(SORT(.sw_initcall.0))) ... KEEP(*(SORT(.sw_initcall.4))) }
```

每个 `*_initcall(fn)` 宏生成一个 `initcall_entry_t` 常量放入对应 section。`initcall_run(start, end)` 顺序遍历调用。同层内按**链接顺序（源码收录序）**执行——`SORT()` 对同名 section 退化为 .o 链接顺序，Makefile/EIDE 源码收录序即调用序（`app_dispatch_init` 靠收录序在协议之前）。

## 中断体系

### 中断源与分派路径

```
外设硬件中断
├─ TIM3_IRQHandler           (pl_tim.c)
│   └─ HAL_TIM_IRQHandler → HAL_TIM_PeriodElapsedCallback
│       └─ osEventFlagsSet(s_scan_evt) → 唤醒 scan_task
│
├─ TIM4_IRQHandler           (pl_tim.c)
│   └─ HAL_TIM_IRQHandler → HAL_TIM_PeriodElapsedCallback
│       └─ PWM 亮度：pl_hub75_oe_set(pwm_cnt >= light_level)
│
├─ TIM7_IRQHandler           (pl_tim.c)
│   └─ HAL_TIM_IRQHandler → HAL_TIM_PeriodElapsedCallback
│       └─ HAL_IncTick() (FreeRTOS 时基，1ms)
│
├─ USART1_IRQHandler         (pl_uart.c)
│   └─ HAL_UART_IRQHandler + uart_idle_handle
│       └─ DMA 空闲中断 → 计算接收长度 → rx_cb→
│           osMessageQueuePut(rx_queue, &len) → 唤醒 rs485_task / rs232_task
│
├─ USART3_IRQHandler / USART6_IRQHandler  (同上)
│
├─ SPI1_IRQHandler           (pl_spi.c)
│   └─ HAL_SPI_IRQHandler → HAL_SPI_TxRxCpltCallback / HAL_SPI_RxCpltCallback
│       └─ rx_cplt_cb(s_ok=true) → dev_w25qxx DMA 完成通知
│
├─ DMA1_Stream1_IRQHandler   (pl_uart.c, USART3 RX DMA)
├─ DMA2_Stream1_IRQHandler   (pl_uart.c, USART6 RX DMA — 句柄已初始化但无通道启动接收)
├─ DMA2_Stream2_IRQHandler   (pl_uart.c, USART1 RX DMA)
│
├─ ETH_IRQHandler            (pl_eth.c, 见下)
│
├─ HardFault_Handler         (stm32f4xx_it.c)
│   └─ naked asm → hard_fault_handler_c → 现场保存 (R0-R3,R12,LR,PC,xPSR,CFSR,BFAR)
│
├─ SysTick_Handler           (FreeRTOS 内部, CMSIS-RTOS V2 封装)
├─ SVC_Handler  → vPortSVCHandler    (FreeRTOS 内核, 启动第一个任务)
└─ PendSV_Handler → xPortPendSVHandler (FreeRTOS 内核, 任务上下文切换)
```

**设计原则**：
- 所有外设 ISR 内聚在 Platform 层 .c 文件中，不分散在 `stm32f4xx_it.c`
- ISR 中只做最小操作（标记、通知），耗时逻辑全部在 RTOS 任务中完成
- 中断优先级：`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5`，FreeRTOS API 仅可在优先级 ≥5 的 ISR 中调用

### 中断→任务解耦的三级流水线

```
[裸机 ISR]                [RTOS 信号量/队列]         [RTOS 任务]
─────────────────────────────────────────────────────────────────
TIM3 行同步    ──→  osEventFlagsSet        ──→  scan_task (Realtime)
TIM4 PWM       (ISR 内直接完成，不唤醒任务)
UART 空闲中断   ──→  osMessageQueuePut       ──→  rs485_task / rs232_task
                                                     └─→ app_channel_dispatch
                                                           └─→ osMessageQueuePut(ch_queue)
                                                                 ──→ frame_dispatch_task
                                                                       └─→ osMessageQueuePut(frame_queue)
                                                                             ──→ 协议处理任务
SPI DMA 完成   ──→  volatile s_ok=true       ──→  dev_w25qxx _read 轮询 (osDelay 轮询)
ETH 收包       ──→  osSemaphoreRelease       ──→  ethernetif_input
ETH 链路状态   ──→  pl_net_link_listener     ──→  UDP/TCP/MQTT 通道任务重建连接
```

**关键模式**：
- **Event Flags**：`scan_task` 用 `osEventFlagsWait` 等待 TIM3 行同步触发。ISR 中使用 `osEventFlagsSet`（`configUSE_OS2_EVENTFLAGS_FROM_ISR=1` 使能 ISR 安全调用）
- **Message Queue**：`rs485_task` / `rs232_task`、`frame_dispatch_task`、协议处理任务逐级通过队列传递数据指针
- **Semaphore**：UDP/TCP 通道用信号量协调连接/断开生命周期（链路断开→信号量释放→任务重建连接）
- **volatile 轮询**：W25Qxx SPI 半双工 DMA 读使用 `while(!s_ok) osDelay(1)` 轮询（历史原因：RTOS 未完全就绪时无法使用 event flags）

## RTOS 配置

`Core/Inc/FreeRTOSConfig.h`，FreeRTOS v10.3.1 + CMSIS-RTOS V2 封装：

| 参数 | 值 | 说明 |
|---|---|---|
| `configTICK_RATE_HZ` | 1000 | 1ms 时基（TIM7） |
| `configMAX_PRIORITIES` | 56 | 优先级范围 0-55 |
| `configTOTAL_HEAP_SIZE` | **36KB** | heap_4 static `ucHeap` → SRAM `.bss`；水位见 06/04 |
| `configAPPLICATION_ALLOCATED_HEAP` | **0** | 不用 CCM 堆文件；禁止堆指针作 DMA |
| `configMINIMAL_STACK_SIZE` | 128 words (512B) | 最小任务栈 |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | 栈溢出检测（检查栈顶标记） |
| `configUSE_MUTEXES` | 1 | 互斥锁（含优先级继承） |
| `configUSE_COUNTING_SEMAPHORES` | 1 | 计数信号量 |
| `configUSE_OS2_EVENTFLAGS_FROM_ISR` | 1 | ISR 中可操作 EventFlags |
| `configGENERATE_RUN_TIME_STATS` | 1 | 运行时统计（时钟=DWT 周期计数器） |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | 5 | 可调用 FreeRTOS API 的最高中断优先级 |
| `configKERNEL_INTERRUPT_PRIORITY` | 15 | 内核中断最低优先级 |

## 任务清单与同步机制

### 任务栈大小设计考量

| 任务 | 栈 | 优先级 | 创建者 | 职责 | 栈设计依据 |
|---|---|---|---|---|---|
| `scan_task` | 256×4=1KB | Realtime(最高) | `dev_display_start` | 行扫描输出 | 扫屏路径无大栈帧（原 2KB 偏大），仅 event wait + ops 调用 |
| `init_task` | 512×4=2KB | High | `app_boot` | 初始化后退出 | `dev_eth_start`→`sw_board_init`→渲染测试，含 printf 和栈数组（`font_buf[512]`、`text_buf[256]`），曾因溢触发 HardFault，从 256×4 扩容 |
| `frame_dispatch_task` | 256×4=1KB | Normal | `app_dispatch_init` | 帧分发引擎 | 核心调度循环，含 `frame_msg_t`（~1052B），无深层嵌套 |
| `iap_handle_task` | 256×4=1KB | Normal | `app_iap` | IAP 帧处理 | CRC32 校验 + cmd 分派（帧缓冲 static；原 2KB 偏大） |
| `ldi_handle_task` | 256×4=1KB | Normal | `app_ldi` | LDI 帧处理 | 显示内容解析与更新（帧缓冲 static；原 2KB 偏大） |
| `ldi_timer_task` | 256×4=1KB | Normal | `app_ldi` | LDI 定时任务 | 周期性状态上报（原 2KB 偏大） |
| `rs485_task` | 256×4=1KB | Normal | `app_rs485` | RS485 接收循环 | `osMessageQueueGet` → `app_channel_dispatch` |
| `rs232_task` | 256×4=1KB | Normal | `app_rs232` | RS232 接收循环 | 同构 rs485_task |
| `half_sec_task` | 128×4=512B | Low | `init_task` | 500ms 喂狗+LED | 最小合法栈，仅 GPIO+RTC API |
| 各网络通道任务 | 256×4=1KB | Normal | 各模块 | UDP/TCP 收发包 | LwIP netconn API 调用，不含大局部变量 |
| `ethernetif_input` | 256×4=1KB | — | `pl_net.c` | LwIP 收包线程 | LwIP 内部调用 |
| `ethernet_link_thread` | 256×4=1KB | — | `pl_net.c` | 链路状态监控 | 轮询 PHY 状态 |

**栈大小设计原则**：CMSIS-RTOS V2 栈单位为 word（4 字节）。数字偏大：
- 512B（128 words）= CMSIS 最小栈，仅用 API 无局部数组
- 1KB（256 words）= 标准栈，含少量局部变量
- 2KB（512 words）= 大栈，含 `frame_msg_t`（~1052B）、CRC 计算、printf 等

### 任务间同步机制全景

```
init_task ──────────────────────────────────────────────────────→ (osThreadExit 自我销毁)
  ├─ dev_eth_start → pl_net_init → osThreadNew(ethernetif_input)
  │                               → osThreadNew(ethernet_link_thread)
  └─ sw_board_init → app_dispatch_init → osThreadNew(frame_dispatch_task)
                    → dev_display_start → osThreadNew(scan_task)

half_sec_task:  osDelay(500) 循环 ──→ pl_iwdg_refresh / pl_gpio_write

scan_task:
  osEventFlagsWait(s_scan_evt) ←── TIM3 ISR: osEventFlagsSet
  osKernelLock + NVIC_DisableIRQ(TIM4)  (OE/LAT 原子窗口)

frame_dispatch_task:
  osMessageQueueGet(g_ch_queue) ←── rs485_task / rs232_task / udp_connect_task / ...
  rb_lock(rb) → probe → rb_read → osMessageQueuePut(frame_queue[i])
  （取出的 channel_t * 先经 app_channel_get 回验，脏通知丢弃）
  
协议处理任务 (iap/ldi/ah_mqtt):
  osMessageQueueGet(frame_queue[i]) → 处理 → channel_send

UART 通道任务 (rs485_task / rs232_task):
  osMessageQueueGet(rx_queue, {block,offset,len}) ←── UART ISR 空闲中断
  app_channel_dispatch → rb_write → osMessageQueuePut(ch_queue)

网络通道任务 (UDP/TCP):
  osSemaphoreAcquire(sem) ←── pl_net_link_listener (链路断开)
  netconn_recv → app_channel_dispatch → rb_write → osMessageQueuePut(ch_queue)
```

## OCP 虚表模式

项目中所有设备驱动和通道使用统一模式：

```c
// 1. 基类第一个成员必须是 ops 指针
struct dev_display { const dev_display_ops_t *ops; /* 通用参数 */ };
struct dev_storage  { const dev_storage_ops_t  *ops; uint32_t capacity; };
struct channel      { uint8_t ch_id, state; const ch_ops_t *ops; };

// 2. 派生类将基类作为第一个成员，统一命名为 me
typedef struct { dev_display_t  me; } dev_display_1_577_t;
typedef struct { dev_storage_t  me; uint32_t base_addr, sector; } dev_flash_int_t;
typedef struct { channel_t      me; pl_uart_handle_t uart; ... } rs485_ch_t;  // app_rs485.c（rs232_ch_t 同构）
typedef struct { dev_w25qxx_t   me; pl_spi_handle_t spi; ... } dev_w25qxx_inner; // (注：w25qxx直接在结构内)

// 3. container_of 向上转型（Kernel/Inc/container_of.h）
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
```

四个核心虚表：

| 虚表 | 定义位置 | 方法签名 | 实现者 |
|---|---|---|---|
| `dev_display_ops` | `dev_display.h` | `prepare(dev)` / `scan(dev,line)` / `set_row(row)` | `dev_display_1_577.c` / `dev_display_1_260.c` / `dev_display_1_969.c` |
| `dev_storage_ops` | `dev_storage.h` | `init/read/write/erase/capacity(dev,...)` | `dev_w25qxx.c` / `dev_flash_int.c` |
| `ch_ops` | `app_dispatch.h` | `send(ch, data, len)` | `app_rs485.c` / `app_rs232.c` / `app_udp.c` / TCP/MQTT 通道 |
| `proto_probe_fn_t` | `app_dispatch.h` | `probe(ch, rb, &total_len, &aux)` | `app_iap.c` / `app_ldi.c` / `app_qh_proto.c` / `app_rls.c` / `ah_mqtt.c` |

## Display 子系统 (`Device/Display/`)

### 基类 (`dev_display.h/.c`)

```c
struct dev_display {
    const dev_display_ops_t *ops;
    // 模组参数
    uint8_t module_rows, module_cols, channels_per_module;
    uint8_t modules_per_row, modules_per_col, scan_lines;
    // 派生参数（由模组参数计算）
    uint16_t screen_rows, screen_cols;
    uint8_t  total_channels;
    uint16_t channel_pixels, scan_line_pixels, buffer_size;
    // CCMRAM 缓冲区（派生实例静态分配）
    uint8_t *pixel_map, *hub75_buff;
    // 运行时
    volatile uint8_t light_level;  // 0-7
    volatile bool dirty;
};
```

通用 API：`set_pixel`、`fill`、`draw_bitmap`（边界检查+MSB-first 位图写入）、`set_brightness`。

### 现行模组 (`dev_display_1_577.c`，EIDE Debug 主例)

单模块 64×32 像素，2 通道/模块，**3×3 拼屏 → 192×96** 屏幕，1/8 扫描（`MODULE_CODE "1000000577"`）。`dev_display_1_260.c` 为 Makefile 默认模组；`dev_display_1_969.c` 为备用模组（CCM 占用同量级 38208B）；`dev_display_p20.c` 未编入任何构建，仅保留。

`g_bsrr[TOTAL_CHANNELS][8]` CCMRAM 预计算查表：每通道×8 色阶 = `_1_577_bsrr_t`（含 r/g/b 三组 `pl_hub75_bsrr_t`），模组 init 时根据 HUB75 引脚定义填入。扫描时直接查表输出，无需分支。

### 扫描架构

```
TIM3 (行同步, 频率=刷新率×scan_lines)
  └─ ISR: osEventFlagsSet(s_scan_evt)
       └─ scan_task (osPriorityRealtime, 最高优先级, 不可抢占):
            1. osEventFlagsWait(s_scan_evt)  ← 阻塞，零 CPU 占用
            2. if(dirty) ops->prepare(dev)   ← pixel_map → hub75_buff
            3. ops->scan(dev, scan_line)     ← BSRR 查表 + CLK 脉冲
            4. OE/LAT 原子窗口               ← osKernelLock + 关TIM4中断
            5. scan_line = (scan_line+1) % scan_lines

TIM4 (PWM, 频率=TIM3×8, 8级亮度):
  └─ ISR: OE=(pwm_cnt >= light_level), pwm_cnt=(pwm_cnt+1)&7
```

**实时性保证**：
- `scan_task` 设为 `osPriorityRealtime`，不会被任何用户任务抢占
- OE/LAT 窗口内 `osKernelLock()` + `NVIC_DisableIRQ(TIM4_IRQn)` 防止 TIM4 抢占破坏 OE 时序
- `prepare` 在 OE/LAT 窗口外完成（off critical path），扫描本身只查表

### 颜色系统

```c
typedef enum { COLOR_BLACK=0, COLOR_RED=1, COLOR_GREEN=2, COLOR_YELLOW=3,
               COLOR_BLUE=4, COLOR_PURPLE=5, COLOR_CYAN=6, COLOR_WHITE=7 } display_color_t;
```
颜色值直接写入 `pixel_map[]`，在 prepare 阶段重新映射——像素重排后写入 `hub75_buff[]`，扫描时查 `g_bsrr` 得到三通道 BSRR 输出值。

## IO 子系统 (`Device/IO/`)

### 按键设备 (`dev_key.c`)

两阶段初始化：`hw_dev_initcall` 注册 EXTI 下降沿回调，`sw_dev_initcall` 创建 TEST 信号量。

| 按键 | GPIO | 类型 | 检测方式 | 用途 |
|---|---|---|---|---|
| SW1 | PE12 | 干接点 | EXTI ↓ + `app_key` 轮询释放 | 通用输入 |
| SW2 | PE11 | 干接点 | EXTI ↓ + `app_key` 轮询释放 | 通用输入 |
| SW3 | PE10 | 干接点 | EXTI ↓ + `app_key` 轮询释放 | 通用输入 |
| KEY_TST | PD8 | 测试键 | EXTI ↓ → 信号量（一次性消耗） | `app_key_test_pressed()` |
| DIP1 | PE7 | 拨码开关 | `app_key` 纯轮询 (3 次一致去抖) | 波特率选择：ON=115200 / OFF=9600（`app_uart_baud`，RS232+RS485 同步） |
| DIP2 | PE8 | 拨码开关 | `app_key` 纯轮询 (3 次一致去抖) | 配置选择 |

所有按键 GPIO 内部上拉，低有效（按下=0）。

**数据流**：
```
硬件按下 → EXTI ISR → dev_key_get_state=标记为 true
                              ↓
app_key key_poll_task (20ms) → 轮询 GPIO 实际电平确认释放
                             → dev_key_clear_state() 清除标记
                             → 更新本地去抖缓存
```

外部通过 `app_key_get_state(DEV_KEY_SW1)` 获取去抖后的稳定状态。`_app_key_init()` 由 `sw_app_initcall` 自动创建轮询任务。

### IO 控制 (`dev_io_ctrl.c`)

`hw_dev_initcall` 初始化 GPIO 输出（PD14 车道灯、PD15 闪光灯），确保启动时均为低电平。

| 函数 | GPIO | 功能 |
|---|---|---|
| `dev_io_lane_light(bool)` | PD14 | 车道灯开关 |
| `dev_io_flash_light(bool)` | PD15 | 黄闪灯开关 |

### 光传感器 (`dev_light_sensor.c`)

ADC1 采样光敏电阻分压（LDR：光越暗→电阻越大→ADC 值越高），8 次均值滤波后映射为亮度等级。`dev_light_sensor_auto_adjust()` 自动更新 `display->light_level`。

**亮度范围限制**：`light_sensor_dev_t` 含 `min_level`/`max_level` 字段（默认 1~7），`dev_light_sensor_read()` 输出前按范围钳位。`app_light_sensor_init()`（`sw_app_initcall`）上电时调用 `dev_light_sensor_set_range(4, 7)`，将光敏自动调光限制在 4~7 级（满足出厂最低/最高亮度要求）。外部可通过 `app_light_sensor_set_range(min, max)` 运行时动态调整。

`app_light_sensor_init`（`sw_app_initcall`）创建 1 秒周期的自动调节任务。

## Storage 子系统 (`Device/Storage/`)

OCP 虚表，上层通过便捷内联调用（如 `dev_storage_read(d, addr, buf, len)` 自动解引用 `d->ops->read`）：

### W25Qxx (`dev_w25qxx.c`)

SPI NOR Flash，JEDEC 自动识别容量（W25Q16~256+），>128Mb 自动 4 字节地址。

**半双工 SPI 读流程**（`_read`）：
```
1. pl_spi_transmit(cmd, addr_len+1)   阻塞发 Read 命令 + 地址
2. pl_spi_receive_dma(buf, len)       DMA 数据→buf，CPU 不参与搬运
3. while(!s_ok) osDelay(1)            轮询 DMA 完成标志
4. _cs_high()                         SPI 释放
```

- DMA 回调 `_dma_cb`：`s_ok=true` + `osEventFlagsSet`（用于定时等待的场景）
- 首次 `_read` 时懒初始化 `s_evt` + 注册 DMA 回调
- 写操作使用读-改-写模式（`_read` 整扇区→修改→`_write_no_check` 整扇区），需先擦除非空扇区

### 内部 Flash (`dev_flash_int.c`)

OCP 虚表实现，提供 `flash_int_ops`（`init/read/write/erase`），由各配置模块绑定到具体实例。

| 实例 | 绑定模块 | 基地址 | 大小 | initcall |
|---|---|---|---|---|
| `g_board_cfg_flash` | `Application/Src/Config/app_board_net_cfg.c` | `0x08004000` (Sector 1) | 16KB | `hw_dev_initcall` |
| (LDI 已迁移) | `Application/Src/LDI/app_ldi_cfg.c` | W25Qxx 最后 4KB 扇区 | 4KB | `sw_dev_initcall` (需要 W25Qxx 先就绪) |

**内部 Flash ops 实现**：
- `_read`：直接内存映射 `memcpy(buf, (void*)(base+addr), len)`
- `_write`：`pl_flash_unlock → pl_flash_program_word × N → pl_flash_lock`（按 word 编程）
- `_erase`：`pl_flash_erase_sector(sector, voltage)`

**板级系统配置** (`Application/Src/Config/app_board_net_cfg.c`)：`app_board_sys_info_t` 记录（72B，方案 B 2026-08-21）= magic(4) + update_sta(4) + FWInfo(40) + NetConfig(20) + CRC32(4)，布局由头文件 `static_assert` 锁定（与 Bootloader/Recovery 二进制兼容）。NetConfig 字段语义：`ip/mask/gw` 两套口共享；`port` = **TCP 业务口**（LDI 0AH / IAP 0x01 报 0x02 写 / TCP Server 监听，出厂默认 9528）；`udp_port` = **CQ UDP 业务口**（CQ setip 写 / CQ UDP 通道读，出厂默认 20103）。网络配置经 `app_board_net_cfg_get` / `app_board_net_cfg_update(ip,mask,gw,port,udp_port)` 读写（LDI 0AH / IAP 4B02 写 port 保留 udp_port；CQ setip 写 udp_port 保留 port），FWInfo/update_sta 经 `app_board_net_cfg_read` 整记录读取（IAP 命令用）。**升级兼容**：旧 68B 记录按新 72B 布局重算 CRC 必失配 → 走空/损坏自愈路径重写（网段配置丢失一次，由 ldi_ctx_init W25 镜像回写或 app_net_boot 默认落盘恢复）；**反向同理**——设备上旧 68B 布局 Bootloader/Recovery 读新 72B 记录同样失配 → 反复重建旧布局出厂记录，**方案 B 后三固件必须同步烧录（`tool/flash_all.sh` 一次会话），否则 Sector1 两布局交替重建、设备可能陷 Recovery（网络全部无响应）**（2026-08-21 纪律强化）。

**LDI 配置** (`Application/Src/LDI/app_ldi_cfg.c`)：已迁移至 W25Qxx 最后一个 4KB 扇区。`app_flash_ldi_record_t`（116B）= magic(4) + cfg(106) + padding(2) + CRC32(4)。`_app_flash_ldi_storage_init`（`sw_dev_initcall`）中 `s_ldi_base = dev_storage_capacity(w25) - 4096`，通过 `dev_w25qxx_get()` 获取存储句柄。

> **解耦已落地（2026-08-14）**：LDI 与 IAP 协议均经中立模块 `app_board_net_cfg`（`Application/Config`）读写 Sector 1 网络配置，互不依赖对方协议目录；`app_iap_cfg.{c,h}` 与死代码 `Device/Inc/config_info.h` 已删除。守卫语义（空/损坏自愈、升级中间态仅放行 net_cfg 字段更新、同值跳过）见 `doc/07_LDI与IAP配置解耦`。

## Render 引擎 (`Application/Src/app_render.c`)

数据驱动字库引擎，字库存储在 W25Qxx Flash 中，模块 `sw_app_initcall` 自注册。

### 字库组织
- 字库 = **(字号, 编码, 字型)** 三元组，由 `font_chip_config_t` 芯片配置描述（`s_chip_configs[]`，当前激活 W25Q64；区块表按芯片配置动态取用，非固定 40 项全局表）
- `_packed_glyph_bytes(size, charset)` = 每字符打包字节数：ASCII `size×((size/2+7)/8)`，GBK `size×((size+7)/8)`
- `_glyph_width_px(size, charset)` = 字符像素宽：ASCII `size/2`，GBK `size`
- `_find_region(size, charset, type)` = 在当前激活芯片配置的区块表中按三元组查找（未命中回退首项）
- `_flash_addr(size, charset, type, ch)` = 单字符 Flash 绝对地址：`char_idx×bpc` 后按 sec/page/byte 分步计算，含芯片级 sec/page/byte 修正（ASCII 原始码/减 0x20、GBK 190 列/94 列按芯片配置区分）

### Tagged Union API

```c
app_render(&(render_cfg_t){
    .type = RENDER_TEXT, .x=0, .y=0, .w=128, .h=16, .color = COLOR_GREEN,
    .text = "你好", .len = strlen("你好"),
    .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_UTF8,
});
```

- `RENDER_TEXT`：UTF8→GBK→逐字 Flash 地址→`dev_storage_read`→`dev_display_draw_bitmap`。支持 `word_wrap` 换行，不换行时截断
- `RENDER_BITMAP`：直接调用 `dev_display_draw_bitmap`
- `RENDER_FILL`：`w=h=0` 全屏填充，否则矩形填充
- 渲染风格 `render_style_t`：`h_align` / `v_align` / `word_wrap`（文字专属）。测量趟+渲染趟两阶段，支持逐行独立水平对齐（左/居中/右）。

## Application 模块全景

| 模块 | 类型 | 通道绑定 | initcall | 职责 |
|---|---|---|---|---|
| `app_dispatch` | 框架 | — | `sw_app_initcall` | 协议调度引擎 |
| `app_render` | 引擎 | — | `sw_app_initcall` | 字库渲染 |
| `app_iap` | 协议 | UDP（RJ45 共享 RB） | `sw_app_initcall` | IAP 固件升级帧处理 |
| `app_ldi` | 协议 | TCP_SERVER / TCP_CLIENT / UDP（RJ45 共享 RB） | `sw_app_initcall` | LDI 显示控制协议 |
| `app_qh_proto` | 协议 | RS485 + RS232 | `sw_app_initcall` | 青海高速费显协议 |
| `app_sc_etc` | 协议 | RS485 + RS232 | `sw_app_initcall` | 四川 ETC 费显协议（0A 帧族；心跳超时显示已停用） |
| `app_sc_mtc` | 协议 | RS485 + RS232 | `sw_app_initcall` | 四川 MTC 费显协议（'{' 方案二 + 0A 46 查询 + 7B 40~45） |
| `app_sc_ol` | 协议 | RS485 + RS232 | `sw_app_initcall` | 四川治超屏协议（FF+len 帧族，BCC 异或） |
| `app_yn_proto` | 协议 | RS485 + RS232 | `sw_app_initcall` | 云南费显协议 |
| `app_cq_proto` | 协议 | UDP_CQ（业务 20103）+ UDP（搜索 10011，RJ45 共享 RB） | `sw_app_initcall` | 重庆高速二代费显协议（JSON `{` + 12B 二进制；队列/缓冲置 CCMRAM；**心跳故障屏开关 `CQ_FAULT_SCREEN`（1 开 / 0 关）：PROTO_CHONGQING 默认开、共存构建默认关（他省上位机不发 syn1），可 `-DCQ_FAULT_SCREEN=1/0` 覆盖，见 doc/03 PartB B.7.2；旧名 CQ_FAULT_SCREEN_ENABLED / CQ_FAULT_SCREEN_FORCE_ON 已废弃**） |
| `app_uart_baud` | 应用 | — | `sw_app_initcall` | DIP1 波特率选择与运行态切换（RS232+RS485） |
| `app_rls` | 协议 | RS485 | `sw_app_initcall` | RLS 重庆高速二代费显协议 |
| `app_vms_ctrl` | 协议 | (LDI 子模块) | — | VMS 情报板控制（LDI→Render 桥接） |
| `ah_mqtt` | 协议 | MQTT | (已注释) | AH 平台 MQTT（签到/状态上报/指令） |
| `app_mqtt` | 通道 | CH_ID_MQTT | — | MQTT 网络传输通道 |
| `app_udp` | 通道 | CH_ID_UDP | — | UDP 广播通道（端口 10011） |
| `app_udp`（CQ 实例） | 通道 | CH_ID_UDP_CQ | — | CQ 业务口 UDP 通道（`PROTO_CHONGQING` 读 Sector1 net_cfg.udp_port 默认 20103（方案 B 2026-08-21），dev 共存构建固定 20103；`app_udp_cq_start`/`app_udp_cq_broadcast`） |
| `app_tcp_server` | 通道 | CH_ID_TCP_SERVER | — | TCP 服务器通道 |
| `app_tcp_client` | 通道 | CH_ID_TCP_CLIENT | — | TCP 客户端通道 |
| `app_rs232` | 通道 | RS232（USART3） | — | RS232-0 通道启动（USART6 语音仅 TX，不启通道） |
| `app_rs485` | 通道 | RS485 | — | RS485 UART 通道启动 |
| `app_key` | 应用 | — | `sw_app_initcall` | 按键轮询去抖 (20ms) |
| `app_light_sensor` | 应用 | — | `sw_app_initcall` | 光传感器自动亮度 (1s 周期) |
| `app_boot` | 编排 | — | — | RTOS 启动编排 |
| `app_net_boot` | 应用 | — | — | 网络配置横切应用（Sector1 net_cfg → netif + TCP Server 口；空/损坏/非法写本构建默认落盘并应用，两口径共用） |
| `app_default_display` | 应用 | — | — | 注册制默认显示界面（协议 sw_initcall 注册回调则用之，否则默认欢迎画面） |
| `app_test` | 测试 | — | — | 硬件测试用例 |

**类型说明**：
- **协议**：注册 probe 到 dispatch，创建处理任务，从 frame_queue 收帧
- **通道**：实现 `ch_ops->send`，收包→`app_channel_dispatch`，发包→网络/UART
- **框架/引擎**：供协议模块调用，不直接通信
- **应用**：硬件相关业务逻辑，不涉及通信协议

**当前运行状态**：`app_boot` 的 `init_task` 显式启动五个通道 — TCP Server、TCP Client、UDP、RS485、RS232。`app_rs232_1_start()`（USART6 语音 TX 桩）已注释；`app_mqtt` 的 start 函数已定义（`static inline`）但未被调用；`ah_mqtt` 的 initcall 已注释（未激活）。

## 协议分发层 (`Application/Src/app_dispatch.c`)

### 核心数据结构 `dispatch_ctx_t`

| 字段 | 类型 | 用途 |
|---|---|---|
| `registered_mask` | `uint32_t` | 已注册协议位掩码（`app_proto_register` 自动分配 2 的幂） |
| `proto_rb[i]` | `ring_buffer_t *[PROTO_MAX_COUNT]` | 协议→缓冲区 |
| `proto_probe[i]` | `proto_probe_fn_t [PROTO_MAX_COUNT]` | 协议→探测函数 |
| `frame_queue[i]` | `osMessageQueueId_t [PROTO_MAX_COUNT]` | 协议→帧队列 |
| `buf_pool[id]` | `ring_buffer_t *[RB_CNT_MAX]` | 缓冲区池（=3 槽：RJ45/RS485/RS232，懒初始化） |
| `ch_queue` | `osMessageQueueId_t` | 通道通知队列 |
| `ch_proto_map[ch_id]` | `proto_mask_t [CH_ID_MAX]` | 通道→协议掩码 |
| `channels[ch_id]` | `channel_t *[CH_ID_MAX]` | 通道注册表 |

### 数据流

**接收**：通道任务 → `app_channel_dispatch(ch,data,len)` → `rb_write` → `osMessageQueuePut(ch_queue,&ch)` → `frame_dispatch_task` → probe → `rb_read` → `osMessageQueuePut(frame_queue[i],&msg)` → 协议任务

**发送**：协议任务 → `channel_send(ch,data,len)`（入口回验 `app_channel_get(ch->ch_id)==ch`，防悬垂通道指针）→ `ch->ops->send(ch,data,len)` → UART DMA / LWIP netconn

### 多协议共享 RB（doc/05 核心成果）

- **一物理通道一 RB**：RJ45 1536 / RS485 768 / RS232 768（`Application/Inc/app_dispatch.h` `_Static_assert` 在册）。网口逻辑通道（TCP Server/Client/UDP/MQTT）共享 RJ45 槽。
- **RB_PROVIDE_WEAK 编译期按需提供**：协议 TU 以 `RB_PROVIDE_WEAK(rb_provide_xxx, size)` 提供 RB 体；未编入任何协议的槽为 NULL（`app_proto_acquire_buf` 返回 nullptr）。
- **同 RB 写去重**：`app_channel_dispatch` 用 `seen[]` 指针去重，多协议共享同一 RB 时只写一次。
- **链式 probe 契约**：READY/SKIP 消费后取下一帧；WAIT/FAKE 继续试下一协议；一轮无消费时 any_wait 停等更多字节 / any_fake 跳 1 字节重同步。
- **probe 首字节快拒**：探测函数首字节不匹配即返回 FAKE，禁止盲 WAIT 阻塞链式探测。

**帧 queue 深度（静态 SRAM 队列，不占 ucHeap）**：IAP 2 / LDI 4 / QH 3 / RLS 2 / AH_MQTT 3 / SC_ETC 3 / SC_MTC 3 / SC_OL 3 / SD 3 / GZ 3 / CQ 3（**CQ 队列体置 CCMRAM**，见上）。

### 新增协议步骤

1. 定义 `proto_probe_fn_t` 探测函数，返回 `PROTO_PROBE_READY/WAIT/SKIP/FAKE`
2. 协议模块 init 函数中：`app_proto_acquire_buf`→`app_proto_register`→`app_proto_bind_channel`（可多次）→`osThreadNew`
3. 协议任务中：`osMessageQueueNew`→`app_proto_set_frame_queue`→循环 `osMessageQueueGet`→处理
4. `sw_app_initcall(module_init)` 自注册

**新协议接入的完整规则、模块骨架、串口/网络专项与 step-by-step 检查清单见 `doc/08_协议模块接入规则/`**（本列表仅为摘要；RB/绑定/兼容矩阵权威仍为 doc/05，内存占用账仍为 doc/06）。

### 通道标识映射

| ch_id | 枚举 | 传输层 | 实现类 | 绑定文件 |
|---|---|---|---|---|
| 0 | `CH_ID_RS485` | UART (USART1) | `rs485_ch_t` | `app_rs485.c`（buf 在 `dev_rs485.c`） |
| 1 | `CH_ID_RS232` | UART (USART3) | `rs232_ch_t` | `app_rs232.c`（buf 在 `dev_rs232.c`） |
| 2 | `CH_ID_TCP_SERVER` | LwIP TCP | `tcp_server_channel_t` | `app_tcp_server.c` |
| 3 | `CH_ID_TCP_CLIENT` | LwIP TCP | `tcp_client_channel_t` | `app_tcp_client.c` |
| 4 | `CH_ID_UDP` | LwIP UDP | `udp_channel_t` | `app_udp.c` (端口 10011) |
| 5 | `CH_ID_MQTT` | LwIP MQTT | `mqtt_channel_t` | `app_mqtt.c` |
| 6 | `CH_ID_RS232_1` | UART (USART6，仅语音 TTS 旁路 TX) | `dev_rs232_voice`（直接 `pl_uart_send`） | 无通道任务、无 DMA RX，禁止协议 bind |
| 7 | `CH_ID_UDP_CQ` | LwIP UDP（业务口，`PROTO_CHONGQING` 读 Sector1 net_cfg.udp_port 默认 20103（方案 B）；dev 共存构建固定 20103） | `udp_channel_t`（CQ 实例） | `app_udp.c`（`app_udp_cq_start`；CQ 协议 bind，JSON 业务 + 12B 二进制） |

**选编口径**：EIDE Debug 编 1-263 模组 + IAP/LDI/四川三协议（`ProtocolParser_SiChuang_{ETC,MTC,Overload}`）/重庆CQ（`ProtocolParser_ChongQing` + cJSON，Debug 未排除），排除 1-260/1-969/1-577/RLS/AH/山东/贵州/青海/云南（`.eide/eide.yml` excludeList；LDI 与 CQ 同编为开发共存口径：CQ 业务口 dev 构建固定 20103 不读 Sector1 net_cfg.udp_port，10011 口 12B 二进制帧先经 LDI probe，LDI 对 `data_len==2`（CQ 12B 帧 len=00 00 00 02 且 CRC 恰好通过 LDI 校验）显式 FAKE 放行、仍由 CQ 认领，见 doc/03 PartB Q23/Q24；量产 CQ 目标需 excludeList 排除 ldi 并 define PROTO_CHONGQING）；Makefile 编 1-263 + 全协议（含 CQ，`PROTO=CQ` 剔除 LDI 并追加 -DPROTO_CHONGQING；1-260 已随 1-263 收录切换移出 SRC_DEVICE）。详见 doc/05，内存占用见 doc/06。

## UART 通道子系统 (`Device/Comm/` + `Application/Src/Channel/`)

RS485/RS232 按板级资源（Device 层）与通道生命周期（Application 层）分层，全库无统一 UART 通道抽象类型。

**Device 层（板级静态资源）**：
- `dev_rs485.c`（USART1，RE=PA8）：静态 DMA 乒乓双缓冲 `s_rs485_buf[2×RS485_BUF_SIZE=1280B]`（`Device/Inc/dev_rs485.h`，块 640×2）+ RE 方向回调 `rs485_dir_cb`（`pl_uart_set_dir_cb` 注入，`hw_dev_initcall`）。
- `dev_rs232.c`（USART3）：仅 `dev_rs232_get_buf(index)` — index0 返回 1280B 乒乓缓冲（`RS232_BUF_SIZE`=640 块 ×2），index1（USART6）返回 `nullptr`（无协议 RX）。
- `dev_rs232_voice.c`（USART6）：语音板 TTS 专用 TX。`dev_rs232_voice_play` / `dev_rs232_voice_volume` 组帧后经 `pl_uart_send(PL_UART6)` 发送，不经过 dispatch 框架。

**Application 层（通道生命周期 + 任务循环）**：
- `app_rs485.c`：`rs485_ch_t`（`channel_t me` + `uart/rx_queue/rx_buf`），`rs485_task` 循环 `osMessageQueueGet(rx_queue, {块号,偏移,长度})` → 按块拷贝 → `app_channel_dispatch`。`app_rs485_start()` 注册通道 + 注入 RX 回调 + `pl_uart_start_rx` + 创建任务（栈 256×4）。
- `app_rs232.c`：同构（`rs232_ch_t` / `rs232_task`，栈 256×4）。`app_rs232_1_start()` 为桩函数（返回 `nullptr` — USART6 语音 TX 不经通道任务）。
- `app_boot` 中 `app_rs485_start()` / `app_rs232_start()` 创建任务，`app_rs232_1_start()` 已注释。

**波特率选择（DIP1）**：`app_uart_baud.c`（`sw_app_initcall`）读 `dev_key_get_state(DEV_KEY_DIP1)`（PE7，active_low）：ON=115200、OFF=9600，经 `pl_uart_set_baud` 同步重配 USART1（RS485）+ USART3（RS232）。sw initcall 早于 `app_rs485_start`/`app_rs232_start`，切换时 DMA RX 未挂，无竞态；运行态切换（MTC 7B 40 命令）由 `pl_uart_set_baud` 内部停/重挂 DMA RX。DIP2（PE8）为 app_render 字库芯片选择，不得改动。

## IAP 协议 (`Application/Src/IAP/app_iap.c`)

固件升级协议，**仅 UDP**（`CH_ID_UDP`，RJ45 共享 RB）。帧格式 `0x5A5A5A5A (4B) | seq (4B) | cmd (4B) | len (4B) | data | CRC32 (4B)`（`app_iap.h`），probe 首字节 `0x5A` 快拒。

- `iap_handle_task`：协议处理任务（栈 256×4=1KB，帧缓冲 static；原 2KB 偏大），循环 `osMessageQueueGet` → 帧解析 → 命令分派
- `IAP_QUEUE_DEPTH=2`，静态 SRAM 队列（不占 ucHeap）
- 命令表 `g_iap_cmd_table[]`（`app_iap_cmd.c`）：0x00 Test / 0x01 上报 IP 配置 / 0x02 强制修改 IP（写 Flash）/ 0x03 上报固件版本·大小·CRC32·升级状态（version 由主固件启动时经 `app_board_net_cfg_fw_version_update` 从 PROGRAM_CODE 落库 Sector1 `app_info.version`，见 doc/07 §13）/ 0x04 准备升级 / 0x05 发送升级包 / 0x06 进 Recovery（写 RTC 备份寄存器标志）/ 0x07 软复位
- **0x03 应答 version 字节序**：载荷 11 word（ReData[0]=size、[1]=crc32、[2..9]=version[32] ASCII、[10]=update_sta）；version 按**大端 word 构造**（对齐 0x01 IP 约定：`v[4i]<<24|v[4i+1]<<16|v[4i+2]<<8|v[4i+3]`），存储侧保持纯 ASCII，禁 memcpy 裸拷（否则上位机按 4 字节一组反转显示）。主固件与 Recovery `cmd.c` 同构。
- `iap_probe_frame`：验证帧头 + 长度（≤256）+ CRC32，返回 READY/WAIT/FAKE

**0E 远程升级协议文档待完善项**（2026-08-14，协议文档与固件实现的一致性缺口，需在协议文档侧补齐）：

| # | 缺口 | 现状 |
|---|------|------|
| 1 | **4B02 应答帧结果码** | 协议文档当前版本无此定义；固件已按扩展实现：len 0→1，1 word 结果码（0=成功含同值跳过，1=擦写错误），借鉴 4B04 应答 0/1 先例。老上位机按「B402 无载荷」解析时会多读一个 word，混合部署需联调验证（详见 doc/07 §10） |
| 2 | **4B06「进 Recovery」应答后重启** | 协议要求应答后重启；主固件实现只写 RTC 备份寄存器标志（`FLAG_FORCE_UPDATE`）未复位，实际复位由后续流程触发——协议与实现出入 |
| 3 | **4B04「准备升级」应答结果码** | 协议文档有 0/1 结果码先例；主固件实现应答无载荷（len=0），未按文档回结果码 |
| 4 | **LDI 0AH 失败码 01H** | 实现已使用（00H=成功、01H=失败：W25 保存或 Sector1 同步任一步失败即 01H），协议文档需明确该语义（与 0BH / `ldi_status_rsp_t` 既有约定一致） |

## LDI 协议 (`app_ldi.c` + `app_ldi_cmd.c`)

车道设备指示器协议，**仅网口**：`CH_ID_TCP_SERVER / CH_ID_TCP_CLIENT / CH_ID_UDP` 三通道各独立 mask，共用 RJ45 RB 与 `g_ldi_msg_queue`（`LDI_QUEUE_DEPTH=4`）。帧 STX `0xFF 0xFF`（`app_ldi.h` 固定值）+ CRC-16/XMODEM 校验；`LDI_PAYLOAD_MAX=512`。

- `ldi_handle_task`：协议处理任务（栈 256×4=1KB，帧缓冲 static；原 2KB 偏大）
- `ldi_timer_task`：周期性状态上报（栈 256×4=1KB；原 2KB 偏大）
- 13 种设备类型（`Application/Inc/LDI/app_ldi.h`，`LDI_DEV_TYPE_COUNT=13`）：RSU 0xE1 / LPR 0xE2 / VTR 0xE3 / 栏杆机 0xE4 / 车检 0xE5 / 显示屏 0xE6 / 信号灯 0xE7 / 报警器 0xE8 / VMS 0xE9 / 雨棚灯 0xEA / 雾灯 0xEB / 语音 0xF4
- `ldi_ctrl_xxx_t`：每种设备类型的控制载荷结构体（`app_ldi_cmd.h`）
- `app_vms_ctrl`：VMS 情报板子模块，将 LDI 命令翻译为 `app_render()` 调用
- 复合指令模块：支持多设备类型组合控制（1BH 命令）

## 青海协议 (`Application/Src/ProtocolParser_QingHai/app_qh_proto.c`)

青海高速费显协议。帧定界 `'{'...'}'`（len 字段 1B → 帧 ≤259），`QH_PAYLOAD_MAX=259`、`QH_QUEUE_DEPTH=3`（静态 SRAM）。`RB_PROVIDE_WEAK` 提供 RS485+RS232 双 RB，绑定 `CH_ID_RS485` + `CH_ID_RS232`（`sw_app_initcall` 自注册 `qh_proto_init`，与 IAP/LDI 同经链式 probe）。语音命令经 `dev_rs232_voice` 旁路 USART6（`app_qh_proto_voice.c`）。

## 山东协议 (`Application/Src/ProtocolParser_ShanDong/app_sd_proto.c`)

山东车道费额显示器通信协议（协议文档编号 39）。帧 `'{' + 命令字('1'~'5','7','8') + 二进制 len + 参数 + '}'`（无 BCC；**与青海完全同构**），`SD_PAYLOAD_MAX=259`、`SD_QUEUE_DEPTH=3`（静态 SRAM，与青海同 801B）。绑定 `CH_ID_RS485` + `CH_ID_RS232`（`sw_app_initcall` 自注册 `sd_proto_init`）。目标屏幕 192×96（FONT_16 6 行，协议行号 1~5 全覆盖）。

- `'1'` 全屏单色（01红/02绿/03黄）→ 整屏 `dev_display_fill`；`'2'` 取版本号 → 裸 ASCII 应答 PROGRAM_CODE（协议未定义应答格式）；`'3'` 单行（颜色'0'~'2' + 行号'1'~'5' + GBK 文本，先清行再渲染）；`'4'` 全屏可编辑（颜色 + X/Y 坐标 + 文本，先清屏后整屏 word_wrap，0x0A 回车由渲染引擎换行、0x0D 被跳过）；`'5'` 清屏；`'7'` 亮度（'0'~'5'：0=恢复光敏任务自动调光，1~5=挂起光敏任务 + 硬件档 {3,4,5,7,8}）；`'8'` 外设（bit0 绿灯/bit1 红灯/bit2 黄闪报警，红灯优先，PD14/PD15）。无 `'6'` 命令。除 `'2'` 外协议未定义应答 → 不回（对齐青海单向模式）。
- **帧头冲突纪律**：命令字 '1'~'5','7','8' 全部落入青海 probe 命令集（'1'~'9','A','B'），全协议 Makefile 构建下青海 probe 先注册（源码收录序 qh 在前）先认领山东帧——'3'/'4'/'5' 语义与青海巧合一致，'1'/'2'/'7'/'8' 语义分歧（山东 '1'=全屏单色 vs 青海 '1'=主机查询；'2'=版本 vs 自检；'7'=亮度 vs 文明语音；'8'=外设 vs 亮度）。**量产必须 EIDE 目录排除与青海/四川MTC 互斥**（doc/05-01 §6）；当前 `.eide/eide.yml` Debug 目标已排除山东目录（与青海/四川MTC 同列，Debug 保留贵州），山东量产目标须启用山东并排除青海+MTC。编译期互斥守卫 `g_brace_proto_guard` 链接期兜底强制（EIDE 多 `{` 族编入即 `multiple definition` 报错；Makefile 全协议构建经 `-DSTD_ALL_PROTO` 豁免）。
- 上电画面「山东省 高速公路 欢迎您」已实现：`app_sd_proto_default.c` 经 `app_default_display_register` 注册（`sw_app_initcall`），显示「山东省 高速公路 欢迎您」（FONT_16 居中黄字；UTF-8 字面量，`FONT_ENC_UTF8` 渲染）。

## 贵州协议 (`Application/Src/ProtocolParser_GuiZhou/app_gz_proto.c`)

贵州常规费显协议（协议文档 06 贵州常规费显协议，2020-06-17 修订）。帧 `'{' + 命令字('1'~'9','A','B',0x01,0x02) + 二进制 len + 参数 + '}'`（无 BCC；**与青海完全同构**），`GZ_PAYLOAD_MAX=259`、`GZ_QUEUE_DEPTH=3`（静态 SRAM，与青海同 801B）。绑定 `CH_ID_RS485` + `CH_ID_RS232`（`sw_app_initcall` 自注册 `gz_proto_init`）。13 命令：'1' 主机查询（仅回「正常」帧 `7B 31 01 00 7D` 至源通道）、'2' 自检（黄色全屏 + 语音「系统正在加电自检」）、'3' 单行（len 3~18，先清行再渲染）、'4' 全屏可编辑（len 4~86，**按 X/Y 坐标渲染** word_wrap=true，len>71 文本截断 64）、'5' 清屏、'6' 固定格式（客/货行文裸机对齐：客车 车型/金额/余额/信息1，信息2 不显示；货车 有超重→金额/余额/总重/超重、无超重→车型/金额/余额/总重；金额 ≥0.5 元同步费额语音；金额/重量整数运算分/吨分无浮点）、'7' 礼貌用语语音（'0'~'3'，'0'=「您好！欢迎行驶贵州高速公路」）、'8' 亮度（0=恢复光敏任务，1~5=挂起光敏 + 硬件 {3,4,5,7,8}）、'9' 音量（1~5 → 语音板 {1,3,5,7,9}）、'A' 外设（bit0 绿/bit1 红/bit2 黄闪，**红优先**）、'B' 费额语音（金额 ASCII 串→分，≥0.5 元播报，不显示）、0x01 全屏点亮（01红/02绿/**03黄**）、0x02 版本号（串口回裸 ASCII PROGRAM_CODE + 屏幕红字「版本:PROGRAM_CODE」FONT_SELF_ADAPT 居中）。无上电画面（不建 default 文件）。
- **帧头冲突纪律**：命令字 '1'~'9','A','B' 全部落入青海 probe 命令集（**完全重叠**），全协议 Makefile 构建下青海 probe 先注册（源码收录序 qh 在前）先认领贵州帧——'1'/'3'/'4'/'5'/'7'/'8'/'9'/'B' 语义基本一致，'2'/'6'/'A' 细节差异（贵州 '2'=自检黄屏+语音、'6'=固定格式行文不同、'A'=红优先）。**量产必须 EIDE 目录排除与青海互斥**（doc/05-01 §6）；当前 `.eide/eide.yml` Debug 目标已启用贵州（排除青海/山东/四川MTC）。编译期互斥守卫 `g_brace_proto_guard` 链接期兜底强制（EIDE 多 `{` 族编入即 `multiple definition` 报错；Makefile 全协议构建经 `-DSTD_ALL_PROTO` 豁免）。0x01/0x02 二进制命令字青海/山东/四川MTC probe 首命令字快拒，无冲突。金额串转分 `gz_amount_to_fen`（整数部分×100 + 小数前两位，`%.2f` 语义）。宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/gz_frame_sim.py`。

## 云南协议 (`Application/Src/ProtocolParser_YunNan/app_yn_proto.c`)

云南常规费显协议（协议文档 01 云南常规费显协议-云南LED费显P5，2022.7.5，云南弥玉项目 2022-S134）。帧 `'{' + 命令字('1'~'9','A','B',0x01,0x02) + 二进制 len + 参数 + '}'`（无 BCC；**与青海/贵州完全同构**），`YN_PAYLOAD_MAX=259`、`YN_QUEUE_DEPTH=3`（静态 SRAM，与青海同 801B）。绑定 `CH_ID_RS485` + `CH_ID_RS232`（`sw_app_initcall` 自注册 `yn_proto_init`）。13 命令：'1' 主机查询（回「正常」帧 `7B 31 01 00 7D` 至源通道，恒回正常）、'2' 自检（**复用 app_factory_test.c 老化循环显示序列**——整屏单字居中，字号 {FONT_16/24/32}×字体 {ST/FS/KT/HT} 循环「重庆创迪科技发展有限公司设备老化测试」，每 5s 播报「系统正在自检」，一次性任务 `yn_selftest_task` 防重入，**可被下一帧命令打断**，打断退出不清屏）、'3' 单行（len 3~18，**FONT_24 渲染**行高 24px——协议 24 点阵，先清行再渲染；行号 '1'~'5' **全按协议接受**，行 5 在 96px 屏高下「执行但不落屏」——渲染调用照常发出、渲染层越界早退）、'4' 全屏可编辑（len 4~86，**按 X/Y 坐标渲染** word_wrap=true，FONT_24，0x0A 换行 0x0D 跳过）、'5' 清屏、'6' **单行清除**（'1'~'5'；与贵州 '6' 固定格式语义不同）、'7' 礼貌用语语音（'0'~'3'，协议原文云南文案）、'8' 亮度（**0x00=NUL=恢复光敏自动调光、'1'~'8' 恒等映射硬件档并挂起光敏任务**，8 最亮）、'9' 音量（1~5 → 语音板 {1,3,5,7,9}）、'A' 外设（bit0 绿/bit1 红/bit2 黄闪，**红优先**）、'B' 费额语音（金额 ASCII 串→分，**0 元不播**、小数播小数末位 0 剔除，不显示）、0x01 全屏点亮（**01红/02绿/03黄/04蓝/05紫/06青/07白**——DATA0 与 display_color_t 枚举恒等，P5 全彩屏 8 色除黑）、0x02 版本号（串口回裸 ASCII **PROGRAM_CODE**）。**无上电效果**：不建 default 文件、不注册默认显示（使用固件默认显示，用户决定 8）。
- **帧头冲突纪律**：命令字 '1'~'9','A','B' 全部落入青海 probe 命令集（**完全重叠**），全协议 Makefile 构建下青海 probe 先注册（源码收录序 qh 在前）先认领云南帧（与贵州处境一致）。**量产必须 EIDE 目录排除与青海/山东/贵州/四川MTC 互斥**（doc/05-01 §6）；当前 `.eide/eide.yml` Debug 目标已启用四川MTC、**排除云南**（`Application/protocol/ProtocolParser_YunNan`，与青海/山东/贵州同列）。编译期互斥守卫 `g_brace_proto_guard` 链接期兜底强制（EIDE 多 `{` 族编入即 `multiple definition` 报错；Makefile 全协议构建经 `-DSTD_ALL_PROTO` 豁免）。0x01/0x02 二进制命令字青海/山东/四川MTC probe 首命令字快拒，无冲突。金额串转分 `yn_amount_to_fen`。已确认决定记录见 `doc/11_云南费显协议/README.md` §8。

## 四川三协议（`ProtocolParser_SiChuang_{ETC,MTC,Overload}`）

三个四川地区协议模块均绑定 `CH_ID_RS485` + `CH_ID_RS232` 双通道、queue 深度 3（静态 SRAM），与青海同构（acquire → register → bind → set_frame_queue）。协议文档提取自 1D/1E/1F，要点：

- **ETC（1D）**：帧 `0A + 显示方式(00/01) + 行号(00~06) + 数据 + 0D`（数据**变长 0x0D 定界**：单行 ≤24B、全屏 ≤145B，无固定 56B——上限对齐 9K1F212701 etc.c `cmd_etc_disPlay_ctrl` 0x0D 扫描索引 ≤148，GBK）；`0A 36/37/38/39 0D` 灯控 → `dev_io_lane_light`/`dev_io_flash_light` 且同步显示颜色（fontColor 语义）；`0A 40 XX YY 0D` 亮度（XX=00 自动调光）；`0A 50 0D` 心跳（解析保留，识别后丢弃）。应答 `0A 00/01/02 0D`（收到即回，心跳不回）。**0x20 清屏（行号 0 全屏）、0x30 初始化 → 软件复位**。全屏渲染按 9K1F212701 `MakeSixteenLattAll` 语义：清屏后自第 1 行第 1 列按屏宽自动换行。**心跳超时显示已停用（2026-08-17）**：原独立计时任务（栈 256×4，5 分钟无有效帧 → 「ETC车道关闭」）已 #if 0、任务不再创建；黄闪 0A 38 开启后的 10 秒自动关闭依赖同一任务 tick，一并失效——开启后须 0A 39 显式关闭。恢复方法见 `app_sc_etc_proto.c` 注释。
- **MTC（1E 方案二）**：帧 `'{' + 命令('1'~'9','A') + 参数 + '}'`——**'}' 定界变长，无长度字段、无 BCC**（对齐 9K1F212701 mtc.c：各命令 handler 逐字节扫 '}' 定帧，参考扫描上限 228；本设备 probe 上限 `SC_MTC_PAYLOAD_MAX=74B` 队列约束）。字段偏移按变长重算：'3' 单行 = 行号[2]+变长文本[3..]，'4' 全屏 = 变长文本[2..]（先清屏后整屏 word_wrap 渲染：w=screen_rows 屏宽、h=screen_cols 整屏高、word_wrap=true，渲染引擎按当前字号自动折行——2026-08-17 修复，原 16B/行切 ≤4 行且单行不换行，第一行超宽溢出被裁、超 64B 被丢弃；'3' 单行保持 word_wrap=false + h=当前字号截断语义），'6' 固定格式 = 类型[2]+数字串（客车 ≥11B / 货车 ≥20B），'1','2','5'→3B、'7'固定→4B（'78' 自定义文本变长）、'8','9'→4B。带 BCC 变体不加判别：BCC 字节视为内容尾部（与参考语义一致），BCC 不校验。主机查询 `0A 46 0A → 0A 64 0A`、清屏 `0A 46 0D`；附加 7B 40~45 原始帧族（同样 '}' 定界）：40 改波特率（`app_uart_baud_apply`）、41 点阵大小、42 字体、43 协议类型（仅记录）、44 全屏点亮、45 版本号 → `SC_FX_P7.62_1.0`。'6' 字段布局按 9K1F212701 mtc.c。'7' 语音经 `dev_rs232_voice`（固定用语 GBK 直送，动态变量不拼读）。7B 46（1280B 载荷）不实现（超 RB 768）。宿主推演 `~/EnvTools/CD-DebugTool-cpp/scripts/probe_sim/sc_mtc_frame_sim.py`（`--old` 可复现旧定长乱码链）。
- **治超（1F 3.5.1）**：帧 `FF + 长度(07~FF，1 字节；0xFE 显式排除给 RLS) + 命令 + 亮度 + 数据(可变长) + BCC(五字段异或) + FF`；**80 全屏显示、81~88 八行显示（9K1F212701 语义：行数据变长 = 总长-6，≤24B 截断、不足不补空格、先清行再渲染）**、94 清屏、96 亮度（00=自动调光，非 0 按 lightLev=(val+1)/32 映射 1~8 档）、99 通行灯（同步显示颜色）、98 黄闪；查询 A0 → A1~A8 每行独立帧（固定 16B 应答兼容工具，参考项目无应答实现）、B6/B9/B8 → 回显当前状态。**与 RLS 区分**：RLS 第二字节 0xFE(254) 被治超 probe 显式排除（长度上限放宽至 FF 后 0xFE 落入合法区间，不排除会把 RLS 帧吞掉并 WAIT 卡死链式），RLS probe 亦要求第二字节 0xFE，双向快拒成立。
- **帧头冲突纪律**：MTC '{' 与青海 '{' 同帧头——MTC probe 以「'}' 定界变长扫描 + 上限 74B」认领，完整青海帧由青海 probe 先认领（qh_proto_init 先注册，源码收录序 QH 在前）；残余风险两类：①青海 '3' 帧总长 ≤74 且数据段含 '}' 字节（GBK 尾字节可为 0x7D）且半帧到达时被 MTC 截断认领，②青海 'A'/'B' 空数据帧与 MTC 7B 41/42 同长（QH probe 先认领）；量产由 EIDE 目录排除纪律兜底（doc/05 §6）；编译期互斥守卫 `g_brace_proto_guard` 链接期兜底强制（EIDE 多 `{` 族编入即 `multiple definition` 报错；Makefile 全协议构建经 `-DSTD_ALL_PROTO` 豁免）。

## RLS 协议 (`Application/Src/RLS/app_rls.c`)

重庆高速二代费显协议。帧头 `0xFF 0xFE` + 尾 `0x0D 0x0C`，`RLS_PAYLOAD_MAX=530`（帧头 6B + bitmap 512B + BCC 1B + 尾 2B）、`RLS_QUEUE_DEPTH=2`。仅绑定 RS485（`RB_PROVIDE_WEAK` 提供 RS485 RB），BCC 校验用 `Kernel/Inc/bcc_utils.h`（`bcc_calcu`）。`sw_app_initcall` 自注册 `rls_module_init`，`rls_handle_task` 栈 256×4。

## AH MQTT 协议 (`ah_mqtt.c` + `ah_mqtt_cmd.c`)

AH 平台 MQTT 应用层协议。固定帧格式（21B~533B），通过 MQTT topic 路由命令。

**数据结构**（`ah_mqtt.h`，均为 packed 结构）：
- `topic_info_t`：`station_hex(8) + lane_hex(2) + device_type(2) + device_id(2)`
- `notify_id_t`：日期+设备信息+发送计数
- `sign_up_t`：设备签到（含软硬件版本号、协议版本、厂商信息）
- `state_report_t`：定期状态上报

**命令处理器**（`ah_mqtt_cmd.h`，通过 `g_ah_mqtt_cmd_table[]` 跳转）：
- `/ASK/board/NULL` → `cmd_display`：UTF8 文本渲染
- `/ASK/display/clean` → `cmd_fill`：全屏填充颜色
- `/ASK/op/restart` → `cmd_restart`：应答后 `NVIC_SystemReset()`
- `/ASK/op/checktime` → `cmd_checktime`：更新时间戳

**注意**：模块 initcall 当前已注释（`// sw_app_initcall(ah_mqtt_module_init)`），`app_mqtt_start()` 也未调用，AH MQTT 链路未激活。

## 网络子系统

### PHY→MAC→LwIP 依赖链

```
Device 层:  dev_dp83848  (PHY 寄存器操作, 自动协商, 链路状态检测)
              └─ 注入 IO 上下文 (pl_eth_phy_io_*)
Platform 层: pl_eth  (ETH MAC/DMA, MDIO 读写)
              └─ 注册 PHY 链路查询回调 (pl_eth_set_phy_link_fn)
             pl_net  (LwIP netif 初始化, IP 配置, 链路监听器注册)
              └─ ethernet_link_thread 轮询链路状态 → 通知 pl_net_link_listener[]
Application: app_udp / app_tcp_server / app_tcp_client / app_mqtt
              └─ pl_net_register_link_listener → 链路断开回调重建连接
```

### 通道生命周期（以 UDP 为例）

```
udp_task: 绑定端口→循环{创建 udp_connect_task→等待信号量→销毁→延迟重连}
  └─ udp_connect_task (每个客户端一个):
       1. udp_channel_init: ch.ops=udp_ch_ops, state=UP, app_channel_register
       2. 循环 netconn_recv → 提取源IP/端口 → app_channel_dispatch
       3. udp_channel_deinit: ops=nullptr, state=DOWN, conn=NULL, app_channel_register(NULL)
       4. osSemaphoreRelease 通知 udp_task 重连
```

**IP 隔离**：`udp_channel_t` 存储源 IP 为 `uint8_t src_ip[4]` 字节数组，而非 LwIP `ip_addr_t`，避免 Application 层暴露 middleware 类型。

### dev_dp83848 PHY 驱动 (`Device/Network/dev_dp83848.c`)

PHY 寄存器操作、自动协商、链路状态检测。通过 IO 上下文注入（`pl_eth_phy_io_*` 函数指针）与 MAC 层解耦。`dev_dp83848.h`（~249 行）定义完整 PHY 寄存器映射和 15+ 操作 API。

### LwIP 配置要点 (`Platform/Inc/lwipopts.h`)

关键参数（139 行，覆盖 `opt.h` 默认值的部分与 CubeMX 生成配置）：
- **内存**：`MEM_SIZE=12KB`；`MEMP_NUM_PBUF=16`、`PBUF_POOL_SIZE=16`（未覆盖，取 `opt.h` 默认值；SRAM 水位 87-94%，每 +16 池约 +4KB，另行核算后才可调）
- **netconn/UDP PCB 池（2026-08-21 覆盖，修复 LDI 搜索广播丢包）**：`MEMP_NUM_NETCONN=8`、`MEMP_NUM_UDP_PCB=8`（opt.h 默认 4）——dev 共存构建 baseline 4 netconn 已满（UDP 10011 + UDP 20103 + TCP Server listener + TCP Client），广播临时 `netconn_new` 必然 NULL 静默失败（LDI 12H 搜索应答 50-75% 无响应）；扩到 8 留余量，bss 增量 304B。**新增 UDP 端口协议时仍须随通道数核算（doc/06 预算）**
- **硬件校验和**：`CHECKSUM_GEN_IP/UDP/TCP=0`、`CHECKSUM_CHECK_*_HW` 卸载到 MAC
- **UDP 广播**：`LWIP_BROADCAST` 未定义；实际 `IP_SOF_BROADCAST=1` + `IP_SOF_BROADCAST_RECV=1`（IAP 升级依赖）。`app_udp_broadcast`/`app_udp_cq_broadcast` 复用常驻通道 conn（`udp_task`/`udp_cq_task` 创建时已 `ip_set_option(SOF_BROADCAST)`）直接 `netconn_sendto` 广播，常驻 conn 未就绪（断链重建窗口，deinit 置 conn=NULL）才回退临时 conn（2026-08-21）
- **MQTT**：`LWIP_MQTT` / `MQTT_MAX_IN_FLIGHT` 未定义；USER CODE 1 定义 `MQTT_REQ_MAX_IN_FLIGHT=16`、`LWIP_SO_RCVTIMEO=1`
- **TCP**：`LWIP_TCP_KEEPALIVE=1`；`TCP_MSS=536`（未覆盖，取 `opt.h` 默认值）
- **线程**：`TCPIP_THREAD_PRIO=24`（=osPriorityHigh）、`TCPIP_THREAD_STACKSIZE=1024`

### pl_net_adapt.h 架构约束

`Platform/Inc/pl_net_adapt.h` 聚合所有 LwIP API 头文件。**严格禁止在任何 .h 文件中包含此头文件**——仅 .c 实现文件可引用，防止 LwIP 类型泄漏到 Application 层头文件中。

## 调试

**SEGGER RTT**（`pl_rtt.c`）：高速调试输出通道（无需占用 UART），通过 J-Link/CMSIS-DAP 的 SWD 接口传输。`pl_rtt_init()` 在 `hw_initcall` 中初始化，`SEGGER_RTT_printf()` 可在任何上下文中使用。不依赖 RTOS，可在 HardFault Handler 等异常上下文中输出。

**硬件测试**（`app_test.c`）：9 个测试函数 — `app_test_pixel_scan` / `app_test_render_text` / `app_test_io_output` / `app_test_led_mapping` / `app_test_scan_line_order` / `app_test_diagonal` / `app_test_oblique_scan` / `app_test_prepare_mapping` 等，由 `app_test_run()` 汇总。`app_test_run()` 当前在 `app_boot` 的 `init_task` 中已注释。另有 `app_factory_test.c`（`sw_app_initcall(_factory_test_init)`）：出厂检测 monitor + 老化循环（TEST 键触发，斜扫测试）。**业务数据到达经 `app_factory_mode_interrupt()` 置中止标志（不销毁任务），monitor 各按键等待点分片（100ms）检查标志回 IDLE——TEST 键全程可用**（对齐 9K1F212701 裸机：收包仅清 testMode）。

## Kernel 工具库 (`Kernel/`)

| 文件 | 关键 API | 说明 |
|---|---|---|
| `ring_buffer.h` | `RB_DEFINE(name,sz)` 编译期静态分配 | 零堆开销环形缓冲区。每个 API 末尾的 `void *mutex` 参数控制锁行为：传 `rb->mutex` 自动加锁/解锁（单步操作），传 `nullptr` 跳过（调用者通过 `rb_lock/rb_unlock` 自行持锁，用于多步原子序列）。`rb_init` 在运行时绑定优先级继承互斥锁。相邻数据的 `rb_peek`/`rb_contig`/`rb_skip` 支持帧探测。 |
| `initcall.h` | `hw_pl/dev_initcall`、`sw_pl/dev/app_initcall` | 6 个层级宏，生成 linker section 条目 |
| `container_of.h` | `container_of` | 向上转型宏。`channel_t`/`ch_ops_t`/`channel_id_t`/`proto_probe_sta_t` 等 dispatch 共用类型定义在 `Application/Inc/app_dispatch.h` |
| `bcc_utils.h` | `bcc_calcu` | BCC 校验（RLS 协议） |
| `text_cvt.h` | `UTF8ToGBK` | UTF-8→GBK 编码转换 |
| `bit_utils.h` | `bit_ctz` | 协议掩码→数组索引（`proto_mask_t` → 数组下标） |
| `crc_utils.h` | CRC 校验 | CRC32（IAP）+ CRC-16/XMODEM（LDI） |

## 代码风格

- `.clang-format`：Microsoft 基础，4 空格缩进，无 Tab，Linux 大括号，不限制列宽
- 语言：C23，中文注释
- 命名：`pl_`=Platform、`dev_`=Device、`app_`=Application
- 派生类基类成员统一 `me`（如 `dev_display_t me`、`channel_t me`）
- 函数命名：`_` 前缀 = 内部静态函数、模块前缀 `pl_`/`dev_`/`app_` = 公开 API
- GPIO 定义遵循 CubeMX 命名
