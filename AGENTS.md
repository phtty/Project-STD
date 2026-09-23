# Project_STD —— 工程指引（给 AI agent）

> 本文是**操作手册**：命令、目录地图、硬约束、以及"别踩的坑"。
> **架构的设计理由**与各子系统细节见 `docs/架构说明.md`；**命名与注释的评审契约**见 `docs/命名约定.md`。
> （全局性内容——中文交流、代理、工具环境——在你自己的 `~/.config/opencode/AGENTS.md` 里，本文不重复。）

## 1. 这是什么

STM32F407ZGTx（Cortex-M4F @168MHz，1MB Flash / 128KB SRAM / 64KB CCMRAM）**裸机固件**，
C23，FreeRTOS v10.3.1 + CMSIS-RTOS V2，LwIP，`arm-none-eabi-gcc`。

业务：**车道诱导屏 / VMS 情报板控制器** —— HUB75 LED 屏驱动 + 多协议通信
（RS485 / RS232 / 以太网上的 IAP、LDI、RLS、AHMQ）+ **多控制卡级联显示**。

两块硬件（`boards/`）：

| 板 | 屏 | 级联 | 画布 | 其它 |
|---|---|---|---|---|
| **3833024**（默认） | P20 16×8 → **128×32** | 1×1（单卡） | **关** | 有 IAP 记录、2×RS232、IO 控制、DIP 拨码 |
| **5006048** | P10 112×10 → **224×50**，1/2 扫 | **1×2**（主卡格在下） | **开** | 无 IAP 记录、无 RS232、无 DIP |

## 2. 构建 / 烧录 / 测试

```sh
make -j8                       # 默认板 3833024，Debug（CONFIG 默认 Debug）
make -j8 BOARD=5006048         # 换板
make -j8 CONFIG=Release        # 另一套输出目录（注意：CFLAGS 恒为 -Og -g，Release 并不改优化）
make -j8 TOOLCHAIN=clang       # 仅换编译器（链接仍走 arm-none-eabi-gcc）
make clean                     # 删 build/<板>/<配置> 与 build/<板>/test（先保留 compile_commands.json 再放回）

make test                      # host 侧 15 个测试套件（见 §7）
make BOARD=5006048 test        # 同上；BOARD 影响测试里的板级宏

# 编译数据库（clangd 用）
bear --output build/3833024/Debug/compile_commands.json -- make -B -j8

# 烧录（OpenOCD）
openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; program ./build/3833024/Debug/Project_STD.hex verify reset exit"
# 整片擦除
openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; stm32f4x unlock 0; stm32f4x mass_erase 0; shutdown"
```

产物在 `build/<板>/<配置>/`：`Project_STD.elf`（带符号，调试用）、`.hex`（烧录）、`.bin`（IAP 用）。
**Makefile 没有烧录目标**——烧录走上面那条 OpenOCD，或 IDE（EIDE）。

## 3. 目录地图

```
Application/{Inc,Src}/              业务层（app_*）
├── Render/     app_render.*（字库渲染引擎）+ app_screen.*（整屏门面）
│               └ app_screen_canvas.c / _status.c / _brightness.c（同模块的按职责拆分 TU）
├── BIST/       app_test.*（硬件冒烟用例）、app_factory_test.*（出厂检测模式）、app_diag.c（运行时健康诊断）
├── IAP/ LDI/ RLS/ CASC/ AHMQ/      五个协议（各有自己的目录）
├── Channel/    UDP / TCP server / TCP client / MQTT / RS485 传输通道
└── 根          app_boot.c（RTOS 编排）、app_dispatch.c（分发引擎）、app_cfg_sched.c、app_key.c、app_light_sensor.c
Device/                            设备层（dev_*）。**源按类别分目录** Display/ IO/ Network/ Storage/，
                                   头在 `Device/Inc/<类别>/` 下（如 `Device/Inc/Display/dev_display.h`）。
                                   ⚠️ 本层**没有** `Device/Src/`——是"类别目录"在源与头两侧镜像，不是 Inc/Src 镜像
Platform/{Inc,Src}/                HAL 薄封装（pl_*），对外只给不透明句柄
Kernel/{Inc,Src}/                  零依赖工具：ring_buffer、initcall、container_of、text_cvt、crc_utils、bit_utils、bcc_utils
Compiler/                          startup.c（C 写的 Reset_Handler + 自定义向量表）、sections.ld（共享段）、OpenOCD cfg
boards/<板>/                       board.h（板级 `BOARD_*` 事实）、board.mk（板级源清单）、Core/（CubeMX 生成）、
                                   Application|Device|Platform 下的覆盖文件、链接脚本
test/                              host 测试套件 + stubs/（遮蔽真头文件的替身）
docs/                              命名约定.md（评审契约）、架构说明.md（设计理由与子系统细节）
```

