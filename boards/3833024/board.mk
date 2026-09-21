# 3833024 的板级源清单（由顶层 Makefile 的 include $(BOARD_DIR)/board.mk 引入）
#
# 按层分组，与主树一致：Platform/ 下是给共享机制消费的板级数据表，Device/ 下是
# 板级设备实现（OCP 虚表），Application/ 下是板级通道启动与板级数据。
# CubeMX 产物在 Core/Src，由顶层 Makefile 的 SRC_CORE 收，不在这里。
#
# 显示模组只列一个：每个驱动自带一份 CCMRAM 帧缓冲（pixel_map + hub75_buff），
# 多编一份直接把 CCMRAM 顶爆（实测多两份超 588B）。同目录下的
# dev_P10_32x16_2200001703.c / dev_p20_16x16_1000001055.c 是可替换的面板选项，
# 换屏时换掉这一行，而不是追加。
SRC_BOARD = \
	$(BOARD_DIR)/Platform/Src/pl_tim_board.c \
	$(BOARD_DIR)/Platform/Src/pl_uart_board.c \
	$(BOARD_DIR)/Platform/Src/pl_exti_board.c \
	$(BOARD_DIR)/Platform/Src/pl_eth_board.c \
	$(BOARD_DIR)/Platform/Src/pl_hub75_board.c \
	$(BOARD_DIR)/Device/Src/dev_rs232.c \
	$(BOARD_DIR)/Device/Src/dev_rs485.c \
	$(BOARD_DIR)/Device/Src/dev_io_ctrl.c \
	$(BOARD_DIR)/Device/Src/dev_key_board.c \
	$(BOARD_DIR)/Device/Src/dev_p20_16x8_2200001667.c \
	$(BOARD_DIR)/Application/Src/app_rs232.c \
	$(BOARD_DIR)/Application/Src/app_test_board.c \
	$(BOARD_DIR)/Application/Src/font_lib_board.c
