/**
 * @file    app_rls_pic.c
 * @brief   RLS 图片槽实现 — 3 个 512B 槽 + 配置记录持久化 + SW1/SW2/SW3 干接点触发
 *
 * 存储走 `app_cfg_sched`：整份记录（有效位 + 3 槽）是一条具名记录（`rls_pic` v1），
 * 由调度器负责记录头/CRC/地址/去重。**不要**改成"器件容量减固定值"的裸地址 ——
 * 那会与字库、渲染持久化、各协议配置区抢地盘，且换器件容量就错位。
 */

#include "app_rls_pic.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "initcall.h"

#include "dev_display.h"
#include "dev_key.h"
#include "app_cfg_sched.h"
#include "app_render.h"
#include "app_screen.h"
#include "pl_task.h"

/** @brief 持久化载荷：有效位掩码 + 3 个槽（= 调度的记录体，版本号在本文件末尾注册处） */
typedef struct {
    uint8_t        valid_mask; /**< bit N = 槽 N 写过（没写过的槽显示时只清屏） */
    rls_pic_slot_t slot[RLS_PIC_SLOT_COUNT];
} rls_pic_record_t;

/* RAM 镜像：既是显示源，也是写 Flash 的源缓冲。**留在 SRAM**：W25Qxx 走 SPI DMA，
   DMA 到不了 CCMRAM（与 app_render 的持久化镜像同一个理由）。 */
static rls_pic_record_t s_rec;

static uint8_t     s_cfg_id = 0xFF; /* 0xFF = 注册失败/未注册 → 只 mem，不落盘 */
static osMutexId_t s_lock;

/* 布局与容量是持久化契约：改了就换记录版本号（旧记录会被判无效 → 槽回落到"未写"），
   否则旧数据会被按新布局解出乱码位图。 */
static_assert(sizeof(rls_pic_slot_t) == 1U + RLS_PIC_BITMAP_BYTES,
              "图片槽布局变了：这是持久化契约，请同时升 app_cfg_sched 的记录版本号");
static_assert(sizeof(rls_pic_record_t) + DEV_CFG_RECORD_HDR_SIZE <= DEV_CFG_RECORD_MAX_IMAGE,
              "图片记录装不进配置记录的最大映像：调大 DEV_CFG_RECORD_MAX_IMAGE");

/* ================================================================
 *  加载（app_cfg_sched 的 load 回调，在 load_all 里被调用）
 * ================================================================ */

static void _rls_pic_load(void)
{
    if (s_cfg_id == 0xFF) return;

    uint16_t                     len = 0;
    const dev_cfg_record_state_t st =
        app_cfg_sched_load(s_cfg_id, (uint8_t *)&s_rec, sizeof(s_rec), &len);

    if (st != DEV_CFG_RECORD_STATE_OK || len != sizeof(s_rec)) {
        /* 空 / 损坏 / 长度不符：全零镜像 = 三个槽都未写过（DS 只清屏，不画垃圾）。 */
        memset(&s_rec, 0, sizeof(s_rec));
    }
}

/* ================================================================
 *  保存与显示
 * ================================================================ */

bool rls_pic_save(uint8_t slot, const uint8_t *bitmap, uint8_t color)
{
    if (slot >= RLS_PIC_SLOT_COUNT || bitmap == nullptr) return false;

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    s_rec.slot[slot].color = color;
    memcpy(s_rec.slot[slot].bitmap, bitmap, RLS_PIC_BITMAP_BYTES);
    s_rec.valid_mask |= (uint8_t)(1U << slot);

    int32_t rc = -1;
    if (s_cfg_id != 0xFF) rc = app_cfg_sched_save(s_cfg_id, (const uint8_t *)&s_rec, sizeof(s_rec));

    if (s_lock) osMutexRelease(s_lock);

    if (rc < 0) printf("[rls] 图片槽 %u 落盘失败（RAM 已更新，本次会话仍可显示）\n", (unsigned)slot);
    return rc == 0;
}

