/**
 * @file    dev_w25qxx.c
 * @brief   W25Qxx SPI Flash 存储设备 — 实现 dev_storage_ops
 */

#include "dev_w25qxx.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_spi.h"
#include "pl_gpio.h"
#include "pl_sys.h"

/* W25Qxx 命令 */
#define W25Q_READ_CMD      0x03
#define W25Q_RESET_ENABLE  0x66
#define W25Q_RESET_DEVICE  0x99
#define W25Q_READ_JEDEC_ID 0x9F
#define W25Q_WRITE_ENABLE  0x06
#define W25Q_PAGE_PROGRAM  0x02
#define W25Q_SECTOR_ERASE  0x20
#define W25Q_CHIP_ERASE    0xC7
#define W25Q_ENTER_4BYTE   0xB7
#define W25Q_READ_STATUS1  0x05

typedef struct {
    dev_storage_t me;
    pl_spi_handle_t spi;
    uint16_t device_id; /* JEDEC ID: Memory Type << 8 | Capacity */
    uint16_t page_size;
    uint32_t sector_size;
} dev_w25qxx_t;

/* ---- 全局实例 ---- */
static dev_w25qxx_t g_w25qxx = {.page_size = 256, .sector_size = 4096};

/* JEDEC ID → 容量 */
static uint32_t _jedec_capacity(uint16_t id)
{
    switch (id & 0xFF) { /* 只用容量字节判断 */
        case 0x15:
            return 2 * 1024 * 1024; /* W25Q16   */
        case 0x16:
            return 4 * 1024 * 1024; /* W25Q32   */
        case 0x17:
            return 8 * 1024 * 1024; /* W25Q64   */
        case 0x18:
            return 16 * 1024 * 1024; /* W25Q128  */
        default:
            return 32 * 1024 * 1024; /* W25Q256+ */
    }
}

/* 容量字节 >= 0x19 → >128Mb → 需 4 字节地址 */
static inline uint8_t _addr_len(dev_w25qxx_t *s)
{
    return (s->device_id & 0xFF) >= 0x19 ? 4 : 3;
}

/* 写地址到 cmd buffer（cmd_cnt 模式），返回写入的字节数 */
static inline uint8_t _put_addr(uint8_t *cmd, uint32_t addr, dev_w25qxx_t *s)
{
    uint8_t n = 0;
    if (_addr_len(s) == 4) cmd[n++] = (uint8_t)(addr >> 24);
    cmd[n++] = (uint8_t)(addr >> 16);
    cmd[n++] = (uint8_t)(addr >> 8);
    cmd[n++] = (uint8_t)addr;
    return n;
}

dev_storage_t *dev_w25qxx_get(void)
{
    return &g_w25qxx.me;
}

/* ---- CS 控制 ---- */
static inline void _cs_low(void)
{
    pl_gpio_write(PL_PORT_B, 1, false);
}
static inline void _cs_high(void)
{
    pl_gpio_write(PL_PORT_B, 1, true);
}

/* ---- DMA 同步（_read 懒初始化）---- */
static osEventFlagsId_t s_evt;
static volatile bool s_ok;

static void _dma_cb(void *ctx)
{
    (void)ctx;
    s_ok = true;
    osEventFlagsSet(s_evt, 0x01);
}

/* ---- OPS 实现 ---- */
static int32_t _init(dev_storage_t *dev)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    self->spi          = pl_spi_get_handle();

    /* 复位（阻塞，无需RTOS） */
    uint8_t rst[2] = {W25Q_RESET_ENABLE, W25Q_RESET_DEVICE};
    _cs_low();
    pl_spi_transmit(self->spi, rst, 2);
    _cs_high();
    pl_delay_ms(10);

    /* JEDEC ID → 容量（阻塞全双工，无需RTOS） */
    uint8_t tx[4] = {W25Q_READ_JEDEC_ID, 0xFF, 0xFF, 0xFF}, rx[4] = {0};
    _cs_low();
    pl_spi_transmit_receive(self->spi, tx, rx, 4);
    _cs_high();

    self->device_id = (uint16_t)(rx[2] << 8) | rx[3]; /* Type|Capacity */
    dev->capacity   = _jedec_capacity(self->device_id);

    /* >128Mb → 4 字节地址模式 */
    if (_addr_len(self) == 4) {
        uint8_t c4 = W25Q_ENTER_4BYTE;
        _cs_low();
        pl_spi_transmit(self->spi, &c4, 1);
        _cs_high();
    }
    return 0;
}

static int32_t _read(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    if (!buf || len == 0) return -1;

    if (!s_evt) {
        s_evt = osEventFlagsNew(NULL);
        if (!s_evt) return -1;
        pl_spi_set_rx_cplt_cb(self->spi, _dma_cb, NULL);
    }

    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_READ_CMD;
    _put_addr(cmd + 1, addr, self);

    s_ok = false;
    _cs_low();
    pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1));
    pl_spi_receive_dma(self->spi, buf, (uint16_t)len);
    while (!s_ok)
        osDelay(1);
    _cs_high();
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
    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_PAGE_PROGRAM;
    _put_addr(cmd + 1, addr, self);
    _cs_low();
    if (pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1)) != 0) {
        _cs_high();
        return -1;
    }
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
        w += ch;
        addr += ch;
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
        uint32_t ch  = 4096 - off;
        if (len - w < ch) ch = len - w;

        _read(dev, sa, sec, 4096);

        bool need = false;
        for (uint16_t i = off; i < off + ch; i++)
            if (sec[i] != 0xFF) {
                need = true;
                break;
            }

        if (need) {
            _write_enable(self);
            uint8_t eal = _addr_len(self);
            uint8_t ec[5];
            ec[0] = W25Q_SECTOR_ERASE;
            _put_addr(ec + 1, sa, self);
            _cs_low();
            pl_spi_transmit(self->spi, ec, (uint16_t)(eal + 1));
            _cs_high();
            _wait_busy(self, 3000);
            memset(sec, 0xFF, 4096);
        }

        memcpy(sec + off, buf + w, ch);
        if (_write_no_check(self, sa, sec, 4096) != 0) return -1;
        w += ch;
        addr += ch;
    }
    return 0;
}

static int32_t _erase(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    (void)len;
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;

    _write_enable(self);
    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_SECTOR_ERASE;
    _put_addr(cmd + 1, addr, self);
    _cs_low();
    int32_t r = pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1));
    _cs_high();
    return (r == 0) ? _wait_busy(self, 3000) : -1;
}

static uint32_t _capacity(dev_storage_t *dev)
{
    return dev->capacity;
}

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
    g_w25qxx.me.ops = &w25qxx_ops;
    _init(&g_w25qxx.me);
}
hw_dev_initcall(dev_w25qxx_init);
