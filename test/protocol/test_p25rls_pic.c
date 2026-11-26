/**
 * @file    test_p25rls_pic.c
 * @brief   P25RLS 图片槽：3 个双平面位图槽、配置记录加载/落盘、上电重放、三色绘制
 *
 * 命令层那半（`SV` 怎么解析帧、图片编号怎么分派）在 `test_p25rls.c` 里；本套件管的是
 * 它桩掉的那层：`app_p25rls_pic.c` 的槽镜像、记录加载、三遍绘制与上电重放。
 *
 * **为什么需要这个测试**：
 *   1. `SV` 的位图是**红/绿两个平面**，落屏要按"红独有 / 绿独有 / 两平面都有"拆成
 *      三遍画（渲染引擎一次只能画一种颜色）。掩码算错就是**颜色错**——现场"发了黄图
 *      显示成红图"这类投诉，抓包只能看到字节是对的；
 *   2. 槽布局是**持久化契约**（1 字节有效位 + 1 字节最后显示槽 + 3 × 576B 位图），
 *      改布局不升版本号，旧记录会被按新布局解出乱码位图 —— 上电就是花屏；
 *   3. 加载回调要处理**空 / 损坏 / 长度不符**三种记录，任何一种漏判都会把 Flash 里的
 *      垃圾当图显示；
 *   4. 上电重放只能在 `last_shown` 指向"写过且合法"的槽时发生，否则会把黑屏覆盖到
 *      渲染层刚恢复出来的内容上。
 *
 * 手法：TU-include 生产 `app_p25rls_pic.c`（`s_rec` / 加载回调都是 static），只把外部
 * 依赖换成记录桩：配置调度器、渲染、屏几何、显示门面。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_cfg_sched.h"
#include "app_p25rls_pic.h"
#include "app_render.h"
#include "app_screen.h"
#include "dev_cfg_record.h"
#include "dev_display.h"

/* ================================================================
 *  断言（附录 A 的 A 型）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  生产模块本体（TU-include：槽镜像与加载回调都是 static）
 * ================================================================ */

#include "../../Application/Src/P25RLS/app_p25rls_pic.c"

/* ================================================================
 *  记录桩
 * ================================================================ */

/* ---- 配置调度器：注册 / 加载 / 保存 ---- */
static const app_cfg_sched_desc_t *s_reg_desc;
static uint8_t                     s_reg_ret = 2;

static int                    s_load_cnt;
static dev_cfg_record_state_t s_load_state;
static uint16_t               s_load_len;
static uint8_t                s_load_payload[2600];

static int      s_save_cnt;
static uint8_t  s_save_id;
static uint16_t s_save_len;
static uint8_t  s_save_payload[2600];
static int32_t  s_save_rc;

uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *desc)
{
    s_reg_desc = desc;
    return s_reg_ret;
}

dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *payload, uint16_t payload_cap,
                                          uint16_t *payload_len)
{
    (void)id;
    s_load_cnt++;
    if (s_load_state != DEV_CFG_RECORD_STATE_OK) return s_load_state;

    const uint16_t n = (s_load_len < payload_cap) ? s_load_len : payload_cap;
    memcpy(payload, s_load_payload, n);
    *payload_len = s_load_len;
    return DEV_CFG_RECORD_STATE_OK;
}

int32_t app_cfg_sched_save(uint8_t id, const uint8_t *payload, uint16_t payload_len)
{
    s_save_cnt++;
    s_save_id  = id;
    s_save_len = payload_len;
    if (payload_len <= sizeof(s_save_payload)) memcpy(s_save_payload, payload, payload_len);
    return s_save_rc;
}

/* ---- 渲染：只记类型/几何/颜色/位图内容（位图缓冲复用，必须当场拷走）---- */
typedef struct {
    bool     fill;
    bool     bitmap;
    uint16_t w;
    uint16_t h;
    uint8_t  color;
    bool     persist;
    uint8_t  bm[P25RLS_PLANE_BYTES];
} draw_t;

#define DRAW_MAX (8U)
static draw_t s_draws[DRAW_MAX];
static int    s_draw_cnt;

