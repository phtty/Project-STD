/**
 * @file    test_display_base.c
 * @brief   显示设备基类：注册 / 颜色能力 / 像素与矩形裁剪 / 位图 stride / 扫描任务骨架
 *
 * 画布/切分套件用的是**参照实现**（`test/display/dev_display_ref.h`），生产
 * `dev_display.c` 本身一直没跑过 host。它管的是所有模组共用、也最容易出"只在真机
 * 看得见"的几件事：
 *   - 像素/填充/位图三层的**裁剪**（越界 memset 直接冲缓冲）；
 *   - `frame_begin/end` 的脏标记纪律（中间态上屏就是闪一下）；
 *   - `_scan_task` 的**帧首才 prepare**（行间重建表 = 一帧撕裂）；
 *   - OE/LAT 原子窗口、PWM 亮度占空比、扫描节拍统计。
 *
 * 手法：TU-include 生产 `dev_display.c`（`_scan_task` / `_on_pwm_period` 是 static），
 * 把 TIM、HUB75 引脚、RTOS 原语换成记录桩；扫描任务真的跑在 pthread 上，
 * 由脚本化的 `osThreadFlagsWait` 推进 4 拍后退出。
 */

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dev_display.h"
#include "pl_task.h"
#include "pl_tim.h"

/* ================================================================
 *  断言（变体 B）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  看门狗
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似扫描任务死等）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* ================================================================
 *  RTOS / 平台桩
 * ================================================================ */

static int      s_flag_wait_cnt;
static int      s_flag_set_cnt;
static uint32_t s_flag_set_flags;
static osThreadId_t s_flag_set_id;
static uint32_t s_tick;
static int      s_kernel_lock_cnt;
static int      s_kernel_unlock_cnt;
static dev_display_t *s_script_dev; /**< 扫描脚本：第 3 拍前把 dirty 重新置上 */

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    (void)flags;
    (void)options;
    (void)timeout;
    s_flag_wait_cnt++;

    /* 时间线：100 → 103（gap 3）→ 110（gap 7，超期）→ 112/115/117/120。
       第 2 拍前把 dirty 再置上：用来区分"只在帧首 prepare"和"每拍都 prepare"。 */
    static const uint32_t ticks[] = {100U, 103U, 110U, 112U, 115U, 117U, 120U};
    if (s_flag_wait_cnt <= 7) {
        s_tick = ticks[s_flag_wait_cnt - 1];
        if (s_flag_wait_cnt == 2 && s_script_dev != NULL) s_script_dev->dirty = true;
        return 0x01;
    }
    pthread_exit(NULL);
    return 0;
}

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
    s_flag_set_cnt++;
    s_flag_set_id    = thread_id;
    s_flag_set_flags = flags;
    return flags;
}

osThreadId_t osThreadGetId(void) { return (osThreadId_t)(uintptr_t)0x5A5AU; }

uint32_t osKernelGetTickCount(void) { return s_tick; }

int32_t osKernelLock(void) { s_kernel_lock_cnt++; return 0; }
int32_t osKernelUnlock(void) { s_kernel_unlock_cnt++; return 0; }

void osThreadExit(void) { pthread_exit(NULL); }

static osThreadFunc_t    s_task_fn;
static void             *s_task_arg;
static osThreadAttr_t    s_task_attr;

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    s_task_fn = fn;
    s_task_arg = arg;
    if (attr != NULL) s_task_attr = *attr;
    return (osThreadId_t)(uintptr_t)0x1234U;
}

/* ---- pl_tim：记录注册/启停/IRQ 开关 ---- */
static pl_tim_period_fn_t s_period_fn[8];
static int s_freeze_cnt;
static int s_start_cnt;
static int s_irq_disable_cnt;
static int s_irq_enable_cnt;

