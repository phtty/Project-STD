#pragma once

#include "main.h"

#include "protocol.h"

extern uint8_t ucCmdCmdIndex;

// 显示控制指令
typedef struct cmd_disp {
    notify_id_t nid;
    char scrn_sw;
    char color;
    char size;
    char text[];
} __packed cmd_display_t;

// 屏幕填充
typedef struct cmd_fill {
    notify_id_t nid;
    char color;
} __packed cmd_fill_t;

// 重启
typedef struct cmd_restart {
    notify_id_t nid;
    char type;
    char set;
    char time[14];
    char reserve;
} __packed cmd_restart_t;

// 对时
typedef struct cmd_checktime {
    notify_id_t nid;
    char type;
    char time[14];
} __packed cmd_checktime_t;

typedef void (*CmdFunc_t)(char *);
extern const CmdFunc_t pfCmdFunc[];
