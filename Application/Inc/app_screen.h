/**
 * @file    app_screen.h
 * @brief   整屏门面 —— 逻辑画布、渲染目标、落屏、亮度
 *
 * **它解决什么**：级联场景里主卡要画的"整屏"比它自己那块屏大（其余由从卡显示），
 * 也需要把同一份内容按卡切分下发。若不收出一个门面，协议模块就要直接摸
 * `dev_display_t`、`app_render`、切分表三样东西 —— 那是本设计明确要避免的耦合。
 *
 * 约定：**凡是影响整屏的动作都从本模块过**，`app_cascade` 只认本模块。
 *   · 渲染   —— 本模块实现 `render_target_t`（定义在 app_render.h）并注册为渲染目标，
 *               渲染引擎一行都不复制（MSL 那版复制了 200 行排版逻辑，是本设计要避开的）
 *   · 调光   —— `app_screen_set_brightness()` 一处设本地 + 置"待下发"标志
 *   · 落屏   —— `app_screen_commit_bitmap()`，**主卡本地与从卡走同一个函数**
 *   · 显存持久化 —— 通过 `render_persist_hook_t` 接管（画布比实屏大，直存实屏会错位）
 *
 * 画布是 **1bpp 位掩码**（`row_bytes = (宽+7)/8`、行优先、MSB-first、bit=1 上色），
 * 与 `dev_display_draw_bitmap` 和 `render_persist_t` 的位序约定**逐位一致** ——
 * 三处同一约定，所以"画布抽取出的位图"从卡可以直接吃，零转码。
 *
 * **每卡一个颜色**：画布只记亮/灭，每个像素最终是什么颜色由它属于哪张卡决定。
 * 代价是**卡内的多色也会塌缩成该卡那一色** —— 这是用户明确接受的取舍，不是缺陷。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/** @brief 显式提交：立刻把画布内容落到本地屏（不必等静默期）
 *
 *  默认不需要调 —— 静默期会自动提交，现有渲染调用点一行都不用改。
 *  只在"必须立即生效"的场合用（如某条协议要求看到即时反馈）。 */
void app_screen_flush(void);

/** @brief 画布内容代数（每次写入自增）。级联用它做"本轮内容是否已过期"的复检。 */
uint32_t app_screen_generation(void);

/** @brief 把一张整屏 1bpp 位图落到本地实屏（主卡本地提交与从卡落屏共用的唯一路径）
 *
 *  `len` 必须等于 `ceil(屏宽/8) × 屏高`，不符直接返回（换模组后的旧内容不适用）。
 *
 *  内部是 `fill(BLACK)` → `dirty=false` → `draw_bitmap`：
 *  中间那段 `dirty` 为假，`scan_task` 的 prepare 不跑，屏上不会闪出全黑帧 ——
 *  这个手法在 `app_factory_test.c` 已有先例。 */
void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color);

/** @brief 设置屏亮度等级（0~7）
 *
 *  级联下由主卡统一分发 —— 从卡若有自己的光传感器，两张卡会各调各的，屏上出现
 *  亮度接缝。故本函数是**唯一**的亮度入口，光传感器也走它。 */
void app_screen_set_brightness(uint8_t level);

/** @brief 当前亮度等级 */
uint8_t app_screen_get_brightness(void);

/** @brief 取走"亮度已变、待下发"的标志 —— 级联协议用它把每秒可能变多次的亮度
 *         攒成一次广播。返回 true 时 *level 给出新等级。
 *
 *  为什么要有这个：光传感器每秒都可能改，而广播要走总线。攒成一次比每次改都发
 *  省得多，且亮度是渐变量、晚几十毫秒下发不可见。 */
bool app_screen_brightness_take_pending(uint8_t *level);

/** @brief 本卡总线地址。0 = 主卡。
 *
 *  **身份属于整屏，不属于某个协议** —— 所以放在这里而不是 app_cascade：否则
 *  光传感器、以及将来任何"只有主卡该做"的事，都得去依赖级联协议。
 *
 *  本期取 board.h 的编译期常量（主卡/从卡烧不同固件）；后续期由 W25Qxx 的切分表
 *  记录覆盖 —— 同型号板子可以是 2/3/4 卡部署，那是**部署期事实**，不可能写死。 */
uint8_t app_screen_self_addr(void);

/** @brief 本卡是否主卡（地址 0） */
bool app_screen_is_master(void);
