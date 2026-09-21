/**
 * @file    app_cfg_sched.c
 * @brief   配置调度器实现（见 app_cfg_sched.h）
 */

#include "app_cfg_sched.h"

#include <stdio.h> /* printf —— 原先靠 main.h → HAL 传递进来 */
#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_storage.h"
#include "dev_w25qxx.h"

typedef struct {
    cfg_sched_desc_t desc;
} cfg_sched_owner_t;

static cfg_sched_owner_t s_owners[CFG_REGION_MAX_BLOCKS];
static uint8_t s_owner_cnt;
static dev_storage_t *s_stor;
static bool s_ready;

/* 所有者 → 块号（0xFF = 尚未落位）。由启动扫描按记录头里的 name 反查得到，
   不再等于注册序号 —— 见文件末尾 _cfg_sched_scan 的说明。 */
static uint8_t s_owner_block[CFG_REGION_MAX_BLOCKS];
static uint8_t s_scanned_owners = 0xFF; /* 扫描时的所有者数；数量变了则重扫 */

/* 调度器内部组包缓冲: 最大记录 (render ≈ 1.4KB) + 24B 头 */
static uint8_t s_scratch[CFG_RECORD_HDR_SIZE + CFG_RECORD_MAX_IMAGE];

/* ---- 注册 ---- */
uint8_t app_cfg_sched_register(const cfg_sched_desc_t *desc)
{
    if (!desc || !desc->name || strlen(desc->name) == 0
     || strlen(desc->name) >= CFG_RECORD_NAME_MAX)
        return 0xFF;
    if (s_owner_cnt >= CFG_REGION_MAX_BLOCKS)
        return 0xFF;

    /* 名字唯一性防御: 重复注册忽略 */
    for (uint8_t i = 0; i < s_owner_cnt; i++)
        if (strncmp(s_owners[i].desc.name, desc->name, CFG_RECORD_NAME_MAX) == 0)
            return 0xFF;

    uint8_t id        = s_owner_cnt;
    s_owners[id].desc = *desc;
    s_owner_cnt++;
    return id;
}

/* ---- 容量契约快照 (W25Qxx 已 hw_dev 初始化, JEDEC 容量可用) ----
 *
 * 绑定（取存储句柄 + 读容量）在首次使用时做，而不是只在 initcall 里做一次：
 * 取句柄用的是 getter，任何时刻问都成立，没有"必须先跑过某个 initcall"的理由。
 * 于是少一个启动顺序依赖 —— 万一将来有人在 sw_dev 之前的层里就 load，也不会
 * 静默拿到"未就绪"。initcall 仍保留：诊断信息要在启动时必定打印一次，而不是
 * 等某个模块先用到调度器才出现。 */
static bool s_bound;

static void _cfg_sched_bind(void)
{
    if (s_bound)
        return;

    s_stor = dev_w25qxx_get();

    uint32_t cap = s_stor ? dev_storage_capacity(s_stor) : 0;

    /* 器件尚未就绪时不闩锁：容量 0 表示 JEDEC ID 未识别（见 dev_w25qxx._jedec_capacity），
       此刻判定的"不可用"可能是暂时的。闩死会让本上电周期内永久禁用持久化且无重试机会。 */
    if (cap == 0)
        return;
    s_bound = true;

    /* 门槛必须与**编译期契约同源**：配置区在器件尾部（capacity - (id+1)*4KB），
       而字库从地址 0 起占据了前 BOARD_FONT_LIB_TOTAL_BYTES 字节（板级量，两版
       字库大小不同）。只判"容量装得下配置区"（>= CFG_REGION_BYTES，32KB）是不够的
       —— JEDEC ID 若被误读成一个"合法但更小"的值（如 0x17 = 8MB），门槛照样通过，
       块地址落到 8MB-32KB，**正好落在字库区内部**，首次 save 的扇区擦除就把字库
       数据毁了。故要求：字库之后必须还放得下整个配置区。 */
    s_ready = (cap >= BOARD_FONT_LIB_TOTAL_BYTES + CFG_REGION_BYTES);

    /* 持久化整体失效必须是可见的：各调用方都不检查 save 的返回值，
       否则"配置不生效"在现场无从查起。 */
    printf("[cfg] W25Qxx 容量 %u KB, 字库 %u KB + 配置区 %u KB -> %s\n", (unsigned)(cap / 1024U),
           (unsigned)(BOARD_FONT_LIB_TOTAL_BYTES / 1024U), (unsigned)(CFG_REGION_BYTES / 1024U),
           s_ready ? "就绪" : "不可用(持久化已禁用)");
}

