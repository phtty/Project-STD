/**
 * @file    app_render.c
 * @brief   文字/图形渲染实现 — 数据驱动字库引擎
 *
 * 上电时通过 DEV_KEY_DIP2 拨码开关自动选择字库芯片配置:
 *   - DIP2 逻辑 0（开关 OFF，GPIO 高）→ W25Q64:   非连续布局, 16/24/32pt
 *   - DIP2 逻辑 1（开关 ON，GPIO 低）→ MX25L256: 连续布局, 14/16/20/24/32pt
 *
 * 两种芯片的关键差异:
 *   - W25Q64:   ASCII 用原始字符码, GBK 用94列序 (ch[0]-0xA1)*94+...
 *   - MX25L256: ASCII 用 ch-0x20,   GBK 用190列序 (ch[0]-0x81)*190+...
 *
 * 每种芯片对应一个 font_chip_config_t 实例（含 flash_region_t 地址表）。
 * 通过 app_render_chip_select() 可在运行中手动切换。
 *
 *   参考文档: doc/02_LDI协议与外设接口/字库芯片更换与移植指导.md
 */

#include "app_render.h"

#include <string.h>
#include "text_cvt.h"
#include "initcall.h"
#include "crc_utils.h"
#include "dev_w25qxx.h"
#include "dev_key.h"

/* ---- 字库区块描述：三元组 (size, charset, type) + Flash 地址修正量 ---- */
typedef struct {
    font_size_t size;   /* 字号 (像素高度) */
    font_enc_t charset; /* 编码: FONT_ENC_ASCII / FONT_ENC_GBK */
    font_type_t type;   /* 字型: ST/FS/KT/HT */
    uint16_t base;      /* 扇区基准 (从01Embedded func.h 宏的整数除法得到) */
    int8_t sec_adj;     /* 扇区修正量 X (readaddr 公式中 sec+X) */
    int8_t page_adj;    /* 页修正量 Y (readaddr 公式中 page+Y, 单位256字节) */
    int16_t byte_adj;   /* 字节修正量 Z (readaddr 公式中 byte+Z) */
} flash_region_t;

#define N_ASC_CHARS (96U)
#define N_GBK_CHARS (23940U)

/*
 * W25Q64 字库区块地址表
 *
 * 01Embedded readaddr 公式:
 *   FonfAddr  = char_idx × bytes_per_char
 *   sec       = FonfAddr / 4096
 *   page      = (FonfAddr % 4096) / 256
 *   byte      = (FonfAddr % 4096) % 256
 *   readaddr  = (base + sec + X) × 4096 + (page + Y) × 256 + byte + Z
 *
 * 简化为:
 *   readaddr = (base + sec + X) × 4096 + (page + Y) × 256 + byte + Z
 *            = base × 4096 + (sec + X) × 4096 + (page + Y) × 256 + byte + Z
 *
 * 其中 X, Y, Z 是每个字型的固定修正量，由01Embedded func.c 的6个读取函数推导:
 *   readaddr = (ADDRESS_MACRO + sectoraddr [+X]) * 4096
 *            + (pageaddr [+Y]) * 256 + byteaddr [+Z]
 *
 * 验证: 逐字符地址已与01Embedded的6个读取函数交叉验证
 */
