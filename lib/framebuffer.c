#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include "framebuffer.h"

int framebuffer_drawpixel(fb_t *pfb, int x, int y, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb)
{
    pfb->prgb[y * pfb->vscreen.xres_virtual + x].r = tmpr;
    pfb->prgb[y * pfb->vscreen.xres_virtual + x].g = tmpg;
    pfb->prgb[y * pfb->vscreen.xres_virtual + x].b = tmpb;

    return 0;
}

int framebuffer_draw_hor_line(fb_t *pfb, int x, int y, int width, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb)
{
    int i = 0;

    for (i = 0; i < width; i++)
    {
        framebuffer_drawpixel(pfb, x+i, y, tmpr, tmpg, tmpb);
    }

    return 0;
}

int framebuffer_draw_ver_line(fb_t *pfb, int x, int y, int height, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb)
{
    int i = 0;

    for (i = 0; i < height; i++)
    {
        framebuffer_drawpixel(pfb, x, y+i, tmpr, tmpg, tmpb);
    }

    return 0;
}

int framebuffer_fill_dict(fb_t *pfb, int x, int y, int width, int height, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb)
{
    int i = 0;

    for (i = 0; i < height; i++)
    {
        framebuffer_draw_hor_line(pfb, x, y+i, width, tmpr, tmpg, tmpb);
    }

    return 0;
}

int framebuffer_clearscreen(fb_t *pfb, unsigned char tmpr, unsigned char tmpg, unsigned char tmpb)
{
    framebuffer_fill_dict(pfb, 0, 0, pfb->vscreen.xres_virtual, pfb->vscreen.yres_virtual,
                          tmpr, tmpg, tmpb);

    return 0;
}

int framebuffer_draw_ascii(fb_t *pfb, int x, int y, unsigned char ch,
                                    unsigned char fr, unsigned char fg, unsigned char fb,
                                    unsigned char br, unsigned char bg, unsigned char bb)
{
    int i = 0;
    int j = 0;
    unsigned char flag = 0x80;

    for (j = 0; j < 16; j++)
    {
        for (flag = 0x80, i = 0; i < 8; i++, flag >>= 1)
        {
            if (flag & fontdata_8x16[ch * 16 + j])
            {
                framebuffer_drawpixel(pfb, x+i, y+j, fr, fg, fb);
            }
            else
            {
                framebuffer_drawpixel(pfb, x+i, y+j, br, bg, bb);
            }
        }
    }

    return 0;
}

int framebuffer_draw_str(fb_t *pfb, int x, int y, char *pstr,
                                    unsigned char fr, unsigned char fg, unsigned char fb,
                                    unsigned char br, unsigned char bg, unsigned char bb)
{
    int n = 0;

    while (*pstr != '\0')
    {
        framebuffer_draw_ascii(pfb, x+8*n, y, *pstr, fr, fg, fb, br, bg, bb);
        n++;
        pstr++;
    }

    return 0;
}

int framebuff_deinit(fb_t *pfb)
{
    munmap(pfb->prgb, pfb->vscreen.xres_virtual * pfb->vscreen.yres_virtual * pfb->vscreen.bits_per_pixel / 8);

    free(pfb);

    return 0;
}

fb_t *framebuff_init(const char *pdevname)
{
    int ret = 0;
    fb_t *pfb = NULL;

    pfb = malloc(sizeof(fb_t));
    if (NULL == pfb)
    {
        perror("fail to malloc");
        return NULL;
    }

    pfb->fd = open(pdevname, O_RDWR);
    if (-1 == pfb->fd)
    {
        perror("fail to open");
        return NULL;
    }

    ret = ioctl(pfb->fd, FBIOGET_VSCREENINFO, &pfb->vscreen);
    if (-1 == ret)
    {
        perror("fail to ioctl");
        return NULL;
    }

#ifdef DEBUG
    printf("==========================================\n");
    printf("xres = %d, yres = %d\n", pfb->vscreen.xres_virtual, pfb->vscreen.yres_virtual);
    printf("bits_per_pixel = %d\n", pfb->vscreen.bits_per_pixel);
    printf("==========================================\n");
#endif

    pfb->prgb = mmap(NULL, pfb->vscreen.xres_virtual * pfb->vscreen.yres_virtual * pfb->vscreen.bits_per_pixel / 8,
               PROT_READ | PROT_WRITE, MAP_SHARED, pfb->fd, 0);
    if (NULL == pfb->prgb)
    {
        perror("fail to mmap");
        return NULL;
    }

    return pfb;
}
