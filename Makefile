# Project_STD Makefile
# 参照 .eide/eide.yml Debug 目标配置生成

# ---- Toolchain ----
TOOLCHAIN ?= gcc

ifeq ($(TOOLCHAIN),gcc)
  CC       = arm-none-eabi-gcc
  LD       = arm-none-eabi-gcc
  TC_FLAGS =
  SPECS    = --specs=nano.specs --specs=nosys.specs
else
  CC       = clang
  LD       = arm-none-eabi-gcc
  TC_FLAGS = --target=arm-none-eabi --sysroot=/usr/arm-none-eabi
  SPECS    = --specs=nano.specs --specs=nosys.specs
endif

OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# ---- Directories ----
CONFIG    ?= Debug
# 构建目录**必须按板分**：两块板的同名目标文件（如 main.o）内容不同，
# 共用一个目录会互相覆盖，链接出的是混合产物且不会有任何报错。
BUILD_DIR  = build/$(BOARD)/$(CONFIG)

# ---- 板级变体 ----
# 同一套 Kernel/Platform/Device/Application 共享代码，配不同板子编译。
# 该板专属的源与头都在 boards/$(BOARD)/ 下（CubeMX 产物、显示模组、板级外设、
# 板级通道）。新增一块板 = 复制一份 boards/<名字>/ 并改这一行。
BOARD     ?= std_a
BOARD_DIR  = boards/$(BOARD)

# ---- MCU Flags ----
CPU       = -mcpu=cortex-m4
FPU       = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU_FLAGS = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ---- Common Flags ----
DEFINES = -DUSE_HAL_DRIVER -DSTM32F407xx

INC_DIRS = \
	-I $(BOARD_DIR)/Inc \
	-I Application/Inc \
	-I Application/Inc/IAP \
	-I Application/Inc/LDI \
	-I Application/Inc/RLS \
	-I Application/Inc/AH_MQTT \
	-I Application/Inc/Channel \
	-I Device/Inc \
	-I Platform/Inc \
	-I Kernel/Inc \
	-I $(BOARD_DIR)/Core/Inc \
	-I Drivers/CMSIS/Include \
	-I Drivers/CMSIS/Device/ST/STM32F4xx/Include \
	-I Drivers/STM32F4xx_HAL_Driver/Inc \
	-I Middlewares/Third_Party/SEGGER_RTT \
	-I Middlewares/Third_Party/LwIP/src/include \
	-I Middlewares/Third_Party/LwIP/system \
	-I Middlewares/Third_Party/FreeRTOS/Source/include \
	-I Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
	-I Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
	-I Compiler

# ---- C Flags ----
CFLAGS  = $(MCU_FLAGS) $(DEFINES) $(INC_DIRS) $(TC_FLAGS)
CFLAGS += -std=gnu23
CFLAGS += -Og -g
CFLAGS += -Wall -Wextra
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common
CFLAGS += -fno-exceptions
CFLAGS += -fshort-enums
CFLAGS += -MMD -MP   # emit <obj>.d header deps (consumed by -include at EOF)

