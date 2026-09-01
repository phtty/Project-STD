/**
 * @file    app_cq_proto_parse.c
 * @brief   重庆高速二代费显协议（CQ）帧解析（纯解析：不渲染、不碰外设、不发包）
 *
 * JSON 帧：cJSON_Parse 建树（钩子已在 cq_proto_init 换绑 FreeRTOS 堆），
 * 按 "cmd" 字段提取参数；文本字段（GB2312 字节）拷贝到内部静态缓冲后删树，
 * 返回的 cq_parsed_cmd_t 指针在下次解析前有效（单消费者任务）。
 * 二进制帧：12 字节精确匹配重启 / 搜索两帧。
 */

#include "app_cq_proto_parse.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ---- 类型安全提取宏（JSON 对象字段）----
 * cJSON 官方 v1.7.18：类型判定统一走 cJSON_IsString/IsNumber/IsObject 辅助函数；
 * cJSON_GetObjectItem 入参为 const cJSON *。 */
#define CQ_JSON_GET_STR(root, key)                                                    \
    ({                                                                                \
        cJSON *_cq_item = cJSON_GetObjectItem((root), (key));                         \
        (_cq_item && cJSON_IsString(_cq_item)) ? _cq_item->valuestring : NULL;        \
    })

#define CQ_JSON_GET_INT(root, key, default_val)                                       \
    ({                                                                                \
        cJSON *_cq_item = cJSON_GetObjectItem((root), (key));                         \
        (_cq_item && cJSON_IsNumber(_cq_item)) ? _cq_item->valueint                  \
                                               : (default_val);                       \
    })

#define CQ_JSON_GET_OBJ(root, key)                                                    \
    ({                                                                                \
        cJSON *_cq_item = cJSON_GetObjectItem((root), (key));                         \
        (_cq_item && cJSON_IsObject(_cq_item)) ? _cq_item : NULL;                     \
    })

/* ---- 内部静态缓冲（单消费者任务，非重入；置 CCMRAM 省 SRAM，CPU 独占无 DMA）---- */
static char s_cq_json_buf[CQ_PAYLOAD_MAX + 1U] __attribute__((section(".ccmram"))); /* NUL 结尾 JSON 拷贝 */
static char s_cq_text_buf[CQ_PAYLOAD_MAX] __attribute__((section(".ccmram")));      /* 命令文本字段拷贝区 */

/* ---- 命令名 → 枚举 ---- */
static const struct {
    const char *name;
    cq_pcmd_t cmd;
} s_cq_cmd_names[] = {
    {"text1", CQ_PCMD_TEXT1},
    {"tra1", CQ_PCMD_TRA1},
    {"pic1", CQ_PCMD_PIC1},
    {"light1", CQ_PCMD_LIGHT1},
    {"voice_control1", CQ_PCMD_VOICE_CONTROL1},
    {"voice1", CQ_PCMD_VOICE1},
    {"voice_play1", CQ_PCMD_VOICE_PLAY1},
    {"warn1", CQ_PCMD_WARN1},
    {"syn1", CQ_PCMD_SYN1},
    {"setip", CQ_PCMD_SETIP},
    {"screen", CQ_PCMD_SCREEN},
    {"full", CQ_PCMD_FULL},
};

/**
 * @brief  命令名字符串 → 命令枚举。
 */
static cq_pcmd_t _cq_lookup_cmd(const char *name)
{
    if (name == NULL)
        return CQ_PCMD_INVALID;
    for (size_t i = 0; i < sizeof(s_cq_cmd_names) / sizeof(s_cq_cmd_names[0]); i++) {
        if (strcmp(name, s_cq_cmd_names[i].name) == 0)
            return s_cq_cmd_names[i].cmd;
    }
    return CQ_PCMD_INVALID;
}

/**
 * @brief  点分十进制 IPv4 字符串 → 4 字节数组（严格校验：每段 0~255，恰好 4 段）。
 * @return true 解析成功。
 */
