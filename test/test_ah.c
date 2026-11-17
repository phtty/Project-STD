/**
 * @file    test_ah.c
 * @brief   AH 协议族纯函数：hex 编码 / 主题前缀 / Client ID / 通知号
 *
 * **为什么需要这个测试**：这四段字符串是 AH/MQTT 与平台之间的“地址”：
 * 主题前缀错一位，平台就收不到/发不出；通知号错一位，平台无法把应答和请求对上。
 * 它们都是纯格式化，最容易在“从别的工程移植”时留下大小写、位数、回卷的偏差，
 * 所以在 host 上逐字符钉死，而不是等上机抓包。
 *
 * 被测代码是生产 TU `Application/Src/AH/app_ah.c` 本体，不复制实现。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_ah.h"
#include "app_ah_cfg.h"

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

/* ================================================================ */

/** @brief 构造一份固定身份的配置（站 52011314、道 01、类型 32H、序号 01） */
static app_ah_cfg_t _cfg_make(void)
{
    app_ah_cfg_t cfg = {0};
    const uint8_t lane[5] = {0x52U, 0x01U, 0x13U, 0x14U, 0x01U};

    memcpy(cfg.lane_hex, lane, sizeof(lane));
    cfg.device_id = (uint8_t)AH_DEV_LED_BOARD;
    cfg.device_no = 0x01U;
    return cfg;
}

/** @brief hex 编码：大写、位数、容量边界与空指针 */
static void case_hex_encode(void)
{
    TEST_BEGIN("hex 编码：大写、精确容量、空指针");

    const uint8_t data[5] = {0x52U, 0x01U, 0x13U, 0x14U, 0x01U};
    char          out[16] = {0};

    CHECK_MSG(app_ah_hex_encode(out, sizeof(out), data, sizeof(data)), "5 字节应编码成功");
    CHECK_MSG(strcmp(out, "5201131401") == 0, "期望 5201131401，得到 %s", out);

    /* 容量正好 11：10 个字符 + NUL */
    char exact[11] = {0};
    CHECK_MSG(app_ah_hex_encode(exact, sizeof(exact), data, sizeof(data)), "容量 11 应成功");
    CHECK_MSG(strcmp(exact, "5201131401") == 0, "容量 11 结果不对：%s", exact);

    /* 容量差 1：必须失败并写空串，不能写穿 */
    char small[10];
    memset(small, 'X', sizeof(small));
    CHECK_MSG(!app_ah_hex_encode(small, sizeof(small), data, sizeof(data)), "容量 10 应失败");
    CHECK_MSG(small[0] == '\0', "失败时应写空串，首字节为 0x%02X", (unsigned)(uint8_t)small[0]);

    CHECK_MSG(!app_ah_hex_encode(nullptr, 16, data, sizeof(data)), "out 为空应失败");
    CHECK_MSG(!app_ah_hex_encode(out, sizeof(out), nullptr, 0), "data 为空应失败");

    /* 0 字节：只要容量 ≥ 1 就是合法的空串 */
    CHECK_MSG(app_ah_hex_encode(out, sizeof(out), data, 0), "0 字节应成功");
    CHECK_MSG(out[0] == '\0', "0 字节结果应为空串");
}

/** @brief 主题前缀：`站8/道2/型2/号2`，容量 17 必须失败（需要 18） */
static void case_topic_prefix(void)
{
    TEST_BEGIN("主题前缀：站/道/类型/序号的位数与分隔符");

    app_ah_cfg_t cfg = _cfg_make();
    char         out[APP_AH_TOPIC_PREFIX_MAX] = {0};

    CHECK_MSG(app_ah_topic_prefix_build(&cfg, out, sizeof(out)), "合法配置应成功");
    CHECK_MSG(strcmp(out, "52011314/01/32/01") == 0, "期望 52011314/01/32/01，得到 %s", out);

    char small[17] = {0};
    CHECK_MSG(!app_ah_topic_prefix_build(&cfg, small, sizeof(small)), "容量 17（差 NUL）应失败");

    cfg.device_id = 0U;
    CHECK_MSG(!app_ah_topic_prefix_build(&cfg, out, sizeof(out)), "类型为 0 应失败");

    CHECK_MSG(!app_ah_topic_prefix_build(nullptr, out, sizeof(out)), "cfg 为空应失败");
}

