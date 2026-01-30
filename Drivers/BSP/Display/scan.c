#include "display.h"
#include "hub75.h"

/**
 * @brief P15的扫描
 *
 */
void convert_pixelmap_p15(void)
{
    uint16_t ModuleGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        row_cnt     = map_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt     = map_cnt % SCREEN_PIXEL_ROW;
        ModuleGroup = col_cnt / 4 + (row_cnt / 4 * (MODULE_PER_ROW * 4)); // 组标

        switch (row_cnt % 4) {
            case 0:
                hub75_buff[2 * (3 - col_cnt % 4) + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
                break;

            case 1:
                hub75_buff[2 * (3 - col_cnt % 4) + 1 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
                break;

            case 2:
                hub75_buff[2 * (col_cnt % 4) + 9 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
                break;

            case 3:
                hub75_buff[2 * (col_cnt % 4) + 8 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
                break;

            default:
                break;
        }
    }
}

/**
 * @brief 新P5模组扫描
 *
 */
void convert_pixelmap_p5(void)
{
    uint16_t ModuleGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;

    for (uint16_t src_cnt = 0; src_cnt < DISRAM_SIZE; src_cnt++) {
        row_cnt     = src_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt     = src_cnt % SCREEN_PIXEL_ROW;
        ModuleGroup = row_cnt / 16 * 8 + row_cnt % 8; // 组标

        if (1 == src_cnt / SCREEN_PIXEL_ROW / 8 % 2)
            hub75_buff[col_cnt * 2 + ModuleGroup * GROUP_SIZE] = pixel_map[src_cnt];
        else
            hub75_buff[col_cnt * 2 + 1 + ModuleGroup * GROUP_SIZE] = pixel_map[src_cnt];
    }
}

/**
 * @brief P6模组的扫描
 *
 */
void convert_pixelmap_p6(void)
{
    uint16_t ModuleGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        row_cnt     = map_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt     = map_cnt % SCREEN_PIXEL_ROW;
        ModuleGroup = row_cnt / 16 * 8 + row_cnt % 8; // 组标

        // 在单行扫描内判断是上半行还是下半行
        if ((row_cnt % 16) >= 8) // 上半行
            hub75_buff[col_cnt + (col_cnt / MODULE_PIXEL_ROW * MODULE_PIXEL_ROW) + (ModuleGroup * GROUP_SIZE)] = pixel_map[map_cnt];
        else // 下半行
            hub75_buff[col_cnt + ((col_cnt / MODULE_PIXEL_ROW + 1) * MODULE_PIXEL_ROW) + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
    }
}

/**
 * @brief 强力的P6模组的扫描
 *
 */
void convert_pixelmap_p6old(void)
{
    uint16_t row_cnt = 0, col_cnt = 0, group_cnt = 0;

    for (uint16_t src_cnt = 0; src_cnt < DISRAM_SIZE; src_cnt++) {
        row_cnt = src_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt = src_cnt % SCREEN_PIXEL_ROW;

        group_cnt = (row_cnt / 16 * 8 + row_cnt % 8) * MODULE_PER_ROW + col_cnt / 32;

        if (row_cnt % 16 / 8) // 下半行
            hub75_buff[group_cnt * GROUP_SIZE + col_cnt % 32] = pixel_map[src_cnt];
        else // 上半行
            hub75_buff[group_cnt * GROUP_SIZE + col_cnt % 32 + 32] = pixel_map[src_cnt];
    }
}

/**
 * @brief P10模组的扫描
 *
 */
void convert_pixelmap_p10(void)
{
    uint16_t ModuleGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        // 点阵缓存的行标和列标计算
        row_cnt = map_cnt / SCREEN_PIXEL_ROW;
        col_cnt = map_cnt % SCREEN_PIXEL_ROW;
        // 点阵缓存的组标计算
        ModuleGroup = (row_cnt % 4 + row_cnt / 8 * 4) * (SCAN_LINE_PIXEL_NUM / GROUP_SIZE) + col_cnt / 8;

        // 在单行扫描内判断是上半行还是下半行
        if ((row_cnt % 8) >= 4) // 下半行的点逆序排列
            hub75_buff[15 - col_cnt % 8 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
        else // 上半行的点顺序排列
            hub75_buff[col_cnt % 8 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
    }
}

#define CHANNEL_NUM       (MODULE_PER_ROW * MOUDLE_CHANNEL_NUM)                                       // 总通道数
#define CHANNEL_PIXEL_NUM (MODULE_PIXEL_ROW * MODULE_PIXEL_COL * MODULE_PER_COL / MOUDLE_CHANNEL_NUM) // 单个通道的像素个数
const ChannelStruct_TypeDef channel_red[10] = {
    {R2_GPIO_Port, R2_Pin},
    {R1_GPIO_Port, R1_Pin},
    {R4_GPIO_Port, R4_Pin},
    {R3_GPIO_Port, R3_Pin},
    {R6_GPIO_Port, R6_Pin},
    {R5_GPIO_Port, R5_Pin},
    {R8_GPIO_Port, R8_Pin},
    {R7_GPIO_Port, R7_Pin},
    {R10_GPIO_Port, R10_Pin},
    {R9_GPIO_Port, R9_Pin},
};
const ChannelStruct_TypeDef channel_green[10] = {
    {G2_GPIO_Port, G2_Pin},
    {G1_GPIO_Port, G1_Pin},
    {G4_GPIO_Port, G4_Pin},
    {G3_GPIO_Port, G3_Pin},
    {G6_GPIO_Port, G6_Pin},
    {G5_GPIO_Port, G5_Pin},
    {G8_GPIO_Port, G8_Pin},
    {G7_GPIO_Port, G7_Pin},
    {G10_GPIO_Port, G10_Pin},
    {G9_GPIO_Port, G9_Pin},
};
const ChannelStruct_TypeDef channel_blue[10] = {
    {B2_GPIO_Port, B2_Pin},
    {B1_GPIO_Port, B1_Pin},
    {B4_GPIO_Port, B4_Pin},
    {B3_GPIO_Port, B3_Pin},
    {B6_GPIO_Port, B6_Pin},
    {B5_GPIO_Port, B5_Pin},
    {B8_GPIO_Port, B8_Pin},
    {B7_GPIO_Port, B7_Pin},
    {B10_GPIO_Port, B10_Pin},
    {B9_GPIO_Port, B9_Pin},
};
/**
 * @brief p5垂直排列扫描
 *
 */
void convert_pixelmap_p5v(void)
{
    uint16_t row_cnt = 0, col_cnt = 0, buff_cnt = 0;

    for (uint16_t src_cnt = 0; src_cnt < DISRAM_SIZE; src_cnt++) {
        row_cnt = src_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt = src_cnt % SCREEN_PIXEL_ROW;

        buff_cnt             = ((col_cnt / 16 + 1) * 8 - 1 - col_cnt % 8) * SCAN_LINE_PIXEL_NUM + row_cnt * 2 + col_cnt / 8 % 2;
        hub75_buff[buff_cnt] = pixel_map[src_cnt];
    }
}

/**
 * @brief 更新点阵缓冲区到显存
 *
 */
void convert_pixelmap(void)
{
    uint16_t ModuleGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        // 点阵缓存的行标和列标计算
        row_cnt = map_cnt / SCREEN_PIXEL_ROW;
        col_cnt = map_cnt % SCREEN_PIXEL_ROW;
        // 点阵缓存的组标计算
        // ModuleGroup = (row_cnt % 4 + row_cnt / 8 * 4) * (SCAN_LINE_PIXEL_NUM / GROUP_SIZE) + col_cnt / 8;
        ModuleGroup = col_cnt / 32 * HUB75_PIXEL_NUM + row_cnt / 8 % 2 * HUB75_PIXEL_NUM / 2 + (row_cnt % 4 * 4 + row_cnt / 16 * 16) + col_cnt % 32 / 8;

        // 在单行扫描内判断是上半行还是下半行
        if ((row_cnt % 8) >= 4) // 下半行的点顺序排列
            hub75_buff[col_cnt % 8 + 8 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
        else // 上半行的点顺序排列
            hub75_buff[col_cnt % 8 + ModuleGroup * GROUP_SIZE] = pixel_map[map_cnt];
    }
}

/**
 * @brief P20_2R1G模组扫描
 *
 */
void convert_pixelmap_p20(void)
{
    uint16_t ModuelGroup = 0;
    uint8_t row_cnt = 0, col_cnt = 0;
    static uint8_t group_offset[] = {5, 4, 1, 0};

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        row_cnt = map_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt = map_cnt % SCREEN_PIXEL_ROW;

        // 计算组标
        ModuelGroup = group_offset[row_cnt % 4] + row_cnt / 4 * CHANNEL_PIXEL_NUM / 8 + col_cnt / 16 * 8 + col_cnt / 8 % 2 * 2;

        hub75_buff[col_cnt % 8 + ModuelGroup * GROUP_SIZE] = pixel_map[map_cnt];
    }
}
