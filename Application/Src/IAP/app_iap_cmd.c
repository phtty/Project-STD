/**
 * @file    app_iap_cmd.c
 * @brief   IAP 协议命令处理（0x00~0x0C）
 *
 * main app 只实现：0x00 查询、0x01/0x02 IP、0x03 状态、0x06 进 Recovery、0x0C 触发
 * TEST；0x04/0x05/0x07 返回“状态不允许”，真正的升级流程在 Recovery 里。
 */

#include "app_iap_cmd.h"

#include "board.h"
#include "app_factory_test.h"
#include "app_rs485.h"
#include "app_screen.h"

#include "pl_crc.h"
#include "pl_net.h"
#include "pl_rtc.h"
#include "pl_iwdg.h"
#include "pl_sys.h"
#include "app_tcp_server.h"
#include "app_udp.h"

#include <string.h>

#define U8_LEN(x)  ((x) * sizeof(uint32_t))
#define U32_LEN(y) ((y) / sizeof(uint32_t))

#define IAP_HS_MAGIC (0x0D000721U)

static void _iap_cmd_query(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_report_ip(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_force_modify_ip(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_report_fw_status(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_prepare_upgrade(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_send_upgrade_pkg(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_enter_recovery(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_restart(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_reserved(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);
static void _iap_cmd_trigger_test(app_ccb_t *ccb, app_iap_frame_t *IAP_Data);

/* ================================================================
 *  命令表（按 cmd 编号索引）
 * ================================================================ */
const app_iap_cmd_handler_fn_t g_iap_cmd_table[] = {
    _iap_cmd_query,          /* 0x00 */
    _iap_cmd_report_ip,      /* 0x01 */
    _iap_cmd_force_modify_ip,/* 0x02 */
    _iap_cmd_report_fw_status,/* 0x03 */
    _iap_cmd_prepare_upgrade,/* 0x04 */
    _iap_cmd_send_upgrade_pkg,/* 0x05 */
    _iap_cmd_enter_recovery, /* 0x06 */
    _iap_cmd_restart,        /* 0x07 */
    _iap_cmd_reserved,       /* 0x08 */
    _iap_cmd_reserved,       /* 0x09 */
    _iap_cmd_reserved,       /* 0x0A */
    _iap_cmd_reserved,       /* 0x0B */
    _iap_cmd_trigger_test,   /* 0x0C */
};

/** @brief 构造 IAP 响应帧并发送 */
static void _iap_cmd_send_re_data(app_ccb_t *ccb, uint32_t ReSeq, uint32_t ReCmd, uint32_t ReLen,
                                  uint32_t *ReData)
{
    static uint32_t ReBuff[FRAME_MAX_LEN] = {0};

    app_iap_frame_t *pIAP_ReTmp = (app_iap_frame_t *)&(ReBuff);
    pIAP_ReTmp->head        = 0x5A5A5A5A;
    pIAP_ReTmp->seq         = ReSeq;
    pIAP_ReTmp->cmd         = ReCmd;
    pIAP_ReTmp->len         = ReLen;

    if (ReLen != 0)
        memcpy(pIAP_ReTmp->data_crc, ReData, U8_LEN(ReLen));

    pIAP_ReTmp->data_crc[ReLen] = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)pIAP_ReTmp, sizeof(app_iap_frame_t) + U8_LEN(ReLen));

    /* cmd01/cmd02 回包走广播：以“目的地意图”表达，由通道各自翻译 ——
       UDP 译为 255.255.255.255，RS485/RS232/TCP 无广播概念则忽略该字段、退化为点对点。
       协议侧因此不必认识具体通道类型（此前是向下转型改 UDP 内部字段 src_ip，
       加一种广播型通道就得回来改这里）。 */
    app_ccb_dst_t dst = {.broadcast = true};
    bool is_bcast = (ReCmd == APP_IAP_RTN_CMD_01 || ReCmd == APP_IAP_RTN_CMD_02);

    app_ccb_send_to(ccb, is_bcast ? &dst : NULL, (uint8_t *)pIAP_ReTmp,
                sizeof(app_iap_frame_t) + U8_LEN(ReLen) + sizeof(uint32_t));
}

/* ---- Command handlers (0x00 ~ 0x0C) ---- */

/** @brief 0x00: 查询模式/能力 */
static void _iap_cmd_query(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    if (IAP_Data->len < 1U || IAP_Data->data_crc[0] != IAP_HS_MAGIC)
        return;

    uint32_t ReData[4] = {IAP_HS_MAGIC, 0U /* main app */, BOARD_IAP_CAPS, BOARD_IAP_HW_ID};
    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_00, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x01: Report current IP config
 *
 *  优先回传 IAP 记录的 net_cfg 镜像；**本板无记录区或记录无效时绝不裸读 Flash**，
 *  改为回落到运行态（pl_net_get_ip + app_tcp_server_get_port）。
 *
 *  为什么必须守：0x08004000 在直烧板（5006048，BOARD_HAS_IAP_RECORD=0）上落在
 *  固件映像内部，裸读出来是代码字节 —— 这就是现场“搜索”时 IAP 广播的地址变成
 *  一团乱码的直接来源；记录为空/损坏（擦-写中途掉电）时裸读则是 255.255.255.255
 *  或陈旧值。运行态才是设备“当前实际在用”的地址，也是 IAP 记录本应镜像的对象。
 *
 *  选回落运行态而非返回“无配置”错误码：① 不改回包长度/命令码（§13 禁改），上位机
 *  解析逻辑无需变动；② 记录区在直烧板上根本不存在，“无配置”不是一个可恢复的错误，
 *  而运行态地址始终有效；③ 与 app_flash_iap_sync_from_runtime() 取源一致。 */
static void _iap_cmd_report_ip(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    uint8_t  ip[4] = {0}, mask[4] = {0}, gw[4] = {0};
    uint32_t port  = app_tcp_server_get_port();

    app_flash_iap_net_cfg_t rec;
    if (app_iap_get_net_cfg(&rec)) {
        memcpy(ip, rec.ip, 4);
        memcpy(mask, rec.mask, 4);
        memcpy(gw, rec.gw, 4);
        port = rec.port;
    } else {
        pl_net_get_ip(ip, mask, gw);
    }

    uint32_t ReData[4] = {0};
    ReData[0]          = (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 | (uint32_t)ip[2] << 8 | ip[3];
    ReData[1]          = (uint32_t)mask[0] << 24 | (uint32_t)mask[1] << 16 | (uint32_t)mask[2] << 8 | mask[3];
    ReData[2]          = (uint32_t)gw[0] << 24 | (uint32_t)gw[1] << 16 | (uint32_t)gw[2] << 8 | gw[3];
    ReData[3]          = port;

    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_01, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x02: 强制改 IP —— main app 中留桩，仅回执
 *
 *  强制改 IP 在 main app 中不实现，避免与具备 IP 配置能力的协议（如 LDI 0AH）
 *  争夺“谁说了算”。分工约定：
 *    - 有配置能力的协议：由其自己的设置命令与上电加载管理 IP
 *    - 无配置能力的协议：由 Recovery app 的 0x02 写 IAP 记录，
 *      该协议上电时读取 IAP 记录（app_flash_iap_is_config_valid）应用
 *  本处理器仅回执，不做任何修改。
 *
 *  原先这里是“整块读 ADDR_CONFIG_SECTOR → 覆盖 net_cfg → edit_config”。该实现
 *  绕过 app_flash_iap_update_net_cfg 的空记录骨架：记录为全 0xFF 时，magic 保持
 *  0xFFFFFFFF 而 crc 被重算，产出**既非空也非有效、且永久如此**的记录——正是
 *  d8c5b06 定性为会砖的那一类。改为留桩后该路径整体消失。 */
static void _iap_cmd_force_modify_ip(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_02, 0, NULL);
}

/** @brief 0x03: 主卡 + 从卡状态
 *
 *  main app 不知道从卡固件细节，只能给出级联层看得到的在线状态；其它字段填
 *  unknown（mode=2、update=3），升级进度以 Recovery 的 0x03 为准。 */
static void _iap_cmd_report_fw_status(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    app_flash_iap_sys_info_t config_info = *((app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR);

    app_screen_status_t st = {0};
    app_screen_status(&st);
    bool slave_online = (st.online_mask & 0x01U) != 0U;

    uint32_t ReData[8] = {0};
    ReData[0]          = config_info.app_info.size;
    ReData[1]          = config_info.app_info.crc32;
    ReData[2]          = config_info.update_status;
    ReData[3]          = slave_online ? 1U : 0U;
    ReData[4]          = 2U; /* unknown */
    ReData[5]          = 0U;
    ReData[6]          = 0U;
    ReData[7]          = 3U; /* unknown */

    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_03, U32_LEN(sizeof(ReData)), ReData);
}

/** @brief 0x04: 准备升级 —— main app 不处理，返回“状态不允许” */
static void _iap_cmd_prepare_upgrade(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    uint32_t status = 2U;
    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_04, 1, &status);
}

/** @brief 0x05: 发送升级包 —— main app 忽略 */
static void _iap_cmd_send_upgrade_pkg(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    (void)ccb;
    (void)IAP_Data;
}

/** @brief 0x06: 进入 Recovery —— 先让从卡也进去，再复位自己 */
static void _iap_cmd_enter_recovery(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    /* 先把角色落盘：Recovery 靠它区分主/从 */
    app_flash_iap_sys_info_t cfg = *g_iap_sys_info;
    cfg.role                     = APP_FLASH_IAP_ROLE_MASTER;
    app_flash_iap_edit_config(&cfg);

    /* 从卡还在 main app：内部 0x14 让它落角色 + 置标志 + 复位。
       这里不等 ACK —— 主卡马上也要复位，主机在 Recovery 里用 0x03 复核从卡；
       没有应答时主卡仍可自己复位，必要时主机重发 0x06（Recovery 里会补发）。 */
    if ((BOARD_IAP_CAPS & 0x01U) != 0U) {
        app_iap_send_internal(0x00014C14U, 0, 0, NULL);
        osDelay(50);
    }

    /* 复位进 Recovery 前强制对账一次镜像：保证 Recovery 读到的 net_cfg 就是
       main app 此刻在用的 IP，消除“上电对账还没跑到就被复位”的滞后窗口 */
    app_flash_iap_sync_from_runtime();
    pl_rtc_bkup_write(pl_rtc_get_handle(), 0 /* RTC 备份寄存器 0 */, FLAG_FORCE_UPDATE);

    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_06, 0, NULL);

    osDelay(100);
    pl_iwdg_refresh(pl_iwdg_get_handle());
    pl_system_reset();
}

/** @brief 0x07: 重启 —— main app 不处理，返回“状态不允许” */
static void _iap_cmd_restart(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    uint32_t status = 2U;
    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_07, 1, &status);
}

/** @brief 0x08~0x0B: 保留 */
static void _iap_cmd_reserved(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    (void)ccb;
    (void)IAP_Data;
}

/** @brief 0x0C: 远程注入一次 TEST 按键（不触发工厂退出） */
static void _iap_cmd_trigger_test(app_ccb_t *ccb, app_iap_frame_t *IAP_Data)
{
    app_factory_test_trigger();
    _iap_cmd_send_re_data(ccb, IAP_Data->seq, APP_IAP_RTN_CMD_0C, 0, NULL);
}
