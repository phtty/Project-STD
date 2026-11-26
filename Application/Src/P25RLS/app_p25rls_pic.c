/**
 * @file    app_p25rls_pic.c
 * @brief   P25RLS 图片槽实现 —— 3 个双平面位图槽 + 配置记录持久化 + 上电重放
 *
 * 存储走 `app_cfg_sched`：整份记录（有效位 + 最后显示槽 + 3 槽位图）是一条具名
 * 记录（`p25_pic` v1），由调度器负责记录头/CRC/地址/去重 —— 不要改成裸地址。
 *
 * 只做"存 + 上电恢复"：协议没有"显示已存储图片"的命令（SV 是存并显示）。
 */

#include "app_p25rls_pic.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "initcall.h"

#include "dev_display.h"
#include "app_cfg_sched.h"
#include "app_render.h"
#include "app_screen.h"

/** @brief 持久化载荷：有效位掩码 + 最后显示的槽 + 3 个槽（= 调度的记录体） */
typedef struct {
    uint8_t           valid_mask; /**< bit N = 槽 N 写过 */
    uint8_t           last_shown; /**< 最后显示的槽号；0xFF = 没有（上电不重放） */
    p25rls_pic_slot_t slot[P25RLS_PIC_SLOT_COUNT];
} p25rls_pic_record_t;

/* RAM 镜像：既是显示源，也是写 Flash 的源缓冲。**留在 SRAM**：W25Qxx 走 SPI DMA，
   DMA 到不了 CCMRAM（与 RLS 图片槽、渲染持久化同一个理由）。 */
static p25rls_pic_record_t s_rec;

static uint8_t s_cfg_id = 0xFF; /* 0xFF = 注册失败/未注册 → 只 mem，不落盘 */

/* 绘制用掩码（单平面大小，逐遍复用；只在本模块任务/初始化上下文里用，无并发） */
static uint8_t s_mask[P25RLS_PLANE_BYTES];

/* 布局与容量是持久化契约：改了就换记录版本号（旧记录会被判无效 → 槽回落到"未写"）。 */
static_assert(sizeof(p25rls_pic_slot_t) == P25RLS_BITMAP_BYTES,
              "图片槽布局变了：这是持久化契约，请同时升记录版本号");
static_assert(sizeof(p25rls_pic_record_t) + DEV_CFG_RECORD_HDR_SIZE <= DEV_CFG_RECORD_MAX_IMAGE,
              "图片记录装不进配置记录的最大映像：调大 DEV_CFG_RECORD_MAX_IMAGE");

/* ================================================================
 *  加载（app_cfg_sched 的 load 回调，在 load_all 里被调用）
 * ================================================================ */

static void _p25rls_pic_load(void)
{
    if (s_cfg_id == 0xFF) return;

    uint16_t                     len = 0;
    const dev_cfg_record_state_t st =
        app_cfg_sched_load(s_cfg_id, (uint8_t *)&s_rec, sizeof(s_rec), &len);

    if (st != DEV_CFG_RECORD_STATE_OK || len != sizeof(s_rec)) {
        /* 空 / 损坏 / 长度不符：全零镜像 = 三个槽都未写过、last_shown=0 → 上电不重放 */
        memset(&s_rec, 0, sizeof(s_rec));
        s_rec.last_shown = 0xFF;
    }
}

/* ================================================================
 *  绘制：红 / 绿 / 黄三遍
 * ================================================================ */

/** @brief 整屏填黑（清屏；不落盘） */
static void _p25rls_fill_black(void)
{
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = app_screen_rows(),
        .h     = app_screen_cols(),
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
}

/** @brief 线上位序 → 内部位序：每字节内位序翻转
 *
 *  **线上是 LSB-first**（每字节 bit0 = 最左），实测自现场上位机发给本版的"车道关闭"
 *  四字图：按 MSB-first 解出来是碎片，翻位后才是完整字形。而渲染引擎/画布的位图
 *  原语是 **MSB-first**（`(宽+7)/8` 行字节、bit7 为最左），所以绘制前逐字节翻一次。
 *
 *  槽里存的是**线上原样字节**（调试/回读时能直接和抓包对照），翻转只发生在落屏前。
 *  注意与 RLS 的差别：RLS 那边按 MSB-first 解（现场运行过的口径），两者**不可照抄**。
 *  @param b 线上字节
 *  @return 内部 MSB-first 字节 */
static uint8_t _p25rls_bit_reverse(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0U) >> 4) | ((b & 0x0FU) << 4));
    b = (uint8_t)(((b & 0xCCU) >> 2) | ((b & 0x33U) << 2));
    b = (uint8_t)(((b & 0xAAU) >> 1) | ((b & 0x55U) << 1));
    return b;
}

/** @brief 把一个双平面位图按"红独有 / 绿独有 / 两平面都有"三遍画上屏
 *
 *  渲染引擎的位图原语是"一张 1bpp 图 + 一个颜色"，而本协议一个像素可能是三种颜色
 *  之一；三个掩码互不相交，依次画就得到逐像素正确的结果。
 *  @param bitmap 576B：先红平面、后绿平面（各 288B，1bpp、行优先、**线上 LSB 为最左**） */
