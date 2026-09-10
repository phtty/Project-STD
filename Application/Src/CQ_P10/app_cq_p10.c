/**
 * @file    app_cq_p10.c
 * @brief   CQ_P10 产品协议（重庆P10状态屏固化协议）— JSON over UDP 实现
 *
 * 协议文档: 重庆P10状态屏固化协议.docx
 * - 5.6 图片固化: {"cmd":"picset","nu":N,"c":"g","tK":"320个0-255,逗号分隔"} ×8 包
 *     每数据 = 1 字节(0~255) = 横向 8 像素(MSB-first); tK = 行 (K-1)*8 .. K*8-1
 *     nu=0..2 固化到 W25Qxx (8 包拼整屏 2560B 位图), 每包回 OK; nu=3 整屏点亮(不固化)
 * - 5.7 修改IP: {"cmd":"setip","ip":"..","mask":"..","wg":"..","port":N} → 存 IAP
 *     net_cfg 重启生效, 原帧回显
 *
 * 设计要点:
 * - RAM 不足以运行 cJSON → 手写关键字字符串匹配解析(零动态内存)
 * - 协议接入 app_dispatch 框架(probe 按 JSON 顶层花括号平衡切帧)
 * - 固化记录存 W25Qxx capacity-16384 起 8KB, 与渲染持久化(-8192)/LDI(-4096)隔离
 * - 落盘按槽位 CRC 指纹去重, 防重复擦写磨损
 */

#include "app_cq_p10.h"

#include <string.h>
#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"
#include "dev_display.h"
#include "dev_key.h"
#include "dev_w25qxx.h"
#include "dev_storage.h"
#include "app_udp.h"
#include "pl_net.h"
#include "pl_crc.h"
#include "pl_flash.h"

/* ======================================================================
 *  常量
 * ====================================================================== */

#define CQ_MSG_SIZE      (sizeof(frame_msg_t) + CQ_FRAME_MAX) /* 1416B */
#define CQ_PROBE_POOL    (CQ_FRAME_MAX + 16U)                 /* probe 扫描缓冲 */
#define CQ_PERSIST_BASE  (16384U)                             /* capacity - 16384 */
#define CQ_PERSIST_MAGIC (0x43515031U)                        /* "CQP1" */

/* picset 固定应答。注意: 必须放 SRAM(.data) 而非 Flash —— udp_ch_send 用 netbuf_ref
 * 引用不拷贝, ETH DMA 只能访问 SRAM, 读 Flash 地址会触发总线错误(FBES)导致
 * HAL 关闭全部 ETH 中断 (项目既有回复缓冲均为 SRAM static) */
static uint8_t s_cq_ok[] = "{\"cmd\":\"picset\",\"t\":\"OK\"}";

/* ---- 固化记录 (W25Qxx): magic + 3×(color+2560B位图) + crc32 ---- */
typedef struct {
    uint8_t color; /* display_color_t; 0xFF = 未固化 */
    uint8_t bitmap[CQ_BITMAP_SIZE];
} cq_pic_slot_t; /* 2561B */

typedef struct {
    uint32_t magic;
    cq_pic_slot_t slot[CQ_PIC_COUNT];
    uint32_t crc32; /* 覆盖 [0, sizeof-4) */
} cq_persist_t;     /* 7692B */

/* ---- 网络配置记录 (内部 Flash Sector1 @0x08004000, 24B, 6 words)
 *  自包含: 不依赖 app_iap_cfg/IAP 模块, CQ 模块可从构建中整体剔除
 *  布局独立于既有 IAP 配置格式 (本产品分支已排除 IAP) */
#define CQ_NETCFG_ADDR  (0x08004000u)
#define CQ_NETCFG_MAGIC (0x43514E31u) /* "CQN1" */

typedef struct {
    uint32_t magic;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint32_t port;
    uint32_t crc32; /* 覆盖前 20B */
} cq_netcfg_t;      /* 24B */

/* ======================================================================
 *  静态分配 (全部 SRAM: W25Qxx DMA 读不可达 CCMRAM)
 * ====================================================================== */