void app_render(const app_render_cfg_t *cfg)
{
    if (s_draw_cnt >= (int)DRAW_MAX) return;

    draw_t *d  = &s_draws[s_draw_cnt++];
    d->fill    = (cfg->type == APP_RENDER_TYPE_FILL);
    d->bitmap  = (cfg->type == APP_RENDER_TYPE_BITMAP);
    d->w       = cfg->w;
    d->h       = cfg->h;
    d->color   = (uint8_t)cfg->color;
    d->persist = cfg->persist;
    if (d->bitmap && (cfg->bitmap != nullptr)) memcpy(d->bm, cfg->bitmap, P25RLS_PLANE_BYTES);
}

/* ---- 屏几何（3833024 口径：128×32）---- */
static uint16_t s_rows = 128;
static uint16_t s_cols = 32;

uint16_t app_screen_rows(void) { return s_rows; }
uint16_t app_screen_cols(void) { return s_cols; }

/* ---- 上电画面接管钩子：记录注册进来的函数（app_boot 用）---- */
static app_render_boot_paint_fn_t s_boot_paint_fn;

void app_render_set_boot_paint(app_render_boot_paint_fn_t fn) { s_boot_paint_fn = fn; }

/* ---- 显示门面：帧窗口计数 + 可切换的"无显示" ---- */
static dev_display_t  s_dsp;
static dev_display_t *s_dsp_ptr = &s_dsp;
static int            s_frame_begin;
static int            s_frame_end;

dev_display_t *dev_display_get(void) { return s_dsp_ptr; }

void dev_display_frame_begin(dev_display_t *dev)
{
    (void)dev; /* 桩只记录，判断留给用例（§4.1） */
    s_frame_begin++;
}

void dev_display_frame_end(dev_display_t *dev)
{
    (void)dev;
    s_frame_end++;
}

/* ================================================================
 *  夹具
 * ================================================================ */

/** @brief 清空所有记录与外部状态（每个用例开头调一次） */
static void fixture_reset(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_cfg_id = 0xFF;

    s_reg_desc      = nullptr;
    s_reg_ret       = 2;
    s_boot_paint_fn = nullptr;

    s_load_cnt   = 0;
    s_load_state = DEV_CFG_RECORD_STATE_OK;
    s_load_len   = (uint16_t)sizeof(s_rec);
    memset(s_load_payload, 0, sizeof(s_load_payload));

    s_save_cnt = 0;
    s_save_id  = 0;
    s_save_len = 0;
    s_save_rc  = 0;
    memset(s_save_payload, 0, sizeof(s_save_payload));

    s_draw_cnt = 0;
    memset(s_draws, 0, sizeof(s_draws));

    s_rows = 128;
    s_cols = 32;

    s_dsp_ptr     = &s_dsp;
    s_frame_begin = 0;
    s_frame_end   = 0;
}

/** @brief 独立的位序 oracle：逐位搬（不复用被测实现） */
static uint8_t reverse_bits(uint8_t b)
{
    uint8_t out = 0;
    for (uint8_t i = 0; i < 8U; i++)
        if (b & (uint8_t)(1U << i)) out |= (uint8_t)(1U << (7U - i));
    return out;
}

/** @brief 造一张双平面位图：红/绿平面各按给定基值铺满
 *  @param red 红平面字节值
 *  @param green 绿平面字节值
 *  @param out 输出 576B */
static void bitmap_make(uint8_t red, uint8_t green, uint8_t *out)
{
    memset(out, red, P25RLS_PLANE_BYTES);
    memset(&out[P25RLS_PLANE_BYTES], green, P25RLS_PLANE_BYTES);
}

/* ================================================================ */

/**
 * @brief 注册：记录名/版本/加载回调是持久化契约；注册失败要降级成"只 RAM 不落盘"
 *
 * 记录按**名字**认领，改名等于换模块（旧记录回落"未写入"）→ 现象是"重启后图全丢"。
 * 注册失败（配置区满/重名）不能停机：本次会话仍要能显示。
 */