static void _p25rls_pic_draw(const uint8_t *bitmap)
{
    static const dev_display_color_t k_colors[3] = {
        DEV_DISPLAY_COLOR_RED, DEV_DISPLAY_COLOR_GREEN, DEV_DISPLAY_COLOR_YELLOW};

    const uint8_t *red   = bitmap;
    const uint8_t *green = &bitmap[P25RLS_PLANE_BYTES];

    for (uint8_t pass = 0; pass < 3U; pass++) {
        for (uint16_t i = 0; i < P25RLS_PLANE_BYTES; i++) {
            /* 线上 LSB-first → 内部 MSB-first（见 _p25rls_bit_reverse） */
            const uint8_t r = _p25rls_bit_reverse(red[i]);
            const uint8_t g = _p25rls_bit_reverse(green[i]);
            /* 位级独立，可以整字节算：红独有 / 绿独有 / 两者都有 */
            s_mask[i] = (pass == 0U) ? (uint8_t)(r & (uint8_t)~g)
                        : (pass == 1U) ? (uint8_t)(g & (uint8_t)~r)
                                       : (uint8_t)(r & g);
        }

        app_render(&(app_render_cfg_t){
            .type   = APP_RENDER_TYPE_BITMAP,
            .x      = 0,
            .y      = 0,
            .w      = (uint16_t)P25RLS_BITMAP_W,
            .h      = (uint16_t)P25RLS_BITMAP_H,
            .color  = k_colors[pass],
            .bitmap = s_mask,
        });
    }
}

/* ================================================================
 *  保存与显示
 * ================================================================ */

bool p25rls_pic_save(uint8_t slot, const uint8_t *bitmap)
{
    if (slot >= P25RLS_PIC_SLOT_COUNT || bitmap == nullptr) return false;

    memcpy(s_rec.slot[slot].bitmap, bitmap, P25RLS_BITMAP_BYTES);
    s_rec.valid_mask |= (uint8_t)(1U << slot);
    s_rec.last_shown = slot;

    int32_t rc = -1;
    if (s_cfg_id != 0xFF) rc = app_cfg_sched_save(s_cfg_id, (const uint8_t *)&s_rec, sizeof(s_rec));

    if (rc < 0) printf("[p25rls] 图片槽 %u 落盘失败（RAM 已更新，本次会话仍可显示）\n", (unsigned)slot);
    return rc == 0;
}

void p25rls_pic_show(uint8_t slot)
{
    dev_display_t *dsp = dev_display_get();
    if (dsp == nullptr) return;

    const bool valid = (slot < P25RLS_PIC_SLOT_COUNT) && (s_rec.valid_mask & (uint8_t)(1U << slot));
    if (!valid) {
        printf("[p25rls] 图片槽 %u 未写入，只清屏\n", (unsigned)slot);
        _p25rls_fill_black();
        return;
    }

    s_rec.last_shown = slot;

    /* 清 + 三遍画包成一帧：直写实屏路径上避免出现"清完还没画"的黑帧（与 RLS 同款） */
    dev_display_frame_begin(dsp);
    _p25rls_fill_black();
    _p25rls_pic_draw(s_rec.slot[slot].bitmap);
    dev_display_frame_end(dsp);
}

/** @brief 上电重放最后一次显示的槽
 *
 *  只在 `last_shown` 指向"写过且槽号合法"时重放；否则不动屏（返回 false，让
 *  `app_boot` 走它自己的恢复/兜底 —— 与本协议"只有 SV 承诺断电保存"一致）。 */
bool p25rls_pic_restore(void)
{
    /* 没有显示设备就画不出来 —— 别谎报"接管了"，让 app_boot 走它自己的流程 */
    if (dev_display_get() == nullptr) return false;
    if (s_rec.last_shown >= P25RLS_PIC_SLOT_COUNT) return false;
    if (0U == (s_rec.valid_mask & (uint8_t)(1U << s_rec.last_shown))) return false;

    p25rls_pic_show(s_rec.last_shown);
    printf("[p25rls] 上电重放图片槽 %u\n", (unsigned)s_rec.last_shown);
    return true;
}

/* ================================================================
 *  自注册
 * ================================================================ */

static const app_cfg_sched_desc_t s_p25rls_pic_desc = {
    .name    = "p25_pic", /**< 记录归属名，改名即视为换模块（旧记录回落"未写入"） */
    .version = 1,         /**< 布局变了才升（见文件头的 static_assert） */
    .load    = _p25rls_pic_load,
};

[[maybe_unused]] static void _p25rls_pic_register(void)
{
    s_cfg_id         = app_cfg_sched_register(&s_p25rls_pic_desc);
    s_rec.last_shown = 0xFF;

    /* 上电画面归本模块：`app_boot` 的 `app_default_display()` 跑在 initcall 之后，
     * 不接管的话它会用 `render_persist`（可能是别的协议时代的旧图、或兜底"车道关闭"）
     * 把刚重放的 SV 多色图盖掉。 */
    app_render_set_boot_paint(p25rls_pic_restore);

    /* 注册失败（配置区满 / 名字冲突）：降级为"只在 RAM 里生效"，屏照常显示、掉电丢图 */
    if (s_cfg_id == 0xFF)
        printf("[p25rls] 图片槽记录注册失败（配置区满或名字冲突）：本次会话可用，掉电不保存\n");
}
sw_dev_initcall(_p25rls_pic_register);
