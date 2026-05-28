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
#define W25Q_READ_CMD       0x03
#define W25Q_RESET_ENABLE   0x66
#define W25Q_RESET_DEVICE   0x99
#define W25Q_READ_JEDEC_ID  0x9F
#define W25Q_WRITE_ENABLE   0x06
#define W25Q_PAGE_PROGRAM   0x02
#define W25Q_SECTOR_ERASE   0x20
#define W25Q_CHIP_ERASE     0xC7
#define W25Q_READ_STATUS1   0x05

static void _cs_low(void)  { pl_gpio_write(PL_PORT_B, 1, false); }
static void _cs_high(void) { pl_gpio_write(PL_PORT_B, 1, true);  }

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

/* ---- 写使能 ---- */
int32_t dev_font_flash_write_enable(font_flash_dev_t *dev)
{
    uint8_t cmd = W25Q_WRITE_ENABLE;
    _cs_low();
    int32_t r = pl_spi_transmit(dev->spi, &cmd, 1);
    _cs_high();
    return r;
}

/* ---- 等待忙结束 ---- */
int32_t dev_font_flash_wait_busy(font_flash_dev_t *dev, uint32_t timeout_ms)
{
    uint8_t status;
    for (uint32_t t = 0; t < timeout_ms; t++) {
        _cs_low();
        uint8_t cmd = W25Q_READ_STATUS1;
        pl_spi_transmit(dev->spi, &cmd, 1);
        pl_spi_receive(dev->spi, &status, 1);
        _cs_high();
        if (!(status & 0x01)) return 0; /* BUSY 清除 */
        osDelay(1);
    }
    return -1; /* 超时 */
}

/* ---- 页写入（≤256 字节，不可跨页） ---- */
int32_t dev_font_flash_write_page(font_flash_dev_t *dev, uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (!dev || !buf || len == 0 || len > 256) return -1;

    dev_font_flash_write_enable(dev);

    uint8_t cmd[4] = { W25Q_PAGE_PROGRAM,
                       (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    _cs_low();
    if (pl_spi_transmit(dev->spi, cmd, 4) != 0) { _cs_high(); return -1; }
    int32_t r = pl_spi_transmit(dev->spi, buf, len);
    _cs_high();

    return (r == 0) ? dev_font_flash_wait_busy(dev, 100) : -1;
}

/* ---- 扇区擦除（4KB） ---- */
int32_t dev_font_flash_erase_sector(font_flash_dev_t *dev, uint32_t addr)
{
    dev_font_flash_write_enable(dev);

    uint8_t cmd[4] = { W25Q_SECTOR_ERASE,
                       (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    _cs_low();
    int32_t r = pl_spi_transmit(dev->spi, cmd, 4);
    _cs_high();

    return (r == 0) ? dev_font_flash_wait_busy(dev, 3000) : -1;
}

/* ---- 全片擦除 ---- */
int32_t dev_font_flash_erase_chip(font_flash_dev_t *dev)
{
    dev_font_flash_write_enable(dev);

    uint8_t cmd = W25Q_CHIP_ERASE;
    _cs_low();
    int32_t r = pl_spi_transmit(dev->spi, &cmd, 1);
    _cs_high();

    return (r == 0) ? dev_font_flash_wait_busy(dev, 250000) : -1;
}

/* ---- 跨页写入（不检查擦除，内部使用） ---- */
static int32_t _write_no_check(font_flash_dev_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t written = 0;

    while (written < len) {
        uint16_t page_remain = 256 - (addr % 256);
        uint16_t chunk       = (len - written <= page_remain) ? (uint16_t)(len - written) : page_remain;

        if (dev_font_flash_write_page(dev, addr, buf + written, chunk) != 0)
            return -1;

        written += chunk;
        addr    += chunk;
    }
    return 0;
}

/* ---- 任意长度写入（自动处理扇区擦除和页边界） ---- */
int32_t dev_font_flash_write(font_flash_dev_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (!dev || !buf || len == 0) return -1;

    static uint8_t sector_buf[4096];
    uint32_t sector_size = 4096;
    uint32_t written     = 0;

    while (written < len) {
        uint32_t sector_addr   = addr - (addr % sector_size);
        uint16_t sector_offset = addr % sector_size;
        uint32_t chunk         = sector_size - sector_offset;
        if (len - written < chunk) chunk = len - written;

        /* 读扇区 → 检查是否需要擦除 → 擦除 → 写回 */
        dev_font_flash_read(dev, sector_addr, sector_buf, sector_size);

        bool need_erase = false;
        for (uint16_t i = sector_offset; i < sector_offset + chunk; i++)
            if (sector_buf[i] != 0xFF) { need_erase = true; break; }

        if (need_erase) {
            dev_font_flash_erase_sector(dev, sector_addr);
            /* 擦除后扇区全为 0xFF，修改 chunk 部分再写回 */
            memset(sector_buf, 0xFF, sector_size);
        }

        memcpy(sector_buf + sector_offset, buf + written, chunk);
        if (_write_no_check(dev, sector_addr, sector_buf, sector_size) != 0)
            return -1;

        written += chunk;
        addr    += chunk;
    }
    return 0;
}
