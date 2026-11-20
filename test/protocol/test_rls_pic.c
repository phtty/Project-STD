/**
 * @file    test_rls_pic.c
 * @brief   RLS 图片槽：SV/DS 的 3 槽存储、配置记录加载/落盘、几何门禁、SW1~SW3 干接点
 *
 * 命令层那半（SV/DS 怎么解析帧、编号怎么分派）在 `test_rls.c` 里；本套件管的是
 * 它桩掉的那层：`app_rls_pic.c` 的槽镜像、持久化、显示与按键任务。
 *
 * **为什么需要这个测试**：
 *   1. 槽布局是**持久化契约**（`valid_mask` + 3 × (颜色 + 512B 位图)），改布局不升
 *      版本号，旧记录会被按新布局解出乱码位图 —— 上电就是花屏；
 *   2. 加载回调要处理**空/损坏/长度不符**三种记录，任何一种漏判都会把 Flash 里的
 *      垃圾当图显示；
 *   3. 本屏几何不是 128×32（512B 位图）时**只能清屏**，按错误 stride 读 512B 就是
 *      越界读；
 *   4. SW1/SW2/SW3 干接点 → 槽 0/1/2 的映射是现场"按键换图"的唯一通路；本板没有
 *      这些键时任务必须退出，否则 100% 占一个任务空转。
 *
 * 手法：TU-include 生产 `app_rls_pic.c`（`s_rec` / 加载回调 / 按键任务都是 static），
 * 只把外部依赖换成记录桩：配置调度器、渲染、屏几何、显示门面、按键、任务创建。
 * 按键任务真的跑在 pthread 上（`dev_key_wait_press` 用假 key 的 ops 驱动）。
 */

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_cfg_sched.h"
#include "app_render.h"
#include "app_rls_pic.h"
#include "app_screen.h"
#include "dev_cfg_record.h"
#include "dev_display.h"
#include "dev_key.h"
#include "pl_task.h"

/* ================================================================
 *  断言（与 test_rls 同一变体 B，见 docs/测试约定.md 附录 B）
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
 *  生产模块本体（TU-include：槽镜像、加载回调、按键任务都是 static）
 * ================================================================ */

#include "../../Application/Src/RLS/app_rls_pic.c"

/* ================================================================
 *  记录桩
 * ================================================================ */

/* ---- 配置调度器：注册 / 加载 / 保存 ---- */
static const app_cfg_sched_desc_t *s_reg_desc;
static uint8_t                     s_reg_ret = 2;

static int                    s_load_cnt;
static dev_cfg_record_state_t s_load_state;
static uint16_t               s_load_len;
static uint8_t                s_load_payload[2048];

static int      s_save_cnt;
static uint8_t  s_save_id;
static uint16_t s_save_len;
static uint8_t  s_save_payload[2048];
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

/* ---- 渲染：只记类型/几何/颜色/持久化/位图指针 ---- */
typedef struct {
    bool                fill;
    bool                bitmap;
    uint16_t            w;
    uint16_t            h;
    uint8_t             color;
    bool                persist;
    const uint8_t      *bm;
} draw_t;

#define DRAW_MAX (8U)
static draw_t s_draws[DRAW_MAX];
static int    s_draw_cnt;

void app_render(const app_render_cfg_t *cfg)
{
    if (s_draw_cnt >= (int)DRAW_MAX) return;

    draw_t *d   = &s_draws[s_draw_cnt++];
    d->fill     = (cfg->type == APP_RENDER_TYPE_FILL);
    d->bitmap   = (cfg->type == APP_RENDER_TYPE_BITMAP);
    d->w        = cfg->w;
    d->h        = cfg->h;
    d->color    = (uint8_t)cfg->color;
    d->persist  = cfg->persist;
    d->bm       = d->bitmap ? cfg->bitmap : nullptr;
}

/* ---- 屏几何（默认 128×32 → 512B，与槽大小一致）---- */
static uint16_t s_rows = 128;
static uint16_t s_cols = 32;

uint16_t app_screen_rows(void) { return s_rows; }
uint16_t app_screen_cols(void) { return s_cols; }

