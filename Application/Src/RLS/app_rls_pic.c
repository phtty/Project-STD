/**
 * @file    app_rls_pic.c
 * @brief   RLS 图片缓存实现 — 3×512B 缓存槽 + W25Qxx 持久化 + SW1/2/3 干接点触发
 */

#include "app_rls_pic.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"

#include "dev_key.h"
#include "dev_w25qxx.h"
#include "dev_storage.h"
#include "dev_display.h"
#include "app_render.h"
#include "pl_crc.h"

/* RAM 缓存即 Flash 记录镜像（1547B，.bss）——必须留在 SRAM：W25Qxx 读走 SPI DMA，不可达 CCMRAM */
static rls_pic_record_t s_rls_pic_rec;
static bool s_rls_pic_slot_valid[RLS_PIC_SLOT_COUNT];

/* 保护记录 + 串行化 RLS 内部所有 W25Qxx 写（dev_w25qxx 的扇区暂存 sec[4096] 为 static 共享） */
static osMutexId_t s_rls_pic_mutex;
static uint32_t s_rls_pic_addr; /* capacity - RLS_PIC_PERSIST_BASE */

/* ---- 记录校验与加载 ---- */

static bool _rls_pic_record_valid(void)
{
    if (s_rls_pic_rec.magic != RLS_PIC_MAGIC)
        return false; /* 空记录（擦除态全 0xFF）也在此失败 */

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_rls_pic_rec,
                                 sizeof(s_rls_pic_rec) - sizeof(s_rls_pic_rec.crc32));
    return s_rls_pic_rec.crc32 == crc;
}

static void _rls_pic_load(void)
{
    if (dev_storage_read(dev_w25qxx_get(), s_rls_pic_addr, (uint8_t *)&s_rls_pic_rec,
                         sizeof(s_rls_pic_rec)) < 0)
        return;

    if (_rls_pic_record_valid()) {
        for (uint8_t i = 0; i < RLS_PIC_SLOT_COUNT; i++)
            s_rls_pic_slot_valid[i] = true;
    }
}

/* ---- 公开 API ---- */

bool rls_pic_save(uint8_t slot, const uint8_t *bitmap, uint8_t color)
{
    if (slot >= RLS_PIC_SLOT_COUNT)
        return false;

    osMutexAcquire(s_rls_pic_mutex, osWaitForever);

    s_rls_pic_rec.magic            = RLS_PIC_MAGIC;
    s_rls_pic_rec.slot[slot].color = color;
    memcpy(s_rls_pic_rec.slot[slot].bitmap, bitmap, RLS_PIC_BITMAP_BYTES);
    /* 显示以 RAM 为真源：写 Flash 失败不阻断本次会话显示（重启后回退记录校验结果） */
    s_rls_pic_slot_valid[slot] = true;
    s_rls_pic_rec.crc32        = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_rls_pic_rec,
                                               sizeof(s_rls_pic_rec) - sizeof(s_rls_pic_rec.crc32));

    /* dev_storage_write 内部 4KB 扇区读-改-写，首次写入自动擦除 */
    int32_t rc = dev_storage_write(dev_w25qxx_get(), s_rls_pic_addr, (uint8_t *)&s_rls_pic_rec,
                                   sizeof(s_rls_pic_rec));

    osMutexRelease(s_rls_pic_mutex);
    return rc >= 0;
}

void rls_pic_show(uint8_t slot)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp)
        return;

    /* 防御：屏幕尺寸与协议位图大小必须一致，否则按无效槽处理（仅清屏） */
    bool bm_ok = (((dsp->screen_rows + 7) / 8) * dsp->screen_cols == RLS_PIC_BITMAP_BYTES);

    osMutexAcquire(s_rls_pic_mutex, osWaitForever);

    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });

    if (slot < RLS_PIC_SLOT_COUNT && s_rls_pic_slot_valid[slot] && bm_ok)
        app_render(&(render_cfg_t){
            .type   = RENDER_BITMAP,
            .x      = 0,
            .y      = 0,
            .w      = dsp->screen_rows,
            .h      = dsp->screen_cols,
            .color  = (display_color_t)s_rls_pic_rec.slot[slot].color,
            .bitmap = s_rls_pic_rec.slot[slot].bitmap,
        });

    app_render_save(); /* 协议持久化内容显示后同步渲染引擎持久化（上电恢复用） */
    osMutexRelease(s_rls_pic_mutex);
}

/* ---- 干接点触发任务：SW1/2/3 → 槽 0/1/2，不改亮度状态 ---- */

static void rls_pic_key_task(void *argument)
{
    (void)argument;
    static const dev_key_id_t s_key_map[RLS_PIC_SLOT_COUNT] = {
        DEV_KEY_SW1, DEV_KEY_SW2, DEV_KEY_SW3,
    };

    for (;;) {
        /* 50ms 超时轮询三个按键信号量（wait_press 阻塞等待，无 CPU 占用） */
        for (uint8_t i = 0; i < RLS_PIC_SLOT_COUNT; i++)
            if (dev_key_wait_press(s_key_map[i], 50))
                rls_pic_show(i);
    }
}

/* ---- 模块自注册 ---- */

void rls_pic_init(void)
{
    s_rls_pic_addr  = dev_storage_capacity(dev_w25qxx_get()) - RLS_PIC_PERSIST_BASE;
    s_rls_pic_mutex = osMutexNew(NULL);

    _rls_pic_load(); /* 运行于 sw_board_init，早于任何通道任务启动 */

    const osThreadAttr_t key_task_attr = {
        .name       = "rls_pic_key",
        .stack_size = 512 * 4,
        .priority   = (osPriority_t)osPriorityNormal,
    };
    osThreadNew(rls_pic_key_task, NULL, &key_task_attr);
}
sw_app_initcall(rls_pic_init);
