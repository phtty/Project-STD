/**
 * @file    test_casc_round.c
 * @brief   级联图传 · **从卡侧**：收一条 IMAGE → 校验 → 落屏 → 回 ACK/NACK
 *
 * **为什么需要这个测试**：从卡这一侧错了，现场看到的是"某块屏上是别的内容"或
 * "内容不更新"，而两块屏分别在两台设备上，没有任何一处日志会指出是哪一步错的。
 * 三条**静默错**都不会报错、不会超时，只是画面不对：
 *
 *   1. **帧长与 bmp_len 自相矛盾**——两端固件不同版本时最常见（旧版发的是分片帧）。
 *      不先按长度拦住，就会按新布局的偏移去读位图，读到的是帧外的东西。
 *   2. **矩形不符却照落**——两块板烧了不同的切分表时，同一地址对应的格子相反，
 *      结果两块屏内容**悄悄互换而所有检查通过**。所以矩形要逐字段核对，
 *      不只是看"装不装得下"。
 *   3. **回带的 seq 不是主卡发来的那个**——主卡的等待循环按 (seq, src) 匹配，
 *      从卡若回自己的计数器，表现是"每张卡都超时"，而两侧日志都看不出问题。
 *
 * 用例走的是**真探针 + 真分派表**（`_casc_probe_frame` → `s_casc_cmd_table[]`），
 * 只有总线与 app_screen 是替身；帧由用例独立构造，不复用被测的 `_build`。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BOARD_CASC_ENABLED 1 /* 本套件测级联，强制编进来（必须早于会拉进 board.h 的头）*/

/* 身份/记录/按键的桩要用到这些类型（本套件不测它们的行为）*/
#include "app_cfg_sched.h"
#include "dev_key.h"
#include "app_casc.h"
#include "app_screen.h"
#include "dev_display.h"
#include "ring_buffer.h"

/* ---- 替身：总线与整屏门面 ---- */

static uint8_t  s_tx_frame[CASC_FRAME_MAX];
static uint16_t s_tx_len;
static int      s_tx_count;

/* ---- 发生顺序：'A' = 回了 ACK，'S' = 落盘 ----
 *
 * "先 ACK 再写 flash"是**必须**的（写一条记录几十~几百 ms，而主卡等 ACK 上限 200ms）。
 * 只看"落盘调了几次"抓不住顺序错误，所以这里按事件顺序记下来。 */
static char s_order[8];
static int  s_order_n;

static void order_push(char c)
{
    if (s_order_n < (int)sizeof(s_order)) s_order[s_order_n++] = c;
}

int32_t app_ccb_send(app_ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    if (l <= sizeof(s_tx_frame)) {
        memcpy(s_tx_frame, d, l);
        s_tx_len = l;
    }
    if (l >= CASC_OVERHEAD && CASC_TYPE_OF(d[2]) == APP_CASC_TYPE_ACK) order_push('A');
    s_tx_count++;
    return (int32_t)l;
}
app_ccb_t *app_rs485_ccb(void) { return nullptr; }
void   app_dispatch_bind(app_pcb_t *p, app_ccb_t *c) { (void)p; (void)c; }
void   pl_task_new_stub(void) {}

/* 本卡身份：默认是**从卡**（addr=1）。case_master_ignores 会临时翻过来。 */
static bool s_is_master;
bool        app_screen_is_master(void) { return s_is_master; }
uint8_t     app_screen_self_addr(void) { return 1; }

/* 落屏替身：把内容留下来供断言 —— 从卡的落屏路径就是"校验通过调一次 commit_bitmap"，
   留证比拉进真的 app_screen.c（要带上切分表与画布一整串）更直接。 */
static uint8_t  s_commit_bm[256];
static uint16_t s_commit_len;
static uint8_t  s_commit_color;
static int      s_commit_count;

void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color)
{
    if (len <= sizeof(s_commit_bm)) {
        memcpy(s_commit_bm, bm, len);
        s_commit_len = len;
    }
    s_commit_color = color;
    s_commit_count++;
}

static uint8_t s_bright_last = 0xFF;
static int     s_bright_calls;
void           app_screen_set_brightness(uint8_t l)
{
    s_bright_last = l;
    s_bright_calls++;
}
uint8_t app_screen_get_brightness(void) { return s_bright_last; }
bool    app_screen_brightness_take_pending(uint8_t *l)
{
    (void)l;
    return false;
}
uint8_t app_screen_index_of_addr(uint8_t a) { (void)a; return 0xFF; }
app_screen_card_state_t app_screen_card_state(uint8_t i) { (void)i; return APP_SCREEN_CARD_STATE_ONLINE; }
void app_screen_card_set_state(uint8_t i, app_screen_card_state_t st) { (void)i; (void)st; }
void app_screen_note_round(uint16_t seq) { (void)seq; }
void app_screen_note_retrans(void) {}