static const flash_region_t g_font_regions[] = {
    /* 16号字体 — W25Q64 无 14/20号 */
    /*                              size  enc     type     base  X  Y    Z    */
    /* 16pt ASCII — bytes_per_char=16 */
    {16, FONT_ENC_ASCII, FONT_ST, 0, 0, 0, 0},  /* (0+sec)*4096 + page*256 + byte */
    {16, FONT_ENC_ASCII, FONT_FS, 0, 0, 0, 0},  /* (0+sec)*4096 + page*256 + byte */
    {16, FONT_ENC_ASCII, FONT_KT, 1, 0, 0, 64}, /* (1+sec)*4096 + page*256 + byte+64 */
    {16, FONT_ENC_ASCII, FONT_HT, 1, 0, 0, 64}, /* (1+sec)*4096 + page*256 + byte+64 */
    /* 16pt GB2312 — bytes_per_char=32 */
    {16, FONT_ENC_GBK, FONT_ST, 2, 0, 0, 128},   /* (2+sec)*4096 + page*256 + byte+128 */
    {16, FONT_ENC_GBK, FONT_FS, 71, 0, 0, 288},  /* (71+sec)*4096 + page*256 + byte+288 */
    {16, FONT_ENC_GBK, FONT_KT, 140, 0, 0, 448}, /* (140+sec)*4096 + page*256 + byte+448 */
    {16, FONT_ENC_GBK, FONT_HT, 209, 0, 2, 96},  /* (209+sec)*4096 + (page+2)*256 + byte+96 */
    /* 24号字体 */
    /* 24pt ASCII — bytes_per_char=48 */
    {24, FONT_ENC_ASCII, FONT_ST, 278, 0, 3, 0},     /* (278+sec)*4096 + (page+3)*256 + byte */
    {24, FONT_ENC_ASCII, FONT_FS, 279, 0, 12, -224}, /* (279+sec)*4096 + (page+12)*256 + byte-224 */
    {24, FONT_ENC_ASCII, FONT_KT, 281, 0, 3, 64},    /* (281+sec)*4096 + (page+3)*256 + byte+64 */
    {24, FONT_ENC_ASCII, FONT_HT, 282, 0, 12, -160}, /* (282+sec)*4096 + (page+12)*256 + byte-160 */
    /* 24pt GB2312 — bytes_per_char=72 */
    {24, FONT_ENC_GBK, FONT_ST, 284, 1, -12, -128}, /* (285+sec)*4096 + (page-12)*256 + byte-128 */
    {24, FONT_ENC_GBK, FONT_FS, 439, 1, -7, -64},   /* (440+sec)*4096 + (page-7)*256 + byte-64 */
    {24, FONT_ENC_GBK, FONT_KT, 594, 1, -2, 0},     /* (595+sec)*4096 + (page-2)*256 + byte */
    {24, FONT_ENC_GBK, FONT_HT, 750, 1, -12, -192}, /* (751+sec)*4096 + (page-12)*256 + byte-192 */
    /* 32号字体 */
    /* 32pt ASCII — bytes_per_char=64 */
    {32, FONT_ENC_ASCII, FONT_ST, 905, 1, -7, -128}, /* (906+sec)*4096 + (page-7)*256 + byte-128 */
    {32, FONT_ENC_ASCII, FONT_FS, 907, 1, -7, -96},  /* (908+sec)*4096 + (page-7)*256 + byte-96 */
    {32, FONT_ENC_ASCII, FONT_KT, 909, 1, -7, -64},  /* (910+sec)*4096 + (page-7)*256 + byte-64 */
    {32, FONT_ENC_ASCII, FONT_HT, 911, 1, -7, -32},  /* (912+sec)*4096 + (page-7)*256 + byte-32 */
    /* 32pt GB2312 — bytes_per_char=128 */
    {32, FONT_ENC_GBK, FONT_ST, 913, 1, -7, 0},   /* (914+sec)*4096 + (page-7)*256 + byte */
    {32, FONT_ENC_GBK, FONT_FS, 1189, 1, -5, 32}, /* (1190+sec)*4096 + (page-5)*256 + byte+32 */
    {32, FONT_ENC_GBK, FONT_KT, 1465, 1, -3, 64}, /* (1466+sec)*4096 + (page-3)*256 + byte+64 */
    {32, FONT_ENC_GBK, FONT_HT, 1741, 1, -1, 96}, /* (1742+sec)*4096 + (page-1)*256 + byte+96 */
};

/*
 * MX25L256 字库地址表（连续布局，从 STD-main-Orig g_font_lib 推算）
 *
 * 布局: 5种字号(14/16/20/24/32) × 4种字型(ST/FS/KT/HT)，每组内 ASCII+GB2312 连续拼接。
 * 地址计算与 W25Q64 共用 sec/page/byte 分步公式:
 *   readaddr = (base + sec + X) * 4096 + (page + Y) * 256 + byte + Z
 * 但 X=0（无扇区修正），Y/Z 用于补偿非扇区对齐的块起始偏移。
 * base 值为累计字节偏移的扇区号，Y/Z 为偏移余数的页/字节拆分。
 * GBK 索引用 190 列序（GBK），非94列序（GB2312）。
 */
