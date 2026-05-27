/**
 * @file    dev_flash_font.c
 * @brief   W25Qxx SPI Flash 字库存储设备实现
 */

#include "dev_flash_font.h"
#include "pl_spi.h"
#include "initcall.h"

/* W25Qxx 读命令 */
#define W25Q_READ_CMD    0x03
#define W25Q_DUMMY_CYCLES 0

void dev_font_flash_init(font_flash_dev_t *dev)
{
    dev->spi = pl_spi_get_handle();
}

int32_t dev_font_flash_read(font_flash_dev_t *dev, uint32_t addr, uint8_t *buf, uint16_t len)
{
    /* W25Qxx 读时序：cmd(1B) + addr(3B) + data(N) */
    uint8_t tx[4];
    tx[0] = W25Q_READ_CMD;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)(addr);

    /* 通过 DMA 发起 SPI 传输（简化实现，实际需等待 DMA 完成） */
    return pl_spi_transmit_receive_dma(dev->spi, tx, buf, 4 + len);
}
