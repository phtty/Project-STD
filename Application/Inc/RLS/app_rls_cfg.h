#pragma once

#include "app_rls.h"

#define DEV_FLASH_RLS_MAGIC (0x114514U)

typedef struct {
    uint32_t magic;
    uint8_t bitmap[512];
} dev_flash_rls_record_t;
