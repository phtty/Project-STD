/**
 * @file    net_diag.h
 * @brief   网络诊断日志 —— 定位"上电后首次连接失败"用的插桩
 *
 * 要回答的三个问题，缺一个就定不了位：
 *   1. 链路什么时候起来？（排除/确认协商时序）
 *   2. UDP 什么时候开始在听？（排除"还没 bind 就发过来了"）
 *   3. **设备到底有没有收到那个请求、有没有回？**
 *      有收有回而上位机没收到 → 问题在下行路径（VM/路由/ARP）
 *      压根没收到         → 问题在上行路径（对端根本没发到设备）
 *
 * 输出经 printf → RTT。带 tick 时间戳（ms，自 RTOS 启动起算），
 * 所以"首次成功离链路起来有多远"是直接读出来的，不用推。
 *
 * **定位完请把 NET_DIAG_ENABLE 置 0** —— 每包一条 printf 在正常流量下太吵，
 * 而且 udp 通道任务栈只有 1KB，printf 要吃掉一部分。
 */

#pragma once

/* **默认关闭。** 排查网络问题时临时置 1，用完改回 0。
 * 2026-09-18 那轮"上电首次连接失败"就是靠它定位的 —— 结论是设备侧清白
 * （20/20 ping 同毫秒回复），丢包在虚拟化层。详见提交信息。 */
#ifndef NET_DIAG_ENABLE
#define NET_DIAG_ENABLE 0
#endif

#if NET_DIAG_ENABLE

#include <stdio.h>
#include "cmsis_os2.h"

/* __VA_OPT__：让宏在"只有一句结论、没有附加数值"时也能用（C23）。
   直接写 __VA_ARGS__ 的话 NET_DIAG("link DOWN") 会展开成 printf(..., ,) 编译不过。 */
#define NET_DIAG(fmt, ...)                                                                         \
    printf("[net] %6lums " fmt "\n", (unsigned long)osKernelGetTickCount() __VA_OPT__(, ) __VA_ARGS__)
#else
#define NET_DIAG(fmt, ...) ((void)0)
#endif
