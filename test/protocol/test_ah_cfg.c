/**
 * @file    test_ah_cfg.c
 * @brief   AH 参数配置：默认值来源、校验矩阵、ah_cfg 记录加载/保存与失败回落
 *
 * **为什么需要这个测试**：配置层是“参数接口”的唯一可信来源，判错一个分支的症状都在现场：
 *   - 默认值取错 → 首次上电设备用 0.0.0.0 或旧 IP，平台连不上；
 *   - 校验漏一条 → 0AH 把组播地址/错网段网关写进 Flash，重启后设备失联；
 *   - 保存失败仍改 RAM → 看着配好了、断电重启变回去；
 *   - IO_ERR 后不再重试 → W25Qxx 上电慢一拍就整周期用默认值。
 * 这些都不能靠“上机看一眼”，所以在 host 上把加载/保存/回落逐条钉住。
 *
 * 手法：用例 TU-include 生产 TU `app_ah_cfg.c`（static 的加载回调与 RAM 镜像内部可见），
 * 外部依赖（配置调度器、IAP 网络镜像、pl_net）全部用记录型桩。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_ah_cfg.h"
#include "app_cfg_sched.h"
#include "app_iap_cfg.h"
#include "pl_net.h"

/* ---- 断言 ---- */
static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  桩：配置调度器（注册 / 加载 / 保存）
 * ================================================================ */

static int      s_reg_calls;
static char     s_reg_name[16];
static uint16_t s_reg_version;

uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *desc)
{
    s_reg_calls++;
    if (desc == nullptr || desc->name == nullptr)
        return 0xFFU;

    snprintf(s_reg_name, sizeof(s_reg_name), "%s", desc->name);
    s_reg_version = desc->version;
    return 0U;
}

static dev_cfg_record_state_t s_load_state = DEV_CFG_RECORD_STATE_EMPTY;
static app_ah_cfg_t           s_load_cfg;
static uint16_t               s_load_len;
static int                    s_load_calls;

dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *payload, uint16_t cap,
                                          uint16_t *len)
{
    (void)id;
    s_load_calls++;
    if (len != nullptr)
        *len = 0U;

    if (s_load_state != DEV_CFG_RECORD_STATE_OK)
        return s_load_state;

    if (payload == nullptr || cap < sizeof(s_load_cfg))
        return DEV_CFG_RECORD_STATE_INVALID;

    memcpy(payload, &s_load_cfg, sizeof(s_load_cfg));
    if (len != nullptr)
        *len = s_load_len;
    return DEV_CFG_RECORD_STATE_OK;
}

static int32_t      s_save_rc;
static int          s_save_calls;
static uint16_t     s_save_len;
static app_ah_cfg_t s_save_cfg;

int32_t app_cfg_sched_save(uint8_t id, const uint8_t *payload, uint16_t len)
{
    (void)id;
    s_save_calls++;
    s_save_len = len;
    if (payload != nullptr && len == sizeof(s_save_cfg))
        memcpy(&s_save_cfg, payload, len);
    return s_save_rc;
}

/* ---- 桩：IAP 网络镜像与当前 netif（默认值来源） ---- */

static bool                    s_iap_valid;
static app_flash_iap_net_cfg_t s_iap_net;

bool app_iap_get_net_cfg(app_flash_iap_net_cfg_t *out)
{
    if (!s_iap_valid || out == nullptr)
        return false;
    *out = s_iap_net;
    return true;
}

static uint8_t s_net_ip[4];
static uint8_t s_net_mask[4];
static uint8_t s_net_gw[4];

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_net_ip, 4);
    memcpy(mask, s_net_mask, 4);
    memcpy(gw, s_net_gw, 4);
}

/* 被测实现：static 的加载回调与 RAM 镜像要在本 TU 内可见 */
#include "../../Application/Src/AH/app_ah_cfg.c"

/* ================================================================
 *  用例装置
 * ================================================================ */