static cq_persist_t s_cq;                    /* 固化记录 RAM 缓存 */
static uint8_t s_seg[CQ_PIC_COUNT];          /* 每图已收段位图 bit(t-1) */
static uint32_t s_flushed_crc[CQ_PIC_COUNT]; /* 每槽上次落盘指纹 */
static uint32_t s_persist_addr;
static cq_netcfg_t s_netcfg; /* 网络配置 SRAM 缓存 */
static bool s_netcfg_valid;
static osMutexId_t s_cq_mutex; /* 保护固化缓存: 协议段写 vs 按键显示 */

/* ---- 帧队列 ---- */
static StaticQueue_t s_cq_queue_cb;
static uint8_t s_cq_queue_buf[2 * CQ_MSG_SIZE];
static const osMessageQueueAttr_t s_cq_queue_attr = {
    .name    = "proto_cq_queue",
    .cb_mem  = &s_cq_queue_cb,
    .cb_size = sizeof(s_cq_queue_cb),
    .mq_mem  = s_cq_queue_buf,
    .mq_size = sizeof(s_cq_queue_buf),
};

static proto_mask_t s_cq_mask;
osMessageQueueId_t g_cq_msg_queue;
osThreadId_t g_cq_task_handle;
const osThreadAttr_t cq_task_attr = {
    .name       = "cq_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ======================================================================
 *  JSON 极简解析器 — 关键字字符串匹配, 零动态内存
 * ====================================================================== */

/** 定位 '"key":' 后的值区起始; 返回 NULL 表示键不存在 */
static const char *cq_json_value(const char *js, uint32_t len, const char *key, uint32_t *avail)
{
    uint32_t klen = (uint32_t)strlen(key);

    for (uint32_t i = 0; i < len; i++) {
        /* 键匹配: '"'+key+'"' 三连, 防 "c"/"cmd" 混淆 */
        if (js[i] == '"' && i + 1 + klen < len && memcmp(js + i + 1, key, klen) == 0 && js[i + 1 + klen] == '"') {
            uint32_t p = i + 1 + klen + 1;
            while (p < len && (js[p] == ' ' || js[p] == '\t'))
                p++;
            if (p < len && js[p] == ':') {
                p++;
                while (p < len && (js[p] == ' ' || js[p] == '\t'))
                    p++;
                if (p < len) {
                    *avail = len - p;
                    return js + p;
                }
            }
        }
    }
    return NULL;
}

/** 取引号字符串值; 值内不允许 '"' 与 '\'。cap 含末尾 '\0' */
static bool cq_json_str(const char *js, uint32_t len, const char *key, char *out, uint32_t cap,
                        uint32_t *out_len)
{
    uint32_t avail;
    const char *v = cq_json_value(js, len, key, &avail);
    if (!v || avail < 2 || v[0] != '"')
        return false;
    v++;
    avail--;

    uint32_t n = 0;
    while (n < avail && v[n] != '"') {
        if (v[n] == '\\' || n + 1 >= cap)
            return false; /* 协议值不含转义 */
        out[n] = v[n];
        n++;
    }
    if (n >= avail)
        return false; /* 引号未闭合 */
    out[n]   = '\0';
    *out_len = n;
    return true;
}

/** 取整数: 纯数字串, 范围 [min,max], 值后仅允许 , ] } 或空白 */
static bool cq_json_int(const char *js, uint32_t len, const char *key, uint32_t min,
                        uint32_t max, uint32_t *out)
{
    uint32_t avail;
    const char *v = cq_json_value(js, len, key, &avail);
    if (!v || avail == 0 || v[0] < '0' || v[0] > '9')
        return false;

    uint32_t val = 0, n = 0;
    while (n < avail && v[n] >= '0' && v[n] <= '9') {
        if (val > (UINT32_MAX - 9) / 10)
            return false;
        val = val * 10 + (uint32_t)(v[n] - '0');
        n++;
    }
    if (n < avail) {
        char c = v[n];
        if (c != ',' && c != '}' && c != ']' && c != ' ' && c != '\t')
            return false;
    }
    if (val < min || val > max)
        return false;
    *out = val;
    return true;
}

/** 严格点分十进制 "a.b.c.d" → 4 字节 (每段 1~3 位数字, ≤255) */
static bool cq_parse_ipv4(const char *s, uint32_t n, uint8_t out[4])
{
    uint32_t seg = 0, val = 0, digits = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 3)
                return false;
            val = val * 10 + (uint32_t)(c - '0');
            if (val > 255)
                return false;
        } else if (c == '.') {
            if (digits == 0 || seg >= 3)
                return false;
            out[seg++] = (uint8_t)val;
            val        = 0;
            digits     = 0;
        } else {
            return false;
        }
    }
    if (digits == 0 || seg != 3)
        return false;
    out[3] = (uint8_t)val;
    return true;
}

