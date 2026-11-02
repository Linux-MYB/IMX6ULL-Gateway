/*
 * gw.h —— 机床工位边缘安全采集网关：公共定义
 *
 * 线程模型：
 *   采集(modbus / can / env) → 判定(judge) → 存储(store) / 上云(upload) / 显示(ui)
 *   线程之间只通过邮箱队列传消息，不共享全局变量（除了退出标志）。
 */
#ifndef __GW_H__
#define __GW_H__

#include <pthread.h>
#include <time.h>

/* ---------------- 运行参数（按现场改这里） ---------------- */
#define GW_SERIAL_DEV      "/dev/ttymxc2"        /* RS485 接 PLC 的串口 */
#define GW_SERIAL_BAUD     9600
#define GW_PLC_SLAVE       1                     /* PLC 从站地址 */
#define GW_PLC_REG_START   0x0000                /* 4 个工艺寄存器起始地址 */
#define GW_PLC_REG_COUNT   4
#define GW_PLC_REG_STOP    0x0010                /* 停机控制寄存器 */
#define GW_POLL_MS         100                   /* PLC 轮询周期 */

#define GW_CAN_IF          "can0"
#define GW_CAN_ID_PERSON   0x100                 /* 人体状态 */
#define GW_CAN_ID_CONF     0x102                 /* 置信度 */
#define GW_CAN_ID_NODE     0x103                 /* 节点状态 */

#define GW_DS18B20_DEV     "/dev/ds18b20_misc"   /* 内核 misc 驱动 */
#define GW_W1_GLOB         "/sys/bus/w1/devices/28-*"
#define GW_ADC_RAW         "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define GW_ADC_SCALE       "/sys/bus/iio/devices/iio:device0/in_voltage_scale"
#define GW_BEEP_DEV        "/dev/beep_misc"
#define GW_FB_DEV          "/dev/fb0"

#define GW_DB_PATH         "/root/gateway.db"
#define GW_RING_MAX        4800                  /* 环形表保留条数 */

#define GW_TEMP_TH         45.0f                 /* 二级：温度阈值 */
#define GW_SMOKE_TH        600                   /* 二级：烟雾原始值阈值 */

#define GW_ENV_MS          1000                  /* 环境采样周期 */
#define GW_FRAME_MS        500                   /* 组帧/落库/上报周期 */

/* MQTT（可选：编译时加 -DGW_HAVE_MQTT=1 并链接 paho） */
#define GW_MQTT_URI        "tcp://127.0.0.1:1883"
#define GW_MQTT_CLIENT     "imx6ull-gateway"
#define GW_MQTT_TOPIC_UP   "gw/workshop01/data"
#define GW_MQTT_TOPIC_TH   "gw/workshop01/threshold"
#define GW_MQTT_KEEPALIVE  20

/* ---------------- 消息类型 ---------------- */
enum gw_msg_type {
    GW_MSG_PLC = 1,     /* 采集：PLC 寄存器 */
    GW_MSG_CAN,         /* 采集：AI 节点上报 */
    GW_MSG_ENV,         /* 采集：温度/烟雾 */
    GW_MSG_FRAME,       /* 判定：合成后的完整帧 */
    GW_MSG_THRESH       /* 云端下发的阈值 */
};

struct gw_plc {
    unsigned short reg[GW_PLC_REG_COUNT];
    int ok;
};

struct gw_can {
    int person;         /* 1 = 检测到人员闯入 */
    int conf;           /* 置信度 0-100 */
    int node_state;     /* 节点状态：0 正常，1 故障 */
};

struct gw_env {
    float temp;
    int smoke_raw;
    int smoke_mv;
};

/* 判定后的一帧，存储、上报、显示都用它 */
struct gw_frame {
    long ts;                              /* 秒级时间戳 */
    unsigned short reg[GW_PLC_REG_COUNT]; /* 工艺参数 */
    int person;
    int conf;
    int node_state;
    float temp;
    int smoke_raw;
    int smoke_mv;
    int level;                            /* 0 正常 / 1 一级 / 2 二级 */
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

/* 退出标志：Ctrl+C 置 0，各线程检查后退出 */
extern volatile int g_run;

/* ---------------- 邮箱（线程安全队列） ---------------- */
typedef struct gw_queue gw_queue_t;
typedef struct gw_mbox gw_mbox_t;

/* 一个线程的上下文：自己的队列 + 邮箱句柄 */
struct gw_ctx {
    gw_mbox_t *mbox;
    gw_queue_t *self;      /* 自己的收件队列 */
    char name[32];
};

gw_mbox_t *gw_mbox_create(void);
int  gw_mbox_add(gw_mbox_t *mb, const char *name, void *(*fn)(void *));
int  gw_send(gw_mbox_t *mb, const char *dst, const struct gw_msg *msg);
int  gw_recv(struct gw_ctx *ctx, struct gw_msg *msg, int timeout_ms);
void gw_mbox_stop(gw_mbox_t *mb);          /* 唤醒所有等待，让线程退出 */
void gw_mbox_destroy(gw_mbox_t *mb);

/* ---------------- 各模块 ---------------- */
/* modbus.c */
int  modbus_open(const char *dev, int baud, int slave);
int  modbus_read_regs(unsigned short start, unsigned short count, unsigned short *out);
int  modbus_write_reg(unsigned short reg, unsigned short val);
void modbus_close(void);

/* canbus.c */
int  can_open(const char *ifname);
int  can_read_msg(struct gw_can *out, int timeout_ms);
void can_close(void);

/* sensor.c */
int  sensor_open(void);
int  sensor_read_temp(float *temp);
int  sensor_read_smoke(int *raw, int *mv);
void sensor_close(void);

/* store.c */
int  store_open(const char *path);
int  store_insert(const struct gw_frame *f);         /* 返回 1 新增，0 失败 */
int  store_fetch_pending(struct gw_frame *out, long *ids, int max, int *got);
int  store_mark_uploaded(long id);
void store_close(void);

/* alarm.c：两级判定 + 阈值（云端可下发） */
void alarm_init(void);
int  alarm_judge(const struct gw_frame *f);          /* 返回等级，并驱动蜂鸣器 */
void alarm_stop(void);
const char *alarm_level_name(int level);
void  alarm_set_thresholds(float temp_th, int smoke_th);
float alarm_temp_th(void);
int   alarm_smoke_th(void);

/* ui.c */
int  ui_open(const char *fbdev);
void ui_draw(const struct gw_frame *f, int mqtt_online);
void ui_close(void);

/* upload.c：MQTT 上报与阈值订阅 */
int  upload_online(void);

/* ---------------- 线程函数（main 里注册到邮箱） ---------------- */
void *th_modbus(void *arg);
void *th_can(void *arg);
void *th_env(void *arg);
void *th_judge(void *arg);
void *th_store(void *arg);
void *th_upload(void *arg);
void *th_ui(void *arg);

#endif /* __GW_H__ */


