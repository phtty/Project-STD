# 3833024 的板级源清单（由顶层 Makefile 的 include $(BOARD_DIR)/board.mk 引入）
#
# 按层分组，与主树一致：Platform/ 下是给共享机制消费的板级数据表，Device/ 下是
# 板级设备实现（OCP 虚表），Application/ 下是板级通道启动与板级数据。
# CubeMX 产物在 Core/Src，由顶层 Makefile 的 SRC_CORE 收，不在这里。
#
# 显示模组只列一个：每个驱动自带一份 CCMRAM 帧缓冲（pixel_map + hub75_buff），
# 多编一份直接顶爆 CCMRAM（P10 32x16 一份就是 2×20KB）。同目录下的
# dev_p20_16x8_2200001667.c（128×32）/ dev_p20_16x16_1000001055.c /
# dev_p5_64x32_.c 是可替换的面板选项，换屏时换掉这一行，而不是追加。
#
# 本产品（2300001735）是重庆 P10 状态屏：320×64，与 CQ_P10 协议的位图约定
# 一致（整屏 2560B = 320/8 × 64，每包 320B = 8 行 × 40B）。
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
	$(BOARD_DIR)/Device/Src/dev_p10_32x16_2200001703.c \
	$(BOARD_DIR)/Application/Src/app_rs232.c \
	$(BOARD_DIR)/Application/Src/app_test_board.c \
	$(BOARD_DIR)/Application/Src/app_font_lib_board.c \
	$(BOARD_DIR)/Application/Src/app_fold_preset_board.c

# ---- 本产品（2300001735）不参与编译的共享源 ----
# 本产物是 CQ_P10 状态屏的直烧固件：无 IAP（升级链路整体去掉），也不跑
# LDI/RLS/CDSTD（它们与 CQ 争同一个 UDP 口/相同持久化区，且 LDI/CDSTD 还依赖
# IAP 的网配记录 API）。保留 render/screen/key/channel 与 CQ_P10。
#
# 与 EIDE 的 3833024 target excludeList 对齐（protocol/{iap,ldi,rls,cdstd}）：
# 两份清单不一致 = 同一份源码在两套构建下产出不同固件（Makefile 顶部那段的约定）。
BOARD_SRC_EXCLUDE = \
	Application/Src/IAP/app_iap.c \
	Application/Src/IAP/app_iap_cmd.c \
	Application/Src/IAP/app_iap_cfg.c \
	Application/Src/LDI/app_ldi.c \
	Application/Src/LDI/app_ldi_cmd.c \
	Application/Src/LDI/app_ldi_cfg.c \
	Application/Src/LDI/app_vms_ctrl.c \
	Application/Src/LDI/app_fold.c \
	Application/Src/RLS/app_rls.c \
	Application/Src/RLS/app_rls_cmd.c \
	Application/Src/CDSTD/app_cdstd.c \
	Application/Src/CDSTD/app_cdstd_cmd.c \
	Application/Src/CDSTD/app_cdstd_cfg.c
