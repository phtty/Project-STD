/**
 * @file    test_ah_mqtt_cmd.c
 * @brief   AH/MQTT 命令层：显示类型分派、清屏、重启、对时、应答格式
 *
 * **为什么需要这个测试**：命令层是平台能“动设备”的唯一入口，
 *   显示命令按设备类型分派错 → 31H 费显收到 32H 载荷被当文字乱画；
 *   颜色/字号不验范围 → 网络字节直接当下标越界；
 *   应答不带请求的通知号 → 平台对不上请求，重发/判超时；
 *   对时字符串解析错 → RTC 被写成 1970 或直接不校时。
 * 这些都不能靠上机抓一次包，所以在 host 上逐条钉住。
 *
 * 手法：用例 TU-include 生产 TU `app_ah_mqtt_cmd.c`（处理函数与回复函数是 static），
 * 渲染/屏幕/RTC/复位/发送全部用记录型桩。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_ah.h"
#include "app_ah_mqtt.h"
#include "app_ah_mqtt_cmd.h"
#include "app_dispatch.h"
#include "app_render.h"
#include "app_screen.h"
#include "dev_display.h"
#include "pl_rtc.h"
#include "text_cvt.h"

/* CMSIS 的复位入口；host 上由本套件记录调用 */
void NVIC_SystemReset(void);

/* 生产实现 TU-include：命令处理与回复拼接是 static */
#include "../Application/Src/AH/app_ah_mqtt_cmd.c"

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
 *  桩：渲染 / 屏幕几何 / 编码转换 / RTC / 复位 / 发送
 * ================================================================ */

typedef struct {
    int               calls;
    app_render_type_t type;
    dev_display_color_t color;
    app_font_size_t   font_size;
    uint16_t          len;
    char              text[160];
} render_cap_t;

static render_cap_t s_render;

void app_render(const app_render_cfg_t *cfg)
{
    s_render.calls++;
    if (cfg == nullptr)
        return;
    s_render.type      = cfg->type;
    s_render.color     = cfg->color;
    s_render.font_size = cfg->font_size;
    s_render.len       = cfg->len;
    if (cfg->text != nullptr && cfg->len < sizeof(s_render.text)) {
        memcpy(s_render.text, cfg->text, cfg->len);
        s_render.text[cfg->len] = '\0';
    }
}

uint16_t app_screen_rows(void)
{
    return 128U;
}

uint16_t app_screen_cols(void)
{
    return 32U;
}

void cvt_utf8_to_gbk(const char *from, uint32_t fromSize, char *to, uint32_t *toSize)
{
    memcpy(to, from, fromSize);
    *toSize = fromSize;
}

static dev_display_t s_display;

dev_display_t *dev_display_get(void)
{
    return &s_display;
}

static int      s_rtc_calls;
static uint32_t s_rtc_ts;

pl_rtc_handle_t pl_rtc_get_handle(void)
{
    return nullptr;
}

bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts)
{
    (void)h;
    s_rtc_calls++;
    s_rtc_ts = ts;
    return true;
}

static int s_reset_calls;

void NVIC_SystemReset(void)
{
    s_reset_calls++;
}

static uint8_t  s_tx[128];
static uint16_t s_tx_len;
static int      s_tx_calls;
static char     s_tx_topic[APP_AH_TOPIC_MAX];

static app_ccb_t s_ccb = {.name = "ah_mqtt_cmd_ut"};