static void case_register(void)
{
    TEST_BEGIN("自注册：p25_pic v1 + 加载回调；注册失败退化为只存 RAM");

    fixture_reset();
    _p25rls_pic_register();

    CHECK_MSG(s_reg_desc != nullptr, "注册回调没有下发描述符");
    CHECK_MSG(s_reg_desc != nullptr && strcmp(s_reg_desc->name, "p25_pic") == 0,
              "记录名必须是 p25_pic（改名=换模块），得到 %s",
              s_reg_desc ? s_reg_desc->name : "(null)");
    CHECK_MSG(s_reg_desc != nullptr && s_reg_desc->version == 1,
              "记录版本应为 1（布局变才升），得到 %u",
              s_reg_desc ? (unsigned)s_reg_desc->version : 0);
    CHECK_MSG(s_reg_desc != nullptr && s_reg_desc->load == _p25rls_pic_load,
              "加载回调没有指向 _p25rls_pic_load");
    CHECK_MSG(s_cfg_id == 2, "注册成功应拿到 id=2，得到 %u", (unsigned)s_cfg_id);
    CHECK_MSG(s_rec.last_shown == 0xFF, "刚注册时没有最后显示槽（0xFF），得到 %u",
              (unsigned)s_rec.last_shown);
    /* 上电画面必须由本模块接管：不注册的话 app_boot 会用 render_persist / 兜底文案盖掉 */
    CHECK_MSG(s_boot_paint_fn == p25rls_pic_restore,
              "注册时应把 p25rls_pic_restore 挂成上电画面接管函数");

    /* 注册失败：id 保持 0xFF，save 不落盘但 RAM 更新 */
    fixture_reset();
    s_reg_ret = 0xFF;
    _p25rls_pic_register();

    uint8_t bm[P25RLS_BITMAP_BYTES];
    bitmap_make(0x11, 0x22, bm);
    CHECK_MSG(s_cfg_id == 0xFF, "注册失败应保持 id=0xFF，得到 %u", (unsigned)s_cfg_id);
    CHECK_MSG(!p25rls_pic_save(0, bm), "无记录句柄时 save 必须返回 false（落盘失败）");
    CHECK_MSG(s_save_cnt == 0, "无记录句柄时不得调用调度器 save（调用 %d 次）", s_save_cnt);
    CHECK_MSG(s_rec.valid_mask == 0x01, "无记录句柄时 RAM 仍要更新，valid_mask=%02X",
              (unsigned)s_rec.valid_mask);
    CHECK_MSG(0 == memcmp(s_rec.slot[0].bitmap, bm, P25RLS_BITMAP_BYTES),
              "无记录句柄时位图仍要进 RAM 镜像");
}

/**
 * @brief 槽布局 = 持久化契约：单槽 576B、整条记录 = 2 + 3×576 = 1730B，装得进配置区
 *
 * 这些断言在生产文件里是编译期 `static_assert`；这里再用运行期断言把**数值**钉住，
 * 布局一改（例如每像素 1 字节）这里先红，而不是等到现场上电花屏。
 */
static void case_layout(void)
{
    TEST_BEGIN("槽布局：单槽 576B / 整条记录 1730B / 装得进配置区");

    CHECK_MSG(P25RLS_PLANE_BYTES == 288, "单平面 12×24 = 288B，得到 %u",
              (unsigned)P25RLS_PLANE_BYTES);
    CHECK_MSG(P25RLS_BITMAP_BYTES == 576, "双平面 576B，得到 %u", (unsigned)P25RLS_BITMAP_BYTES);
    CHECK_MSG(sizeof(p25rls_pic_slot_t) == 576, "单槽结构体应恰好 576B，得到 %u",
              (unsigned)sizeof(p25rls_pic_slot_t));
    CHECK_MSG(sizeof(p25rls_pic_record_t) == 2 + 3 * 576, "整条记录应 1730B，得到 %u",
              (unsigned)sizeof(p25rls_pic_record_t));
    CHECK_MSG((uint32_t)sizeof(p25rls_pic_record_t) + DEV_CFG_RECORD_HDR_SIZE <=
                  DEV_CFG_RECORD_MAX_IMAGE,
              "整条记录 + 记录头必须装进 %uB 映像（现在 %uB）", (unsigned)DEV_CFG_RECORD_MAX_IMAGE,
              (unsigned)(sizeof(p25rls_pic_record_t) + DEV_CFG_RECORD_HDR_SIZE));
}

