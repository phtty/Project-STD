/**
 * @file    app_ldi_cfg.c
 * @brief   LDI 配置持久化 — W25Qxx 尾部配置区（由配置调度器管理）
 *
 * 记录格式：cfg_record 24B 头 {name[16], version, len, crc32} + 106B 载荷。
 * 归属/地址/组包/去重/擦写都由调度器处理，本模块只关心载荷语义。
 *
 * 协议边界：本模块只操作 LDI 自己的配置，不引用、不读写其他协议的任何存储。
 */

#include <stdio.h>
#include <string.h>

#include "app_ldi_cfg.h"

#include "cfg_record.h"
#include "app_cfg_sched.h"
#include "initcall.h"

/* ================================================================
 *  加载
 * ================================================================ */

static uint8_t s_cfg_id = 0xFF; /* 调度器注册句柄 */
static app_flash_ldi_cfg_info_t s_loaded_cfg;
static bool s_loaded_valid;
static bool s_load_done;

static void _ldi_cfg_load(void)
{
    /* 两个入口都会走到这里：本模块被首次用到时的按需加载，以及调度器的启动
       加载遍。谁先到谁生效，后到的直接返回——避免白读一遍 Flash（24B 头 +
       106B 载荷外加一次 CRC）。两条路径都保留，是为了让 LDI 不依赖加载遍的时机：
       本模块的调用方在 sw_post(4)，而加载遍在 sw_app(3)，二者顺序其实有保证，
       但同层 initcall 的顺序取决于链接顺序，不该被依赖（见 initcall.h）。 */
    if (s_load_done) return;

    /* 注册失败（重名/满员）是本次上电的确定状态，重试没有意义 */
    if (s_cfg_id == 0xFF) {
        s_load_done = true;
        return;
    }

    s_loaded_valid = false;
    memset(&s_loaded_cfg, 0, sizeof(s_loaded_cfg));

    uint16_t      rec_len = 0;
    cfg_rec_sta_t sta     = app_cfg_sched_load(s_cfg_id, (uint8_t *)&s_loaded_cfg,
                                               sizeof(s_loaded_cfg), &rec_len);

    /* **只在"问到了答案"时置位**：IO_ERR 表示这次没读到（器件未识别、调度器
     * 尚未就绪），那不是"配置不存在"。把它一起缓存会让本上电周期内永远返回默认
     * 值且再无重试机会。EMPTY/INVALID 是确定性的判断，可以缓存。 */
    if (sta != CFG_REC_IO_ERR) s_load_done = true;

    if (sta == CFG_REC_OK && rec_len == sizeof(s_loaded_cfg)) s_loaded_valid = true;
}

bool app_flash_ldi_load_config(app_flash_ldi_cfg_info_t *info)
{
    if (!s_load_done) _ldi_cfg_load();

    if (!s_loaded_valid) {
        memset(info, 0, sizeof(app_flash_ldi_cfg_info_t));
        return false;
    }
    memcpy(info, &s_loaded_cfg, sizeof(app_flash_ldi_cfg_info_t));
    return true;
}

/* ================================================================
 *  保存
 * ================================================================ */

int32_t app_flash_ldi_save_config(const app_flash_ldi_cfg_info_t *info)
{
    int32_t sta = app_cfg_sched_save(s_cfg_id, (const uint8_t *)info, sizeof(*info));
    if (sta != 0)
        printf("[ldi_cfg] 保存失败（%u 字节）\n", (unsigned)sizeof(*info));
    return sta;
}

/* ================================================================
 *  调度器自注册（sw_dev：早于 sw_app(3) 的启动加载遍）
 * ================================================================ */

static const cfg_sched_desc_t s_ldi_cfg_desc = {
    .name    = "ldi_cfg",
    .version = APP_FLASH_LDI_VERSION,
    .load    = _ldi_cfg_load,
};

static void _app_flash_ldi_cfg_register(void)
{
    s_cfg_id = app_cfg_sched_register(&s_ldi_cfg_desc);
    if (s_cfg_id == 0xFF)
        printf("[ldi_cfg] 配置所有者注册失败，本次上电配置不生效\n");
}
sw_dev_initcall(_app_flash_ldi_cfg_register);
