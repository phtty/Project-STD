# Project_STD —— 工程指引（给 AI agent）

> 本文是**操作手册**：命令、目录地图、硬约束、以及"别踩的坑"。
> **架构的设计理由**与各子系统细节见 `docs/架构说明.md`；**命名与注释的评审契约**见 `docs/命名约定.md`。
> （全局性内容——中文交流、代理、工具环境——在你自己的 `~/.config/opencode/AGENTS.md` 里，本文不重复。）
>
> **第一原则：能用 MCP 就别手改工程配置。** 构建、烧录、目标切换、宏、包含路径、排除文件都走 `mcp__eide__*`；调试与 RTT 走 `mcp__openocd__*`。EIDE 会在打开/保存时重写 `.eide/eide.yml`，手改的内容可能被无声覆盖，而 MCP 是直接对它自己的进程发请求，不存在这个竞态。

## 0. 工具分工：先判断「这是不是 MCP 的活」

DSH 里挂了两个 MCP：`mcp__eide__*`（EIDE 扩展内置：工程配置 / 构建 / 烧录）与 `mcp__openocd__*`（OpenOCD + GDB：调试 / RTT / 寄存器）。**EIDE 是工程配置的事实源**，`Makefile` 是给 host 测试和无 IDE 场景的旁路。

| 要做的事 | 用它 | 别这么做 |
|---|---|---|
| 构建固件 | `mcp__eide__eide_build`（增量）/ `eide_rebuild`（全量） | 只跑 `make` 就以为 IDE 那边也绿了 |
| 烧录 | `mcp__eide__eide_flash` | 手拼 openocd 命令行（cwd/相对路径容易错）；在调试会话里烧（抢探针） |
| 切目标板 | `eide_get_targets` 看、`eide_switch_target` 切 | 手改 `eide.yml` 的 `targets` |
| 宏 / 包含路径 | `eide_get_defines`/`eide_set_defines`、`eide_get_inc_dirs`/`eide_set_inc_dirs` | 手改 `defineList` / `incList` |
| 排除源文件 | `eide_exclude_src_file` / `eide_include_src_file` | 手改 `excludeList` |
| 新增源文件 | 放进目录 + 补 `Makefile`；EIDE 侧**不会自动收集**（实测：新文件不参与编译），新增目录用 `eide_add_src_dir` | 以为「丢进已有目录 EIDE 就会编」 |
| host 测试 / 变异 / lint | `make test` / `make mutate` / `make lint` | 以为 MCP 能跑 host 测试（不能） |
| 调试 / RTT | `mcp__openocd__*`（见 §2.3） | 同时开两个 OpenOCD |

一句话：**源码手改，工程配置与构建/烧录走 MCP，host 测试走 make。** 改完源码按 `eide_build` → `make test` 两道闸都过一遍。

> ⚠️ **EIDE 两个 target 的源集合不一样（记录时 2026-11-25 实测）**：EIDE 用**显式文件清单**（`eide.yml` 的 `virtualFolder`）+ target 级 `excludeList`，不做目录扫描。`3833024` target 排除了 virtual folder `protocol/{ah,cas,ldi,cdstd}`，所以它的产物**不含 AH 与级联**（204 个 .o）；`5006048` target 排除的是 `protocol/{rls,ldi,cdstd}`（含 AH/CASC、不含 RLS）。而 `Makefile` 的 `3833024` 构建**含** AH 与 `app_casc.c`（210 个源，LDI/CDSTD 两边都不编）。**结论：需要 AH/级联行为的固件以 `make` 产物为准**；要改 EIDE 的取舍就在目标里增删排除项，别拿不同 target 的产物互相当基准。

## 1. 这是什么

STM32F407ZGTx（Cortex-M4F @168MHz，1MB Flash / 128KB SRAM / 64KB CCMRAM）**裸机固件**，
C23，FreeRTOS v10.3.1 + CMSIS-RTOS V2，LwIP，`arm-none-eabi-gcc`。

