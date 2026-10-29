/**
 * @file    app_lcsp_cmd.c
 * @brief   LCSP 命令实现：逐命令校验请求并组应答
 *
 * 分工（与 app_lcsp.c 的约定）：
 *   · 本文件**只组包、不发包**，也不做判重 —— 发与判重在任务里统一做，见
 *     `app_lcsp_req_t` 的说明；
 *   · 每条命令都必须给一个应答（协议 4.4：CRC 正确但语义非法时要回错误状态，
 *     不能静默丢弃，否则上位机只能靠超时判失败）。
 *
 * 校验口径统一：**先验参数、后动屏**。参数非法时一个像素都不许改
 * （协议 7.5"先校验后显示"）—— 现场表现是"发错的命令把正常画面打黑了"，
 * 而且没有任何日志能说明是谁改的。
 */

#include "app_lcsp_cmd.h"

#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "pl_mem.h"
#include "pl_net.h"

#include "app_lcsp_cfg.h"
#include "app_lcsp_slot.h"

/* ---- 命令实现前向声明 ---- */
static void _lcsp_device_info(app_lcsp_req_t *req);
static void _lcsp_set_ip(app_lcsp_req_t *req);
static void _lcsp_query_ip(app_lcsp_req_t *req);
static void _lcsp_fill_screen(app_lcsp_req_t *req);
static void _lcsp_show_image(app_lcsp_req_t *req);
static void _lcsp_show_slot(app_lcsp_req_t *req);
static void _lcsp_set_brightness(app_lcsp_req_t *req);
static void _lcsp_set_oe(app_lcsp_req_t *req);
static void _lcsp_reboot(app_lcsp_req_t *req);

/* 下标 = 命令字低半字节；下标 0 恒空（0x00 保留，见 app_lcsp_cmd.h） */
const app_lcsp_cmd_handler_fn_t g_lcsp_cmd_table[APP_LCSP_CMD_COUNT] = {
    nullptr,              /* 0x00 保留 */
    _lcsp_device_info,    /* 0x01 → 0x10 */
    _lcsp_set_ip,         /* 0x02 → 0x20 */
    _lcsp_query_ip,       /* 0x03 → 0x30 */
    _lcsp_fill_screen,    /* 0x04 → 0x40 */
    _lcsp_show_image,     /* 0x05 → 0x50 */
    _lcsp_show_slot,      /* 0x06 → 0x60 */
    _lcsp_set_brightness, /* 0x07 → 0x70 */
    _lcsp_set_oe,         /* 0x08 → 0x80 */
    _lcsp_reboot,         /* 0x09 → 0x90 */
};

/* ---- 命令层工作缓冲 ----
 *
 * 1bpp 位序翻转要在**另一块**缓冲里做（源是接收队列那帧，只读），整屏最大 2800 字节。
 * 只有协议任务用（命令表全部在本任务里跑），故单份静态即可；放 CCMRAM 是因为它
 * 只在收图时被碰，不占 SRAM 的常驻数据区。 */
static uint8_t s_image_buf[LCSP_SLOT_PAYLOAD_MAX] PL_CCMRAM;

/* ---- 小工具 ---- */

/** @brief 取大端 16 位 */
static uint16_t _lcsp_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** @brief 写大端 16 位 */
static void _lcsp_put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFU);
}

/** @brief IP 合法性（协议 7.2 行为要求 1）
 *
 *  拒绝：全零/首字节为 0（`0.x.x.x` 不是可路由主机地址）、环回、组播与保留
 *  （`224–255`）、以及 `255.255.255.x`（协议点名的那一类广播）。
 *  **网关不做校验**：`0.0.0.0` 在只有二层互通的现场是合法配置（"没有网关"），
 *  拦掉反而让现场没法保存配置。 */
static bool _lcsp_is_ip_valid(const uint8_t ip[4])
{
    if (0U == ip[0])
        return false;
    if (127U == ip[0])
        return false;
    if (ip[0] >= 224U)
        return false;
    if ((255U == ip[0]) && (255U == ip[1]) && (255U == ip[2]))
        return false;

    return true;
}

/** @brief 掩码合法性：必须是连续的一段 1（协议 7.2 "掩码非连续"）
 *
 *  判据放在**反码**上：连续掩码的反码是"低位连续若干个 1"，即 `inv` 与 `inv+1`
 *  互斥。全 0 另判 —— 它的反码是全 1，`inv+1` 溢出成 0，会漏过判据，而
 *  `0.0.0.0` 掩码在设备上等于"整个 IP 空间都在同一子网"，不是可用配置。 */