static const flash_region_t g_font_regions_orig[] = {
    /*                              size  enc     type  base    X  Y    Z  */
    /* 14pt — bpg: ASCII=14, GBK=28 */
    {14, FONT_ENC_ASCII, FONT_ST,     0,  0,   0,    0},
    {14, FONT_ENC_ASCII, FONT_FS,     0,  0,   5,   64},
    {14, FONT_ENC_ASCII, FONT_KT,     0,  0,  10,  128},
    {14, FONT_ENC_ASCII, FONT_HT,     0,  0,  15,  192},
    {14, FONT_ENC_GBK,   FONT_ST,     1,  0,   5,    0},
    {14, FONT_ENC_GBK,   FONT_FS,   164,  0,  15,  112},
    {14, FONT_ENC_GBK,   FONT_KT,   328,  0,   9,  224},
    {14, FONT_ENC_GBK,   FONT_HT,   492,  0,   4,   80},
    /* 16pt — bpg: ASCII=16, GBK=32 */
    {16, FONT_ENC_ASCII, FONT_ST,   655,  0,  14,  192},
    {16, FONT_ENC_ASCII, FONT_FS,   656,  0,   4,  192},
    {16, FONT_ENC_ASCII, FONT_KT,   656,  0,  10,  192},
    {16, FONT_ENC_ASCII, FONT_HT,   657,  0,   0,  192},
    {16, FONT_ENC_GBK,   FONT_ST,   657,  0,   6,  192},
    {16, FONT_ENC_GBK,   FONT_FS,   844,  0,   7,   64},
    {16, FONT_ENC_GBK,   FONT_KT,  1031,  0,   7,  192},
    {16, FONT_ENC_GBK,   FONT_HT,  1218,  0,   8,   64},
    /* 20pt — bpg: ASCII=40, GBK=60 */
    {20, FONT_ENC_ASCII, FONT_ST,  1405,  0,   8,  192},
    {20, FONT_ENC_ASCII, FONT_FS,  1406,  0,   7,  192},
    {20, FONT_ENC_ASCII, FONT_KT,  1407,  0,   6,  192},
    {20, FONT_ENC_ASCII, FONT_HT,  1408,  0,   5,  192},
    {20, FONT_ENC_GBK,   FONT_ST,  1409,  0,   4,  192},
    {20, FONT_ENC_GBK,   FONT_FS,  1759,  0,  15,  176},
    {20, FONT_ENC_GBK,   FONT_KT,  2110,  0,  10,  160},
    {20, FONT_ENC_GBK,   FONT_HT,  2461,  0,   5,  144},
    /* 24pt — bpg: ASCII=48, GBK=72 */
    {24, FONT_ENC_ASCII, FONT_ST,  2812,  0,   0,  128},
    {24, FONT_ENC_ASCII, FONT_FS,  2813,  0,   2,  128},
    {24, FONT_ENC_ASCII, FONT_KT,  2814,  0,   4,  128},
    {24, FONT_ENC_ASCII, FONT_HT,  2815,  0,   6,  128},
    {24, FONT_ENC_GBK,   FONT_ST,  2816,  0,   8,  128},
    {24, FONT_ENC_GBK,   FONT_FS,  3237,  0,   5,  160},
    {24, FONT_ENC_GBK,   FONT_KT,  3658,  0,   2,  192},
    {24, FONT_ENC_GBK,   FONT_HT,  4078,  0,  15,  224},
    /* 32pt — bpg: ASCII=64, GBK=128 */
    {32, FONT_ENC_ASCII, FONT_ST,  4499,  0,  13,    0},
    {32, FONT_ENC_ASCII, FONT_FS,  4501,  0,   5,    0},
    {32, FONT_ENC_ASCII, FONT_KT,  4502,  0,  13,    0},
    {32, FONT_ENC_ASCII, FONT_HT,  4504,  0,   5,    0},
    {32, FONT_ENC_GBK,   FONT_ST,  4505,  0,  13,    0},
    {32, FONT_ENC_GBK,   FONT_FS,  5253,  0,  15,    0},
    {32, FONT_ENC_GBK,   FONT_KT,  6002,  0,   1,    0},
    {32, FONT_ENC_GBK,   FONT_HT,  6750,  0,   3,    0},
};

/* ---- 自适应字号候选列表 ---- */
static const font_size_t s_adaptive_w25q64[] = {FONT_32, FONT_24, FONT_16};
static const font_size_t s_adaptive_mx25l[]  = {FONT_32, FONT_24, FONT_20, FONT_16, FONT_14};

