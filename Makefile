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
BUILD_DIR  = build/$(CONFIG)

# ---- MCU Flags ----
CPU       = -mcpu=cortex-m4
FPU       = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU_FLAGS = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ---- Common Flags ----
DEFINES = -DUSE_HAL_DRIVER -DSTM32F407xx

INC_DIRS = \
	-I Application/Inc \
	-I Application/Inc/IAP \
	-I Application/Inc/LDI \
	-I Application/Inc/AH_MQTT \
	-I Application/Inc/Channel \
	-I Device/Inc \
	-I Platform/Inc \
	-I Kernel/Inc \
	-I Core/Inc \
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
LDSCRIPT  = Compiler/STM32F407XX_FLASH.ld
LDFLAGS  = $(MCU_FLAGS)
LDFLAGS += -T $(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/Project_STD.map,--cref
LDFLAGS += -Wl,--gc-sections
LDFLAGS += $(SPECS)
LDFLAGS += -u _printf_float
LDFLAGS += -lm

# ---- Source Files ----
# Core/Application
SRC_CORE = \
	Core/Src/main.c \
	Core/Src/stm32f4xx_it.c \
	Core/Src/syscalls.c \
	Core/Src/sysmem.c \
	Core/Src/adc.c \
	Core/Src/dma.c \
	Core/Src/gpio.c \
	Core/Src/iwdg.c \
	Core/Src/rtc.c \
	Core/Src/spi.c \
	Core/Src/stm32f4xx_hal_msp.c \
	Core/Src/stm32f4xx_hal_timebase_tim.c \
	Core/Src/system_stm32f4xx.c \
	Core/Src/tim.c \
	Core/Src/usart.c \
	Core/Src/crc.c \

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
	Kernel/Src/text_cvt.c

# Platform（仅含无冲突的文件，其他在 Phase 3 逐步加入）
SRC_PLATFORM = \
	Platform/Src/pl_gpio.c \
	Platform/Src/pl_rtt.c \
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
	Platform/Src/pl_hub75.c \
	Platform/Src/pl_adc.c \
	Platform/Src/pl_spi.c \
	Platform/Src/pl_uart.c

# Device (仅 Project_STD 新模块，resend dev_* 等 Phase 6 Platform 集成后加入)
SRC_DEVICE = \
	Device/IO/dev_io_ctrl.c \
	Device/IO/dev_key.c \
	Device/Display/dev_display.c \
	Device/Display/dev_p20_16x8_2200001667.c \
	Device/IO/dev_light_sensor.c \
	Device/Storage/dev_w25qxx.c \
	Device/Storage/dev_flash_int.c \
	Device/Network/dev_dp83848.c \
	Device/Network/dev_eth.c \
	Device/Comm/dev_rs485.c \
	Device/Comm/dev_rs232.c

# Application (Project_STD 新模块，resend app_* 等 Phase 7 集成后加入)
SRC_APPLICATION = \
	Application/Src/app_test.c \
	Application/Src/app_factory_test.c \
	Application/Src/app_boot.c \
	Application/Src/app_dispatch.c \
	Application/Src/app_render.c \
	Application/Src/app_key.c \
	Application/Src/app_light_sensor.c \
	Application/Src/IAP/app_iap.c \
	Application/Src/IAP/app_iap_cmd.c \
	Application/Src/IAP/app_iap_cfg.c \
	Application/Src/LDI/app_ldi.c \
	Application/Src/LDI/app_ldi_cmd.c \
	Application/Src/LDI/app_ldi_cfg.c \
	Application/Src/LDI/app_vms_ctrl.c \
	Application/Src/AH_MQTT/ah_mqtt.c \
	Application/Src/AH_MQTT/ah_mqtt_cmd.c \
	Application/Src/Channel/app_udp.c \
	Application/Src/Channel/app_tcp_server.c \
	Application/Src/Channel/app_tcp_client.c \
	Application/Src/Channel/app_mqtt.c \
	Application/Src/Channel/app_rs232.c \
	Application/Src/Channel/app_rs485.c

# ---- All Sources ----
SRC_ALL = \
	$(SRC_KERNEL) \
	$(SRC_PLATFORM) \
	$(SRC_DEVICE) \
	$(SRC_APPLICATION) \
	$(SRC_CORE) \
	$(SRC_HAL) \
	$(SRC_RTT) \
	$(SRC_LWIP) \
	$(SRC_FREERTOS) \
	$(SRC_STARTUP)

# ---- Object Files ----
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
	rm -rf $(BUILD_DIR)

# ---- Host Unit Tests ----
# 用 test/stubs 下的 cmsis_os2 替身，让生产源码（不替换、不改写）原样在 host 上编译。
# ASan/UBSan 常开：缓冲区越界、未对齐访问这类缺陷在硬件上极难构造，在这里是必现的。
HOSTCC      = cc
TEST_CFLAGS = -std=gnu23 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
              -fno-omit-frame-pointer -I Kernel/Inc -I test/stubs
TEST_BUILD  = build/test

test: $(TEST_BUILD)/test_ring_buffer
	@echo "---- run test_ring_buffer ----"
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BUILD)/test_ring_buffer

$(TEST_BUILD)/test_ring_buffer: Kernel/Src/ring_buffer.c test/test_ring_buffer.c test/stubs/os_stub.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $^

# ---- Header Dependencies ----
# -MMD writes <obj>.d next to each object; -MP adds phony targets so deleting a
# header does not break the build. Without this, editing a header does not
# trigger recompilation and "it builds" refers to a stale binary.
-include $(OBJ_ALL:.o=.d)