/**
 * @brief 加载：有效记录原样进镜像；空/损坏/长度不符一律回落"三槽未写、不重放"
 *
 * 后三种是真实现场会遇到的（新板空 Flash、写一半掉电、以后改布局），漏判任何一个
 * 都会把 Flash 里的垃圾当图显示。
 */
static void case_load(void)
{
    TEST_BEGIN("加载：有效记录原样；空/损坏/长度不符 → 全部回落未写");

    /* 有效记录 */
    fixture_reset();
    s_cfg_id = 3;

    p25rls_pic_record_t good;
    memset(&good, 0, sizeof(good));
    good.valid_mask = 0x05;
    good.last_shown = 2;
    for (uint8_t i = 0; i < P25RLS_PIC_SLOT_COUNT; i++)
        memset(good.slot[i].bitmap, (uint8_t)(0x40U + i), P25RLS_BITMAP_BYTES);
    s_load_len = (uint16_t)sizeof(good);
    memcpy(s_load_payload, &good, sizeof(good));

    _p25rls_pic_load();
    CHECK_MSG(s_load_cnt == 1, "应查 1 次记录，实际 %d", s_load_cnt);
    CHECK_MSG(s_rec.valid_mask == 0x05, "有效位应原样，实际 %02X", (unsigned)s_rec.valid_mask);
    CHECK_MSG(s_rec.last_shown == 2, "最后显示槽应原样，实际 %u", (unsigned)s_rec.last_shown);
    CHECK_MSG(0 == memcmp(s_rec.slot[1].bitmap, good.slot[1].bitmap, P25RLS_BITMAP_BYTES),
              "槽 1 的位图应原样进镜像");

    /* 空 / 损坏 / 长度不符：镜像必须全零 + last_shown=0xFF */
    struct {
        dev_cfg_record_state_t state;
        uint16_t               len;
        const char            *what;
    } const bad[] = {
        {DEV_CFG_RECORD_STATE_EMPTY, (uint16_t)sizeof(good), "空记录"},
        {DEV_CFG_RECORD_STATE_INVALID, (uint16_t)sizeof(good), "记录无效（CRC/版本不符）"},
        {DEV_CFG_RECORD_STATE_OK, (uint16_t)(sizeof(good) - 1U), "长度不符"},
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fixture_reset();
        s_cfg_id         = 3;
        s_rec.valid_mask = 0xFF; /* 先弄脏，确认加载会清掉 */
        s_load_state     = bad[i].state;
        s_load_len       = bad[i].len;

        _p25rls_pic_load();
        CHECK_MSG(s_rec.valid_mask == 0x00, "%s：有效位应清 0，实际 %02X", bad[i].what,
                  (unsigned)s_rec.valid_mask);
        CHECK_MSG(s_rec.last_shown == 0xFF, "%s：最后显示槽应 0xFF（不重放），实际 %u",
                  bad[i].what, (unsigned)s_rec.last_shown);
    }

    /* 未注册（id=0xFF）：连记录都不查 */
    fixture_reset();
    s_cfg_id = 0xFF;
    _p25rls_pic_load();
    CHECK_MSG(s_load_cnt == 0, "未注册时不该查记录，实际查了 %d 次", s_load_cnt);
}

/**
 * @brief 保存：位图进槽、置有效位、记最后显示槽，并把**整条记录**交给调度器
 *
 * 交给调度器的长度必须是整条记录（不是单槽）：落盘长度短了，重启读回来的就是
 * 半条记录 → 长度不符 → 图全丢（而且现场只看得到"图没保存"）。
 */