static void _reset(void)
{
    s_ah_cfg_id        = 0xFFU;
    s_ah_cfg_loaded    = false;
    s_ah_cfg_load_done = false;
    memset(&s_ah_cfg, 0, sizeof(s_ah_cfg));

    s_reg_calls = 0;
    memset(s_reg_name, 0, sizeof(s_reg_name));
    s_reg_version = 0U;

    s_load_calls = 0;
    s_load_state = DEV_CFG_RECORD_STATE_EMPTY;
    s_load_len   = 0U;
    memset(&s_load_cfg, 0, sizeof(s_load_cfg));

    s_save_calls = 0;
    s_save_rc    = 0;
    s_save_len   = 0U;
    memset(&s_save_cfg, 0, sizeof(s_save_cfg));

    s_iap_valid = false;
    memset(&s_iap_net, 0, sizeof(s_iap_net));
    memset(s_net_ip, 0, sizeof(s_net_ip));
    memset(s_net_mask, 0, sizeof(s_net_mask));
    memset(s_net_gw, 0, sizeof(s_net_gw));
}

/** @brief 一份能通过校验的配置（身份 + 192.168.1.50/24 + 10.0.0.1:1883） */
static app_ah_cfg_t _valid_cfg(void)
{
    app_ah_cfg_t cfg = {0};

    const uint8_t lane[5]      = {0x52U, 0x01U, 0x13U, 0x14U, 0x01U};
    const uint8_t mqtt_ip[4]   = {10U, 0U, 0U, 1U};
    const uint8_t client_ip[4] = {192U, 168U, 1U, 50U};
    const uint8_t mask[4]      = {255U, 255U, 255U, 0U};
    const uint8_t gw[4]        = {192U, 168U, 1U, 1U};

    memcpy(cfg.lane_hex, lane, sizeof(lane));
    cfg.device_id   = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    cfg.device_no   = 0x01U;
    cfg.client_port = 6001U;

    memcpy(cfg.mqtt_ip, mqtt_ip, sizeof(mqtt_ip));
    cfg.mqtt_port = 1883U;
    memcpy(cfg.client_ip, client_ip, sizeof(client_ip));
    memcpy(cfg.netmask, mask, sizeof(mask));
    memcpy(cfg.gateway, gw, sizeof(gw));
    return cfg;
}

/* ================================================================
 *  默认值
 * ================================================================ */

/** @brief 主应用默认网络取板级 netif（1.100），不受 IAP 记录 114.200 影响 */
static void case_defaults_netif_default(void)
{
    TEST_BEGIN("默认值：主应用默认取 netif，IAP 记录不参与");

    _reset();
    /* IAP 记录即使有效，也不能把主应用默认拉到 114.200 */
    s_iap_valid = true;
    const uint8_t iap_ip[4]   = {10U, 11U, 12U, 13U};
    const uint8_t iap_mask[4] = {255U, 255U, 255U, 0U};
    const uint8_t iap_gw[4]   = {10U, 11U, 12U, 1U};
    memcpy(s_iap_net.ip, iap_ip, 4);
    memcpy(s_iap_net.mask, iap_mask, 4);
    memcpy(s_iap_net.gw, iap_gw, 4);
    s_iap_net.port = 9528U;

    s_net_ip[0] = 192U;
    s_net_ip[1] = 168U;
    s_net_ip[2] = 1U;
    s_net_ip[3] = 100U;
    s_net_mask[0] = 255U;
    s_net_mask[1] = 255U;
    s_net_mask[2] = 255U;
    s_net_mask[3] = 0U;
    s_net_gw[0] = 192U;
    s_net_gw[1] = 168U;
    s_net_gw[2] = 1U;
    s_net_gw[3] = 1U;

    app_ah_cfg_t cfg;
    app_ah_cfg_defaults(&cfg);

    CHECK_MSG(memcmp(cfg.client_ip, s_net_ip, 4) == 0, "设备 IP 应取 netif（1.100）");
    CHECK_MSG(memcmp(cfg.netmask, s_net_mask, 4) == 0, "掩码应取 netif");
    CHECK_MSG(memcmp(cfg.gateway, s_net_gw, 4) == 0, "网关应取 netif（1.1）");
    CHECK_MSG(cfg.device_id == (uint8_t)BOARD_AH_DISPLAY_TYPE, "类型应取板级默认");
    CHECK_MSG(cfg.device_no == 0x01U, "序号默认 1");
    CHECK_MSG(cfg.client_port == 6001U, "参数口默认 6001");
    CHECK_MSG(cfg.mqtt_port == 6000U, "MQTT 端口沿用 6000");
    CHECK_MSG(memcmp(cfg.mqtt_user, "pxh", 3) == 0, "MQTT 用户默认 pxh");
    CHECK_MSG(cfg.lane_hex[0] == 0x32U && cfg.lane_hex[4] == 0x01U, "车道默认 3201FFFF/01");
}

