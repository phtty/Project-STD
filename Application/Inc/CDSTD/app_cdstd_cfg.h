#pragma once

/**
 * @file    app_cdstd_cfg.h
 *
 * @brief   CDSTD 配置持久化：W25Qxx 配置区记录格式与读写接口
 */

#include "stdint.h"

typedef struct [[gnu::packed]] {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint8_t port[2];
} app_cdstd_net_cfg_t;

/* 载荷长度是持久化契约：记录头里的 len 与实际结构体不符时，读回会静默错位。
   14 字节 = IP(4) + 掩码(4) + 网关(4) + 端口(2)，与 0xA5 请求 / 0xB6 应答一致。 */
static_assert(sizeof(app_cdstd_net_cfg_t) == 14, "与 0xA5/0xB6 载荷 14 字节不符");

bool app_cdstd_load_config(app_cdstd_net_cfg_t *info);

int32_t app_cdstd_save_config(const app_cdstd_net_cfg_t *info);
