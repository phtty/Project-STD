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
BOARD     ?= 3833024
BOARD_DIR  = boards/$(BOARD)

# ---- MCU Flags ----
CPU       = -mcpu=cortex-m4
FPU       = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU_FLAGS = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ---- Common Flags ----
DEFINES = -DUSE_HAL_DRIVER -DSTM32F407xx

# 板级头按层分布，镜像主树：Platform/Inc、Device/Inc、Application/Inc 各管自己那层，
# board.h（纯板级配置，不分层）放板根。原来的 -I $(BOARD_DIR)/Inc 已拆成这四条。
INC_DIRS = \
	-I $(BOARD_DIR) \
	-I $(BOARD_DIR)/Platform/Inc \
	-I $(BOARD_DIR)/Device/Inc \
	-I $(BOARD_DIR)/Application/Inc \
	-I Application/Inc \
	-I Application/Inc/IAP \
	-I Application/Inc/LDI \
	-I Application/Inc/RLS \
	-I Application/Inc/AH_MQTT \
	-I Application/Inc/Channel \
	-I Application/Inc/CASCADE \
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
	test/stubs/pl_crc_stub.c \
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
# 例如 3833024 有两路 RS232 和两路灯控 IO，5006048 一个都没有。
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
	Application/Src/app_screen.c \
	Application/Src/CASCADE/app_cascade.c \
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
# 工程级排除：AH_MQTT 暂未启用，所有板都不编。
# **必须与 eIDE 保持一致** —— 两个 target 的 excludeList 里都有
# <virtual_root>/Application/protocol/ah，这里不排的话，同一份源码在
# Makefile 与 eIDE 下会产出不同的固件。
SRC_EXCLUDE = Application/Src/AH_MQTT/ah_mqtt.c
SRC_EXCLUDE += Application/Src/AH_MQTT/ah_mqtt_cmd.c

# board.mk 还可以追加本板不参与编译的**共享源**（板级源直接不写进 SRC_BOARD 即可）。
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
	rm -rf $(BUILD_DIR) build/$(BOARD)/test
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
# 与 BUILD_DIR 同理，**必须按板分开**：两板的字库表、board.h 都不同，而且都参与
# 测试编译。共用目录时，切 BOARD 后 make 看到二进制比"本板源文件"还新（本板源文件
# 压根没动过），于是不重编 —— 直接跑另一块板编出来的二进制，且**退出码是 0**。
# 这正是固件那边踩过的坑（同名的 .o 在板间静默混用），测试侧同样适用。
TEST_BUILD   = build/$(BOARD)/test
TEST_INC     = \
	-I test/stubs \
	-I $(BOARD_DIR) \
	-I $(BOARD_DIR)/Platform/Inc \
	-I $(BOARD_DIR)/Device/Inc \
	-I $(BOARD_DIR)/Application/Inc \
	-I Application/Inc \
	-I Application/Inc/IAP \
	-I Application/Inc/LDI \
	-I Application/Inc/RLS \
	-I Application/Inc/CASCADE \
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

# 套件五：IAP 记录（内部 Flash Sector 1）
# pl_flash_stub 把内部 Flash 换成 RAM —— 掉电时序在真机上构造不出来。
# os_stub 提供那把串行化用的互斥量（pthread 实现）。
# 注意：**不要**把 Application/Src/IAP/app_iap_cfg.c 列进来 —— 测试文件直接
# include 了它的实现 TU（为了调用 static 的 _iap_cfg_lock_init，host 上
# sw_dev_initcall 不执行），重复编译会符号重定义。
TEST_IAP_CFG_SRCS = \
	test/stubs/os_stub.c \
	test/stubs/pl_crc_stub.c \
	test/stubs/pl_flash_stub.c \
	test/test_iap_cfg.c \
	Device/Storage/dev_flash_int.c

# 被测试文件 #include 的实现 TU：**不参与编译，但必须参与依赖**。
# 它们不在任何 TEST_*_SRCS 里（列进去会符号重定义），于是 make 看不见它们 ——
# 改了被测源码测试不会重编，跑的还是旧二进制，"反向验证"会得到假的绿。
# 用单独的规则行把它们挂成依赖，配方里则显式列 SRCS（不能用 $^，那会把它们也编进去）。
# 目前两处：test_cfg_sched ← app_ldi_cfg.c、test_iap_cfg ← app_iap_cfg.c，
# 各自的依赖行分别写在对应的规则下面。

# 套件六：LDI 0AH 跨两条记录的交互
# 三个实现 TU 由测试文件直接 include（static 注册/锁初始化从外部够不到），
# 故这里都不列；依赖行在下面单独挂。
TEST_LDI_0AH_SRCS = \
	test/stubs/os_stub.c \
	test/stubs/pl_crc_stub.c \
	test/stubs/pl_flash_stub.c \
	test/test_ldi_0ah.c \
	Device/Storage/dev_flash_int.c \
	Device/Storage/cfg_record.c \
	Application/Src/app_cfg_sched.c \
	Application/Src/LDI/app_ldi.c \
	Kernel/Src/crc_utils.c \
	Kernel/Src/ring_buffer.c

