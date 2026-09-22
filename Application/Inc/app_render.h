/**
 * @file    app_render.h
 * @brief   文字/图形渲染 — tagged union 统一入参
 *
 * 字库存储为 (字号, 编码, 字型) 三元组的顺序拼接。
 * **具体有哪些三元组、各自多大、怎么索引，是板级事实** —— 由
 * boards/<板>/Src/font_lib_board.c 的 g_board_font 描述，本文件只声明接口。
 * 新增渲染类型只需在 render_type_t 和 union 中追加。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dev_display.h"
#include "dev_storage.h"

/* ---- 字号（像素高度，ASCII 半宽 = size/2）----
 *
 * 这里列的是**全部可能取值的并集**，不是某块板实际有的集合 —— 两板实测就差很多
 * （3833024 是 14/16/20/24/32，5006048 是 16/24/32/48）。本板到底有哪些，
 * 看 g_board_font.sizes[]。
 *
 * 调用方请求了本板没有的字号时**回落到最接近的一个**（见 app_render.c 的
 * _resolve_size），不会静默按别的字库单元渲染。 */
typedef enum {
    FONT_SELF_ADAPT = 0,
    FONT_14         = 14,
    FONT_16         = 16,
    FONT_20         = 20,
    FONT_24         = 24,
    FONT_32         = 32,
    FONT_48         = 48,
} font_size_t;

/* ---- 字型 ---- */
typedef enum {
    FONT_ST = 0, /* 宋体 */
    FONT_FS = 1, /* 仿宋 */
    FONT_KT = 2, /* 楷体 */
    FONT_HT = 3, /* 黑体 */
} font_type_t;

/* ---- 编码类型 ---- */
typedef enum {
    FONT_ENC_ASCII = 0, /* 字库编码 — ASCII 单字节 */
    FONT_ENC_GBK   = 1, /* 字库编码 — GBK  双字节 */
    FONT_ENC_UTF8  = 2, /* 输入文本编码 — 内部自动转 GBK */
} font_enc_t;

/* ---- 字库三元组：内部检索 key，调用方无需接触 ---- */
typedef struct {
    font_size_t size;   /* 字号 */
    font_enc_t charset; /* 字库编码 (ASCII/GBK) — 渲染器内部按字符自动填充 */
    font_type_t type;   /* 字型 */
} font_key_t;

/* ---- 汉字在字库单元内的索引方式 ----
 * 两版字库的区位基准不同，差错了就是整块取到别人的字形（不报错、只是显示成别的字）：
 *   GBK    : (hi-0x81)*190 + (lo - (lo>=0x80 ? 0x41 : 0x40))   23940 字
 *   GB2312 : 94*(hi-0xA1) + (lo-0xA1)                           8836 字 = 94×94 区位全集
 * 由板级表逐块指定 —— 同一个型号的屏换一批字库母片就可能换一种。 */
typedef enum {
    FONT_IDX_GBK = 0,
    FONT_IDX_GB2312,
} font_idx_kind_t;

/* ---- 字库单元（板级表的一项）---- */
typedef struct {
    font_key_t key;
    uint32_t   unit_size; /* 该三元组在 Flash 中占用的总字节数 */
} font_unit_t;

/* ---- 板级字库描述 ----
 *
 * 单元在 W25Qxx 中**从地址 0 起线性连续**排列：第 i 项的起始偏移 = 前 i 项的
 * unit_size 之和（两板的实物映像都验过是严丝合缝的链，无空洞）。
 *
 * **因此 lib[] 的顺序必须与实物映像的排布逐项一致。** 顺序错了不会有任何报错，
 * 只会让每种字型都取到别人的字形（旧 feat/old_font_lib 分支就踩在这上面：
 * 它按 ST,FS,KT,HT 排，而 5006048 的映像地理顺序是 FS,HT,KT,ST，24/32 项错位）。
 *
 * 唯一实例 g_board_font 由 boards/<板>/Src/font_lib_board.c 提供。 */
typedef struct {
    const font_unit_t *lib;
    uint16_t           lib_count;
    const font_size_t *sizes;     /* 本板可用字号，**必须升序**（最近邻回落依赖它） */
    uint8_t            size_count;
    uint8_t            asc_index_base; /* ASCII 索引起点：0x20（96 槽）或 0x00（128 槽） */
    font_idx_kind_t    gb_index;       /* 汉字索引式 */
    uint32_t           total_bytes;    /* 必须等于 lib[] 各项之和 */
} font_lib_desc_t;

