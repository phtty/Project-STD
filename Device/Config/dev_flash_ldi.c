#include <string.h>
#include "dev_flash_ldi.h"

#include "pl_crc.h"
#include "dev_w25qxx.h"
#include "initcall.h"

/* ---- LDI 配置在 W25Qxx 的存储地址（最后一个 4KB 扇区，避免与字库冲突） ---- */
static uint32_t s_ldi_base;

/* ---- 初始化（sw_dev_initcall: 确保 dev_w25qxx 已初始化） ---- */
static void _dev_flash_ldi_storage_init(void)
{
    dev_storage_t *w25 = dev_w25qxx_get();
    s_ldi_base         = dev_storage_capacity(w25) - 4096;
}
sw_dev_initcall(_dev_flash_ldi_storage_init);

dev_storage_t *dev_flash_ldi_get_storage(void)
{
    return dev_w25qxx_get();
}

// dev_flash_ldi_record_t: magic(4) + cfg(106) + padding(2) + crc32(4) = 116 byte = 29 words
#define FLASH_WORD_COUNT ((sizeof(dev_flash_ldi_record_t) + 3) / 4)

/* ================================================================
 *  实现
 * ================================================================ */

/**
 * @brief 判断 Flash 配置项是否为空（或 0xFF 默认值）
 */
bool dev_flash_ldi_is_config_empty(volatile const dev_flash_ldi_record_t *rec)
{
    if (rec->magic == 0xFFFFFFFF && rec->crc32 == 0xFFFFFFFF)
        return true;
    return false;
}

/**
 * @brief 验证 Flash 配置校验值 (magic + CRC32)
 */
bool dev_flash_ldi_is_config_valid(volatile const dev_flash_ldi_record_t *rec)
{
    if (rec->magic != DEV_FLASH_LDI_MAGIC)
        return false;

    /* CRC 校验覆盖 magic + 配置，不包含 crc32 自身 */
    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)rec, (FLASH_WORD_COUNT - 1) * 4);
    if (rec->crc32 != crc)
        return false;

    return true;
}

/**
 * @brief 擦除 W25Qxx 中 LDI 配置所在扇区（最后一个 4KB 扇区）
 */
int32_t dev_flash_ldi_erase_config(void)
{
    return dev_storage_erase(dev_flash_ldi_get_storage(), s_ldi_base, 4096);
}

/**
 * @brief 将 dev_flash_ldi_record_t 写入 W25Qxx
 */
int32_t dev_flash_ldi_write_config(dev_flash_ldi_record_t *rec)
{
    return dev_storage_write(dev_flash_ldi_get_storage(), s_ldi_base, (uint8_t *)rec, sizeof(*rec));
}

/**
 * @brief 保存 LDI 配置信息至 Flash（用于出厂时预置）
 */
void dev_flash_ldi_save_config(dev_flash_ldi_cfg_info_t *info)
{
    dev_flash_ldi_record_t rec = {0};
    rec.magic                  = DEV_FLASH_LDI_MAGIC;
    memcpy(&rec.cfg, info, sizeof(dev_flash_ldi_cfg_info_t));
    rec.crc32 = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&rec, (FLASH_WORD_COUNT - 1) * 4);

    dev_flash_ldi_erase_config();
    dev_flash_ldi_write_config(&rec);
}

/**
 * @brief 从 W25Qxx 加载 LDI 配置信息
 * @param info  输出参数，有效配置写入此处
 * @return true  读取到有效配置
 * @return false 配置为空/校验失败/读取错误
 */
bool dev_flash_ldi_load_config(dev_flash_ldi_cfg_info_t *info)
{
    dev_flash_ldi_record_t rec;
    dev_storage_t *w25 = dev_flash_ldi_get_storage();

    if (dev_storage_read(w25, s_ldi_base, (uint8_t *)&rec, sizeof(rec)) < 0) {
        memset(info, 0, sizeof(dev_flash_ldi_cfg_info_t));
        return false;
    }

    if (dev_flash_ldi_is_config_empty(&rec) || !dev_flash_ldi_is_config_valid(&rec)) {
        memset(info, 0, sizeof(dev_flash_ldi_cfg_info_t));
        return false;
    }

    memcpy(info, &rec.cfg, sizeof(dev_flash_ldi_cfg_info_t));
    return true;
}