static bool _cq_parse_ip4(const char *s, uint8_t out[4])
{
    if (s == NULL)
        return false;
    for (int o = 0; o < 4; o++) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10U + (uint32_t)(*s - '0');
            if (v > 255U)
                return false;
            s++;
            digits++;
        }
        if (digits == 0)
            return false;
        out[o] = (uint8_t)v;
        if (o < 3) {
            if (*s != '.')
                return false;
            s++;
        }
    }
    return *s == '\0';
}

/**
 * @brief  字号字符串 "16"/"24"/"32" → 像素值；非法回默认 24。
 */
static uint8_t _cq_font_from_str(const char *s)
{
    if (s == NULL)
        return 24;
    if (strcmp(s, "16") == 0)
        return 16;
    if (strcmp(s, "24") == 0)
        return 24;
    if (strcmp(s, "32") == 0)
        return 32;
    return 24; /* 非法字号回默认 24 */
}

/**
 * @brief  颜色字符串 "r"/"g"/"y" → 0红/1绿/2黄；非法回默认绿(1)。
 */
static uint8_t _cq_color_from_str(const char *s)
{
    if (s == NULL)
        return 1;
    if (s[0] == 'r')
        return 0;
    if (s[0] == 'g')
        return 1;
    if (s[0] == 'y')
        return 2;
    return 1; /* 非法颜色回默认绿 */
}

/**
 * @brief  拷贝文本到内部静态缓冲（越界即失败）。
 * @return 成功返回缓冲内指针，失败返回 NULL。
 */
static char *_cq_text_dup(const char *src, size_t *used, size_t len)
{
    if (src == NULL || *used + len > sizeof(s_cq_text_buf))
        return NULL;
    char *dst = &s_cq_text_buf[*used];
    memcpy(dst, src, len);
    *used += len;
    return dst;
}

/**
 * @brief  提取 text1：scr + ln0~ln7（s 字号 / c 颜色 / t 文本）。
 */
static void _cq_parse_text1(cJSON *root, cq_parsed_cmd_t *cmd)
{
    cJSON *scr = cJSON_GetObjectItem(root, "scr");
    cmd->p.text1.scr_clear = (scr && cJSON_IsNumber(scr) && scr->valueint == 0);

    size_t used = 0;
    for (uint8_t i = 0; i < 8; i++) {
        char key[8];
        snprintf(key, sizeof(key), "ln%u", (unsigned)i);
        cJSON *ln = CQ_JSON_GET_OBJ(root, key);
        if (ln == NULL)
            continue;

        const char *t = CQ_JSON_GET_STR(ln, "t");
        if (t == NULL)
            continue; /* 无文本 → 该行跳过 */

        size_t tlen = strlen(t);
        char *dup    = _cq_text_dup(t, &used, tlen);
        if (dup == NULL) {
            cmd->sta = CQ_PARSE_ERR_PARAM; /* 文本总量超限 */
            return;
        }
        cq_line_t *L       = &cmd->p.text1.ln[i];
        L->valid           = true;
        L->font            = _cq_font_from_str(CQ_JSON_GET_STR(ln, "s"));
        L->color           = _cq_color_from_str(CQ_JSON_GET_STR(ln, "c"));
        L->text            = dup;
        L->text_len        = (uint16_t)tlen;
    }
}

/**
 * @brief  解析 CQ 原始帧。
 */
