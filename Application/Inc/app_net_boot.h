/**
 * @file    app_net_boot.h
 * @brief   网络配置启动应用（横切职责中立模块，两口径共用）
 *
 * 「把 Sector1 net_cfg 应用到 netif + TCP Server 口」的统一入口，
 * 由 app_boot.c init_task 调用（dev/LDI 与 PROTO_CHONGQING 两口径都编入）。
 */

#ifndef APP_NET_BOOT_H
#define APP_NET_BOOT_H

void app_net_boot_apply(void);

#endif /* APP_NET_BOOT_H */