/**
 * @file    app_factory_test.c
 * @brief   出厂检测模式 — monitor 监听 TEST 驱动状态机
 */

#include "app_factory_test.h"

#include "board.h" /* BOARD_CASC_ENABLED —— 必须先于下面的条件包含 */

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"
#include "dev_key.h"
#if BOARD_CASC_ENABLED
#include "app_casc.h" /* 首次按键时认领主卡（单卡板不引：那一档没有级联） */
#endif
#include "app_render.h"
#include "app_screen.h" /* app_screen_rows/cols/layout/card：渲染用**逻辑屏**几何与切分表，
                          * 不是实屏（程序码要按卡分带、不骑缝） */
#include "app_dispatch.h"
#include "app_light_sensor.h"
#include "pl_task.h"

#define AGING_TEXT   "重庆创迪科技发展有限公司设备老化测试"
#define PROGRAM_CODE "9210209C41"

static const app_font_size_t s_aging_sizes[] = {
    APP_FONT_SIZE_16,
    APP_FONT_SIZE_24,
    APP_FONT_SIZE_32,
};
static const app_font_type_t s_aging_types[] = {
    APP_FONT_TYPE_ST,
    APP_FONT_TYPE_FS,
    APP_FONT_TYPE_KT,
    APP_FONT_TYPE_HT,
};
#define AGING_SIZE_COUNT (sizeof(s_aging_sizes) / sizeof(s_aging_sizes[0]))
#define AGING_TYPE_COUNT (sizeof(s_aging_types) / sizeof(s_aging_types[0]))

/* 长等待的分步轮询步长：决定 abort（见 s_factory_abort_req / _wait_press_or_abort）
 * 的**最坏响应延迟** —— 一次等待动作最多晚这么久让出。 */
#define FACTORY_POLL_STEP_MS (50U)

static osThreadId_t s_factory_test_task_handle;

/** 工厂测试是否正在进行（IDLE 之外的所有阶段）。
 *  置位/清零都在 _factory_monitor_task 内，app_factory_mode_interrupt 只读它 ——
 *  用它把"每收一包数据"的路径挡在"重复置 abort 请求"之外。 */
static volatile bool s_factory_active;

/** @brief "请工厂任务自行退出"的请求位（见 app_factory_mode_interrupt 的说明）。
 *
 *  中断路径**只置位、不异步终止任务** —— 这是协作式自退的握手点。工厂任务在每个
 *  安全点（长等待的每步、以及两次渲染之间）检查它，自行走统一收尾回 IDLE。
 *  由 `_factory_cleanup` 在收尾的最后一步清零。 */
static volatile bool s_factory_abort_req;

/* ---- 老化辅助 ---- */
/** @brief 清屏（走**逻辑屏**：多卡时是整台设备，单卡时就是本卡）
 *
 *  **它必须与紧随其后的那次渲染一起**被 `dev_display_frame_begin/end` 包住 ——
 *  单独一次清屏会让屏上闪一帧全黑；包住之后只输出最终那一帧。
 *  两处调用点（SHOW_CODE、老化逐字）都是这个写法。 */
static void _clear_screen(void)
{
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
}

/** @brief 丢弃 TEST 键**已经累积**的信号量令牌（非阻塞排空）
 *
 *  **为什么需要它**：机械按键一次按压/释放可能产生**不止一个**下降沿
 *  （`dev_key.c` 的去抖窗口只有 50ms，而实测抖动/释放回弹可以更晚），而
 *  EXTI 回调是**每个边沿都释放一次**信号量（上限 1）。于是上一次操作会留下
 *  一个"残留令牌"，被下一次 `dev_key_wait_press` 立刻消费。
 *
 *  现场症状 A：进入 SHOW_CODE 后，紧随的"等第二下"立刻返回，红色
 *  DEAD_PIXEL 当场盖掉程序码 —— 排障结论就是那次等待消费了这次按键留下的
 *  残留令牌（老代码只在进 AGING 轮播前排空，两处不对称，本函数补齐）。
 *
 *  语义：**进入一个新的等待阶段前，先丢弃之前累积的计数**。正常单击
 *  （只有一个令牌、且已被上一次 wait 消费）不受影响；本函数不阻塞、不等待新按键。 */