/** @brief IAP 无有效记录时回落当前 netif 地址 */
static void case_defaults_netif_fallback(void)
{
    TEST_BEGIN("默认值：IAP 无记录时回落 netif");

    _reset();
    s_iap_valid     = false;
    s_net_ip[0]     = 192U;
    s_net_ip[1]     = 168U;
    s_net_ip[2]     = 114U;
    s_net_ip[3]     = 200U;
    s_net_mask[0]   = 255U;
    s_net_mask[1]   = 255U;
    s_net_mask[2]   = 255U;
    s_net_mask[3]   = 0U;
    s_net_gw[0]     = 192U;
    s_net_gw[1]     = 168U;
    s_net_gw[2]     = 114U;
    s_net_gw[3]     = 1U;

    app_ah_cfg_t cfg;
    app_ah_cfg_defaults(&cfg);

    CHECK_MSG(cfg.client_ip[2] == 114U && cfg.client_ip[3] == 200U, "设备 IP 应回落 netif");
    CHECK_MSG(cfg.gateway[3] == 1U, "网关应回落 netif");
}

/* ================================================================
 *  校验
 * ================================================================ */

/** @brief 合法配置通过；未使用的可选字段为 0 也通过 */
static void case_validate_ok(void)
{
    TEST_BEGIN("校验：合法配置与 0 值可选字段");

    app_ah_cfg_t cfg = _valid_cfg();
    app_ah_err_t err = APP_AH_ERR_PARAM;
    CHECK_MSG(app_ah_cfg_validate(&cfg, &err), "合法配置应通过");
    CHECK_MSG(err == APP_AH_ERR_OK, "成功时 err 应为 OK，得到 %u", (unsigned)err);

    /* server1/2/3、ntp、mqtt 全 0 = 未启用，不应被误判 */
    app_ah_cfg_t zero = _valid_cfg();
    memset(zero.server1_ip, 0, 4);
    memset(zero.server2_ip, 0, 4);
    memset(zero.server3_ip, 0, 4);
    memset(zero.ntp_ip, 0, 4);
    memset(zero.mqtt_ip, 0, 4);
    zero.mqtt_port = 0U;
    CHECK_MSG(app_ah_cfg_validate(&zero, &err), "可选字段全 0 应通过（= 未启用）");
}

/** @brief 身份与端口类非法值逐条拒绝 */
static void case_validate_rejects_identity(void)
{
    TEST_BEGIN("校验：类型/序号/行数/端口非法");

    app_ah_cfg_t cfg;
    app_ah_err_t err;

    cfg            = _valid_cfg();
    cfg.device_id  = (uint8_t)AH_DEV_FEE_DISPLAY;
    err            = APP_AH_ERR_OK;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, &err) && err == APP_AH_ERR_PARAM, "非本板类型应拒绝");

    cfg           = _valid_cfg();
    cfg.device_no = 0U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "序号 0 应拒绝");

    cfg           = _valid_cfg();
    cfg.font_line = 7U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "行数 7 应拒绝");

    cfg             = _valid_cfg();
    cfg.client_port = 0U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "参数口 0 应拒绝");
}

/** @brief 网络类非法值逐条拒绝（组播/错掩码/错网段/环回/0.x） */
static void case_validate_rejects_network(void)
{
    TEST_BEGIN("校验：IP/掩码/网关/端口组合非法");

    app_ah_cfg_t cfg;

    cfg         = _valid_cfg();
    cfg.mqtt_ip[0] = 224U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "组播 MQTT IP 应拒绝");

    cfg           = _valid_cfg();
    cfg.mqtt_port = 0U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "MQTT IP 配了但端口 0 应拒绝");

    cfg          = _valid_cfg();
    cfg.netmask[1] = 0U; /* 255.0.255.0：不连续 */
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "不连续掩码应拒绝");

    cfg           = _valid_cfg();
    cfg.gateway[2] = 2U; /* 192.168.2.1 与 192.168.1.50/24 不同网段 */
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "跨网段网关应拒绝");

    cfg            = _valid_cfg();
    cfg.client_ip[0] = 127U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "环回地址应拒绝");

    cfg             = _valid_cfg();
    cfg.server1_ip[0] = 0U;
    cfg.server1_ip[1] = 1U;
    CHECK_MSG(!app_ah_cfg_validate(&cfg, nullptr), "0.x.x.x 应拒绝");
}

