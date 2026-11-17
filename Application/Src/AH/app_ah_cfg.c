/**
 * @file    app_ah_cfg.c
 * @brief   AH 参数配置实现：默认值、校验、ah_cfg 记录加载/保存
 *
 * 记录归属/地址/版本/长度/CRC32 全由 `app_cfg_sched` + `dev_cfg_record` 负责；
 * 本模块只管“载荷语义 + 默认值 + 合法性”。加载失败一律回落默认值，不把设备卡死。
 */

#include "app_ah_cfg.h"

#include <stdio.h>
#include <string.h>

#include "app_cfg_sched.h"
#include "board.h"
#include "initcall.h"
#include "pl_net.h"

static uint8_t      s_ah_cfg_id = 0xFFU; /**< 配置调度器句柄；0xFF = 未注册 */
static app_ah_cfg_t s_ah_cfg;            /**< RAM 镜像（唯一写者是本模块） */
static bool         s_ah_cfg_loaded;     /**< RAM 镜像可用（记录或默认值） */
static bool         s_ah_cfg_load_done;  /**< 本次上电已尝试过加载（IO_ERR 时留 false 重试） */

/* ================================================================
 *  校验辅助
 * ================================================================ */

/** @brief IP 是否为“可接受的单播地址或全 0（未配置）” */
static bool _ah_cfg_ip_is_valid(const uint8_t ip[4])
{
    if (ip[0] == 0U && ip[1] == 0U && ip[2] == 0U && ip[3] == 0U)
        return true;
    if (ip[0] == 0U) /* 0.x.x.x 不是可用单播地址 */
        return false;
    if (ip[0] >= 224U) /* 组播 + 保留 + 广播 */
        return false;
    if (ip[0] == 127U) /* 环回 */
        return false;
    if (ip[0] == 169U && ip[1] == 254U) /* 链路本地 */
        return false;
    return true;
}

/** @brief 掩码是否为“全 0（未配置）、全 1，或连续的 1” */
static bool _ah_cfg_mask_is_valid(const uint8_t mask[4])
{
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) |
                 ((uint32_t)mask[2] << 8) | (uint32_t)mask[3];

    if (m == 0U)
        return true;
    if (m == 0xFFFFFFFFU)
        return false;

    uint32_t inv = ~m;
    return (inv & (inv + 1U)) == 0U; /* 反码必须是低位连续的 0（即掩码高位连续 1） */
}

/** @brief 两个地址在给定掩码下是否同网段 */
static bool _ah_cfg_same_subnet(const uint8_t a[4], const uint8_t b[4], const uint8_t mask[4])
{
    for (uint8_t i = 0; i < 4U; i++) {
        if ((a[i] & mask[i]) != (b[i] & mask[i]))
            return false;
    }
    return true;
}

/** @brief 四个字节是否全 0 */
static bool _ah_cfg_ip_is_zero(const uint8_t ip[4])
{
    return ip[0] == 0U && ip[1] == 0U && ip[2] == 0U && ip[3] == 0U;
}

/* ================================================================
 *  默认值
 * ================================================================ */

void app_ah_cfg_defaults(app_ah_cfg_t *out)
{
    if (out == nullptr)
        return;

    memset(out, 0, sizeof(*out));

    /* 文档主题样例：站 3201FFFF、道 01 */
    const uint8_t lane[5] = {0x32U, 0x01U, 0xFFU, 0xFFU, 0x01U};
    memcpy(out->lane_hex, lane, sizeof(lane));

    out->device_id = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    out->device_no = 0x01U;
    out->font_line = 0U;

    /* 沿用现网的 MQTT 平台地址（现场由 0AH 覆盖） */
    const uint8_t mqtt_ip[4] = {120U, 46U, 136U, 199U};
    memcpy(out->mqtt_ip, mqtt_ip, sizeof(mqtt_ip));
    out->mqtt_port = 6000U;

    const char user[] = "pxh";
    memcpy(out->mqtt_user, user, sizeof(user) - 1U); /* 其余保持 0x00 填充 */

    out->client_port = 6001U;

    /* 主应用默认网络跟随板级 netif 默认（dev_eth：192.168.1.100/24，gw 1.1）。
       IAP 记录的 192.168.114.200 是 Recovery/升级约定地址，不作为主应用默认。 */
    pl_net_get_ip(out->client_ip, out->netmask, out->gateway);
}

/* ================================================================
 *  校验
 * ================================================================ */

bool app_ah_cfg_validate(const app_ah_cfg_t *cfg, app_ah_err_t *err)
{
    if (err != nullptr)
        *err = APP_AH_ERR_PARAM;
    if (cfg == nullptr)
        return false;

    if (cfg->device_id != (uint8_t)BOARD_AH_DISPLAY_TYPE)
        return false;
    if (cfg->device_no == 0U)
        return false;
    if (cfg->font_line > APP_AH_FONT_LINE_MAX)
        return false;
    if (cfg->client_port == 0U)
        return false;

    if (!_ah_cfg_ip_is_valid(cfg->server1_ip) || !_ah_cfg_ip_is_valid(cfg->server2_ip) ||
        !_ah_cfg_ip_is_valid(cfg->server3_ip) || !_ah_cfg_ip_is_valid(cfg->mqtt_ip) ||
        !_ah_cfg_ip_is_valid(cfg->ntp_ip) || !_ah_cfg_ip_is_valid(cfg->client_ip) ||
        !_ah_cfg_ip_is_valid(cfg->gateway))
        return false;

    if (!_ah_cfg_mask_is_valid(cfg->netmask))
        return false;

    /* MQTT 地址配了就必须有端口；地址全 0 视为不启用 MQTT */
    if (!_ah_cfg_ip_is_zero(cfg->mqtt_ip) && cfg->mqtt_port == 0U)
        return false;

    /* 网关与设备地址必须同网段（三者都配了才检查，0 表示未配置） */
    if (!_ah_cfg_ip_is_zero(cfg->client_ip) && !_ah_cfg_ip_is_zero(cfg->gateway) &&
        !_ah_cfg_ip_is_zero(cfg->netmask) &&
        !_ah_cfg_same_subnet(cfg->client_ip, cfg->gateway, cfg->netmask))
        return false;

    if (err != nullptr)
        *err = APP_AH_ERR_OK;
    return true;
}