/* 调度器互斥：s_scratch（本文件）与 cfg_record.c 的去重读回缓冲都是文件级静态，
   而 load/save 可能来自不同任务（LDI 任务经 app_vms_ctrl、RLS 任务经 app_rls_cmd
   都会触发渲染显存落盘）。没有这把锁，两个任务同时 save 会让后到者覆写先到者的
   组包内容，落盘记录互相混杂 → 下次上电校验失败 → 静默回落默认值。
   顺带也串行化了 _cfg_sched_scan（它写 s_owner_block[] / s_scanned_owners）。
   在 sw_dev(2) 创建：早于任何协议任务，故 save/load 里可以直接用。 */
static osMutexId_t s_lock;

static void _cfg_sched_init(void)
{
    const osMutexAttr_t attr = {.name = "cfg_sched", .attr_bits = osMutexPrioInherit};
    s_lock                   = osMutexNew(&attr);

    _cfg_sched_bind();
}
sw_dev_initcall(_cfg_sched_init);

bool app_cfg_sched_ready(void)
{
    _cfg_sched_bind(); /* 未绑定就问会答 false，与存储实际可用性无关 */
    return s_ready;
}

/* ---- 块地址 ---- */

/* 块 0 = 最后一个扇区: 块 i 地址 = capacity - (i+1)*4096 */
static uint32_t _block_addr(uint8_t blk)
{
    return dev_storage_capacity(s_stor) - (uint32_t)(blk + 1) * CFG_REGION_SECTOR;
}

/**
 * 启动扫描 —— 按记录头里的 name 反查每个所有者记录所在的块
 *
 * 原实现把"块号 = 注册序号"当作纯计算，隐含假设了注册顺序稳定。而注册顺序
 * 就是同层 initcall 顺序，即**链接顺序 / 构建清单的文件次序** —— 移动或重命名
 * 源文件、新增一个排得更前的注册者，都会让所有块号平移，于是旧记录在原地址
 * 上依然有效却再也没人去找，表现为"配置莫名其妙全部回落默认值"。
 *
 * 改为"找出来"而不是"算出来"：记录位置与任何顺序无关。唯一还会丢配置的情况
 * 变成"改了注册名"—— 那本来就该视为另一个模块。
 *
 * 只比对 name、不看 version：版本升级应当判为"记录失效、回落默认"，而不是
 * 让记录搬家（正式读取时 cfg_record_load 会在找到的地址上做全量校验）。
 */
