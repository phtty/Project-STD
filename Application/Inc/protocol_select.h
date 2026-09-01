/**
 * @file    protocol_select.h
 * @brief   [已废弃] 旧协议切换头文件
 *
 * @deprecated 本文件已被废弃。协议切换方式已改为：
 *   1. EIDE 工程目录包含/排除协议模块文件夹（如 LDI/、ProtocolParser_QingHai/）
 *   2. 协议模块通过 sw_app_initcall 自注册到 app_dispatch 框架
 *   3. 框架零分支、零宏切换
 *
 * @see doc/05_协议模块多协议兼容优化/01_architecture.md — 协议模块组织与工程编入
 * @see doc/05_协议模块多协议兼容优化/02_as_built_status.md — 落地状态
 *
 * 请勿在新代码中 include 本文件。如仍有引用请逐步清理。
 */

#pragma once

#warning "protocol_select.h 已废弃：请使用 EIDE 目录编入方式切换协议，勿 include 本文件"