业务：**车道诱导屏 / VMS 情报板控制器** —— HUB75 LED 屏驱动 + 多协议通信
（RS485 / RS232 / 以太网上的 IAP、LDI、RLS、CDSTD、AH）+ **多控制卡级联显示**。

两块硬件（`boards/`）：

| 板 | 屏 | 级联 | 画布 | 其它 |
|---|---|---|---|---|
| **3833024**（默认） | P20 16×8 → **128×32** | 1×1（单卡） | **关** | 有 IAP 记录、2×RS232、IO 控制、DIP 拨码 |
| **5006048** | P15 64×8 → **128×32**，静态扫（P10 112×10 → 224×50 是备选，换 `board.mk` 一行） | **1×2**（主卡格在下） | **开** | 无 IAP 记录、无 RS232、无 DIP |

## 2. 构建 / 烧录 / 测试

### 2.1 固件：EIDE MCP（首选）

```
mcp__eide__eide_get_targets(uid)              # 当前目标 + 可选板（3833024 / 5006048）
mcp__eide__eide_switch_target(uid, "5006048")
mcp__eide__eide_build(uid)                    # 增量；全量 eide_rebuild，清产物 eide_clean
mcp__eide__eide_flash(uid, eraseAll=false)    # 烧录（upload 命令已改成 ${workspaceFolder} 绝对路径）
mcp__eide__eide_reload(uid)                   # 工程文件被外部改过之后，让 EIDE 重新读
```

`uid` 取 `.eide/eide.yml` 的 `miscInfo.uid`。产物在 `build/<板>/Project_STD.elf|.hex|.bin`。
**MCP 依赖 VS Code 开着**（服务跑在 EIDE 扩展进程里）；关了 VS Code 就退回 2.2 的命令行。

### 2.2 命令行旁路：host 测试与无 IDE 场景

```sh
make -j$(nproc)                  # 固件（旁路），产物 build/<板>/Debug/ —— 与 EIDE 的 build/<板>/ 不是同一目录
make -j$(nproc) BOARD=5006048
make -j$(nproc) CONFIG=Release   # 另一套输出目录（CFLAGS 恒为 -Og -g，Release 并不改优化）
make -j$(nproc) TOOLCHAIN=clang  # 仅换编译器（链接仍走 arm-none-eabi-gcc）
make clean                       # 删 build/<板>/<配置> 与 build/<板>/test（保留 compile_commands.json 再放回）

make test                        # host 侧 32 个套件（见 §7）；第一条自动跑结构规则闸
make BOARD=5006048 test
make lint                        # 只跑结构规则闸（秒级；规范见 docs/测试约定.md）
make mutate MUT_ARGS="--suite test_cdstd --fail-on-alive"   # 变异闸（要求基线是绿的）

bear --output build/3833024/Debug/compile_commands.json -- make -B -j$(nproc)   # clangd 数据库
```

手工烧录（只在 MCP 不可用时）：

```sh
openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; program ./build/3833024/Debug/Project_STD.hex verify reset exit"
openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; stm32f4x unlock 0; stm32f4x mass_erase 0; shutdown"
```

> ⚠️ 两套构建的产物目录不同（EIDE `build/<板>/`、Makefile `build/<板>/Debug/`）——**调试/烧录用哪套编的 ELF 要对上**，别拿旧产物。

### 2.3 交互式调试（DSH 的 openocd MCP）

DSH 里挂了 `mcp__openocd__*`（OpenOCD + GDB/MI，server 在 `~/.dsh/mcp/openocd-mcp/`）。
典型流程：

```
mcp__openocd__set_project("/home/phtty/workspace/source/Project_STD")  # 读 .vscode/launch.json
mcp__openocd__debug_attach(config_name="attach", firmware_path="…/build/3833024/Project_STD.elf")
mcp__openocd__debug_interrupt()          # attach 不停目标，要读寄存器必须先中断
mcp__openocd__debug_command("info registers pc")
mcp__openocd__debug_command("x/24wx 0x40023800")   # 值 → ~/workspace/tools/SVD/svd.py decode
mcp__openocd__debug_continue(); mcp__openocd__debug_stop()
```