/* ---- 身份/记录/按键：本套件不测认领与落盘，桩成"不动作" ---- */
void    app_screen_set_addr(uint8_t a) { (void)a; }
void    app_screen_reinit_identity(void) {}
bool    app_screen_canvas_touched(void) { return false; }
uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *d) { (void)d; return 0xFF; }
dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *p, uint16_t c, uint16_t *l)
{
    (void)id; (void)p; (void)c; (void)l;
    return DEV_CFG_RECORD_STATE_EMPTY; /* "没有记录" → 身份回落到板级默认 */
}
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *p, uint16_t n)
{
    (void)id; (void)p; (void)n;
    return 0;
}
dev_key_t *dev_key_get(dev_key_id_t id) { (void)id; return nullptr; } /* 两侧都没有拨码 */
const app_screen_layout_t *app_screen_layout(void) { return nullptr; }
/* 从卡落盘与开轮时 peek 的持久化请求位：本套件桩成"从不请求持久化" */
static int s_save_calls;
void       app_render_save(void)
{
    s_save_calls++;
    order_push('S');
}
bool app_render_peek_persist_req(void) { return false; }
bool app_render_busy(void) { return false; }                  /* 本套件不在渲染途中 */
uint8_t app_screen_output_color(uint8_t c) { return c; }      /* 无颜色覆盖 */
/* 身份的应用与格↔地址规则：本套件不测它们（由主卡套件覆盖），桩成恒等 */
void    app_screen_apply_identity(uint8_t a, uint8_t mc) { (void)a; (void)mc; }
uint8_t app_screen_master_cell(void) { return 0; }
uint8_t app_screen_addr_of_cell(uint8_t cell, uint8_t mc) { return (uint8_t)(cell == mc ? 0 : cell); }
uint8_t app_screen_cell_of_addr(uint8_t addr, uint8_t mc) { (void)mc; return addr; }



/* 本卡实屏：几何必须与 IMAGE 里的矩形一致，否则从卡回 NACK —— 这正是被测行为之一 */
#define DEV_W (48U)
#define DEV_H (16U)
static dev_display_t s_dev;
static uint8_t       s_pixel_map[DEV_W * DEV_H];

dev_display_t *dev_display_get(void) { return &s_dev; }

/* 本卡切分表：一张卡，就是本卡自己。用例可以改它来构造"切分表不符"。
   `app_screen_self_index()` 返回 0xFF 表示"本卡地址不在切分表里"（配置错）。 */
static app_screen_card_t s_self_card = {.addr  = 1,
                                    .color = DEV_DISPLAY_COLOR_GREEN,
                                    .x     = 0,
                                    .y     = 0,
                                    .w     = DEV_W,
                                    .h     = DEV_H,
                                    .state = APP_SCREEN_CARD_STATE_ONLINE};
static uint8_t       s_self_idx  = 0;

const app_screen_card_t *app_screen_card(uint8_t idx) { return (idx == 0) ? &s_self_card : nullptr; }
uint8_t              app_screen_self_index(void) { return s_self_idx; }

uint16_t app_screen_card_bm_len(uint8_t idx)
{
    const app_screen_card_t *c = app_screen_card(idx);
    return c ? (uint16_t)(((c->w + 7U) / 8U) * c->h) : 0;
}

/* ---- 被测：生产源码本体（探针与分派表都是 static） ---- */
#include "../Application/Src/CASC/app_casc.c"

/* ================================================================
 *  夹具
 * ================================================================ */

RB_DEFINE(s_rb, 4096);

static uint8_t s_msg_buf[sizeof(app_dispatch_msg_t) + FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));
static app_dispatch_msg_t *s_msg = (app_dispatch_msg_t *)s_msg_buf;

#define BMP_LEN (96U) /* ceil(48/8) × 16 */
#define SEQ     (7U)

static uint8_t s_bmp[BMP_LEN];

/** 位图图案：每字节都不同，错位 / 少一截 / 旧内容残留都会露出来 */
static void bmp_pattern(void)
{
    for (uint16_t i = 0; i < BMP_LEN; i++) s_bmp[i] = (uint8_t)(i * 7U + 3U);
}

