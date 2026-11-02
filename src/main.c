#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "gw.h"

static volatile sig_atomic_t g_quit = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_quit = 1;
    g_run = 0;
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void *th_judge(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg m;
    struct gw_frame cur;
    long t_plc = 0, t_can = 0, t_env = 0;
    long last_emit = 0;
    long last_stop = 0;
    long frames = 0;

    memset(&cur, 0, sizeof(cur));

    while (g_run) {
        long nowm;
        long now_s;

        if (gw_recv(ctx, &m, 50) == 0) {
            switch (m.type) {
            case GW_MSG_PLC:
                memcpy(cur.reg, m.u.plc.reg, sizeof(cur.reg));
                cur.plc_ok = m.u.plc.ok;
                t_plc = now_ms();
                break;

            case GW_MSG_CAN:
                cur.person     = m.u.can.person;
                cur.conf       = m.u.can.conf;
                cur.node_state = m.u.can.node_state;
                cur.can_ok     = 1;
                t_can = now_ms();
                break;

            case GW_MSG_ENV:
                if (m.u.env.temp > -50.0f) {
                    cur.temp = m.u.env.temp;
                }
                cur.smoke_raw = m.u.env.smoke_raw;
                cur.smoke_mv  = m.u.env.smoke_mv;
                t_env = now_ms();
                break;

            default:
                break;
            }
        }

        nowm = now_ms();
        now_s = (long)time(NULL);

        if (nowm - t_plc > 3000) {
            cur.plc_ok = 0;
        }
        if (nowm - t_can > 3000) {
            cur.can_ok = 0;
            cur.person = 0;
        }
        if (nowm - t_env > 5000) {
            cur.smoke_raw = 0;
            cur.smoke_mv  = 0;
        }

        if (nowm - last_emit < GW_FRAME_MS) {
            continue;
        }
        last_emit = nowm;

        cur.ts = now_s;
        cur.level = alarm_judge(&cur);

        if (1 == cur.level && cur.plc_ok && (nowm - last_stop > 5000)) {
            last_stop = nowm;
            if (modbus_write_reg(GW_PLC_REG_STOP, 1) == 0) {
                printf("[judge] 已下发紧急停机报文（reg0x%04X=1）\n", GW_PLC_REG_STOP);
            } else {
                printf("[judge] 停机报文发送失败\n");
            }
        }

        memset(&m, 0, sizeof(m));
        m.type = GW_MSG_FRAME;
        m.ts = cur.ts;
        m.u.frame = cur;

        gw_send(ctx->mbox, "store", &m);
        gw_send(ctx->mbox, "upload", &m);
        gw_send(ctx->mbox, "ui", &m);

        frames++;
        if (0 == (frames % 20)) {
            printf("[judge] #%ld 温度 %.2f℃ 烟雾 %d ppm 闯入 %d 等级 %s PLC:%s CAN:%s\n",
                   frames, (double)cur.temp, cur.smoke_raw, cur.person,
                   alarm_level_name(cur.level),
                   cur.plc_ok ? "OK" : "ERR", cur.can_ok ? "OK" : "ERR");
        }
    }

    return NULL;
}

int main(void)
{
    gw_mbox_t *mbox = NULL;
    const char *serial_dev = getenv("GW_SERIAL");
    const char *can_if     = getenv("GW_CAN");
    const char *db_path    = getenv("GW_DB");
    const char *fb_dev     = getenv("GW_FB");

    if (NULL == serial_dev) serial_dev = GW_SERIAL_DEV;
    if (NULL == can_if)     can_if     = GW_CAN_IF;
    if (NULL == db_path)    db_path    = GW_DB_PATH;
    if (NULL == fb_dev)     fb_dev     = GW_FB_DEV;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    printf("==================== 机床工位边缘安全采集网关 ====================\n");
    printf(" PLC: %s @%d 8N1  从站 %d\n", serial_dev, GW_SERIAL_BAUD, GW_PLC_SLAVE);
    printf(" CAN: %s   AI 节点 %d/%d/%d\n", can_if,
           GW_CAN_ID_PERSON, GW_CAN_ID_CONF, GW_CAN_ID_NODE);
    printf(" 数据库: %s  环形表上限 %d 条\n", db_path, GW_RING_MAX);
    printf(" 温度阈值 %.1f℃  烟雾阈值 %d ppm\n", GW_TEMP_TH, GW_SMOKE_TH);
    printf(" Ctrl+C 退出\n");
    printf("=================================================================\n");

    if (store_open(db_path) != 0) {
        printf("数据库打不开，退出\n");
        return 1;
    }

    alarm_init();
    sensor_open();

    if (modbus_open(serial_dev, GW_SERIAL_BAUD, GW_PLC_SLAVE) != 0) {
        printf("警告：PLC 串口打不开，plc_ok 会一直是 0\n");
    }
    if (can_open(can_if) != 0) {
        printf("警告：CAN 打不开，can_ok 会一直是 0\n");
    }
    if (ui_open(fb_dev) != 0) {
        printf("警告：LCD 打不开，只输出到串口\n");
    }

    mbox = gw_mbox_create();
    if (NULL == mbox) {
        printf("邮箱创建失败\n");
        return 1;
    }

    gw_mbox_add(mbox, "judge",  th_judge);
    gw_mbox_add(mbox, "store",  th_store);
    gw_mbox_add(mbox, "upload", th_upload);
    gw_mbox_add(mbox, "ui",     th_ui);
    gw_mbox_add(mbox, "modbus", th_modbus);
    gw_mbox_add(mbox, "can",    th_can);
    gw_mbox_add(mbox, "env",    th_env);

    printf("已启动 7 个线程，开始采集...\n");

    while (!g_quit && g_run) {
        sleep(1);
    }

    printf("\n准备退出：通知各线程...\n");
    gw_mbox_stop(mbox);
    gw_mbox_destroy(mbox);

    modbus_close();
    can_close();
    alarm_stop();

    printf("网关已退出\n");
    return 0;
}