extern const font_lib_desc_t g_board_font;

/* 容量契约用编译期量 BOARD_FONT_LIB_TOTAL_BYTES（在 board.h，因为 app_cfg_sched.h
 * 的 _Static_assert 要用它）。g_board_font.total_bytes 是它的运行期副本，两者由
 * _render_init 的交叉校验钉住 —— 单靠编译期常量挡不住"表里写错一项"。 */

/* ---- 水平/垂直对齐 ---- */
typedef enum {
    ALIGN_LEFT_UP    = 0, /* 左对齐 / 上对齐 */
    ALIGN_CENTER     = 1, /* 居中 */
    ALIGN_RIGHT_DOWN = 2, /* 右对齐 / 下对齐 */
} align_t;

/* ---- 渲染风格（文字专属）---- */
typedef struct {
    align_t h_align; /* 水平对齐 */
    align_t v_align; /* 垂直对齐 */
    bool word_wrap;  /* 超宽时自动换行 */
} render_style_t;

/* ---- 渲染类型：告诉 app_render 如何解析 union ---- */
typedef enum {
    RENDER_TEXT   = 0, /* 文字渲染 — 使用 text/len/font_key/style/text_enc */
    RENDER_BITMAP = 1, /* 位图渲染 — 使用 bitmap/w/h           */
    RENDER_FILL   = 2, /* 矩形填充 — 使用公共字段 x/y/w/h/color (w/h=0 全屏) */
} render_type_t;

/* ---- 统一渲染参数 — tagged union — type 决定哪个 union 分支生效 ---- */
typedef struct {
    /* 公共 — 调用方设置后渲染器只读 */
    const uint16_t x, y;         /* 目标起点 */
    const uint16_t w, h;         /* 目标宽高 (fill 时 w/h=0 表示全屏) */
    const display_color_t color; /* 绘制颜色 */
    const render_type_t type;    /* 标签: 指定使用哪个 union 分支 */

    /** @brief **这一帧的内容要落盘**（掉电再上电自动恢复）。默认 false。
     *
     *  语义是"**内容定稿之后**存"，不是"渲染完立刻存"：渲染返回时内容还在画布上、
     *  没有落屏，此刻读实屏存下去的是**上一帧**。所以这里只记一个请求位，
     *  由 `app_screen` 在**落屏之后**（commit_bitmap / commit_self）取走。
     *  没有画布（单卡、或从卡直写实屏）时画的就是实屏，当场就存 —— 与加这个成员
     *  之前的行为逐字一致。
     *
     *  **显式开关**：不给"每次都写"的默认 —— W25Qxx 每扇区约 10 万次擦写，
     *  而内容可能是几秒一变的。 */
    const bool persist;

    union {
        /* RENDER_TEXT — 文字专属 */
        struct {
            const char *text;            /* 字符串 */
            const uint16_t len;          /* 字符串长度（字节数） */
            const font_size_t font_size; /* 字号 */
            const font_type_t font_type; /* 字型 */
            const render_style_t *style; /* 对齐/换行 (NULL=默认) */
            const font_enc_t text_enc;   /* 输入文本编码 (UTF8需转换/GBK直通) */
        };

        /* RENDER_BITMAP — 位图专属 */
        const uint8_t *const bitmap; /* 位图数据, 每行 (w+7)/8 字节, MSB first */

        /* RENDER_FILL — 无专属字段, 只用公共的 x/y/w/h/color */
    };
} render_cfg_t;

/* ---- 可插拔渲染目标 ----
 *
 * 渲染引擎默认直接画到 `dev_display_get()` 那块实屏上。但级联场景里主卡要画到
 * **覆盖整屏的逻辑画布**（比它自己那块屏大），画完再切分下发 —— 那需要换渲染目标。
 *
 * **为什么不复制一份渲染引擎**：前人那版联动（MSL, `origin/wire_p20`）就是复制了
 * 一份 `msl_render_*`，把 `_render_text` 里 200 行排版/换行/对齐/字库索引逻辑抄了
 * 一遍，只为把输出目标从实屏换成虚拟屏。那是本设计明确要避免的形态 —— 两份排版
 * 逻辑必然漂移，而漂移的表现是"某个字号/对齐方式下两卡排版不一致"，极难查。
 *
 * 走这条缝的只有 3 个原语 + 2 处几何读取（见 app_render.c），排版逻辑一行不动。 */
