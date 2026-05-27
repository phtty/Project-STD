#include "dev_flash_ldi.h"

#include "pl_crc.h"
#include "pl_flash.h"

// dev_flash_ldi_record_t: magic(4) + cfg(106) + padding(2) + crc32(4) = 116 字节 = 29 words
#define FLASH_WORD_COUNT ((sizeof(dev_flash_ldi_record_t) + 3) / 4)

/* ================================================================
 *  实现
 * ================================================================ */

/**
 * @brief 判断 Flash 配置区是否为空 (全 0xFF 即从未写入过)
 */
bool dev_flash_ldi_is_config_empty(volatile const dev_flash_ldi_record_t *rec)
{
    if (rec->magic == 0xFFFFFFFF && rec->crc32 == 0xFFFFFFFF)
        return true;
    return false;
}

/**
 * @brief 校验 Flash 配置区完整性 (magic + CRC32)
 */
bool dev_flash_ldi_is_config_valid(volatile const dev_flash_ldi_record_t *rec)
{
    if (rec->magic != DEV_FLASH_LDI_MAGIC)
        return false;

    // CRC 覆盖 magic + cfg, 不含 crc32 自身 (最后 1 word)
    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)rec, (FLASH_WORD_COUNT - 1) * 4);
    if (rec->crc32 != crc)
        return false;

    return true;
}

/**
 * @brief 擦除 Sector 11 (0x080E0000, 128KB)
 */
int32_t dev_flash_ldi_erase_config(void)
{
    pl_flash_unlock();
    pl_flash_clear_errors();
    int32_t ret = pl_flash_erase_sector(PL_FLASH_SECTOR_11, PL_FLASH_VOLTAGE_3);
    pl_flash_lock();
    return ret;
}

/**
 * @brief 将 dev_flash_ldi_record_t 按 word 写入 Flash Sector 11
 */
int32_t dev_flash_ldi_write_config(dev_flash_ldi_record_t *rec)
{
    pl_flash_unlock();
    pl_flash_clear_errors();

    uint32_t *p = (uint32_t *)rec;
    int32_t ret = 0;
    for (uint32_t i = 0; i < FLASH_WORD_COUNT; i++) {
        ret = pl_flash_program_word(ADDR_LDI_CONFIG_SECTOR + i * 4, p[i]);
        if (ret != 0) break;
    }

    pl_flash_lock();
    return ret;
}

/**
 * @brief 保存 LDI 配置信息到 Flash (先擦除再写入)
 */
void dev_flash_ldi_save_config(dev_flash_ldi_cfg_info_t *info)
{
    dev_flash_ldi_record_t rec = {0};
    rec.magic = DEV_FLASH_LDI_MAGIC;
    memcpy(&rec.cfg, info, sizeof(dev_flash_ldi_cfg_info_t));
    rec.crc32 = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&rec, (FLASH_WORD_COUNT - 1) * 4);

    dev_flash_ldi_erase_config();
    dev_flash_ldi_write_config(&rec);
}

/**
 * @brief 从 Flash 加载 LDI 配置信息, 若为空或校验失败则返回全零
 */
void dev_flash_ldi_load_config(dev_flash_ldi_cfg_info_t *info)
{
    volatile dev_flash_ldi_record_t *rec = (volatile dev_flash_ldi_record_t *)ADDR_LDI_CONFIG_SECTOR;

    if (dev_flash_ldi_is_config_empty(rec) || !dev_flash_ldi_is_config_valid(rec)) {
        memset(info, 0, sizeof(dev_flash_ldi_cfg_info_t));
        return;
    }

    memcpy(info, (void *)&rec->cfg, sizeof(dev_flash_ldi_cfg_info_t));
}
