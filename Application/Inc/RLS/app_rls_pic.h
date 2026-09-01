/**
 * @file    app_rls_pic.h
 * @brief   RLS 图片缓存模块 — 3 个 512B 缓存槽 + W25Qxx 持久化 + 干接点触发显示
 *
 * 设计约束（SOLID）：
 * - 本模块全部符号内聚于 RLS 目录，外部零引用，可从构建中整体剔除
 * - 协议持久化（图片记录，capacity-12288）与渲染引擎持久化（app_render_save，
 *   capacity-8192）物理分离、互不干扰；显示协议持久化内容后同步调
 *   app_render_save() 供上电恢复
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define RLS_PIC_SLOT_COUNT   (3U)
#define RLS_PIC_BITMAP_BYTES (512U)        /* (128+7)/8 × 32，全屏 1bpp 位图 */
#define RLS_PIC_MAGIC        (0x0D00A901U) /* 区别于 0x0d000721(render) / 0x0D001B00(LDI) */
#define RLS_PIC_PERSIST_BASE (12288U)      /* 存储地址 = capacity - 12288 */

/** @brief 单缓存槽：颜色 + 全屏位图（每行 16B，MSB-first） */
typedef struct [[gnu::packed]] {
    uint8_t color;                        /* display_color_t */
    uint8_t bitmap[RLS_PIC_BITMAP_BYTES];
} rls_pic_slot_t;

/** @brief Flash 记录：magic + 3 槽 + CRC32（覆盖 magic+slots，共 1547B） */
typedef struct [[gnu::packed]] {
    uint32_t magic;
    rls_pic_slot_t slot[RLS_PIC_SLOT_COUNT];
    uint32_t crc32;
} rls_pic_record_t;

/**
 * @brief 写入缓存槽并持久化到 W25Qxx
 * @param slot    槽号 0~2
 * @param bitmap  512B 全屏位图（每行 16B，MSB-first）
 * @param color   显示颜色（display_color_t）
 * @return true 持久化成功；false 失败（RAM 缓存已更新，本次会话显示不受影响）
 */
bool rls_pic_save(uint8_t slot, const uint8_t *bitmap, uint8_t color);

/**
 * @brief 显示缓存槽内容
 * - 槽有效：清屏 + 绘制槽内位图（槽内保存的颜色）
 * - 槽无效（未写入/校验失败）：仅清屏
 * - 末尾一律调用 app_render_save() 同步渲染引擎持久化
 */
void rls_pic_show(uint8_t slot);

/** @brief 模块初始化（sw_app_initcall）：加载记录 + 建互斥量 + 建按键任务 */
void rls_pic_init(void);