static void case_save(void)
{
    TEST_BEGIN("保存：槽位图 + 有效位 + 最后显示槽 + 整条记录落盘");

    fixture_reset();
    s_cfg_id = 5;

    uint8_t bm[P25RLS_BITMAP_BYTES];
    bitmap_make(0xA5, 0x5A, bm);

    CHECK_MSG(p25rls_pic_save(2, bm), "save 应返回 true（落盘成功）");
    CHECK_MSG(s_save_cnt == 1, "应落盘 1 次，实际 %d", s_save_cnt);
    CHECK_MSG(s_save_id == 5, "应写自己的记录 id=5，实际 %u", (unsigned)s_save_id);
    CHECK_MSG(s_save_len == (uint16_t)sizeof(p25rls_pic_record_t),
              "落盘长度应是整条记录 %uB，实际 %uB", (unsigned)sizeof(p25rls_pic_record_t),
              (unsigned)s_save_len);
    CHECK_MSG(0 == memcmp(s_rec.slot[2].bitmap, bm, P25RLS_BITMAP_BYTES), "位图应进槽 2");
    CHECK_MSG(s_rec.valid_mask == 0x04, "槽 2 的有效位应置起，实际 %02X",
              (unsigned)s_rec.valid_mask);
    CHECK_MSG(s_rec.last_shown == 2, "最后显示槽应记成 2，实际 %u", (unsigned)s_rec.last_shown);
    CHECK_MSG(0 == memcmp(s_save_payload, &s_rec, sizeof(s_rec)), "落盘内容应是整条记录");

    /* 落盘失败：RAM 已更新、返回 false */
    fixture_reset();
    s_cfg_id  = 5;
    s_save_rc = -1;
    CHECK_MSG(!p25rls_pic_save(0, bm), "落盘失败应返回 false");
    CHECK_MSG(s_rec.valid_mask == 0x01, "落盘失败时 RAM 仍要更新，valid_mask=%02X",
              (unsigned)s_rec.valid_mask);

    /* 参数非法：越界槽号 / 空指针都不许碰调度器 */
    fixture_reset();
    s_cfg_id = 5;
    CHECK_MSG(!p25rls_pic_save(P25RLS_PIC_SLOT_COUNT, bm), "槽号越界应返回 false");
    CHECK_MSG(!p25rls_pic_save(0, nullptr), "空指针应返回 false");
    CHECK_MSG(s_save_cnt == 0, "参数非法时不得调用调度器 save（调用 %d 次）", s_save_cnt);
    CHECK_MSG(s_rec.valid_mask == 0x00, "参数非法时不得改 RAM 镜像");
}

/**
 * @brief 显示：帧窗口内"清屏 + 三遍绘制"，红/绿/黄三种像素逐位正确
 *
 * 这是本套件最要紧的一条：两个平面合成三种颜色，靠的是三个**互不相交**的掩码。
 * 掩码算错（比如漏掉 `&~other`）会把黄色画成红+绿两遍叠加，看起来也许还行；
 * 但把"两平面都有"错算成任意单色，现场就是**颜色不对**。
 */
