#pragma once

/**
 * @file    app_iap.h
 * @brief   IAP 升级协议：帧格式、协议控制块与任务接口
 */

#include <stdint.h>
#include "cmsis_os2.h"

#include "app_dispatch.h"
#include "ring_buffer.h"

#define FRAME_MIN_LEN     (5U)        /**< 最小帧长（word：帧头 4 + CRC 1） */
#define FRAME_MAX_LEN     (5U + 256U) /**< 最大帧长（word：5 + 256 载荷） */

#define FRAME_HEAD        (0x5A5A5A5AU) /**< 帧头魔数 */
#define FRAME_HEAD_OFFSET (0U)          /**< 帧头字段偏移（word） */
#define FRAME_SEQ_OFFSET  (1U)          /**< 序号字段偏移（word） */
#define FRAME_CMD_OFFSET  (2U)          /**< 命令码字段偏移（word） */
#define FRAME_LEN_OFFSET  (3U)          /**< 载荷长度字段偏移（word） */

/** @brief IAP 帧：4 个定长头字段 + 载荷/CRC 区（字段单位均为 word） */
typedef struct {
    uint32_t head;       /**< 帧头，恒为 FRAME_HEAD */
    uint32_t seq;        /**< 帧序号 */
    uint32_t cmd;        /**< 命令码 */
    uint32_t len;        /**< 载荷长度（word） */
    uint32_t data_crc[]; /**< 载荷 + CRC32（共 len + 1 个 word） */
} app_iap_frame_t;

/** @brief 取 IAP 协议控制块
 *
 *  供板级代码把"本板才有的通道"绑到 IAP 上（如 3833024 的两路 RS232）。
 *  共享的 app_iap.c 只绑定两块板都有的通道（RS485 / UDP），板级特有的通道由
 *  各板自己绑——否则共享文件要认识每块板的外设。 */
app_pcb_t *app_iap_pcb(void);

/** @brief IAP 协议处理任务：阻塞等待帧队列 → 按 cmd 字段查表分派到命令处理函数
 *  @param argument 未使用（单例任务） */
void app_iap_task(void *argument);

/** @brief IAP 帧探测（pcb_ops.probe）—— 契约见 app_dispatch.h 的 app_pcb_probe_fn_t
 *  @param self 协议控制块（提供接收 ring buffer）
 *  @param ccb 收到本帧的通道（本协议不使用）
 *  @param src 帧来源描述（本协议不使用）
 *  @param[out] scratch 框架提供的暂存区，本函数填入完整帧字节
 *  @param scratch_size 暂存区容量（字节）
 *  @param[out] total_len 完整帧长度（READY / SKIP 时有效）
 *  @param[out] aux 协议层分类（本协议取命令码 cmd & 0xFF） */
app_pcb_probe_state_t app_iap_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                          uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                          uint8_t *aux);

/** @brief 取一帧的完整命令码（给分发的接收监听识别 0x4Bxx/0x4Cxx/0xBCxx）
 *  @param frame 完整帧字节
 *  @param len   帧字节数
 *  @return 命令码；帧太短返回 0 */
uint32_t app_iap_frame_cmd(const uint8_t *frame, uint16_t len);

/** @brief 往内部 485 发一帧（主卡→从卡；从卡应答也走这里） */
void app_iap_send_internal(uint32_t cmd, uint32_t seq, uint32_t len, const uint32_t *data);

/** @brief 在当前 IAP 任务里等一帧内部应答（0x06 等 0x14 ACK 用）
 *
 *  只应在 IAP 任务上下文中调用：会直接消费本协议队列，等不到就超时返回。
 *  等待期间收到的非目标帧会被丢弃（0x06 是一次性命令，不存在需要保留的会话）。
 *  @param expect_cmd 期望的完整应答命令码（如 0x0000BC14）
 *  @param timeout_ms 超时
 *  @return true = 收到期望应答 */
bool app_iap_wait_internal_ack(uint32_t expect_cmd, uint32_t timeout_ms);