三条硬约束：

1. **`firmware_path` 必须显式给**：`launch.json` 里两份配置的 `executable` 都写着
   `build/5006048/Project_STD.elf`，`debug_start` 会真的 `-target-download`——不传就等于把
   5006048 的固件写进 3833024。
2. **`debug_attach` 不 halt 目标**：紧跟着读寄存器会卡 30 s 超时，先 `debug_interrupt`。
3. **调试会话独占探针**：此时不能跑 `eide_flash`，也不能有第二个 OpenOCD。

`monitor ...` 与 `-break-list` 不回显（MCP 只取 console 流），确认状态用 `debug_state`
和 `info breakpoints`。寄存器字段名用 `~/workspace/tools/SVD/svd.py`（`--chip STM32F407`）。

**RTT 日志怎么读**：固件带 RTT —— `Platform/Src/pl_rtt.c` 的 `hw_pl_initcall(pl_rtt_init)`
调 `SEGGER_RTT_Init()`，`printf` 经 `SEGGER_RTT_Syscalls_GCC.c` 的 `_write` 走 RTT（上行 1 KB）。
周期诊断 `app_diag` 默认关（`app_diag.c` 的 `APP_DIAG_ENABLE 0`，一次全量打印 ~0.9 KB 会挤占
上行缓冲），所以正常运行时只有启动日志和错误日志。读法：`debug_start`/`debug_attach` 之后
先 `debug_continue`，再 `read_rtt` —— 它会在第一次调用时懒配置 RTT（短暂 halt 做控制块搜索、
随即放行、等 0.5 s 收数据），启动日志也能一起拿到。`read_rtt` 报"RTT 尚未就绪"说明固件还没跑过
`SEGGER_RTT_Init()`，continue 一下即可。

**探针不能抢**：同时跑两个 OpenOCD（包括一个忘了退的 `timeout` 包裹进程）会让 CMSIS-DAP
刷 `error reading data: Input/Output Error`，随后 `CMD_INFO failed`。清理用
`pgrep -af '/usr/bin/openocd'` 看，`timeout` 只发 SIGTERM（OpenOCD 可能不退出），必要时 `kill -9`。

## 3. 目录地图

```
Application/{Inc,Src}/              业务层（app_*）
├── Render/     app_render.*（字库渲染引擎）+ app_screen.*（整屏门面）
│               └ app_screen_canvas.c / _status.c / _brightness.c（同模块的按职责拆分 TU）
├── BIST/       app_test.*（硬件冒烟用例）、app_factory_test.*（出厂检测模式）、app_diag.c（运行时健康诊断）
├── IAP/ LDI/ RLS/ CASC/ CDSTD/ AH/ 六个协议（各有自己的目录；AH = AH/MQTT + AH/TCP）
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
test/                              host 测试套件，按被测对象分目录：kernel/ dispatch/ channel/
                                   config/ protocol/ display/（布局与写法见 docs/测试约定.md §1.1）；
                                   stubs/（遮蔽真头文件的替身，含整套替身 host_p15/）、tools/（主机侧脚本）
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

1. **`.eide/eide.yml` 是 EIDE 的，不是你的**：EIDE 打开/保存时会重写它。改目标、宏、包含路径、排除文件一律走 §0 的 MCP 工具；只有 MCP 覆盖不到的情况才手改，改完 `eide_reload` 让 EIDE 重读，并在提交前 `git diff .eide/eide.yml` 确认没被回退。
2. **新增源文件：两边都要显式加，EIDE 不会自动收集**：EIDE 的工程文件是**显式清单**（`eide.yml` 的 `virtualFolder`）加 target 级 `excludeList`；**实测把新 `.c` 丢进已有目录后 `eide_build` 编译 0 个文件**。`Makefile` 同样是显式 `SRC_*`（无 wildcard）。所以「加文件」= 落盘 + 补 `Makefile` + 让 EIDE 也认它：新增**目录**用 `eide_add_src_dir`（实测会写进 `srcDirs`，该目录下的文件随后被自动收集并编译），**单个文件**目前 MCP 没有对应工具，只能在 EIDE 界面里加。**不要**手动改 `eide.yml` 的 `virtualFolder`/`srcDirs`（会被 EIDE 重写）；不想让某文件参与编译用 `eide_exclude_src_file`。
3. **Doxygen 与 `__attribute__` 的两个错位**（见 `docs/命名约定.md` §12「Doxygen 已知行为」）：
   - **尾置**属性（`uint8_t x[] __attribute__((aligned(4)));`）会让 Doxygen 把**后续声明吞进前一条**、误报符号未声明 → 属性写**前置**
   - `typedef` 类型的文档要点必须放**尾部** `/**< */`
4. **initcall 同层次序不可依赖**：宏生成的段名同层完全相同，`SORT()` 是空操作，实际次序是链接顺序。
   要表达依赖用**层级**（如"读配置"放 `sw_post(4)`，让 `sw_app(3)` 的"加载配置"先跑）。
5. **`Platform/Inc/pl_net_adapt.h` 严禁出现在任何 `.h` 文件里**——它会泄漏 LwIP 类型到 Application 层。只有 `.c` 可引用。
6. **`#if BOARD_*` 整块消除时，"那个符号必须仍然存在"**：若别的 TU（尤其不分板编译的）还引用它，就会链接失败。
   有外部调用者就必须给 `#else` 桩（例：`app_screen_canvas.c` 里 `set_color_override`/`output_color` 的桩）。
   另一形态：共享的 `app_fold.c` **无条件**引用板级 `g_board_fold_presets`，所以 `app_fold_preset_board.c` **两块板都要有**
   （3833024 是空槽）——缺了链接不过。