static bool _lcsp_is_mask_valid(const uint8_t mask[4])
{
    uint32_t value = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) | ((uint32_t)mask[2] << 8) | (uint32_t)mask[3];
    uint32_t inv;

    if (0U == value)
        return false;

    inv = ~value;

    return 0U == (inv & (inv + 1U));
}

/** @brief 1bpp 位序翻转：协议"bit0 最左" → 本工程"MSB 最左"
 *
 *  本工程画布 / `dev_display_draw_bitmap` / `app_render` 三处是同一个位序约定
 *  （`app_render.h` 顶部契约），而协议 7.5 规定 bit0 是最左像素。不转的话屏上
 *  每一行的 8 个像素左右颠倒 —— 文字看着像镜像，图形看着像错位，但长度与 CRC 全对。
 *
 *  查半字节表而不是逐位循环：整屏 2800 字节时是 5600 次查表 vs 22400 次移位判断，
 *  在 168MHz 上差几毫秒，而这条路径是"收到就显示"的时延敏感路径。
 */
static void _lcsp_bitmap_to_msb(const uint8_t *src, uint16_t len, uint8_t *dst)
{
    static const uint8_t s_nibble_rev[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                             0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};

    for (uint16_t i = 0; i < len; i++)
        dst[i] = (uint8_t)((s_nibble_rev[src[i] & 0x0FU] << 4) | s_nibble_rev[src[i] >> 4]);
}

/* ---- 应答组包 ---- */

/** @brief 组"只有状态码"的应答 */
static void _lcsp_reply_status(app_lcsp_req_t *req, uint8_t cmd, uint8_t status)
{
    req->reply_len = app_lcsp_build_frame(cmd, req->frame->seq, &status, 1, req->reply_buf, req->reply_cap);
}

/** @brief 组"状态码 + 应答数据"的应答 */
static void _lcsp_reply_data(app_lcsp_req_t *req, uint8_t cmd, uint8_t status, const uint8_t *rdata,
                             uint16_t rdata_len)
{
    uint8_t buf[1 + LCSP_REPLY_MAX_LEN];

    if (rdata_len > (uint16_t)(sizeof(buf) - 1U)) {
        _lcsp_reply_status(req, cmd, (uint8_t)APP_LCSP_STATUS_FAIL);
        return;
    }

    buf[0] = status;
    if (rdata_len != 0U)
        memcpy(&buf[1], rdata, rdata_len);

    req->reply_len = app_lcsp_build_frame(cmd, req->frame->seq, buf, (uint16_t)(1U + rdata_len), req->reply_buf,
                                          req->reply_cap);
}

/* ---- 配置读-改-写 ---- */

/** @brief 载入配置（无记录时取默认值），供"只改一个字段"的命令用
 *  @param[out] cfg 接收配置 */
static void _lcsp_load_cfg_or_default(app_lcsp_cfg_t *cfg)
{
    if (!app_lcsp_cfg_load(cfg))
        app_lcsp_cfg_default(cfg);
}

/* ================================================================
 *  0x01 查询设备信息
 * ================================================================ */

static void _lcsp_device_info(app_lcsp_req_t *req)
{
    /* 只读命令，不校验 DATA 长度：发现类工具可能带填充，答复比拒答有用。
       RDATA 15 字节（协议 7.1）：序列号(8) + 屏宽(2) + 屏高(2) + 槽位数(1) + 亮度(1) + 屏幕状态(1) */
    uint8_t rdata[15] = {0};
    app_lcsp_cfg_t cfg;
    uint8_t state = 0;

    _lcsp_load_cfg_or_default(&cfg);
    memcpy(rdata, cfg.serial, LCSP_SERIAL_LEN);

    uint16_t rows = app_screen_rows();
    uint16_t cols = app_screen_cols();
    _lcsp_put_be16(&rdata[8], rows);
    _lcsp_put_be16(&rdata[10], cols);

    /* 槽位数：上位机据此校验 0x05/0x06 的槽位号，必须报**真实可用数量**而不是协议上限 */
    rdata[12] = (uint8_t)LCSP_SLOT_MAX;

    /* 亮度按"硬件实际值"报：关屏时 0，与 bit0 的屏幕状态自洽。
       报期望值的话，上位机会看到"亮度 80 但屏是灭的"这种自相矛盾的快照。 */
    rdata[13] = (uint8_t)((uint16_t)app_screen_get_brightness() * 100U / 7U);

    if (app_screen_is_oe_enabled())
        state |= 0x01U;
    if (app_lcsp_slot_any_occupied())
        state |= 0x02U;
    rdata[14] = state;

    _lcsp_reply_data(req, APP_LCSP_REPLY_DEVICE_INFO, (uint8_t)APP_LCSP_STATUS_OK, rdata, sizeof(rdata));
}