static void case_show(void)
{
    TEST_BEGIN("显示：一帧内清屏 + 红/绿/黄三遍，掩码互不相交、逐位正确");

    fixture_reset();
    s_cfg_id = 1;

    uint8_t bm[P25RLS_BITMAP_BYTES];
    memset(bm, 0, sizeof(bm));

    uint8_t *red   = bm;
    uint8_t *green = &bm[P25RLS_PLANE_BYTES];

    /* **槽里存的是线上原样字节：每字节 bit0 = 最左**（实测上位机口径），
       落屏前才翻成 MSB-first。所以这里造"线上"位：bit0 对应每组 8 像素的最左。 */
    red[0]   = 0x01; /* 字节 0：红独有（最左那一列） */
    green[1] = 0x01; /* 字节 1：绿独有（x=8） */
    red[2]   = 0x01; /* 字节 2：两平面都有 → 黄（x=16） */
    green[2] = 0x01;
    /* 字节 3：两平面都没有 → 三遍都不该出现 */
    red[4]   = 0xFF; /* 字节 4：翻转后红 0xFF / 绿 0x0F → 红独有 0xF0、黄 0x0F */
    green[4] = 0xF0;

    CHECK_MSG(p25rls_pic_save(1, bm), "先存进槽 1");
    s_draw_cnt    = 0;
    s_frame_begin = s_frame_end = 0;

    p25rls_pic_show(1);

    CHECK_MSG(s_frame_begin == 1 && s_frame_end == 1, "整段绘制应包成一帧（begin=%d end=%d）",
              s_frame_begin, s_frame_end);
    CHECK_MSG(s_draw_cnt == 4, "应是清屏 + 三遍绘制共 4 笔，实际 %d", s_draw_cnt);
    if (s_draw_cnt != 4) return;

    CHECK_MSG(s_draws[0].fill, "第一笔应是清屏填充");
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏应为黑，实际 %u",
              (unsigned)s_draws[0].color);
    CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "清屏应铺满逻辑屏，实际 %ux%u",
              (unsigned)s_draws[0].w, (unsigned)s_draws[0].h);

    /* 三遍的几何与颜色 */
    const uint8_t want_color[3] = {DEV_DISPLAY_COLOR_RED, DEV_DISPLAY_COLOR_GREEN,
                                   DEV_DISPLAY_COLOR_YELLOW};
    for (int pass = 0; pass < 3; pass++) {
        const draw_t *d = &s_draws[1 + pass];
        CHECK_MSG(d->bitmap, "第 %d 遍应是位图绘制", pass);
        CHECK_MSG(d->color == want_color[pass], "第 %d 遍颜色应为 %u，实际 %u", pass,
                  (unsigned)want_color[pass], (unsigned)d->color);
        CHECK_MSG(d->w == 96 && d->h == 24, "位图应按协议几何 96×24 绘制，实际 %ux%u",
                  (unsigned)d->w, (unsigned)d->h);
        CHECK_MSG(!d->persist, "槽绘制不得再落盘（持久化在 save 里）");
    }

    /* 逐字节核对掩码 */
    const draw_t *red_pass = &s_draws[1];
    const draw_t *grn_pass = &s_draws[2];
    const draw_t *yel_pass = &s_draws[3];

    CHECK_MSG(red_pass->bm[0] == 0x80, "字节 0：线上 bit0（最左）翻成内部 80，实际 %02X",
              (unsigned)red_pass->bm[0]);
    CHECK_MSG(grn_pass->bm[0] == 0x00, "字节 0：绿遍应为 0，实际 %02X", (unsigned)grn_pass->bm[0]);
    CHECK_MSG(grn_pass->bm[1] == 0x80, "字节 1：绿独有应为 80，实际 %02X",
              (unsigned)grn_pass->bm[1]);
    CHECK_MSG(red_pass->bm[2] == 0x00, "字节 2：红遍应剔除同时为绿的位，实际 %02X",
              (unsigned)red_pass->bm[2]);
    CHECK_MSG(yel_pass->bm[2] == 0x80, "字节 2：两平面都有 → 黄（x=16 → 内部 80），实际 %02X",
              (unsigned)yel_pass->bm[2]);
    CHECK_MSG((red_pass->bm[3] | grn_pass->bm[3] | yel_pass->bm[3]) == 0x00,
              "字节 3：两平面都为 0 → 三遍都不画");
    CHECK_MSG(red_pass->bm[4] == 0xF0 && yel_pass->bm[4] == 0x0F,
              "字节 4：翻转后红 0xFF/绿 0x0F → 红独有 F0、黄 0F，实际 %02X/%02X",
              (unsigned)red_pass->bm[4], (unsigned)yel_pass->bm[4]);

    int bad = 0;
    for (uint16_t i = 0; i < P25RLS_PLANE_BYTES; i++) {
        const uint8_t r = reverse_bits(red[i]);  /* 线上 LSB-first → 内部 MSB-first */
        const uint8_t g = reverse_bits(green[i]);
        if ((red_pass->bm[i] != (uint8_t)(r & (uint8_t)~g)) ||
            (grn_pass->bm[i] != (uint8_t)(g & (uint8_t)~r)) ||
            (yel_pass->bm[i] != (uint8_t)(r & g)))
            bad++;
        if ((red_pass->bm[i] & grn_pass->bm[i]) || (red_pass->bm[i] & yel_pass->bm[i]) ||
            (grn_pass->bm[i] & yel_pass->bm[i]))
            bad++;
    }
    CHECK_MSG(bad == 0, "全部 288 字节的三遍掩码必须逐位正确且互不相交（错 %d 处）", bad);
}