/** 解析 picset 段值 → 320 字节; 兼容两种线上格式:
 *  文档形式:  "t1":"0,255,..."   (引号字符串)
 *  上位机实际: "t1":[0,255,...]  (JSON 裸数组)
 *  直接于原帧上解析, 零暂存缓冲; 值须恰 320 个 0~255, 超限/非法整段拒绝 */
static bool cq_parse_t_value(const char *js, uint32_t len, const char *key,
                             uint8_t out[CQ_PACKET_PAYLOAD])
{
    uint32_t avail;
    const char *v = cq_json_value(js, len, key, &avail);
    if (!v || avail < 2)
        return false;

    char term; /* 段结束符 */
    if (v[0] == '"') {
        v++;
        avail--;
        term = '"';
    } else if (v[0] == '[') {
        v++;
        avail--;
        term = ']';
    } else {
        return false; /* 既非字符串也非数组 */
    }

    uint32_t idx = 0, val = 0, digits = 0;
    for (uint32_t i = 0; i <= avail; i++) {
        char c = (i < avail) ? v[i] : term; /* 末尾补结束符强制收尾 */
        if (c >= '0' && c <= '9') {
            if (++digits > 3)
                return false;
            val = val * 10 + (uint32_t)(c - '0');
            if (val > 255)
                return false;
        } else if (c == ' ' || c == '\t') {
            continue; /* 宽容数字间空白 */
        } else if (c == ',' || c == term) {
            if (digits == 0 || idx >= CQ_PACKET_PAYLOAD)
                return false; /* 空段/超数/尾逗号 */
            out[idx++] = (uint8_t)val;
            val        = 0;
            digits     = 0;
            if (c == term)
                return idx == CQ_PACKET_PAYLOAD; /* 收尾: 须恰 320 个 */
        } else {
            return false; /* 非法字符 */
        }
    }
    return false; /* 未遇到结束符 → 未闭合 */
}

/** 颜色字符 r/g/y → display_color_t; 其他返回 0 */
static uint8_t cq_color_of(char c)
{
    switch (c) {
        case 'r':
            return (uint8_t)COLOR_RED;
        case 'g':
            return (uint8_t)COLOR_GREEN;
        case 'y':
            return (uint8_t)COLOR_YELLOW;
        default:
            return 0;
    }
}

/* ======================================================================
 *  固化记录持久化 (W25Qxx capacity-16384, 8KB)
 * ====================================================================== */

static void cq_persist_reset(void)
{
    memset(&s_cq, 0, sizeof(s_cq)); /* 对齐填充字节恒为 0, CRC 确定 */
    s_cq.magic = CQ_PERSIST_MAGIC;
    for (uint8_t i = 0; i < CQ_PIC_COUNT; i++)
        s_cq.slot[i].color = 0xFF;
}

static bool cq_persist_valid(void)
{
    if (s_cq.magic != CQ_PERSIST_MAGIC)
        return false;
    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_cq, sizeof(s_cq) - 4);
    return s_cq.crc32 == crc;
}

static void cq_persist_load(void)
{
    dev_storage_t *w = dev_w25qxx_get();
    s_persist_addr   = dev_storage_capacity(w) - CQ_PERSIST_BASE;

    if (dev_storage_read(w, s_persist_addr, (uint8_t *)&s_cq, sizeof(s_cq)) < 0 || !cq_persist_valid()) {
        cq_persist_reset(); /* 首次上电(全 0xFF)或记录损坏 → 空记录 */
        return;
    }
    for (uint8_t i = 0; i < CQ_PIC_COUNT; i++)
        s_flushed_crc[i] =
            pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_cq.slot[i], sizeof(s_cq.slot[i]));
}

