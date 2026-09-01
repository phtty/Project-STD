/**
 * @file    app_net_boot.c
 * @brief   网络配置启动应用（横切职责中立模块，两口径共用）
 *
 * 职责：「把 Sector1 net_cfg 应用到 netif + TCP Server 口」统一收拢于此，
 * 与 LDI / CQ 协议模块解耦：
 *  - ldi_ctx_init（sw_board_init 内）只负责 Sector1/W25 自愈与 LDI 设备配置
 *    装载，不再调用 pl_net_set_ip / app_tcp_server_set_port；
 *  - TCP Server 口两口径统一应用 net_cfg.port（2026-08-24 修订）：TCP Server/
 *    Client 通道由 app_boot.c 无条件启动（无口径裁剪），故端口应用也不得按
 *    PROTO_CHONGQING 裁剪——否则「PROTO_CHONGQING 与 LDI 共存」的混合构建
 *    （如 EIDE Debug 口径错配）中 TCP 口恒为编译期默认 9528，与 LDI 12H /
 *    IAP 0x01 上报的 Sector1 net_cfg.port（0AH/4B02 已改值）矛盾；
 *  - CQ UDP 业务口由 _udp_cq_read_port 直接读 Sector1 net_cfg.udp_port
 *    （方案 B，2026-08-21：CQ setip 只改 udp_port，TCP 口 port 不受污染——
 *    setip 端口语义不因本修订改变）。
 *
 * accept_write（两口径统一）：Sector1 空/损坏/非法（IP 全 0 或 port 不在
 * 1~65535）→ 写本构建默认记录落盘并应用：
 *  - PROTO_CHONGQING：192.168.1.5 / 255.255.255.0 / 192.168.1.1 / port 9528 /
 *    udp_port 20103；
 *  - 其余（dev/LDI）：192.168.114.200 / 255.255.255.0 / 192.168.114.1 / port 9528 /
 *    udp_port 20103。
 *
 * 顺序约束：ldi_module_init（含 ldi_ctx_init 的 W25 镜像自愈与 Sector1 回写）
 * 先于本调用执行；本函数由 app_boot.c init_task 在
 * app_board_net_cfg_fw_version_update 之后调用——fw_version_update 对空/损坏
 * 扇区初始化时 net_cfg 置 0，本函数随后判无效写默认（accept_write）。
 */
#include <stdbool.h>
#include <stdint.h>

#include "app_board_net_cfg.h"
#include "app_tcp_server.h"
#include "pl_net.h"

void app_net_boot_apply(void)
{
    app_board_net_cfg_t cfg;
    bool valid = (app_board_net_cfg_get(&cfg) == 0) &&
                 (cfg.ip[0] | cfg.ip[1] | cfg.ip[2] | cfg.ip[3]) != 0 &&
                 cfg.port > 0 && cfg.port <= 65535;

    if (!valid) {
        /* accept_write：Sector1 空/损坏/非法 → 写本构建默认记录落盘并应用。
         * 方案 B（2026-08-21）：port（TCP 业务口）两口径默认均 9528；udp_port
         * （CQ UDP 业务口）默认 20103——两字段完整落盘保持记录自洽。 */
#ifdef PROTO_CHONGQING
        static const uint8_t d_ip[4]   = {192, 168, 1, 5};
        static const uint8_t d_mask[4] = {255, 255, 255, 0};
        static const uint8_t d_gw[4]   = {192, 168, 1, 1};
#else
        static const uint8_t d_ip[4]   = {192, 168, 114, 200};
        static const uint8_t d_mask[4] = {255, 255, 255, 0};
        static const uint8_t d_gw[4]   = {192, 168, 114, 1};
#endif
        const uint32_t d_port     = 9528;  /* TCP 业务口默认（两口径统一） */
        const uint32_t d_udp_port = 20103; /* CQ UDP 业务口默认 */
        (void)app_board_net_cfg_update(d_ip, d_mask, d_gw, d_port, d_udp_port);
        pl_net_set_ip(d_ip, d_mask, d_gw);
        app_tcp_server_set_port((uint16_t)d_port);
        return;
    }

    /* Sector1 有效：应用 netif + TCP Server 口（两口径统一，2026-08-24 修订：
     * TCP 口恒与 LDI 12H / IAP 0x01 上报的 net_cfg.port 一致；CQ UDP 业务口
     * 由 _udp_cq_read_port 读 cfg.udp_port，与本处无涉） */
    pl_net_set_ip(cfg.ip, cfg.mask, cfg.gw);
    app_tcp_server_set_port((uint16_t)cfg.port);
}