/* ---- 字库芯片配置实例 ---- */
static const font_chip_config_t s_chip_configs[FONT_CHIP_CNT] = {
    [FONT_CHIP_W25Q64] = {
        .name           = "W25Q64",
        .regions        = g_font_regions,
        .region_count   = sizeof(g_font_regions) / sizeof(g_font_regions[0]),
        .adaptive_sizes = s_adaptive_w25q64,
        .adaptive_count = sizeof(s_adaptive_w25q64) / sizeof(s_adaptive_w25q64[0]),
        .ascii_raw_code = true, /* W25Q64: ASCII 用原始字符码 */
        .gbk_index_190 = false,  /* W25Q64: GB2312 用94列序 (ch[0]-0xA1)*94+... */
    },
    [FONT_CHIP_MX25L256] = {
        .name           = "MX25L256",
        .regions        = g_font_regions_orig,
        .region_count   = sizeof(g_font_regions_orig) / sizeof(g_font_regions_orig[0]),
        .adaptive_sizes = s_adaptive_mx25l,
        .adaptive_count = sizeof(s_adaptive_mx25l) / sizeof(s_adaptive_mx25l[0]),
        .ascii_raw_code = false, /* MX25L256: ASCII 用 ch-0x20 */
        .gbk_index_190 = true,   /* MX25L256: GBK 用190列序 (ch[0]-0x81)*190+... */
    },
};

/* ---- 当前激活的配置 ---- */
static const font_chip_config_t *s_active_config = &s_chip_configs[FONT_CHIP_W25Q64];

/* ---- 内部: 单字符在 Flash 上的打包字节数 ---- */
static inline uint16_t _packed_glyph_bytes(font_size_t size, font_enc_t charset)
{
    uint8_t w = (charset == FONT_ENC_ASCII) ? size / 2 : size;
    return (uint16_t)size * ((w + 7) / 8);
}

/* ---- 内部: 字符像素宽度 ---- */
static inline uint8_t _glyph_width_px(font_size_t size, font_enc_t charset)
{
    return (charset == FONT_ENC_ASCII) ? size / 2 : size;
}

/* ---- 内部: 查找字库区块（从当前激活配置中查找）---- */
static const flash_region_t *_find_region(font_size_t size, font_enc_t charset, font_type_t type)
{
    const flash_region_t *regions = (const flash_region_t *)s_active_config->regions;
    for (uint8_t i = 0; i < s_active_config->region_count; i++) {
        if (regions[i].size == size &&
            regions[i].charset == charset &&
            regions[i].type == type)
            return &regions[i];
    }
    return &regions[0]; /* fallback: 第一个条目 */
}

/* ---- 内部: 单个字符在 Flash 中的绝对字节地址 ----
 *
 * 与01Embedded完全一致的分步计算:
 *   FonfAddr  = char_idx × bytes_per_char
 *   sec       = FonfAddr / 4096
 *   page      = (FonfAddr % 4096) / 256
 *   byte      = (FonfAddr % 4096) % 256
 *   readaddr  = (base + sec + X) × 4096 + (page + Y) × 256 + byte + Z
 */
static uint32_t _flash_addr(font_size_t size, font_enc_t charset, font_type_t type, const uint8_t *ch)
{
    const flash_region_t *r = _find_region(size, charset, type);
    uint16_t bytes          = _packed_glyph_bytes(size, charset);

    /* 计算字符索引 — ascii_raw_code 控制是否减 0x20 */
    uint32_t char_idx;
    if (charset == FONT_ENC_ASCII) {
        if (s_active_config->ascii_raw_code) {
            char_idx = ch[0]; /* W25Q64: 用原始字符码，如 'F'=0x46 */
        } else {
            char_idx = ch[0] - 0x20U; /* MX25L256: ASCII 从 0x20 开始 */
        }
    } else {
        if (s_active_config->gbk_index_190) {
            /* MX25L256: GBK 190列序 — 与 Orig 工程 _char_addr 一致 */
            char_idx = (uint32_t)(ch[0] - 0x81) * 190
                     + (ch[1] >= 0x80 ? ch[1] - 0x41 : ch[1] - 0x40);
        } else {
            /* W25Q64: GB2312 94列序, ch[0]=区号+0xA0, ch[1]=位号+0xA0 */
            char_idx = (uint32_t)(ch[0] - 0xA1) * 94 + (ch[1] - 0xA1);
        }
    }

    /* 按01Embedded公式分步计算 — 关键: X/Y/Z 作用在 sec/page/byte 级别 */
    uint32_t fonf = char_idx * bytes;
    uint32_t sec  = fonf / 4096;
    uint32_t page = (fonf % 4096) / 256;
    uint32_t byte = (fonf % 4096) % 256;

    return (uint32_t)(r->base + sec + r->sec_adj) * 4096 + (uint32_t)(page + r->page_adj) * 256 + byte + r->byte_adj;
}

