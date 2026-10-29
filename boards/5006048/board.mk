# 5006048 的板级源清单（由顶层 Makefile 的 include $(BOARD_DIR)/board.mk 引入）
#
# 按层分组，与主树一致：Platform/ 下是给共享机制消费的板级数据表，Device/ 下是
# 板级设备实现（OCP 虚表），Application/ 下是板级通道启动与板级数据。
# CubeMX 产物在 Core/Src，由顶层 Makefile 的 SRC_CORE 收，不在这里。
#
# 与 3833024 的差异：没有 RS232（两路都没有）、没有 dev_io_ctrl（车道灯/黄闪灯）、
# 显示模组是 P10 112×10 而非 P20 16×8。
SRC_BOARD = \
	$(BOARD_DIR)/Platform/Src/pl_tim_board.c \
	$(BOARD_DIR)/Platform/Src/pl_uart_board.c \
	$(BOARD_DIR)/Platform/Src/pl_exti_board.c \
	$(BOARD_DIR)/Platform/Src/pl_eth_board.c \
	$(BOARD_DIR)/Platform/Src/pl_hub75_board.c \
	$(BOARD_DIR)/Device/Src/dev_rs485.c \
	$(BOARD_DIR)/Device/Src/dev_key_board.c \
	$(BOARD_DIR)/Device/Src/dev_p10_112x10_1000000983.c \
	$(BOARD_DIR)/Application/Src/app_font_lib_board.c \
	$(BOARD_DIR)/Application/Src/app_fold_preset_board.c

# ---- 本板不参与编译的共享源 ----
# 与 EIDE 的 5006048 target excludeList 对齐（<virtual_root>/Application/protocol/{cdstd,rls}）：
# 两份清单不一致 = 同一份源码在两套构建下产出不同固件（Makefile 顶部那段的约定）。
# 本板走 LDI（VMS）协议；CDSTD 是另一套折叠屏协议，随整目录排除。
BOARD_SRC_EXCLUDE = \
	Application/Src/CDSTD/app_cdstd.c \
	Application/Src/CDSTD/app_cdstd_cmd.c \
	Application/Src/CDSTD/app_cdstd_cfg.c \
	Application/Src/RLS/app_rls.c \
	Application/Src/RLS/app_rls_cmd.c