/* ---- 显示门面：帧窗口计数 + 可切换的"无显示" ---- */
static dev_display_t s_dsp;
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

/* ---- 按键：本板是否装了 SW1~SW3；按下脚本由假 key 的 ops 驱动 ---- */
static bool            s_keys_present;
static bool            s_wait_press_ret = true;
static int             s_wait_cnt;
static uint32_t        s_wait_timeout;
static int             s_key_get_cnt[DEV_KEY_COUNT];

static bool _fake_get_state(dev_key_t *key)
{
    (void)key;
    return false;
}

/** @brief 假按键的 wait_press：按脚本返回 true；第 4 次调用直接退出任务线程
 *
 *  生产任务是一个 `for (;;)` 循环，只能从"等按键"里退出 —— 真机上靠
 *  `osThreadExit()`，这里用 `pthread_exit()` 让线程结束，父线程 join 后断言。 */
static bool _fake_wait_press(dev_key_t *key, uint32_t timeout_ms)
{
    (void)key;
    s_wait_cnt++;
    s_wait_timeout = timeout_ms;
    if (s_wait_cnt <= 3) return s_wait_press_ret;
    pthread_exit(NULL);
    return false;
}

static const dev_key_ops_t s_fake_ops = {
    .get_state  = _fake_get_state,
    .wait_press = _fake_wait_press,
};

static dev_key_t s_fake_keys[DEV_KEY_COUNT];

dev_key_t *dev_key_get(dev_key_id_t id)
{
    if (id >= DEV_KEY_COUNT) return nullptr;
    s_key_get_cnt[id]++;
    if (!s_keys_present) return nullptr;
    if (id > DEV_KEY_SW3) return nullptr;

    dev_key_t *k = &s_fake_keys[id];
    k->ops       = &s_fake_ops;
    k->id        = id;
    return k;
}

/* ---- 任务创建：本套件不跑真实任务框架，只保证符号存在 ---- */
osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    return nullptr;
}

/* ================================================================
 *  看门狗
 *
 *  按键任务是真的 pthread；生产里的循环若因为映射写错而永远等不到按键，
 *  回归会以"挂死"而不是"失败"的形式出现。用 SIGALRM 把它转成明确失败。
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似干接点任务死循环）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* ================================================================
 *  夹具
 * ================================================================ */

/** @brief 清空所有记录与外部状态（每个用例开头调一次） */
static void fixture_reset(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_cfg_id = 0xFF;
    s_lock   = nullptr;

    s_reg_desc = nullptr;
    s_reg_ret  = 2;

    s_load_cnt  = 0;
    s_load_state = DEV_CFG_RECORD_STATE_OK;
    s_load_len  = (uint16_t)sizeof(s_rec);
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

    s_keys_present  = false;
    s_wait_cnt      = 0;
    s_wait_timeout  = 0;
    memset(s_key_get_cnt, 0, sizeof(s_key_get_cnt));
}

/** @brief 往 RAM 镜像里摆一个"已写过"的槽（不走 save，用于显示/按键用例） */
static void _seed_slot(uint8_t slot, uint8_t color, uint8_t pattern)
{
    s_rec.slot[slot].color = color;
    memset(s_rec.slot[slot].bitmap, pattern, RLS_PIC_BITMAP_BYTES);
    s_rec.valid_mask |= (uint8_t)(1U << slot);
}

/* ================================================================ */

/**
 * @brief 注册：名字/版本/加载回调是记录归属契约；注册失败要降级成"只 mem 不落盘"
 *
 * 记录按**名字**认领，改名等于换模块（旧记录回落"未写入"），所以名字拼错是
 * "重启后图全丢"；注册失败（配置区满/重名）则不能停机 —— 本次会话仍要能显示。
 */
