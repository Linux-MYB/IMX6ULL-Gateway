#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gw.h"
#include "framebuffer.h"

static fb_t *g_fb = NULL;
static int   g_text_y = 0;

int ui_open(const char *fbdev)
{
    g_fb = framebuff_init(fbdev ? fbdev : GW_FB_DEV);
    if (NULL == g_fb) {
        printf("[ui] 打开 %s 失败，界面不可用\n", fbdev ? fbdev : GW_FB_DEV);
        return -1;
    }

    framebuffer_clearscreen(g_fb, 255, 255, 255);
    printf("[ui] 屏幕 %dx%d 已就绪\n",
           g_fb->vscreen.xres_virtual, g_fb->vscreen.yres_virtual);
    return 0;
}

static int line(const char *s, unsigned char r, unsigned char g, unsigned char b)
{
    int y = g_text_y;

    framebuffer_draw_str(g_fb, 20, y, (char *)s, r, g, b, 255, 255, 255);
    g_text_y += 20;
    return y;
}

void ui_draw(const struct gw_frame *f, int mqtt_online)
{
    char buf[128];
    unsigned char tr, tg, tb;

    if (NULL == g_fb || NULL == f) {
        return;
    }

    g_text_y = 20;
    framebuffer_clearscreen(g_fb, 255, 255, 255);

    line("CNC EDGE GATEWAY", 0x1F, 0x3B, 0x57);
    snprintf(buf, sizeof(buf), "UPTIME STAMP %ld", f->ts);
    line(buf, 0x66, 0x66, 0x66);

    if (f->level == 2) {
        tr = 0xC0; tg = 0x20; tb = 0x20;
    } else if (f->level == 1) {
        tr = 0xD0; tg = 0x90; tb = 0x00;
    } else {
        tr = 0x10; tg = 0x80; tb = 0x40;
    }

    framebuffer_fill_dict(g_fb, 16, g_text_y, 440, 24, tr, tg, tb);
    snprintf(buf, sizeof(buf), "  ALARM: %s", alarm_level_name(f->level));
    framebuffer_draw_str(g_fb, 20, g_text_y + 4, buf, 255, 255, 255, tr, tg, tb);
    g_text_y += 32;

    snprintf(buf, sizeof(buf), "PLC  reg0=%u reg1=%u reg2=%u reg3=%u",
             f->reg[0], f->reg[1], f->reg[2], f->reg[3]);
    line(buf, 0x1A, 0x1A, 0x1A);

    snprintf(buf, sizeof(buf), "TEMP %.2f C   (th %.1f)", f->temp, alarm_temp_th());
    line(buf, 0x1A, 0x1A, 0x1A);

    snprintf(buf, sizeof(buf), "SMOKE %d ppm  (th %d)  adc %d mV",
             f->smoke_raw, alarm_smoke_th(), f->smoke_mv);
    line(buf, 0x1A, 0x1A, 0x1A);

    snprintf(buf, sizeof(buf), "AI person=%d conf=%d node=%d",
             f->person, f->conf, f->node_state);
    line(buf, 0x1A, 0x1A, 0x1A);

    snprintf(buf, sizeof(buf), "LINK plc=%s can=%s mqtt=%s",
             f->plc_ok ? "OK" : "ERR",
             f->can_ok ? "OK" : "ERR",
             mqtt_online ? "OK" : "OFF");
    line(buf, 0x66, 0x66, 0x66);
}

void ui_close(void)
{
    if (g_fb) {
        framebuff_deinit(g_fb);
        g_fb = NULL;
    }
}

void *th_ui(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg msg;

    while (g_run) {
        if (gw_recv(ctx, &msg, 300) != 0) {
            continue;
        }
        if (msg.type != GW_MSG_FRAME) {
            continue;
        }
        ui_draw(&msg.u.frame, upload_online());
    }

    ui_close();
    return NULL;
}
