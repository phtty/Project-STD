# std_b 的板级源清单（由顶层 Makefile 的 include $(BOARD_DIR)/board.mk 引入）
#
# 与 std_a 的差异：没有 RS232（两路都没有）、没有 dev_io_ctrl（车道灯/黄闪灯）、
# 显示模组是 P10 112×10 而非 P20 16×8。
SRC_BOARD = \
	$(BOARD_DIR)/Src/dev_rs485.c \
	$(BOARD_DIR)/Src/dev_key_board.c \
	$(BOARD_DIR)/Src/dev_p10_112x10_1000000661.c \
	$(BOARD_DIR)/Src/pl_tim_board.c \
	$(BOARD_DIR)/Src/pl_uart_board.c \
	$(BOARD_DIR)/Src/pl_exti_board.c \
	$(BOARD_DIR)/Src/pl_hub75_board.c