## 4. 架构要点（一句话版，细节见 `docs/架构说明.md`）

- **分层单向依赖**：`Core` → `Platform`(`pl_`) → `Device`(`dev_`) → `Application`(`app_`)；`Kernel` 零依赖工具。
  上层可见下层，**反之不可**。层前缀由"定义在哪个 `Inc/` 树"决定。
- **initcall 自注册**：`hw_initcall`（RTOS 前，`main()` 里 `initcall_run`）分 **4 层**
  （`hw_pre`(0) `hw_pl`(1) `hw_dev`(2) `hw_post`(3)）；`sw_initcall`（RTOS 后，`initcall_run_sw()`）分 **5 层**
  （`sw_pre`(0) `sw_pl`(1) `sw_dev`(2) `sw_app`(3) `sw_post`(4)）。**同层次序不可依赖**——见 §6 坑 4。
- **OCP 虚表**：结构体首成员是基类对象，名字统一 `base`（如 `dev_storage_t base`），用 `container_of` 零开销上转型。
  四个虚表：`dev_display_ops` / `dev_storage_ops` / `app_ccb_ops` / `app_pcb_ops`。
  ⚠️ 注意基类**内部**字段顺序不一致：Device 层基类（`dev_display_t`/`dev_storage_t`）首字段是 `ops`，
  而 `app_pcb_t`/`app_ccb_t` 首字段是 `name`（`ops` 在第二位）。`container_of` 取的是**派生结构体里的 `base` 成员**，
  与基类内部字段顺序无关，但别照 Device 的习惯去假设 `ops` 在首位。
  跨模块只暴露 **API**；`g_` 变量只在**本模块的 `Inc/` 头**声明。
- **分发模型**：`app_pcb_t`（协议控制块：probe + 环形缓冲 + 帧队列）绑定到 `app_ccb_t`（通道控制块：`ops->send`）。
  收包 → `app_ccb_dispatch` → 逐协议 probe（4 态：READY/WAIT/FAKE/SKIP）→ 帧投给该协议的队列。
- **板级 vs 共享**：`boards/<板>/` 下的**同名文件覆盖**共享层；单卡/级联、画布开关、字库、向量表全部由板级 `BOARD_*` 决定
  （`BOARD_SCREEN_CANVAS`、`BOARD_CASC_ENABLED = COLS*ROWS>1`、`BOARD_HAS_IAP_RECORD`…）。
- **显示路径**：HUB75 的实际扫描在板级 Device 驱动里（TIM3 行同步 → `scan_task` → BSRR 查表）；
  "整屏"与级联在 `app_screen`（1bpp 逻辑画布 + 切分表 + 身份）与 `app_casc`（走 RS485 的多卡同步）。

## 5. 约定（指针，不在此重复）

- **命名**（前缀、类型/函数/变量、缩写白名单、目录与文件、Doxygen 契约、禁改清单、例外登记）→ **`docs/命名约定.md`**
- 改动前请先看它的 §12（注释怎么写）与 §13（哪些名字**不能**改：落盘键、线格式、链接符号）

## 6. 别踩的坑（都是实际踩过的）

1. **`.eide/eide.yml` 会被 EIDE 重写**：它由 IDE 维护，改了 `Makefile` 忘了它、或反过来，都会让另一套构建编不过。
   改它尽量在 EIDE 关闭时；提交前 `git diff .eide/eide.yml` 看一眼有没有被回退。
