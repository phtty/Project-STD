/**
 * @file    dev_flash_int.c
 * @brief   STM32 内部 Flash 存储 — g_flash_int_ops 虚表实现（不含实例）
 *
 * 实例和 ops 绑定由各自的配置模块负责：
 *   dev_flash_iap.c → g_flash_iap → hw_dev_initcall
 *   dev_flash_ldi.c → g_flash_ldi → hw_dev_initcall
 */

#include "dev_flash_int.h"

#include <string.h>
#include "container_of.h"
#include "pl_flash.h"

/* ---- OPS ---- */
static int32_t _init(dev_storage_t *dev)
{
    /* 内部 Flash 无需额外初始化，pl_flash_init 已在 hw_initcall 中执行 */
    (void)dev;
    return 0;
}

static int32_t _read(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    dev_flash_int_t *self = container_of(dev, dev_flash_int_t, base);
    /* base_addr 是"地址整数"：经 uintptr_t 转指针，既表达意图，也让 64 位宿主机测试
     * 不再报 -Wint-to-pointer-cast（目标机 32 位下与原先完全等价，无 codegen 变化） */
    memcpy(buf, (void *)(uintptr_t)(self->base_addr + addr), len);
    return 0; /* 约定: 0 = 成功（不返回字节数） */
}

static int32_t _write(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    dev_flash_int_t *self = container_of(dev, dev_flash_int_t, base);
    uint32_t abs_addr     = self->base_addr + addr;
    uint32_t word_cnt     = (len + 3) / 4;
    uint32_t tmp[word_cnt];
    int32_t r = 0;

    memset(tmp, 0xFF, word_cnt * 4);
    memcpy(tmp, buf, len);

    pl_flash_unlock();
    pl_flash_clear_errors();
    for (uint32_t i = 0; i < word_cnt; i++)
        if (pl_flash_program_word(abs_addr + i * 4, tmp[i]) != 0)
            r = -1; /* 编程失败必须上报：调用方据此判定配置未落盘 */
    pl_flash_lock();
    return r;
}

static int32_t _erase(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
    dev_flash_int_t *self = container_of(dev, dev_flash_int_t, base);

    pl_flash_unlock();
    pl_flash_clear_errors();
    int32_t r = pl_flash_erase_sector(self->sector, PL_FLASH_VOLTAGE_3);
    pl_flash_lock();
    return r;
}

static uint32_t _capacity(dev_storage_t *dev)
{ return dev->capacity; }

const dev_storage_ops_t g_flash_int_ops = {
    .init     = _init,
    .read     = _read,
    .write    = _write,
    .erase    = _erase,
    .capacity = _capacity,
};