static void fixture_reset(void)
{
    s_rb.read_index  = 0;
    s_rb.write_index = 0;
    s_casc_pcb.rb    = &s_rb;

    s_tx_len   = 0;
    s_tx_count = 0;
    s_order_n  = 0;

    s_save_calls = 0;

    s_commit_count = 0;
    s_commit_len   = 0;
    s_commit_color = 0;

    s_bright_calls = 0;
    s_is_master    = false;

    s_dev.screen_rows = DEV_W;
    s_dev.screen_cols = DEV_H;
    s_dev.pixel_map   = s_pixel_map;
    memset(s_pixel_map, 0, sizeof(s_pixel_map));

    s_self_idx  = 0;
    s_self_card = (app_screen_card_t){.addr  = 1,
                                  .color = DEV_DISPLAY_COLOR_GREEN,
                                  .x     = 0,
                                  .y     = 0,
                                  .w     = DEV_W,
                                  .h     = DEV_H,
                                  .state = APP_SCREEN_CARD_STATE_ONLINE};

    bmp_pattern();
}

/** @brief 把一帧喂进**真探针**，READY 后交给**真分派表** —— 与框架走同一条路径
 *
 *  探针窥视的目标与入参缓冲是同一块内存（生产里也是这么做的，见 app_dispatch.c
 *  的说明），所以 READY 之后帧内容已经在 `s_msg->data` 里了。 */
static bool feed(const uint8_t *f, uint16_t len)
{
    rb_write(&s_rb, f, len, nullptr);

    uint32_t        tl  = 0;
    uint8_t         aux = 0;
    app_pcb_probe_state_t st =
        _casc_probe_frame(&s_casc_pcb, nullptr, nullptr, s_msg->data, FRAME_DATA_MAX_LEN, &tl, &aux);

    if (st != APP_PCB_PROBE_STATE_READY) {
        rb_skip(&s_rb, 1, nullptr); /* 伪帧：与框架一样跳 1 字节重试 */
        return false;
    }
    rb_skip(&s_rb, tl, nullptr);

    s_msg->data_len = (uint16_t)tl;
    s_msg->aux      = aux;
    s_msg->ccb      = nullptr;

    if (aux <= CASC_TYPE_MASK && s_casc_cmd_table[aux]) s_casc_cmd_table[aux](s_msg);
    return true;
}

/* ---- 帧构造：**独立**实现，不复用被测的 _build ---- */

/** @brief 按协议规则组一帧；载荷由调用方给（`payload` 与输出缓冲**必须分开**，
 *         因为本函数会先 memset 输出缓冲） */
static uint16_t build(uint8_t *out, uint8_t type, uint16_t seq, const void *payload, uint16_t plen)
{
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(out, 0, len);
    out[0] = CASC_SOF0;
    out[1] = CASC_SOF1;
    out[2] = (uint8_t)((CASC_PROTO_VER << 6) | type);
    out[3] = 1;                /* dst = 本卡（addr 1） */
    out[4] = CASC_ADDR_MASTER; /* src = 主卡 */
    _casc_put_u16(out + 5, seq);
    out[7] = 0; /* 保留字段 */
    out[8] = 0;
    _casc_put_u16(out + 9, len);
    if (plen) memcpy(out + 11, payload, plen);
    _casc_put_u32(out + len - 4U, pl_crc32_calc(pl_crc_get_handle(), out + 2, len - 6U));
    return len;
}

/** @brief 组一条 IMAGE 的载荷（12 字节头 + 位图），返回载荷长度
 *
 *  各项都可以给成"不对的值"，用例据此构造各种拒绝路径。 */
static uint16_t image_payload(uint8_t *out, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint16_t bmp_len, uint8_t bright, uint8_t color, const uint8_t *bmp,
                              uint8_t persist)
{
    app_casc_image_t *p = (app_casc_image_t *)out;
    memset(p, 0, sizeof(*p));

    _casc_put_u16(p->x, x);
    _casc_put_u16(p->y, y);
    _casc_put_u16(p->w, w);
    _casc_put_u16(p->h, h);
    _casc_put_u16(p->bmp_len, bmp_len);
    p->bright  = bright;
    p->color   = color;
    p->persist = persist;

    if (bmp) memcpy(p->bitmap, bmp, BMP_LEN);
    return (uint16_t)(sizeof(app_casc_image_t) + bmp_len);
}

