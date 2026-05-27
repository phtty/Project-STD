# Project_STD Makefile
# 参照 .eide/eide.yml Debug 目标配置生成

# ---- Toolchain ----
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# ---- Directories ----
BUILD_DIR = build

# ---- MCU Flags ----
CPU       = -mcpu=cortex-m4
FPU       = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU_FLAGS = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ---- Common Flags ----
DEFINES = -DUSE_HAL_DRIVER -DSTM32F407xx

INC_DIRS = \
	-I Application/Inc \
	-I Device/Inc \
	-I Platform/Inc \
	-I Kernel/Inc \
	-I Core/Inc \
	-I Drivers/CMSIS/Include \
	-I Drivers/CMSIS/Device/ST/STM32F4xx/Include \
	-I Drivers/STM32F4xx_HAL_Driver/Inc \
	-I Drivers/BSP/Components/dp83848 \
	-I Drivers/BSP/Components/HUB75 \
	-I Drivers/BSP/Components/W25Qxx \
	-I Drivers/BSP/Components/Key \
	-I Drivers/BSP/Components/LightSensor \
	-I Drivers/BSP/Components/IOCtrl \
	-I Drivers/BSP/Display \
	-I Drivers/BSP/Display/text_cvt \
	-I Drivers/BSP/Protocol \
	-I Drivers/BSP/Protocol/IAP \
	-I Drivers/BSP/Protocol/AH_MQTT \
	-I Drivers/BSP/RingBuff \
	-I Drivers/RTT/Config \
	-I Drivers/RTT/RTT \
	-I LWIP/App \
	-I LWIP/Target \
	-I Middlewares/Third_Party/LwIP/src/include \
	-I Middlewares/Third_Party/LwIP/system \
	-I Middlewares/Third_Party/FreeRTOS/Source/include \
	-I Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
	-I Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
	-I Compiler

# ---- C Flags ----
CFLAGS  = $(MCU_FLAGS) $(DEFINES) $(INC_DIRS)
CFLAGS += -std=gnu23
CFLAGS += -Og -g
CFLAGS += -Wall -Wextra
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common
CFLAGS += -fno-exceptions
CFLAGS += --specs=nano.specs

# ---- LDFLAGS ----
LDSCRIPT  = Compiler/STM32F407XX_FLASH.ld
LDFLAGS  = $(MCU_FLAGS)
LDFLAGS += -T $(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/Project_STD.map,--cref
LDFLAGS += -Wl,--gc-sections
LDFLAGS += --specs=nano.specs --specs=nosys.specs
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
	Core/Src/freertos.c

# LWIP
SRC_LWIP = \
	LWIP/App/lwip.c \
	LWIP/Target/ethernetif.c \
	LWIP/App/mqtt_app.c \
	LWIP/App/tcp_client_app.c \
	LWIP/App/tcp_server_app.c \
	LWIP/App/udp_app.c

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

# BSP Components
SRC_BSP = \
	Drivers/BSP/Components/dp83848/dp83848.c \
	Drivers/BSP/Components/W25Qxx/w25qxx.c \
	Drivers/BSP/Components/LightSensor/light.c \
	Drivers/BSP/Components/IOCtrl/IOCtrl.c \
# BSP Display
SRC_DISPLAY = \
	Drivers/BSP/Display/display.c \
	Drivers/BSP/Display/render.c \
	Drivers/BSP/Display/text_cvt/text_cvt.c

# BSP Protocol
SRC_PROTOCOL = \
	Drivers/BSP/Protocol/protocol.c \
	Drivers/BSP/Protocol/IAP/config_info.c \
	Drivers/BSP/Protocol/IAP/iap.c \
	Drivers/BSP/Protocol/IAP/iap_cmd.c \
	Drivers/BSP/Protocol/AH_MQTT/ah_mqtt.c \
	Drivers/BSP/Protocol/AH_MQTT/ah_mqtt_cmd.c

# BSP RingBuff
SRC_RINGBUF = \
	Drivers/BSP/RingBuff/RingBuff.c

# RTT
SRC_RTT = \
	Drivers/RTT/RTT/SEGGER_RTT.c \
	Drivers/RTT/RTT/SEGGER_RTT_printf.c \
	Drivers/RTT/RTT/SEGGER_RTT_Syscalls_GCC.c

# LwIP Middleware
SRC_LWIP_MW = \
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
	Kernel/Src/crc_utils.c

# Platform（仅含无冲突的文件，其他在 Phase 3 逐步加入）
SRC_PLATFORM = \
	Platform/Src/pl_gpio.c \
	Platform/Src/pl_crc.c \
	Platform/Src/pl_iwdg.c \
	Platform/Src/pl_dma.c \
	Platform/Src/pl_dwt.c \
	Platform/Src/pl_tim.c \
	Platform/Src/pl_rtc.c \
	Platform/Src/pl_hub75.c \
	Platform/Src/pl_adc.c \
	Platform/Src/pl_spi.c \
	Platform/Src/pl_uart.c

# Device (仅 Project_STD 新模块，resend dev_* 等 Phase 6 Platform 集成后加入)
SRC_DEVICE = \
	Device/Src/dev_io_ctrl.c \
	Device/Src/dev_key.c \
	Device/Src/dev_display.c \
	Device/Src/dev_light_sensor.c \
	Device/Src/dev_flash_font.c \
	Device/Src/dev_uart_channel.c \
	Device/Src/dev_rs485.c \
	Device/Src/dev_rs232.c

# Application (Project_STD 新模块，resend app_* 等 Phase 7 集成后加入)
SRC_APPLICATION = \
	Application/Src/app_boot.c \
	Application/Src/app_render.c \
	Application/Src/app_light_sensor.c

# ---- All Sources ----
SRC_ALL = \
	$(SRC_KERNEL) \
	$(SRC_PLATFORM) \
	$(SRC_DEVICE) \
	$(SRC_APPLICATION) \
	$(SRC_CORE) \
	$(SRC_LWIP) \
	$(SRC_HAL) \
	$(SRC_BSP) \
	$(SRC_DISPLAY) \
	$(SRC_PROTOCOL) \
	$(SRC_RINGBUF) \
	$(SRC_RTT) \
	$(SRC_LWIP_MW) \
	$(SRC_FREERTOS) \
	$(SRC_STARTUP)

# ---- Object Files ----
OBJ_ALL = $(addprefix $(BUILD_DIR)/,$(SRC_ALL:.c=.o))

# ---- Targets ----
.PHONY: all clean

all: $(BUILD_DIR)/Project_STD.elf $(BUILD_DIR)/Project_STD.hex $(BUILD_DIR)/Project_STD.bin
	@echo "==== Build complete ===="
	@$(SIZE) $(BUILD_DIR)/Project_STD.elf

$(BUILD_DIR)/Project_STD.elf: $(OBJ_ALL)
	@echo "Linking $@"
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

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
