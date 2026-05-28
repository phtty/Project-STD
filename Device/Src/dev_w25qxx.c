/**
 * @file    dev_w25qxx.c
 * @brief   W25Qxx SPI Flash 存储设备 — 实现 dev_storage_ops
 */

#include "dev_w25qxx.h"
#include "pl_spi.h"
#include "pl_gpio.h"
#include "initcall.h"
#include "cmsis_os2.h"
#include <string.h>

/* W25Qxx 命令 */
#define W25Q_READ_CMD      0x03
#define W25Q_RESET_ENABLE  0x66
#define W25Q_RESET_DEVICE  0x99
#define W25Q_READ_JEDEC_ID 0x9F
#define W25Q_WRITE_ENABLE  0x06
#define W25Q_PAGE_PROGRAM  0x02
#define W25Q_SECTOR_ERASE  0x20
#define W25Q_CHIP_ERASE    0xC7
#define W25Q_READ_STATUS1  0x05

/* ---- 全局实例 ---- */
static dev_w25qxx_t g_w25qxx;

dev_storage_t *dev_w25qxx_get(void) { return &g_w25qxx.dev; }

/* ---- CS 控制 ---- */
static void _cs_low(void)  { pl_gpio_write(PL_PORT_B, 1, false); }
static void _cs_high(void) { pl_gpio_write(PL_PORT_B, 1, true);  }

/* ---- DMA 同步 ---- */
static osEventFlagsId_t s_evt;
static volatile bool s_ok;

static void _dma_cb(void *ctx) { (void)ctx; s_ok = true; osEventFlagsSet(s_evt, 0x01); }

/* ---- OPS 实现 ---- */
static int32_t _init(dev_storage_t *dev)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    self->spi = pl_spi_get_handle();

    s_evt = osEventFlagsNew(NULL);
    pl_spi_set_rx_cplt_cb(self->spi, _dma_cb, NULL);

    /* 复位 */
    uint8_t rst[2] = {W25Q_RESET_ENABLE, W25Q_RESET_DEVICE};
    _cs_low();
    pl_spi_transmit_receive_dma(self->spi, rst, NULL, 2);
    osEventFlagsWait(s_evt, 0x01, osFlagsWaitAny, 100);
    _cs_high(); osDelay(10);

    /* JEDEC ID → 容量 */
    uint8_t id_tx[4] = {W25Q_READ_JEDEC_ID, 0, 0, 0}, id_rx[4] = {0};
    _cs_low(); s_ok = false;
    pl_spi_transmit_receive_dma(self->spi, id_tx, id_rx, 4);
    osEventFlagsWait(s_evt, 0x01, osFlagsWaitAny, 100);
    _cs_high();

    dev->capacity = 32 * 1024 * 1024; /* W25Q256 */
    (void)id_rx;
    return 0;
}

static int32_t _read(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    if (!buf || len == 0 || len > 2048) return -1;

    uint8_t tx[4 + 2048] = {0}, rx[4 + 2048] = {0};
    uint16_t total = 4 + (uint16_t)len;

    tx[0] = W25Q_READ_CMD;
    tx[1] = (uint8_t)(addr >> 16); tx[2] = (uint8_t)(addr >> 8); tx[3] = (uint8_t)addr;

    s_ok = false;
    _cs_low();
    pl_spi_transmit_receive_dma(self->spi, tx, rx, total);
    osEventFlagsWait(s_evt, 0x01, osFlagsWaitAny, 100);
    _cs_high();

    memcpy(buf, rx + 4, len);
    return (int32_t)len;
}

static int32_t _write_enable(dev_w25qxx_t *self)
{
    uint8_t cmd = W25Q_WRITE_ENABLE;
    _cs_low();
    int32_t r = pl_spi_transmit(self->spi, &cmd, 1);
    _cs_high();
    return r;
}

static int32_t _wait_busy(dev_w25qxx_t *self, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t++) {
        _cs_low();
        uint8_t cmd = W25Q_READ_STATUS1, st;
        pl_spi_transmit(self->spi, &cmd, 1);
        pl_spi_receive(self->spi, &st, 1);
        _cs_high();
        if (!(st & 0x01)) return 0;
        osDelay(1);
    }
    return -1;
}

static int32_t _write_page(dev_w25qxx_t *self, uint32_t addr, const uint8_t *buf, uint16_t len)
{
    _write_enable(self);
    uint8_t cmd[4] = {W25Q_PAGE_PROGRAM, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    _cs_low();
    if (pl_spi_transmit(self->spi, cmd, 4) != 0) { _cs_high(); return -1; }
    int32_t r = pl_spi_transmit(self->spi, buf, len);
    _cs_high();
    return (r == 0) ? _wait_busy(self, 100) : -1;
}

static int32_t _write_no_check(dev_w25qxx_t *self, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t w = 0;
    while (w < len) {
        uint16_t pr = 256 - (addr % 256);
        uint16_t ch = (len - w <= pr) ? (uint16_t)(len - w) : pr;
        if (_write_page(self, addr, buf + w, ch) != 0) return -1;
        w += ch; addr += ch;
    }
    return 0;
}

static int32_t _write(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    if (!buf || len == 0) return -1;

    static uint8_t sec[4096];
    uint32_t w = 0;

    while (w < len) {
        uint32_t sa  = addr - (addr % 4096);
        uint16_t off = addr % 4096;
        uint32_t ch  = 4096 - off; if (len - w < ch) ch = len - w;

        _read(dev, sa, sec, 4096);

        bool need = false;
        for (uint16_t i = off; i < off + ch; i++)
            if (sec[i] != 0xFF) { need = true; break; }

        if (need) {
            _write_enable(self);
            uint8_t ec[4] = {W25Q_SECTOR_ERASE, (uint8_t)(sa >> 16), (uint8_t)(sa >> 8), (uint8_t)sa};
            _cs_low(); pl_spi_transmit(self->spi, ec, 4); _cs_high();
            _wait_busy(self, 3000);
            memset(sec, 0xFF, 4096);
        }

        memcpy(sec + off, buf + w, ch);
        if (_write_no_check(self, sa, sec, 4096) != 0) return -1;
        w += ch; addr += ch;
    }
    return 0;
}

static int32_t _erase(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    (void)len;
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;

    _write_enable(self);
    uint8_t cmd[4] = {W25Q_SECTOR_ERASE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    _cs_low();
    int32_t r = pl_spi_transmit(self->spi, cmd, 4);
    _cs_high();
    return (r == 0) ? _wait_busy(self, 3000) : -1;
}

static uint32_t _capacity(dev_storage_t *dev) { return dev->capacity; }

static const dev_storage_ops_t w25qxx_ops = {
    .init     = _init,
    .read     = _read,
    .write    = _write,
    .erase    = _erase,
    .capacity = _capacity,
};

/* ---- 自动初始化 ---- */
void dev_w25qxx_init(void)
{
    g_w25qxx.dev.ops = &w25qxx_ops;
    _init(&g_w25qxx.dev);
}
static void _dev_w25qxx_initcall(void) { dev_w25qxx_init(); }
sw_device_initcall(_dev_w25qxx_initcall);
