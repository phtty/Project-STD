/**
 * @file    app_key.c
 * @brief   按键应用层 — dev_key 薄封装 + 干接点选图
 *
 * 两种用法：
 *   · DIP1/DIP2   上电读一次即可（级联地址）
 *   · SW1/SW2/SW3 干接点；本产品里它们选"预存画面"：SW1→槽位 1、SW2→2、SW3→3
 */

#include "app_key.h"

#include <stdio.h>

#include "initcall.h"
#include "pl_mem.h"
#include "pl_task.h"

#include "app_lcsp_slot.h"
#include "app_render.h"
#include "app_screen.h"
#include "dev_display.h"

static bool s_dip1_state;
static bool s_dip2_state;

bool app_key_get_state(dev_key_id_t key_id)
{
    switch (key_id) {
        case DEV_KEY_DIP1: return s_dip1_state;
        case DEV_KEY_DIP2: return s_dip2_state;
        default:           return dev_key_get_state(key_id);
    }
}

/* ---- 上电读取 DIP ---- */
static void _app_key_init(void)
{
    s_dip1_state = dev_key_get_state(DEV_KEY_DIP1);
    s_dip2_state = dev_key_get_state(DEV_KEY_DIP2);
}
sw_app_initcall(_app_key_init);

/* ================================================================
 *  干接点选图：SW1/SW2/SW3 → 槽位 1/2/3
 *
 *  **为什么轮询而不是 wait_press**：三个干接点要一起看，而且"闭合"这个动作
 *  需要去抖（机械触点抖动 5~20ms，EXTI 会把一次闭合拆成好几次）。20ms 扫一次、
 *  连续 3 次同一电平才认，正好 60ms —— 比人手快，又能吃掉抖动。
 *
 *  **语义**：认**闭合沿**，然后**锁存**（触点松开不切回去）。三个同时闭合时按
 *  SW1→SW2→SW3 的顺序处理，最后一个生效。槽位为空/几何不符就保持当前画面，
 *  只打一行日志 —— 现场至少还能看到原内容，而不是黑屏。
 *
 *  **落屏走和 0x06 一样的路径**（先 FILL 黑再贴图，画布只写 bit=1）：所以按下
 *  干接点后级联那一轮也会把对应内容推给从卡，不需要另写同步逻辑。
 *  `persist=true` 让它同时成为"上电画面"（本工程既有语义：最后一帧就是开机画面）。
 * ================================================================ */

#define CONTACT_SLOT_COUNT (3U)  /**< SW1..SW3 → 槽位 1..3 */
#define CONTACT_POLL_MS    (20U) /**< 采样周期 */
#define CONTACT_STABLE_CNT (3U)  /**< 连续同电平次数（约 60ms）才算稳 */

typedef struct {
    bool    raw;      /**< 上一次采样值 */
    bool    stable;   /**< 去抖后的状态 */
    uint8_t same_cnt; /**< 连续相同次数 */
} contact_filter_t;

static contact_filter_t s_contact[CONTACT_SLOT_COUNT];

/** 读整幅槽位图的工作缓冲。
 *  键任务和 LCSP 命令任务各跑各的，**不能**共用命令层那块 `s_image_buf`。
 *  放 CCMRAM（只在按键时用，是冷路径），W25Qxx 读已支持 CCMRAM 目标（分块中转）。 */
static uint8_t s_contact_img[LCSP_SLOT_PAYLOAD_MAX] PL_CCMRAM;

/** @brief 把槽位图铺到整屏画布；失败返回 false（画面保持不变） */
static bool _contact_show_slot(uint8_t slot)
{
    const uint16_t rows = app_screen_rows();
    const uint16_t cols = app_screen_cols();
    if ((0U == rows) || (0U == cols)) return false;

    const uint16_t stride = (uint16_t)((rows + 7U) / 8U);
    const uint32_t need   = (uint32_t)stride * (uint32_t)cols;
    if ((need > sizeof(s_contact_img)) || (need > LCSP_BITMAP_MAX_BYTES)) return false;

    app_lcsp_slot_image_t *img = (app_lcsp_slot_image_t *)s_contact_img;
    uint16_t                bm_len = 0;
    if (!app_lcsp_slot_load(slot, img, (uint16_t)sizeof(s_contact_img), &bm_len)) {
        printf("[key] 槽位 %u 没有可用画面，保持当前内容\n", (unsigned)slot);
        return false;
    }

    /* 几何/颜色必须配得上当前屏：照旧图铺新屏是一幅斜纹，而长度校验还会通过 */
    if ((img->rows != rows) || (img->cols != cols) || (bm_len != (uint16_t)need) ||
        (img->color > (uint8_t)DEV_DISPLAY_COLOR_WHITE)) {
        printf("[key] 槽位 %u 与当前屏不符（%ux%u/%uB vs %ux%u/%uB），忽略\n", (unsigned)slot,
               (unsigned)img->rows, (unsigned)img->cols, (unsigned)bm_len, (unsigned)rows,
               (unsigned)cols, (unsigned)need);
        return false;
    }

    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = rows,
        .h     = cols,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    app_render(&(app_render_cfg_t){
        .type    = APP_RENDER_TYPE_BITMAP,
        .x       = 0,
        .y       = 0,
        .w       = rows,
        .h       = cols,
        .color   = (dev_display_color_t)img->color,
        .persist = true,
        .bitmap  = img->bitmap,
    });
    return true;
}

static void _app_key_contact_task(void *argument)
{
    (void)argument;

    for (;;) {
        for (uint8_t i = 0; i < CONTACT_SLOT_COUNT; i++) {
            const bool        raw = dev_key_get_state((dev_key_id_t)(DEV_KEY_SW1 + i));
            contact_filter_t *f   = &s_contact[i];

            if (raw != f->raw) { /* 电平刚变：重新计数，别急着认 */
                f->raw      = raw;
                f->same_cnt = 0;
                continue;
            }
            if (f->same_cnt < 255U) f->same_cnt++;
            if ((f->same_cnt < CONTACT_STABLE_CNT) || (f->stable == raw)) continue;

            f->stable = raw;
            if (!raw) continue; /* 断开沿不动作：画面锁存 */

            printf("[key] 干接点 SW%u 闭合 → 显示槽位 %u\n", (unsigned)(i + 1U),
                   (unsigned)(i + 1U));
            (void)_contact_show_slot(i + 1U);
        }
        osDelay(CONTACT_POLL_MS);
    }
}

/** @brief 有干接点的板子才起这个任务（5006048 没有 SW1~SW3） */
static void _app_key_contact_init(void)
{
    bool any = false;
    for (uint8_t i = 0; i < CONTACT_SLOT_COUNT; i++)
        any = any || (dev_key_get((dev_key_id_t)(DEV_KEY_SW1 + i)) != nullptr);
    if (!any) return;

    static const osThreadAttr_t attr = {
        .name       = "app_key_contact",
        .stack_size = 512 * 4,
        .priority   = osPriorityNormal,
    };
    (void)pl_task_new(_app_key_contact_task, nullptr, &attr);
}
sw_app_initcall(_app_key_contact_init);
