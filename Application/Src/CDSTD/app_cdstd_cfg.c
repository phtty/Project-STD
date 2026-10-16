/**
 * @file    app_cdstd_cfg.c
 *
 * @brief   CDSTD 配置持久化：W25Qxx尾部配置区接口实现（由配置调度器管理）
 */

#include "app_cdstd_cfg.h"

#include <stdio.h>

#include "initcall.h"

#include "app_cfg_sched.h"

static uint8_t s_cdstd_id               = 0xFF;
static app_cdstd_net_cfg_t s_loaded_cfg = {0};
static bool s_loaded_valid              = false;
static bool s_load_done                 = false;

static void _cdstd_load(void)
{
    // 存配置入口检查两个守卫，一个去重，一个判配置是否有效
    if (s_load_done)
        return;

    if (0xFF == s_cdstd_id) {
        s_load_done = true;
        return;
    }

    s_loaded_valid = false;
    memset(&s_loaded_cfg, 0, sizeof(s_loaded_cfg));

    uint16_t loaded_len          = 0;
    dev_cfg_record_state_t state = app_cfg_sched_load(s_cdstd_id, (uint8_t *)&s_loaded_cfg, sizeof(s_loaded_cfg), &loaded_len);

    if (DEV_CFG_RECORD_STATE_IO_ERR != state)
        s_load_done = true;

    if (DEV_CFG_RECORD_STATE_OK == state && sizeof(s_loaded_cfg) == loaded_len)
        s_loaded_valid = true;
}

/**
 * @brief 加载 CDSTD 的配置信息
 *
 * @param info CDSTD 配置结构体
 * @return true
 * @return false
 */
bool app_cdstd_load_config(app_cdstd_net_cfg_t *info)
{
    // 读配置去重，若已经读过，则把配置缓存拷贝过去，只有无法读出时，才返回失败
    if (!s_load_done)
        _cdstd_load();

    // 判是否正常读出，若未能读出，则返回失败
    if (!s_loaded_valid) {
        memset(info, 0, sizeof(*info));
        return false;
    }

    memcpy(info, &s_loaded_cfg, sizeof(app_cdstd_net_cfg_t));
    return true;
}

int32_t app_cdstd_save_config(const app_cdstd_net_cfg_t *info)
{
    int32_t state = app_cfg_sched_save(s_cdstd_id, (const uint8_t *)info, sizeof(*info));
    if (state != 0) {
        printf("[cdstd_cfg] 保存失败（%u 字节）\n", (unsigned)sizeof(*info));
        return state;
    }

    s_load_done = false;
    return state;
}

static const app_cfg_sched_desc_t s_cdstd_cfg_desc = {
    .name    = "cdstd_cfg",
    .version = 0,
    .load    = _cdstd_load,
};

static void _app_cdstd_cfg_init(void)
{
    s_cdstd_id = app_cfg_sched_register(&s_cdstd_cfg_desc);
    if (s_cdstd_id == 0xFF)
        printf("[cdstd_cfg] 配置所有者注册失败，本次上电配置不生效\n");
}
sw_dev_initcall(_app_cdstd_cfg_init);