/* ---- 判断两字节是否为合法 GBK 码 ---- */
static bool _is_gbk(uint8_t high, uint8_t low)
{
    return (high >= 0x81 && high <= 0xFE) && (low >= 0x40 && low <= 0xFE && low != 0x7F);
}

/* ---- 统计文本所需宽高（按现有文本内容、字号、换行符/自动换行计算）---- */
static bool _measure_text_box(const uint8_t *text_buf, uint16_t text_len, font_size_t font_size,
                              uint16_t max_w, bool word_wrap, uint16_t *out_w, uint16_t *out_h)
{
    if (!text_buf || !text_len || !font_size || !out_w || !out_h)
        return false;

    const uint16_t wrap_w = max_w ? max_w : UINT16_MAX;
    uint16_t line_w       = 0;
    uint16_t max_line_w   = 0;
    uint16_t line_cnt     = 1;
    uint16_t char_pos     = 0;

    while (char_pos < text_len) {
        if (text_buf[char_pos] == '\n') {
            if (line_w > max_line_w)
                max_line_w = line_w;
            line_w = 0;
            line_cnt++;
            if (line_cnt > 64)
                return false;
            char_pos++;
            continue;
        }

        uint8_t glyph_w;
        if (text_buf[char_pos] >= 0x20 && text_buf[char_pos] <= 0x7F) {
            glyph_w = _glyph_width_px(font_size, FONT_ENC_ASCII);
            char_pos += 1;
        } else if (char_pos + 1 < text_len && _is_gbk((uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1])) {
            glyph_w = _glyph_width_px(font_size, FONT_ENC_GBK);
            char_pos += 2;
        } else {
            char_pos++;
            continue;
        }

        if (word_wrap && line_w + glyph_w > wrap_w) {
            if (line_w > max_line_w)
                max_line_w = line_w;
            line_cnt++;
            if (line_cnt > 64)
                return false;
            line_w = glyph_w;
            if (line_w > wrap_w)
                return false;
        } else {
            line_w = (uint16_t)(line_w + glyph_w);
            if (!word_wrap && line_w > wrap_w)
                return false;
        }
    }

    if (line_w > max_line_w)
        max_line_w = line_w;

    *out_w = max_line_w;
    *out_h = (uint16_t)(line_cnt * font_size);
    return true;
}

/* ---- 自适应字号选择：从大到小尝试，选择可完整放入区域的最大字号 ---- */
static font_size_t _select_adaptive_font(const uint8_t *text_buf, uint16_t text_len, uint16_t w, uint16_t h,
                                         bool word_wrap)
{
    /* 从当前激活配置中读取自适应字号候选列表 */
    const font_size_t *candidates = s_active_config->adaptive_sizes;
    uint8_t count                 = s_active_config->adaptive_count;

    for (uint8_t i = 0; i < count; i++) {
        font_size_t size = candidates[i];
        uint16_t need_w = 0, need_h = 0;
        if (_measure_text_box(text_buf, text_len, size, w, word_wrap, &need_w, &need_h) && need_w <= w && need_h <= h)
            return size;
    }

    return FONT_16;
}

/* ---- 注册的句柄 ---- */
static dev_display_t *s_render_display;
static dev_storage_t *s_render_font;
static uint32_t s_persist_addr;

/* ---- 内部: 读取 DIP2 拨码开关选择字库芯片 ----
 *
 *  DEV_KEY_DIP2 为 active_low (内部上拉):
 *    开关 OFF (位置 0) → pin HIGH → get_state()=false → 逻辑 0 → MX25L256
 *    开关 ON  (位置 1) → pin LOW  → get_state()=true  → 逻辑 1 → W25Q64
 */
