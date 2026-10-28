/**
 * @file    app_lcsp_cfg.c
 * @brief   LCSP 配置持久化：W25Qxx 配置区记录（由配置调度器管理）
 *
 * 读-改-写的语义全在 `app_cfg_sched` 里（块位扫描、名字认领、内容去重、整记录 CRC32），
 * 本文件只负责"一条记录长什么样"和"没有记录时默认值是什么"。
 */

#include "app_lcsp_cfg.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "initcall.h"

#include "app_cfg_sched.h"

/* 前 14 字节必须与协议 0x02/0x20/0x30 的线上布局逐字节一致（IP/掩码/网关/端口）：
   应答回读与请求解析都是按这 14 字节直接 memcpy 的，成员一挪两边都"看着对"，
   只是配置会以错位的字段落盘 —— 这种错误要等重启不进网才会发现。 */
static_assert(offsetof(app_lcsp_cfg_t, serial) == 14, "配置结构体前 14 字节必须是线上 IP 配置");

static uint8_t        s_cfg_id = 0xFF;
static app_lcsp_cfg_t s_loaded_cfg;
static bool           s_loaded_valid;
static bool           s_load_done;

/** @brief 读一次记录（只在首次使用、或保存后缓存失效时执行）
 *
 *  IO 错误不闩锁（`s_load_done` 不置位）：SPI Flash 偶发读失败时下次还能重试，
 *  闩死会让本次上电周期内配置永久回落默认值，且没有任何补救路径。 */
static void _lcsp_cfg_reload(void)
{
    if (s_load_done)
        return;

    if (0xFF == s_cfg_id) {
        s_load_done = true; /* 未注册（存储不可用/注册满）：不再重试，避免每次命令都走一遍 */
        return;
    }

    s_loaded_valid = false;
    memset(&s_loaded_cfg, 0, sizeof(s_loaded_cfg));

    uint16_t               loaded_len = 0;
    dev_cfg_record_state_t state =
        app_cfg_sched_load(s_cfg_id, (uint8_t *)&s_loaded_cfg, (uint16_t)sizeof(s_loaded_cfg), &loaded_len);

    if (DEV_CFG_RECORD_STATE_IO_ERR != state)
        s_load_done = true;

    if ((DEV_CFG_RECORD_STATE_OK == state) && (sizeof(s_loaded_cfg) == loaded_len))
        s_loaded_valid = true;
}

void app_lcsp_cfg_default(app_lcsp_cfg_t *cfg)
{
    if (nullptr == cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));

    /* 协议 1.1 的出厂值 */
    cfg->ip[0]   = 192;
    cfg->ip[1]   = 168;
    cfg->ip[2]   = 1;
    cfg->ip[3]   = 100;
    cfg->mask[0] = 255;
    cfg->mask[1] = 255;
    cfg->mask[2] = 255;
    cfg->mask[3] = 0;
    cfg->gw[0]   = 192;
    cfg->gw[1]   = 168;
    cfg->gw[2]   = 1;
    cfg->gw[3]   = 1;
    cfg->port[0] = (uint8_t)(LCSP_PORT_DEFAULT >> 8);
    cfg->port[1] = (uint8_t)(LCSP_PORT_DEFAULT & 0xFFU);

    memcpy(cfg->serial, LCSP_SERIAL_DEFAULT, LCSP_SERIAL_LEN);

    cfg->brightness = LCSP_BRIGHTNESS_DEFAULT;
    cfg->oe_enabled = 1U;
}

bool app_lcsp_cfg_load(app_lcsp_cfg_t *cfg)
{
    if (nullptr == cfg)
        return false;

    _lcsp_cfg_reload();

    if (!s_loaded_valid) {
        memset(cfg, 0, sizeof(*cfg));
        return false;
    }

    memcpy(cfg, &s_loaded_cfg, sizeof(*cfg));
    return true;
}

int32_t app_lcsp_cfg_save(const app_lcsp_cfg_t *cfg)
{
    if (nullptr == cfg)
        return -1;

    if (0xFF == s_cfg_id) {
        printf("[lcsp_cfg] 配置所有者未注册，本次写入丢弃\n");
        return -1;
    }

    int32_t state = app_cfg_sched_save(s_cfg_id, (const uint8_t *)cfg, (uint16_t)sizeof(*cfg));
    if (0 != state) {
        printf("[lcsp_cfg] 保存失败（%u 字节）\n", (unsigned)sizeof(*cfg));
        return state;
    }

    s_load_done = false; /* 让下一次 load 从 Flash 读回（0x02 的应答要的是读回值）*/
    return 0;
}

static const app_cfg_sched_desc_t s_lcsp_cfg_desc = {
    .name    = "lcsp_cfg",
    .version = 1,
    .load    = nullptr, /* 不参与启动加载遍：上电应用在 app_lcsp.c 的 _lcsp_apply_cfg 里做 */
};

static void _lcsp_cfg_init(void)
{
    s_cfg_id = app_cfg_sched_register(&s_lcsp_cfg_desc);
    if (0xFF == s_cfg_id)
        printf("[lcsp_cfg] 配置所有者注册失败，本次上电配置不生效\n");
}
sw_dev_initcall(_lcsp_cfg_init);