/**
 * @brief 显示未写过的槽 / 越界槽号 / 无显示设备：只清屏（或无动作），不画垃圾
 */
static void case_show_guard(void)
{
    TEST_BEGIN("显示门禁：未写过的槽只清屏；无显示设备不动作；越界槽号按未写处理");

    fixture_reset();
    s_cfg_id = 1;
    p25rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 1, "未写过的槽应只清屏 1 笔，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].fill && s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏应为黑");
    CHECK_MSG(s_frame_begin == 0, "只清屏不必开帧窗口（实际 begin=%d）", s_frame_begin);

    /* 越界槽号：按未写处理，不许越界读 s_rec.slot[] */
    fixture_reset();
    s_cfg_id         = 1;
    s_rec.valid_mask = 0xFF;
    p25rls_pic_show(P25RLS_PIC_SLOT_COUNT);
    CHECK_MSG((s_draw_cnt == 1) && s_draws[0].fill, "越界槽号应只清屏，实际 %d 笔", s_draw_cnt);

    /* 无显示设备：直接返回 */
    fixture_reset();
    s_cfg_id  = 1;
    s_dsp_ptr = nullptr;
    p25rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 0, "无显示设备时不该绘制，实际 %d 笔", s_draw_cnt);
}

/**
 * @brief 上电重放：只在"最后显示槽已写过"时重放一次，否则什么都不做
 *
 * 重放多了会把渲染层刚从配置里恢复出来的内容盖成黑屏；重放少了就是"断电后图没了"。
 */
static void case_restore(void)
{
    TEST_BEGIN("上电重放：last_shown 已写过 → 重放一次；否则不动作");

    /* 正常重放 */
    fixture_reset();
    s_cfg_id = 1;

    uint8_t bm[P25RLS_BITMAP_BYTES];
    bitmap_make(0x80, 0x00, bm);
    CHECK_MSG(p25rls_pic_save(1, bm), "先存槽 1");
    s_draw_cnt    = 0;
    s_frame_begin = s_frame_end = 0;

    const bool painted = p25rls_pic_restore();
    CHECK_MSG(painted, "有有效槽时应返回 true（= 上电画面已被接管）");
    CHECK_MSG(s_frame_begin == 1 && s_frame_end == 1, "重放应走一次完整帧窗口（begin=%d end=%d）",
              s_frame_begin, s_frame_end);
    CHECK_MSG(s_draw_cnt == 4, "重放应是清屏 + 三遍共 4 笔，实际 %d", s_draw_cnt);

    /* last_shown 指向没写过的槽 */
    fixture_reset();
    s_cfg_id         = 1;
    s_rec.last_shown = 0;
    CHECK_MSG(!p25rls_pic_restore(), "槽未写过 → 返回 false（交回 app_boot 兜底）");
    CHECK_MSG(s_draw_cnt == 0, "槽未写过 → 不重放，实际 %d 笔", s_draw_cnt);

    /* last_shown = 0xFF（新板/空记录）*/
    fixture_reset();
    s_cfg_id = 1;
    CHECK_MSG(!p25rls_pic_restore(), "last_shown=0xFF → 返回 false");
    CHECK_MSG(s_draw_cnt == 0, "last_shown=0xFF → 不重放，实际 %d 笔", s_draw_cnt);

    /* 无显示设备 */
    fixture_reset();
    s_cfg_id         = 1;
    s_dsp_ptr        = nullptr;
    s_rec.valid_mask = 0x01;
    s_rec.last_shown = 0;
    CHECK_MSG(!p25rls_pic_restore(), "无显示设备时（画不出）仍算接管？——应为 false");
    CHECK_MSG(s_draw_cnt == 0, "无显示设备时不该绘制");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[1mP25RLS 图片槽\033[0m\n");

    case_register();
    case_layout();
    case_load();
    case_save();
    case_show();
    case_show_guard();
    case_restore();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
