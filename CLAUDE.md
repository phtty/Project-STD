# CLAUDE.md

此文件为 Claude Code (claude.ai/code) 在此仓库中工作时提供指导。

## 构建系统

STM32F407ZGTx 嵌入式项目，工具链 `arm-none-eabi-gcc`，C23 标准。

- **构建**：`make -j8`（根目录 Makefile，支持 `TOOLCHAIN=gcc|clang`，`CONFIG=Debug|Release`）
- **编译数据库**：`bear --output build/Debug/compile_commands.json -- make -B -j8`
- **烧录**：`openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; program ./build/Debug/Project_STD.hex verify reset exit"`
- **整片擦除**：`openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; stm32f4x unlock 0; stm32f4x mass_erase 0; shutdown"`
- **清理**：`make clean`

### 构建产物

| 产物 | 格式 | 用途 |
|---|---|---|
| `Project_STD.elf` | ELF（带调试符号） | 链接器输出，含完整符号表，调试器（probe-rs / cortex-debug）直接使用。`arm-none-eabi-size` 输出各段大小 |
| `Project_STD.hex` | Intel HEX（ASCII 文本） | 烧录文件。OpenOCD 通过 `program ... verify` 写入并回读比对。文本格式，每行含地址+数据+校验和 |
| `Project_STD.bin` | 纯二进制 | 裸二进制映像，无地址信息。适用于 IAP bootloader 直接写入 Flash（从 `0x08040000` 偏移处） |

**链接优化**：`--gc-sections` + `-ffunction-sections -fdata-sections`，未引用的函数/数据自动丢弃。`--specs=nano.specs` 链接精简版 newlib。

## 硬件架构

**MCU**：STM32F407ZGTx（Cortex-M4F，168MHz，FPU，1024KB Flash，128KB SRAM + 64KB CCMRAM）

**时钟**：HSE 8MHz → PLL (M=4, N=168, P=2, Q=7) → SYSCLK 168MHz / APB1 42MHz / APB2 84MHz

**外设**：ADC1、SPI1（W25Qxx）、TIM2/3/4/7、USART1/USART3/USART6、DMA1/2、CRC、IWDG、RTC、Ethernet MAC + DP83848 PHY

## 内存布局

链接脚本：`Compiler/STM32F407XX_FLASH.ld`

```
Flash (1024KB, 起始 0x08000000)
├─ Sector 0:  0x08000000  16KB  中断向量表 (.isr_vector)
├─ Sector 1:  0x08004000  16KB  IAP 系统配置 (dev_flash_iap)
├─ Sector 2:  0x08008000  16KB  Recovery 固件 (备用)
├─ Sector 3:  0x0800C000  16KB  (预留)
├─ Sector 4:  0x08010000  64KB  }
├─ Sector 5:  0x08020000  128KB } 应用程序 (.text/.rodata)
├─ Sector 6:  0x08040000  128KB } ← 主固件入口 (SCB->VTOR = 0x08040000)
├─ Sector 7:  0x08060000  128KB }
├─ Sector 8:  0x08080000  128KB  (预留扩展)
├─ Sector 9:  0x080A0000  128KB  (预留扩展)
├─ Sector10:  0x080C0000  128KB  (预留扩展)
└─ Sector11:  0x080E0000  128KB  LDI 设备配置 (dev_flash_ldi)

SRAM (128KB, 0x20000000)
├─ .data        已初始化全局变量
├─ .bss         零初始化全局变量
├─ FreeRTOS heap  20KB (heap_4.c)
├─ LwIP mem heap  12KB
├─ 环形缓冲区池    4×2KB = 8KB
├─ UART DMA 缓冲   3×2KB = 6KB
├─ 任务栈          见任务栈表
└─ _user_heap_stack  512B heap + 2KB MSP 栈 (链接脚本预留)

CCMRAM (64KB, 0x10000000, NOLOAD 段)
├─ pixel_map[256]           256B  显示帧缓冲
├─ hub75_buff[256]          256B  扫描输出缓冲
├─ g_bsrr[4][8]             ~384B BSRR 查表
└─ W25Qxx DMA 读缓冲         ~4KB (静态, .ccmram section)
```