pl_tim_handle_t pl_tim_get_handle(uint8_t id) { return (pl_tim_handle_t)(uintptr_t)(id + 1U); }
void pl_tim_start_it(pl_tim_handle_t h) { (void)h; s_start_cnt++; }
void pl_tim_dbg_freeze(pl_tim_handle_t h) { (void)h; s_freeze_cnt++; }
void pl_tim_irq_disable(uint8_t irq) { (void)irq; s_irq_disable_cnt++; }
void pl_tim_irq_enable(uint8_t irq) { (void)irq; s_irq_enable_cnt++; }
uint8_t pl_tim_irq_of(uint8_t id) { return (uint8_t)(id + 10U); }

void pl_tim_register_period_fn(uint8_t tim_id, pl_tim_period_fn_t fn)
{
    if (tim_id < 8) s_period_fn[tim_id] = fn;
}

/* ---- pl_hub75：OE/LAT/行地址记录 ---- */
static int  s_oe_blank_cnt;
static int  s_oe_level_cnt;
static int  s_latch_cnt;
static int  s_hub_init_cnt;
static int  s_row_last;

void pl_hub75_init(void) { s_hub_init_cnt++; }
void pl_hub75_oe_set(bool blank)
{
    if (blank) s_oe_blank_cnt++;
    else s_oe_level_cnt++;
}
void pl_hub75_latch_pulse(void) { s_latch_cnt++; }
void pl_hub75_set_row(uint8_t row) { s_row_last = (int)row; }

/* ================================================================
 *  生产模块本体
 * ================================================================ */

#include "../../Device/Display/dev_display.c"

/* ================================================================
 *  假显示设备
 * ================================================================ */

/* 缓冲**严格等于**屏大小（4×2）：越界写必须让 ASan 当场抓到，不能靠大数组兜着 */
static uint8_t       s_pixels[4U * 2U];
static uint8_t       s_hub[4U * 2U];
static int           s_prepare_cnt;
static int           s_scan_cnt;
static int           s_set_row_cnt;
static int           s_scan_line_seen[8];
static int           s_prepare_at[8]; /**< prepare 发生在第几次 scan 之前 */

static void _fake_prepare(dev_display_t *dev)
{
    (void)dev;
    if (s_prepare_cnt < 8) s_prepare_at[s_prepare_cnt] = s_scan_cnt;
    s_prepare_cnt++;
}
static void _fake_scan(dev_display_t *dev, uint8_t line)
{
    (void)dev;
    if (s_scan_cnt < 8) s_scan_line_seen[s_scan_cnt] = line;
    s_scan_cnt++;
}
static void _fake_set_row(uint8_t line)
{
    (void)line;
    s_set_row_cnt++;
}

static const dev_display_ops_t s_ops = {
    .prepare = _fake_prepare,
    .scan    = _fake_scan,
    .set_row = _fake_set_row,
};

static dev_display_t s_dev;

/** @brief 4×2 的小屏（= 8 像素），3 扫行 —— 方便手算裁剪与行循环 */
static void _make_dev(uint8_t scan_lines)
{
    memset(&s_dev, 0, sizeof(s_dev));
    memset(s_pixels, 0, sizeof(s_pixels));
    memset(s_hub, 0, sizeof(s_hub));
    s_dev.ops                 = &s_ops;
    s_dev.screen_rows         = 4;
    s_dev.screen_cols         = 2;
    s_dev.scan_lines          = scan_lines;
    s_dev.buffer_size         = 8;
    s_dev.pixel_map           = s_pixels;
    s_dev.hub75_buff          = s_hub;
    s_dev.light_level         = 7;
    s_dev.supported_color_mask = 0;

    s_prepare_cnt = s_scan_cnt = s_set_row_cnt = 0;
    memset(s_scan_line_seen, 0, sizeof(s_scan_line_seen));
    memset(s_prepare_at, 0, sizeof(s_prepare_at));
    s_scan_task_id   = NULL;
    s_scan_max_gap_ms = 0;
    s_scan_overruns   = 0;
    s_scan_last_tick  = 0;
}

