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
#include "pl_net_adapt.h"

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
const iap_cmd_handler_fn_t g_iap_cmd_table[] = {
    cmd_Test_00,
    cmd_ReportIp_01,
    cmd_ForceModifyIP_02,
    cmd_ReportFirmwareStatus_03,
    cmd_PrepareUpgrade_04,
    cmd_SendUpgradePackage_05,
    cmd_EnterRecoveryMode_06,
    cmd_Restart_07,
};

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
        udp_channel_t *udp = container_of(ch, udp_channel_t, ch);
        memset(udp->src_ip, 0xFF, 4); /* 255.255.255.255 广播 */
    }

    channel_send(ch, (uint8_t *)pIAP_ReTmp, sizeof(iap_frame_t) + U8_LEN(ReLen) + sizeof(uint32_t));
}

typedef struct {
    ip4_addr_t ip;
    ip4_addr_t mask;
    ip4_addr_t gw;
    uint16_t port;
} iap_ipconfig_t;

/** @brief TCP/IP 线程回调：应用新 IP 配置到 netif */
static void iap_update_ip(void *ctx)
{
    iap_ipconfig_t *config = (iap_ipconfig_t *)ctx;
    netif_set_addr(netif_default, &config->ip, &config->mask, &config->gw);
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
    dev_flash_iap_sys_info_t config_info = *((dev_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[4] = {0};
    ReData[0]          = config_info.net_cfg.ip[0] << 24 | config_info.net_cfg.ip[1] << 16 | config_info.net_cfg.ip[2] << 8 | config_info.net_cfg.ip[3];
    ReData[1]          = config_info.net_cfg.mask[0] << 24 | config_info.net_cfg.mask[1] << 16 | config_info.net_cfg.mask[2] << 8 | config_info.net_cfg.mask[3];
    ReData[2]          = config_info.net_cfg.gw[0] << 24 | config_info.net_cfg.gw[1] << 16 | config_info.net_cfg.gw[2] << 8 | config_info.net_cfg.gw[3];
    ReData[3]          = config_info.net_cfg.port;

    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd01, U32_LEN(sizeof(ReData)), ReData);
}

static iap_ipconfig_t ipconfig = {0};

/** @brief 0x02: Force modify IP and write to Flash */
static void cmd_ForceModifyIP_02(channel_t *ch, iap_frame_t *IAP_Data)
{
    dev_flash_iap_sys_info_t config_info = *((dev_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    uint32_t TmpData[4] = {0};
    memcpy(TmpData, IAP_Data->data_crc, sizeof(TmpData));

    dev_flash_iap_net_cfg_t net_info = {0};
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

    config_info.net_cfg = net_info;
    IP4_ADDR(&ipconfig.ip, net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    IP4_ADDR(&ipconfig.mask, net_info.mask[0], net_info.mask[1], net_info.mask[2], net_info.mask[3]);
    IP4_ADDR(&ipconfig.gw, net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);
    tcpip_callback(iap_update_ip, &ipconfig);

    dev_flash_iap_edit_config(&config_info);
    cmd_SendReData(ch, IAP_Data->seq, rtn_cmd02, 0, NULL);
}

/** @brief 0x03: Report firmware version, size, CRC32, update status */
static void cmd_ReportFirmwareStatus_03(channel_t *ch, iap_frame_t *IAP_Data)
{
    dev_flash_iap_sys_info_t config_info = *((dev_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[11] = {0};
    ReData[0]           = config_info.app_info.size;
    ReData[1]           = config_info.app_info.crc32;
    memcpy(ReData + 2, config_info.app_info.version, sizeof(config_info.app_info.version));
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