/** @brief 组一条 IMAGE 并喂进去（带 persist 的那一版）
 *
 *  @param len_delta 把"整帧实际带的载荷"加减几个字节，用来构造"帧长与 bmp_len
 *                   自相矛盾"的帧（跨版本固件的第一道防线就是拦它）
 *  @param persist   主卡带下来的"这次内容要长期保留" */
static void feed_image_p(uint16_t seq, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t bmp_len, uint8_t bright, uint8_t color, const uint8_t *bmp,
                         int len_delta, uint8_t persist)
{
    static uint8_t pl[CASC_FRAME_MAX]; /* 载荷暂存：与整帧缓冲分开，见 build() 的说明 */
    static uint8_t fr[CASC_FRAME_MAX];

    const uint16_t plen = image_payload(pl, x, y, w, h, bmp_len, bright, color, bmp, persist);
    const uint16_t n    = build(fr, APP_CASC_TYPE_IMAGE, seq, pl, (uint16_t)(plen + len_delta));
    feed(fr, n);
}

static void feed_image(uint16_t seq, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t bmp_len,
                       uint8_t bright, uint8_t color, const uint8_t *bmp, int len_delta)
{
    feed_image_p(seq, x, y, w, h, bmp_len, bright, color, bmp, len_delta, 0);
}

/** @brief 发一条正常的 IMAGE（本夹具的几何、位图、颜色） */
static void send_image(uint16_t seq, uint8_t bright)
{
    feed_image(seq, 0, 0, DEV_W, DEV_H, BMP_LEN, bright, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0);
}

/* ---- 收到的应答 ---- */

static bool last_is(uint8_t type)
{
    return s_tx_len >= CASC_OVERHEAD && CASC_TYPE_OF(s_tx_frame[2]) == type;
}
/** @brief 最近一条应答回带的 seq（从帧头读，不是载荷） */
static uint16_t last_seq(void) { return _casc_get_u16(s_tx_frame + 5); }
/** @brief 最近一条 NACK 的 err；帧头 11 字节之后就是它 */
static uint8_t last_nack_err(void) { return s_tx_frame[11]; }

/* ---- 断言 ---- */
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

/* ================================================================ */

/** 一条 IMAGE 就是完整一轮：收下即落屏，内容逐字节相等 */
static void case_image_ok(void)
{
    TEST_BEGIN("一条 IMAGE：落屏一次，内容与颜色逐字节相等，回无载荷 ACK");

    fixture_reset();
    send_image(SEQ, 4);

    CHECK_MSG(s_commit_count == 1, "一条 IMAGE 应恰好落屏一次，得到 %d 次", s_commit_count);
    CHECK_MSG(s_commit_len == BMP_LEN, "落屏长度应为 %u，得到 %u", (unsigned)BMP_LEN,
              (unsigned)s_commit_len);
    CHECK_MSG(memcmp(s_commit_bm, s_bmp, BMP_LEN) == 0, "落屏内容与发出去的不一致");
    CHECK_MSG(s_commit_color == DEV_DISPLAY_COLOR_GREEN, "落屏颜色应取 IMAGE 里的本卡颜色，得到 %u",
              (unsigned)s_commit_color);

    CHECK_MSG(last_is(APP_CASC_TYPE_ACK), "应回一帧 ACK");
    CHECK_MSG(s_tx_len == CASC_OVERHEAD, "ACK 应无载荷（整帧 %u 字节），得到 %u",
              (unsigned)CASC_OVERHEAD, (unsigned)s_tx_len);
}

/** 幂等：主卡没收到 ACK 会重发同一条帧，重落同一份内容无副作用 */
static void case_image_idempotent(void)
{
    TEST_BEGIN("同一条 IMAGE 重发（ACK 丢了）幂等");

    fixture_reset();
    send_image(SEQ, 4);
    send_image(SEQ, 4);

    CHECK_MSG(s_commit_count == 2, "两次应各落屏一次（内容相同，无副作用），得到 %d 次",
              s_commit_count);
    CHECK_MSG(memcmp(s_commit_bm, s_bmp, BMP_LEN) == 0, "第二次落屏的内容仍应与发出去的一致");
    CHECK_MSG(last_is(APP_CASC_TYPE_ACK), "第二次仍应回 ACK");
}

