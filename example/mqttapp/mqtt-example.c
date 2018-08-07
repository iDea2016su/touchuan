/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iot_import.h"
#include "iot_export.h"
#include "iot_export_mqtt.h"
#include "aos/log.h"
#include "aos/yloop.h"
#include "aos/network.h"
#include <netmgr.h>
#include <aos/kernel.h>
#include <k_err.h>
#include <netmgr.h>
#include <aos/cli.h>
#include <aos/cloud.h>

#include "soc_init.h"

#ifdef AOS_ATCMD
#include <atparser.h>
#endif

typedef struct {
    char productKey[16];
    char deviceName[32];
    char deviceSecret[48];

    int max_msg_size;
    int max_msgq_size;
    int connected;
    int (*event_handler)(int event_type, void *ctx);
    int (*delete_subdev)(char *productKey, char *deviceName, void *ctx);
    void *ctx;
} MqttContext;

// These are pre-defined topics
#define TOPIC_UPDATE            "/"PRODUCT_KEY"/"DEVICE_NAME"/update"
#define TOPIC_ERROR             "/"PRODUCT_KEY"/"DEVICE_NAME"/update/error"
#define TOPIC_GET               "/"PRODUCT_KEY"/"DEVICE_NAME"/get"

#define ALINK_BODY_FORMAT         "{\"id\":\"%d\",\"version\":\"1.0\",\"method\":\"%s\",\"params\":%s}"

#define RAW_TOPIC_PROP_UP         "/sys/"PRODUCT_KEY"/"DEVICE_NAME"/thing/model/up_raw"
#define RAW_TOPIC_PROP_DOWN       "/sys/"PRODUCT_KEY"/"DEVICE_NAME"/thing/model/down_raw"

#define ALINK_METHOD_PROP_POST    "thing.event.property.post"
#define ALINK_METHOD_EVENT_POST    "thing.event.Alarm.post"

#define MSG_LEN_MAX             (2048)

#define FRE_KEY_ADDR  "led_fre"
#define FRE_BUF_LEN   (64)
static int  led_fre = 3; 

#define TEMP_KEY_ADDR  "temp_val"
#define TEMP_BUF_LEN   (64)
static int  temp_val = 320; 
static int  alarm_status = 0; 
static int led_fre_upload_status =1;

static int alarm_clear = 0;

void set_led_fre(int p_fre);
int get_led_fre(void);

static void app_delayed_action(void *arg)
{
    static int count =0;
    int fre_count = get_led_fre();
    count++;
    if(count>=(10/fre_count))
    {

         hal_gpio_output_toggle(&brd_gpio_table[8]);
        count = 0;
    }
    aos_post_delayed_action(100, app_delayed_action, NULL);
}
int cnt = 0;
static int is_subscribed = 0;

#ifdef MQTT_PRESS_TEST
static int sub_counter = 0;
static int pub_counter = 0;
#endif
char msg_pub[512];
static int fd_temp  = -1;

static int sensor_all_open(void)
{
    int fd = -1;
    fd = aos_open(dev_temp_path, O_RDWR);
    if (fd < 0) {
        printf("Error: aos_open return %d.\n", fd);
        return -1;
    }
    fd_temp = fd;
    printf("temp socket  %d\n", fd);
    return 0;
}

static int get_temp_data(int *x)
{
    temperature_data_t temp = {0};
    ssize_t size = 0;
    size = aos_read(fd_temp, &temp, sizeof(temp));
    if (size != sizeof(temp)) {
        printf("aos_read return error.\n");
        return -1;
    }
    *x = temp.t;
    return 0;
}

static int t_count = 20;
static void ota_init(void *pclient);
int mqtt_client_example(void);
static void wifi_service_event(input_event_t *event, void *priv_data)
{
    if (event->type != EV_WIFI) {
        return;
    }

    if (event->code != CODE_WIFI_ON_GOT_IP) {
        return;
    }
    LOG("wifi_service_event!");
    mqtt_client_example();
}

static void mqtt_sub_callback(char *topic, int topic_len, void *payload, int payload_len, void *ctx)
{
    LOG("----");
    LOG("Topic: '%.*s' (Length: %d)",
        topic_len,
        topic,
        topic_len);
    LOG("Payload: '%.*s' (Length: %d)",
        payload_len,
        (unsigned char *)payload,
        payload_len);
    LOG("----");
    unsigned char receive_buf[30]={0};
    for(int i=0;i<payload_len;i++)
    {
        receive_buf[i] = ((unsigned char *)payload)[i];
    }
    printf("receive message : ");
    for(int i=0;i<9;i++)
    {
        printf("%x",receive_buf[i]);
    }
    printf("\n");
    int rec_temp = ((int)receive_buf[5])*10+(int)receive_buf[6];
    int rec_fre = ((int)receive_buf[7])*10+(int)receive_buf[8];
    printf("rec_temp %d\n",rec_temp);
    printf("rec_fre %d\n",rec_fre);
    if(rec_fre>0)
    {
        set_led_fre(rec_fre);
    }  
}