# 套件七：ISR 在 initcall 之前的安全性（上机卡死过的那一类）
# 只编两个 Platform .c；HAL 由 test/stubs/stm32f4xx_hal.h 给形状，
# 板级表 g_pl_tim_board / g_pl_uart_board 由测试文件自己提供。
TEST_ISR_PREINIT_SRCS = \
	test/test_isr_preinit.c \
	test/stubs/os_stub.c \
	Platform/Src/pl_tim.c \
	Platform/Src/pl_uart.c

# 套件八：板级字库表（累加偏移必须逐条等于实物映像基址）
# 只编板级表本身 —— app_render.c 的依赖太重（W25Qxx / RTOS / 配置调度器），
# 它的消费逻辑由本套件的结构性断言与 _render_init 的运行期校验共同覆盖。
# 按 BOARD 分板编译：两版字库的字号集合、字符集、单元顺序都不同。
TEST_FONT_LIB_SRCS = \
	test/test_font_lib.c \
	$(BOARD_DIR)/Application/Src/font_lib_board.c

# 套件九：整屏画布（经画布渲染必须与直写实屏逐像素相等）
# 用例直接 include app_screen.c（sink 是 static），并自己提供 dev_display 原语作为
# 独立参考实现 —— 所以不列 app_screen.c，也不列 Device/Display/dev_display.c。
TEST_SCREEN_CANVAS_SRCS = \
	test/test_screen_canvas.c \
	test/stubs/os_stub.c

# 套件十二：切分表与抽带（画布上的矩形 → 1bpp 位图，必须与独立参考逐位相等）
# 同套件九：用例 include app_screen.c（sink 与抽带都是 static），并自带一套
# 1B/px 帧缓冲作为独立参考 —— 所以不列 app_screen.c，也不列 dev_display.c。
# 本套件的本卡地址被钉成 1（非原点矩形），故与套件九不是重复覆盖。
TEST_SCREEN_LAYOUT_SRCS = \
	test/test_screen_layout.c \
	test/stubs/os_stub.c

# 套件十三：级联图传 · 从卡侧（分片暂存 / 陈旧分片 / 短分片 / 缺片应答 / NACK）
# 用例 include app_cascade.c（探针与分派表都是 static），用真探针 + 真分派表，
# 只把总线与 app_screen 换成替身。
TEST_CASCADE_ROUND_SRCS = \
	test/test_cascade_round.c \
	test/stubs/pl_crc_stub.c \
	Kernel/Src/ring_buffer.c \
	test/stubs/os_stub.c

# 套件十四：级联图传 · 主卡侧（开轮、分片下发、结算、定向重传、本地提交门控）
# 同套件十三：用例 include app_cascade.c。总线那头坐着一个**假从卡**，
# 它收到的分片经真探针 + 真队列喂回主卡 —— 分帧、探针、等待循环都是真的。
TEST_CASCADE_MASTER_SRCS = \
	test/test_cascade_master.c \
	test/stubs/pl_crc_stub.c \
	Kernel/Src/ring_buffer.c \
	test/stubs/os_stub.c

# 套件十：硬件 CRC32 封装（任意长度/对齐都要算全，不得截断）
# 链接**真的** Platform/Src/pl_crc.c —— 桩会掩盖这类缺陷，测真文件才有意义。
TEST_CRC_SRCS = \
	test/test_crc.c \
	test/stubs/pl_crc_stub.c \
	test/stubs/os_stub.c

# 套件十一：级联协议探针（四态 / 地址过滤 / 长度域 / 与既有协议互不毒化）
# 用例 include app_cascade.c（探针是 static）；除探针真正用到的 rb 与 CRC 外，
# 其余符号都要给桩 —— 探针只窥视，不碰通道与队列。
TEST_CASCADE_FRAME_SRCS = \
	test/test_cascade_frame.c \
	test/stubs/pl_crc_stub.c \
	Kernel/Src/ring_buffer.c \
	test/stubs/os_stub.c

# 套件十五：RS485 收包槽位（环回拆帧 / 投递失败不漏槽）
# 用例 include app_rs485.c（rs485_isr_cb 与槽位都是 static），只把 HAL 替身接上；
# 队列用**生产的那份属性**，所以深度与缓冲定容写错也会被发现。
TEST_RS485_SLOTS_SRCS = \
	test/test_rs485_slots.c \
	test/stubs/os_stub.c \
	Kernel/Src/ring_buffer.c

