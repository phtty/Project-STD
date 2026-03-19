#pragma once

#include "main.h"
#include "lwip.h"

#include "iap.h"
#include "msg.h"
#include "udp_app.h"
#include "config_info.h"

#define FLAG_FORCE_UPDATE      (uint32_t)(0x0000DEADU)
#define FIRMWARE_MAX_FRAME_NUM (768U)
#define FLASH_SECTOR_SIZE      (0x20000U)

#define FIRMWARE_MAXLEN        (FIRMWARE_MAX_FRAME_NUM * 256U)
#define BITMAP_SIZE            (FIRMWARE_MAX_FRAME_NUM / 8)

typedef enum {
    rtn_cmd01 = (uint32_t)0x0000b401U,
    rtn_cmd02 = (uint32_t)0x0000b402U,
    rtn_cmd03 = (uint32_t)0x0000b403U,
    rtn_cmd04 = (uint32_t)0x0000b404U,
    rtn_cmd05 = (uint32_t)0x0000b405U,
    rtn_cmd06 = (uint32_t)0x0000b406U,
    rtn_cmd07 = (uint32_t)0x0000b407U,
} rtn_cmd_t;

typedef struct iap_ipconfig {
    ip4_addr_t ip;
    ip4_addr_t mask;
    ip4_addr_t gw;
    uint16_t port;
} iap_ipconfig_t;

typedef void (*pfcmd_Functions)(MsgQueueItem_t *, IAP_Frame_t *);
extern const pfcmd_Functions pfIAP_CMD[];