/** 槽位落盘: 内容未变(指纹相同)则跳过, 防擦写磨损 */
static void cq_persist_flush(uint8_t nu)
{
    uint32_t slot_crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_cq.slot[nu], sizeof(s_cq.slot[nu]));
    if (slot_crc == s_flushed_crc[nu])
        return;

    s_cq.crc32 = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)&s_cq, sizeof(s_cq) - 4);
    int32_t rc = dev_storage_write(dev_w25qxx_get(), s_persist_addr, (uint8_t *)&s_cq,
                                   sizeof(s_cq));
    if (rc >= 0)
        s_flushed_crc[nu] = slot_crc; /* 写失败保留旧指纹, 下次重试 */
}

/* ======================================================================
 *  setip / picset 处理
 * ====================================================================== */

static bool cq_netcfg_save(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                           uint32_t port);

static void cq_reply_ok(channel_t *ch)
{
    channel_send(ch, s_cq_ok, sizeof(s_cq_ok) - 1); /* 同步发送, 发送期间缓冲有效 */
}

/** setip: ip/mask/wg/port 全合法才写 IAP net_cfg(重启生效), 原帧回显 */
static void cq_handle_setip(frame_msg_t *msg)
{
    const char *js = (const char *)msg->data;
    uint32_t len   = msg->data_len;
    uint8_t ip[4], mask[4], gw[4];
    char buf[20];
    uint32_t blen, port;

    if (!cq_json_str(js, len, "ip", buf, sizeof(buf), &blen) || !cq_parse_ipv4(buf, blen, ip))
        return;
    if (!cq_json_str(js, len, "mask", buf, sizeof(buf), &blen) || !cq_parse_ipv4(buf, blen, mask))
        return;
    if (!cq_json_str(js, len, "wg", buf, sizeof(buf), &blen) || !cq_parse_ipv4(buf, blen, gw))
        return; /* 键名按协议为 "wg" */
    if (!cq_json_int(js, len, "port", 1, 65535, &port))
        return;

    cq_netcfg_save(ip, mask, gw, port);
    channel_send(msg->ch, msg->data, msg->data_len); /* 原帧逐字节 echo */
}

/* ======================================================================
 *  网络配置持久化 — 内部 Flash Sector1, 自包含(pl_flash), 重启后 cq_net_apply 应用
 * ====================================================================== */

static void cq_netcfg_load(void)
{
    memcpy(&s_netcfg, (const void *)CQ_NETCFG_ADDR, sizeof(s_netcfg)); /* 内存映射读 */

    if (s_netcfg.magic != CQ_NETCFG_MAGIC)
        return; /* 空记录(擦除态全 0xFF)或旧 IAP 布局 → 无效 */
    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (const uint8_t *)&s_netcfg, 20);
    if (s_netcfg.crc32 != crc)
        return; /* CRC 不符 → 无效, 不应用不覆盖(由下次 setip 重写) */
    s_netcfg_valid = true;
}

/** 写 IAP Sector1 记录: 整扇区擦除 + 6 word 编程。setip 低频, 擦写寿命无忧 */
static bool cq_netcfg_save(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                           uint32_t port)
{
    s_netcfg.magic = CQ_NETCFG_MAGIC;
    memcpy(s_netcfg.ip, ip, 4);
    memcpy(s_netcfg.mask, mask, 4);
    memcpy(s_netcfg.gw, gw, 4);
    s_netcfg.port  = port;
    s_netcfg.crc32 = pl_crc32_calc(pl_crc_get_handle(), (const uint8_t *)&s_netcfg, 20);

    pl_flash_unlock();
    pl_flash_clear_errors();
    if (pl_flash_erase_sector(PL_FLASH_SECTOR_1, PL_FLASH_VOLTAGE_3) != 0) {
        pl_flash_lock();
        return false;
    }
    const uint32_t *w = (const uint32_t *)&s_netcfg;
    for (uint32_t i = 0; i < sizeof(s_netcfg) / 4; i++) {
        if (pl_flash_program_word(CQ_NETCFG_ADDR + i * 4, w[i]) != 0) {
            pl_flash_lock();
            return false;
        }
    }
    pl_flash_lock();
    s_netcfg_valid = true;
    return true;
}

