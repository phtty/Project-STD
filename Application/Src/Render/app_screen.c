/**
 * @file    app_screen.c
 * @brief   整屏门面实现 —— 切分表/几何、卡状态、落屏、亮度、身份
 *
 * 见 app_screen.h 的设计说明。逻辑几何来自切分表（单卡时即本屏几何）。
 * 画布那一簇（1bpp 缓冲、渲染目标 sink、抽带、静默期提交、显存持久化）已拆到
 * app_screen_canvas.c —— 本文件只保留"与画布无关"的门面职责。
 *
 * 由 board.h 的 BOARD_SCREEN_CANVAS 开关：多卡级联的板子打开（5006048 即如此），
 * 单独一块卡上，画布是纯开销 —— 它多占一块 1bpp 缓冲、多一次拷贝，还要把卡内多色
 * 塌缩成单色，却没有换来任何功能。中间靠 `app_render_set_target(NULL)` 即可完全回退。
 */

#include "app_screen.h"

#include <stdio.h>
#include "board.h" /* BOARD_SCREEN_CANVAS / _COLOR / BOARD_CASC_* */
#include "app_render.h"
#include "dev_display.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_task.h"

/* 几何（整屏逻辑宽高）与身份/亮度/卡状态都归门面；画布自己的缓冲与 stride 在
   app_screen_canvas.c。s_rows/s_cols 为 0 表示门面停用，此时回落到本卡实屏几何。 */
static dev_display_t *s_display_dev;
static uint16_t       s_rows;     /* **整屏**逻辑宽（单卡时 == 本屏宽） */
static uint16_t       s_cols;     /* 整屏逻辑高 */
static uint8_t        s_self_idx; /* 本卡在切分表里的下标 */

/* ---- 哪一格是主卡格：**运行期事实**（出厂默认来自 board.h）----
 *
 * 它 + 网格形状就是整张切分表：主卡格编 addr 0，其余格按网格顺序编 1..N。
 * 允许运行期改的理由见 app_screen.h —— 编译期钉死会让"谁被按谁主卡"变成
 * "被按的那张卡以为自己在另一块屏上"（两块屏的上下半幅对调）。 */
static uint8_t s_master_cell = (uint8_t)BOARD_CASC_MASTER_CELL;

uint8_t app_screen_master_cell(void)
{
    return s_master_cell;
}

/** @brief 格号 ↔ 地址：**整张切分表就这一条规则**（与 _layout_build_grid 同源）
 *
 *  纯函数，不依赖当前表 —— 认领时要同时算"旧表里你在哪"和"新表里你去哪"，
 *  而此刻表只能有一份。 */
uint8_t app_screen_addr_of_cell(uint8_t cell, uint8_t master_cell)
{
    if (cell == master_cell) return 0;
    return (uint8_t)(cell < master_cell ? cell + 1U : cell);
}

uint8_t app_screen_cell_of_addr(uint8_t addr, uint8_t master_cell)
{
    if (addr == 0) return master_cell;
    return (uint8_t)(addr <= master_cell ? addr - 1U : addr);
}

/* ================================================================
 *  切分表 —— 本期由 board.h 的网格参数合成
 *
 *  **整屏 = COLS×ROWS 张等尺寸卡**，每张卡占一整块"本卡屏几何"大小的矩形，
 *  地址 = 网格下标（行优先），所以地址 0 恒在网格原点（主卡 = 左上）。
 *
 *  合成而不是写一张常量表：单卡几何是**运行期**才知道的（dev_display_t 没有
 *  编译期几何宏，换模组只需改 board.mk 一行）—— 写死一张表，换屏就得同步改表，
 *  而不同步的表现是画面错位，不报错。
 *
 *  后续期这里改成"先查 W25Qxx 的切分表记录，没有才回落本网格" —— 那时才谈得上
 *  异形拼法与现场改址。 */
static app_screen_card_t   s_card_table[APP_SCREEN_CARD_MAX];
static app_screen_layout_t s_screen_layout = {.cards = s_card_table, .count = 0, .rows = 0, .cols = 0};

const app_screen_layout_t *app_screen_layout(void)
{
    return &s_screen_layout;
}

/* 门面停用（显示未就绪 / 地址不在表里）时 s_rows/s_cols 还是 0，回落到本卡屏几何 ——
   与加级联之前逐字相同，免得"门面一停用，渲染矩形就成了 0×0"。 */
uint16_t app_screen_rows(void)
{
    if (s_rows) return s_rows;
    const dev_display_t *d = dev_display_get();
    return d ? d->screen_rows : 0;
}