/* ================================================================
 *  0x02 设置 IP（只写配置，重启生效）
 * ================================================================ */

static void _lcsp_set_ip(app_lcsp_req_t *req)
{
    app_lcsp_cfg_t cfg;
    app_lcsp_cfg_t readback;
    uint8_t rdata[14] = {0};

    do {
        if (14U != req->payload_len)
            break;

        uint16_t port = _lcsp_be16(&req->payload[12]);
        if (!_lcsp_is_ip_valid(&req->payload[0]) || !_lcsp_is_mask_valid(&req->payload[4]) || (0U == port))
            break;

        _lcsp_load_cfg_or_default(&cfg);
        memcpy(cfg.ip, &req->payload[0], 4);
        memcpy(cfg.mask, &req->payload[4], 4);
        memcpy(cfg.gw, &req->payload[8], 4);
        _lcsp_put_be16(cfg.port, port);

        if (0 != app_lcsp_cfg_save(&cfg))
            break;

        /* 协议 7.2：应答带回**从 Flash 读回**的配置，不是简单回显请求 ——
           回显只能证明"设备收到了"，读回才能证明"落盘了"，而这是重启前唯一
           可验证的环节（写错地址又不重启时可以原地重发，不会失联）。 */
        if (!app_lcsp_cfg_load(&readback))
            break;

        memcpy(rdata, &readback, sizeof(rdata));
        _lcsp_reply_data(req, APP_LCSP_REPLY_SET_IP, (uint8_t)APP_LCSP_STATUS_OK, rdata, sizeof(rdata));

        /* IAP 记录镜像（无 IAP 记录区的板子内部短路）：Recovery 上报运行态用。
           运行参数此刻**不变**，所以镜像也不该变 —— 这里写的是"下次启动要用的值"。 */
        app_flash_iap_update_net_cfg(readback.ip, readback.mask, readback.gw, _lcsp_be16(readback.port));
        return;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_SET_IP, (uint8_t)APP_LCSP_STATUS_FAIL);
}

/* ================================================================
 *  0x03 查询 IP（应答固定走 UDP 广播）
 * ================================================================ */

static void _lcsp_query_ip(app_lcsp_req_t *req)
{
    uint8_t rdata[14] = {0};

    /* 报**当前实际运行**的配置，不是 0x02 刚写入、尚未生效的那份（协议 7.3.2） */
    pl_net_get_ip(&rdata[0], &rdata[4], &rdata[8]);
    _lcsp_put_be16(&rdata[12], app_tcp_server_get_port());

    _lcsp_reply_data(req, APP_LCSP_REPLY_QUERY_IP, (uint8_t)APP_LCSP_STATUS_OK, rdata, sizeof(rdata));

    /* 广播而不是单播：上位机多网卡/NAT/跨网段时单播回不去，而广播能让同网段的
       每一台上位机都拿到设备地址（协议 7.3）。TCP 请求进来时通道忽略广播标志，
       退化为回本连接 —— 那种场合本来也收不到（协议 7.3 注意事项 1）。 */
    req->reply_broadcast = true;
}

/* ================================================================
 *  0x04 全屏填充
 * ================================================================ */

static void _lcsp_fill_screen(app_lcsp_req_t *req)
{
    uint8_t status = (uint8_t)APP_LCSP_STATUS_FAIL;

    do {
        if (1U != req->payload_len)
            break;

        uint8_t color = req->payload[0];
        /* bit3–bit7 保留必须为 0（协议 7.4）；0–7 与 dev_display_color_t 一一对应 */
        if (color > (uint8_t)DEV_DISPLAY_COLOR_WHITE)
            break;

        /* **不持久化**（协议 7.4"只改变当前显示内容，不写入断电保存区"）：
           清屏/纯色填充是瞬时状态，落盘会让掉电重启后是一屏纯色，反而看不到
           "屏体本身是否正常"这个现场判据。要掉电保持请用 0x05 + 槽位。 */
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_FILL,
            .x       = 0,
            .y       = 0,
            .w       = app_screen_rows(),
            .h       = app_screen_cols(),
            .color   = (dev_display_color_t)color,
            .persist = false,
        });
        status = (uint8_t)APP_LCSP_STATUS_OK;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_FILL_SCREEN, status);
}