int32_t app_ccb_send_to(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data,
                        uint16_t len)
{
    (void)ccb;
    s_tx_calls++;
    snprintf(s_tx_topic, sizeof(s_tx_topic), "%s",
             (dst != nullptr && dst->topic != nullptr) ? dst->topic : "");
    if (len > sizeof(s_tx))
        len = (uint16_t)sizeof(s_tx);
    memcpy(s_tx, data, len);
    s_tx_len = len;
    return (int32_t)len;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static const char k_notify[APP_AH_NOTIFY_ID_LEN + 1] = "20250305143201000007";

static app_ah_mqtt_proto_t s_proto;

static void _reset(uint8_t device_id)
{
    memset(&s_proto, 0, sizeof(s_proto));
    snprintf(s_proto.prefix, sizeof(s_proto.prefix), "52011314/01/32/01");
    s_proto.cfg.device_id = device_id;
    s_proto.cfg.device_no = 0x01U;

    memset(&s_render, 0, sizeof(s_render));
    s_rtc_calls  = 0;
    s_rtc_ts     = 0U;
    s_reset_calls = 0;
    s_tx_calls   = 0;
    s_tx_len     = 0;
    memset(s_tx, 0, sizeof(s_tx));
    memset(s_tx_topic, 0, sizeof(s_tx_topic));
}

/** @brief 组 32H 显示载荷：NotifyID(20) + OpenClose + FontColor + FontSize + Text */
static uint16_t _display_buff(char *buf, char color, char size, const char *text)
{
    memcpy(buf, k_notify, APP_AH_NOTIFY_ID_LEN);
    buf[20] = '1';
    buf[21] = color;
    buf[22] = size;
    size_t n = strlen(text);
    memcpy(buf + 23U, text, n);
    buf[23U + n] = '\0';
    return (uint16_t)(23U + n);
}

/** @brief 应答必须是「20 字节通知号 + 1 字节结果」，且通知号与请求一致 */
static bool _reply_is(const char *result, const char *topic_suffix)
{
    char expect_topic[APP_AH_TOPIC_MAX];
    snprintf(expect_topic, sizeof(expect_topic), "52011314/01/32/01/%s", topic_suffix);

    return s_tx_calls == 1 && s_tx_len == (APP_AH_NOTIFY_ID_LEN + 1U) &&
           memcmp(s_tx, k_notify, APP_AH_NOTIFY_ID_LEN) == 0 && s_tx[APP_AH_NOTIFY_ID_LEN] == (uint8_t)result[0] &&
           strcmp(s_tx_topic, expect_topic) == 0;
}

/* ================================================================
 *  显示
 * ================================================================ */

/** @brief 32H：文字渲染 + 通知号回显 + 成功应答 */
static void case_display_ok(void)
{
    TEST_BEGIN("显示 32H：文字渲染 + 应答");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char     buff[64];
    uint16_t n = _display_buff(buff, '2', '4', "Hello");
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_render.calls == 1, "应渲染一次，实际 %d", s_render.calls);
    CHECK_MSG(s_render.type == APP_RENDER_TYPE_TEXT, "渲染类型应为文字");
    CHECK_MSG(s_render.color == DEV_DISPLAY_COLOR_RED, "颜色 '2' 应映射为红色");
    CHECK_MSG(s_render.font_size == APP_FONT_SIZE_32, "字号 '4' 应映射为 32");
    CHECK_MSG(s_render.len == 5U && strcmp(s_render.text, "Hello") == 0, "文本应为 Hello");
    CHECK_MSG(n == 28U, "载荷长度自检应为 28");

    CHECK_MSG(_reply_is("0", "Reply/board/NULL"), "应答应为 通知号+'0'，主题 Reply/board/NULL");
    CHECK_MSG(memcmp(&s_proto.notify_in, k_notify, APP_AH_NOTIFY_ID_LEN) == 0,
              "协议对象应记下请求的通知号");
}

/** @brief 颜色/字号越界：不渲染，回 '1' */
static void case_display_bad_index(void)
{
    TEST_BEGIN("显示：颜色/字号越界 → 不渲染 + 回 '1'");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char     buff[64];
    _display_buff(buff, '9', '4', "Hello");
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_render.calls == 0, "越界颜色不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/board/NULL"), "失败应答应为 '1'");

    _reset((uint8_t)AH_DEV_LED_BOARD);
    _display_buff(buff, '2', '9', "Hello");
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);
    CHECK_MSG(s_render.calls == 0, "越界字号不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/board/NULL"), "字号越界也应回 '1'");
}

/** @brief 非 32H：31H/未知类型都不按情报板载荷渲染，回 '1' */
static void case_display_other_type(void)
{
    TEST_BEGIN("显示：31H/未知类型 → 不渲染，回 '1'");
    _reset((uint8_t)AH_DEV_FEE_DISPLAY);

    char     buff[64];
    _display_buff(buff, '2', '4', "Hello");
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);
    CHECK_MSG(s_render.calls == 0, "31H 本期不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/board/NULL"), "31H 应回 '1'");

    _reset(0x99U);
    _display_buff(buff, '2', '4', "Hello");
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);
    CHECK_MSG(s_render.calls == 0, "未知类型不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/board/NULL"), "未知类型应回 '1'");
}

/** @brief 超长文本：不渲染半截，回 '1' */
static void case_display_too_long(void)
{
    TEST_BEGIN("显示：文本超过 127 B → 不渲染，回 '1'");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char text[201];
    memset(text, 'A', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    char     buff[256];
    _display_buff(buff, '2', '4', text);
    g_ah_mqtt_cmd_table[0].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_render.calls == 0, "超长文本不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/board/NULL"), "超长文本应回 '1'");
}

/* ================================================================
 *  清屏
 * ================================================================ */