uint16_t app_screen_cols(void)
{
    if (s_cols) return s_cols;
    const dev_display_t *d = dev_display_get();
    return d ? d->screen_cols : 0;
}

const app_screen_card_t *app_screen_card(uint8_t card_idx)
{
    return (card_idx < s_screen_layout.count) ? &s_screen_layout.cards[card_idx] : nullptr;
}

/* ================================================================
 *  卡片状态与整屏状态快照
 * ================================================================ */

/* 最近一轮序号：**只存低 8 位**，只为日志对照，不参与任何判断 */
static uint8_t s_status_seq_lo;

static app_screen_alarm_fn_t s_alarm_fn;
static uint16_t              s_retrans_cnt;
static uint8_t               s_evict_cnt;
static uint8_t               s_last_alarm;

app_screen_card_state_t app_screen_card_state(uint8_t card_idx)
{
    const app_screen_card_t *c = app_screen_card(card_idx);
    return c ? (app_screen_card_state_t)c->state : APP_SCREEN_CARD_STATE_MISSING;
}

void app_screen_card_set_state(uint8_t card_idx, app_screen_card_state_t st)
{
    /* **必须走 s_card_table[] 而不是 s_screen_layout.cards** —— 后者是 const 指针（表对外只读），
       从这里改会被编译器拦下（或悄悄写进只读段）。 */
    app_screen_card_t *c = (card_idx < s_screen_layout.count) ? &s_card_table[card_idx] : nullptr;
    if (!c || c->state == (uint8_t)st) return; /* 没变就不算一次跳变 */

    const uint8_t addr = c->addr;
    c->state           = (uint8_t)st;

    if (st == APP_SCREEN_CARD_STATE_OFFLINE) s_evict_cnt++;
    s_last_alarm = addr;

    /* **不注册就什么都不发生** —— 协议只暴露状态，不决定去向，也不认识任何上层协议。
       要报的产品自己注册；不报的产品一个字节都不产生。 */
    if (s_alarm_fn) s_alarm_fn(addr, (uint8_t)st);
}

void app_screen_register_alarm(app_screen_alarm_fn_t fn)
{
    s_alarm_fn = fn;
}

void app_screen_note_retrans(void)
{
    s_retrans_cnt++;
}

void app_screen_note_round(uint16_t seq)
{
    s_status_seq_lo = (uint8_t)seq;
}

void app_screen_status(app_screen_status_t *out)
{
    if (!out) return;

    out->seq_lo      = s_status_seq_lo;
    out->online_mask = 0;
    out->retrans_cnt = s_retrans_cnt;
    out->evict_cnt   = s_evict_cnt;
    out->last_alarm  = s_last_alarm;

    /* 位 i ↔ 地址 i+1：地址 0 是本卡，不进掩码。从卡地址上限 0x1F，8 位够放
       （本工程 APP_SCREEN_CARD_MAX=4，实际只用到低 3 位）。 */
    for (uint8_t i = 0; i < s_screen_layout.count; i++) {
        const app_screen_card_t *c = &s_screen_layout.cards[i];
        if (c->addr >= 1U && c->addr <= 8U && c->state == (uint8_t)APP_SCREEN_CARD_STATE_ONLINE)
            out->online_mask |= (uint8_t)(1U << (c->addr - 1U));
    }
}


uint8_t app_screen_index_of_addr(uint8_t addr)
{
    for (uint8_t i = 0; i < s_screen_layout.count; i++)
        if (s_screen_layout.cards[i].addr == addr) return i;
    return 0xFF;
}

uint8_t app_screen_self_index(void)
{
    return app_screen_index_of_addr(app_screen_self_addr());
}

uint16_t app_screen_card_bm_len(uint8_t card_idx)
{
    const app_screen_card_t *c = app_screen_card(card_idx);
    if (!c) return 0;
    return (uint16_t)(((c->w + 7U) / 8U) * c->h);
}

/** @brief 按 nx×ny 的网格合成切分表；本卡屏几何取自运行期的 display
 *
 *  取参数而不是直接读 board.h 的宏：后续期这里是"先查 W25Qxx 的切分表记录、
 *  没有才回落网格"的那条路，届时 nx/ny 来自记录。
 *
 *  @param nx          网格列数（水平方向并排的卡数）
 *  @param ny          网格行数（垂直方向堆叠的卡数）
 *  @param master_cell 主卡所在的**网格下标**（行优先）。
 *
 *  **主卡不必在网格原点** —— 现场的拼法就有"上面一块、下面一块，下面那块是主卡"
 *  的（此时主卡在最后一行）。所以地址不能拿网格下标当：主卡那格编 0，
 *  其余按行优先依次编 1、2、3…。全工程没有任何地方假设主卡在原点
 *  （持久化恢复也按本卡矩形映射，见 app_screen_canvas.c 的 _persist_restore）。 */