/** picset: nu=3 整屏点亮; nu=0..2 固化段直写 + 满 8 段落盘; 合法包回 OK */
static void cq_handle_picset(frame_msg_t *msg)
{
    const char *js = (const char *)msg->data;
    uint32_t len   = msg->data_len;
    uint32_t nu;
    char cstr[4];
    uint32_t clen;

    if (!cq_json_int(js, len, "nu", 0, 3, &nu))
        return;
    if (!cq_json_str(js, len, "c", cstr, sizeof(cstr), &clen) || clen != 1)
        return;
    uint8_t color = cq_color_of(cstr[0]);
    if (color == 0)
        return; /* c 非 r/g/y */

    if (nu == 3) { /* 全屏点亮: 不解析 t 数据, 不固化 */
        dev_display_t *dsp = dev_display_get();
        if (dsp)
            dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols,
                             (display_color_t)color);
        cq_reply_ok(msg->ch);
        return;
    }

    /* 枚举 t1..t8: 恰 1 个命中才合法 */
    uint8_t seg_no = 0, hit = 0;
    char key[3];
    for (uint8_t n = 1; n <= 8; n++) {
        key[0] = 't';
        key[1] = (char)('0' + n);
        key[2] = '\0';
        uint32_t avail;
        if (cq_json_value(js, len, key, &avail)) {
            hit++;
            seg_no = n;
        }
    }
    if (hit != 1)
        return;

    uint8_t bytes[CQ_PACKET_PAYLOAD]; /* 320B, 任务栈内可容纳 */
    key[1] = (char)('0' + seg_no);
    if (!cq_parse_t_value(js, len, key, bytes))
        return; /* 值缺失/个数≠320/超 255/非法字符 → 整包拒绝 */

    /* 段直写 RAM 缓存 (tK 在整幅位图中的偏移 = (K-1)*320); 持锁防按键任务并发读 */
    osMutexAcquire(s_cq_mutex, osWaitForever);
    s_cq.slot[nu].color = color;
    memcpy(&s_cq.slot[nu].bitmap[(seg_no - 1) * CQ_PACKET_PAYLOAD], bytes, CQ_PACKET_PAYLOAD);
    s_seg[nu] |= (uint8_t)(1u << (seg_no - 1));
    if (s_seg[nu] == 0xFF) { /* 8 段齐 → 落盘一次 */
        s_seg[nu] = 0;
        cq_persist_flush(nu);
    }
    osMutexRelease(s_cq_mutex);
    cq_reply_ok(msg->ch);
}

/* ======================================================================
 *  干接点显示固化图 — SW1/SW2/SW3 → 槽 0/1/2
 *  槽未固化(color=0xFF) → 清屏; 不改亮度状态
 * ====================================================================== */

static void cq_pic_show(uint8_t slot)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp)
        return;

    osMutexAcquire(s_cq_mutex, osWaitForever);
    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    if (slot < CQ_PIC_COUNT && s_cq.slot[slot].color != 0xFF)
        dev_display_draw_bitmap(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols,
                                s_cq.slot[slot].bitmap,
                                (display_color_t)s_cq.slot[slot].color);
    osMutexRelease(s_cq_mutex);
}

static void cq_key_task(void *argument)
{
    (void)argument;
    static const dev_key_id_t s_key_map[CQ_PIC_COUNT] = {
        DEV_KEY_SW1,
        DEV_KEY_SW2,
        DEV_KEY_SW3,
    };

    for (;;) {
        /* 50ms 超时轮询三个按键信号量 (EXTI 下降沿触发, 无 CPU 占用) */
        for (uint8_t i = 0; i < CQ_PIC_COUNT; i++)
            if (dev_key_wait_press(s_key_map[i], 50))
                cq_pic_show(i);
    }
}

static void cq_handle_frame(frame_msg_t *msg)
{
    const char *js = (const char *)msg->data;
    uint32_t len   = msg->data_len;
    char cmd[16];
    uint32_t cmd_len;

    if (!cq_json_str(js, len, "cmd", cmd, sizeof(cmd), &cmd_len))
        return;
    if (cmd_len == 6 && memcmp(cmd, "picset", 6) == 0)
        cq_handle_picset(msg);
    else if (cmd_len == 5 && memcmp(cmd, "setip", 5) == 0)
        cq_handle_setip(msg);
    /* 其他 cmd → 忽略不回复 */
}