static void _drain_test_tokens(void)
{
    while (dev_key_wait_press(DEV_KEY_TST, 0)) {}
}

/* ---- "等键稳定释放"的静默窗与步进 ----
 *
 * **为什么需要"稳定"而不只是"排空"**：`_drain_test_tokens` 是**时点**排空 —— 只丢弃
 * 调用那一刻已累积的令牌，挡不住**调用之后**才到达的尾随边沿。而慢按（边沿间隔 > 去抖
 * 窗口 50ms）的尾随下降沿，恰好会落进"渲染第一帧（读字库 SPI，几十 ms）→ 带超时等待"
 * 这个窗口：等待立刻返回 true → 退出轮播；若还有第二个尾随沿，IDLE 的等待再消费一个 →
 * 又回程序码。所以要在**进新阶段前**等键真正松开并安静一小段。
 *
 * `TEST_SETTLE_MS` 是"慢按尾随边沿"的静默窗：键必须连续这么久**未被读到按下**才认稳定。
 * 取值 150ms（> 去抖窗 50ms 的 3 倍），留出释放回弹的余量；据现场表现可调。 */
#define TEST_SETTLE_MS      (150U)
#define TEST_SETTLE_STEP_MS (10U)   /* 轮询步进：比静默窗小一个量级，不误判 */
#define TEST_SETTLE_MAX_MS  (1000U) /* 硬上界：键卡住/长按也不能把流程等死 */

/** @brief 进新阶段前，等 TEST 键**已释放且稳定**，再排空一次令牌
 *
 *  语义：**排空 → 轮询等键"已释放"并连续静默 `TEST_SETTLE_MS` → 再排空一次**。
 *  两次排空把"静默窗外累积的"和"静默窗内到达的"尾随令牌都清掉；中间那段稳定等待
 *  则是主动把慢按的尾随边沿**等过去**，而不是被它打断下一个等待。
 *
 *  **有界**：整段最多等 `TEST_SETTLE_MAX_MS`；超界就（再排空后）继续往下走，不在这里
 *  死等 —— 键卡住或用户长按都只是多一次排空，绝不会让工厂流程停住。超界不打日志：
 *  它是"键被按住"的常态，每进一个阶段都刷一行只会淹没现场输出。
 *
 *  **也查 abort**：本段最长 1s，若不查 `s_factory_abort_req`，abort 的响应上界就会被
 *  它拉长到 1s、破坏"一个轮询步长"的承诺；收到请求即提前退出，交给调用方走收尾。 */
static void _settle_test_key(void)
{
    _drain_test_tokens(); /* ① 先丢弃调用点之前已累积的令牌 */

    const uint32_t t0          = osKernelGetTickCount();
    uint32_t       quiet_since = t0; /* "最近一次读到按下"的时刻；未按下则一直是 t0 */

    while (osKernelGetTickCount() - t0 < TEST_SETTLE_MAX_MS) {
        if (s_factory_abort_req) break; /* 被要求中止：不再等静默窗，尽早让出 */
        if (dev_key_get_state(DEV_KEY_TST)) {
            /* 仍按下（或释放回弹被读到）：静默窗重新起算 */
            quiet_since = osKernelGetTickCount();
        } else if (osKernelGetTickCount() - quiet_since >= TEST_SETTLE_MS) {
            break; /* 已释放且连续静默满窗 → 稳定 */
        }
        osDelay(TEST_SETTLE_STEP_MS);
    }

    /* ② 再排空一次：把静默窗内到达的尾随令牌一并丢弃 */
    _drain_test_tokens();
}