**CCMRAM 特性**：零等待状态，与内核直连不经过总线矩阵。NOLOAD 段在 `startup.c` 中整体循环清零（行为等同 .bss）。仅数据可用，不可执行代码。

## 闪存扇区职能地图

| 扇区 | 地址 | 大小 | 内容 | 读写方式 |
|---|---|---|---|---|
| 0 | `0x08000000` | 16KB | ISR 向量表 + 固件头 | 烧录时写入 |
| 1 | `0x08004000` | 16KB | IAP 系统配置 (magic + FWInfo + NetConfig + CRC32) | `dev_flash_iap` 内存映射读 + 擦写 |
| 2 | `0x08008000` | 16KB | Recovery 固件 | IAP 升级时写入 |
| 3-10 | `0x0800C000`~`0x080C0000` | 640KB | 主固件 (.text/.rodata/.initcall) | 烧录时写入 |
| 11 | `0x080E0000` | 128KB | LDI 设备配置 (字体/动画/亮度参数) | `dev_flash_ldi` 擦写 |

**外部 Flash**（W25Qxx，SPI1，~8MB）：字库数据（5 字号×4 字型×2 编码的 40 个三元组），通过 `dev_storage_read` 接口访问。

## 软件分层架构

依赖方向严格单向（上层可见下层，反之不可）：

```
┌──────────────────────────────────────────────────────────────────┐
│  Application/  (app_*)                                           │
│  业务逻辑：boot编排、协议处理(IAP/LDI/AH_MQTT)、渲染引擎、        │
│  传感器管理、网络通道(UDP/TCP/MQTT/RS232/RS485任务)              │
│                                                                  │
│  依赖：Device/ + Kernel/ + Platform/ (不直接碰 HAL 地址)          │
├──────────────────────────────────────────────────────────────────┤
│  Device/  (dev_*)                                                │
│  设备抽象：OCP 虚表封装硬件差异                                   │
│  Display/  Storage/  Comm/  Network/  IO/  Config/               │
│                                                                  │
│  依赖：Platform/ + Kernel/ (通过 pl_* 接口操作硬件)               │
├──────────────────────────────────────────────────────────────────┤
│  Kernel/                                                         │
│  纯软件工具：ring_buffer、initcall、dispatch_types、text_cvt、   │
│  bit_utils、crc_utils                                            │
│                                                                  │
│  依赖：无（零硬件依赖，仅 stdint/stddef/stdbool）                 │
├──────────────────────────────────────────────────────────────────┤
│  Platform/  (pl_*)                                               │
│  HAL 薄封装：不透明句柄 (void*)，HAL 类型对外不可见               │
│  pl_spi  pl_tim  pl_uart  pl_gpio  pl_hub75  pl_eth  pl_net    │
│  pl_flash  pl_crc  pl_rtc  pl_iwdg  pl_dma  pl_dwt  pl_sys     │
│                                                                  │
│  依赖：Core/ + HAL 库 (仅 .c 文件 include Core 头文件)            │
├──────────────────────────────────────────────────────────────────┤
│  Core/                                                           │
│  STM32CubeMX 生成：MX_xxx_Init()、ISR 声明、HAL 配置             │
│  main.c  system_stm32f4xx.c  stm32f4xx_it.c  FreeRTOSConfig.h   │
│                                                                  │
│  依赖：HAL 库 + CMSIS                                            │
├──────────────────────────────────────────────────────────────────┤
│  Compiler/                                                       │
│  startup.c (C 语言 Reset_Handler + 自定义向量表)                  │
│  STM32F407XX_FLASH.ld (链接脚本，含 initcall 段定义)              │
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
  ├─ SystemInit() — FPU 使能, 向量表重定位
  └─ main() (Core/Src/main.c)
       ├─ HAL_Init()                     ← HAL 库基础
       ├─ SystemClock_Config()           ← 168MHz
       ├─ initcall_run(__hw_initcall)    ← RTOS 前硬件初始化
       └─ app_boot()                     ← RTOS 编排
            ├─ osKernelInitialize()
            ├─ osThreadNew(init_task, prio=High, stack=512×4)
            └─ osKernelStart()
                 ├─ [FreeRTOS 接管: SysTick→PendSV 任务切换]
                 └─ init_task:
                      ├─ dev_eth_start()       ← LwIP + netif
                      ├─ sw_board_init()       ← initcall_run(__sw_initcall)
                      ├─ osThreadNew(half_sec_task)
                      └─ osThreadExit()        ← 自我销毁
```