static bool _layout_build_grid(uint8_t nx, uint8_t ny, uint8_t master_cell)
{
    const uint16_t cw = s_display_dev ? s_display_dev->screen_rows : 0; /* 单卡屏宽 */
    const uint16_t ch = s_display_dev ? s_display_dev->screen_cols : 0; /* 单卡屏高 */

    if (!cw || !ch) return false;
    if ((uint16_t)nx * ny > APP_SCREEN_CARD_MAX) {
        printf("[screen] 切分 %ux%u 张卡超过 APP_SCREEN_CARD_MAX=%u\n", (unsigned)nx, (unsigned)ny,
               (unsigned)APP_SCREEN_CARD_MAX);
        return false;
    }
    if (master_cell >= (uint8_t)(nx * ny)) {
        printf("[screen] 主卡格号 %u 超出 %ux%u 网格（0..%u）\n", (unsigned)master_cell,
               (unsigned)nx, (unsigned)ny, (unsigned)(nx * ny - 1));
        return false;
    }

    s_screen_layout.count = (uint8_t)(nx * ny);
    s_screen_layout.rows  = (uint16_t)(cw * nx); /* 整屏宽 */
    s_screen_layout.cols  = (uint16_t)(ch * ny); /* 整屏高 */

    uint8_t next_addr = 1; /* 0 留给主卡 */
    for (uint8_t r = 0; r < ny; r++)
        for (uint8_t c = 0; c < nx; c++) {
            const uint8_t i = (uint8_t)(r * nx + c);
            s_card_table[i]      = (app_screen_card_t){
                .addr  = (i == master_cell) ? 0U : next_addr++,
                .color = BOARD_SCREEN_COLOR,
                .x     = (uint16_t)(c * cw),
                .y     = (uint16_t)(r * ch),
                .w     = cw,
                .h     = ch,
                /* 运行期状态：建表时谁都没应答过 —— MISSING 与 OFFLINE 的区别
                   就在这里起步（从卡根本没起来 vs 曾经在线后掉线）。 */
                .state = APP_SCREEN_CARD_STATE_MISSING,
            };
        }
    return true;
}

/** @brief 本期取板级网格参数（见 board.h 的 BOARD_CASC_COLS/ROWS/MASTER_CELL） */
static bool _layout_build(void)
{
    return _layout_build_grid((uint8_t)BOARD_CASC_COLS, (uint8_t)BOARD_CASC_ROWS,
                              s_master_cell);
}

/** @brief 由切分表推出逻辑几何、定位本卡；画布几何与清零交给画布 TU
 *
 *  `_screen_init` 与 host 用例都走这一条 —— 用例换一组几何/网格时不必自己拼
 *  "设几何 + 清画布"那几步，也就不会与生产初始化漂移。
 *  @return false = 本卡地址不在表里，或画布池装不下整屏 */
static bool _apply_layout(void)
{
    s_rows = s_screen_layout.rows;
    s_cols = s_screen_layout.cols;

    s_self_idx = app_screen_self_index();
    if (s_self_idx >= s_screen_layout.count) {
        /* 本卡地址不在切分表里 = 板上的地址与部署对不上。**不静默降级成"单卡占满"**：
           那会让现场以为一切正常，只是别的卡永远不亮（而"别的卡不亮"最容易被当成
           硬件故障去查线）。 */
        printf("[screen] 本卡地址 %u 不在切分表里（表内 %u 张卡），整屏门面停用\n",
               (unsigned)app_screen_self_addr(), (unsigned)s_screen_layout.count);
        return false;
    }

#if BOARD_SCREEN_CANVAS
    /* 画布几何由此固定。内部做：池容校验 → memset → 作废颜色主张；
       **校验失败必须先于 memset 返回**（池子小了先清就是直接写穿）。 */
    if (!app_screen_canvas_attach(s_rows, s_cols)) return false;
#endif
    return true;
}

/* ================================================================
 *  落屏：主卡本地提交与从卡落屏**共用的唯一路径**
 * ================================================================ */