/** @brief 分步等待 TEST 按下，每步检查 abort 请求
 *
 *  **为什么分步**：`dev_key_wait_press(…, osWaitForever/3000)` 一睡到底 —— 协作式自退
 *  期间 `s_factory_abort_req` 置位也唤不醒它：老化那处最多再等 3000ms 超时，其余
 *  `osWaitForever` 处则**永远等不到**（除非用户恰好按键）。拆成 `FACTORY_POLL_STEP_MS`
 *  的小步后，abort 的**响应上界 = 一个步长**。
 *
 *  语义与原 API 一致：该等按键的照等（按到就返回 true），只是多了 abort 检查。
 *  @param timeout_ms `osWaitForever` = 一直等；否则累计最多等这么久
 *  @return true = 按到 TEST；false = 超时**或**被要求中止（调用方据 `s_factory_abort_req` 区分） */
static bool _wait_press_or_abort(uint32_t timeout_ms)
{
    const uint32_t t0 = osKernelGetTickCount();
    for (;;) {
        if (s_factory_abort_req) return false;
        if (dev_key_wait_press(DEV_KEY_TST, FACTORY_POLL_STEP_MS)) return true;
        if (timeout_ms != osWaitForever && (osKernelGetTickCount() - t0) >= timeout_ms) return false;
    }
}

/** @brief 统一收尾：正常退出（老化里按 TEST）与 abort 退出（上位机来帧）共用
 *
 *  唯一差异是**是否清屏**：
 *   · 正常退出清**逻辑屏**（多卡时两块一起清空）——保持原有行为；
 *   · abort **不清屏**：收到帧本身就说明上位机在通信，它随后的显示命令会重画；这里再清
 *     一次只会让屏多闪一帧黑，且清出来的也不是上位机要的最终画面。
 *
 *  **颜色覆盖必须在这里取消**（正常与 abort 两条路都要）：DEAD_PIXEL 逐色设过
 *  `app_screen_set_color_override(c)`，不取消的话残留覆盖 = 那一轮的颜色，下次渲染的
 *  程序码整块被染成那个色（现场症状：重置回程序码、且用的是那一轮的颜色）。
 *
 *  两处复位都幂等：正常路径上覆盖早已取消、光感任务早已恢复，重复一遍无副作用。
 *  `s_factory_abort_req` **最后清** —— 清掉才算真正回到可再次响应的 IDLE。 */
static void _factory_cleanup(bool aborted)
{
    app_screen_set_color_override(0xFF);        /* 幂等：正常路径此处已复位过 */
    osThreadResume(g_light_sensor_task_handle); /* 幂等：未挂起是空操作 */
    if (!aborted) {
        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_FILL,
            .x     = 0,
            .y     = 0,
            .color = DEV_DISPLAY_COLOR_BLACK,
        });
    }
    s_factory_active    = false;
    s_factory_abort_req = false;
}

/** @brief 在**每张卡**的矩形内各居中渲染一份程序码
 *
 *  **为什么不沿用"整屏逻辑坐标垂直居中"**：5006048 是 1×2 双卡、逻辑屏
 *  224×100，两块实屏的缝在 y=50；16px 高的程序码整屏居中后落在 y=42..58，
 *  **骑在缝上**，每块实屏只剩 8 行 —— 症状 B。工厂测试的目的是"技术员在每块
 *  卡上都能读到程序码"，所以按切分表逐卡分带渲染。
 *
 *  **不改 app_screen/app_render 语义**：级联下发的仍是"整块画布按卡抽带"，
 *  主卡把每张卡那一份渲染进各自矩形，抽带时各得其所 —— 与既有契约一致。
 *  单卡时切分表只有一项、且矩形 == 整屏，逐卡渲染等价于旧的一整份居中。 */