2. **新增/搬动源文件要同步两套构建清单**：`Makefile`（固件 **和测试** 两处 `INC_DIRS`/源清单）+ `.eide/eide.yml`（**两个目标**）。
3. **Doxygen 与 `__attribute__` 的两个错位**（见 `docs/命名约定.md` §12「Doxygen 行为实测」）：
   - **尾置**属性（`uint8_t x[] __attribute__((aligned(4)));`）会让 Doxygen 把**后续声明吞进前一条**、误报符号未声明 → 属性写**前置**
   - `typedef` 类型的文档要点必须放**尾部** `/**< */`
4. **initcall 同层次序不可依赖**：宏生成的段名同层完全相同，`SORT()` 是空操作，实际次序是链接顺序。
   要表达依赖用**层级**（如"读配置"放 `sw_post(4)`，让 `sw_app(3)` 的"加载配置"先跑）。
5. **`Platform/Inc/pl_net_adapt.h` 严禁出现在任何 `.h` 文件里**——它会泄漏 LwIP 类型到 Application 层。只有 `.c` 可引用。
6. **`#if BOARD_*` 整块消除时，"那个符号必须仍然存在"**：若别的 TU（尤其不分板编译的）还引用它，就会链接失败。
   有外部调用者就必须给 `#else` 桩（例：`app_screen_canvas.c` 里 `set_color_override`/`output_color` 的桩）。
7. **持久化记录与线上格式是契约**：W25Qxx/内部 Flash 记录的 `name[16]` 字符串、记录头布局、协议帧字段与命令码、
   链接脚本符号——**改名即变砖/丢身份**（`docs/命名约定.md` §13）。
8. **`test/` 里有白盒套件直接 `#include` 生产 `.c`**（为了测 `static` 内部结构）：改动对应的生产文件会影响它们，
   且拆 TU 时要给它们补 include（否则链接未定义）。
9. **`test` 目标必须留在 `.PHONY` 里**：工程里存在同名的 `test/` 目录，不声明 `.PHONY` 会被 `make` 当成
   "已存在的目标"而**跳过测试配方**（`Makefile` 里有注释说明）。
10. **AHMQ 目前被排除在所有板之外**：根 `Makefile` 的 `SRC_EXCLUDE` 把 `Application/Src/AHMQ/*.c` 排除
    （`app_ahmq.c` / `app_ahmq_cmd.c`），EIDE 两个目标也加了对应 exclude。要启用必须**两处同时改**——
    注释里写明"不排的话同一份源码在两套构建下会产出不同固件"。

## 7. 测试（host 侧，`make test`）

15 个套件，在 `build/<板>/test/` 生成可执行文件；用 pthread 版 cmsis_os2 + `test/stubs/` 遮蔽真头文件，
带 ASan/UBSan。**它们跑在宿主机上，不碰硬件**：

| 套件 | 覆盖 |
|---|---|
| `test_ring_buffer` | 环形缓冲区（含容量钳位） |
| `test_dispatch` | 分发引擎（pcb/ccb、probe 4 态、帧投递） |
| `test_probes` | IAP / LDI / RLS 三个协议的探针 |
| `test_cfg_sched` | 配置记录调度器（块扫描、校验） |
| `test_iap_cfg` | IAP 记录（走 RAM 假扇区；含跨模块访问器契约） |
| `test_ldi_0ah` | LDI 0AH 配置跨 IAP + LDI 两条记录 |
| `test_isr_preinit` | initcall 之前的 ISR 安全 |
| `test_font_lib` | 板级字库偏移表 |
| `test_screen_canvas` | 整屏画布 vs 直写的像素等价、越界裁剪 |
| `test_screen_layout` | 切分表合成 + 按矩形抽带（逐位） |
| `test_crc` | 硬件 CRC32（长度/对齐） |
| `test_casc_frame` | 级联探针 4 态 / 地址 / 长度 |
| `test_casc_round` / `test_casc_master` | 级联从卡/主卡的图传与轮次 |
| `test_rs485_slots` | RS485 收包槽位（环回拆帧） |

**host 测试覆盖不到真实硬件时序**（TIM3/TIM4 扫描、BSRR、真实 SPI/DMA）——涉及显示路径或外设时序的改动，
除 `make test` 外还应上机冒烟（两板、含级联显示与工厂逐色老化）。
