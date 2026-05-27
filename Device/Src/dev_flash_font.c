/**
 * @file    dev_flash_font.c
 * @brief   W25Qxx SPI Flash 字库存储设备实现
 */

#include "dev_flash_font.h"

#include <stdint.h>
#include <string.h>
#include "initcall.h"
#include "cmsis_os2.h"
#include "pl_spi.h"
#include "pl_gpio.h"

font_flash_dev_t g_font_flash;

static void dev_font_flash_initcall(void)
{
    dev_font_flash_init(&g_font_flash);
}
sw_device_initcall(dev_font_flash_initcall);

/* W25Qxx 命令 */
#define W25Q_READ_CMD      0x03
#define W25Q_RESET_ENABLE  0x66
#define W25Q_RESET_DEVICE  0x99
#define W25Q_READ_JEDEC_ID 0x9F

static osEventFlagsId_t s_dma_done;
static volatile bool s_dma_ok;

static void dma_cplt_cb(void *ctx)
{
    (void)ctx;
    s_dma_ok = true;
    osEventFlagsSet(s_dma_done, 0x01);
}

void dev_font_flash_init(font_flash_dev_t *dev)
{
    dev->spi = pl_spi_get_handle();

    s_dma_done = osEventFlagsNew(NULL);
    pl_spi_set_rx_cplt_cb(dev->spi, dma_cplt_cb, NULL);

    /* 复位设备 */
    uint8_t tx[2];
    tx[0] = W25Q_RESET_ENABLE;
    tx[1] = W25Q_RESET_DEVICE;
    pl_gpio_write(PL_PORT_B, 1, false); /* CS low */
    pl_spi_transmit_receive_dma(dev->spi, tx, NULL, 2);
    osEventFlagsWait(s_dma_done, 0x01, osFlagsWaitAny, 100);
    pl_gpio_write(PL_PORT_B, 1, true); /* CS high */
    osDelay(10);                       /* wait for reset */

    /* 读 JEDEC ID */
    uint8_t id_tx[4] = {W25Q_READ_JEDEC_ID, 0, 0, 0};
    uint8_t id_rx[4] = {0};
    pl_gpio_write(PL_PORT_B, 1, false);
    s_dma_ok = false;
    pl_spi_transmit_receive_dma(dev->spi, id_tx, id_rx, 4);
    osEventFlagsWait(s_dma_done, 0x01, osFlagsWaitAny, 100);
    pl_gpio_write(PL_PORT_B, 1, true);

    /* W25Q256: JEDEC ID = 0xEF4019, capacity = 32MB */
    dev->capacity = 32 * 1024 * 1024;
    (void)id_rx;
}

int32_t dev_font_flash_read(font_flash_dev_t *dev, uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (!dev || !buf || len == 0) return -1;

    uint16_t total           = 4 + len; /* cmd(1) + addr(3) + data(len) */
    uint8_t tx_buf[4 + 2048] = {0};     /* max page read */
    uint8_t rx_buf[4 + 2048] = {0};

    uint16_t size = (total > sizeof(tx_buf)) ? sizeof(tx_buf) : total;

    tx_buf[0] = W25Q_READ_CMD;
    tx_buf[1] = (uint8_t)(addr >> 16);
    tx_buf[2] = (uint8_t)(addr >> 8);
    tx_buf[3] = (uint8_t)(addr);
    for (uint16_t i = 4; i < size; i++) tx_buf[i] = 0xFF;

    s_dma_ok = false;
    pl_gpio_write(PL_PORT_B, 1, false); /* CS low */
    pl_spi_transmit_receive_dma(dev->spi, tx_buf, rx_buf, size);
    osEventFlagsWait(s_dma_done, 0x01, osFlagsWaitAny, 100);
    pl_gpio_write(PL_PORT_B, 1, true); /* CS high */

    /* 跳过前4字节（cmd+addr期间的MISO垃圾数据） */
    uint16_t copy_len = (size - 4 < len) ? (size - 4) : len;
    memcpy(buf, rx_buf + 4, copy_len);

    return (int32_t)copy_len;
}
