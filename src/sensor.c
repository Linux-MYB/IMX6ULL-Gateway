/*
 * sensor.c —— 环境感知：DS18B20 温度 + MQ-2 烟雾
 *
 * 温度：优先用内核驱动 /dev/ds18b20_misc（read 一个 short，单位 0.0625℃）；
 *       打不开就退回到 w1 总线 sysfs（/sys/bus/w1/devices/28-xxxx/w1_slave）。
 * 烟雾：MQ-2 的 AO 接到 ADC，通过 IIO sysfs 读原始值，再按 scale 换算成 mV，
 *       并用标定系数折算成浓度估计值。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include "gw.h"

/* MQ-2 标定：洁净空气约 400mV，电压每升高 1mV 约等于 2ppm（按自家标定改） */
#define SMOKE_MV_BASE   400
#define SMOKE_PPM_PER_MV 2

static int g_temp_fd = -1;
static char g_w1_path[256] = {0};

static int find_w1_device(void)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/sys/bus/w1/devices");
    if (NULL == dir) {
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "28-", 3) == 0) {
            snprintf(g_w1_path, sizeof(g_w1_path),
                     "/sys/bus/w1/devices/%s/w1_slave", de->d_name);
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return -1;
}

int sensor_open(void)
{
    g_temp_fd = open(GW_DS18B20_DEV, O_RDWR);
    if (g_temp_fd < 0) {
        printf("[sensor] %s 打不开（%s），改读 w1 sysfs\n",
               GW_DS18B20_DEV, strerror(errno));
        if (find_w1_device() < 0) {
            printf("[sensor] 也没找到 w1 设备，温度将不可用\n");
        } else {
            printf("[sensor] 使用 %s\n", g_w1_path);
        }
    } else {
        printf("[sensor] %s 已打开\n", GW_DS18B20_DEV);
    }

    return 0;
}

int sensor_read_temp(float *temp)
{
    if (NULL == temp) {
        return -1;
    }

    if (g_temp_fd >= 0) {
        short raw = 0;
        ssize_t n = read(g_temp_fd, &raw, sizeof(raw));
        if (n != (ssize_t)sizeof(raw)) {
            return -1;
        }
        *temp = (float)raw * 0.0625f;
        return 0;
    }

    if (g_w1_path[0]) {
        FILE *fp = fopen(g_w1_path, "r");
        char line[256];
        int found = 0;

        if (NULL == fp) {
            return -1;
        }

        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "t=");
            if (p) {
                int milli = atoi(p + 2);
                *temp = (float)milli / 1000.0f;
                found = 1;
                break;
            }
        }
        fclose(fp);

        return found ? 0 : -1;
    }

    return -1;
}

int sensor_read_smoke(int *raw, int *mv)
{
    FILE *fp;
    int value = 0;

    if (NULL == raw || NULL == mv) {
        return -1;
    }

    fp = fopen(GW_ADC_RAW, "r");
    if (NULL == fp) {
        return -1;
    }
    if (fscanf(fp, "%d", &value) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* IIO 的 scale 是“每个 LSB 多少 mV” */
    {
        float scale = 0.805f;        /* 3.3V / 4096 的默认值，读到就覆盖 */
        fp = fopen(GW_ADC_SCALE, "r");
        if (fp) {
            if (fscanf(fp, "%f", &scale) != 1) {
                scale = 0.805f;
            }
            fclose(fp);
        }
        *mv = (int)((float)value * scale);
    }

    if (*mv < 0) {
        *mv = 0;
    }

    *raw = (*mv - SMOKE_MV_BASE) > 0 ? (*mv - SMOKE_MV_BASE) * SMOKE_PPM_PER_MV : 0;

    return 0;
}

void sensor_close(void)
{
    if (g_temp_fd >= 0) {
        close(g_temp_fd);
        g_temp_fd = -1;
    }
}

/* 采集线程：每秒读一次温度与烟雾 */
void *th_env(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg m;
    float temp = 0.0f;
    int raw = 0;
    int mv = 0;

    while (g_run) {
        memset(&m, 0, sizeof(m));
        m.type = GW_MSG_ENV;
        m.ts = (long)time(NULL);

        if (sensor_read_temp(&temp) == 0) {
            m.u.env.temp = temp;
        } else {
            m.u.env.temp = -99.0f;          /* 标记读数失败 */
        }

        if (sensor_read_smoke(&raw, &mv) == 0) {
            m.u.env.smoke_raw = raw;
            m.u.env.smoke_mv = mv;
        }

        gw_send(ctx->mbox, "judge", &m);
        usleep(GW_ENV_MS * 1000);
    }

    sensor_close();
    return NULL;
}