cq_parsed_cmd_t cq_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    cq_parsed_cmd_t cmd = {0};
    cmd.sta             = CQ_PARSE_ERR_FRAME;
    if (raw == NULL || raw_len == 0U)
        return cmd;

    /* ---- 12B 二进制帧精确判别 ---- */
    if (raw[0] == 0xFF) {
        if (raw_len < 12U)
            return cmd;
        if (memcmp(raw, cq_bin_reboot_req, 12) == 0) {
            cmd.cmd = CQ_PCMD_BIN_REBOOT;
            cmd.sta = CQ_PARSE_OK;
        } else if (memcmp(raw, cq_bin_search_req, 12) == 0) {
            cmd.cmd = CQ_PCMD_BIN_SEARCH;
            cmd.sta = CQ_PARSE_OK;
        }
        return cmd;
    }

    /* ---- JSON 帧 ---- */
    if (raw[0] != '{' || raw_len > CQ_PAYLOAD_MAX)
        return cmd;

    memcpy(s_cq_json_buf, raw, raw_len);
    s_cq_json_buf[raw_len] = '\0';

    cJSON *root = cJSON_Parse(s_cq_json_buf);
    if (root == NULL)
        return cmd;

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (cmd_item == NULL || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        return cmd;
    }
    cmd.cmd = _cq_lookup_cmd(cmd_item->valuestring);
    if (cmd.cmd == CQ_PCMD_INVALID) {
        cJSON_Delete(root);
        cmd.sta = CQ_PARSE_ERR_CMD;
        return cmd;
    }

    /* 各命令参数提取（默认 OK，非法置 ERR_PARAM） */
    cmd.sta = CQ_PARSE_OK;
    switch (cmd.cmd) {
        case CQ_PCMD_TEXT1:
            _cq_parse_text1(root, &cmd);
            break;

        case CQ_PCMD_TRA1: {
            const char *turn = CQ_JSON_GET_STR(root, "turn");
            if (turn == NULL || (turn[0] != 'r' && turn[0] != 'g'))
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.tra1.turn = (turn[0] == 'g') ? 1U : 0U;
            break;
        }

        case CQ_PCMD_PIC1: /* stub：解析后忽略 */
            break;

        case CQ_PCMD_LIGHT1: {
            int level = CQ_JSON_GET_INT(root, "level", -1);
            if (level < 0 || level > 15)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.light1.level = level;
            break;
        }

        case CQ_PCMD_VOICE_CONTROL1: {
            int level = CQ_JSON_GET_INT(root, "level", -1);
            if (level < 0 || level > 15)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.voice_ctrl1.level = level;
            break;
        }

        case CQ_PCMD_VOICE1: {
            const char *t = CQ_JSON_GET_STR(root, "t");
            if (t == NULL) {
                cmd.sta = CQ_PARSE_ERR_PARAM;
                break;
            }
            size_t tlen = strlen(t);
            size_t used = 0;
            char *dup   = _cq_text_dup(t, &used, tlen);
            if (dup == NULL || tlen > UINT16_MAX) {
                cmd.sta = CQ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.voice1.text     = dup;
            cmd.p.voice1.text_len = (uint16_t)tlen;
            break;
        }

        case CQ_PCMD_VOICE_PLAY1: /* stub：解析后忽略 */
            break;

        case CQ_PCMD_WARN1: {
            int second = CQ_JSON_GET_INT(root, "second", -9999);
            if (second < -1)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.warn1.second = second;
            break;
        }

        case CQ_PCMD_SYN1: /* 心跳帧：无参数 */
            break;

        case CQ_PCMD_SETIP: {
            const char *ip   = CQ_JSON_GET_STR(root, "ip");
            const char *mask = CQ_JSON_GET_STR(root, "mask");
            const char *wg   = CQ_JSON_GET_STR(root, "wg");
            int port         = CQ_JSON_GET_INT(root, "port", -1);
            if (!_cq_parse_ip4(ip, cmd.p.setip.ip) ||
                !_cq_parse_ip4(mask, cmd.p.setip.mask) ||
                !_cq_parse_ip4(wg, cmd.p.setip.gw) ||
                port < 1 || port > 65535)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.setip.port = (uint32_t)port;
            break;
        }

        case CQ_PCMD_SCREEN: {
            int led = CQ_JSON_GET_INT(root, "led", -1);
            if (led < 0)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.screen.led = led;
            break;
        }

        case CQ_PCMD_FULL: {
            int color = CQ_JSON_GET_INT(root, "color", -1);
            if (color < 0 || color > 3)
                cmd.sta = CQ_PARSE_ERR_PARAM;
            else
                cmd.p.full.color = color;
            break;
        }

        default:
            cmd.sta = CQ_PARSE_ERR_CMD;
            break;
    }

    cJSON_Delete(root);
    return cmd;
}