#include "render.h"
#include "cmsis_os2.h"
#include "w25qxx.h"

typedef uint32_t (*getFontLibraryAddr)(uint32_t, const uint8_t *, FontType_t, uint16_t);
extern osThreadId_t Display_TaskHandle;

uint32_t getFontLibraryAddr_ASCII(uint32_t offset, const uint8_t *p_text, FontType_t type, uint16_t bytes_per_char);
uint32_t getFontLibraryAddr_GBK(uint32_t offset, const uint8_t *p_text, FontType_t type, uint16_t bytes_per_char);

// 不同大小字体的分发跳转
static getFontLibraryAddr calcu_addr[] = {
    getFontLibraryAddr_ASCII,
    getFontLibraryAddr_GBK,
};

// 字体大小对应表
static uint8_t Font_Height_Table[] = {
    font14,
    font16,
    font20,
    font24,
    font32,
};

// 字号决定基地址偏移
static const uint32_t size_table[] = {
    ASCII_14_OFFSET,
    GBK_14_OFFSET,
    ASCII_16_OFFSET,
    GBK_16_OFFSET,
    ASCII_20_OFFSET,
    GBK_20_OFFSET,
    ASCII_24_OFFSET,
    GBK_24_OFFSET,
    ASCII_32_OFFSET,
    GBK_32_OFFSET,
};

// 字型决定步进值
static const uint32_t type_table[] = {
    ASCII_14_STEP,
    GBK_14_STEP,
    ASCII_16_STEP,
    GBK_16_STEP,
    ASCII_20_STEP,
    GBK_20_STEP,
    ASCII_24_STEP,
    GBK_24_STEP,
    ASCII_32_STEP,
    GBK_32_STEP,
};

static void getFontLibraryBuff(const uint8_t *p_text, uint8_t *font_buff, FontType_t font_type, FontSize_t font_size, bool mode);

/**
 * @brief 全屏填充
 *
 * @param color 要填充的颜色
 */
void Disp_Fill(DispColor_t color)
{
    for (uint32_t k = 0; k < DISRAM_SIZE; k++)
    { // 清空原始点阵区
        pixel_map[k] = color;
    }
    osThreadFlagsSet(Display_TaskHandle, 0x01);
}

