#include <string.h>
#include "dev_flash_ldi.h"

#include "pl_crc.h"
#include "dev_flash_int.h"

// dev_flash_ldi_record_t: magic(4) + cfg(106) + padding(2) + crc32(4) = 116 瀛楄妭 = 29 words
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
 * @brief 鏍￠獙 Flash 閰嶇疆鍖哄畬鏁存 (magic + CRC32)
 */
bool dev_flash_ldi_is_config_valid(volatile const dev_flash_ldi_record_t *rec)
{
    if (rec->magic != DEV_FLASH_LDI_MAGIC)
        return false;

    // CRC 瑕疵检查 magic 配置，不包含 crc32 字节（链长 1 字）
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
    return dev_storage_erase(dev_flash_int_get(DEV_FLASH_INT_LDI), 0, 0);
}

/**
 * @brief 将 dev_flash_ldi_record_t 的 word 写入 Flash 扇区 11
 */
int32_t dev_flash_ldi_write_config(dev_flash_ldi_record_t *rec)
{
    return dev_storage_write(dev_flash_int_get(DEV_FLASH_INT_LDI), 0, (uint8_t *)rec, sizeof(*rec));
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
 * @brief 从Flash加载LDI配置信息，用于错误或验证失败后的回退
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
