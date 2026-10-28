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

# ---- 本板不参与编译的共享源：**本产品只跑 LCSP** ----
#
# 产品编码就是分支名后半截（`produc/2300001860`）：车道诱导屏，上位机走 LCSP
# （以太网）。一台设备只跑一种上位机协议，所以其余三个协议整目录不进固件：
#
#   CDSTD  折叠屏创迪标准 —— 创迪平台的折叠屏产品才用
#   LDI    车道诱导屏协议 —— 老的 VMS 平台，本产品不上
#   RLS    车道屏远程上载/显示帧 —— 同上
#
# EIDE 的 3833024 目标一直是这么配的（`Application/protocol/{cdstd,ldi,rls}` 全排），
# Makefile 这边以前只漏排了 CDSTD —— 两套构建因此产出**不同固件**（AGENTS.md §6 坑 1）。
# 这里补齐，让两边编同一份源；省下的不只是 Flash，还有这三个协议各自的静态队列与暂存
# （CCMRAM 本来就紧）。
#
# 两处**故意保留**：
#   · `app_fold_preset_board.c`（SRC_BOARD 里）—— EIDE 也列着它。它只是 3 个空槽常量，
#     几十字节；以后若加回折叠屏，不用再补这个符号。
#   · `Application/Inc/LDI` 等头文件路径（INC_DIRS 里）—— 头不占固件，且板级文件仍
#     include 它们的类型（如 app_fold.h 的 app_fold_preset_t）。
BOARD_SRC_EXCLUDE = \
	Application/Src/CDSTD/app_cdstd.c \
	Application/Src/CDSTD/app_cdstd_cmd.c \
	Application/Src/CDSTD/app_cdstd_cfg.c \
	Application/Src/LDI/app_ldi.c \
	Application/Src/LDI/app_ldi_cmd.c \
	Application/Src/LDI/app_ldi_cfg.c \
	Application/Src/LDI/app_vms_ctrl.c \
	Application/Src/LDI/app_fold.c \
	Application/Src/RLS/app_rls.c \
	Application/Src/RLS/app_rls_cmd.c
