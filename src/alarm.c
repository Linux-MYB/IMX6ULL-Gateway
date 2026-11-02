/*
 * alarm.c —— 两级联锁判定 + 蜂鸣器
 *
 * 一级：主轴在转（PLC 寄存器 0 非 0）且 AI 检测到人员闯入
 *       → judge 线程收到一级后调用 modbus_write_reg 下发停机报文
 * 二级：温度超阈 或 烟雾超阈
 *       → 本地声光（蜂鸣器 + LCD）
 *
 * 阈值支持云端下发：upload 线程收到消息后调 alarm_set_thresholds()。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "gw.h"

static int  g_beep_fd = -1;
static int  g_beep_on = -1;                  /* 当前状态，避免重复写 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_th_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_temp_th  = GW_TEMP_TH;
static int   g_smoke_th = GW_SMOKE_TH;

void alarm_init(void)
{
    int v = 0;

    g_beep_fd = open(GW_BEEP_DEV, O_RDWR);
    if (g_beep_fd < 0) {
        printf("[alarm] %s 打不开（%s），告警只在界面和日志体现\n",
               GW_BEEP_DEV, strerror(errno));
    } else {
        ssize_t n;
        printf("[alarm] %s 已打开\n", GW_BEEP_DEV);
        n = write(g_beep_fd, &v, sizeof(v));
        (void)n;
        g_beep_on = 0;
    }
}

static void beep_set(int on)
{
    int v = on ? 1 : 0;

    if (g_beep_fd < 0 || g_beep_on == v) {
        return;
    }
    if (write(g_beep_fd, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
        g_beep_on = v;
    }
}

void alarm_set_thresholds(float temp_th, int smoke_th)
{
    pthread_mutex_lock(&g_th_lock);
    if (temp_th > 0.0f) {
        g_temp_th = temp_th;
    }
    if (smoke_th > 0) {
        g_smoke_th = smoke_th;
    }
    pthread_mutex_unlock(&g_th_lock);

    printf("[alarm] 阈值更新: 温度 %.1f℃，烟雾 %d\n", g_temp_th, g_smoke_th);
}

float alarm_temp_th(void)
{
    float v;
    pthread_mutex_lock(&g_th_lock);
    v = g_temp_th;
    pthread_mutex_unlock(&g_th_lock);
    return v;
}

int alarm_smoke_th(void)
{
    int v;
    pthread_mutex_lock(&g_th_lock);
    v = g_smoke_th;
    pthread_mutex_unlock(&g_th_lock);
    return v;
}

const char *alarm_level_name(int level)
{
    switch (level) {
    case 1:  return "L1-INTRUDE";
    case 2:  return "L2-FIRE";
    default: return "NORMAL";
    }
}

int alarm_judge(const struct gw_frame *f)
{
    int level = 0;
    int spindle_run = 0;
    float temp_th;
    int smoke_th;

    if (NULL == f) {
        return 0;
    }

    temp_th  = alarm_temp_th();
    smoke_th = alarm_smoke_th();

    /* 寄存器 0 约定：0 停机、非 0 表示主轴在转 */
    if (f->reg[0] != 0) {
        spindle_run = 1;
    }

    /* 一级：人员闯入且设备在运转，才需要停机 */
    if (f->can_ok && f->person && spindle_run) {
        level = 1;
    }

    /* 二级：火情风险，比闯入更严重，等级取大 */
    if (f->temp >= temp_th || f->smoke_raw >= smoke_th) {
        level = 2;
    }

    pthread_mutex_lock(&g_lock);
    beep_set(level > 0 ? 1 : 0);
    pthread_mutex_unlock(&g_lock);

    return level;
}

void alarm_stop(void)
{
    pthread_mutex_lock(&g_lock);
    beep_set(0);
    pthread_mutex_unlock(&g_lock);

    if (g_beep_fd >= 0) {
        close(g_beep_fd);
        g_beep_fd = -1;
        g_beep_on = -1;
    }
}

