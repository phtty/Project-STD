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

#define U8_LEN(x)  ((x) * sizeof(uint32_t))
#define U32_LEN(y) ((y) / sizeof(uint32_t))

static void cmd_Test_00(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_ReportIp_01(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_ForceModifyIP_02(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_ReportFirmwareStatus_03(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_PrepareUpgrade_04(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_SendUpgradePackage_05(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_EnterRecoveryMode_06(ccb_t *ccb, iap_frame_t *IAP_Data);
static void cmd_Restart_07(ccb_t *ccb, iap_frame_t *IAP_Data);

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
static void cmd_SendReData(ccb_t *ccb, uint32_t ReSeq, uint32_t ReCmd, uint32_t ReLen,
                           uint32_t *ReData)
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

    /* cmd01/cmd02 回包走广播：以"目的地意图"表达，由通道各自翻译 ——
       UDP 译为 255.255.255.255，RS485/RS232/TCP 无广播概念则忽略该字段、退化为点对点。
       协议侧因此不必认识具体通道类型（此前是向下转型改 UDP 内部字段 src_ip，
       加一种广播型通道就得回来改这里）。 */
    ccb_dst_t dst = {.broadcast = true};
    bool is_bcast = (ReCmd == rtn_cmd01 || ReCmd == rtn_cmd02);

    ccb_send_to(ccb, is_bcast ? &dst : NULL, (uint8_t *)pIAP_ReTmp,
                sizeof(iap_frame_t) + U8_LEN(ReLen) + sizeof(uint32_t));
}

/* ---- Command handlers (0x00 ~ 0x07) ---- */

/** @brief 0x00: Test (no-op) */
static void cmd_Test_00(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    (void)ccb;
    (void)IAP_Data;
}

/** @brief 0x01: Report current IP config */
static void cmd_ReportIp_01(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    app_flash_iap_sys_info_t config_info = *((app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[4] = {0};
    ReData[0]          = config_info.net_cfg.ip[0] << 24 | config_info.net_cfg.ip[1] << 16 | config_info.net_cfg.ip[2] << 8 | config_info.net_cfg.ip[3];
    ReData[1]          = config_info.net_cfg.mask[0] << 24 | config_info.net_cfg.mask[1] << 16 | config_info.net_cfg.mask[2] << 8 | config_info.net_cfg.mask[3];
    ReData[2]          = config_info.net_cfg.gw[0] << 24 | config_info.net_cfg.gw[1] << 16 | config_info.net_cfg.gw[2] << 8 | config_info.net_cfg.gw[3];
    ReData[3]          = config_info.net_cfg.port;

    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd01, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x02: 强制改 IP —— main app 中留桩，仅回执
 *
 *  强制改 IP 在 main app 中不实现，避免与具备 IP 配置能力的协议（如 LDI 0AH）
 *  争夺"谁说了算"。分工约定：
 *    - 有配置能力的协议：由其自己的设置命令与上电加载管理 IP
 *    - 无配置能力的协议：由 Recovery app 的 0x02 写 IAP 记录，
 *      该协议上电时读取 IAP 记录（app_flash_iap_is_config_valid）应用
 *  本处理器仅回执，不做任何修改。
 *
 *  原先这里是"整块读 ADDR_CONFIG_SECTOR → 覆盖 net_cfg → edit_config"。该实现
 *  绕过 app_flash_iap_update_net_cfg 的空记录骨架：记录为全 0xFF 时，magic 保持
 *  0xFFFFFFFF 而 crc 被重算，产出**既非空也非有效、且永久如此**的记录——正是
 *  d8c5b06 定性为会砖的那一类。改为留桩后该路径整体消失。 */
static void cmd_ForceModifyIP_02(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd02, 0, NULL);
}

/** @brief 0x03: Report firmware version, size, CRC32, update status */
static void cmd_ReportFirmwareStatus_03(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    app_flash_iap_sys_info_t config_info = *((app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    uint32_t ReData[11] = {0};
    ReData[0]           = config_info.app_info.size;
    ReData[1]           = config_info.app_info.crc32;
    memcpy(ReData + 2, config_info.app_info.version, sizeof(config_info.app_info.version));
    ReData[10] = config_info.update_sta;

    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd03, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x04: Prepare for firmware upgrade (main app responsibility) */
static void cmd_PrepareUpgrade_04(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd04, 0, NULL);
}

/** @brief 0x05: Send upgrade package (main app responsibility) */
static void cmd_SendUpgradePackage_05(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd05, 0, NULL);
}

/** @brief 0x06: Enter recovery mode (set flag in RTC backup register then reboot) */
static void cmd_EnterRecoveryMode_06(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    /* 复位进 Recovery 前强制对账一次镜像：保证 Recovery 读到的 net_cfg 就是
       main app 此刻在用的 IP，消除"上电对账还没跑到就被复位"的滞后窗口 */
    app_flash_iap_sync_from_runtime();

    pl_rtc_bkup_write(pl_rtc_get_handle(), 0 /* RTC 备份寄存器 0 */, FLAG_FORCE_UPDATE);
    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd06, 0, NULL);
}

/** @brief 0x07: Soft reset */
static void cmd_Restart_07(ccb_t *ccb, iap_frame_t *IAP_Data)
{
    cmd_SendReData(ccb, IAP_Data->seq, rtn_cmd07, 0, NULL);
    pl_iwdg_refresh(pl_iwdg_get_handle());
    pl_system_reset();
}