void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color)
{
    dev_display_t *d = s_display_dev;
    if (!d || !bm) return;

    /* 长度必须正好是本屏的位图长度。不符说明这块内容不是给这块屏的
       （换模组、或主从卡几何不一致），拒绝而不是将就 —— 将就的后果是错位画面。 */
    uint16_t need = (uint16_t)(((d->screen_rows + 7U) / 8U) * d->screen_cols);
    if (len != need) {
        printf("[screen] 位图长度 %u != 本屏 %u，已拒绝落屏\n", (unsigned)len, (unsigned)need);
        return;
    }

    /* 清底 + 画内容**当成一帧**：中间不让 _scan_task 跑 prepare（否则屏上闪一帧全黑）。
       见 dev_display_frame_begin 的说明 —— 以前这里是手写 `d->dirty = false`。 */
    dev_display_frame_begin(d);
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, DEV_DISPLAY_COLOR_BLACK);
    dev_display_draw_bitmap(d, 0, 0, d->screen_rows, d->screen_cols, bm, (dev_display_color_t)color);
    dev_display_frame_end(d);
}

/* ================================================================
 *  亮度
 * ================================================================ */

static volatile bool    s_bright_pending_flag;
static volatile uint8_t s_bright_level;

void app_screen_set_brightness(uint8_t level)
{
    if (level > 7) level = 7;
    if (s_display_dev) dev_display_set_brightness(s_display_dev, level);

    s_bright_level   = level;
    s_bright_pending_flag = true; /* 由级联协议取走并广播给从卡 */
}

uint8_t app_screen_get_brightness(void)
{
    return s_display_dev ? s_display_dev->light_level : 0;
}

bool app_screen_brightness_take_pending(uint8_t *level)
{
    if (!s_bright_pending_flag) return false;
    s_bright_pending_flag = false;
    if (level) *level = s_bright_level;
    return true;
}

/* ---- 本卡身份（总线地址）----
 *
 * `board.h` 的 `BOARD_CASC_ADDR` 从"身份"**降级为出厂默认**：运行期可以由
 * 拨码（3833024）或识别帧（按键认领）改写，改完写 flash 记录、掉电不忘 ——
 * 这样两块板烧同一份固件，谁被按谁是主卡。
 *
 * **状态在这里、策略不在这里**："该取哪个值、记在哪"是部署/协议侧的事
 * （见 app_casc.c 的身份解析与 `SET_ADDR`），本模块只存值并按新值重装门面。 */
static uint8_t s_addr = (uint8_t)BOARD_CASC_ADDR;

uint8_t app_screen_self_addr(void)
{
    return s_addr;
}

/** @brief 只改值：**不落盘、不重应用**（持久化与重应用是调用方的事） */
void app_screen_set_addr(uint8_t addr)
{
    s_addr = addr;
}

bool app_screen_is_master(void)
{
    return app_screen_self_addr() == 0U; /* 地址 0 = 主卡 */
}

/** @brief 按**当前** `app_screen_self_addr()` 装好整屏门面 —— 上电与运行期换身份共用
 *
 *  把原来 `_screen_init` 里"与身份有关的那半段"整段搬到这里：重算几何、定位本卡、
 *  **清画布**、按主/从注册或撤销渲染目标与持久化钩子、清待落屏标志。
 *  换身份的路径（按键认领 / 收到识别帧）只需改 `s_addr` 再调本函数 ——
 *  两条路走同一段代码，就不会有"上电装对了、运行期漏装一样"的漂移。
 *
 *  @return false = 门面不可用（显示未就绪 / 地址不在切分表里 / 画布池装不下） */
