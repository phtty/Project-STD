#include "display.h"
#include "tim.h"

osThreadId_t RefreshTaskHandle;
const osThreadAttr_t RefreshTask_attributes = {
    .name       = "RefreshTask",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityAboveNormal,
};

__attribute__((section(".ccmram"))) uint8_t pixel_map[DISRAM_SIZE]  = {0};
__attribute__((section(".ccmram"))) uint8_t hub75_buff[DISRAM_SIZE] = {0};

const ChannelStruct_TypeDef channel_red[] = {
    {HUB75_R1_GPIO_Port, HUB75_R1_Pin},
    {HUB75_R2_GPIO_Port, HUB75_R2_Pin},
    {HUB75_R3_GPIO_Port, HUB75_R3_Pin},
    {HUB75_R4_GPIO_Port, HUB75_R4_Pin},
    {HUB75_R5_GPIO_Port, HUB75_R5_Pin},
    {HUB75_R6_GPIO_Port, HUB75_R6_Pin},
    {HUB75_R7_GPIO_Port, HUB75_R7_Pin},
    {HUB75_R8_GPIO_Port, HUB75_R8_Pin},
    {HUB75_R9_GPIO_Port, HUB75_R9_Pin},
    {HUB75_R10_GPIO_Port, HUB75_R10_Pin},
};

const ChannelStruct_TypeDef channel_green[] = {
    {HUB75_G1_GPIO_Port, HUB75_G1_Pin},
    {HUB75_G2_GPIO_Port, HUB75_G2_Pin},
    {HUB75_G3_GPIO_Port, HUB75_G3_Pin},
    {HUB75_G4_GPIO_Port, HUB75_G4_Pin},
    {HUB75_G5_GPIO_Port, HUB75_G5_Pin},
    {HUB75_G6_GPIO_Port, HUB75_G6_Pin},
    {HUB75_G7_GPIO_Port, HUB75_G7_Pin},
    {HUB75_G8_GPIO_Port, HUB75_G8_Pin},
    {HUB75_G9_GPIO_Port, HUB75_G9_Pin},
    {HUB75_G10_GPIO_Port, HUB75_G10_Pin},
};

const ChannelStruct_TypeDef channel_blue[] = {
    {HUB75_B1_GPIO_Port, HUB75_B1_Pin},
    {HUB75_B2_GPIO_Port, HUB75_B2_Pin},
    {HUB75_B3_GPIO_Port, HUB75_B3_Pin},
    {HUB75_B4_GPIO_Port, HUB75_B4_Pin},
    {HUB75_B5_GPIO_Port, HUB75_B5_Pin},
    {HUB75_B6_GPIO_Port, HUB75_B6_Pin},
    {HUB75_B7_GPIO_Port, HUB75_B7_Pin},
    {HUB75_B8_GPIO_Port, HUB75_B8_Pin},
    {HUB75_B9_GPIO_Port, HUB75_B9_Pin},
    {HUB75_B10_GPIO_Port, HUB75_B10_Pin},
};

__STATIC_INLINE void handle_black(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin << 0x10;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin << 0x10;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin << 0x10;
}

__STATIC_INLINE void handle_red(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin << 0x10;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin << 0x10;
}

__STATIC_INLINE void handle_green(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin << 0x10;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin << 0x10;
}

__STATIC_INLINE void handle_blue(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin << 0x10;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin << 0x10;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin;
}

__STATIC_INLINE void handle_yellow(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin << 0x10;
}

__STATIC_INLINE void handle_purple(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin << 0x10;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin;
}

__STATIC_INLINE void handle_cyan(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin << 0x10;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin;
}

__STATIC_INLINE void handle_white(int16_t channel_cnt)
{
    channel_red[channel_cnt].port->BSRR   = channel_red[channel_cnt].pin;
    channel_green[channel_cnt].port->BSRR = channel_green[channel_cnt].pin;
    channel_blue[channel_cnt].port->BSRR  = channel_blue[channel_cnt].pin;
}

// 跳转表
ColorHandler color_handlers[] = {
    handle_black,  // default
    handle_red,    // case red
    handle_green,  // case green
    handle_yellow, // case yellow
    handle_blue,   // case blue
    handle_purple, // case purple
    handle_cyan,   // case cyan
    handle_white,  // case white
};

void convert_pixelmap(void)
{
    uint16_t row_cnt = 0, col_cnt = 0, group_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        row_cnt = map_cnt / SCREEN_PIXEL_ROW; // 屏幕的行标
        col_cnt = map_cnt % SCREEN_PIXEL_ROW;

        group_cnt = (row_cnt / 16 * 8 + row_cnt % 8) * MODULE_PER_ROW + col_cnt / 32;

        if (row_cnt % 16 / 8) // 下半行
            hub75_buff[group_cnt * GROUP_SIZE + col_cnt % 32] = pixel_map[map_cnt];
        else // 上半行
            hub75_buff[group_cnt * GROUP_SIZE + col_cnt % 32 + 32] = pixel_map[map_cnt];
    }
}