static void _cfg_sched_scan(void)
{
    bool taken[CFG_REGION_MAX_BLOCKS] = {false};

    for (uint8_t i = 0; i < CFG_REGION_MAX_BLOCKS; i++)
        s_owner_block[i] = 0xFF;

    /* 1) 认领：记录头里的 name 与本所有者的注册名相符 */
    for (uint8_t blk = 0; blk < CFG_REGION_MAX_BLOCKS; blk++) {
        cfg_record_hdr_t hdr;
        if (dev_storage_read(s_stor, _block_addr(blk), (uint8_t *)&hdr, sizeof(hdr)) != 0)
            continue;
        if ((uint8_t)hdr.name[0] == 0xFFU)
            continue; /* 空块 */

        for (uint8_t id = 0; id < s_owner_cnt; id++) {
            if (s_owner_block[id] != 0xFF)
                continue; /* 已被更靠前的块认领 */
            /* name 为 NUL 填充定长字段；正常写入的记录两处比较等价 */
            if (strncmp(hdr.name, s_owners[id].desc.name, CFG_RECORD_NAME_MAX) == 0) {
                s_owner_block[id] = blk;
                taken[blk]        = true;
                break;
            }
        }
    }

    /* 2) 落位：首次上电（或换名）时给未认领者分一个空块。
       优先用注册序号那块，其次第一个空块 —— 落位只需"是空的"，不需要可预测。 */
    for (uint8_t id = 0; id < s_owner_cnt; id++) {
        if (s_owner_block[id] != 0xFF)
            continue;

        uint8_t blk = 0xFF;
        if (id < CFG_REGION_MAX_BLOCKS && !taken[id])
            blk = id;
        else
            for (uint8_t b = 0; b < CFG_REGION_MAX_BLOCKS; b++)
                if (!taken[b]) {
                    blk = b;
                    break;
                }

        if (blk != 0xFF) {
            s_owner_block[id] = blk;
            taken[blk]        = true;
        }
    }
}

/** @brief 确保存储已绑定、且扫描已按当前所有者集合完成（首次使用或注册数变化时执行）*/
static void _cfg_sched_ensure_scan(void)
{
    _cfg_sched_bind();
    if (!s_stor || !s_ready) return;
    if (s_scanned_owners == s_owner_cnt) return;

    _cfg_sched_scan();
    s_scanned_owners = s_owner_cnt;
}

static uint32_t _owner_addr(uint8_t id)
{
    if (!s_stor || !s_ready || id >= s_owner_cnt)
        return 0;

    uint8_t blk = s_owner_block[id];
    if (blk == 0xFF)
        return 0; /* 无空块可落位 */

    return _block_addr(blk);
}

/* ---- 加载/保存 (内部用注册名 + version + cfg_record 全量校验) ---- */
cfg_rec_sta_t app_cfg_sched_load(uint8_t id, uint8_t *payload, uint16_t payload_cap, uint16_t *payload_len)
{
    uint32_t addr;

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    _cfg_sched_ensure_scan();

    if (id >= s_owner_cnt || !s_stor || !s_ready) {
        if (s_lock) osMutexRelease(s_lock);
        return CFG_REC_IO_ERR;
    }

    addr = _owner_addr(id);
    if (!addr) {
        if (s_lock) osMutexRelease(s_lock);
        return CFG_REC_IO_ERR;
    }

    cfg_rec_sta_t sta =
        cfg_record_load(s_stor, addr, s_owners[id].desc.name, s_owners[id].desc.version, NULL,
                        payload, payload_cap, payload_len);

    if (s_lock) osMutexRelease(s_lock);
    return sta;
}

int32_t app_cfg_sched_save(uint8_t id, const uint8_t *payload, uint16_t payload_len)
{
    uint32_t addr;
    int32_t  sta;

    if (!payload) return -1;

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    _cfg_sched_ensure_scan();

    if (id >= s_owner_cnt || !s_stor || !s_ready)
        goto fail;

    addr = _owner_addr(id);
    if (!addr)
        goto fail;

    sta = cfg_record_save(s_stor, addr, s_owners[id].desc.name, s_owners[id].desc.version, NULL,
                          payload, payload_len, s_scratch, sizeof(s_scratch));
    if (s_lock) osMutexRelease(s_lock);
    return sta;

fail:
    if (s_lock) osMutexRelease(s_lock);
    return -1;
}

/* ---- 启动加载遍: 按注册顺序调用各 load 回调 ---- */
void app_cfg_sched_load_all(void)
{
    _cfg_sched_ensure_scan();

    for (uint8_t o = 0; o < s_owner_cnt; o++)
        if (s_owners[o].desc.load)
            s_owners[o].desc.load();
}
sw_app_initcall(app_cfg_sched_load_all);