static void _dip_font_select(void)
{
    if (dev_key_get_state(DEV_KEY_DIP2))
        app_render_chip_select(FONT_CHIP_W25Q64);   /* 逻辑 1 → W25Q64 */
    else
        app_render_chip_select(FONT_CHIP_MX25L256); /* 逻辑 0 → MX25L256 */
}

/* ---- 模块自注册（依赖 storage / display / dev_key）---- */
static void _render_init(void)
{
    s_render_display = dev_display_get();
    s_render_font    = dev_w25qxx_get();
    s_persist_addr   = dev_storage_capacity(s_render_font) - 4096 * 2;

    _dip_font_select();
}
sw_app_initcall(_render_init);

/* ---- 字库芯片切换 API ---- */

bool app_render_chip_select(font_chip_id_t id)
{
    if (id >= FONT_CHIP_CNT)
        return false;
    s_active_config = &s_chip_configs[id];
    return true;
}

font_chip_id_t app_render_chip_current(void)
{
    for (uint8_t i = 0; i < FONT_CHIP_CNT; i++) {
        if (s_active_config == &s_chip_configs[i])
            return (font_chip_id_t)i;
    }
    return FONT_CHIP_W25Q64;
}

/* ---- 内部: 渲染趟行宽读取守卫（line_idx 超出测量趟记录行数时钳制到末行，防读穿 line_widths） ---- */
static uint16_t _line_width_get(const uint16_t *widths, uint8_t idx, uint8_t count)
{
    if (count == 0)
        return 0;
    if (idx >= count)
        idx = (uint8_t)(count - 1);
    return widths[idx];
}

/* ---- 渲染分支（各功能静态内联）---- */

