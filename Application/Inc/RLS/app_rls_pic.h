#pragma once

/**
 * @file    app_rls_pic.h
 * @brief   RLS 图片槽 — 3 个全屏位图槽（协议图片编号 0/1/2）+ W25Qxx 持久化 + 干接点触发
 *
 * 文档的 `SV`（显示并保存）带"图片编号 0/1/2"，`DS`（显示已存储图片）按号取图 ——
 * 三个槽正好对应本板的三个干接点 **SW1/SW2/SW3**（`boards/3833024/Device/Src/dev_key_board.c`
 * 里注释即"干接点，EXTI 下降沿"），按下就显示对应槽。
 *
 * **存储**：整份记录（有效位 + 3 槽）作为**一条** `app_cfg_sched` 记录（名字 `rls_pic`，
 * 版本 1），落在 W25Qxx 尾部的配置区里 —— 地址由调度器按记录名反查分配，本模块
 * 不接触绝对地址，也不和字库/渲染持久化抢固定偏移。
 *
 * **RAM 镜像必须留在 SRAM**：`s_rec` 作为写 Flash 的源缓冲，W25Qxx 走 SPI DMA，
 * 而 DMA 到不了 CCMRAM（与渲染持久化同一个理由）。
 */

#include <stdint.h>
#include <stdbool.h>

/** @brief 图片槽个数 —— 协议 `DS`/`SV` 的"图片编号"字段取值 0/1/2 */
#define RLS_PIC_SLOT_COUNT (3U)

/** @brief 单槽位图字节数：ceil(128/8) × 32 —— 与协议一帧能带的整屏位图同尺寸
 *
 *  3833024 是 128×32 单卡，恰好 512B；几何不符时 `rls_pic_show()` 只清屏不画
 *  （见该函数的说明），**不会**按错误的 stride 去读槽里的位图。 */
#define RLS_PIC_BITMAP_BYTES (512U)

/** @brief 单槽记录：颜色 + 全屏 1bpp 位图（行优先、MSB-first，与 app_render 同约定） */
typedef struct {
    uint8_t color; /**< 显示颜色（`dev_display_color_t`，与协议线上字节同序） */
    uint8_t bitmap[RLS_PIC_BITMAP_BYTES]; /**< 1bpp 位图，行优先、MSB-first */
} rls_pic_slot_t;

/**
 * @brief 把一帧位图写进槽并持久化
 *
 * 只更新 RAM 镜像 + 落盘，**不显示**（显示由调用方随后调 `rls_pic_show()` 完成，
 * 这样"存"与"显示"的失败互不牵连）。落盘失败时 RAM 已更新，本次会话仍能显示，
 * 返回值供调用方打日志。
 *
 * @param slot   槽号 0..RLS_PIC_SLOT_COUNT-1；越界直接返回 false
 * @param bitmap 512 字节位图（调用方须已校验帧内有这么多字节）
 * @param color  颜色（`dev_display_color_t` 的数值，调用方已做越界校验）
 * @return true = 落盘成功；false = 参数非法或落盘失败（RAM 可能已更新）
 */
bool rls_pic_save(uint8_t slot, const uint8_t *bitmap, uint8_t color);

/**
 * @brief 显示某个槽
 *
 * 先清屏再画槽内位图，两笔包在 `dev_display_frame_begin/end` 里（直写实屏路径上
 * 避免出现"清完还没画"的黑帧）。位图带 `persist` 请求 —— 这就是协议 `SV` 的
 * "断电保存"：落屏之后由渲染层把这一帧存起来，上电自动恢复最后显示的画面。
 *
 * 槽未写过、或本屏几何不等于 512B 位图时**只清屏**并打日志（不画垃圾、不越界读）。
 *
 * @param slot 槽号 0..RLS_PIC_SLOT_COUNT-1；越界按"未写入"处理
 */
void rls_pic_show(uint8_t slot);
