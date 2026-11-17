/**
 * @file    app_ah.h
 * @brief   AH 协议族公共定义：设备类型、错误码、通知号与长度上限
 *
 * AH = AH/MQTT（业务）+ AH/TCP（参数）。两条信道共用本头的设备类型编码与错误码；
 * 通知号只属于 MQTT 业务，但它是 20 字节定长线格式，放在协议族公共头里便于纯函数测试。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 设备类型编码（《IP 化设备通讯协议》§1.3） ---- */
#define AH_DEV_BARRIER     (0x11U) /**< 自动栏杆机 */
#define AH_DEV_PASS_LIGHT  (0x12U) /**< 通行信号灯 */
#define AH_DEV_FOG_LIGHT   (0x13U) /**< 雾灯 */
#define AH_DEV_TOP_LIGHT   (0x14U) /**< 天棚信号灯 */
#define AH_DEV_ALARM       (0x15U) /**< 报警器灯 */
#define AH_DEV_HEIGHT_LIMIT (0x16U) /**< 限高架 */
#define AH_DEV_VEHICLE_DET (0x21U) /**< 光栅/线圈/激光车检 */
#define AH_DEV_FEE_DISPLAY (0x31U) /**< 费额显示屏 */
#define AH_DEV_LED_BOARD   (0x32U) /**< LED 情报板 */
#define AH_DEV_AUDIO       (0x39U) /**< 语音播报 */

/** @brief 通知号字符数（不含结尾 NUL） */
#define APP_AH_NOTIFY_ID_LEN (20U)
/** @brief 主题前缀缓冲上限（"站8/道2/型2/号2" 共 17 字符 + NUL，留余量） */
#define APP_AH_TOPIC_PREFIX_MAX (24U)
/** @brief MQTT Client ID 缓冲上限（"CD" + 8 + 2 + 2 + 2 共 16 字符 + NUL，留余量） */
#define APP_AH_CLIENT_ID_MAX (24U)

typedef struct app_ah_cfg app_ah_cfg_t; /**< AH 参数配置（定义见 app_ah_cfg.h） */

/** @brief 参数配置错误码（0AH 应答的 ErrCode） */
typedef enum {
    APP_AH_ERR_OK = 0x00,       /**< 成功 */
    APP_AH_ERR_PARAM = 0x01,    /**< 参数非法 */
    APP_AH_ERR_SAVE = 0x02,     /**< 保存失败 */
    APP_AH_ERR_NOT_READY = 0x03,/**< 存储/模块未就绪 */
    APP_AH_ERR_CMD = 0x04,      /**< 命令或协议版本不支持 */
    APP_AH_ERR_FORMAT = 0x05,   /**< 帧格式错（长度/CRC） */
} app_ah_err_t;

/**
 * @brief 通知号：YYYYMMDDhh + 设备类型 + 设备序号 + 6 位自增
 *
 * 线格式是 20 个字符，**没有 NUL**：定长字段用 `nonstring` 标注，
 * 拼接/发送时按固定长度处理，不要用 `strlen`。
 */
typedef struct [[gnu::packed]] app_ah_notify_id {
    __attribute__((nonstring)) char date_time[10]; /**< YYYYMMDDhh（10 位） */
    __attribute__((nonstring)) char device_type[2]; /**< 设备类型（2 位 hex） */
    __attribute__((nonstring)) char device_no[2];   /**< 设备序号（2 位 hex） */
    __attribute__((nonstring)) char send_count[6];  /**< 发送计数（6 位十进制） */
} app_ah_notify_id_t;

static_assert(sizeof(app_ah_notify_id_t) == APP_AH_NOTIFY_ID_LEN, "通知号必须为 20 字节线格式");

/**
 * @brief 把字节串编码成大写 hex 字符串
 * @param[out] out 输出缓冲；容量不足时写空串（cap > 0 时）
 * @param cap  输出缓冲容量（字节）
 * @param data 输入字节（只读）
 * @param len  输入字节数
 * @return true 编码完成；false 参数为空或容量不足（需要 len*2+1 字节）
 */
bool app_ah_hex_encode(char *out, size_t cap, const uint8_t *data, size_t len);

/**
 * @brief 按文档派生 MQTT 主题前缀：`站8/道2/型2/号2`
 * @param cfg 配置（只读；用 lane_hex + device_id + device_no）
 * @param[out] out 输出缓冲（NUL 结尾）
 * @param cap 输出缓冲容量，须 >= APP_AH_TOPIC_PREFIX_MAX
 * @return true 生成成功；false 参数为空、容量不足或 device_id 为 0
 */
bool app_ah_topic_prefix_build(const app_ah_cfg_t *cfg, char *out, size_t cap);

/**
 * @brief 派生 MQTT Client ID：`CD` + 站8 + 道2 + 型2 + 号2
 * @param cfg 配置（只读）
 * @param[out] out 输出缓冲（NUL 结尾）
 * @param cap 输出缓冲容量，须 >= APP_AH_CLIENT_ID_MAX
 * @return true 生成成功；false 参数为空、容量不足或 device_id 为 0
 * @note 文档没定义 Client ID；按身份派生可保证多台设备默认不重名。
 */
bool app_ah_client_id_build(const app_ah_cfg_t *cfg, char *out, size_t cap);

/**
 * @brief 生成 20 字符通知号：YYYYMMDDhh + 类型 + 序号 + 6 位自增
 * @param[out] id 输出通知号
 * @param unix_ts 平台时间（Unix 秒）；0 表示时间未设置，日期段写全 0
 * @param cfg 配置（只读；用 device_id/device_no）
 * @param send_seq 发送计数（按 1000000 回卷）
 * @return true 生成成功；false 参数为空/类型为 0/时间转换失败
 */
bool app_ah_notify_build(app_ah_notify_id_t *id, uint32_t unix_ts, const app_ah_cfg_t *cfg,
                         uint32_t send_seq);