### initcall 初始化顺序表

**hw_initcall (RTOS 前，main() 中执行)**：

| 顺序 | 层级 | 函数 | 文件 | 职责 |
|---|---|---|---|---|
| 0-pre | — | （空） | — | 预留 |
| 1-pl | pl | `pl_dma_init` | `pl_dma.c` | DMA 流初始化 |
| 1-pl | pl | `pl_gpio_init` | `pl_gpio.c` | GPIO 时钟使能 |
| 1-pl | pl | `pl_spi_init` | `pl_spi.c` | SPI1 初始化 |
| 1-pl | pl | `pl_tim_init` | `pl_tim.c` | TIM2/3/4 初始化 |
| 1-pl | pl | `pl_uart_init` | `pl_uart.c` | USART1/3/6 初始化 |
| 1-pl | pl | `pl_crc_init` | `pl_crc.c` | 硬件 CRC |
| 1-pl | pl | `pl_iwdg_init` | `pl_iwdg.c` | 看门狗 |
| 1-pl | pl | `pl_rtc_init` | `pl_rtc.c` | RTC |
| 1-pl | pl | `pl_adc_init` | `pl_adc.c` | ADC1 |
| 1-pl | pl | `pl_exti_init` | `pl_exti.c` | 外部中断 |
| 1-pl | pl | `pl_hub75_init` | `pl_hub75.c` | HUB75 GPIO |
| 1-pl | pl | `pl_flash_init` | `pl_flash.c` | Flash 解锁（空操作） |
| 1-pl | pl | `pl_rtt_init` | `pl_rtt.c` | SEGGER RTT |
| 2-dev | dev | `dev_display_init` | `dev_display.c` | HUB75 引脚 + DBG 冻结 |
| 2-dev | dev | `dev_display_p20_init` | `dev_display_p20.c` | BSRR 查表 + ops 绑定 |
| 2-dev | dev | `dev_w25qxx_init` | `dev_w25qxx.c` | SPI Flash JEDEC 识别 |
| 2-dev | dev | `dev_eth_init` | `dev_eth.c` | ETH MAC + DP83848 PHY |
| 2-dev | dev | `dev_rs485_init` | `dev_rs485.c` | RS485 通道 hw init |
| 2-dev | dev | `dev_rs232_init` | `dev_rs232.c` | RS232 通道 hw init |
| 2-dev | dev | `_dev_flash_int_init` | `dev_flash_int.c` | 内部 Flash ops 绑定 |
| 3-post | — | （空） | — | 预留 |

**sw_initcall (RTOS 后，init_task → sw_board_init 中执行)**：

| 顺序 | 层级 | 函数 | 文件 | 职责 |
|---|---|---|---|---|
| 0-pre | — | （空） | — | 预留 |
| 1-pl | pl | `pl_exti_sw_init` | `pl_exti.c` | EXTI 中断使能 |
| 2-dev | dev | `dev_display_start` | `dev_display.c` | 创建 scan_task + 启动 TIM3/4 |
| 3-app | app | `app_dispatch_init` | `app_dispatch.c` | 创建 ch_queue + frame_dispatch_task |
| 3-app | app | `_render_init` | `app_render.c` | 绑定 display + storage 句柄 |
| 4-post | — | （空） | — | 预留 |

### initcall 实现原理

链接脚本中定义两个特殊段：
```
.hw_initcall : { KEEP(*(SORT(.hw_initcall.0))) ... KEEP(*(SORT(.hw_initcall.3))) }
.sw_initcall : { KEEP(*(SORT(.sw_initcall.0))) ... KEEP(*(SORT(.sw_initcall.4))) }
```