static void fixture_reset(void)
{
    _make_dev(3);
    s_active_display = NULL;

    s_flag_wait_cnt = s_flag_set_cnt = 0;
    s_flag_set_flags = 0;
    s_flag_set_id    = NULL;
    s_tick           = 0;
    s_kernel_lock_cnt = s_kernel_unlock_cnt = 0;
    s_script_dev     = NULL;

    s_period_fn[0] = s_period_fn[1] = NULL;
    s_freeze_cnt = s_start_cnt = s_irq_disable_cnt = s_irq_enable_cnt = 0;
    s_oe_blank_cnt = s_oe_level_cnt = s_latch_cnt = s_hub_init_cnt = 0;
    s_row_last = -1;
}

/* ================================================================ */

/** @brief 注册/颜色能力：0 掩码按"全彩"处理（老驱动漏填字段的兼容语义） */
static void case_register_color(void)
{
    TEST_BEGIN("注册与颜色能力：0 掩码=全彩、显式掩码过滤、黑恒支持");

    fixture_reset();
    CHECK_MSG(dev_display_get() == NULL, "未注册时应返回 NULL");
    dev_display_register(&s_dev);
    CHECK_MSG(dev_display_get() == &s_dev, "注册后应能取到活动实例");
    dev_display_register(NULL);
    CHECK_MSG(dev_display_get() == NULL, "注册 NULL 后应取不到实例");

    dev_display_register(&s_dev);
    CHECK_MSG(dev_display_color_mask(&s_dev) == DEV_DISPLAY_COLOR_ALL, "0 掩码应视为全彩");
    CHECK_MSG(dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_WHITE), "全彩应支持白");
    CHECK_MSG(dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_BLACK), "黑恒支持");
    CHECK_MSG(!dev_display_supports_color(NULL, DEV_DISPLAY_COLOR_RED), "NULL 设备不支持任何色");
    CHECK_MSG(dev_display_color_mask(NULL) == 0U, "NULL 设备掩码应为 0");

    s_dev.supported_color_mask = 0x0EU; /* bit1/2/3 = R/G/Y */
    CHECK_MSG(dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_RED) &&
                  dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_GREEN) &&
                  dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_YELLOW),
              "掩码 0x0E 应支持 R/G/Y");
    CHECK_MSG(!dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_BLUE) &&
                  !dev_display_supports_color(&s_dev, DEV_DISPLAY_COLOR_WHITE),
              "掩码 0x0E 不应支持 B/白");
}

/** @brief 硬件初始化：HUB75 + 两个 TIM 的 DBG 冻结/周期回调登记 */
static void case_init(void)
{
    TEST_BEGIN("初始化：HUB75 init + 两个 TIM 的 DBG 冻结与周期回调");

    fixture_reset();
    dev_display_init();

    CHECK_MSG(s_hub_init_cnt == 1, "应初始化 HUB75（%d）", s_hub_init_cnt);
    CHECK_MSG(s_freeze_cnt == 2, "应对 TIM3/TIM4 各做一次 DBG 冻结（%d）", s_freeze_cnt);
    CHECK_MSG(s_period_fn[PL_TIM_DISPLAY_SCAN] == _on_scan_period,
              "扫描周期回调应登记到 TIM3");
    CHECK_MSG(s_period_fn[PL_TIM_DISPLAY_PWM] == _on_pwm_period, "PWM 周期回调应登记到 TIM4");

    /* 任务尚未起来：扫描节拍直接丢弃，不得访问空句柄 */
    _on_scan_period();
    CHECK_MSG(s_flag_set_cnt == 0, "任务未注册时不得发 thread flag（%d）", s_flag_set_cnt);
}