void rls_pic_show(uint8_t slot)
{
    dev_display_t *dsp = dev_display_get();
    if (dsp == nullptr) return;

    /* 几何门禁：槽里的位图是按 128×32（512B）存的，本屏不是这个尺寸就只清屏 ——
       否则 app_render 会按本屏 stride 去读这 512B，越界读或整幅错位。 */
    const uint16_t w       = app_screen_rows();
    const uint16_t h       = app_screen_cols();
    const uint32_t need    = (((uint32_t)w + 7U) / 8U) * h;
    const bool     geom_ok = (need == RLS_PIC_BITMAP_BYTES);

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    const bool valid =
        slot < RLS_PIC_SLOT_COUNT && (s_rec.valid_mask & (uint8_t)(1U << slot)) && geom_ok;

    /* 清 + 画包成一帧：直写实屏路径上 _scan_task 可能先 prepare 出"清完还没画"的
       中间态，包住后最终画面不变、只是不再经过那一帧（与 app_vms_ctrl.c 同款）。 */
    dev_display_frame_begin(dsp);
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    if (valid) {
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_BITMAP,
            .x       = 0,
            .y       = 0,
            .w       = w,
            .h       = h,
            .color   = (dev_display_color_t)s_rec.slot[slot].color,
            .bitmap  = s_rec.slot[slot].bitmap,
            /* SV 的"断电保存"：内容定稿之后由渲染层存一份，上电自动恢复最后画面 */
            .persist = true,
        });
    }
    dev_display_frame_end(dsp);

    if (s_lock) osMutexRelease(s_lock);

    if (!valid)
        printf("[rls] 图片槽 %u %s，已清屏\n", (unsigned)slot,
               geom_ok ? "未写入（等 SV 命令）" : "与本屏几何不符（槽内 128×32/512B）");
}

/* ================================================================
 *  干接点：SW1/SW2/SW3 → 槽 0/1/2
 * ================================================================ */

/** @brief 干接点轮询任务：对三个键各等 50ms，按下即显示对应槽（不改亮度状态）
 *
 *  用 `dev_key_wait_press` 的信号量而不是轮询 `get_state`：前者阻塞、零 CPU 占用，
 *  去抖已在 EXTI 回调里做过（见 dev_key.c 的 KEY_DEBOUNCE_MS 说明）。
 *  本板三颗键都是"干接点、EXTI 下降沿"（boards/3833024/Device/Src/dev_key_board.c）。 */
static void _rls_pic_key_task(void *argument)
{
    (void)argument;
    static const dev_key_id_t keys[RLS_PIC_SLOT_COUNT] = {DEV_KEY_SW1, DEV_KEY_SW2, DEV_KEY_SW3};

    for (;;) {
        for (uint8_t i = 0; i < RLS_PIC_SLOT_COUNT; i++)
            if (dev_key_wait_press(keys[i], 50)) rls_pic_show(i);
    }
}

/* ================================================================
 *  自注册：记录先登记（load_all 之前），任务后起（load_all 之后）
 * ================================================================ */

static const app_cfg_sched_desc_t s_rls_pic_desc = {
    .name    = "rls_pic", /**< 记录归属名，改名即视为换模块（旧记录回落"未写入"） */
    .version = 1,         /**< 布局变了才升（见文件头的 static_assert） */
    .load    = _rls_pic_load,
};

[[maybe_unused]] static void _rls_pic_register(void)
{
    s_lock   = osMutexNew(NULL);
    s_cfg_id = app_cfg_sched_register(&s_rls_pic_desc);

    /* 注册失败（配置区满 / 名字冲突）：功能降级为"只在 RAM 里生效"，不必停机 ——
       屏照样能显示，只是掉电丢图，所以只打日志。 */
    if (s_cfg_id == 0xFF)
        printf("[rls] 图片槽记录注册失败（配置区满或名字冲突）：本次会话可用，掉电不保存\n");
}
sw_dev_initcall(_rls_pic_register);

[[maybe_unused]] static void _rls_pic_start(void)
{
    const osThreadAttr_t attr = {
        .name       = "rls_pic_key",
        .stack_size = 256 * 4,
        .priority   = (osPriority_t)osPriorityNormal,
    };
    if (pl_task_new(_rls_pic_key_task, nullptr, &attr) == nullptr)
        printf("[rls] 图片槽干接点任务创建失败\n");
}
sw_post_initcall(_rls_pic_start);
