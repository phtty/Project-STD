#pragma once

#include <stdint.h>

typedef enum {
    APP_PROTOCOL_LDI = 1,
    APP_PROTOCOL_QINGHAI = 2,
} app_protocol_t;

#ifndef APP_PROTOCOL
#define APP_PROTOCOL APP_PROTOCOL_LDI
#endif

static inline app_protocol_t app_protocol_current(void)
{
    return (app_protocol_t)APP_PROTOCOL;
}

static inline uint8_t app_protocol_enabled(app_protocol_t proto)
{
    return (app_protocol_current() == proto) ? 1U : 0U;
}