// 检查是否为GBK编码
static inline bool IsGBK(uint8_t high, uint8_t low)
{
    // 检查高位：0x81 - 0xFE
    if (high >= 0x81 && high <= 0xFE)
    {
        // 检查低位：0x40 - 0xFE，且排除 0x7F
        if (low >= 0x40 && low <= 0xFE && low != 0x7F)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 在屏幕上渲染字符串（支持中英文混排及换行）
 *
 * @param start_x 起始横坐标
 * @param start_y 起始纵坐标
 * @param p_text 字符串指针
 * @param text_len 字符串长度
 * @param color 字体颜色
 * @param font_size 字号选择
 * @param font_type 字型选择
 */
void RenderString(uint32_t start_x, uint32_t start_y, const uint8_t *p_text, uint32_t text_len, DispColor_t color, FontSize_t font_size, FontType_t font_type)
{
    uint16_t cur_x = start_x;
    uint16_t cur_y = start_y;

    // 边界检查
    if (font_size < font_14)
        font_size = font_14;
    else if (font_size > font_32)
        font_size = font_32;

    // 清屏
    Disp_Fill(black);

    uint16_t i = 0;
    while (i < text_len)
    {
        // "_" 和 "\n" 换行
        if (i + 1 < text_len)
        {
            if ((p_text[i] == '\n') || (p_text[i] == '_'))
            {
                cur_x = 0;
                cur_y += Font_Height_Table[font_size];
                i += 1;
                continue;
            }
        }

        if (p_text[i] >= 0x20 && p_text[i] <= 0x7F)
        { // 判断是否为ASCII
            RenderChar(&p_text[i], &cur_x, &cur_y, font_size, font_type, false, true, color);
            i += 1;
        }
        else if (IsGBK(p_text[i], p_text[i + 1]))
        { // 判断是否为GBK
            RenderChar(&p_text[i], &cur_x, &cur_y, font_size, font_type, true, true, color);
            i += 2;
        }
        else
        {
            i++; // 容错处理
        }
    }

    // 标记画面已更新，等待底层扫描刷新
    osThreadFlagsSet(Display_TaskHandle, 0x01);
}

#define CHAR_HEIGHT (Font_Height_Table[font_size])
#define CHAR_WEIGHT (Font_Height_Table[font_size] / (2 - mode))
/**
 * @brief 渲染单个字符（支持自动换行）
 *
 * @param p_text 字符指针
 * @param x 横坐标指针
 * @param y 纵坐标指针
 * @param font_size 字号选择
 * @param font_type 字型选择
 * @param mode ASCII码/GBK码选择
 * @param line_break 是否自动换行
 * @param color 字符颜色
 */
void RenderChar(const uint8_t *p_text, uint16_t *x, uint16_t *y, FontSize_t font_size, FontType_t font_type, bool mode, bool line_break, DispColor_t color)
{
    static uint8_t font_buff[512] = {0};
    getFontLibraryBuff(p_text, font_buff, font_type, font_size, mode);

    // 若是显示GBK文字，则先判断此行剩余空间是否足够
    if ((*x > SCREEN_PIXEL_ROW - CHAR_WEIGHT) && line_break && mode)
    {
        *y += CHAR_HEIGHT;
        *x = 0;
    }

    for (uint8_t i = 0; i < CHAR_HEIGHT; i++)
    { // 行遍历
        for (uint8_t j = 0; j < CHAR_WEIGHT; j++)
        { // 列遍历
            if (((font_buff[i * ((CHAR_WEIGHT + 7) / 8) + j / 8] << j % 8) & 0x80) == 0)
                pixel_map[SCREEN_PIXEL_ROW * (i + *y) + *x + j] = black;
            else
                pixel_map[SCREEN_PIXEL_ROW * (i + *y) + *x + j] = color;
        }
    }

    *x += CHAR_WEIGHT;

    if ((*x > SCREEN_PIXEL_ROW - CHAR_HEIGHT / 2) && line_break)
    { // 全屏显示的时候需要换行，单行显示的时候不用
        *y += CHAR_HEIGHT;
        *x = 0;
    }
}

/**
 * @brief 读取字库芯片中对应字号和字型的点阵位图
 *
 * @param p_text 字符指针
 * @param font_buff 位图缓冲区指针
 * @param font_type 字型选择
 * @param font_size 字号选择
 * @param mode ASCII码/GBK码选择
 */
void getFontLibraryBuff(const uint8_t *p_text, uint8_t *font_buff, FontType_t font_type, FontSize_t font_size, bool mode)
{
    // 计算需要从字库芯片中读取的字节数
    uint16_t bytes_per_char = CHAR_HEIGHT * ((CHAR_WEIGHT + 7) / 8);

    // 计算点阵位图在字库中的地址
    uint32_t base_offset = size_table[font_size * 2 + mode] + type_table[font_size * 2 + mode] * font_type;

    // 目标地址 = 该字库分区的基地址 + (字符在字库中的序号 × 每个字符占用的字节数)
    uint32_t buff_addr = calcu_addr[mode](base_offset, p_text, font_type, bytes_per_char);

    W25Q256_Read(buff_addr, font_buff, bytes_per_char);
}

uint32_t getFontLibraryAddr_ASCII(uint32_t offset, const uint8_t *p_text, FontType_t type, uint16_t bytes_per_char)
{
    uint32_t buff_addr = 0;

    buff_addr = offset + (p_text[0] - 0x20) * bytes_per_char;

    return buff_addr;
}

uint32_t getFontLibraryAddr_GBK(uint32_t offset, const uint8_t *p_text, FontType_t type, uint16_t bytes_per_char)
{
    uint32_t buff_addr = 0, index = 0;

    if (p_text[1] < 0x7f)
        index = (uint32_t)(p_text[0] - 0x81) * 190 + (p_text[1] - 0x40);
    else
        index = (uint32_t)(p_text[0] - 0x81) * 190 + (p_text[1] - 0x41);

    buff_addr = offset + index * bytes_per_char;

    return buff_addr;
}

uint16_t max_display_len = 0;
/**
 * @brief 字体自适应,有单行截断
 *
 * @param len 字符串长度
 * @param font_size 设置的字号码
 * @return uint8_t 处理完成后的字号大小
 */
uint8_t auto_font_size(uint16_t len, uint8_t font_size)
{
    max_display_len = SCREEN_PIXEL_ROW / ((font_size + 1) * 4);

    if (!font_size)
    { // 若字号为0，则进行自适应
        for (int16_t i = 1; i <= 3; i++)
        { // 遍历所有字号在当前屏幕大小下的最大字符长度
            max_display_len = SCREEN_PIXEL_ROW / ((i + 1) * 4);

            if (max_display_len >= len)
            {
                font_size++;
            }
            else
            {
                max_display_len = SCREEN_PIXEL_ROW / (i * 4);
                break;
            }
        }

        if (!font_size)
        { // 若最小字号都会溢出，则使用最小字号
            font_size = font_16;
            max_display_len = SCREEN_PIXEL_ROW / ((font_16 + 1) * 4);
        }
    }

    return font_size;
}

/**
 * @brief 自适应行数
 *
 * @param line 设定的行数
 * @param font_size 字体大小
 * @return uint8_t y轴起始值
 */
uint16_t auto_line(uint8_t line, uint8_t font_size)
{
    uint16_t y;

    if (line)
    { // 非自适应则通过行数计算y轴的坐标
        y = (font_size + 1) * 8 * line;
    }
    else
    {
        y = (SCREEN_PIXEL_COL - (font_size + 1) * 8) / 2;
    }

    return y;
}

/**
 * @brief 设置对齐模式
 *
 * @param len 字符串长度
 * @param font_size 字体大小
 * @param align 对其模式
 * @return uint16_t x轴起始值
 */
uint16_t set_align(uint16_t len, uint8_t font_size, uint8_t align)
{
    uint16_t x;

    switch (align)
    {
    case 1: // 居中对齐
        x = (SCREEN_PIXEL_ROW - ((font_size + 1) * 4 * (len))) / 2;
        break;

    case 2: // 左对齐
        x = 0;
        break;

    case 3: // 右对齐
        x = SCREEN_PIXEL_ROW - ((font_size + 1) * 4 * len);
        break;

    default:
        x = 0;
        break;
    }

    return x;
}
