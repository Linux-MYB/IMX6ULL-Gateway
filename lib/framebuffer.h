#ifndef __FRAMEBUFFER_H__
#define __FRAMEBUFFER_H__

#include <linux/fb.h>

#define DEBUG

typedef struct rgb {
    unsigned char b;
    unsigned char g;
    unsigned char r;
    unsigned char reserved;
}rgb_t;

typedef struct fb {
    int fd;
    struct fb_var_screeninfo vscreen;
    rgb_t *prgb;
}fb_t;

extern unsigned char fontdata_8x16[4096];

extern int framebuff_deinit(fb_t *pfb);
extern fb_t *framebuff_init(const char *pdevname);
extern int framebuffer_drawpixel(fb_t *pfb, int x, int y, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb);
extern int framebuffer_draw_hor_line(fb_t *pfb, int x, int y, int width, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb);
extern int framebuffer_draw_ver_line(fb_t *pfb, int x, int y, int height, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb);
extern int framebuffer_fill_dict(fb_t *pfb, int x, int y, int width, int height, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb);
extern int framebuffer_clearscreen(fb_t *pfb, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb);
extern int framebuffer_draw_ascii(fb_t *pfb, int x, int y, unsigned char ch,
                                        unsigned char fr, unsigned char fg, unsigned char fb,
                                        unsigned char br, unsigned char bg, unsigned char bb);
extern int framebuffer_draw_str(fb_t *pfb, int x, int y, char *pstr,
                                    unsigned char fr, unsigned char fg, unsigned char fb,
                                    unsigned char br, unsigned char bg, unsigned char bb);
#endif