static void _show_program_code(void)
{
    const app_screen_layout_t *L = app_screen_layout();
    if (!L || L->count == 0) {
        /* 兜底：门面停用（显示未就绪/地址不在表里）时按整屏逻辑几何渲染一份，
           与加级联之前的行为一致（app_screen_rows/cols 会回落到本卡实屏几何）。 */
        app_render(&(app_render_cfg_t){
            .type      = APP_RENDER_TYPE_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = app_screen_rows(),
            .h         = app_screen_cols(),
            .color     = DEV_DISPLAY_COLOR_GREEN,
            .text      = PROGRAM_CODE,
            .len       = strlen(PROGRAM_CODE),
            .font_size = APP_FONT_SIZE_16,
            .font_type = APP_FONT_TYPE_ST,
            .text_enc  = APP_FONT_ENC_UTF8,
            .style     = &(app_render_style_t){
                .h_align = APP_RENDER_ALIGN_CENTER,
                .v_align = APP_RENDER_ALIGN_CENTER,
            },
        });
        return;
    }

    for (uint8_t i = 0; i < L->count; i++) {
        const app_screen_card_t *c = app_screen_card(i);
        if (!c) continue;
        /* 在卡自己的矩形内居中：不骑缝，每块实屏各得一份完整程序码。 */
        app_render(&(app_render_cfg_t){
            .type      = APP_RENDER_TYPE_TEXT,
            .x         = c->x,
            .y         = c->y,
            .w         = c->w,
            .h         = c->h,
            .color     = DEV_DISPLAY_COLOR_GREEN,
            .text      = PROGRAM_CODE,
            .len       = strlen(PROGRAM_CODE),
            .font_size = APP_FONT_SIZE_16,
            .font_type = APP_FONT_TYPE_ST,
            .text_enc  = APP_FONT_ENC_UTF8,
            .style     = &(app_render_style_t){
                .h_align = APP_RENDER_ALIGN_CENTER,
                .v_align = APP_RENDER_ALIGN_CENTER,
            },
        });
    }
}

static void _aging_fill_screen(app_font_size_t size, app_font_type_t type, const char *ch_utf8, uint8_t ch_len)
{
    /* **一律用逻辑屏几何**（`app_screen_rows/cols`），不用实屏（`dsp->screen_*`）：
       多卡时逻辑屏是整台设备（如 224×100），实屏只是本卡那半幅（224×50）。
       用实屏的话内容会落到整屏的左上角那一格 —— 也就是**从卡那一格**，
       于是"主卡按一下、整设备一起老化"变成"主卡自己黑屏、从卡显示内容"。

       `app_screen_rows/cols` 在门面停用（单卡、地址不在表里）时会回落到本卡屏几何，
       所以这段在哪种形态下都对。 */
    const uint16_t sw = app_screen_rows();
    const uint16_t sh = app_screen_cols();

    uint8_t cols = (uint8_t)(sh / size);
    uint8_t rows = (uint8_t)(sw / size);
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    /* 清屏也走逻辑屏（多卡主卡上"本卡实屏缓冲"与画布是两回事） ——
       与紧随的文字**当成一帧**输出，见 _clear_screen 的说明 */
    dev_display_frame_begin(dev_display_get());
    _clear_screen();

    /* 把字符重复 cols×rows 份放缓冲区，word_wrap 自动分行 */
    static char buf[256];
    uint16_t pos   = 0;
    uint16_t count = cols * rows;
    while (count--) {
        memcpy(buf + pos, ch_utf8, ch_len);
        pos += ch_len;
    }

    app_render(&(app_render_cfg_t){
        .type      = APP_RENDER_TYPE_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = sw,
        .h         = sh,
        /* 老化文字用白：全彩模组是三通道全亮；只有 R/G 灯珠的模组（P20）上
           B 位没有灯珠，物理呈现就是黄 —— 位与即物理现实，不必按能力特判。 */
        .color     = DEV_DISPLAY_COLOR_WHITE,
        .text      = buf,
        .len       = pos,
        .font_size = size,
        .font_type = type,
        .text_enc  = APP_FONT_ENC_UTF8,
        .style     = &(app_render_style_t){
            .h_align   = APP_RENDER_ALIGN_CENTER,
            .v_align   = APP_RENDER_ALIGN_CENTER,
            .word_wrap = true,
        },
    });
    dev_display_frame_end(dev_display_get());
}