每个 `*_initcall(fn)` 宏生成一个 `initcall_entry_t` 常量放入对应 section。`initcall_run(start, end)` 顺序遍历调用。同层内按函数名字母序排列（`SORT()` 保证确定性）。

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
│           osMessageQueuePut(rx_queue, &len) → 唤醒 uart_channel_task
│
├─ USART3_IRQHandler / USART6_IRQHandler  (同上)
│
├─ SPI1_IRQHandler           (pl_spi.c)
│   └─ HAL_SPI_IRQHandler → HAL_SPI_TxRxCpltCallback / HAL_SPI_RxCpltCallback
│       └─ rx_cplt_cb(s_ok=true) → dev_w25qxx DMA 完成通知
│
├─ DMA1_Stream1_IRQHandler   (pl_uart.c, USART3 RX DMA)
├─ DMA2_Stream1_IRQHandler   (pl_uart.c, USART6 RX DMA)
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
UART 空闲中断   ──→  osMessageQueuePut       ──→  uart_channel_task
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
- **Message Queue**：`uart_channel_task`、`frame_dispatch_task`、协议处理任务逐级通过队列传递数据指针
- **Semaphore**：UDP/TCP 通道用信号量协调连接/断开生命周期（链路断开→信号量释放→任务重建连接）
- **volatile 轮询**：W25Qxx SPI 半双工 DMA 读使用 `while(!s_ok) osDelay(1)` 轮询（历史原因：RTOS 未完全就绪时无法使用 event flags）

## RTOS 配置

`Core/Inc/FreeRTOSConfig.h`，FreeRTOS v10.3.1 + CMSIS-RTOS V2 封装：

| 参数 | 值 | 说明 |
|---|---|---|
| `configTICK_RATE_HZ` | 1000 | 1ms 时基（TIM7） |
| `configMAX_PRIORITIES` | 56 | 优先级范围 0-55 |
| `configTOTAL_HEAP_SIZE` | 20KB | heap_4 动态分配 |
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
| `scan_task` | 512B | Realtime(最高) | `dev_display_start` | 行扫描输出 | 无深层调用、无局部大数组，仅 event wait + ops 调用 |
| `init_task` | 512×4=2KB | High | `app_boot` | 初始化后退出 | `dev_eth_start`→`sw_board_init`→渲染测试，含 printf 和栈数组（`font_buf[512]`、`text_buf[256]`），曾因溢触发 HardFault，从 256×4 扩容 |
| `frame_dispatch_task` | 256×4=1KB | Normal | `app_dispatch_init` | 帧分发引擎 | 核心调度循环，含 `frame_msg_t`（~1052B），无深层嵌套 |
| `iap_handle_task` | 512×4=2KB | Normal | `app_iap` | IAP 帧处理 | CRC32 校验 + cmd 分派，含 256B 临时缓冲 |
| `ldi_handle_task` | 512×4=2KB | Normal | `app_ldi` | LDI 帧处理 | 显示内容解析与更新 |
| `ldi_timer_task` | 512×4=2KB | Normal | `app_ldi` | LDI 定时任务 | 周期性状态上报 |
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
  osMessageQueueGet(g_ch_queue) ←── uart_channel_task / udp_connect_task / ...
  rb_lock(rb) → probe → rb_read → osMessageQueuePut(frame_queue[i])
  
协议处理任务 (iap/ldi/ah_mqtt):
  osMessageQueueGet(frame_queue[i]) → 处理 → channel_send

UART 通道任务 (uart_channel_task):
  osMessageQueueGet(rx_queue, &len) ←── UART ISR 空闲中断
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
typedef struct { dev_display_t  me; } dev_display_p20_t;
typedef struct { dev_storage_t  me; uint32_t base_addr, sector; } dev_flash_int_t;
typedef struct { channel_t      me; pl_uart_handle_t uart; ... } uart_channel_t;
typedef struct { dev_w25qxx_t   me; pl_spi_handle_t spi; ... } dev_w25qxx_inner; // (注：w25qxx直接在结构内)