/* ================================================================
 *  0x05 显示图片（整帧）
 * ================================================================ */

static void _lcsp_show_image(app_lcsp_req_t *req)
{
    uint8_t status = (uint8_t)APP_LCSP_STATUS_FAIL;

    do {
        /* DATA = 槽位号(1) + 颜色码(1) + 整屏点阵 */
        if (req->payload_len < (2U + 1U))
            break;

        uint8_t slot  = req->payload[0];
        uint8_t color = req->payload[1];

        if (slot > LCSP_SLOT_MAX)
            break;
        if (color > (uint8_t)DEV_DISPLAY_COLOR_WHITE)
            break;

        uint16_t rows   = app_screen_rows();
        uint16_t cols   = app_screen_cols();
        uint16_t stride = (uint16_t)((rows + 7U) / 8U);
        uint32_t need   = (uint32_t)stride * (uint32_t)cols;

        /* 几何超出本模块的位图上限（更大屏体）→ 明确拒绝，不猜 */
        if ((0U == need) || (need > LCSP_BITMAP_MAX_BYTES))
            break;

        /* 协议 7.5：长度必须**恰好**是 ceil(宽/8)×高，不符回 0x01 且不改屏。
           取 ≥ 不行 —— 多出来的字节会让"这一帧属于哪个屏"变得不可判。 */
        if (req->payload_len != (uint16_t)(2U + need))
            break;

        app_lcsp_slot_image_t *img = (app_lcsp_slot_image_t *)s_image_buf;
        img->rows                  = rows;
        img->cols                  = cols;
        img->color                 = color;
        _lcsp_bitmap_to_msb(&req->payload[2], (uint16_t)need, img->bitmap);

        /* **先清整屏再贴图**：位图渲染只写 bit=1 的像素（bit=0 不动，见 `_sink_bitmap`
           的注释）。不清的话上一幅图的亮点会原样留着 —— 现场表现就是"发新图没反应、
           屏还是全亮"（第一幅图铺满之后，之后所有黑底图都盖不回去）。
           清屏走 FILL 黑色（画布上就是清零），与贴图在同一个命令里、同一帧内完成。 */
        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_FILL,
            .x     = 0,
            .y     = 0,
            .w     = rows,
            .h     = cols,
            .color = DEV_DISPLAY_COLOR_BLACK,
        });

        /* 槽位 0 = 只显示不保存；槽位 1..N = 掉落盘（走槽位记录，不是显存记录） */
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_BITMAP,
            .x       = 0,
            .y       = 0,
            .w       = rows,
            .h       = cols,
            .color   = (dev_display_color_t)color,
            .persist = (LCSP_SLOT_NONE != slot),
            .bitmap  = img->bitmap,
        });
        status = (uint8_t)APP_LCSP_STATUS_OK;

        /* 协议 7.5：先显示、后落盘；落盘失败回 0x01，但显示已经生效（不回滚 ——
           半张旧图比"没存住"更糟）。 */
        if ((LCSP_SLOT_NONE != slot) && (0 != app_lcsp_slot_save(slot, img, (uint16_t)need)))
            status = (uint8_t)APP_LCSP_STATUS_FAIL;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_SHOW_IMAGE, status);
}

/* ================================================================
 *  0x06 显示已存图片
 * ================================================================ */

