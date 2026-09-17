/**
 * @file    cfg_record.c
 * @brief   配置记录通用服务实现（见 cfg_record.h）
 */

#include "cfg_record.h"

#include <string.h>
#include "crc_utils.h"

/* ---- 去重读回对比缓冲 (仅 save 路径使用) ---- */
static uint8_t s_cmp_buf[CFG_RECORD_MAX_IMAGE];

/* name 填充: 调用方名字补 NUL 到定长, 与头内字段全量比对 */
static void _pad_name(char out[CFG_RECORD_NAME_MAX], const char *name)
{
    memset(out, 0, CFG_RECORD_NAME_MAX);
    if (name)
        strncpy(out, name, CFG_RECORD_NAME_MAX - 1);
}

/* ---- 加载 ---- */
cfg_rec_sta_t cfg_record_load(dev_storage_t *stor, uint32_t addr,
                              const char *name, uint16_t version, cfg_record_crc_fn crc,
                              uint8_t *payload, uint16_t payload_cap, uint16_t *payload_len)
{
    cfg_record_hdr_t hdr;
    char want[CFG_RECORD_NAME_MAX];

    if (!stor) return CFG_REC_IO_ERR;

    if (dev_storage_read(stor, addr, (uint8_t *)&hdr, sizeof(hdr)) != 0)
        return CFG_REC_IO_ERR;

    /* 空判断: name 首字节 0xFF 表示从未写入 */
    if ((uint8_t)hdr.name[0] == 0xFFU)
        return CFG_REC_EMPTY;

    /* 归属 + 版本 + 长度逐项校验 */
    _pad_name(want, name);
    if (memcmp(hdr.name, want, CFG_RECORD_NAME_MAX) != 0)
        return CFG_REC_INVALID;
    if (hdr.version != version)
        return CFG_REC_INVALID;
    if (hdr.len == 0U || hdr.len > payload_cap)
        return CFG_REC_INVALID;

    if (dev_storage_read(stor, addr + CFG_RECORD_HDR_SIZE, payload, hdr.len) != 0)
        return CFG_REC_IO_ERR;

    /* payload CRC 校验 */
    cfg_record_crc_fn crc_fn = crc ? crc : crc32_calc;
    if (crc_fn(payload, hdr.len) != hdr.crc32)
        return CFG_REC_INVALID;

    if (payload_len) *payload_len = hdr.len;
    return CFG_REC_OK;
}

/* ---- 保存 ---- */
int32_t cfg_record_save(dev_storage_t *stor, uint32_t addr,
                        const char *name, uint16_t version, cfg_record_crc_fn crc,
                        const uint8_t *payload, uint16_t payload_len,
                        uint8_t *scratch, uint16_t scratch_cap)
{
    cfg_record_hdr_t *hdr = (cfg_record_hdr_t *)scratch;
    uint16_t total;

    if (!stor || !payload || !scratch)
        return -1;

    /* 先在 uint32 里算再窄化。若先窄化："24 + payload_len" 在
       payload_len ∈ [65512, 65535] 时溢出 uint16（65536 → 0），容量校验会放行，
       随后下面那句 memcpy 把 64KB 写进 scratch。本函数是公开 API，payload_len
       由调用方给 —— 将来谁把协议帧里的 16 位长度域直接传进来就会踩中。 */
    uint32_t total32 = CFG_RECORD_HDR_SIZE + (uint32_t)payload_len;
    if ((uint32_t)scratch_cap < total32)
        return -1;
    total = (uint16_t)total32;

    cfg_record_crc_fn crc_fn = crc ? crc : crc32_calc;

    /* 组包: 头 + payload */
    _pad_name(hdr->name, name);
    hdr->version = version;
    hdr->len     = payload_len;
    hdr->crc32   = crc_fn(payload, payload_len);
    memcpy(scratch + CFG_RECORD_HDR_SIZE, payload, payload_len);

    /* 去重: 读回现有内容, 完全一致则跳过写 (NOR 擦写磨损保护) */
    if (total <= CFG_RECORD_MAX_IMAGE && dev_storage_read(stor, addr, s_cmp_buf, total) == 0 && memcmp(s_cmp_buf, scratch, total) == 0) {
        return 0;
    }

    return dev_storage_write(stor, addr, scratch, total);
}