/** @brief PWM 亮度：level N → 每 8 拍里后 (8-N) 拍不消隐（占空比 (8-N)/8） */
static void case_pwm(void)
{
    TEST_BEGIN("PWM 亮度：level 决定 8 拍窗口里的消隐占空比");

    fixture_reset();
    s_active_display = &s_dev;

    s_dev.light_level = 3U;
    s_oe_blank_cnt = s_oe_level_cnt = 0;
    for (int i = 0; i < 8; i++) _on_pwm_period();
    CHECK_MSG(s_oe_blank_cnt == 5 && s_oe_level_cnt == 3,
              "level 3 应 3 拍点亮/5 拍消隐（%d/%d）", s_oe_blank_cnt, s_oe_level_cnt);

    s_dev.light_level = 7U;
    s_oe_blank_cnt = s_oe_level_cnt = 0;
    for (int i = 0; i < 8; i++) _on_pwm_period();
    CHECK_MSG(s_oe_blank_cnt == 1 && s_oe_level_cnt == 7,
              "level 7 应只在 1 拍消隐（%d/%d）", s_oe_blank_cnt, s_oe_level_cnt);

    s_dev.light_level = 0U;
    s_oe_blank_cnt = s_oe_level_cnt = 0;
    for (int i = 0; i < 8; i++) _on_pwm_period();
    CHECK_MSG(s_oe_blank_cnt == 8 && s_oe_level_cnt == 0, "level 0 应全消隐（屏灭）");

    dev_display_set_brightness(&s_dev, 9U);
    CHECK_MSG(s_dev.light_level == 7U, "亮度 >7 应钳到 7，得到 %u", (unsigned)s_dev.light_level);
}

/** @brief 启动：置脏 + 起实时优先级扫描任务；真实任务句柄/参数要正确 */
static void case_start(void)
{
    TEST_BEGIN("启动：置脏 + 创建实时优先级扫描任务");

    fixture_reset();
    s_active_display = &s_dev;

    dev_display_start();

    CHECK_MSG(s_dev.dirty, "启动时必须先置脏（否则首帧不 prepare）");
    CHECK_MSG(s_scan_task_id == NULL, "start 只建任务；登记句柄发生在任务体内");
    CHECK_MSG(s_task_fn != NULL && s_task_arg == &s_dev, "应把 _scan_task 与设备实例交给 pl_task_new");
    CHECK_MSG(s_task_attr.priority == osPriorityRealtime, "扫描任务必须是实时优先级（%d）",
              (int)s_task_attr.priority);
    CHECK_MSG(s_task_attr.stack_size == 512U, "扫描任务栈应为 512（%u）",
              (unsigned)s_task_attr.stack_size);
}