/* ================================================================
 *  _factory_monitor_task
 * ================================================================ */

static void _factory_monitor_task(void *argument)
{
    (void)argument;

    for (;;) {
        /* IDLE: 等待 TEST 激活。
           **先等键稳定释放再挂起等待**：上一轮可能留下"按下"的尾随令牌 —— 尤其是
           协作式自退（见 app_factory_mode_interrupt）把任务从半途放回本函数、重进 IDLE
           时，裸的 `osWaitForever` 会立刻消费该令牌、跳过 IDLE 直接又进 SHOW_CODE。现场
           表现为"上位机一来帧就随机重置成程序码"（见 _settle_test_key 的语义与上界）。 */
        _settle_test_key();
        if (!_wait_press_or_abort(osWaitForever)) {
            /* IDLE 期 interrupt 因 !s_factory_active 直接返回，正常不会收到 abort；
               万一有陈旧标志就清掉重试，避免忙转。 */
            s_factory_abort_req = false;
            continue;
        }

        s_factory_active = true;
        bool aborted     = false; /* abort 退出标记：供统一收尾区分"清不清屏" */

        /* ===== 主从识别（并入"第一次按键"这一步）=====
         * 用户定的口径：老化测试走 render API、而 render 已兼容级联，所以按主卡
         * 一个键就能触发整设备老化 —— **不必区分老化与主副卡识别**。
         *
         * **只投递请求**：写身份记录要几十毫秒、发识别帧要占 s_tx_buf 并等 ACK（最长约
         * 1s）——那些必须由**级联任务**做（s_tx_buf 与轮次都是它的）。本任务是
         * _factory_monitor_task，而 TEST 键只有它一个消费者，在这里投递不会冲突。 */
#if BOARD_CASC_ENABLED
        app_casc_claim_master();
#endif

        /* ===== SHOW_CODE ===== */
        /* 进入本阶段先排空残留令牌（见 _drain_test_tokens 的说明）。
           真正关键的一道在**渲染之后、等第二下之前**（下面的 _settle_test_key）——
           因为残留多半是这次按压的抖动/回弹在渲染期间凑出来的第二、三个下降沿。 */
        _drain_test_tokens();

        /* 清屏与文字**都走逻辑屏**（多卡时是整台设备的屏，单卡时就是本卡）
           —— 用实屏几何的话，内容会整块落到左上那一格（多卡时就是从卡那一格）。
           两步**当成一帧**输出：中间不让 _scan_task 跑 prepare（见 dev_display.h）。 */
        dev_display_frame_begin(dev_display_get());
        _clear_screen();
        /* 逐卡分带渲染：程序码落在每块实屏各自的矩形内居中，不再骑在 1×2 的缝上
           （症状 B）。单卡时切分表只有一项、矩形 == 整屏，与旧行为等价。 */
        _show_program_code();
        dev_display_frame_end(dev_display_get());

        /* **关键的一道"等键稳定释放"**：单单排空是**时点**操作，挡不住"调用之后才到"的
           尾随边沿。渲染 SHOW_CODE 要读字库（SPI）、多卡时还要走级联，慢按（边沿间隔 >
           去抖窗 50ms）的尾随沿正好落进"渲染 → 等第二下"这段窗口；它若被紧随的等待
           消费，红色 DEAD_PIXEL 就当场盖掉程序码（症状 A）。所以这里不排空、而是等键
           松开安静一小段 —— 见 _settle_test_key 的语义与上界。 */
        _settle_test_key();
        /* 安全点：SHOW_CODE 的 frame_begin/end 已闭合，此处无半途状态。
           abort 时 helper 在一个轮询步长内返回 false → 交统一收尾。 */
        if (!_wait_press_or_abort(osWaitForever)) {
            aborted = true;
            goto factory_done;
        }

        /* ===== DEAD_PIXEL ===== */
        osThreadSuspend(g_light_sensor_task_handle);
        /* 亮度走**整屏**那个入口：`dev_display_set_brightness` 只设本卡实屏，
           从卡拿到的仍是每轮 IMAGE 里带的旧亮度 → 一块亮一块暗。
           `app_screen_set_brightness` 本地 + 置"待下发"，由级联广播给从卡。 */
        app_screen_set_brightness(7);
        /* 逐色满屏点亮，清单**由本模组的颜色能力决定**（`dev_display_supports_color`）：
           P20 只有 R/G 灯珠 → 红/绿/黄 三色，与旧硬编码清单逐项相同（3833024 行为不变）；
           全彩模组（P10）→ 红/绿/黄/蓝/紫/青/白 七色全轮一遍（黑 = 灭，不点）。
           旧清单是 P20 时代写死的三色 —— 在全彩模组上**蓝灯珠永远点不亮、坏点查不出来**，
           死点检测会漏掉一整个通道。 */
        const dev_display_t *dsp = dev_display_get();
        for (uint8_t c = (uint8_t)DEV_DISPLAY_COLOR_RED; c <= (uint8_t)DEV_DISPLAY_COLOR_WHITE; c++) {
            if (!dev_display_supports_color(dsp, (dev_display_color_t)c)) continue;

            /* 颜色要**覆盖**：画布是 1bpp（只记亮/灭），颜色在协议里是逐卡给的
               （来自切分表）—— 不覆盖的话纯色会全显示成每块屏自己的那个颜色。
               覆盖之后两块屏一起按这个颜色亮（单卡就是本卡那块）。 */
            app_screen_set_color_override(c);
            app_render(&(app_render_cfg_t){
                .type  = APP_RENDER_TYPE_FILL,
                .x     = 0,
                .y     = 0,
                .color = (dev_display_color_t)c,
            });
            /* 每色等一次 TEST：技术员按一下换下一色（色数随模组能力可变）。
               安全点：上面是单笔 FILL（无 frame 包裹），等待返回即已离开它。 */
            if (!_wait_press_or_abort(osWaitForever)) {
                aborted = true;
                goto factory_done;
            }
        }
        app_screen_set_color_override(0xFF); /* 取消覆盖：后面的老化轮播用各卡自己的颜色 */

        /* ===== AGING ===== */
        osThreadResume(g_light_sensor_task_handle);

        /* 进衰老轮播前等键稳定释放（原为一次时点排空，慢按挡不住，改成主动等）：
           上面 DEAD_PIXEL 段用的是 osWaitForever，慢按的尾随沿会在"渲染第一个字 →
           下面第一次 wait_press(…, 3000)"之间到达并被立刻消费 —— 表现为"只显示第一个
           字就退出轮播并清屏"。等键松开安静一小段，让尾随沿先到、再被排空清掉。
           见 _settle_test_key 的语义与上界。 */
        _settle_test_key();
        /* 安全点：settle 最多分步等 1s，abort 时会提前退出，此处无半途状态。 */
        if (s_factory_abort_req) {
            aborted = true;
            goto factory_done;
        }

        bool aging_exit = false;
        for (uint8_t type_idx = 0; !aging_exit; type_idx = (type_idx + 1) % AGING_TYPE_COUNT) {
            for (uint8_t size_idx = 0; size_idx < AGING_SIZE_COUNT; size_idx++) {
                app_font_size_t fsize = s_aging_sizes[size_idx];
                /* 字号能不能放下，判的是**逻辑屏**（多卡时整台设备更大，
                   大字在上面才排得开） */
                if (fsize > app_screen_rows() || fsize > app_screen_cols()) continue;

                const char *ch_ptr = AGING_TEXT;
                while (*ch_ptr) {
                    uint8_t ch_len    = ((uint8_t)*ch_ptr >= 0xE0) ? 3 : 1;
                    char single_ch[4] = {ch_ptr[0], ch_len > 1 ? ch_ptr[1] : 0,
                                         ch_len > 2 ? ch_ptr[2] : 0, 0};

                    _aging_fill_screen(fsize, s_aging_types[type_idx], single_ch, ch_len);

                    /* 安全点：_aging_fill_screen 内部已 frame_end，渲染已完成，
                       此处不在任何 app_render / frame 之内。 */
                    if (_wait_press_or_abort(3000)) {
                        aging_exit = true;
                        break;
                    }
                    /* abort 而非超时：不再推进下一个字，交统一收尾（超时才继续）。 */
                    if (s_factory_abort_req) {
                        aborted = true;
                        goto factory_done;
                    }
                    ch_ptr += ch_len;
                }
                if (aging_exit) break;
            }
            if (aging_exit) break;
        }

factory_done:
        /* 统一收尾：正常退出（老化里按 TEST）与 abort 退出共用一条路径，
           `aborted` 只决定是否清屏 —— 见 _factory_cleanup。 */
        _factory_cleanup(aborted);
    }
}