typedef struct {
    void (*fill)(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t c);
    void (*bitmap)(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bm,
                   display_color_t c);
    void (*set_pixel)(void *ctx, uint16_t x, uint16_t y, display_color_t c);
    void    *ctx;
    uint16_t rows; /**< 目标宽（_render_fill 的全屏语义、_render_text 的边界判断要用） */
    uint16_t cols; /**< 目标高 */
} render_target_t;

/** @brief 换渲染目标；传 NULL 回落 `dev_display_get()`（默认）。
 *
 *  可逆：传 NULL 即恢复直写实屏，级联整套关掉时靠它。 */
void app_render_set_target(const render_target_t *t);

/** @brief 显存持久化的接管钩子
 *
 *  `app_render_save/restore` 原先直接对 `dev_display_t` 读写。装了逻辑画布之后
 *  那样会错位：主卡自己的带恢复了旧内容、画布却是黑的。注册了钩子就整体委托 ——
 *  由画布所有者决定"存什么、从哪恢复"。不注册则保持原行为。 */
typedef struct {
    void (*save)(void);
    bool (*restore)(void);
} render_persist_hook_t;

void app_render_set_persist_hook(const render_persist_hook_t *h);

/* ---- API（模块自注册 sw_app_initcall，调用方无需传 display/font 句柄）---- */

/** @brief 统一渲染入口 — 根据 cfg->type 分派到内部实现 */
void app_render(const render_cfg_t *cfg);

/* ---- 持久化显示 ----
 *
 * 归属（注册名 "render_persist"）、格式版本、长度、CRC32 由配置记录头统一管理
 * （见 Device/Inc/cfg_record.h），本结构只承载"渲染状态"本身。 */

typedef struct [[gnu::packed]] {
    uint16_t screen_rows;
    uint16_t screen_cols;
    uint8_t  color;    /* 非黑像素颜色 (display_color_t) */
    uint8_t  bitmap[]; /* (cols × ((rows+7)/8)) 字节, MSB first per row */
} render_persist_t;

/** 记录格式版本。布局变更时 +1 —— 版本不符会被判为记录失效、回落默认，
 *  而不是让记录搬家（见 app_cfg_sched.c 的扫描认领）。 */
#define RENDER_PERSIST_VERSION (1U)

/**
 * @brief 位图区上限（字节）—— 本工程树内最大显示模组的 1bpp 位图
 *
 * P10 2200001703（10×4 个 32×16 模组 → screen_rows=320, screen_cols=64）:
 *   ((320 + 7) / 8) × 64 = 40 × 64 = 2560
 *
 * 本工程的真 dev_display.h 里**没有**编译期几何宏（尺寸是运行期从 dev_display_t 读的），
 * 所以这里只能取"全工程最大值"这个常量，由 app_render_save 在写入前按运行期几何
 * 检查来兜底。新增更大的模组时这里与 CFG_RECORD_MAX_IMAGE 都要跟着调 ——
 * 两者的一致性由 app_cfg_sched.h 的 _Static_assert 守住。 */
#define RENDER_PERSIST_BITMAP_MAX (2560U)

/** @brief 显存持久化载荷上限 = 位图上限 + 头部(5B) */
#define RENDER_PERSIST_PAYLOAD_MAX (sizeof(render_persist_t) + RENDER_PERSIST_BITMAP_MAX)

/** @brief "这一帧要落盘"的请求位 —— 见 render_cfg_t.persist 的说明
 *
 *  取走（清标志）只在**落屏之后**做：`app_screen` 的 commit 路径是唯一的消费者。
 *  `peek` 给级联开轮用（帧在落屏之前发出去，那时还不能取）。 */
bool app_render_take_persist_req(void);
bool app_render_peek_persist_req(void);

/** @brief 将当前显存写入存储设备持久化扇区 */
void app_render_save(void);

/** @brief 从存储设备加载持久化数据恢复显存
 *  @return true=成功恢复并置脏标记, false=无有效数据/尺寸不匹配/CRC错误 */
bool app_render_restore(void);