// 3. container_of 向上转型（Kernel/Inc/dispatch_types.h）
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
```

四个核心虚表：

| 虚表 | 定义位置 | 方法签名 | 实现者 |
|---|---|---|---|
| `dev_display_ops` | `dev_display.h` | `prepare(dev)` / `scan(dev,line)` / `set_row(row)` | `dev_display_p20.c` |
| `dev_storage_ops` | `dev_storage.h` | `init/read/write/erase/capacity(dev,...)` | `dev_w25qxx.c` / `dev_flash_int.c` |
| `ch_ops` | `dispatch_types.h` | `send(ch, data, len)` | `dev_uart_channel.c` / `app_udp.c` / TCP/MQTT |
| `proto_probe_fn_t` | `app_dispatch.h` | `probe(ch, rb, &total_len, &aux)` | `app_iap.c` / `app_ldi.c` |

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

### P20 模组 (`dev_display_p20.c`)

单模块 16×8 像素，2 通道，2 模块横向排列 → **16 列 × 16 行** 屏幕，静态扫描。

`g_bsrr[4][8]` CCMRAM 预计算查表：每个颜色通道（R1/G1/B1/R2/G2/B2）×8 色阶 = `p20_bsrr_t`（含 r/g/b 三组 `{GPIO_TypeDef *port; uint32_t val}`），`_p20_init` 时根据 HUB75 引脚定义填入。扫描时直接 `pl_hub75_bsrr_flush(&b->r/g/b)` 无需分支。

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

两个实例共享一套 ops：

| 实例 | 基地址 | 大小 | 用途 |
|---|---|---|---|
| `g_flash_iap` | `0x08004000` (Sector 1) | 16KB | IAP 系统配置 |
| `g_flash_ldi` | `0x080E0000` (Sector 11) | 128KB | LDI 设备配置 |

- `_read`：直接内存映射 `memcpy(buf, (void*)(base+addr), len)`
- `_write`：`pl_flash_unlock → pl_flash_program_word × N → pl_flash_lock`（按 word 编程）
- `_erase`：`pl_flash_erase_sector(sector, voltage)`

## Render 引擎 (`Application/Src/app_render.c`)

数据驱动字库引擎，字库存储在 W25Qxx Flash 中，模块 `sw_app_initcall` 自注册。

### 字库组织
- 字库 = **(字号, 编码, 字型)** 三元组的顺序拼接
- `g_font_lib[40]` 描述表 = 5 字号×2 编码(ASCII/GBK)×4 字型(宋/仿宋/楷/黑)
- `_glyph_bytes(key)` = 每字符字节数：ASCII `(size/2宽+7)/8×size`，GBK `(size宽+7)/8×size`
- `_font_offset(key)` = 线性扫描累加 unit_size，找到目标单元的起始偏移
- `_char_addr(key, ch)` = 单字符 Flash 地址：ASCII `base+(ch-0x20)×bpc`，GBK `base+((hi-0x81)×190+(lo-offset))×bpc`

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
- 渲染风格 `render_style_t`：`h_align` / `v_align` / `word_wrap`（文字专属）

## 协议分发层 (`Application/Src/app_dispatch.c`)

### 核心数据结构 `dispatch_ctx_t`

| 字段 | 类型 | 用途 |
|---|---|---|
| `proto_count` | `uint8_t` | 运行时自动递增 |
| `proto_rb[i]` | `ring_buffer_t*[]` | 协议→缓冲区 |
| `proto_probe[i]` | `proto_probe_fn_t[]` | 协议→探测函数 |
| `frame_queue[i]` | `osMessageQueueId_t[]` | 协议→帧队列 |
| `buf_pool[id]` | `ring_buffer_t*[4]` | 缓冲区池（懒初始化） |
| `ch_queue` | `osMessageQueueId_t` | 通道通知队列 |
| `ch_proto_map[ch_id]` | `proto_mask_t[]` | 通道→协议掩码 |
| `channels[ch_id]` | `channel_t*[]` | 通道注册表 |

### 数据流

**接收**：通道任务 → `app_channel_dispatch(ch,data,len)` → `rb_write` → `osMessageQueuePut(ch_queue,&ch)` → `frame_dispatch_task` → probe → `rb_read` → `osMessageQueuePut(frame_queue[i],&msg)` → 协议任务

**发送**：协议任务 → `channel_send(ch,data,len)` → `ch->ops->send(ch,data,len)` → UART DMA / LWIP netconn

### 新增协议步骤

1. 定义 `proto_probe_fn_t` 探测函数，返回 `PROTO_PROBE_READY/WAIT/SKIP/FAKE`
2. 协议模块 init 函数中：`app_proto_acquire_buf`→`app_proto_register`→`app_proto_bind_channel`（可多次）→`osThreadNew`
3. 协议任务中：`osMessageQueueNew`→`app_proto_set_frame_queue`→循环 `osMessageQueueGet`→处理
4. `sw_app_initcall(module_init)` 自注册

## UART 通道子系统 (`Device/Comm/`)

`uart_channel_t` 继承 `channel_t`，统一定义 send / task / init / deinit 流程：

```
hw_initcall:  dev_rs485_init / dev_rs232_init
                └─ uart_channel_init(self, uart, ch_id, rs485_mode, buf, size)
                    设置: self->me.ch_id + ops + uart 句柄 + rs485_mode + buf

