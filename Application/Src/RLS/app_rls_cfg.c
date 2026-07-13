#include "app_rls_cfg.h"

#include <string.h>

#include "pl_crc.h"
#include "dev_w25qxx.h"
#include "initcall.h"

/* ---- RLS 配置在 W25Qxx 的存储地址（倒数第二个 4KB 扇区，避免与字库冲突） ---- */
static uint32_t s_rls_base;

/* ---- 初始化（sw_dev_initcall: 确保 dev_w25qxx 已初始化） ---- */
static void _dev_flash_rls_storage_init(void)
{
    dev_storage_t *w25 = dev_w25qxx_get();
    s_rls_base         = dev_storage_capacity(w25) - 4096 * 2;
}
sw_dev_initcall(_dev_flash_rls_storage_init);

dev_storage_t *dev_flash_rls_get_storage(void)
{
    return dev_w25qxx_get();
}

/* ================================================================
 *  实现
 * ================================================================ */

/**
 * @brief 判断是否存储点阵图（或 0xFF 默认值）
 */
bool dev_flash_rls_bitmap_is_valid(volatile const dev_flash_rls_record_t *rec)
{
    if (rec->magic != DEV_FLASH_RLS_MAGIC)
        return true;
    return false;
}

/**
 * @brief 保存位图信息至 Flash
 */
void dev_flash_rls_save_config(uint8_t bitmap[512])
{
    static dev_flash_rls_record_t rec = {0};
    rec.magic                         = DEV_FLASH_RLS_MAGIC;
    memcpy(rec.bitmap, bitmap, sizeof(rec.bitmap));

    dev_storage_write(dev_flash_rls_get_storage(), s_rls_base, (uint8_t *)&rec, sizeof(rec));
}

/**
 * @brief 从flash中加载位图
 * @param bitmap  输出参数，有效配置写入此处
 * @return true  读取到有效配置
 * @return false 配置为空/校验失败/读取错误
 */
bool dev_flash_rls_load_config(uint8_t bitmap[512])
{
    dev_flash_rls_record_t rec = {0};
    dev_storage_t *strg        = dev_flash_rls_get_storage();

    if (dev_storage_read(strg, s_rls_base, (uint8_t *)&rec, sizeof(rec)) < 0)
        return false;

    if (!dev_flash_rls_bitmap_is_valid(&rec))
        return false;

    memcpy(bitmap, &rec.bitmap, sizeof(rec.bitmap));
    return true;
}