static void case_register(void)
{
    fixture_reset();

    TEST_BEGIN("自注册：rls_pic v1 + 加载回调；注册失败退化为只存 RAM");

    _rls_pic_register();

    CHECK_MSG(s_reg_desc != nullptr, "注册回调没有下发描述符");
    CHECK_MSG(s_reg_desc != nullptr && strcmp(s_reg_desc->name, "rls_pic") == 0,
              "记录名必须是 rls_pic（改名=换模块），得到 %s",
              s_reg_desc ? s_reg_desc->name : "(null)");
    CHECK_MSG(s_reg_desc != nullptr && s_reg_desc->version == 1,
              "记录版本应为 1（布局变才升），得到 %u",
              s_reg_desc ? (unsigned)s_reg_desc->version : 0);
    CHECK_MSG(s_reg_desc != nullptr && s_reg_desc->load == _rls_pic_load,
              "加载回调没有指向 _rls_pic_load");
    CHECK_MSG(s_cfg_id == 2 && s_lock != nullptr, "注册成功应拿到 id=2 并建互斥量（id=%u）",
              (unsigned)s_cfg_id);

    /* 注册失败：id 保持 0xFF，save 不落盘但 RAM 更新、show 仍能显示 */
    fixture_reset();
    s_reg_ret = 0xFF;
    _rls_pic_register();

    const uint8_t bm[RLS_PIC_BITMAP_BYTES] = {0};
    CHECK_MSG(s_cfg_id == 0xFF, "注册失败应保持 id=0xFF，得到 %u", (unsigned)s_cfg_id);
    CHECK_MSG(!rls_pic_save(0, bm, 4), "无记录句柄时 save 必须返回 false（落盘失败）");
    CHECK_MSG(s_save_cnt == 0, "无记录句柄时不得调用调度器 save（调用 %d 次）", s_save_cnt);
    CHECK_MSG(s_rec.valid_mask == 0x01, "无记录句柄时 RAM 仍要更新，valid_mask=%02X",
              (unsigned)s_rec.valid_mask);

    rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 2 && s_draws[1].bitmap && s_draws[1].color == 4,
              "无记录句柄时 show 仍应显示 RAM 里的槽（绘制 %d 次）", s_draw_cnt);
}

/**
 * @brief 记录布局是持久化契约：槽 = 1B 颜色 + 512B 位图，整条记录必须装进最大映像
 */
static void case_record_layout(void)
{
    fixture_reset();

    TEST_BEGIN("记录布局：槽 513B / 整条 1540B，且装得进配置记录最大映像");

    CHECK_MSG(RLS_PIC_SLOT_COUNT == 3, "协议 DS/SV 的图片编号只有 0/1/2，槽数 %u",
              (unsigned)RLS_PIC_SLOT_COUNT);
    CHECK_MSG(RLS_PIC_BITMAP_BYTES == 512, "128×32 的 1bpp = 512B，得到 %u",
              (unsigned)RLS_PIC_BITMAP_BYTES);
    CHECK_MSG(sizeof(rls_pic_slot_t) == 1U + RLS_PIC_BITMAP_BYTES,
              "单槽布局 = 颜色 + 位图（%u B），得到 %u", (unsigned)(1U + RLS_PIC_BITMAP_BYTES),
              (unsigned)sizeof(rls_pic_slot_t));
    CHECK_MSG(sizeof(rls_pic_record_t) == 1U + RLS_PIC_SLOT_COUNT * sizeof(rls_pic_slot_t),
              "整条记录 = 有效位 + 3 槽（%u B），得到 %u",
              (unsigned)(1U + RLS_PIC_SLOT_COUNT * sizeof(rls_pic_slot_t)),
              (unsigned)sizeof(rls_pic_record_t));
    CHECK_MSG(sizeof(rls_pic_record_t) + DEV_CFG_RECORD_HDR_SIZE <= DEV_CFG_RECORD_MAX_IMAGE,
              "记录 %u B + 头 %u B 超过最大映像 %u B", (unsigned)sizeof(rls_pic_record_t),
              (unsigned)DEV_CFG_RECORD_HDR_SIZE, (unsigned)DEV_CFG_RECORD_MAX_IMAGE);
}

/**
 * @brief 加载：有效记录按字节恢复；空/损坏/长度不符一律回落"三槽都未写"
 *
 * 这几个分支写错的症状是"上电花屏/显示上一版的残图"，而且只在真机掉电后出现。
 */