static void mqtt_work(void *parms)
{

    int rc = -1;
    if (is_subscribed == 0) 
    {
        /* Subscribe the specific topic */
        rc = mqtt_subscribe(RAW_TOPIC_PROP_DOWN, mqtt_sub_callback, NULL);
        if (rc < 0) {
            // IOT_MQTT_Destroy(&pclient);
            LOG("IOT_MQTT_Subscribe() failed, rc = %d", rc);
        }
        is_subscribed = 1;
    }
    else
    {
        /* Generate topic message */
        memset(msg_pub, 0, sizeof(msg_pub));
        int temperature =0;
        get_temp_data(&temperature);        
        int frequency = get_led_fre();
        int temp = temperature/10; 
        int frequency_h = frequency/256;
        int frequency_l = frequency%256;
        int temp_h = temp/256;
        int temp_l = temp%256;
        LOG("temperature : %d\n",temp);
        LOG("frequency :   %d\n",frequency);
        {
            unsigned char p_buf[10]={0};
            p_buf[0]=0;
            p_buf[1]=cnt/1000;
            p_buf[2]=(cnt/100)%10;
            p_buf[3]=(cnt/10)%10;
            p_buf[4]=cnt%10;
            p_buf[5]=temp_h;
            p_buf[6]=temp_l;
            p_buf[7]=frequency_h;
            p_buf[8]=frequency_l;
            rc = mqtt_publish(RAW_TOPIC_PROP_UP, IOTX_MQTT_QOS1, p_buf, 9);
            if (rc < 0) LOG("error occur when publish");
            printf("send message : ");
            for(int i=0;i<9;i++)
            {
                printf("%x",p_buf[i]);
            }
            printf("\n");
        }
        LOG("system is running %d\n",cnt);
    }
    cnt++;
    if (cnt < 200) {
        aos_post_delayed_action(3000, mqtt_work, NULL);
    } else {
        aos_cancel_delayed_action(3000, mqtt_work, NULL);
        mqtt_unsubscribe(TOPIC_GET);
        aos_msleep(200);
        mqtt_deinit_instance();
        is_subscribed = 0;
        cnt = 0;
    }
}



static void mqtt_service_event(input_event_t *event, void *priv_data)
{

    if (event->type != EV_SYS) {
        return;
    }
    if (event->code != CODE_SYS_ON_MQTT_READ) {
        return;
    }
    LOG("mqtt_service_event!");
    mqtt_work(NULL);
}

static int smartled_event_handler(int event_type, void *ctx)
{
    LOG("event_type %d\n", event_type);
    switch (event_type) {
        default:
            break;
    }
    return 0;
}

static MqttContext mqtt;

int mqtt_client_example(void)
{
    memset(&mqtt, 0, sizeof(MqttContext));

    strncpy(mqtt.productKey,   PRODUCT_KEY,   sizeof(mqtt.productKey)   - 1);
    strncpy(mqtt.deviceName,   DEVICE_NAME,   sizeof(mqtt.deviceName)   - 1);
    strncpy(mqtt.deviceSecret, DEVICE_SECRET, sizeof(mqtt.deviceSecret) - 1);

    mqtt.max_msg_size = MSG_LEN_MAX;
    mqtt.max_msgq_size = 8;

    mqtt.event_handler = smartled_event_handler;
    mqtt.delete_subdev = NULL;
    if (mqtt_init_instance(mqtt.productKey, mqtt.deviceName, mqtt.deviceSecret, mqtt.max_msg_size) < 0) {
        LOG("mqtt_init_instance failed\n");
        return -1;
    }
    aos_register_event_filter(EV_SYS,  mqtt_service_event, NULL);

    return 0;

}

static void handle_mqtt(char *pwbuf, int blen, int argc, char **argv)
{
    mqtt_client_example();
}

static struct cli_command mqttcmd = {
    .name = "mqtt",
    .help = "factory mqtt",
    .function = handle_mqtt
};

#ifdef AOS_ATCMD
static void at_uart_configure(uart_dev_t *u)
{
    u->port                = AT_UART_PORT;
    u->config.baud_rate    = AT_UART_BAUDRATE;
    u->config.data_width   = AT_UART_DATA_WIDTH;
    u->config.parity       = AT_UART_PARITY;
    u->config.stop_bits    = AT_UART_STOP_BITS;
    u->config.flow_control = AT_UART_FLOW_CONTROL;
}
#endif



int application_start(int argc, char *argv[])
{
#if AOS_ATCMD
    at.set_mode(ASYN);
    at.init(AT_RECV_PREFIX, AT_RECV_SUCCESS_POSTFIX,
            AT_RECV_FAIL_POSTFIX, AT_SEND_DELIMITER, 1000);
#endif


#ifdef WITH_SAL
    sal_init();
#elif defined (CSP_LINUXHOST)
    aos_post_event(EV_WIFI, CODE_WIFI_ON_GOT_IP, 0u);
#endif

    aos_set_log_level(AOS_LL_DEBUG);
    sensor_all_open();
    aos_register_event_filter(EV_WIFI, wifi_service_event, NULL);

    netmgr_init();
    netmgr_start(false);

    aos_cli_register_command(&mqttcmd);

    aos_post_delayed_action(100, app_delayed_action, NULL);

    aos_loop_run();
    return 0;
}

static void ota_init(void *P)
{
    aos_post_event(EV_SYS, CODE_SYS_ON_START_FOTA, 0u);
}

void set_led_fre(int p_fre)
{
    led_fre = p_fre;
}

int get_led_fre(void)
{
    return led_fre;
}