/* ================================================================
 *  记录加载 / 保存
 * ================================================================ */

/** @brief 配置调度器的启动加载回调：读记录、校验、回落默认值 */
static void _ah_cfg_load(void)
{
    if (s_ah_cfg_load_done)
        return;

    if (s_ah_cfg_id == 0xFFU) {
        /* 没注册上（名重复/满员）：用默认值，不持久化 */
        app_ah_cfg_defaults(&s_ah_cfg);
        s_ah_cfg_loaded    = true;
        s_ah_cfg_load_done = true;
        return;
    }

    app_ah_cfg_t           loaded;
    uint16_t               len = 0;
    dev_cfg_record_state_t state =
        app_cfg_sched_load(s_ah_cfg_id, (uint8_t *)&loaded, (uint16_t)sizeof(loaded), &len);

    if (state == DEV_CFG_RECORD_STATE_OK && len == (uint16_t)sizeof(loaded)) {
        s_ah_cfg = loaded;

        /* 记录可能来自另一块板/另一个型号：类型必须服从本板，否则显示载荷会解析错 */
        if (s_ah_cfg.device_id != (uint8_t)BOARD_AH_DISPLAY_TYPE) {
            printf("[ah/cfg] 记录显示类型 0x%02X 与本板 0x%02X 不符，按本板类型处理\n",
                   (unsigned)s_ah_cfg.device_id, (unsigned)BOARD_AH_DISPLAY_TYPE);
            s_ah_cfg.device_id = (uint8_t)BOARD_AH_DISPLAY_TYPE;
            s_ah_cfg.font_line = 0U;
        }

        /* CRC 对不代表语义对（旧固件/别的板写进来的记录）：语义非法同样回落默认值 */
        if (!app_ah_cfg_validate(&s_ah_cfg, nullptr)) {
            printf("[ah/cfg] 记录参数非法，回落默认值\n");
            app_ah_cfg_defaults(&s_ah_cfg);
        }

        s_ah_cfg_loaded    = true;
        s_ah_cfg_load_done = true;
        return;
    }

    /* EMPTY / INVALID / 长度不符：回落默认值。IO_ERR 时留一次重试机会 */
    app_ah_cfg_defaults(&s_ah_cfg);
    s_ah_cfg_loaded = true;

    if (state == DEV_CFG_RECORD_STATE_INVALID)
        printf("[ah/cfg] 记录无效（版本/长度/CRC 不符），回落默认值\n");
    else if (state == DEV_CFG_RECORD_STATE_OK)
        printf("[ah/cfg] 记录长度 %u 与载荷 %u 不符，回落默认值\n", (unsigned)len,
               (unsigned)sizeof(loaded));

    if (state != DEV_CFG_RECORD_STATE_IO_ERR)
        s_ah_cfg_load_done = true;
}

void app_ah_cfg_get(app_ah_cfg_t *out)
{
    if (out == nullptr)
        return;

    _ah_cfg_load();
    *out = s_ah_cfg;
}

int32_t app_ah_cfg_save(const app_ah_cfg_t *cfg)
{
    if (cfg == nullptr)
        return -1;

    app_ah_err_t err = APP_AH_ERR_PARAM;
    if (!app_ah_cfg_validate(cfg, &err)) {
        printf("[ah/cfg] 保存前校验失败（ErrCode=%u），拒绝写入\n", (unsigned)err);
        return -1;
    }

    if (s_ah_cfg_id == 0xFFU) {
        printf("[ah/cfg] 记录未注册，参数不持久化\n");
        return -1;
    }

    int32_t rc = app_cfg_sched_save(s_ah_cfg_id, (const uint8_t *)cfg, (uint16_t)sizeof(*cfg));
    if (rc == 0) {
        s_ah_cfg           = *cfg;
        s_ah_cfg_loaded    = true;
        s_ah_cfg_load_done = true;
    } else {
        printf("[ah/cfg] 保存失败（%d），RAM 保持原值\n", (int)rc);
    }
    return rc;
}

/* ================================================================
 *  自注册
 * ================================================================ */

static const app_cfg_sched_desc_t s_ah_cfg_desc = {
    .name    = APP_AH_CFG_RECORD_NAME,
    .version = APP_AH_CFG_VERSION,
    .load    = _ah_cfg_load,
};

/** @brief sw_dev(2)：注册配置所有者；真正的读取由 load_all 在 sw_app(3) 触发 */
static void _ah_cfg_register(void)
{
    s_ah_cfg_id = app_cfg_sched_register(&s_ah_cfg_desc);
    if (s_ah_cfg_id == 0xFFU)
        printf("[ah/cfg] 配置所有者注册失败，本次上电参数不持久化\n");

    /* 若注册前有人先取过配置（测试/异常时序），注册后重新走一遍加载 */
    s_ah_cfg_loaded    = false;
    s_ah_cfg_load_done = false;
}
sw_dev_initcall(_ah_cfg_register);