/* ---- 模块自注册 ---- */
static void _factory_test_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "factory_monitor",
        .stack_size = 512 * 4,
        .priority   = osPriorityBelowNormal,
    };
    s_factory_test_task_handle = pl_task_new(_factory_monitor_task, NULL, &attr);
}

static void _factory_module_init(void)
{
    /* 框架不再直接认识本模块 —— 收到数据就退出工厂模式这件事，
       由本模块自己注册监听（见 app_dispatch_register_rx_listener）。 */
    app_dispatch_register_rx_listener(app_factory_mode_interrupt);
    _factory_test_init();
}
sw_app_initcall(_factory_module_init);

/**
 * @brief 请求退出工厂测试模式，回到 IDLE（**协作式自退**）
 *
 *  **只置 `s_factory_abort_req`，不在这里动线程**。异步 `osThreadTerminate` 会从半途
 *  删除工厂任务，留下渲染互斥量锁死 / `dirty_hold` 永久为真 / 光感任务永久挂起三类
 *  不可恢复的损坏（host 无工厂 harness 可覆盖，故长期潜伏）——详见函数体内注释。
 *  由 `_factory_monitor_task` 在**安全点**自行走统一收尾回 IDLE；线程只在启动时创建
 *  一次，"终止—重建"的 churn 一并消失。
 */
