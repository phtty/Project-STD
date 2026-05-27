#include "iap.h"

#include "crc.h"

#include "udp_app.h"
#include "config_info.h"
#include "iap_cmd.h"

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osMessageQueueId_t gx_IapQueue;
osThreadId_t g_iap_task_handle;
const osThreadAttr_t IapTask_attributes = {
    .name       = "iap_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/**
 * @brief 灏嗙紦鍐插尯涓鐨勬暟鎹杩涜屽崗璁瑙ｆ瀽
 *
 */
void iap_handle_task(void *argument)
{
    static frame_msg_t msg;
    const osMessageQueueAttr_t proto_iap_queue_attr = {
        .name = "g_proto_iap_queue",
    };
    gx_IapQueue                                = osMessageQueueNew(2, sizeof(frame_msg_t), &proto_iap_queue_attr);
    g_frame_queue[proto_index(PROTO_MASK_IAP)] = gx_IapQueue;

    for (;;) {
        // 绛夊緟鏁版嵁鎺ユ敹浠诲姟鍙戦侀槦鍒
        if (osOK != osMessageQueueGet(gx_IapQueue, &msg, NULL, osWaitForever)) {
            continue;
        }

        iap_frame_t *frame_data = (iap_frame_t *)msg.data;
        uint8_t cmd             = (uint8_t)((frame_data->cmd) & 0xff);

        g_iap_cmd_table[cmd](&(msg.meta), frame_data);
    }
}

proto_probe_sta_t iap_probe_frame(const ch_meta_t *meta, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *cmd_num)
{
    uint32_t available = rb_avail(buff, nullptr);
    (void)meta; // 鏈浣跨敤meta鍙傛暟

    // 鍩虹闀垮害妫鏌
    if (available < 4) return PROTO_PROBE_WAIT;

    // 甯уご妫鏌
    uint32_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 4, nullptr);
    if (head != FRAME_HEAD)
        return PROTO_PROBE_FAKE; // 甯уご涓嶅悎娉曪紝鏍囪颁负fake瑙﹀彂璺宠繃

    // 闀垮害鍚堟硶鎬у垵姝ユ鏌 (闃叉㈡伓鎰忓ぇ鍖呮垨浼閫犲寘)
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        rb_peek(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4, nullptr);

        if (payload_len > 256)
            return PROTO_PROBE_FAKE; // 鍗忚瑙勫畾鏈澶256锛岃秴杩囪寖鍥村垯涓轰吉閫犲抚澶

    } else {
        return PROTO_PROBE_WAIT; // 杩為暱搴﹀瓧娈甸兘杩樻病鏀跺埌
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    // 鏁版嵁瀹屾暣鎬ф鏌
    if (available < full_bytes) {
        // 濡傛灉鍦ㄥ綋鍓嶅崐鎴鍖呯殑鑼冨洿鍐呭彂鐜颁簡鏂板抚澶
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            rb_peek(buff, i, (uint8_t *)&next_head, 4, nullptr);

            if (next_head == FRAME_HEAD)
                return PROTO_PROBE_FAKE;
        }
        return PROTO_PROBE_WAIT;
    }

    // CRC楠岃瘉
    static uint32_t tmp_buff[FRAME_MAX_LEN];
    rb_peek(buff, 0, (uint8_t *)tmp_buff, full_bytes, nullptr);
    iap_frame_t *ptemp = (iap_frame_t *)tmp_buff;

    uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)ptemp, ptemp->len + 4);
    if (crc != ptemp->data_crc[ptemp->len]) {
        return PROTO_PROBE_FAKE; // 闀垮害澶熶簡浣嗘牎楠屼笉杩囷紝鍒ゅ畾涓轰吉閫犳垨鎹熷潖
    }

    // 楠岃瘉閫氳繃
    *total_len = full_bytes;
    *cmd_num   = ptemp->cmd & 0xFF;
    return PROTO_PROBE_READY;
}