static void case_load(void)
{
    fixture_reset();

    TEST_BEGIN("加载：有效记录恢复三槽；空/损坏/长度不符回落全零");

    s_cfg_id = 2;

    /* 有效记录：valid_mask=0b101，槽 0 颜色 3、槽 2 颜色 5 */
    memset(s_load_payload, 0, sizeof(s_load_payload));
    s_load_payload[0] = 0x05;
    s_load_payload[1 + 0] = 3;
    memset(&s_load_payload[1 + 1], 0xA5, RLS_PIC_BITMAP_BYTES);
    s_load_payload[1 + 2 * sizeof(rls_pic_slot_t)] = 5;
    memset(&s_load_payload[1 + 2 * sizeof(rls_pic_slot_t) + 1], 0x5A, RLS_PIC_BITMAP_BYTES);

    s_load_len = (uint16_t)sizeof(s_rec);
    _rls_pic_load();

    CHECK_MSG(s_load_cnt == 1, "应加载 1 次（实际 %d 次）", s_load_cnt);
    CHECK_MSG(s_rec.valid_mask == 0x05, "有效位应恢复成 0b101，得到 %02X",
              (unsigned)s_rec.valid_mask);
    CHECK_MSG(s_rec.slot[0].color == 3 && s_rec.slot[2].color == 5,
              "槽 0/2 颜色应为 3/5，得到 %u/%u", (unsigned)s_rec.slot[0].color,
              (unsigned)s_rec.slot[2].color);
    CHECK_MSG(s_rec.slot[0].bitmap[0] == 0xA5 && s_rec.slot[2].bitmap[0] == 0x5A,
              "槽 0/2 位图首字节应为 A5/5A，得到 %02X/%02X", (unsigned)s_rec.slot[0].bitmap[0],
              (unsigned)s_rec.slot[2].bitmap[0]);

    /* 损坏记录：不能把半截垃圾留在镜像里 */
    memset(&s_rec, 0xCC, sizeof(s_rec));
    s_load_state = DEV_CFG_RECORD_STATE_INVALID;
    _rls_pic_load();
    CHECK_MSG(s_rec.valid_mask == 0, "损坏记录必须回落全零，得到 valid_mask=%02X",
              (unsigned)s_rec.valid_mask);

    /* 长度不符（状态 OK 也不能用） */
    memset(&s_rec, 0xCC, sizeof(s_rec));
    s_load_state = DEV_CFG_RECORD_STATE_OK;
    s_load_len   = (uint16_t)(sizeof(s_rec) - 1U);
    _rls_pic_load();
    CHECK_MSG(s_rec.valid_mask == 0, "长度不符必须回落全零，得到 valid_mask=%02X",
              (unsigned)s_rec.valid_mask);

    /* 未注册：不读记录，也不破坏现有镜像（本模块无记录时的行为） */
    memset(&s_rec, 0x11, sizeof(s_rec));
    s_rec.valid_mask = 0x01;
    s_cfg_id         = 0xFF;
    const int before = s_load_cnt;
    _rls_pic_load();
    CHECK_MSG(s_load_cnt == before && s_rec.valid_mask == 0x01,
              "未注册时不得读记录、也不得改动镜像（load=%d）", s_load_cnt);
}

/**
 * @brief 保存：先改 RAM 再落盘；落盘失败要返回 false 但 RAM 保留；越界/空指针零副作用
 */
