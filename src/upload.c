#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "gw.h"

#if GW_HAVE_MQTT
#include <MQTTClient.h>

static MQTTClient g_client = NULL;
static volatile int g_online = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void parse_threshold(const char *payload, int len)
{
    char buf[256];
    char *p;
    float temp_th = 0.0f;
    int smoke_th = 0;

    if (len <= 0 || len >= (int)sizeof(buf)) {
        return;
    }
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';

    p = strstr(buf, "temp_th");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            temp_th = (float)atof(p + 1);
        }
    }
    p = strstr(buf, "smoke_th");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            smoke_th = atoi(p + 1);
        }
    }

    if (temp_th <= 0.0f && smoke_th <= 0) {
        if (sscanf(buf, "%f,%d", &temp_th, &smoke_th) != 2) {
            printf("[upload] 阈值报文解析失败: %s\n", buf);
            return;
        }
    }

    alarm_set_thresholds(temp_th, smoke_th);
}

static int on_message(void *context, char *topic, int topic_len, MQTTClient_message *msg)
{
    (void)context;
    (void)topic_len;

    printf("[upload] 收到 %s\n", topic ? topic : "?");
    if (topic && strstr(topic, "threshold")) {
        parse_threshold((const char *)msg->payload, msg->payloadlen);
    }

    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}

static void on_disconnect(void *context, char *cause)
{
    (void)context;
    (void)cause;
    g_online = 0;
    printf("[upload] 连接断开: %s\n", cause ? cause : "?");
}

static void on_delivered(void *context, MQTTClient_deliveryToken dt)
{
    (void)context;
    (void)dt;
}

static int mqtt_try_connect(void)
{
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    if (g_client) {
        return g_online;
    }

    rc = MQTTClient_create(&g_client, GW_MQTT_URI, GW_MQTT_CLIENT,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[upload] create 失败 rc=%d\n", rc);
        g_client = NULL;
        return 0;
    }

    MQTTClient_setCallbacks(g_client, NULL, on_disconnect, on_message, on_delivered);

    opts.keepAliveInterval = GW_MQTT_KEEPALIVE;
    opts.cleansession = 1;

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[upload] connect 失败 rc=%d（3 秒后重试）\n", rc);
        MQTTClient_destroy(&g_client);
        g_client = NULL;
        return 0;
    }

    MQTTClient_subscribe(g_client, GW_MQTT_TOPIC_TH, 1);
    g_online = 1;
    printf("[upload] 已连接 %s，订阅 %s\n", GW_MQTT_URI, GW_MQTT_TOPIC_TH);
    return 1;
}

static int publish_frame(const struct gw_frame *f)
{
    char json[512];
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    if (!g_online || NULL == g_client || NULL == f) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{\"ts\":%ld,\"temp\":%.2f,\"smoke\":%d,\"smoke_mv\":%d,"
             "\"reg0\":%u,\"reg1\":%u,\"reg2\":%u,\"reg3\":%u,"
             "\"person\":%d,\"conf\":%d,\"node\":%d,\"level\":%d,"
             "\"plc_ok\":%d,\"can_ok\":%d}",
             f->ts, (double)f->temp, f->smoke_raw, f->smoke_mv,
             f->reg[0], f->reg[1], f->reg[2], f->reg[3],
             f->person, f->conf, f->node_state, f->level,
             f->plc_ok, f->can_ok);

    msg.qos = 1;
    msg.retained = 0;
    msg.payload = json;
    msg.payloadlen = (int)strlen(json);

    rc = MQTTClient_publishMessage(g_client, GW_MQTT_TOPIC_UP, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        g_online = 0;
        return -1;
    }

    MQTTClient_waitForCompletion(g_client, token, 2000);
    return 0;
}

static void flush_pending(void)
{
    struct gw_frame batch[16];
    long ids[16];
    int got = 0;
    int i;

    while (g_run && g_online) {
        if (store_fetch_pending(batch, ids, 16, &got) != 0 || got <= 0) {
            return;
        }
        for (i = 0; i < got; i++) {
            if (publish_frame(&batch[i]) != 0) {
                return;
            }
            store_mark_uploaded(ids[i]);
        }
        if (got < 16) {
            return;
        }
    }
}

int upload_online(void)
{
    return g_online;
}

#else

int upload_online(void)
{
    return 0;
}

#endif

void *th_upload(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg msg;
    time_t last_try = 0;

#if GW_HAVE_MQTT
    struct gw_frame latest;
    int have_latest = 0;
#endif

    while (g_run) {
        if (gw_recv(ctx, &msg, 200) == 0) {
#if GW_HAVE_MQTT
            if (msg.type == GW_MSG_FRAME) {
                latest = msg.u.frame;
                have_latest = 1;
            }
#endif
        }

#if GW_HAVE_MQTT
        if (!upload_online()) {
            if (time(NULL) - last_try >= 3) {
                last_try = time(NULL);
                mqtt_try_connect();
                if (upload_online()) {
                    flush_pending();
                }
            }
            continue;
        }

        if (have_latest) {
            publish_frame(&latest);
            have_latest = 0;
        }
#else
        (void)last_try;
        usleep(100 * 1000);
#endif
    }

#if GW_HAVE_MQTT
    if (g_client) {
        MQTTClient_disconnect(g_client, 1000);
        MQTTClient_destroy(&g_client);
        g_client = NULL;
        g_online = 0;
    }
#endif

    return NULL;
}
