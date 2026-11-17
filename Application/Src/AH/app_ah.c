/**
 * @file    app_ah.c
 * @brief   AH 协议族纯函数：hex 编码、主题前缀、Client ID、通知号
 *
 * 这里只做**无状态格式化**：不碰 Flash、不碰任务、不读全局配置。
 * 输入是 `app_ah_cfg_t` 的一份拷贝，输出写调用方缓冲；因此可以在 host 上直接测。
 */

#include "app_ah.h"

#include "app_ah_cfg.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/** @brief 把配置里的身份四段拆成 hex 字符串（站 8 / 道 2 / 型 2 / 号 2） */
static bool _ah_hex_parts(const app_ah_cfg_t *cfg, char station[9], char lane[3], char type[3],
                          char no[3])
{
    if (cfg == nullptr || cfg->device_id == 0U)
        return false;

    if (!app_ah_hex_encode(station, 9U, cfg->lane_hex, 4U))
        return false;
    if (!app_ah_hex_encode(lane, 3U, &cfg->lane_hex[4], 1U))
        return false;
    if (!app_ah_hex_encode(type, 3U, &cfg->device_id, 1U))
        return false;
    if (!app_ah_hex_encode(no, 3U, &cfg->device_no, 1U))
        return false;
    return true;
}

bool app_ah_hex_encode(char *out, size_t cap, const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789ABCDEF";

    if (out == nullptr || data == nullptr)
        return false;
    if (len > (SIZE_MAX - 1U) / 2U) {
        if (cap > 0U)
            out[0] = '\0';
        return false;
    }
    if (cap < len * 2U + 1U) {
        if (cap > 0U)
            out[0] = '\0';
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        out[i * 2U]      = digits[data[i] >> 4];
        out[i * 2U + 1U] = digits[data[i] & 0x0FU];
    }
    out[len * 2U] = '\0';
    return true;
}

bool app_ah_topic_prefix_build(const app_ah_cfg_t *cfg, char *out, size_t cap)
{
    if (out == nullptr || cap < APP_AH_TOPIC_PREFIX_MAX)
        return false;

    char station[9];
    char lane[3];
    char type[3];
    char no[3];
    if (!_ah_hex_parts(cfg, station, lane, type, no))
        return false;

    int n = snprintf(out, cap, "%s/%s/%s/%s", station, lane, type, no);
    return n > 0 && (size_t)n < cap;
}

bool app_ah_client_id_build(const app_ah_cfg_t *cfg, char *out, size_t cap)
{
    if (out == nullptr || cap < APP_AH_CLIENT_ID_MAX)
        return false;

    char station[9];
    char lane[3];
    char type[3];
    char no[3];
    if (!_ah_hex_parts(cfg, station, lane, type, no))
        return false;

    int n = snprintf(out, cap, "CD%s%s%s%s", station, lane, type, no);
    return n > 0 && (size_t)n < cap;
}

bool app_ah_notify_build(app_ah_notify_id_t *id, uint32_t unix_ts, const app_ah_cfg_t *cfg,
                         uint32_t send_seq)
{
    if (id == nullptr || cfg == nullptr || cfg->device_id == 0U)
        return false;

    char     text[APP_AH_NOTIFY_ID_LEN + 1U];
    uint32_t seq = send_seq % 1000000U;
    int      n;

    if (unix_ts == 0U) {
        /* 时间未设置：日期段写 0，避免把 1970 当成真实时间发给平台 */
        n = snprintf(text, sizeof(text), "0000000000%02X%02X%06u", (unsigned)cfg->device_id,
                     (unsigned)cfg->device_no, (unsigned)seq);
    } else {
        time_t    ts = (time_t)unix_ts;
        struct tm tm;
        if (gmtime_r(&ts, &tm) == nullptr)
            return false;

        n = snprintf(text, sizeof(text), "%04d%02d%02d%02d%02X%02X%06u", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, (unsigned)cfg->device_id,
                     (unsigned)cfg->device_no, (unsigned)seq);
    }

    if (n != (int)APP_AH_NOTIFY_ID_LEN)
        return false;

    memcpy(id, text, APP_AH_NOTIFY_ID_LEN);
    return true;
}
