/**
 * @file    app_lcsp_slot.c
 * @brief   LCSP 断电保存槽位：整屏点阵存进 W25Qxx 配置区（走配置调度器）
 *
 * **为什么走配置调度器而不是"字库之后划一段裸地址"**：
 *   · 记录头自带 name/版本/长度/CRC32 —— 槽位写坏一个字节会被判为"记录无效"、
 *     命令回 0x01，而不是把一幅花屏当正常图显示出来（现场分不清是上位机发错还是存储坏了）；
 *   · 容量门槛、块位认领、内容相同去重都是现成的（`app_cfg_sched`），
 *     裸地址方案这三样都要自己再写一遍，而写错的后果是**擦掉字库**。
 *
 * 代价是槽位与其它配置共用一个 12 块的尾部区（见 `app_cfg_sched.h`），
 * 每槽一个 4KB 块 —— 一幅整屏图 2805 字节，一块装得下。
 */

#include "app_lcsp_slot.h"

#include <stdio.h>
#include <string.h>

#include "initcall.h"

#include "app_cfg_sched.h"
#include "dev_cfg_record.h"
#include "pl_mem.h"

/** @brief 槽位记录格式版本；载荷布局变了就 +1（旧记录判为无效 → 槽位视为空）*/
#define LCSP_SLOT_VERSION (1U)

static_assert(LCSP_SLOT_PAYLOAD_MAX <= DEV_CFG_RECORD_MAX_IMAGE, "槽位载荷超过配置记录镜像上限：调大 DEV_CFG_RECORD_MAX_IMAGE");

static uint8_t s_slot_cfg_id[LCSP_SLOT_MAX] = {0xFF, 0xFF, 0xFF};

/* 占用位（bit N = 槽位 N+1）。上电后首次查询时扫一遍记录，保存成功时就地置位；
   不每次查询都读 3 遍整幅点阵 —— 0x01 设备信息会被上位机反复轮询。 */
static uint8_t s_occupied_mask;
static bool s_occupied_known;

/** @brief 占用扫描用缓冲（只在本文件用：读整记录才能区分"有记录"与"记录坏"）
 *
 *  `app_cfg_sched_load` 的载荷容量语义是"装不下即 INVALID"，传小缓冲无法区分
 *  "空槽位"与"有图但比缓冲大"，故必须接一整幅。放 CCMRAM，不占 SRAM 常驻区。 */
static uint8_t s_slot_buf[LCSP_SLOT_PAYLOAD_MAX] PL_CCMRAM;

/** @brief 扫一遍槽位占用（IO 失败不闩锁，下次查询再试）*/
static void _lcsp_slot_scan(void)
{
    if (s_occupied_known)
        return;

    uint8_t mask = 0;

    for (uint8_t slot = 1; slot <= LCSP_SLOT_MAX; slot++) {
        uint8_t id = s_slot_cfg_id[slot - 1U];
        if (0xFF == id)
            continue;

        uint16_t len                 = 0;
        dev_cfg_record_state_t state = app_cfg_sched_load(id, s_slot_buf, (uint16_t)sizeof(s_slot_buf), &len);

        if (DEV_CFG_RECORD_STATE_IO_ERR == state)
            return; /* 存储暂不可用：保留"未知"，别把它当成"空"钉死 */

        if ((DEV_CFG_RECORD_STATE_OK == state) && (len > (uint16_t)sizeof(app_lcsp_slot_image_t)))
            mask |= (uint8_t)(1U << (slot - 1U));
    }

    s_occupied_mask  = mask;
    s_occupied_known = true;
}

bool app_lcsp_slot_is_occupied(uint8_t slot)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX))
        return false;

    _lcsp_slot_scan();

    return 0U != (s_occupied_mask & (uint8_t)(1U << (slot - 1U)));
}

bool app_lcsp_slot_any_occupied(void)
{
    _lcsp_slot_scan();

    /* 扫描因 IO 失败未完成时，按"已有图但读不到"回 false：上位机据此不会
       误以为槽位可用（0x06 会自己回 0x01）*/
    return 0U != s_occupied_mask;
}

int32_t app_lcsp_slot_save(uint8_t slot, const app_lcsp_slot_image_t *img, uint16_t bitmap_len)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX) || (nullptr == img) || (0U == bitmap_len))
        return -1;

    uint32_t payload_len = (uint32_t)sizeof(app_lcsp_slot_image_t) + bitmap_len;
    if (payload_len > LCSP_SLOT_PAYLOAD_MAX)
        return -1;

    uint8_t id = s_slot_cfg_id[slot - 1U];
    if (0xFF == id) {
        printf("[lcsp_slot] 槽位 %u 未注册（配置区已满），拒绝保存\n", (unsigned)slot);
        return -1;
    }

    int32_t state = app_cfg_sched_save(id, (const uint8_t *)img, (uint16_t)payload_len);
    if (0 != state) {
        printf("[lcsp_slot] 槽位 %u 保存失败\n", (unsigned)slot);
        return state;
    }

    s_occupied_mask = (uint8_t)(s_occupied_mask | (uint8_t)(1U << (slot - 1U)));
    /* 不置 `s_occupied_known`：若本次上电还没扫过，别的槽位仍可能是"有图但未知"，
       闩成"已知"会让它们被报成空槽（0x01 的 bit1 只反映这一个槽位就错了）。 */
    return 0;
}

bool app_lcsp_slot_load(uint8_t slot, app_lcsp_slot_image_t *img, uint16_t img_cap, uint16_t *bitmap_len)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX) || (nullptr == img) || (nullptr == bitmap_len))
        return false;

    uint8_t id = s_slot_cfg_id[slot - 1U];
    if (0xFF == id)
        return false;

    uint16_t len                 = 0;
    dev_cfg_record_state_t state = app_cfg_sched_load(id, (uint8_t *)img, img_cap, &len);

    if ((DEV_CFG_RECORD_STATE_OK != state) || (len <= (uint16_t)sizeof(app_lcsp_slot_image_t)))
        return false;

    *bitmap_len = (uint16_t)(len - sizeof(app_lcsp_slot_image_t));
    return true;
}

/* ---- 注册（每个槽位一个记录所有者）---- */

static const app_cfg_sched_desc_t s_slot_desc[LCSP_SLOT_MAX] = {
    {.name = "lcsp_img1", .version = LCSP_SLOT_VERSION, .load = nullptr},
    {.name = "lcsp_img2", .version = LCSP_SLOT_VERSION, .load = nullptr},
    {.name = "lcsp_img3", .version = LCSP_SLOT_VERSION, .load = nullptr},
};

static void _lcsp_slot_init(void)
{
    for (uint8_t i = 0; i < LCSP_SLOT_MAX; i++) {
        s_slot_cfg_id[i] = app_cfg_sched_register(&s_slot_desc[i]);
        if (0xFF == s_slot_cfg_id[i])
            printf("[lcsp_slot] 槽位 %u 注册失败（配置区已满），该槽位不可用\n", (unsigned)(i + 1U));
    }
}
sw_dev_initcall(_lcsp_slot_init);