/* ======================================================================
 *  帧探测 — JSON 字节流切帧: 顶层花括号平衡 (跳过字符串内字符与转义)
 * ====================================================================== */

static proto_probe_sta_t cq_probe_frame(const channel_t *ch, const ring_buffer_t *buff,
                                        uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    static uint8_t pool[CQ_PROBE_POOL];

    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < 12) /* 最小合法帧 {"cmd":""} */
        return PROTO_PROBE_WAIT;
    if (avail > sizeof(pool))
        avail = sizeof(pool);
    rb_peek(buff, 0, pool, avail, nullptr);

    if (pool[0] != '{')
        return PROTO_PROBE_FAKE; /* 前导垃圾 → dispatch 跳 1 字节重试 */

    uint32_t depth = 0;
    bool in_str    = false;
    for (uint32_t i = 0; i < avail; i++) {
        uint8_t c = pool[i];
        if (in_str) {
            if (c == '\\') {
                i++; /* 跳过转义字符 (含 \" \{ \}) */
                continue;
            }
            if (c == '"')
                in_str = false;
        } else {
            if (c == '"')
                in_str = true;
            else if (c == '{')
                depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    if (i + 1 < 12)
                        return PROTO_PROBE_FAKE;
                    *total_len = i + 1;
                    return PROTO_PROBE_READY;
                }
            }
        }
    }
    /* 未闭合: 缓冲已满仍无配 `}` → 超长伪帧; 否则等更多数据 */
    return (avail >= sizeof(pool)) ? PROTO_PROBE_FAKE : PROTO_PROBE_WAIT;
}

/* ======================================================================
 *  帧处理任务 + 模块自注册
 * ====================================================================== */

static void cq_handle_task(void *argument)
{
    (void)argument;
    static uint8_t _msg_buf[CQ_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;

    g_cq_msg_queue = osMessageQueueNew(2, CQ_MSG_SIZE, &s_cq_queue_attr);
    app_proto_set_frame_queue(s_cq_mask, g_cq_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_cq_msg_queue, msg, NULL, osWaitForever))
            continue;
        cq_handle_frame(msg);
    }
}

/** 启动时应用保存的 IP/端口(重启后 setip 生效的载体); 无效记录 → 默认 192.168.1.10:20102 */
static void cq_net_apply(void)
{
    uint16_t port = CQ_UDP_PORT_DEFAULT;

    cq_netcfg_load();
    if (s_netcfg_valid) {
        uint8_t cur_ip[4], cur_mask[4], cur_gw[4];
        pl_net_get_ip(cur_ip, cur_mask, cur_gw);
        if (memcmp(s_netcfg.ip, cur_ip, 4) || memcmp(s_netcfg.mask, cur_mask, 4) || memcmp(s_netcfg.gw, cur_gw, 4))
            pl_net_set_ip(s_netcfg.ip, s_netcfg.mask, s_netcfg.gw);

        if (s_netcfg.port >= 1 && s_netcfg.port <= 65535)
            port = (uint16_t)s_netcfg.port;
    }
    /* app_udp_start 在 sw_board_init 之后执行, 此处设置端口先于监听绑定 */
    app_udp_set_port(port);
}

[[maybe_unused]] static void cq_module_init(void)
{
    s_cq_mutex = osMutexNew(NULL);

    cq_persist_load();

    /* 上电默认显示: 第 0 号固化图存在则显示 (未固化则不动屏幕) */
    if (s_cq.slot[0].color != 0xFF)
        cq_pic_show(0);

    cq_net_apply();

    ring_buffer_t *rb = app_proto_acquire_buf(1, 2048);
    s_cq_mask         = app_proto_register(cq_probe_frame, rb);
    if (s_cq_mask == 0)
        return;
    app_proto_bind_channel(s_cq_mask, CH_ID_UDP);

    g_cq_task_handle = osThreadNew(cq_handle_task, nullptr, &cq_task_attr);

    /* 干接点显示固化图任务 */
    const osThreadAttr_t key_task_attr = {
        .name       = "cq_key_task",
        .stack_size = 512 * 4,
        .priority   = (osPriority_t)osPriorityNormal,
    };
    osThreadNew(cq_key_task, NULL, &key_task_attr);
}
sw_app_initcall(cq_module_init);
