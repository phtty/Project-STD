#include "initcall.h"
/**
 * @file    dev_flash_int.c
 * @brief   STM32 内部 Flash 存储设备 — 实现 dev_storage_ops
 *
 * 一个模块，两个实例：Sector1 (IAP) 和 Sector11 (LDI)，共享同一套 ops。
 */

#include "dev_flash_int.h"
#include "pl_flash.h"
#include <string.h>

#define IAP_BASE   0x08004000
#define IAP_SIZE   0x4000    /* 16KB */
#define IAP_SECTOR PL_FLASH_SECTOR_1

#define LDI_BASE   0x080E0000
#define LDI_SIZE   0x20000   /* 128KB */
#define LDI_SECTOR PL_FLASH_SECTOR_11

/* ---- 两个实例 ---- */
static dev_flash_int_t g_flash_iap = { .dev = {.capacity = IAP_SIZE}, .base_addr = IAP_BASE, .sector = PL_FLASH_SECTOR_1  };
static dev_flash_int_t g_flash_ldi = { .dev = {.capacity = LDI_SIZE}, .base_addr = LDI_BASE, .sector = PL_FLASH_SECTOR_11 };

dev_storage_t *dev_flash_int_get(uint8_t id)
{
    return (id == 0) ? &g_flash_iap.dev : &g_flash_ldi.dev;
}

/* ---- OPS ---- */
static int32_t _init(dev_storage_t *dev)
{
    /* 内部 Flash 无需额外初始化，pl_flash_init 已在 hw_initcall 中执行 */
    (void)dev;
    return 0;
}

static int32_t _read(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    dev_flash_int_t *self = (dev_flash_int_t *)dev;
    memcpy(buf, (void *)(self->base_addr + addr), len);
    return (int32_t)len;
}

static int32_t _write(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    dev_flash_int_t *self = (dev_flash_int_t *)dev;
    uint32_t abs_addr = self->base_addr + addr;
    uint32_t word_cnt = (len + 3) / 4;
    uint32_t tmp[word_cnt];

    memset(tmp, 0xFF, word_cnt * 4);
    memcpy(tmp, buf, len);

    pl_flash_unlock();
    pl_flash_clear_errors();
    for (uint32_t i = 0; i < word_cnt; i++)
        pl_flash_program_word(abs_addr + i * 4, tmp[i]);
    pl_flash_lock();
    return (int32_t)len;
}

static int32_t _erase(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    (void)addr; (void)len;
    dev_flash_int_t *self = (dev_flash_int_t *)dev;

    pl_flash_unlock();
    pl_flash_clear_errors();
    int32_t r = pl_flash_erase_sector(self->sector, PL_FLASH_VOLTAGE_3);
    pl_flash_lock();
    return r;
}

static uint32_t _capacity(dev_storage_t *dev) { return dev->capacity; }

static const dev_storage_ops_t flash_int_ops = {
    .init     = _init,
    .read     = _read,
    .write    = _write,
    .erase    = _erase,
    .capacity = _capacity,
};

/* ---- 初始化（hw_initcall 设置 ops） ---- */
static void _dev_flash_int_init(void)
{
    g_flash_iap.dev.ops = &flash_int_ops;
    g_flash_ldi.dev.ops = &flash_int_ops;
}
hw_device_initcall(_dev_flash_int_init);
