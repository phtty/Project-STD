#include "mqtt_app.h"

#include "lwip.h"
#include "lwip/sys.h"
#include "lwip/apps/mqtt.h"

#include "protocol.h"
#include "ah_mqtt.h"

// Broker 鏈嶅姟鍣 IP
#define BROKER_IP_ADDR "120.46.136.199"
#define BROKER_PORT    6000

#define CLIENT_NAME    "CD_ZTP"
#define CLIENT_USER    "pxh"
#define CLIENT_PASS    "123456"

static mqtt_client_t *mqtt_client;                         // MQTT 瀹㈡埛绔鎺у埗鍧
static struct mqtt_connect_client_info_t mqtt_client_info; // 杩炴帴淇℃伅
static uint32_t current_payload_offset = 0;                // mqtt鏁版嵁鎺ユ敹缂撳啿鍖哄亸绉昏℃暟
static uint8_t mqtt_rcv_buf[1024];                         // mqtt鏁版嵁鎺ユ敹缂撳啿鍖
volatile mqtt_StateMachine mqtt_state = no_connect;        // mqtt杩炴帴鐘舵佺姸鎬佹満
osSemaphoreId_t mqttConnSemHandle;                         // 鏂寮杩炴帴鏍囧織淇″彿閲
osMessageQueueId_t xMessageLenQueue;                       // 娑堟伅鐨勯暱搴

osThreadId_t mqttManageTaskHandle;
const osThreadAttr_t mqttManageTask_attributes = {
    .name       = "mqttManageTask",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/**
 * @brief 鏀跺埌publish甯х殑鍥炶皟
 *
 * @param arg
 * @param topic
 * @param tot_len
 */
static void mqtt_incoming_publish_cb(void *arg, const char *topic, uint32_t tot_len)
{
    // 閲嶇疆鍋忕Щ閲忥紝鍑嗗囨帴鏀舵柊鍖
    current_payload_offset = 0;

    // 灏唗opic鏀惧叆鍏冩暟鎹
    ch_meta_t *meta = (ch_meta_t *)arg;
    strcpy(meta->handle.mqtt.topic, topic);
}

// 璁㈤槄鎴愬姛鐨勫洖璋
static void mqtt_sub_request_cb(void *arg, err_t result)
{
    printf("subscribe result: %d", (int)result);
}

/**
 * @brief 杩炴帴鍥炶皟
 *
 * @param client
 * @param arg
 * @param status
 */
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED) {
        mqtt_state = connected;

    } else {
        mqtt_state = no_connect;
    }

    osSemaphoreRelease(mqttConnSemHandle); // 閲婃斁淇″彿閲忛氱煡杩炴帴绠＄悊浠诲姟
}

/**
 * @brief 澶勭悊Payload鏁版嵁鍥炶皟(鍙鑳藉垎澶氭¤皟鐢)
 *
 * @param arg
 * @param data 鏁版嵁鎸囬拡
 * @param len 鏈娆￠暱搴
 * @param flags MQTT_DATA_FLAG_LAST琛ㄧず杩欐槸鏈鍚庝竴鐗
 */
static void mqtt_incoming_data_cb(void *arg, const uint8_t *data, uint16_t len, uint8_t flags)
{
    // 闃叉㈢紦鍐插尯婧㈠嚭
    if (current_payload_offset + len < sizeof(mqtt_rcv_buf)) {
        memcpy(&mqtt_rcv_buf[current_payload_offset], data, len);
        current_payload_offset += len;
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        // 鍔燶0锛屾柟渚挎搷浣滃瓧绗︿覆
        if (current_payload_offset < sizeof(mqtt_rcv_buf)) {
            mqtt_rcv_buf[current_payload_offset] = '\0';
            len++;
        }

        // 涓寸晫淇濇姢鍖猴紝闃叉㈠氫釜淇￠亾鍚屾椂鍐檙ingbuff

        // 灏嗘暟鎹鏀惧叆鐜褰㈢紦鍐插尯
        rb_write(&g_proto_rb, (uint8_t *)mqtt_rcv_buf, len, g_proto_rb.mutex);

        // 鍏冩暟鎹鍏ラ槦
        ch_meta_t *meta               = (ch_meta_t *)arg;
        meta->handle.mqtt.payload_len = len;
        osMessageQueuePut(g_meta_queue, meta, 0, 100);

    }
}