/** @brief 像素：越界不置脏、正常写色置脏；frame_begin/end 压制与"是否真写过" */
static void case_set_pixel(void)
{
    TEST_BEGIN("像素：越界不置脏；frame 压制按 frame_touched 决定是否上屏");

    fixture_reset();

    dev_display_set_pixel(&s_dev, 0, 0, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(s_pixels[0] == DEV_DISPLAY_COLOR_RED && s_dev.dirty, "正常写像素应写色并置脏");

    s_dev.dirty = false;
    dev_display_set_pixel(&s_dev, 4, 0, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty && s_pixels[0] == DEV_DISPLAY_COLOR_RED, "x==rows 越界不得置脏");
    dev_display_set_pixel(&s_dev, 0, 2, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "y==cols 越界不得置脏");
    dev_display_set_pixel(NULL, 0, 0, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "NULL 设备不得置脏");

    /* 压制期间：写了 → frame_touched；end 时置一次脏 */
    s_dev.dirty = false;
    dev_display_frame_begin(&s_dev);
    dev_display_set_pixel(&s_dev, 1, 0, DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(!s_dev.dirty && s_dev.frame_touched, "压制期间不得置脏，但要记住写过");
    dev_display_frame_end(&s_dev);
    CHECK_MSG(s_dev.dirty && !s_dev.dirty_hold, "end 时按 touched 置脏并解除压制");

    /* 压制期间什么都没写（画布路径）：end 不得置脏（否则白跑一次 prepare） */
    s_dev.dirty     = false;
    s_dev.frame_touched = false;
    dev_display_frame_begin(&s_dev);
    dev_display_frame_end(&s_dev);
    CHECK_MSG(!s_dev.dirty, "压制期间没写过实屏缓冲 → end 不得置脏");
}

/** @brief 填充：起点越界、0 尺寸、右/下裁剪、x+w 回绕都要只画屏内 */
static void case_fill(void)
{
    TEST_BEGIN("填充：越界起点/0 尺寸/右下标裁剪/x+w 回绕");

    fixture_reset();

    dev_display_fill(&s_dev, 4, 0, 1, 1, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "x==rows 起点越界应整块跳过（不得置脏）");
    dev_display_fill(&s_dev, 0, 2, 1, 1, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "y==cols 起点越界应整块跳过");
    dev_display_fill(&s_dev, 1, 0, 0, 1, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "w=0 不得画（设备层没有 0=全屏约定）");

    dev_display_fill(&s_dev, 2, 0, 100, 1, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(s_pixels[2] == DEV_DISPLAY_COLOR_RED && s_pixels[3] == DEV_DISPLAY_COLOR_RED &&
                  s_pixels[0] == 0,
              "右边界应裁剪到可见的 x=2..3");
    CHECK_MSG(s_dev.dirty, "填充应置脏");

    s_dev.dirty = false;
    dev_display_fill(&s_dev, 3, 0, 0xFFFFU, 1, DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(s_pixels[3] == DEV_DISPLAY_COLOR_GREEN && s_pixels[0] == 0,
              "x+w 在 uint16 回绕时必须按 32 位比较裁剪");

    /* 压制期间 fill 也要走 frame_touched */
    s_dev.dirty = false;
    dev_display_frame_begin(&s_dev);
    dev_display_fill(&s_dev, 0, 0, 1, 1, DEV_DISPLAY_COLOR_BLUE);
    CHECK_MSG(!s_dev.dirty && s_dev.frame_touched, "压制期间 fill 不得直接置脏");
    dev_display_frame_end(&s_dev);
    CHECK_MSG(s_dev.dirty, "end 后应置脏");
}

/** @brief 位图：bit=0 不改原像素、右侧裁剪后行 stride 仍按原 w、越界起点跳过 */
static void case_bitmap(void)
{
    TEST_BEGIN("位图：bit=0 保留原像素；右侧裁剪不改变源行 stride");

    fixture_reset();
    dev_display_fill(&s_dev, 0, 0, 4, 2, DEV_DISPLAY_COLOR_BLACK);
    s_dev.dirty = false;

    /* w=12（每行 2 字节），x=8 远超 4 列 → 起点越界，整张跳过 */
    static const uint8_t bm[4] = {0xAA, 0x55, 0xAA, 0x55};
    dev_display_draw_bitmap(&s_dev, 8, 0, 12, 2, bm, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "x 越界应整张跳过（不得置脏）");

    /* 改小 x 到屏内、w=12：可见列只有 x..3。源第 0 行 bit7.. 对应 col0.. */
    dev_display_draw_bitmap(&s_dev, 2, 0, 12, 2, bm, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(s_pixels[2] == DEV_DISPLAY_COLOR_RED && s_pixels[3] == DEV_DISPLAY_COLOR_BLACK,
              "bit=1 写色、bit=0 保留原像素（%u/%u）", (unsigned)s_pixels[2],
              (unsigned)s_pixels[3]);
    CHECK_MSG(s_dev.dirty, "位图应置脏");

    /* 第二行源字节 = bm[2..3]（stride 按原始 w=12 算，不能因裁剪成 2 列而读错） */
    s_dev.dirty = false;
    dev_display_draw_bitmap(&s_dev, 2, 1, 12, 1, &bm[2], DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(s_pixels[4 + 2] == DEV_DISPLAY_COLOR_GREEN, "第二行应按原 stride 取到 bm[2] 的 bit7");

    s_dev.dirty = false;
    dev_display_draw_bitmap(&s_dev, 0, 0, 4, 2, NULL, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(!s_dev.dirty, "空位图必须直接忽略（不得置脏）");
}

static void *_scan_thread_body(void *arg)
{
    _scan_task(arg);
    return NULL;
}

/** @brief 扫描任务：帧首才 prepare、行号循环、OE/LAT 原子窗口、节拍统计 */
static void case_scan_task(void)
{
    TEST_BEGIN("扫描任务：帧首 prepare / 行循环 / OE+LAT 窗口 / 节拍统计");

    fixture_reset();
    _make_dev(3);
    s_script_dev = &s_dev;
    s_dev.dirty  = true;

    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _scan_thread_body, &s_dev) == 0,
              "扫描任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "扫描任务未退出");

    CHECK_MSG(s_scan_task_id == (osThreadId_t)(uintptr_t)0x5A5AU, "任务体内应登记自身句柄");
    CHECK_MSG(s_start_cnt == 2, "应启动 TIM3/TIM4 两个定时器（%d）", s_start_cnt);
    CHECK_MSG(s_scan_cnt == 7, "脚本喂了 7 拍（%d）", s_scan_cnt);
    CHECK_MSG(s_scan_line_seen[0] == 0 && s_scan_line_seen[1] == 1 && s_scan_line_seen[2] == 2 &&
                  s_scan_line_seen[3] == 0 && s_scan_line_seen[4] == 1 &&
                  s_scan_line_seen[5] == 2 && s_scan_line_seen[6] == 0,
              "行号应按 0→1→2 循环两轮半（%d/%d/%d/%d/%d/%d/%d）", s_scan_line_seen[0],
              s_scan_line_seen[1], s_scan_line_seen[2], s_scan_line_seen[3],
              s_scan_line_seen[4], s_scan_line_seen[5], s_scan_line_seen[6]);
    CHECK_MSG(s_prepare_cnt == 2, "只在帧首（line 0）且 dirty 时 prepare（%d 次）", s_prepare_cnt);
    CHECK_MSG(s_prepare_at[0] == 0 && s_prepare_at[1] == 3,
              "prepare 应发生在第 1、4 拍（scan 前），实际 %d/%d", s_prepare_at[0],
              s_prepare_at[1]);
    CHECK_MSG(s_set_row_cnt == 7 && s_latch_cnt == 7, "每拍都应 set_row + latch（%d/%d）",
              s_set_row_cnt, s_latch_cnt);
    CHECK_MSG(s_kernel_lock_cnt == 7 && s_kernel_unlock_cnt == 7,
              "OE/LAT 窗口应成对锁内核（%d/%d）", s_kernel_lock_cnt, s_kernel_unlock_cnt);
    CHECK_MSG(s_irq_disable_cnt == 7 && s_irq_enable_cnt == 7, "PWM IRQ 应成对开关（%d/%d）",
              s_irq_disable_cnt, s_irq_enable_cnt);
    CHECK_MSG(s_oe_blank_cnt >= 7, "每拍都要先消隐（%d）", s_oe_blank_cnt);

    uint32_t max_gap = 0, overruns = 0;
    dev_display_scan_stats(&max_gap, &overruns);
    CHECK_MSG(max_gap == 7U, "最大唤醒间隔应为 7ms（%u）", (unsigned)max_gap);
    CHECK_MSG(overruns == 1U, ">4ms 的超期应计 1 次（%u）", (unsigned)overruns);

    dev_display_scan_stats(NULL, NULL); /* 可空指针 */
}

/* ================================================================ */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36m显示设备基类（注册/裁剪/帧纪律/扫描任务）\033[0m\n");

    case_register_color();
    case_init();
    case_pwm();
    case_start();
    case_set_pixel();
    case_fill();
    case_bitmap();
    case_scan_task();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