7. **持久化记录与线上格式是契约**：W25Qxx/内部 Flash 记录的 `name[16]` 字符串、记录头布局、协议帧字段与命令码、
   链接脚本符号——**改名即变砖/丢身份**（`docs/命名约定.md` §13）。
8. **`test/` 里有白盒套件直接 `#include` 生产 `.c`**（为了测 `static` 内部结构）：改动对应的生产文件会影响它们，
   且拆 TU 时要给它们补 include（否则链接未定义）。
9. **`test` 目标必须留在 `.PHONY` 里**：工程里存在同名的 `test/` 目录，不声明 `.PHONY` 会被 `make` 当成
   "已存在的目标"而**跳过测试配方**（`Makefile` 里有注释说明）。
10. **AH 协议族：TCP 参数口 + MQTT 业务都已接通**：`Application/Src/AH/` 下 AH/MQTT（`app_ah_mqtt*`）
    与 AH/TCP（`app_ah_tcp*`，参数口 0AH/0BH）。AH/TCP 在 `sw_post` 起监听口（端口取
    `ah_cfg.client_port`，默认 6001），只做“校验 → 落盘 → 应答”；AH/MQTT 在 `sw_post` 用 `ah_cfg`
    快照派生主题/Client ID/凭据并登记订阅，`app_boot` 里 `app_mqtt_start()` 起通道任务。
    **主题与凭据只在启动时构建**：0AH 改参数统一重启生效，运行期不重建。改 `Makefile` 或 EIDE 的
    AH 源清单时两处必须同步（否则同一份源码会产出不同固件）。
11. **host 与 ARM 的 `char` 符号性相反**：host（x86）默认 signed、ARM 默认 unsigned。任何"字节值 ≥ 0x80
    的比较/移位"在 host 上会走**另一条路** —— `Kernel/Src/text_cvt.c` 的 UTF-8 解析曾因此在 host 上
    完整假绿（`out=0`，什么都不做），只有 ARM 上才暴露。处理字节一律**显式 `uint8_t`**；给这类函数
    写 host 用例时要按 `-funsigned-char` 复核一次（`test_render` 的 cvt 用例即按此双跑过）。
