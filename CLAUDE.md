# CLAUDE.md

此文件为 Claude Code (claude.ai/code) 在此仓库中工作时提供指导。

## 构建系统

这是一个 STM32F407ZGTx 嵌入式项目，使用 **eIDE** 插件配合 VS Code 开发。工具链为 `arm-none-eabi-gcc`。

- **构建**：使用 VS Code 任务 `build`（调用 `eide.project.build`），或命令行运行 `eide build`
- **烧录**：使用 VS Code 任务 `flash`（OpenOCD + 自定义配置），或 `openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; program ./build/Debug/Project_STD.hex verify reset exit"`
- **整片擦除**：`openocd -f ./Compiler/stm32f407zg.cfg -c "init; halt; stm32f4x unlock 0; stm32f4x mass_erase 0; shutdown"`
- **清理**：使用 VS Code 任务 `clean`
- **调试**：提供两种启动配置 — `probe-rs Test`（probe-rs 调试器）和 `Dap`（cortex-debug + OpenOCD）
- **编译数据库**：生成在 `build/Debug/compile_commands.json`
- **语言标准**：Debug 版本用 C23，Release 版本用 C17
- **预定义宏**：`USE_HAL_DRIVER`、`STM32F407xx`

## 硬件架构

**MCU**：STM32F407ZGTx（Cortex-M4F，168MHz，1024KB Flash，128KB SRAM + 64KB CCMRAM）

**内存布局**（见链接脚本 `Compiler/STM32F407XX_FLASH.ld`）：
- Flash 起始地址：`0x08040000`（低 256KB 保留，用于 bootloader/IAP）
- RAM：`0x20000000`（128KB）
- CCMRAM：`0x10000000`（64KB）
- 启动时重定位向量表：`SCB->VTOR = FLASH_BASE | 0x40000`

**已启用的外设**：ADC1、SPI1（W25Qxx Flash）、TIM2/3/4/7、USART1（RS485）、USART3（RS232-1）、USART6（RS232-2）、DMA、CRC、IWDG、RTC、Ethernet（DP83848 PHY）

## 软件架构

### RTOS（FreeRTOS + CMSIS-RTOS V2）

所有任务在 FreeRTOS 下运行，在 `Core/Src/freertos.c` 中初始化：

- **HalfSecTask** — 周期性任务：喂狗（IWDG）、翻转 LED、每 30 秒备份 RTC 寄存器
- **InitTask** — 一次性初始化：启动 LWIP、SEGGER RTT、W25Qxx Flash、协议通道，然后创建各协议任务线程。初始化完成后将自己挂起。

InitTask 中启动的网络任务：`udpManageTask`，可选还有 `mqttManageTask`、`tcpServerTask`、`tcpClientTask`、`rs2321ManageTask`、`rs2322ManageTask`、`rs485ManageTask`。

### 协议分发层（`Drivers/BSP/Protocol/protocol.c`）

这是整个系统的核心架构模式。任意信道（UART、TCP、UDP、MQTT）的数据都通过统一的分发系统流转：

1. **信道元数据**（`ch_meta_t`）描述一个通信信道——包含类型（UART/TCP/UDP/MQTT）、协议掩码、以及信道专用句柄
2. **环形缓冲区**按*组*组织（`RB_GROUP_COUNT` = 2）：组 0 给 IAP（高可靠协议），组 1 给普通业务协议
3. 每个协议（IAP、AH_MQTT）都有一个**探测函数**（`proto_probe_fn_t`），检查环形缓冲区并返回 `PROTO_PROBE_READY`（找到完整帧）、`PROTO_PROBE_WAIT`（数据不足）、`PROTO_PROBE_FAKE`（头数据无效）
4. `frame_dispatch_task` 循环：从 `g_meta_queue` 接收信道元数据 → 遍历各协议 → 调用探测函数 → 提取完整帧 → 投递到对应协议的消息队列（`g_frame_queue[i]`）
5. 每个协议处理任务阻塞在自己的消息队列上，逐帧处理
6. 发送数据使用 `channel_send()`，根据信道类型分发到正确的传输层

**新增协议的步骤**：定义探测函数，在 `channel_init()` 中注册（设置 `g_proto_to_group[proto_index(mask)]` 和 `g_proto_probers[index]`），创建一个从 `g_frame_queue[index]` 读取消息的处理任务，同时将 `PROTOCOL_CNT` 加 1。

### 显示子系统（`Drivers/BSP/Display/`）

驱动 HUB75 LED 点阵屏（128×64 像素 = 8×4 个 16×16 像素模组，每个模组 2 通道）：
- `display.c`/`.h` — 像素映射、颜色定义、扫描控制
- `scan.c` — HUB75 扫描输出（当前被排除在构建之外）
- `render.c`/`.h` — 文字/字体渲染
- `text_cvt/` — 文本转换工具
- TIM3 中断触发 `send_hub75_buff()` 进行扫描行 DMA 输出
- TIM4 中断触发 `pwm_light_handle()` 进行亮度控制

### BSP 组件（`Drivers/BSP/Components/`）

- **HUB75** — 底层 HUB75 GPIO 位拆裂驱动
- **W25Qxx** — SPI NOR Flash 驱动（W25Q256）
- **Key** — 按键/拨码开关处理（EXTI 中断）
- **RS232_485** — 基于 UART 的 RS232/RS485 收发（带环形缓冲区）
- **LightSensor** — 环境光传感器（基于 ADC）
- **IOCtrl** — GPIO 输出控制（车道灯、闪光灯）
- **dp83848** — 以太网 PHY 驱动

### 网络协议栈（`LWIP/`）

- LwIP 协议栈在 `LWIP/App/lwip.c` 中初始化（`MX_LWIP_Init()`）
- `LWIP/Target/ethernetif.c` — 以太网接口（STM32 MAC + DP83848 PHY）
- `LWIP/Target/lwipopts.h` — LwIP 配置
- 应用层：`mqtt_app`、`tcp_server_app`、`tcp_client_app`、`udp_app`

### 调试输出

SEGGER RTT（通道 0，在 `launch.json` 中配置）提供来自目标板的 `printf` 输出。可使用 `SEGGER_RTT_printf()` 或标准 `printf()`（通过 `SEGGER_RTT_Syscalls_GCC.c` 钩入）。

## 代码风格

- `.clang-format` 配置：Microsoft 基础风格，4 空格缩进，不使用 Tab，Linux 大括号风格，不限制列宽
- 语言：C23（Debug）/ C17（Release），中文注释
- `main.h` 中的 GPIO 引脚定义遵循 STM32CubeMX 命名规范：`外设_Pin` / `外设_GPIO_Port`