/* ================================================================
 *  注册 / 加载 / 保存
 * ================================================================ */

/** @brief 注册名与版本就是持久化契约；重复注册前会重置加载状态 */
static void case_register(void)
{
    TEST_BEGIN("注册：记录名 ah_cfg、版本 1");

    _reset();
    _ah_cfg_register();

    CHECK_MSG(s_reg_calls == 1, "应注册一次，实际 %d", s_reg_calls);
    CHECK_MSG(strcmp(s_reg_name, "ah_cfg") == 0, "记录名应为 ah_cfg，实际 %s", s_reg_name);
    CHECK_MSG(s_reg_version == 1U, "版本应为 1，实际 %u", (unsigned)s_reg_version);
    CHECK_MSG(s_ah_cfg_id == 0U, "注册成功应拿到句柄 0");
}

/** @brief 记录有效：原样采纳 */
static void case_load_ok(void)
{
    TEST_BEGIN("加载：有效记录原样采纳");

    _reset();
    _ah_cfg_register();
    s_load_state = DEV_CFG_RECORD_STATE_OK;
    s_load_cfg   = _valid_cfg();
    s_load_cfg.lane_hex[0] = 0xAAU;
    s_load_len   = (uint16_t)sizeof(s_load_cfg);

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);

    CHECK_MSG(s_load_calls == 1, "应只读一次记录，实际 %d", s_load_calls);
    CHECK_MSG(out.lane_hex[0] == 0xAAU, "记录里的车道应被采纳");
    CHECK_MSG(out.device_id == (uint8_t)BOARD_AH_DISPLAY_TYPE, "类型应保持本板类型");
    CHECK_MSG(s_ah_cfg_load_done, "成功后应标记加载完成");
}

/** @brief 记录无效/为空：回落默认值，且不再反复读 Flash */
static void case_load_invalid_falls_back(void)
{
    TEST_BEGIN("加载：无效记录回落默认值");

    _reset();
    _ah_cfg_register();
    s_load_state = DEV_CFG_RECORD_STATE_INVALID;

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);

    CHECK_MSG(out.lane_hex[0] == 0x32U, "应回落默认车道");
    CHECK_MSG(out.mqtt_port == 6000U, "应回落默认 MQTT 端口");
    CHECK_MSG(out.device_id == (uint8_t)BOARD_AH_DISPLAY_TYPE, "类型应为板级默认");
    CHECK_MSG(s_ah_cfg_load_done, "INVALID 后应标记完成（重读也不会变好）");
}

/** @brief IO_ERR：先给默认值，但保留重试机会 */
static void case_load_ioerr_retries(void)
{
    TEST_BEGIN("加载：IO_ERR 先回落默认值、下次再试");

    _reset();
    _ah_cfg_register();
    s_load_state = DEV_CFG_RECORD_STATE_IO_ERR;

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);
    CHECK_MSG(out.device_id == (uint8_t)BOARD_AH_DISPLAY_TYPE, "IO_ERR 也应给出可用默认值");
    CHECK_MSG(!s_ah_cfg_load_done, "IO_ERR 不应标记完成");
    CHECK_MSG(s_load_calls == 1, "首次应尝试读取");

    app_ah_cfg_get(&out);
    CHECK_MSG(s_load_calls == 2, "第二次 get 应重试，实际 %d 次", s_load_calls);
}

