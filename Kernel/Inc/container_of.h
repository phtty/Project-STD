/**
 * @file    container_of.h
 * @brief   从成员指针找回包含它的结构体 — 平台无关工具宏
 */
#pragma once

#include <stddef.h>

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr)-offsetof(type, member)))
