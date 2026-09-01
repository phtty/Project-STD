/**
 * @file    app_iap_cmd.c
 * @brief   IAP 协议命令处理（8 条命令：测试、IP、固件、升级、恢复、重启）
 */

#include "app_iap_cmd.h"
#include "pl_crc.h"
#include "pl_rtc.h"
#include "pl_iwdg.h"
#include "pl_sys.h"
#include "app_udp.h"
#include "app_board_net_cfg.h"
#include <string.h>

#define U8_LEN(x)  ((x) * sizeof(uint32_t))
#define U32_LEN(y) ((y) / sizeof(uint32_t))

static void cmd_Test_00(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_ReportIp_01(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_ForceModifyIP_02(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_ReportFirmwareStatus_03(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_PrepareUpgrade_04(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_SendUpgradePackage_05(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_EnterRecoveryMode_06(channel_t *ch, iap_frame_t *IAP_Data);
static void cmd_Restart_07(channel_t *ch, iap_frame_t *IAP_Data);

/* ================================================================
 *  命令表（按 cmd 编号索引）
 * ================================================================ */
const iap_cmd_handler_fn_t g_iap_cmd_table[IAP_CMD_COUNT] = {
    cmd_Test_00,
    cmd_ReportIp_01,
    cmd_ForceModifyIP_02,
    cmd_ReportFirmwareStatus_03,
    cmd_PrepareUpgrade_04,
    cmd_SendUpgradePackage_05,
    cmd_EnterRecoveryMode_06,
    cmd_Restart_07,
};

/* 与 app_iap_cmd.h 的 IAP_CMD_COUNT 保持同步，防增删命令时查表越界 */
static_assert(sizeof(g_iap_cmd_table) / sizeof(g_iap_cmd_table[0]) == IAP_CMD_COUNT,
              "IAP 命令表条目数与 IAP_CMD_COUNT 不同步");

/** @brief 构造 IAP 响应帧并发送 */
/* ================================================================
 *  命令实现
 * ================================================================ */
static void cmd_SendReData(channel_t *ch, uint32_t ReSeq, uint32_t ReCmd, uint32_t ReLen, uint32_t *ReData)
{
    static uint32_t ReBuff[FRAME_MAX_LEN] = {0};

    iap_frame_t *pIAP_ReTmp = (iap_frame_t *)&(ReBuff);
    pIAP_ReTmp->head        = 0x5A5A5A5A;
    pIAP_ReTmp->seq         = ReSeq;
    pIAP_ReTmp->cmd         = ReCmd;
    pIAP_ReTmp->len         = ReLen;

    if (ReLen != 0)
        memcpy(pIAP_ReTmp->data_crc, ReData, U8_LEN(ReLen));

    pIAP_ReTmp->data_crc[ReLen] = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)pIAP_ReTmp, sizeof(iap_frame_t) + U8_LEN(ReLen));

    /* cmd01/cmd02 reply via broadcast */
    if (ReCmd == rtn_cmd01 || ReCmd == rtn_cmd02) {
        udp_channel_t *udp = container_of(ch, udp_channel_t, me);
        memset(udp->src_ip, 0xFF, 4); /* 255.255.255.255 广播 */
    }

    channel_send(ch, (uint8_t *)pIAP_ReTmp, sizeof(iap_frame_t) + U8_LEN(ReLen) + sizeof(uint32_t));
}

/* ---- Command handlers (0x00 ~ 0x07) ---- */

/** @brief 0x00: Test (no-op) */
static void cmd_Test_00(channel_t *ch, iap_frame_t *IAP_Data)
{
    (void)ch;
    (void)IAP_Data;
}

/** @brief 0x01: Report current IP config */
static void cmd_ReportIp_01(channel_t *ch, iap_frame_t *IAP_Data)
{
    app_board_sys_info_t config_info;
    app_board_net_cfg_read(&config_info);

    uint32_t ReData[4] = {0};
    ReData[0]          = config_info.net_cfg.ip[0] << 24 | config_info.net_cfg.ip[1] << 16 | config_info.net_cfg.ip[2] << 8 | config_info.net_cfg.ip[3];
    ReData[1]          = config_info.net_cfg.mask[0] << 24 | config_info.net_cfg.mask[1] << 16 | config_info.net_cfg.mask[2] << 8 | config_info.net_cfg.mask[3];
    ReData[2]          = config_info.net_cfg.gw[0] << 24 | config_info.net_cfg.gw[1] << 16 | config_info.net_cfg.gw[2] << 8 | config_info.net_cfg.gw[3];
    ReData[3]          = config_info.net_cfg.port;

    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd01, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x02: Force modify IP and write to Flash */
static void cmd_ForceModifyIP_02(channel_t *ch, iap_frame_t *IAP_Data)
{
    uint32_t TmpData[4] = {0};
    memcpy(TmpData, IAP_Data->data_crc, sizeof(TmpData));

    app_board_net_cfg_t net_info = {0};
    net_info.ip[0]       = (uint8_t)(TmpData[0] >> 24);
    net_info.ip[1]       = (uint8_t)(TmpData[0] >> 16);
    net_info.ip[2]       = (uint8_t)(TmpData[0] >> 8);
    net_info.ip[3]       = (uint8_t)(TmpData[0]);
    net_info.mask[0]     = (uint8_t)(TmpData[1] >> 24);
    net_info.mask[1]     = (uint8_t)(TmpData[1] >> 16);
    net_info.mask[2]     = (uint8_t)(TmpData[1] >> 8);
    net_info.mask[3]     = (uint8_t)(TmpData[1]);
    net_info.gw[0]       = (uint8_t)(TmpData[2] >> 24);
    net_info.gw[1]       = (uint8_t)(TmpData[2] >> 16);
    net_info.gw[2]       = (uint8_t)(TmpData[2] >> 8);
    net_info.gw[3]       = (uint8_t)(TmpData[2]);
    net_info.port        = TmpData[3];

    /* 写入路径（见 app_board_net_cfg_update）：空/损坏扇区完整初始化，升级中间态仅放行
     * net_cfg 更新（update_sta/app_info 保留），net_cfg 同值跳过擦写。所有改 IP 接口
     * （LDI 0AH / IAP 4B02 / Recovery IAP）统一
     * **重启生效**——此处仅持久化到 Sector1，不即时改 netif；上电由 Bootloader /
     * Recovery MX_LWIP_Init / 主固件 ldi_ctx_init 从 Sector1 读取生效。
     *
     * 协议扩展（方案 2，2026-08-14）：应答帧由无载荷改为 1 word 结果码，
     * 参照 4B04「准备升级」应答 0/1 先例——0x00000000 成功（含同值跳过）、
     * 0x00000001 失败（擦写错误）。
     * 兼容性风险：老上位机若按「B402 无载荷」解析，会多读到一个 word
     * （被忽略还是报错取决于上位机实现），混合部署期需联调验证。 */
    /* 方案 B（2026-08-21）：4B02 写 TCP 业务口（包内 port），udp_port 保留现值
     * （get 失败用出厂默认 20103）。 */
    app_board_net_cfg_t cur_cfg;
    uint32_t udp_port = (app_board_net_cfg_get(&cur_cfg) == 0) ? cur_cfg.udp_port : 20103U;
    int32_t ret = app_board_net_cfg_update(net_info.ip, net_info.mask, net_info.gw, net_info.port,
                                           udp_port);
    uint32_t result = ret < 0 ? 1 : 0;
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd02, 1, &result);
}

/** @brief 0x03: Report firmware version, size, CRC32, update status */
static void cmd_ReportFirmwareStatus_03(channel_t *ch, iap_frame_t *IAP_Data)
{
    app_board_sys_info_t config_info;
    app_board_net_cfg_read(&config_info);

    uint32_t ReData[11] = {0};
    ReData[0]           = config_info.app_info.size;
    ReData[1]           = config_info.app_info.crc32;
    /* version 是 ASCII 字符串：按上位机 word 显示约定（大端字节序）逐 word 构造，
     * 与 0x01 的 IP 构造同构；不能 memcpy（否则上位机按 4 字节一组反转显示） */
    const char *ver = config_info.app_info.version;
    for (uint32_t i = 0; i < sizeof(config_info.app_info.version) / sizeof(uint32_t); i++) {
        ReData[2 + i] = (uint32_t)(uint8_t)ver[4 * i] << 24 |
                        (uint32_t)(uint8_t)ver[4 * i + 1] << 16 |
                        (uint32_t)(uint8_t)ver[4 * i + 2] << 8 |
                        (uint32_t)(uint8_t)ver[4 * i + 3];
    }
    ReData[10] = config_info.update_sta;

    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd03, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x04: Prepare for firmware upgrade (main app responsibility) */
static void cmd_PrepareUpgrade_04(channel_t *ch, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd04, 0, NULL);
}

/** @brief 0x05: Send upgrade package (main app responsibility) */
static void cmd_SendUpgradePackage_05(channel_t *ch, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd05, 0, NULL);
}

/** @brief 0x06: Enter recovery mode (set flag in RTC backup register then reboot) */
static void cmd_EnterRecoveryMode_06(channel_t *ch, iap_frame_t *IAP_Data)
{
    pl_rtc_bkup_write(pl_rtc_get_handle(), 0 /* RTC 备份寄存器 0 */, FLAG_FORCE_UPDATE);
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd06, 0, NULL);
}

/** @brief 0x07: Soft reset */
static void cmd_Restart_07(channel_t *ch, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd07, 0, NULL);
    pl_iwdg_refresh(pl_iwdg_get_handle());
    pl_system_reset();
}