static void case_save(void)
{
    fixture_reset();

    TEST_BEGIN("保存：RAM+落盘；落盘失败 RAM 保留；槽越界/空位图零副作用");

    s_cfg_id = 2;
    uint8_t bm[RLS_PIC_BITMAP_BYTES];
    for (uint16_t i = 0; i < sizeof(bm); i++) bm[i] = (uint8_t)(i * 3U + 1U);

    CHECK_MSG(rls_pic_save(1, bm, 6), "save 应成功（落盘 rc=0）");
    CHECK_MSG(s_save_cnt == 1 && s_save_id == 2 && s_save_len == sizeof(s_rec),
              "落盘应调用 1 次、id=2、长度=%u（实际 %d/%u/%u）", (unsigned)sizeof(s_rec),
              s_save_cnt, (unsigned)s_save_id, (unsigned)s_save_len);
    CHECK_MSG(s_rec.valid_mask == 0x02 && s_rec.slot[1].color == 6,
              "RAM 应立刻更新（valid_mask=%02X color=%u）", (unsigned)s_rec.valid_mask,
              (unsigned)s_rec.slot[1].color);
    CHECK_MSG(memcmp(s_rec.slot[1].bitmap, bm, sizeof(bm)) == 0, "槽内位图应等于入参");
    CHECK_MSG(memcmp(s_save_payload, &s_rec, sizeof(s_rec)) == 0,
              "落盘载荷应就是整条 RAM 镜像");

    /* 落盘失败：RAM 已更新，返回值 false（调用方只打日志） */
    s_save_rc = -1;
    uint8_t bm2[RLS_PIC_BITMAP_BYTES] = {0};
    bm2[0] = 0x7E;
    CHECK_MSG(!rls_pic_save(0, bm2, 2), "落盘失败时 save 应返回 false");
    CHECK_MSG(s_rec.valid_mask == 0x03 && s_rec.slot[0].color == 2 &&
                  s_rec.slot[0].bitmap[0] == 0x7E,
              "落盘失败也要保留 RAM（valid_mask=%02X color=%u）", (unsigned)s_rec.valid_mask,
              (unsigned)s_rec.slot[0].color);

    /* 越界槽 / 空位图：不得碰 RAM、不得落盘 */
    const uint8_t mask_before = s_rec.valid_mask;
    const int     saves_before = s_save_cnt;
    CHECK_MSG(!rls_pic_save(RLS_PIC_SLOT_COUNT, bm, 1), "槽号 = 槽数必须被拒");
    CHECK_MSG(!rls_pic_save(0, nullptr, 1), "空位图必须被拒");
    CHECK_MSG(s_rec.valid_mask == mask_before && s_save_cnt == saves_before,
              "非法入参必须零副作用（mask=%02X save=%d）", (unsigned)s_rec.valid_mask,
              s_save_cnt);
}

/**
 * @brief 显示：已写槽 = 清屏 + 带 persist 的位图；未写/几何不符/越界只清屏
 *
 * "几何不符只清屏"是这条用例的核心：槽里固定 512B（128×32），换屏后按本屏 stride
 * 读它就是越界读；"未写槽只清屏"则防把全零/垃圾位图画上屏。
 */
static void case_show(void)
{
    fixture_reset();

    TEST_BEGIN("显示：已写槽清屏+位图(persist)；未写/几何不符/越界只清屏");

    _seed_slot(1, 6, 0x3C);

    rls_pic_show(1);
    CHECK_MSG(s_frame_begin == 1 && s_frame_end == 1, "清+画应包在同一个帧窗口里（%d/%d）",
              s_frame_begin, s_frame_end);
    CHECK_MSG(s_draw_cnt == 2 && s_draws[0].fill && s_draws[1].bitmap,
              "已写槽应先清屏再画位图（绘制 %d 次）", s_draw_cnt);
    CHECK_MSG(s_draws[1].w == 128 && s_draws[1].h == 32, "几何应取逻辑屏 128×32，得到 %ux%u",
              (unsigned)s_draws[1].w, (unsigned)s_draws[1].h);
    CHECK_MSG(s_draws[1].color == 6 && s_draws[1].bm == s_rec.slot[1].bitmap,
              "颜色/位图指针应取槽 1（color=%u）", (unsigned)s_draws[1].color);
    CHECK_MSG(s_draws[1].persist, "SV 的断电保存：位图必须带 persist");

    /* 未写槽：只清屏 */
    fixture_reset();
    rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 1 && s_draws[0].fill, "未写槽只清屏（绘制 %d 次）", s_draw_cnt);

    /* 几何不符（64×16 → 128B 够屏，但不够 512B 槽）：只清屏 */
    fixture_reset();
    _seed_slot(0, 1, 0xFF);
    s_rows = 64;
    s_cols = 16;
    rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 1 && s_draws[0].fill,
              "几何不符时必须只清屏、不得按错误 stride 读 512B（绘制 %d 次）", s_draw_cnt);

    /* 槽号越界：按未写处理 */
    fixture_reset();
    rls_pic_show(RLS_PIC_SLOT_COUNT);
    CHECK_MSG(s_draw_cnt == 1 && s_draws[0].fill, "越界槽号只清屏（绘制 %d 次）", s_draw_cnt);

    /* 没有显示实例：什么都不做（连帧窗口都不开） */
    fixture_reset();
    s_dsp_ptr = nullptr;
    rls_pic_show(0);
    CHECK_MSG(s_draw_cnt == 0 && s_frame_begin == 0 && s_frame_end == 0,
              "无显示实例时不得绘制/开关帧窗口（%d/%d）", s_frame_begin, s_frame_end);
}