static inline void _render_text(const render_cfg_t *cfg)
{
    // 入口参数检查
    if (!cfg->text || !cfg->len)
        return;
    if (!cfg->w || !cfg->h)
        return;

    uint16_t cur_x = cfg->x, cur_y = cfg->y;
    static uint8_t font_buf[512];
    static char text_buf[256];
    uint16_t text_len;

    if (cfg->text_enc == FONT_ENC_UTF8) {
        uint32_t out_len = sizeof(text_buf);
        UTF8ToGBK(cfg->text, cfg->len, text_buf, &out_len);
        text_len = (uint16_t)out_len;
    } else {
        uint16_t n = cfg->len < sizeof(text_buf) ? cfg->len : sizeof(text_buf);
        memcpy(text_buf, cfg->text, n);
        text_len = n;
    }

    font_size_t font_size = cfg->font_size;
    bool word_wrap = cfg->style && cfg->style->word_wrap;
    if (font_size == FONT_SELF_ADAPT) {
        font_size = _select_adaptive_font((const uint8_t *)text_buf, text_len, cfg->w, cfg->h, word_wrap);
    }
    if (!font_size)
        font_size = FONT_16;

    /* ---- 测量趟：记录每行宽度（用于逐行对齐） ---- */
    uint16_t line_widths[32];
    uint8_t line_count = 0;
    uint16_t line_w    = 0;
    uint16_t line_h    = font_size;
    uint16_t char_pos  = 0;

    while (char_pos < text_len) {
        if (text_buf[char_pos] == '\n') {
            /* 防 ≥33 换行文本写穿 line_widths[32]（栈溢出），与折行分支同策略：截断终止 */
            if (line_count >= (sizeof(line_widths) / sizeof(line_widths[0])))
                return;
            line_widths[line_count++] = line_w;
            line_w                    = 0;
            char_pos++;
            continue;
        }

        uint8_t glyph_w;
        if (text_buf[char_pos] >= 0x20 && text_buf[char_pos] <= 0x7F) {
            glyph_w = _glyph_width_px(font_size, FONT_ENC_ASCII);
            char_pos += 1;
        } else if (char_pos + 1 < text_len && _is_gbk((uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1])) {
            glyph_w = _glyph_width_px(font_size, FONT_ENC_GBK);
            char_pos += 2;
        } else {
            char_pos++;
            continue;
        }

        if (line_w + glyph_w > cfg->w) {
            if (word_wrap) {
                /* 防 line_widths[32] 越界写：先判后写（原判断滞后一次写入会写穿下标 32） */
                if (line_count >= (sizeof(line_widths) / sizeof(line_widths[0])))
                    return;
                line_widths[line_count++] = line_w;
                line_w = glyph_w;
            } else {
                /* 不换行：超出部分截断，不计入宽度 */
                continue;
            }
        } else {
            line_w += glyph_w;
        }
    }
    /* 最后一行：同样先判后写防 line_widths[32] 越界 */
    if (line_count >= (sizeof(line_widths) / sizeof(line_widths[0])))
        return;
    line_widths[line_count++] = line_w; /* 最后一行 */

    /* ---- 垂直对齐 ---- */
    uint16_t text_h = line_count * line_h;
    if (cfg->style) {
        if (cfg->style->v_align == ALIGN_CENTER && cfg->h > text_h)
            cur_y += (cfg->h - text_h) / 2;
        else if (cfg->style->v_align == ALIGN_RIGHT_DOWN && cfg->h > text_h)
            cur_y += (cfg->h - text_h);
    }

    /* ---- 渲染趟：逐行独立水平对齐 ---- */
    uint8_t line_idx       = 0;
    uint16_t line_origin_x = cfg->x;
    if (cfg->style) {
        if (cfg->style->h_align == ALIGN_CENTER)
            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count)) / 2;
        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count));
    }
    cur_x    = line_origin_x;
    char_pos = 0;

    while (char_pos < text_len) {
        if (text_buf[char_pos] == '\n') {
            cur_y += line_h;
            line_idx++;
            line_origin_x = cfg->x;
            if (cfg->style) {
                if (cfg->style->h_align == ALIGN_CENTER)
                    line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count)) / 2;
                else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                    line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count));
            }
            cur_x = line_origin_x;
            char_pos++;
            continue;
        }

        if (text_buf[char_pos] >= 0x20 && text_buf[char_pos] <= 0x7F) {
            uint8_t glyph_w = _glyph_width_px(font_size, FONT_ENC_ASCII);

            if (cur_x + glyph_w > cfg->w) {
                if (cfg->style && cfg->style->word_wrap) {
                    cur_y += line_h;
                    line_idx++;
                    line_origin_x = cfg->x;
                    if (cfg->style) {
                        if (cfg->style->h_align == ALIGN_CENTER)
                            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count)) / 2;
                        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count));
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos++;
                    continue;
                }
            }

            uint8_t ch_byte = (uint8_t)text_buf[char_pos];
            uint32_t addr   = _flash_addr(font_size, FONT_ENC_ASCII, cfg->font_type, &ch_byte);
            uint16_t bytes  = _packed_glyph_bytes(font_size, FONT_ENC_ASCII);
            dev_storage_read(s_render_font, addr, font_buf, bytes);
            dev_display_fill(s_render_display, cur_x, cur_y, glyph_w, font_size, COLOR_BLACK);
            dev_display_draw_bitmap(s_render_display, cur_x, cur_y, glyph_w, font_size, font_buf, cfg->color);

            cur_x += glyph_w;
            char_pos++;
            continue;

        } else if (char_pos + 1 < text_len && _is_gbk((uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1])) {
            uint8_t glyph_w = _glyph_width_px(font_size, FONT_ENC_GBK);

            if (cur_x + glyph_w > cfg->w) {
                if (cfg->style && cfg->style->word_wrap) {
                    cur_y += line_h;
                    line_idx++;
                    line_origin_x = cfg->x;
                    if (cfg->style) {
                        if (cfg->style->h_align == ALIGN_CENTER)
                            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count)) / 2;
                        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - _line_width_get(line_widths, line_idx, line_count));
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos += 2;
                    continue;
                }
            }

            uint8_t gbk_ch[2] = {(uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1]};
            uint32_t addr     = _flash_addr(font_size, FONT_ENC_GBK, cfg->font_type, gbk_ch);
            uint16_t bytes    = _packed_glyph_bytes(font_size, FONT_ENC_GBK);
            dev_storage_read(s_render_font, addr, font_buf, bytes);
            dev_display_fill(s_render_display, cur_x, cur_y, glyph_w, font_size, COLOR_BLACK);
            dev_display_draw_bitmap(s_render_display, cur_x, cur_y, glyph_w, font_size, font_buf, cfg->color);

            cur_x += glyph_w;
            char_pos += 2;
            continue;

        } else {
            char_pos++;
        }
    }
}