static bool _apply_identity(void)
{
    if (!_layout_build()) {
        printf("[screen] 切分表合成失败，整屏门面停用\n");
        return false;
    }
    /* 逻辑几何 = **整屏**（多卡时比本卡那块屏大）—— 主卡画的就是这个。
       `_apply_layout` 同时会定位本卡、并在尺寸合格后**清画布**。 */
    if (!_apply_layout()) return false;

    /* board.h 的 BOARD_CASC_BAND_MAX 只是编译期上限，这里按**运行期**几何核一次 */
    const uint16_t band = app_screen_card_bm_len(s_self_idx);
    if (band > BOARD_CASC_BAND_MAX) {
        printf("[screen] 本卡矩形位图 %u > BOARD_CASC_BAND_MAX %u，整屏门面停用\n",
               (unsigned)band, (unsigned)BOARD_CASC_BAND_MAX);
        return false;
    }

#if BOARD_SCREEN_CANVAS
    /* 只有主卡有"整屏"这个概念。**从卡不注册画布**：它的屏是整屏的一块窗口，
       内容由主卡下发；本地渲染没有意义（下一轮就被覆盖），回落到直写实屏才不会
       与下发的内容打架。
       **两个方向都要做**：从主变从要撤、从从变主要装 —— 这是本函数存在的理由。 */
    if (app_screen_is_master()) {
        app_screen_canvas_enable(s_rows, s_cols);
    } else {
        app_screen_canvas_disable();
    }
#endif

    const app_screen_card_t *c = app_screen_card(s_self_idx);
#if BOARD_SCREEN_CANVAS
    /* 画布字节数在这里按几何现算（s_bm_len 是画布 TU 的内部量，不跨 TU 取） */
    const uint16_t canvas_bytes = (uint16_t)(((s_rows + 7U) / 8U) * s_cols);
    printf("[screen] 整屏画布 %ux%u（%u 字节，1bpp）共 %u 卡；本卡 #%u addr=%u "
           "矩形 %ux%u@(%u,%u) 颜色 %u%s\n",
           (unsigned)s_rows, (unsigned)s_cols, (unsigned)canvas_bytes, (unsigned)s_screen_layout.count,
           (unsigned)s_self_idx, (unsigned)app_screen_self_addr(), (unsigned)c->w, (unsigned)c->h,
           (unsigned)c->x, (unsigned)c->y, (unsigned)c->color,
           app_screen_is_master()
               ? (s_screen_layout.count > 1 ? "（主卡，落屏由级联轮次统一做）" : "（单卡）")
               : "（从卡，内容由主卡下发）");
#else
    printf("[screen] 整屏门面未启用（BOARD_SCREEN_CANVAS=0），渲染直写实屏；"
           "本卡 addr=%u 共 %u 卡\n",
           (unsigned)app_screen_self_addr(), (unsigned)s_screen_layout.count);
    (void)c;
#endif
    return true;
}

void app_screen_apply_identity(uint8_t addr, uint8_t master_cell)
{
    /* **一个都没变就别重装**：重装会按新身份重建表、清画布与"画布被写过"的闩 ——
       上电那一次与"同址确认"都是这种情况，白清一次内容。 */
    if (addr == s_addr && master_cell == s_master_cell) return;

    s_addr        = addr;
    s_master_cell = master_cell;
    app_screen_reinit_identity();
}

void app_screen_reinit_identity(void)
{
    if (!s_display_dev) return;

    if (_apply_identity()) return;

    /* 本卡地址不在切分表里 → **停用门面**（不是静默降级成"单卡占满"）：
       主卡身份残留会让这张卡继续往画布上画、而屏上什么都没有。 */
#if BOARD_SCREEN_CANVAS
    app_screen_canvas_disable();
#else
    app_render_set_target(nullptr);
    app_render_set_persist_hook(nullptr);
#endif
}

/* ================================================================
 *  静默期自动提交的消费者：单卡
 *
 *  落屏的消费者**只能有一个**：
 *   · 单卡       → 本模块自己的静默期任务（就是加级联之前的行为）
 *   · 多卡主卡   → 级联的"轮"。它还要把同一份内容分发给从卡，必须由它统一决定
 *                 何时落屏 —— 两个消费者并存的话主卡屏与从卡屏会差一轮
 *   · 从卡       → 两个都不是，它的内容来自总线（app_screen_commit_bitmap）
 *
 *  本任务只在**画布打开**时存在（没有画布就没有"待落屏"，渲染是直写实屏的）。
 *  任务体只用画布 TU 的公开 API（take_pending_settled + commit_self），
 *  周期宏与任务体一起留在这里（画布 TU 不再需要它）。 */
#if BOARD_SCREEN_CANVAS
#define SCREEN_POLL_MS (10U) /**< 检查静默期的周期 */

static void _screen_task(void *arg)
{
    (void)arg;
    for (;;) {
        osDelay(SCREEN_POLL_MS);
        if (app_screen_take_pending_settled()) (void)app_screen_commit_self();
    }
}
#endif /* BOARD_SCREEN_CANVAS */

/* ================================================================
 *  初始化
 * ================================================================ */

static void _screen_init(void)
{
    dev_display_t *d = dev_display_get();
    if (!d) {
        printf("[screen] 显示未就绪，整屏门面停用\n");
        return;
    }
    s_display_dev = d;

    if (!_apply_identity()) {
        s_display_dev = nullptr;
        return;
    }

#if BOARD_SCREEN_CANVAS
    if (s_screen_layout.count <= 1) {
        const osThreadAttr_t attr = {
            .name       = "screen",
            .stack_size = 256 * 4,
            .priority   = osPriorityNormal,
        };
        pl_task_new(_screen_task, nullptr, &attr);
    }
#endif
}
sw_dev_initcall(_screen_init);
