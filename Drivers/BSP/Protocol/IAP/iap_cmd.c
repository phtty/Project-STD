#include "iap_cmd.h"

#include "rtc.h"
#include "crc.h"
#include "iwdg.h"

#define U8_LEN(x)  ((x) * sizeof(uint32_t))
#define U32_LEN(y) ((y) / sizeof(uint32_t))

static void cmd_Test_00(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_ReportIp_01(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_ForceModifyIP_02(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_ReportFirmwareStatus_03(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_PrepareUpgrade_04(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_SendUpgradePackage_05(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_EnterRecoveryMode_06(ch_meta_t *meta, iap_frame_t *IAP_Data);
static void cmd_Restart_07(ch_meta_t *meta, iap_frame_t *IAP_Data);

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

/**
 * @brief  内部函数，构造返回数据包并发送
 *
 * @param  ReSeq: 帧序号
 * @param  ReCmd: 返回命令
 * @param  ReLen: 数据长度(单位: uint32_t)
 * @param  ReData: 数据
 */
static void cmd_SendReData(ch_meta_t *meta, uint32_t ReSeq, uint32_t ReCmd, uint32_t ReLen, uint32_t *ReData)
{
    static uint32_t ReBuff[FRAME_MAX_LEN] = {0};

    iap_frame_t *pIAP_ReTmp = (iap_frame_t *)&(ReBuff);
    pIAP_ReTmp->head        = 0x5A5A5A5A;
    pIAP_ReTmp->seq         = ReSeq;
    pIAP_ReTmp->cmd         = ReCmd;
    pIAP_ReTmp->len         = ReLen;

    if (ReLen != 0) // 数据载荷
        memcpy(pIAP_ReTmp->data_crc, ReData, U8_LEN(ReLen));

    // crc校验值
    pIAP_ReTmp->data_crc[ReLen] = HAL_CRC_Calculate(&hcrc, (uint32_t *)pIAP_ReTmp, U32_LEN((sizeof(iap_frame_t) + U8_LEN(ReLen))));

    // 命令1和2使用广播发送
    if (ReCmd == rtn_cmd01 || ReCmd == rtn_cmd02)
        ipaddr_aton("255.255.255.255", &(meta->handle.udp.src_ip));

    channel_send(meta, (uint8_t *)pIAP_ReTmp, sizeof(iap_frame_t) + U8_LEN(ReLen) + sizeof(uint32_t));
}

/**
 * @brief 在tcpip线程执行的修改ip回调函数
 *
 * @param ctx ip地址参数
 */
static void iap_update_ip(void *ctx)
{
    iap_ipconfig_t *config = (iap_ipconfig_t *)ctx;

    netif_set_addr(netif_default, &config->ip, &config->mask, &config->gw);
}

/**
 * @brief  测试
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_Test_00(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
}

/**
 * @brief  报告IP地址
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_ReportIp_01(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    SysInfo_t config_info = *((SysInfo_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[4] = {0};
    ReData[0]          = config_info.net_cfg.ip[0] << 24 | config_info.net_cfg.ip[1] << 16 | config_info.net_cfg.ip[2] << 8 | config_info.net_cfg.ip[3];
    ReData[1]          = config_info.net_cfg.mask[0] << 24 | config_info.net_cfg.mask[1] << 16 | config_info.net_cfg.mask[2] << 8 | config_info.net_cfg.mask[3];
    ReData[2]          = config_info.net_cfg.gw[0] << 24 | config_info.net_cfg.gw[1] << 16 | config_info.net_cfg.gw[2] << 8 | config_info.net_cfg.gw[3];
    ReData[3]          = config_info.net_cfg.port;

    cmd_SendReData(meta, IAP_Data->seq, rtn_cmd01, U32_LEN(sizeof(ReData)), ReData);
}

iap_ipconfig_t ipconfig = {0};
/**
 * @brief  强制修改IP地址
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_ForceModifyIP_02(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    SysInfo_t config_info = *((SysInfo_t *)ADDR_CONFIG_SECTOR);

    uint32_t TmpData[4] = {0};
    memcpy(TmpData, IAP_Data->data_crc, sizeof(TmpData));

    NetConfig_t net_info = {0};
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

    // 写入配置信息
    Edit_Config_Info(&config_info);

    cmd_SendReData(meta, IAP_Data->seq, rtn_cmd02, 0, NULL);
}

/**
 * @brief 报告固件状态
 *
 * @param IAP_Data IAP数据包
 */
static void cmd_ReportFirmwareStatus_03(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    SysInfo_t config_info = *((SysInfo_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[11] = {0};
    ReData[0]           = config_info.app_info.size;                                        // 固件大小
    ReData[1]           = config_info.app_info.crc32;                                       // 固件CRC32
    memcpy(ReData + 2, config_info.app_info.version, sizeof(config_info.app_info.version)); // 固件版本信息
    ReData[10] = config_info.update_sta;

    cmd_SendReData(meta, IAP_Data->seq, rtn_cmd03, U32_LEN(sizeof(ReData)), ReData);
}

/**
 * @brief 准备升级
 *
 * @param IAP_Data IAP数据包
 */
static void cmd_PrepareUpgrade_04(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    // main app无升级
}

/**
 * @brief  发送升级包
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_SendUpgradePackage_05(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    // main app无升级
}

/**
 * @brief  进入恢复模式
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_EnterRecoveryMode_06(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, FLAG_FORCE_UPDATE);

    cmd_SendReData(meta, IAP_Data->seq, rtn_cmd06, 0, NULL);
}

/**
 * @brief  重新启动
 *
 * @param  IAP_Data: IAP数据包
 */
static void cmd_Restart_07(ch_meta_t *meta, iap_frame_t *IAP_Data)
{
    cmd_SendReData(meta, IAP_Data->seq, rtn_cmd07, 0, NULL);
    HAL_IWDG_Refresh(&hiwdg);

    NVIC_SystemReset();
}
