/**
 * @file    container_of.h
 * @brief   从成员指针找回包含它的结构体 — 平台无关工具宏
 */
#pragma once

#include <stddef.h>

/** @brief 由成员指针反推其宿主结构体指针
 *  @param ptr    指向结构体成员的指针
 *  @param type   宿主结构体类型
 *  @param member 成员在结构体中的名字 */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr)-offsetof(type, member)))