#define LINE_OFFSET    (scan_line * SCAN_LINE_PIXEL_NUM) // 像素点在扫描行的偏移
#define CHANNEL_OFFSET (channel_cnt * CHANNEL_PIXEL_NUM) // 像素点在通道的偏移
/**
 * @brief 动态扫描行切换
 *
 * @param line_cnt 行计算
 */
static void scan_channel(uint8_t line_cnt)
{
    if (0 == line_cnt)
        HUB75_B = 1;
    HUB75_A = 1;

    for (uint8_t i = 0; i < 2; i++)
        __NOP();

    HUB75_A = 0;
    if (0 == line_cnt)
        HUB75_B = 0;
}

/**
 * @brief 发送显存数据到HUB75接口
 *
 */
void send_hub75_buff(void)
{
    static uint8_t scan_line = 0;

    for (int16_t line_cnt = 0; line_cnt < SCAN_LINE_PIXEL_NUM; line_cnt++) {
        for (int16_t channel_cnt = 0; channel_cnt < CHANNEL_NUM; channel_cnt++) {
            // 取出第channel_cnt通道中，第scan_line行的第line_cnt个像素点的颜色数据
            DispColor_t color_index = (DispColor_t)hub75_buff[line_cnt + LINE_OFFSET + CHANNEL_OFFSET];
            // 通过跳转表执行对应颜色通道引脚的电平变化
            color_handlers[color_index](channel_cnt);
        }

        // CLK给1个脉冲，LED驱动芯片移位寄存器移位
        HUB75_CLK = 1;
        __NOP();
        __NOP();
        HUB75_CLK = 0;
    }

    // 所有通道的第scan_line扫描行数据发送完毕，控制LED驱动芯片将数据放入输出锁存器
    // 在多行扫描中为原子操作，需将OE保持除能
    NVIC_DisableIRQ(TIM4_IRQn);
    HUB75_OE = 1;
    scan_channel(scan_line); // 扫描行切换

    // LE信号给一个周期，除能闩锁器一个脉冲的时间，使数据从移位寄存器进入输出锁存器
    HUB75_LAT = 1;
    for (uint8_t i = 0; i < 2; i++)
        __NOP();
    HUB75_LAT = 0;

    NVIC_EnableIRQ(TIM4_IRQn);

    // 行扫描计数自增
    scan_line += 1;
    if (scan_line >= 8)
        scan_line = 0;
}

/**
 * @brief 模组点顺序测试，用于单个模组的像素点规律寻找
 *
 * @param color 颜色
 * @param num_of_point 点数
 * @param channel 通道数
 */
void point_order_test(DispColor_t color, int32_t num_of_point, uint8_t channel)
{
    color_handlers[color](channel);

    for (int32_t i = 0; i < num_of_point; i++) {
        HUB75_CLK = 0;
        __NOP();
        __NOP();
        HUB75_CLK = 1;
        __NOP();
        __NOP();
    }

    HAL_NVIC_DisableIRQ(TIM4_IRQn);
    HUB75_OE = 1;

    HUB75_LAT = 1;
    __NOP();
    __NOP();
    HUB75_LAT = 0;
    __NOP();
    __NOP();
    HUB75_OE = 0;
    HAL_NVIC_EnableIRQ(TIM4_IRQn);
}

uint8_t light_level = 1; // 亮度等级
/**
 * @brief 软件pwm调光
 *
 */
void pwm_light_handle(void)
{
    static uint8_t pwm_cnt = 0; // pwm计数

    if (pwm_cnt < light_level) // light_level的取值范围：0~7,0为关闭显示
        HUB75_OE = 0;
    else
        HUB75_OE = 1;

    pwm_cnt++;
    if (pwm_cnt >= 8)
        pwm_cnt = 0;
}

void RefreshTask(void *argument)
{
    uint32_t refresh_flag = 0;

    __HAL_DBGMCU_FREEZE_TIM3();
    __HAL_DBGMCU_FREEZE_TIM4();

    light_level = 1;
    bsp_init_hub75();
    HAL_TIM_Base_Start_IT(&htim3);
    HAL_TIM_Base_Start_IT(&htim4);

    for (;;) {
        refresh_flag = osThreadFlagsWait(0x01, osFlagsWaitAny, osWaitForever);
        if (refresh_flag)
            convert_pixelmap();
    }
}