void app_factory_mode_interrupt(void)
{
    if (!s_factory_active) return; /* 已在 IDLE：无可中断 */

    /* **为什么不能异步终止（本函数以前的做法）**：本函数挂在"收到任意有效非 internal_bus
       帧"的路径上，会在工厂流程**半途**（正在渲染、正在逐色老化、或 frame 未闭合时）删掉
       工厂任务。FreeRTOS 的 `vTaskDelete` 立即回收任务、**不释放**它持有的资源：
         · 若删除点落在 `app_render()` 持 `s_render_lock` 期间（渲染逐字读字库 SPI、单笔
           持久化还会写 W25Qxx，窗口几十~上百 ms），该 mutex 永久锁死 —— 之后**任何**任务
           的渲染都在 `osMutexAcquire(…, osWaitForever)` 上永久阻塞；
         · 若删除点落在 `dev_display_frame_begin..end` 之间，`dirty_hold` 永久为真 →
           `_scan_task` 再也不认脏帧，**屏永久冻结**；
         · 若删除点落在 DEAD_PIXEL 的"挂起光感任务"与"恢复"之间，光感任务**永久挂起**。
       本函数**只置请求位**：工厂任务在安全点（长等待拆成小步、每步检查；以及渲染 / frame
       闭合之后）自行走到统一收尾，把颜色覆盖、光感挂起、`s_factory_active` 全部按正常
       路径复位。**响应上界 = 一个 `FACTORY_POLL_STEP_MS`**（settle 段也查该标志）。
       这里也不做屏幕复位：收尾由工厂任务自己完成，避免"中断方与任务方"两处改同一状态。 */
    s_factory_abort_req = true;
}