static inline void _render_bitmap(const render_cfg_t *cfg)
{
    if (!cfg->w || !cfg->h || !cfg->bitmap) return;

    dev_display_draw_bitmap(s_render_display, cfg->x, cfg->y, cfg->w, cfg->h, cfg->bitmap, cfg->color);
}

static inline void _render_fill(const render_cfg_t *cfg)
{
    uint16_t w = cfg->w, h = cfg->h;
    if (!w || !h) {
        w = s_render_display->screen_rows;
        h = s_render_display->screen_cols;
    }
    dev_display_fill(s_render_display, cfg->x, cfg->y, w, h, cfg->color);
}

/* ---- 渲染跳表 ---- */
typedef void (*render_fn_t)(const render_cfg_t *);
static const render_fn_t g_render_fn[] = {
    [RENDER_TEXT]   = _render_text,
    [RENDER_BITMAP] = _render_bitmap,
    [RENDER_FILL]   = _render_fill,
};

/* ---- 公开 API：tagged union 分派 ---- */
void app_render(const render_cfg_t *cfg)
{
    if (!cfg || !s_render_display) return;
    if (cfg->type < sizeof(g_render_fn) / sizeof(g_render_fn[0]) && g_render_fn[cfg->type])
        g_render_fn[cfg->type](cfg);
    dev_display_commit_frame(s_render_display);
}

/* ================================================================
 *  持久化显示 — 将显存以位图格式保存/恢复到存储设备
 *
 *  pixel_map (逐像素颜色) → bitmap (1bit/pixel + 单色)，大幅压缩闪存占用。
 *  恢复阶段 fill(BLACK) + draw_bitmap(color) 重建 pixel_map。
 * ================================================================ */

#define PERSIST_BUF_SIZE (sizeof(render_persist_t) + 4096) /* 容纳 64×64 像素位图 */

void app_render_save(void)
{
    dev_display_t *d = s_render_display;
    if (!d || !s_render_font) return;

    uint16_t rows      = d->screen_rows;
    uint16_t cols      = d->screen_cols;
    uint16_t row_bytes = (rows + 7) / 8;
    uint16_t bm_bytes  = cols * row_bytes;

    static uint8_t buf[PERSIST_BUF_SIZE];
    render_persist_t *r = (render_persist_t *)buf;
    memset(r->bitmap, 0, bm_bytes);

    uint8_t color = COLOR_BLACK;
    for (uint16_t y = 0; y < cols; y++) {
        for (uint16_t x = 0; x < rows; x++) {
            uint8_t px = d->pixel_map[y * rows + x];
            if (px != COLOR_BLACK) {
                r->bitmap[y * row_bytes + x / 8] |= (uint8_t)(0x80 >> (x % 8));
                if (color == COLOR_BLACK) color = px;
            }
        }
    }

    r->magic       = RENDER_PERSIST_MAGIC;
    r->screen_rows = rows;
    r->screen_cols = cols;
    r->color       = color;
    r->crc32       = crc32_calc(r->bitmap, bm_bytes);

    dev_storage_write(s_render_font, s_persist_addr, buf,
                      sizeof(render_persist_t) + bm_bytes);
}

bool app_render_restore(void)
{
    dev_display_t *d = s_render_display;
    if (!d || !s_render_font)
        return false;
    if (!s_render_font->ops)
        return false;

    uint16_t rows      = d->screen_rows;
    uint16_t cols      = d->screen_cols;
    uint16_t row_bytes = (rows + 7) / 8;
    uint16_t bm_bytes  = cols * row_bytes;

    static uint8_t buf[PERSIST_BUF_SIZE];
    if (dev_storage_read(s_render_font, s_persist_addr, buf,
                         sizeof(render_persist_t) + bm_bytes) < 0)
        return false;

    render_persist_t *r = (render_persist_t *)buf;
    if (r->magic != RENDER_PERSIST_MAGIC)
        return false;
    if (r->screen_rows != rows || r->screen_cols != cols)
        return false;
    if (r->crc32 != crc32_calc(r->bitmap, bm_bytes))
        return false;

    dev_display_fill(d, 0, 0, rows, cols, COLOR_BLACK);
    dev_display_draw_bitmap(d, 0, 0, rows, cols, r->bitmap, (display_color_t)r->color);
    return true;
}