12. **`app_render` 的 `x/y/w/h` 是区域 `[x, x+w) × [y, y+h)`**（`w/h` 是**尺寸**，不是绝对右/下边界）；
    越界语义是**裁剪**，且**两层各自防御**（`dev_display_*` 与画布 `_sink_*` 都自带裁剪）。改渲染/
    裁剪相关代码前，先读 `Application/Inc/Render/app_render.h` 顶部的"区域契约"块。
13. **Windows 上 binutils ≥ 2.47 的 `ld` 会把"同盘的第二份脚本"误判成重复**：判重按 `stat()` 的
    (st_dev, st_ino) 比较，而 Windows 上 `st_ino` 恒为 0、`st_dev` 只是盘符号 —— 于是同一盘符上的两份
    脚本必然报 `error: linker script file '...' appears multiple times`。两块板的 `STM32F407XX_FLASH.ld`
    都 `INCLUDE Compiler/sections.ld`，**两套构建的链接会一起失败**（`make` 与 EIDE 一样；写成两个 `-T`
    也逃不掉）。上游修复只在 binutils master（commit `ab1830f6`，改为无 inode 时按文件名比较），
    2.47 发布版没有。本机已把 MSYS2 ucrt64 的 `arm-none-eabi-binutils` **钉在 2.46.1**
    （`/etc/pacman.conf` 的 `IgnorePkg`，注释写明理由与解除条件）；`pacman -Syu` 后若再撞这个错，
    先跑 `arm-none-eabi-ld --version` 看是不是被升回 ≥ 2.47。

## 7. 测试（host 侧，`make test`）