/** ACK 必须回带**主卡发来的那个 seq** —— 回自己的计数器会让主卡永远匹配不上 */
static void case_ack_echoes_seq(void)
{
    TEST_BEGIN("ACK 回带 IMAGE 的 seq（主卡按 (seq, src) 匹配）");

    fixture_reset();
    send_image(0x1234, 4);

    CHECK_MSG(last_is(APP_CASC_TYPE_ACK), "应回 ACK");
    CHECK_MSG(last_seq() == 0x1234, "ACK 应回带 0x1234，得到 0x%04X", (unsigned)last_seq());
}

/** 帧长与 bmp_len 自相矛盾 —— 两端固件不同版本时最常见，必须回 NACK 而不是照读 */
static void case_len_mismatch_nack(void)
{
    TEST_BEGIN("帧长与 bmp_len 不符 → NACK(LEN)，不落屏");

    fixture_reset();
    feed_image(SEQ, 0, 0, DEV_W, DEV_H, BMP_LEN, 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, -1);

    CHECK_MSG(last_is(APP_CASC_TYPE_NACK), "帧长不符应回 NACK（跨版本固件的第一道防线）");
    CHECK_MSG(last_nack_err() == APP_CASC_NACK_LEN, "NACK 原因应为 LEN，得到 %u",
              (unsigned)last_nack_err());
    CHECK_MSG(s_commit_count == 0, "帧长不符时绝不能落屏");
}

/** 矩形不符 → 明确回 NACK，而不是将就出一幅错位画面（两块屏互换就是这种） */
static void case_geometry_nack(void)
{
    TEST_BEGIN("矩形与本卡切分表不符 → NACK(GEOM)，不落屏");

    /* 尺寸不符 */
    fixture_reset();
    feed_image(SEQ, 0, 0, (uint16_t)(DEV_W + 1U), DEV_H, BMP_LEN, 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0);
    CHECK_MSG(last_is(APP_CASC_TYPE_NACK) && last_nack_err() == APP_CASC_NACK_GEOM,
              "尺寸不符应回 NACK(GEOM)，得到 err=%u", (unsigned)last_nack_err());
    CHECK_MSG(s_commit_count == 0, "几何不符时绝不能落屏");

    /* 尺寸对但**位置**不对：切分表不一致的典型形态（格子一样大、谁占哪格相反） */
    fixture_reset();
    feed_image(SEQ, 0, (uint16_t)(DEV_H + 1U), DEV_W, DEV_H, BMP_LEN, 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0);
    CHECK_MSG(last_is(APP_CASC_TYPE_NACK) && last_nack_err() == APP_CASC_NACK_GEOM,
              "y 偏移不符也应回 NACK(GEOM) —— 这才是「两块屏内容互换」的那条防线");
    CHECK_MSG(s_commit_count == 0, "位置不符时绝不能落屏");

    /* 本卡地址不在切分表里（配置错） */
    fixture_reset();
    s_self_idx = 0xFF;
    send_image(SEQ, 4);
    CHECK_MSG(last_is(APP_CASC_TYPE_NACK) && last_nack_err() == APP_CASC_NACK_GEOM,
              "本卡地址不在切分表里应回 NACK(GEOM)");
    CHECK_MSG(s_commit_count == 0, "配置错时绝不能落屏");
}

/** bmp_len 与本卡矩形算出来的长度不符 → NACK（否则会按错的长度读载荷） */
static void case_bmp_len_nack(void)
{
    TEST_BEGIN("bmp_len 与本卡矩形不符 → NACK(GEOM)");

    fixture_reset();
    /* 声明 BMP_LEN+8，就真带 BMP_LEN+8 字节 —— 这样整帧长度是**自洽**的，
       只有"与本卡矩形算出来的长度不符"这一条能拦住它 */
    feed_image(SEQ, 0, 0, DEV_W, DEV_H, (uint16_t)(BMP_LEN + 8U), 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0);

    CHECK_MSG(last_is(APP_CASC_TYPE_NACK) && last_nack_err() == APP_CASC_NACK_GEOM,
              "bmp_len 不符应回 NACK(GEOM)，得到 err=%u", (unsigned)last_nack_err());
    CHECK_MSG(s_commit_count == 0, "bmp_len 不符时绝不能落屏");
}

