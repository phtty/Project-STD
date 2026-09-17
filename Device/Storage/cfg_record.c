/**
 * @file    cfg_record.c
 * @brief   配置记录通用服务实现（见 cfg_record.h）
 */

#include "cfg_record.h"

#include <string.h>
#include "crc_utils.h"

/** 去重回读的比较块大小。
 *
 * 这里用固定小缓冲分块比对，而不是"整条读回再 memcmp"——后者要按
 * CFG_RECORD_MAX_IMAGE 静态分配一个 2.6KB 的缓冲，而本函数只需要"是否一致"
 * 这个判断，逐块比即可，一旦不同立刻返回。
 *
 * 代价是多几次 read。但 save 路径本来就要擦扇区（几十到几百毫秒），
 * 多几十次几十微秒的读可以忽略；而且内容一致时反而省掉了擦写（去重的意义所在）。
 *
 * 副作用：本文件不再有文件级可变状态，cfg_record_save / cfg_record_load
 * 因此**都是可重入的**。 */
#define CFG_RECORD_CMP_CHUNK (64U)

/** @brief 比对器件上的记录与待写镜像
 *  @return 0 = 完全一致，1 = 不一致，-1 = 读失败（按"不一致"处理，走写路径） */
static int32_t _same_as_stored(dev_storage_t *stor, uint32_t addr, const uint8_t *image,
                               uint16_t total)
{
    uint8_t chunk[CFG_RECORD_CMP_CHUNK];

    for (uint16_t off = 0; off < total;) {
        uint16_t n = (uint16_t)(total - off);
        if (n > sizeof(chunk)) n = sizeof(chunk);

        if (dev_storage_read(stor, addr + off, chunk, n) != 0) return -1;
        if (memcmp(chunk, image + off, n) != 0) return 1;

        off = (uint16_t)(off + n);
    }
    return 0;
}

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

    /* 去重: 内容完全一致则跳过写 (NOR 擦写磨损保护) */
    if (_same_as_stored(stor, addr, scratch, total) == 0) return 0;

    return dev_storage_write(stor, addr, scratch, total);
}