/** @brief 兼容清屏 0/1/2/3：黑/绿/红/黄，全部回 '0' */
static void case_clean(void)
{
    TEST_BEGIN("清屏：0/1/2/3 → 黑/绿/红/黄");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    static const struct {
        char                code;
        dev_display_color_t color;
    } cases[] = {
        {'0', DEV_DISPLAY_COLOR_BLACK},
        {'1', DEV_DISPLAY_COLOR_GREEN},
        {'2', DEV_DISPLAY_COLOR_RED},
        {'3', DEV_DISPLAY_COLOR_YELLOW},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buff[APP_AH_NOTIFY_ID_LEN + 1U];
        memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
        buff[APP_AH_NOTIFY_ID_LEN] = cases[i].code;

        g_ah_mqtt_cmd_table[1].handler(&s_proto.base, &s_ccb, buff);

        CHECK_MSG(s_render.calls == (int)(i + 1U), "清屏 %c 应渲染一次", cases[i].code);
        CHECK_MSG(s_render.type == APP_RENDER_TYPE_FILL, "清屏应走 FILL");
        CHECK_MSG(s_render.color == cases[i].color, "清屏 %c 颜色映射不对", cases[i].code);
        CHECK_MSG(_reply_is("0", "Reply/display/clean"), "清屏 %c 应回 '0'", cases[i].code);

        /* 每条命令独立计数 */
        s_tx_calls = 0;
    }
}

/** @brief 清屏颜色越界：不渲染，回 '1' */
static void case_clean_bad(void)
{
    TEST_BEGIN("清屏：颜色越界 → 不渲染，回 '1'");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char buff[APP_AH_NOTIFY_ID_LEN + 1U];
    memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
    buff[APP_AH_NOTIFY_ID_LEN] = '9';

    g_ah_mqtt_cmd_table[1].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_render.calls == 0, "越界颜色不应渲染");
    CHECK_MSG(_reply_is("1", "Reply/display/clean"), "越界应回 '1'");
}

/* ================================================================
 *  重启 / 对时
 * ================================================================ */

/** @brief 重启：先回应答再复位，通知号回显 */
static void case_restart(void)
{
    TEST_BEGIN("重启：应答通知号回显后复位");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char buff[APP_AH_NOTIFY_ID_LEN + 2U];
    memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
    buff[APP_AH_NOTIFY_ID_LEN]      = '0';
    buff[APP_AH_NOTIFY_ID_LEN + 1U] = '\0';

    g_ah_mqtt_cmd_table[2].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(_reply_is("0", "Reply/op/restart"), "重启应答应为 通知号+'0'");
    CHECK_MSG(s_reset_calls == 1, "应调用一次复位");
}

/** @brief 对时 type=1：解析 YYYYMMDDhhmmss 写 RTC */
static void case_checktime_ok(void)
{
    TEST_BEGIN("对时：type=1 解析并写 RTC");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char buff[APP_AH_NOTIFY_ID_LEN + 1U + 14U];
    memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
    buff[APP_AH_NOTIFY_ID_LEN] = '1';
    memcpy(buff + APP_AH_NOTIFY_ID_LEN + 1U, "20250305143200", 14);

    g_ah_mqtt_cmd_table[3].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_rtc_calls == 1, "应写一次 RTC");
    CHECK_MSG(s_rtc_ts == 1741185120U, "RTC 时间应为 1741185120，实际 %u", (unsigned)s_rtc_ts);
    CHECK_MSG(_reply_is("0", "Reply/op/checktime"), "对时应回 '0'");
}

/** @brief 对时 type=0：不写 RTC，仍回 '0' */
static void case_checktime_skip(void)
{
    TEST_BEGIN("对时：type=0 不写 RTC");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char buff[APP_AH_NOTIFY_ID_LEN + 1U + 14U];
    memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
    buff[APP_AH_NOTIFY_ID_LEN] = '0';
    memcpy(buff + APP_AH_NOTIFY_ID_LEN + 1U, "20250305143200", 14);

    g_ah_mqtt_cmd_table[3].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_rtc_calls == 0, "type=0 不应写 RTC");
    CHECK_MSG(_reply_is("0", "Reply/op/checktime"), "type=0 应回 '0'");
}

/** @brief 对时时间非法：不写 RTC，回 '1' */
static void case_checktime_bad(void)
{
    TEST_BEGIN("对时：时间串非法 → 不写 RTC，回 '1'");
    _reset((uint8_t)AH_DEV_LED_BOARD);

    char buff[APP_AH_NOTIFY_ID_LEN + 1U + 14U];
    memcpy(buff, k_notify, APP_AH_NOTIFY_ID_LEN);
    buff[APP_AH_NOTIFY_ID_LEN] = '1';
    memcpy(buff + APP_AH_NOTIFY_ID_LEN + 1U, "20250230000000", 14); /* 2 月没有 30 日 */

    g_ah_mqtt_cmd_table[3].handler(&s_proto.base, &s_ccb, buff);

    CHECK_MSG(s_rtc_calls == 0, "非法时间不应写 RTC");
    CHECK_MSG(_reply_is("1", "Reply/op/checktime"), "非法时间应回 '1'");
}

/* ================================================================ */

int main(void)
{
    case_display_ok();
    case_display_bad_index();
    case_display_other_type();
    case_display_too_long();
    case_clean();
    case_clean_bad();
    case_restart();
    case_checktime_ok();
    case_checktime_skip();
    case_checktime_bad();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