static void _lcsp_show_slot(app_lcsp_req_t *req)
{
    uint8_t status = (uint8_t)APP_LCSP_STATUS_FAIL;

    do {
        if (1U != req->payload_len)
            break;

        uint8_t slot = req->payload[0];
        if ((slot < 1U) || (slot > LCSP_SLOT_MAX))
            break;

        uint16_t rows   = app_screen_rows();
        uint16_t cols   = app_screen_cols();
        uint16_t stride = (uint16_t)((rows + 7U) / 8U);
        uint32_t need   = (uint32_t)stride * (uint32_t)cols;

        if ((0U == need) || (need > LCSP_BITMAP_MAX_BYTES))
            break;

        app_lcsp_slot_image_t *img = (app_lcsp_slot_image_t *)s_image_buf;
        uint16_t bitmap_len        = 0;
        if (!app_lcsp_slot_load(slot, img, (uint16_t)sizeof(s_image_buf), &bitmap_len))
            break; /* 槽位为空/记录损坏：原画面保持不变（协议 7.6）*/

        /* 几何必须与当前屏一致：存图后被换屏/换级联配置，旧 stride 铺新屏会是一幅斜纹 */
        if ((img->rows != rows) || (img->cols != cols) || (bitmap_len != (uint16_t)need))
            break;
        if (img->color > (uint8_t)DEV_DISPLAY_COLOR_WHITE)
            break;

        /* **先清整屏再贴图**：位图渲染只写 bit=1 的像素（bit=0 不动，见 `_sink_bitmap`
           的注释）。不清的话上一幅图的亮点会原样留着 —— 现场表现就是"发新图没反应、
           屏还是全亮"（第一幅图铺满之后，之后所有黑底图都盖不回去）。
           清屏走 FILL 黑色（画布上就是清零），与贴图在同一个命令里、同一帧内完成。 */
        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_FILL,
            .x     = 0,
            .y     = 0,
            .w     = rows,
            .h     = cols,
            .color = DEV_DISPLAY_COLOR_BLACK,
        });

        /* 存下来的就是 MSB-first，直接喂画布，不必再翻位序 */
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_BITMAP,
            .x       = 0,
            .y       = 0,
            .w       = rows,
            .h       = cols,
            .color   = (dev_display_color_t)img->color,
            .persist = true, /* 调图就是为了掉电后还在，这条必须持久化 */
            .bitmap  = img->bitmap,
        });
        status = (uint8_t)APP_LCSP_STATUS_OK;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_SHOW_SLOT, status);
}

/* ================================================================
 *  0x07 亮度调节
 * ================================================================ */

static void _lcsp_set_brightness(app_lcsp_req_t *req)
{
    uint8_t status = (uint8_t)APP_LCSP_STATUS_FAIL;

    do {
        if (1U != req->payload_len)
            break;

        uint8_t value = req->payload[0];
        if (LCSP_BRIGHTNESS_AUTO == value) {
            app_light_sensor_resume();
        } else if (value <= 100U) {
            app_light_sensor_set_fixed(app_lcsp_level_from_percent(value));
        } else {
            break;
        }

        /* 掉电保持（协议 7.7）。写进同一条配置记录：内容相同时配置调度器会去重，
           所以上位机重复下发同一亮度不会产生额外擦写。 */
        app_lcsp_cfg_t cfg;
        _lcsp_load_cfg_or_default(&cfg);
        cfg.brightness = value;
        if (0 != app_lcsp_cfg_save(&cfg))
            printf("[lcsp] 亮度 %u 已生效但未落盘\n", (unsigned)value);

        status = (uint8_t)APP_LCSP_STATUS_OK;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_SET_BRIGHT, status);
}

/* ================================================================
 *  0x08 显示开关
 * ================================================================ */

static void _lcsp_set_oe(app_lcsp_req_t *req)
{
    uint8_t status = (uint8_t)APP_LCSP_STATUS_FAIL;

    do {
        if (1U != req->payload_len)
            break;

        uint8_t value = req->payload[0];
        if (value > 1U)
            break;

        /* 开关与亮度是两个独立状态（见 app_screen.h 的 app_screen_oe_enable）：
           关屏只切输出，亮度档位原样保留，重新开屏恢复关闭前的内容与亮度。 */
        if (0U != value)
            app_screen_oe_enable();
        else
            app_screen_oe_disable();

        app_lcsp_cfg_t cfg;
        _lcsp_load_cfg_or_default(&cfg);
        cfg.oe_enabled = value;
        if (0 != app_lcsp_cfg_save(&cfg))
            printf("[lcsp] 屏幕开关 %u 已生效但未落盘\n", (unsigned)value);

        status = (uint8_t)APP_LCSP_STATUS_OK;
    } while (false);

    _lcsp_reply_status(req, APP_LCSP_REPLY_SET_OE, status);
}

/* ================================================================
 *  0x09 重启设备
 * ================================================================ */

static void _lcsp_reboot(app_lcsp_req_t *req)
{
    /* 只读参数：不校验 DATA 长度 —— 重启是远程故障恢复的最后手段，
       因为多带了一个字节就拒绝执行，现场就只能跑去现场按复位键。 */
    _lcsp_reply_status(req, APP_LCSP_REPLY_REBOOT, (uint8_t)APP_LCSP_STATUS_OK);
    req->reboot_after_reply = true;
}