/** @brief Client ID：`CD` + 站8 + 道2 + 型2 + 号2 */
static void case_client_id(void)
{
    TEST_BEGIN("Client ID：CD + 身份四段，且容量边界正确");

    app_ah_cfg_t cfg = _cfg_make();
    char         out[APP_AH_CLIENT_ID_MAX] = {0};

    CHECK_MSG(app_ah_client_id_build(&cfg, out, sizeof(out)), "合法配置应成功");
    CHECK_MSG(strcmp(out, "CD52011314013201") == 0, "期望 CD52011314013201，得到 %s", out);

    char small[16] = {0};
    CHECK_MSG(!app_ah_client_id_build(&cfg, small, sizeof(small)), "容量 16（差 NUL）应失败");
}

/** @brief 通知号：UTC 时间格式化、6 位计数回卷、未对时的全 0 日期 */
static void case_notify(void)
{
    TEST_BEGIN("通知号：YYYYMMDDhh + 类型/序号 + 6 位自增");

    app_ah_cfg_t      cfg = _cfg_make();
    app_ah_notify_id_t id;

    /* 2025-03-05 14:00:00 UTC */
    CHECK_MSG(app_ah_notify_build(&id, 1741183200U, &cfg, 7U), "合法时间应成功");
    CHECK_MSG(memcmp(&id, "20250305143201000007", APP_AH_NOTIFY_ID_LEN) == 0, "通知号不对");

    /* 序号按 1000000 回卷：1000007 与 7 必须相同 */
    app_ah_notify_id_t id_wrap;
    CHECK_MSG(app_ah_notify_build(&id_wrap, 1741183200U, &cfg, 1000007U), "回卷应成功");
    CHECK_MSG(memcmp(&id, &id_wrap, APP_AH_NOTIFY_ID_LEN) == 0, "1000007 应回卷为 000007");

    /* 时间未设置：日期段全 0，不能把 1970 当真实时间发给平台 */
    CHECK_MSG(app_ah_notify_build(&id, 0U, &cfg, 0U), "时间为 0 应成功");
    CHECK_MSG(memcmp(&id, "00000000003201000000", APP_AH_NOTIFY_ID_LEN) == 0,
              "时间为 0 时日期段应全 0");

    /* 设备序号 0x0A 必须是两位 hex，不能退化成 1 位 */
    cfg.device_no = 0x0AU;
    CHECK_MSG(app_ah_notify_build(&id, 0U, &cfg, 0U), "序号 0x0A 应成功");
    CHECK_MSG(memcmp(&id, "0000000000320A000000", APP_AH_NOTIFY_ID_LEN) == 0, "序号应为 0A");

    cfg.device_id = 0U;
    CHECK_MSG(!app_ah_notify_build(&id, 0U, &cfg, 0U), "类型为 0 应失败");
    CHECK_MSG(!app_ah_notify_build(nullptr, 0U, &cfg, 0U), "id 为空应失败");
}

/** @brief `YYYYMMDDhhmmss` → Unix：合法值、闰年、边界与非法串 */
static void case_unix_from_datetime(void)
{
    TEST_BEGIN("对时字符串：UTC 解析、闰年、非法输入");

    uint32_t ts = 0U;
    CHECK_MSG(app_ah_unix_from_datetime("20250305143200", 14U, &ts), "合法时间应成功");
    CHECK_MSG(ts == 1741185120U, "2025-03-05 14:32:00 应为 1741185120，实际 %u", (unsigned)ts);

    CHECK_MSG(app_ah_unix_from_datetime("19700101000000", 14U, &ts), "Unix 原点应成功");
    CHECK_MSG(ts == 0U, "1970-01-01 00:00:00 应为 0");

    /* 2024 是闰年：2 月 29 日合法 */
    CHECK_MSG(app_ah_unix_from_datetime("20240229000000", 14U, &ts), "闰年 2 月 29 日应成功");
    CHECK_MSG(ts == 1709164800U, "2024-02-29 00:00:00 应为 1709164800，实际 %u", (unsigned)ts);

    /* 2023 不是闰年：2 月 29 日非法 */
    CHECK_MSG(!app_ah_unix_from_datetime("20230229000000", 14U, &ts), "平年 2 月 29 日应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("20251301000000", 14U, &ts), "月份 13 应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("20250101240000", 14U, &ts), "小时 24 应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("20250101235960", 14U, &ts), "秒 60 应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("2025O101000000", 14U, &ts), "非数字应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("2025010100000", 14U, &ts), "长度 13 应失败");
    CHECK_MSG(!app_ah_unix_from_datetime("20250101000000", 13U, &ts), "len=13 应失败");
    CHECK_MSG(!app_ah_unix_from_datetime(nullptr, 14U, &ts), "空指针应失败");
}

/* ================================================================ */

int main(void)
{
    case_hex_encode();
    case_topic_prefix();
    case_client_id();
    case_notify();
    case_unix_from_datetime();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