32 个套件，在 `build/<板>/test/` 生成可执行文件；用 pthread 版 cmsis_os2 + `test/stubs/` 遮蔽真头文件，
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
| `test_screen_brightness` | 整屏亮度与屏开关（期望值 / OE 门控 / 钳位 / 级联待下发取有效值 / 不冻结口径） |
| `test_screen_layout` | 切分表合成 + 按矩形抽带（逐位） |
| `test_crc` | 硬件 CRC32（长度/对齐） |
| `test_casc_frame` | 级联探针 4 态 / 地址 / 长度 |
| `test_casc_round` / `test_casc_master` | 级联从卡/主卡的图传与轮次 |
| `test_rs485_slots` | RS485 收包槽位（环回拆帧） |
| `test_tcp_server` | TCP 服务器通道：实例 API（默认 9528 / 单扩展实例 / 未绑协议不启动）、发送门禁（DOWN+有 conn 也丢弃）、conn 生命周期（keepalive 10s/2s×3、只派发 len>1 的段、断开置 DOWN+释放信号量、早退也放）、manage 循环（bind/listen/accept/delete/失败重试）。用真 LwIP 头 + 脚本化 netconn |
| `test_tcp_client` | TCP 客户端通道：远端配置（set_remote 打断当前连接）、非阻塞 connect → 4s 超时预算、建连成功后恢复阻塞并派生 conn 任务、断开/失败都 delete + 1s 重试。osDelay/时钟是假的（不真睡） |
| `test_udp` | UDP 通道：端口配置、广播（SOF_BROADCAST + 255.255.255.255）、本帧来源回包与 dst->broadcast、收包来源/计数、0 字节段跳过、bind 循环与 2s 重试（每轮新建 netconn，O6 已修） |
| `test_mqtt` | MQTT 通道：订阅登记（16 条上限/追加基址）、凭据注入（NULL=不带字段、有界拷贝）、connect 同步失败也必须放信号量、CONNACK 状态映射、发布目标（dst topic 优先）、分片累计与 LAST 派发、主题截断、任务重连（UP+逐条订阅 / free+1s 重建） |
| `test_display_base` | 显示设备基类（生产 `dev_display.c` 首次进 host）：注册/颜色能力（0 掩码=全彩）、像素与矩形裁剪（含 x+w 回绕、0 尺寸、越界起点）、位图右侧裁剪仍按原 stride、frame_begin/end 脏标记纪律、扫描任务（帧首才 prepare、行循环、OE+LAT 原子窗口、PWM 占空比、节拍统计） |
| `test_render` | 渲染引擎的区域契约（高度门禁 / 换行与截断 / 对齐防下溢 / 越界裁剪）、`text_cvt` 转换边界、`app_render` 锁配对 |
| `test_rls` | RLS 协议命令层（从**一帧完整帧**往里测，生产 TU 照原样编译）：7 类命令码的分派矩阵、回显规则（BM/SV/搜索不回、其余回原帧）、显示位图门禁与颜色透传/>7 拒显、亮度 0~31→1~7 与 255 恢复跟随、显示开关只切 OE、修改 IP 的地址与端口取舍、搜索应答 21 帧逐字节（含 BCC）、图片槽 SV/DS 编号与「够屏不够槽」守卫。帧探测在 `test_probes` |
| `test_rls_pic` | RLS 图片槽（SV/DS 的 3 槽）：记录布局契约（513B 槽 / 1540B 记录）、配置记录加载（有效/空/损坏/长度不符）、落盘失败仍存 RAM、几何门禁（非 128×32 只清屏）、SW1/2/3 干接点→槽 0/1/2 与无键退出。命令层在 `test_rls` |
| `test_fold` | 折叠屏模式判定（几何门禁 / E9 / E9+EA）、`app_fold_split_lines` 切分、变体1 E9 两行各限半屏（FLAT 与暂态变体2） |
| `test_cdstd` | CDSTD 协议：探针四态 / 整帧等待 / CRC→FAKE / 与 LDI 同一 FFFF 帧头互不毒化、队列取帧与命令分派；命令层 A0 屏开关、A1 文字、A2 亮度、A3 全屏填充、A4 固化显示、A5/A6 IP（A7/A8 仍空壳） |
| `test_ah` | AH 纯函数：hex 编码、主题前缀（站8/道2/型2/号2）、Client ID、20 字符通知号（时间/计数回卷） |
| `test_ah_cfg` | AH 参数配置：默认值来源（IAP 镜像/netif）、校验矩阵、`ah_cfg` v1 记录加载/保存、无效记录与 IO_ERR 回落 |
| `test_ah_tcp` | AH/TCP 参数口：探针四态（WAIT/FAKE/SKIP/READY）、0AH 字段合并/校验/落盘/RTC/静默丢弃、0BH 采集应答（31H/32H 模块长度） |
| `test_ah_mqtt` | AH/MQTT 上电装配：主题前缀/4 条订阅/2 条上报主题、Client ID、Broker/凭据注入（空密码→NULL）、订阅登记失败不启动、探针按完整主题分类 |
| `test_ah_mqtt_cmd` | AH/MQTT 命令层：32H 文字显示与 31H/未知类型分派、颜色/字号越界、超长/短帧防护、清屏 0/1/2/3、重启、对时（写 RTC/跳过/非法）与应答通知号回显 |
| `test_p15_mapping` | P15 64×8 静态扫描的映射：像素 → BSRR 位流逐位等于老 `lamp_scan`（参照模型独立异写：老 fixBuf 字节布局 + 老发送序 + 老 `LED_Color` 枚举）；含 32 路引脚唯一性、点亮位数不变式、`scan` 384 步与 `set_row` 行地址。**只用 `test/stubs/host_p15/` 的专属替身，不走 `test/stubs/`**（要真的 `dev_display.h` 的 ops 虚表） |

**规范与三道闸**见 `docs/测试约定.md`：`make lint`（结构规则 R1~R11）/ `make mutate`（变异测试）/ 双板全绿。

**host 测试覆盖不到真实硬件时序**（TIM3/TIM4 扫描、BSRR、真实 SPI/DMA）。
但**既有模组驱动（P20/P15/P10）的 BSRR 位流与 OE/LAT 时序已在真机逐模组验证过**
（每次新增/改模组驱动都上机，含两板、级联显示与工厂逐色老化）——这是既成事实，
**不要再当成"未验证风险"重复要求冒烟**。只有**新增模组驱动**、或**改动扫描/OE 时序路径**
时才需要重新上机验证。