# 注意：新加套件时**必须同时**加进上面的依赖列表**和**下面的运行段。
# 只加依赖的话 make 会编它但永远不跑 —— 看起来像覆盖了，实际一条断言都没执行。
test: $(TEST_BUILD)/test_ring_buffer $(TEST_BUILD)/test_dispatch $(TEST_BUILD)/test_probes \
      $(TEST_BUILD)/test_cfg_sched $(TEST_BUILD)/test_iap_cfg $(TEST_BUILD)/test_ldi_0ah \
      $(TEST_BUILD)/test_isr_preinit $(TEST_BUILD)/test_font_lib \
      $(TEST_BUILD)/test_screen_canvas $(TEST_BUILD)/test_crc \
      $(TEST_BUILD)/test_cascade_frame $(TEST_BUILD)/test_screen_layout \
      $(TEST_BUILD)/test_cascade_round $(TEST_BUILD)/test_cascade_master \
      $(TEST_BUILD)/test_rs485_slots
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
	@echo ""
	@echo "──── IAP 记录 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_iap_cfg
	@echo ""
	@echo "──── LDI 0AH 跨记录 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_ldi_0ah
	@echo ""
	@echo "──── ISR 在 initcall 之前 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_isr_preinit
	@echo ""
	@echo "──── 板级字库表 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_font_lib
	@echo ""
	@echo "──── 整屏画布 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_screen_canvas
	@echo ""
	@echo "──── 硬件 CRC32 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_crc
	@echo ""
	@echo "──── 级联协议探针 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_cascade_frame
	@echo ""
	@echo "──── 切分表与抽带 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_screen_layout
	@echo ""
	@echo "──── 级联图传 · 从卡侧 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_cascade_round
	@echo ""
	@echo "──── 级联图传 · 主卡侧 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_cascade_master
	@echo ""
	@echo "──── RS485 收包槽位 ────"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_rs485_slots

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
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_CFG_SCHED_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_cfg_sched: Application/Src/LDI/app_ldi_cfg.c

$(TEST_BUILD)/test_font_lib: $(TEST_FONT_LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_FONT_LIB_SRCS) $(TEST_LDFLAGS)

# 换板换字库时必须重编本套件（期望表是按板 #if 选的）
$(TEST_BUILD)/test_font_lib: $(BOARD_DIR)/board.h

$(TEST_BUILD)/test_screen_canvas: $(TEST_SCREEN_CANVAS_SRCS) Application/Src/app_screen.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_SCREEN_CANVAS_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_crc: $(TEST_CRC_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_CRC_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_cascade_frame: $(TEST_CASCADE_FRAME_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_CASCADE_FRAME_SRCS) $(TEST_LDFLAGS)

# **必须单独挂依赖**：用例 TU-include 了 app_cascade.c，它不在 SRCS 里，
# make 看不见 —— 不挂这行的话改了被测源码测试不重编，跑的是旧二进制。
# （同 test_cfg_sched / test_iap_cfg / test_ldi_0ah / test_screen_canvas 的那几条。）
$(TEST_BUILD)/test_cascade_frame: Application/Src/CASCADE/app_cascade.c

# 同上：用例 TU-include 了 app_screen.c 与板级 board.h，都不在 SRCS 里。
$(TEST_BUILD)/test_screen_layout: $(TEST_SCREEN_LAYOUT_SRCS) Application/Src/app_screen.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_SCREEN_LAYOUT_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_screen_layout: Application/Src/app_screen.c $(BOARD_DIR)/board.h

# 同上：用例 TU-include 了 app_cascade.c。
$(TEST_BUILD)/test_cascade_round: $(TEST_CASCADE_ROUND_SRCS) Application/Src/CASCADE/app_cascade.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_CASCADE_ROUND_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_cascade_round: Application/Src/CASCADE/app_cascade.c \
	Application/Inc/CASCADE/app_cascade.h $(BOARD_DIR)/board.h

$(TEST_BUILD)/test_cascade_master: $(TEST_CASCADE_MASTER_SRCS) \
	Application/Src/app_screen.c Application/Src/CASCADE/app_cascade.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_CASCADE_MASTER_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_cascade_master: Application/Src/CASCADE/app_cascade.c \
	Application/Inc/CASCADE/app_cascade.h $(BOARD_DIR)/board.h

# 同理：用例 TU-include 了 app_rs485.c，它不在 SRCS 里，必须单独挂依赖。
$(TEST_BUILD)/test_rs485_slots: $(TEST_RS485_SLOTS_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_RS485_SLOTS_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_rs485_slots: Application/Src/Channel/app_rs485.c

# ---- Header Dependencies ----
# -MMD writes <obj>.d next to each object; -MP adds phony targets so deleting a
# header does not break the build. Without this, editing a header does not
# trigger recompilation and "it builds" refers to a stale binary.
-include $(OBJ_ALL:.o=.d)

$(TEST_BUILD)/test_iap_cfg: $(TEST_IAP_CFG_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_IAP_CFG_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_iap_cfg: Application/Src/IAP/app_iap_cfg.c

$(TEST_BUILD)/test_ldi_0ah: $(TEST_LDI_0AH_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_LDI_0AH_SRCS) $(TEST_LDFLAGS)

$(TEST_BUILD)/test_ldi_0ah: Application/Src/LDI/app_ldi_cfg.c Application/Src/IAP/app_iap_cfg.c \
                           Application/Src/LDI/app_ldi_cmd.c

$(TEST_BUILD)/test_isr_preinit: $(TEST_ISR_PREINIT_SRCS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_ISR_PREINIT_SRCS) $(TEST_LDFLAGS)