# ---- LDFLAGS ----
LDSCRIPT  = $(BOARD_DIR)/STM32F407XX_FLASH.ld
LDFLAGS  = $(MCU_FLAGS)
LDFLAGS += -T $(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/Project_STD.map,--cref
LDFLAGS += -Wl,--gc-sections
LDFLAGS += $(SPECS)
LDFLAGS += -u _printf_float
LDFLAGS += -lm

# ---- Source Files ----
# CubeMX 产物（属板级：外设集、引脚、时钟、中断向量都随板子变）
SRC_CORE = \
	$(BOARD_DIR)/Core/Src/main.c \
	$(BOARD_DIR)/Core/Src/stm32f4xx_it.c \
	$(BOARD_DIR)/Core/Src/syscalls.c \
	$(BOARD_DIR)/Core/Src/sysmem.c \
	$(BOARD_DIR)/Core/Src/adc.c \
	$(BOARD_DIR)/Core/Src/dma.c \
	$(BOARD_DIR)/Core/Src/gpio.c \
	$(BOARD_DIR)/Core/Src/iwdg.c \
	$(BOARD_DIR)/Core/Src/rtc.c \
	$(BOARD_DIR)/Core/Src/spi.c \
	$(BOARD_DIR)/Core/Src/stm32f4xx_hal_msp.c \
	$(BOARD_DIR)/Core/Src/stm32f4xx_hal_timebase_tim.c \
	$(BOARD_DIR)/Core/Src/system_stm32f4xx.c \
	$(BOARD_DIR)/Core/Src/tim.c \
	$(BOARD_DIR)/Core/Src/usart.c \
	$(BOARD_DIR)/Core/Src/crc.c \

# LWIP
# STM32 HAL Driver
SRC_HAL = \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_iwdg.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_crc.c

# BSP (旧协议层依赖 display/render/text_cvt，等协议迁移完成后移除)

# display/render 已迁移至 dev_display + app_render; text_cvt 已移至 Kernel

# BSP Protocol

# RTT
SRC_RTT = \
	Middlewares/Third_Party/SEGGER_RTT/SEGGER_RTT.c \
	Middlewares/Third_Party/SEGGER_RTT/SEGGER_RTT_printf.c \
	Middlewares/Third_Party/SEGGER_RTT/SEGGER_RTT_Syscalls_GCC.c

# LwIP Middleware
# LwIP Middleware
SRC_LWIP = \
	Middlewares/Third_Party/LwIP/system/OS/sys_arch.c \
	Middlewares/Third_Party/LwIP/src/api/api_lib.c \
	Middlewares/Third_Party/LwIP/src/api/api_msg.c \
	Middlewares/Third_Party/LwIP/src/api/err.c \
	Middlewares/Third_Party/LwIP/src/api/if_api.c \
	Middlewares/Third_Party/LwIP/src/api/netbuf.c \
	Middlewares/Third_Party/LwIP/src/api/netdb.c \
	Middlewares/Third_Party/LwIP/src/api/netifapi.c \
	Middlewares/Third_Party/LwIP/src/api/sockets.c \
	Middlewares/Third_Party/LwIP/src/api/tcpip.c \
	Middlewares/Third_Party/LwIP/src/core/altcp.c \
	Middlewares/Third_Party/LwIP/src/core/altcp_alloc.c \
	Middlewares/Third_Party/LwIP/src/core/altcp_tcp.c \
	Middlewares/Third_Party/LwIP/src/core/def.c \
	Middlewares/Third_Party/LwIP/src/core/dns.c \
	Middlewares/Third_Party/LwIP/src/core/inet_chksum.c \
	Middlewares/Third_Party/LwIP/src/core/init.c \
	Middlewares/Third_Party/LwIP/src/core/ip.c \
	Middlewares/Third_Party/LwIP/src/core/mem.c \
	Middlewares/Third_Party/LwIP/src/core/memp.c \
	Middlewares/Third_Party/LwIP/src/core/netif.c \
	Middlewares/Third_Party/LwIP/src/core/pbuf.c \
	Middlewares/Third_Party/LwIP/src/core/raw.c \
	Middlewares/Third_Party/LwIP/src/core/stats.c \
	Middlewares/Third_Party/LwIP/src/core/sys.c \
	Middlewares/Third_Party/LwIP/src/core/tcp.c \
	Middlewares/Third_Party/LwIP/src/core/tcp_in.c \
	Middlewares/Third_Party/LwIP/src/core/tcp_out.c \
	Middlewares/Third_Party/LwIP/src/core/timeouts.c \
	Middlewares/Third_Party/LwIP/src/core/udp.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/autoip.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/igmp.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c \
	Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/dhcp6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/ethip6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/icmp6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/inet6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/ip6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/ip6_addr.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/ip6_frag.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/mld6.c \
	Middlewares/Third_Party/LwIP/src/core/ipv6/nd6.c \
	Middlewares/Third_Party/LwIP/src/apps/mqtt/mqtt.c \
	Middlewares/Third_Party/LwIP/src/netif/bridgeif.c \
	Middlewares/Third_Party/LwIP/src/netif/bridgeif_fdb.c \
	Middlewares/Third_Party/LwIP/src/netif/ethernet.c \
	Middlewares/Third_Party/LwIP/src/netif/lowpan6.c \
	Middlewares/Third_Party/LwIP/src/netif/lowpan6_ble.c \
	Middlewares/Third_Party/LwIP/src/netif/lowpan6_common.c \
	Middlewares/Third_Party/LwIP/src/netif/slipif.c \
	Middlewares/Third_Party/LwIP/src/netif/zepif.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/auth.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/ccp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/chap_ms.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/chap-md5.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/chap-new.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/demand.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/eap.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/ecp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/eui64.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/fsm.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/ipcp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/ipv6cp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/lcp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/magic.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/mppe.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/multilink.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/ppp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/pppapi.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/pppcrypt.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/pppoe.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/pppol2tp.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/pppos.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/upap.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/utils.c \
	Middlewares/Third_Party/LwIP/src/netif/ppp/vj.c


# FreeRTOS
SRC_FREERTOS = \
	Middlewares/Third_Party/FreeRTOS/Source/croutine.c \
	Middlewares/Third_Party/FreeRTOS/Source/event_groups.c \
	Middlewares/Third_Party/FreeRTOS/Source/list.c \
	Middlewares/Third_Party/FreeRTOS/Source/queue.c \
	Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c \
	Middlewares/Third_Party/FreeRTOS/Source/tasks.c \
	Middlewares/Third_Party/FreeRTOS/Source/timers.c \
	Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c \
	Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c \
	Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c

# Startup
SRC_STARTUP = \
	Compiler/startup.c

# Kernel
SRC_KERNEL = \
	Kernel/Src/initcall.c \
	Kernel/Src/ring_buffer.c \
	Kernel/Src/bit_utils.c \
	Kernel/Src/crc_utils.c \
	Kernel/Src/bcc_utils.c \
	Kernel/Src/text_cvt.c

# Platform（仅含无冲突的文件，其他在 Phase 3 逐步加入）
SRC_PLATFORM = \
	Platform/Src/pl_gpio.c \
	Platform/Src/pl_rtt.c \
	Platform/Src/pl_task.c \
	Platform/Src/pl_exti.c \
	Platform/Src/pl_net.c \
	Platform/Src/pl_eth.c \
	Platform/Src/pl_crc.c \
	Platform/Src/pl_iwdg.c \
	Platform/Src/pl_dma.c \
	Platform/Src/pl_dwt.c \
	Platform/Src/pl_tim.c \
	Platform/Src/pl_rtc.c \
	Platform/Src/pl_sys.c \
	Platform/Src/pl_flash.c \
	Platform/Src/pl_adc.c \
	Platform/Src/pl_spi.c \
	Platform/Src/pl_uart.c

# ---- 板级源 ----
# 清单由板自己声明：boards/<板>/board.mk 定义 SRC_BOARD。
# 放在这里而不是写死在顶层，是因为"本板有哪些源文件"本身就是板级事实 ——
# 例如 std_a 有两路 RS232 和两路灯控 IO，std_b 一个都没有。
# 显示模组**同时只能编一个**：每个驱动自带一份 CCMRAM 帧缓冲，多编一份直接
# 把 CCMRAM 顶爆（实测多两份超 588B）。同目录下的其他驱动是可替换的面板选项，
# 换屏时改 board.mk 里那一行，而不是追加。
include $(BOARD_DIR)/board.mk


# Device (仅 Project_STD 新模块，resend dev_* 等 Phase 6 Platform 集成后加入)
SRC_DEVICE = \
	Device/IO/dev_key.c \
	Device/Display/dev_display.c \
	Device/IO/dev_light_sensor.c \
	Device/Storage/dev_w25qxx.c \
	Device/Storage/dev_flash_int.c \
	Device/Storage/cfg_record.c \
	Device/Network/dev_dp83848.c \
	Device/Network/dev_eth.c

# Application (Project_STD 新模块，resend app_* 等 Phase 7 集成后加入)
SRC_APPLICATION = \
	Application/Src/app_test.c \
	Application/Src/app_factory_test.c \
	Application/Src/app_boot.c \
	Application/Src/app_dispatch.c \
	Application/Src/app_render.c \
	Application/Src/app_cfg_sched.c \
	Application/Src/app_diag.c \
	Application/Src/app_key.c \
	Application/Src/app_light_sensor.c \
	Application/Src/IAP/app_iap.c \
	Application/Src/IAP/app_iap_cmd.c \
	Application/Src/IAP/app_iap_cfg.c \
	Application/Src/LDI/app_ldi.c \
	Application/Src/LDI/app_ldi_cmd.c \
	Application/Src/LDI/app_ldi_cfg.c \
	Application/Src/LDI/app_vms_ctrl.c \
	Application/Src/RLS/app_rls.c \
	Application/Src/RLS/app_rls_cmd.c \
	Application/Src/AH_MQTT/ah_mqtt.c \
	Application/Src/AH_MQTT/ah_mqtt_cmd.c \
	Application/Src/Channel/app_udp.c \
	Application/Src/Channel/app_tcp_server.c \
	Application/Src/Channel/app_tcp_client.c \
	Application/Src/Channel/app_mqtt.c \
	Application/Src/Channel/app_rs485.c

# ---- All Sources ----
SRC_ALL = \
	$(SRC_KERNEL) \
	$(SRC_PLATFORM) \
	$(SRC_DEVICE) \
	$(SRC_BOARD) \
	$(SRC_APPLICATION) \
	$(SRC_CORE) \
	$(SRC_HAL) \
	$(SRC_RTT) \
	$(SRC_LWIP) \
	$(SRC_FREERTOS) \
	$(SRC_STARTUP)

# ---- Object Files ----
# board.mk 可选：本板不参与编译的**共享源**（板级源直接不写进 SRC_BOARD 即可）。
# 用 filter-out 而非让各板复制清单：排除项是少数、共享清单是多数，反过来的话
# 每加一个共享文件都要改每一块板。
# 必须放在 SRC_ALL **之后**：放前面的话 := 当场展开，那时 SRC_ALL 还是空的。
SRC_ALL := $(filter-out $(SRC_EXCLUDE),$(SRC_ALL))

OBJ_ALL = $(addprefix $(BUILD_DIR)/,$(SRC_ALL:.c=.o))

# ---- Targets ----
# 注意: test 必须列为 .PHONY —— 工程里已有一个同名 test/ 目录，
# 不加声明会被 make 当成"已存在且比依赖新"的文件而跳过配方。
.PHONY: all clean test

all: $(BUILD_DIR)/Project_STD.elf $(BUILD_DIR)/Project_STD.hex $(BUILD_DIR)/Project_STD.bin
	@echo "==== Build complete ===="
	@$(SIZE) $(BUILD_DIR)/Project_STD.elf

$(BUILD_DIR)/Project_STD.elf: $(OBJ_ALL)
	@echo "Linking $@"
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/Project_STD.hex: $(BUILD_DIR)/Project_STD.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/Project_STD.bin: $(BUILD_DIR)/Project_STD.elf
	$(OBJCOPY) -O binary $< $@

# ---- Compile Rule ----
$(BUILD_DIR)/%.o: %.c
	@echo "Compiling $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@# compile_commands.json 由 EIDE 生成、.clangd 消费；删掉会让编辑器当场失去索引，
	@# clangd 退化成启发式猜测并刷满假诊断（甚至崩溃）。这里先挪出、清完再放回。
	@if [ -f $(BUILD_DIR)/compile_commands.json ]; then \
		cp $(BUILD_DIR)/compile_commands.json build/.ccdb.bak; \
		echo "保留 compile_commands.json（clangd 索引）"; \
	fi
	rm -rf $(BUILD_DIR) build/test
	@if [ -f build/.ccdb.bak ]; then \
		mkdir -p $(BUILD_DIR); \
		mv build/.ccdb.bak $(BUILD_DIR)/compile_commands.json; \
	fi

# ---- Host Unit Tests ----
# 用 test/stubs 下的替身（cmsis_os2 用 pthread 实现、FreeRTOS.h/main.h/dev_display.h
# 只给形状），让生产源码**不替换、不改写**原样在 host 上编译运行。
# ASan/UBSan 常开：缓冲区越界、未对齐访问这类缺陷在硬件上极难构造，在这里是必现的。
#
# -I test/stubs 排在最前：FreeRTOS.h / main.h / dev_display.h 靠它遮蔽真头文件
# （真头文件会拉进 HAL、LwIP、ARM 移植层，host 编不了）。
# cmsis_os2.h 不设替身 —— 直接用工程自带的 CMSIS-RTOS V2 头（纯声明，host 可编译），
# 测试因此与固件看到的是同一份 API；替身只实现被引用到的原语，其余在链接期失败。
#
# --gc-sections 是关键：协议源文件整份编译，但只有探针真正被引用，任务 / initcall
# 等未引用段会被丢弃，因此不必为它们准备桩。
HOSTCC       = cc
TEST_BUILD   = build/test
TEST_INC     = \
	-I test/stubs \
	-I $(BOARD_DIR)/Inc \
	-I Application/Inc \
	-I Application/Inc/IAP \
	-I Application/Inc/LDI \
	-I Application/Inc/RLS \
	-I Application/Inc/Channel \
	-I Kernel/Inc \
	-I Platform/Inc \
	-I Device/Inc \
	-I Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2

TEST_CFLAGS  = -std=gnu23 -g -O1 -Wall -Wextra -fno-omit-frame-pointer \
               -ffunction-sections -fdata-sections \
               -fsanitize=address,undefined $(TEST_INC)

TEST_LDFLAGS = -fsanitize=address,undefined -lpthread -Wl,--gc-sections

# 套件一：ring_buffer（窥视路径的容量夹紧）
TEST_RB_SRCS = \
	Kernel/Src/ring_buffer.c \
	test/test_ring_buffer.c \
	test/stubs/os_stub.c

# 套件二：协议/通道分发引擎（真实 frame_dispatch_task 跑在 pthread 上）
TEST_DISPATCH_SRCS = \
	test/stubs/os_stub.c \
	test/test_dispatch.c \
	Application/Src/app_dispatch.c \
	Platform/Src/pl_task.c \
	Kernel/Src/ring_buffer.c

# 套件三：协议探针（IAP / LDI / RLS 真探针，各自独立 TU）
TEST_PROBES_SRCS = \
	test/stubs/os_stub.c \
	test/stubs/pl_crc_stub.c \
	test/test_probes.c \
	Platform/Src/pl_task.c \
	Application/Src/IAP/app_iap.c \
	Application/Src/LDI/app_ldi.c \
	Application/Src/RLS/app_rls.c \
	Kernel/Src/ring_buffer.c \
	Kernel/Src/crc_utils.c

# 套件四：配置调度器（记录读写 + W25Qxx 尾部配置区的块位扫描）
# os_stub 提供 osMutexNew/Acquire/Release（调度器里那把串行化 save 的锁；host 上
# _cfg_sched_init 未被执行，s_lock 为 NULL，调用点都带空守卫）。
# dev_w25qxx_get() 的桩与假 Flash 在测试文件里。
# 注意：**不要**把 Application/Src/LDI/app_ldi_cfg.c 列进来 —— 测试文件直接
# include 了它的实现 TU（为了触达 static 的注册入口，见测试文件顶部说明），
# 重复编译会符号重定义。
TEST_CFG_SCHED_SRCS = \
	test/stubs/os_stub.c \
	test/test_cfg_sched.c \
	Device/Storage/cfg_record.c \
	Application/Src/app_cfg_sched.c \
	Kernel/Src/crc_utils.c

test: $(TEST_BUILD)/test_ring_buffer $(TEST_BUILD)/test_dispatch $(TEST_BUILD)/test_probes \
      $(TEST_BUILD)/test_cfg_sched
	@echo "──── ring_buffer ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_ring_buffer
	@echo ""
	@echo "──── 分发引擎 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_dispatch
	@echo ""
	@echo "──── 协议探针 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_probes
	@echo ""
	@echo "──── 配置调度器 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_cfg_sched

$(TEST_BUILD)/test_ring_buffer: $(TEST_RB_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(TEST_BUILD)/test_dispatch: $(TEST_DISPATCH_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(TEST_BUILD)/test_probes: $(TEST_PROBES_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(TEST_BUILD)/test_cfg_sched: $(TEST_CFG_SCHED_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ---- Header Dependencies ----
# -MMD writes <obj>.d next to each object; -MP adds phony targets so deleting a
# header does not break the build. Without this, editing a header does not
# trigger recompilation and "it builds" refers to a stale binary.
-include $(OBJ_ALL:.o=.d)
