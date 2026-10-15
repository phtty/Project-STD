#pragma once

/**
 * @file    app_cdstd.h
 * @brief   折叠屏创迪标准协议：帧格式、协议控制块与任务接口
 */

#include <stdint.h>
#include "cmsis_os2.h"

#include "app_dispatch.h"
#include "ring_buffer.h"

#define CDSTD_HEAD (0xFFFFU)

typedef struct [[gnu::packed]] {
    uint8_t head[2];
    uint8_t ver;
    uint8_t seq;
    uint8_t len[4];
    uint8_t cmd;
    uint8_t data_crc[];
} app_cdstd_frame_t;
