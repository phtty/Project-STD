/**
 * @file    app_test_board.c
 * @brief   std_a 特有的硬件测试用例
 *
 * 车道灯（PD14）与黄闪灯（PD15）只有 std_a 有，所以这个用例归板级，
 * 不放共享的 Application/Src/app_test.c —— 否则共享文件要认识某一块板的外设。
 * 声明在本板的 Inc/board.h 里。
 */

#include "board.h"
#include "dev_io_ctrl.h"
#include "cmsis_os2.h"

void board_test_io_output(void)
{
    dev_io_lane_light(true);
    osDelay(1000);
    dev_io_lane_light(false);

    dev_io_flash_light(true);
    osDelay(1000);
    dev_io_flash_light(false);
}