void mqtt_connection(void)
{
    ip_addr_t broker_ip;
    err_t err;

    if (mqtt_state == connecting)
        return;

    if (mqtt_client != NULL) {
        LOCK_TCPIP_CORE();
        mqtt_disconnect(mqtt_client);
        UNLOCK_TCPIP_CORE(); // 瑙ｉ攣
        mqtt_client_free(mqtt_client);
        mqtt_client = NULL;
    }

    mqtt_client = mqtt_client_new();
    if (mqtt_client == NULL) {
        return;
    }

    ipaddr_aton(BROKER_IP_ADDR, &broker_ip);

    memset(&mqtt_client_info, 0, sizeof(mqtt_client_info));
    mqtt_client_info.client_id   = CLIENT_NAME;
    mqtt_client_info.keep_alive  = 60;          // 60s 蹇冭烦
    mqtt_client_info.client_user = CLIENT_USER; // 鐢ㄦ埛鍚嶅拰瀵嗙爜
    mqtt_client_info.client_pass = CLIENT_PASS;

    LOCK_TCPIP_CORE();
    err = mqtt_client_connect(mqtt_client, &broker_ip, BROKER_PORT, mqtt_connection_cb, NULL, &mqtt_client_info);
    UNLOCK_TCPIP_CORE(); // 瑙ｉ攣

    // 鍒涘缓鏁版嵁鎺ユ敹鍥炶皟
    static ch_meta_t meta = {
        .type     = CH_TYPE_MQTT,
        .protocol = PROTO_MASK_AH_MQTT,
    };
    mqtt_set_inpub_callback(mqtt_client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, (void *)&meta);
    if (err == ERR_OK) {
        mqtt_state = connecting;

    } else { // 杩炴帴鍙戣捣澶辫触锛岄噴鏀捐祫婧
        mqtt_state = no_connect;
        mqtt_client_free(mqtt_client);
        mqtt_client = NULL;
    }
}

/**
 * @brief 鍙戦佺‘璁ゅ洖璋冿紝浼氭牴鎹甉oS鏈変笉鍚岀殑璋冪敤鏈哄埗
 *
 * @param arg
 * @param result
 */
static void mqtt_pub_request_cb(void *arg, err_t result)
{
    NULL;
}

/**
 * @brief 閫氳繃mqtt鍙戝竷娑堟伅
 *
 * @param topic 瑕佸彂甯冪殑topic
 * @param message 鍙戝竷鍐呭
 */
void mqtt_send_data(const char *topic, const char *message)
{
    err_t err;

    // 鍔犱簰鏂ラ攣闃叉㈠拰lwip鐨勪换鍔¤祫婧愮珵浜
    LOCK_TCPIP_CORE();

    // qos 1, retain 0
    if (mqtt_client != NULL && mqtt_client_is_connected(mqtt_client))
        err = mqtt_publish(mqtt_client, topic, message, strlen(message), 1, 0, mqtt_pub_request_cb, NULL);
    else
        err = ERR_CLSD;

    UNLOCK_TCPIP_CORE(); // 瑙ｉ攣

    if (err != ERR_OK) {
        // 閿欒澶勭悊
    }
}

void mqtt_sub_set(char *topic_cmd)
{
    char topic_name[64] = {0};

    // 鎷兼帴瑕佽㈤槄鐨則opic瀛楃︿覆
    snprintf(topic_name, sizeof(topic_name), "%.8s/%.2s/%.2s/%.2s/%s",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id,
             topic_cmd);

    // 璁㈤槄鎿嶄綔鍔犱簰鏂ラ攣
    LOCK_TCPIP_CORE();
    mqtt_subscribe(mqtt_client, topic_name, 1, mqtt_sub_request_cb, NULL);
    UNLOCK_TCPIP_CORE();
}

void mqttManageTask(void *argument)
{
    // 鍒涘缓闀垮害闃熷垪锛岀敤浜庨氱煡鍗忚澶勭悊浠诲姟娑堟伅闀垮害
    xMessageLenQueue = osMessageQueueNew(16, sizeof(uint16_t), NULL);
    // 鍒涘缓杩炴帴鐘舵佷俊鍙烽噺锛岄氱煡杩炴帴绠＄悊浠诲姟
    mqttConnSemHandle = osSemaphoreNew(1, 0, NULL);

    for (;;) {
        // 绛夊緟缃戠粶鎺ュ彛灏辩华
        if (!(netif_is_up(netif_default) && netif_is_link_up(netif_default))) {
            osDelay(100);
            continue;
        }

        if (mqtt_state == connected) {
            // 杩炴帴鎴愬姛鍚庯紝寮濮嬭㈤槄鎵鏈夊懡浠ょ殑topic
            mqtt_sub_set("ASK/board/NULL");
            mqtt_sub_set("ASK/display/clean");
            mqtt_sub_set("ASK/op/restart");
            mqtt_sub_set("ASK/op/checktime");

            // 杩炴帴姝ｅ父锛岄樆濉炵瓑寰呮柇寮浜嬩欢
            osSemaphoreAcquire(mqttConnSemHandle, osWaitForever);

        } else {
            mqtt_connection();
            // 绛夊緟杩炴帴缁撴灉锛5s瓒呮椂
            osSemaphoreAcquire(mqttConnSemHandle, 5000);
            if (mqtt_state != connected) {
                // 杩炴帴澶辫触锛岄噸杩
                mqtt_state = no_connect;
            }
        }
    }
}