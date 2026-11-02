#ifndef __GW_H__
#define __GW_H__

#include <pthread.h>
#include <time.h>

#define GW_SERIAL_DEV      "/dev/ttymxc2"
#define GW_SERIAL_BAUD     9600
#define GW_PLC_SLAVE       1
#define GW_PLC_REG_START   0x0000
#define GW_PLC_REG_COUNT   4
#define GW_PLC_REG_STOP    0x0010
#define GW_POLL_MS         100

#define GW_CAN_IF          "can0"
#define GW_CAN_ID_PERSON   0x100
#define GW_CAN_ID_CONF     0x102
#define GW_CAN_ID_NODE     0x103

#define GW_DS18B20_DEV     "/dev/ds18b20_misc"
#define GW_W1_GLOB         "/sys/bus/w1/devices/28-*"
#define GW_ADC_RAW         "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define GW_ADC_SCALE       "/sys/bus/iio/devices/iio:device0/in_voltage_scale"
#define GW_BEEP_DEV        "/dev/beep_misc"
#define GW_FB_DEV          "/dev/fb0"

#define GW_DB_PATH         "/root/gateway.db"
#define GW_RING_MAX        4800

#define GW_TEMP_TH         45.0f
#define GW_SMOKE_TH        600

#define GW_ENV_MS          1000
#define GW_FRAME_MS        500

#define GW_MQTT_URI        "tcp://127.0.0.1:1883"
#define GW_MQTT_CLIENT     "imx6ull-gateway"
#define GW_MQTT_TOPIC_UP   "gw/workshop01/data"
#define GW_MQTT_TOPIC_TH   "gw/workshop01/threshold"
#define GW_MQTT_KEEPALIVE  20

enum gw_msg_type {
    GW_MSG_PLC = 1,
    GW_MSG_CAN,
    GW_MSG_ENV,
    GW_MSG_FRAME,
    GW_MSG_THRESH
};

struct gw_plc {
    unsigned short reg[GW_PLC_REG_COUNT];
    int ok;
};

struct gw_can {
    int person;
    int conf;
    int node_state;
};

struct gw_env {
    float temp;
    int smoke_raw;
    int smoke_mv;
};

struct gw_frame {
    long ts;
    unsigned short reg[GW_PLC_REG_COUNT];
    int person;
    int conf;
    int node_state;
    float temp;
    int smoke_raw;
    int smoke_mv;
    int level;
    int plc_ok;
    int can_ok;
};

struct gw_msg {
    int type;
    long ts;
    union {
        struct gw_plc plc;
        struct gw_can can;
        struct gw_env env;
        struct gw_frame frame;
        struct { float temp_th; int smoke_th; } thresh;
    } u;
};

extern volatile int g_run;

typedef struct gw_queue gw_queue_t;
typedef struct gw_mbox gw_mbox_t;

struct gw_ctx {
    gw_mbox_t *mbox;
    gw_queue_t *self;
    char name[32];
};

gw_mbox_t *gw_mbox_create(void);
int  gw_mbox_add(gw_mbox_t *mb, const char *name, void *(*fn)(void *));
int  gw_send(gw_mbox_t *mb, const char *dst, const struct gw_msg *msg);
int  gw_recv(struct gw_ctx *ctx, struct gw_msg *msg, int timeout_ms);
void gw_mbox_stop(gw_mbox_t *mb);
void gw_mbox_destroy(gw_mbox_t *mb);

int  modbus_open(const char *dev, int baud, int slave);
int  modbus_read_regs(unsigned short start, unsigned short count, unsigned short *out);
int  modbus_write_reg(unsigned short reg, unsigned short val);
void modbus_close(void);

int  can_open(const char *ifname);
int  can_read_msg(struct gw_can *out, int timeout_ms);
void can_close(void);

int  sensor_open(void);
int  sensor_read_temp(float *temp);
int  sensor_read_smoke(int *raw, int *mv);
void sensor_close(void);

int  store_open(const char *path);
int  store_insert(const struct gw_frame *f);
int  store_fetch_pending(struct gw_frame *out, long *ids, int max, int *got);
int  store_mark_uploaded(long id);
void store_close(void);

void alarm_init(void);
int  alarm_judge(const struct gw_frame *f);
void alarm_stop(void);
const char *alarm_level_name(int level);
void  alarm_set_thresholds(float temp_th, int smoke_th);
float alarm_temp_th(void);
int   alarm_smoke_th(void);

int  ui_open(const char *fbdev);
void ui_draw(const struct gw_frame *f, int mqtt_online);
void ui_close(void);

int  upload_online(void);

void *th_modbus(void *arg);
void *th_can(void *arg);
void *th_env(void *arg);
void *th_judge(void *arg);
void *th_store(void *arg);
void *th_upload(void *arg);
void *th_ui(void *arg);

#endif
