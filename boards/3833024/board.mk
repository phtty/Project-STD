# 3833024 的板级源清单（由顶层 Makefile 的 include $(BOARD_DIR)/board.mk 引入）
#
# 按层分组，与主树一致：Platform/ 下是给共享机制消费的板级数据表，Device/ 下是
# 板级设备实现（OCP 虚表），Application/ 下是板级通道启动与板级数据。
# CubeMX 产物在 Core/Src，由顶层 Makefile 的 SRC_CORE 收，不在这里。
#
# 显示模组只列一个：每个驱动自带一份 CCMRAM 帧缓冲（pixel_map + hub75_buff），
# 多编一份直接把 CCMRAM 顶爆（实测多两份超 588B）。同目录下的
# dev_p10_32x16_2200001703.c / dev_p20_16x16_1000001055.c 是可替换的面板选项，
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
	$(BOARD_DIR)/Device/Src/dev_p10_48x24_2200000581.c \
	$(BOARD_DIR)/Application/Src/app_rs232.c \
	$(BOARD_DIR)/Application/Src/app_screen_board.c \
	$(BOARD_DIR)/Application/Src/app_test_board.c \
	$(BOARD_DIR)/Application/Src/app_font_lib_board.c \
	$(BOARD_DIR)/Application/Src/app_fold_preset_board.c

# ---- 本板不参与编译的共享源 ----
# CDSTD（折叠屏创迪标准协议）是**折叠屏产品**的协议，本板没有折叠屏，且产品口径是
# 一台设备只跑一种上位机协议（LCSP 与 CDSTD/RLS 不同时启用）。EIDE 的 3833024 目标
# 早就把 `<virtual_root>/Application/protocol/cdstd` 排掉了，Makefile 这边一直漏着 ——
# 两套构建因此产出**不同固件**（AGENTS.md §6 坑 1）。这里对齐，顺带把 CDSTD 占的
# 5.8KB CCMRAM 还给整屏图像路径。
BOARD_SRC_EXCLUDE = \
	Application/Src/CDSTD/app_cdstd.c \
	Application/Src/CDSTD/app_cdstd_cmd.c \
	Application/Src/CDSTD/app_cdstd_cfg.c