/** 亮度每轮重新断言 —— "从卡永久停在旧亮度" 的唯一防线 */
static void case_bright_asserted(void)
{
    TEST_BEGIN("每轮 IMAGE 都重新断言亮度");

    fixture_reset();
    send_image(SEQ, 5);
    CHECK_MSG(s_bright_calls == 1 && s_bright_last == 5, "IMAGE 应把亮度断言到 5，得到 %u",
              (unsigned)s_bright_last);

    /* 下一轮改亮度，必须跟着变（SET_BRIGHT 广播丢了就靠这里自愈） */
    fixture_reset();
    send_image(SEQ, 2);
    CHECK_MSG(s_bright_calls == 1 && s_bright_last == 2, "下一轮应重新断言成 2，得到 %u",
              (unsigned)s_bright_last);

    /* 亮度断言在**参数校验之后**：几何不符时不改屏上任何东西 */
    fixture_reset();
    feed_image(SEQ, 0, 0, (uint16_t)(DEV_W + 1U), DEV_H, BMP_LEN, 6, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0);
    CHECK_MSG(s_bright_calls == 0, "被 NACK 的 IMAGE 不该改亮度");
}

/** 主卡收到发给从卡的帧：什么都不做（半双工回声 / 地址配重时会遇到） */
static void case_master_ignores(void)
{
    TEST_BEGIN("本卡是主卡时，IMAGE 一律不应答、不落屏");

    fixture_reset();
    s_is_master = true;
    send_image(SEQ, 4);

    CHECK_MSG(s_tx_count == 0, "主卡不该应答发给从卡的帧，却发了 %d 帧", s_tx_count);
    CHECK_MSG(s_commit_count == 0, "主卡不该按从卡路径落屏");
}

/** @brief IMAGE 带 persist → 从卡**先回 ACK、再落盘**，且只落一次
 *
 *  **顺序是这条用例的全部**：写一条记录是整扇区读-改-写 + 擦除，几十~几百 ms，
 *  而主卡等 ACK 的上限是 `CASC_ACK_TIMEOUT_MS`(200ms)。先写就成了"每轮都超时、
 *  每轮都整帧重发"——现场表现为"级联特别慢 + 总线一直忙"，而两块屏的内容其实是对的，
 *  所以只看画面完全看不出来。
 *
 *  **反向验证**：把 `_cmd_image` 末尾那句 `if (p->persist) app_render_save()` 挪到
 *  回 ACK **之前**，本用例立刻红（"ACK 必须在落盘之前"）。
 *
 *  内容是否与上次一致、要不要真擦写由 dev_cfg_record 那一层去重，不属于本套件。 */
static void case_persist_slave_acks_before_saving(void)
{
    TEST_BEGIN("IMAGE 带 persist → 先 ACK、后落盘（一次）");

    fixture_reset();
    feed_image_p(SEQ, 0, 0, DEV_W, DEV_H, BMP_LEN, 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0, /*persist=*/1);

    CHECK_MSG(s_commit_count == 1, "带 persist 的一轮照样要落屏，得到 %d 次", s_commit_count);
    CHECK_MSG(last_is(APP_CASC_TYPE_ACK), "带 persist 的一轮必须回 ACK");
    CHECK_MSG(s_save_calls == 1, "persist=1 应从卡落盘一次，得到 %d 次", s_save_calls);
    CHECK_MSG(s_order_n == 2 && s_order[0] == 'A' && s_order[1] == 'S',
              "ACK 必须在落盘之前（否则主卡每轮都超时重发）；实测顺序 %c%c",
              s_order_n > 0 ? s_order[0] : '-', s_order_n > 1 ? s_order[1] : '-');

    /* 不带 persist：一次都不许写（flash 每扇区约 10 万次擦写，内容却可能几秒一变） */
    fixture_reset();
    send_image(SEQ, 4);
    CHECK_MSG(last_is(APP_CASC_TYPE_ACK), "不带 persist 的一轮同样要回 ACK");
    CHECK_MSG(s_save_calls == 0, "persist=0 时不该落盘，得到 %d 次", s_save_calls);

    /* 被拒绝的一轮更不该落盘（它连屏都没落） */
    fixture_reset();
    feed_image_p(SEQ, 0, 0, (uint16_t)(DEV_W + 1U), DEV_H, BMP_LEN, 4, DEV_DISPLAY_COLOR_GREEN, s_bmp, 0, 1);
    CHECK_MSG(s_save_calls == 0, "被 NACK 的一轮不该落盘，得到 %d 次", s_save_calls);
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m级联图传 · 从卡侧（本卡 %ux%u）\033[0m\n", (unsigned)DEV_W, (unsigned)DEV_H);

    case_image_ok();
    case_image_idempotent();
    case_ack_echoes_seq();
    case_len_mismatch_nack();
    case_geometry_nack();
    case_bmp_len_nack();
    case_bright_asserted();
    case_master_ignores();
    case_persist_slave_acks_before_saving();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
