#pragma once

/**
 * @file    app_lcsp_slot.h
 * @brief   LCSP 断电保存槽位（协议 7.5/7.6：`0x05` 存、`0x06` 取）
 *
 * **槽位是"预存画面"而不是"预存图库"**：每个槽位存一整屏 1bpp 点阵 + 它自己的颜色，
 * 存满即覆盖，没有编号/名称之类的元数据 —— 协议 7.6 只按槽位号调图，多存的字段
 * 没有任何读者。**本产品实现 3 个槽位**（现场只用放行/禁行/故障三张），协议字段
 * 是 1 字节、预留更大容量，`0x01` 上报的是本机实际数量；超出本机数量的槽位号回 `0x01`。
 *
 * **朝向以本工程为准**：存进去的是 MSB-first（`app_render`/画布/驱动的共同约定），
 * 不是协议线上的 LSB-first。转换在命令层做一次（`0x05` 入、`0x06` 出都不必再转）。
 * 这样槽位里的字节可以直接喂给 `app_render`，少一层出错的机会。
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_lcsp.h"

/** @brief 支持的槽位数（槽位号 1..N；0 = 仅显示不保存）*/
#define LCSP_SLOT_MAX (3U)

/** @brief 槽位 0 的语义：只显示、不落盘（协议 7.5）*/
#define LCSP_SLOT_NONE (0U)

/** @brief 槽位存储的载荷：几何 + 颜色 + 1bpp 点阵（MSB-first）
 *
 *  带几何是因为"换屏/换几何后旧图不适用"：不匹配就判槽位无效，而不是按旧 stride
 *  当新图铺上去 —— 后者不报错，屏上是一幅斜纹。 */
typedef struct [[gnu::packed]] {
    uint16_t rows;    /**< 存图时的整屏宽（像素）*/
    uint16_t cols;    /**< 存图时的整屏高（像素）*/
    uint8_t  color;   /**< 存图时的颜色（`dev_display_color_t`）*/
    uint8_t  bitmap[];/**< 1bpp，`ceil(rows/8) × cols` 字节，MSB-first */
} app_lcsp_slot_image_t;

/** @brief 该槽位是否有可用画面（记录存在且长度合法）
 *
 *  只读记录头（24 字节），不读整幅点阵 —— 上电后首次调用会把 3 个槽位扫一遍并缓存，
 *  之后由保存路径维护。`0x01` 的设备信息应答靠它填"有已存图片"位。
 *  @param slot 槽位号 1..`LCSP_SLOT_MAX`；其余返回 false
 *  @return true = 槽位有图 */
bool app_lcsp_slot_is_occupied(uint8_t slot);

/** @brief 任一槽位是否有图（设备信息应答的 bit1）
 *  @return true = 至少一个槽位有图 */
bool app_lcsp_slot_any_occupied(void);

/** @brief 保存一整屏点阵到槽位（覆盖旧内容）
 *  @param slot       槽位号 1..`LCSP_SLOT_MAX`
 *  @param img        载荷（几何 + 颜色 + 点阵，点阵长度由 `bitmap_len` 给出）
 *  @param bitmap_len 点阵字节数
 *  @return 0 成功（含内容相同去重），负值失败（槽位号越界/存储不可用/写失败）*/
int32_t app_lcsp_slot_save(uint8_t slot, const app_lcsp_slot_image_t *img, uint16_t bitmap_len);

/** @brief 读出槽位内容
 *
 *  只做"记录是否完整"的校验；**几何是否配得上当前屏由调用方判断**（槽位层不知道
 *  当前屏多大，也不该为它引入 `app_screen` 依赖）。
 *
 *  @param slot       槽位号 1..`LCSP_SLOT_MAX`
 *  @param[out] img   接收载荷的缓冲（按其自身容量决定能装多少）
 *  @param img_cap    img 缓冲容量（字节）
 *  @param[out] bitmap_len 实际点阵字节数
 *  @return true = 读到有效记录；false = 槽位空/记录损坏/装不下 */
bool app_lcsp_slot_load(uint8_t slot, app_lcsp_slot_image_t *img, uint16_t img_cap, uint16_t *bitmap_len);

/** @brief 单个槽位记录的载荷上限（字节）= 头 + 整屏点阵
 *
 *  命令层按它静态分配工作缓冲；`app_lcsp_slot.c` 有 static_assert 把它钉在
 *  配置记录镜像上限之内（超了 save 会返回 -1，表现为"槽位存不进去"）。 */
#define LCSP_SLOT_PAYLOAD_MAX (sizeof(app_lcsp_slot_image_t) + LCSP_BITMAP_MAX_BYTES)