/** @brief 按键任务线程体：生产函数是无限循环，靠假 wait_press 里的 pthread_exit 退出 */
static void *_key_thread(void *arg)
{
    (void)arg;
    _rls_pic_key_task(nullptr);
    return nullptr;
}

static void _run_key_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, nullptr, _key_thread, nullptr) == 0, "按键任务线程创建失败");
    CHECK_MSG(pthread_join(tid, nullptr) == 0, "按键任务线程未退出");
}

/**
 * @brief 干接点：本板无 SW1~SW3 时任务必须退出；有键时按下 → 显示对应槽
 *
 * 三颗键都查不到还留在循环里，就是 100% 占一个任务空转（`dev_key_wait_press`
 * 对不存在的键立刻返回 false）；映射错了则是"按 SW2 出 SW3 的图"。
 */
static void case_key_task(void)
{
    TEST_BEGIN("干接点：无键退出；SW1/2/3 → 槽 0/1/2，各等 50ms");

    /* 本板无键：三个都查不到 → 直接退出，不能进循环 */
    fixture_reset();
    _run_key_task();
    CHECK_MSG(s_key_get_cnt[DEV_KEY_SW1] == 1 && s_key_get_cnt[DEV_KEY_SW2] == 1 &&
                  s_key_get_cnt[DEV_KEY_SW3] == 1,
              "无键时也应逐个查一遍（%d/%d/%d）", s_key_get_cnt[DEV_KEY_SW1],
              s_key_get_cnt[DEV_KEY_SW2], s_key_get_cnt[DEV_KEY_SW3]);
    CHECK_MSG(s_wait_cnt == 0 && s_draw_cnt == 0, "无键任务不得等待/显示（wait=%d draw=%d）",
              s_wait_cnt, s_draw_cnt);

    /* 有键：脚本按 SW1→SW2→SW3 各按一次，第 4 次等待退出线程 */
    fixture_reset();
    s_keys_present = true;
    _seed_slot(0, 1, 0x11);
    _seed_slot(1, 2, 0x22);
    _seed_slot(2, 4, 0x44);
    _run_key_task();

    CHECK_MSG(s_wait_cnt == 4, "三次按下 + 一次退出等待，实际 wait=%d 次", s_wait_cnt);
    CHECK_MSG(s_wait_timeout == 50, "干接点轮询超时应为 50ms，得到 %u",
              (unsigned)s_wait_timeout);
    CHECK_MSG(s_draw_cnt == 6, "三次显示 × (清屏+位图) = 6 次绘制，得到 %d", s_draw_cnt);
    CHECK_MSG(s_draws[1].color == 1 && s_draws[3].color == 2 && s_draws[5].color == 4,
              "SW1/2/3 应分别显示槽 0/1/2 的颜色（%u/%u/%u）", (unsigned)s_draws[1].color,
              (unsigned)s_draws[3].color, (unsigned)s_draws[5].color);
}

/* ================================================================ */

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36mRLS 图片槽（记录加载/落盘 / 显示门禁 / 干接点）\033[0m\n");

    case_register();
    case_record_layout();
    case_load();
    case_save();
    case_show();
    case_key_task();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