RTOS后:       app_rs485_start / app_rs232_start
                └─ osThreadNew(uart_channel_task, self)
                     ├─ osMessageQueueNew(rx_queue)      ← ISR→任务通知队列
                     ├─ app_channel_register(ch_id, &me)  ← 通道注册
                     ├─ pl_uart_set_rx_cb → ISR 适配器注册
                     ├─ pl_uart_start_rx(buf, size)       ← 启动 DMA 接收
                     └─ 循环: osMessageQueueGet(rx_queue, &len)
                           → app_channel_dispatch(&me, buf, len)
```

**板级差异**（`dev_rs485.c` / `dev_rs232.c`）：仅 UART 实例、RE 引脚方向回调、DMA 缓冲区不同。通道逻辑完全复用 `dev_uart_channel`。

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
       3. udp_channel_deinit: ops=nullptr, state=DOWN, app_channel_register(NULL)
       4. osSemaphoreRelease 通知 udp_task 重连
```

**IP 隔离**：`udp_channel_t` 存储源 IP 为 `uint8_t src_ip[4]` 字节数组，而非 LwIP `ip_addr_t`，避免 Application 层暴露 middleware 类型。

## Kernel 工具库 (`Kernel/`)

| 文件 | 关键 API | 说明 |
|---|---|---|
| `ring_buffer.h` | `RB_DEFINE(name,sz)`、`rb_read/write/peek/skip/flush` | 编译期静态分配，每个 API 通过 `void *mutex` 参数控制锁（传 `rb->mutex` 自动加锁，`nullptr` 跳过） |
| `initcall.h` | `hw_pl/dev_initcall`、`sw_pl/dev/app_initcall` | 6 个层级宏，生成 linker section 条目 |
| `dispatch_types.h` | `channel_t`、`ch_ops_t`、`channel_id_t`、`proto_probe_sta_t`、`container_of` | Device↔Application 共用类型 |
| `text_cvt.h` | `UTF8ToGBK` | UTF-8→GBK 编码转换 |
| `bit_utils.h` | `bit_ctz` | 协议掩码→数组索引 |
| `crc_utils.h` | CRC 校验 | — |

## 代码风格

- `.clang-format`：Microsoft 基础，4 空格缩进，无 Tab，Linux 大括号，不限制列宽
- 语言：C23，中文注释
- 命名：`pl_`=Platform、`dev_`=Device、`app_`=Application
- 派生类基类成员统一 `me`（如 `dev_display_t me`、`channel_t me`）
- 函数命名：`_` 前缀 = 内部静态函数、模块前缀 `pl_`/`dev_`/`app_` = 公开 API
- GPIO 定义遵循 CubeMX 命名