/** @brief 记录 CRC 对但语义非法（端口 0）：回落默认值，不能拿坏参数上电 */
static void case_load_bad_fields_falls_back(void)
{
    TEST_BEGIN("加载：记录语义非法回落默认值");

    _reset();
    _ah_cfg_register();
    s_load_state         = DEV_CFG_RECORD_STATE_OK;
    s_load_cfg           = _valid_cfg();
    s_load_cfg.client_port = 0U; /* 参数服务必须有端口 */
    s_load_len           = (uint16_t)sizeof(s_load_cfg);

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);

    CHECK_MSG(out.client_port == 6001U, "非法记录应回落默认参数口 6001");
    CHECK_MSG(out.lane_hex[0] == 0x32U, "非法记录应回落默认车道");
}

/** @brief 记录来自别的型号：类型强制回本板，避免用错显示载荷 */
static void case_load_wrong_type(void)
{
    TEST_BEGIN("加载：记录类型与本板不符时强制校正");

    _reset();
    _ah_cfg_register();
    s_load_state        = DEV_CFG_RECORD_STATE_OK;
    s_load_cfg          = _valid_cfg();
    s_load_cfg.device_id = (uint8_t)AH_DEV_FEE_DISPLAY;
    s_load_cfg.font_line = 4U;
    s_load_cfg.lane_hex[0] = 0x77U;
    s_load_len          = (uint16_t)sizeof(s_load_cfg);

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);

    CHECK_MSG(out.device_id == (uint8_t)BOARD_AH_DISPLAY_TYPE, "类型应被强制回本板");
    CHECK_MSG(out.font_line == 0U, "类型校正后行数应清零");
    CHECK_MSG(out.lane_hex[0] == 0x77U, "其余字段不应被牵连");
}

/** @brief 保存：成功才更新 RAM；非法/未注册/写失败都不改运行态 */
static void case_save(void)
{
    TEST_BEGIN("保存：成功才提交 RAM，失败保持原值");

    _reset();
    _ah_cfg_register();

    app_ah_cfg_t cfg = _valid_cfg();
    cfg.device_no    = 0x07U;
    s_save_rc        = 0;

    CHECK_MSG(app_ah_cfg_save(&cfg) == 0, "保存应成功");
    CHECK_MSG(s_save_calls == 1, "应调用一次调度器保存");
    CHECK_MSG(s_save_len == (uint16_t)sizeof(cfg), "保存长度应为 80");
    CHECK_MSG(memcmp(&s_save_cfg, &cfg, sizeof(cfg)) == 0, "落盘内容应与入参一致");

    app_ah_cfg_t out;
    app_ah_cfg_get(&out);
    CHECK_MSG(out.device_no == 0x07U, "成功后 RAM 镜像应更新");

    /* 非法配置：不落盘 */
    app_ah_cfg_t bad = cfg;
    bad.device_no    = 0U;
    int before       = s_save_calls;
    CHECK_MSG(app_ah_cfg_save(&bad) == -1, "非法配置应拒绝");
    CHECK_MSG(s_save_calls == before, "非法配置不应调用保存");

    /* 写失败：RAM 不跟着改 */
    s_save_rc       = -2;
    app_ah_cfg_t cfg2 = cfg;
    cfg2.device_no  = 0x09U;
    CHECK_MSG(app_ah_cfg_save(&cfg2) == -2, "写失败应把错误码透传");
    app_ah_cfg_get(&out);
    CHECK_MSG(out.device_no == 0x07U, "写失败后 RAM 应保持上一次成功值");
}

/** @brief 未注册：保存必须失败，不能假装成功 */
static void case_save_unregistered(void)
{
    TEST_BEGIN("保存：记录未注册时拒绝");

    _reset();
    app_ah_cfg_t cfg = _valid_cfg();

    CHECK_MSG(app_ah_cfg_save(&cfg) == -1, "未注册应返回 -1");
    CHECK_MSG(s_save_calls == 0, "未注册不应触碰 Flash");
}

/* ================================================================ */

int main(void)
{
    case_defaults_netif_default();
    case_defaults_netif_fallback();
    case_validate_ok();
    case_validate_rejects_identity();
    case_validate_rejects_network();
    case_register();
    case_load_ok();
    case_load_invalid_falls_back();
    case_load_ioerr_retries();
    case_load_bad_fields_falls_back();
    case_load_wrong_type();
    case_save();
    case_save_unregistered();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
