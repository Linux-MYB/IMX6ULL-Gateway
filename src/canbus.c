#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "gw.h"

static int g_fd = -1;
static char g_ifname[IFNAMSIZ] = GW_CAN_IF;

static int can_try_open(void)
{
    struct sockaddr_can addr;
    struct ifreq ifr;
    int fd;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, g_ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    {
        struct can_filter filters[3];
        filters[0].can_id = GW_CAN_ID_PERSON;
        filters[0].can_mask = CAN_SFF_MASK;
        filters[1].can_id = GW_CAN_ID_CONF;
        filters[1].can_mask = CAN_SFF_MASK;
        filters[2].can_id = GW_CAN_ID_NODE;
        filters[2].can_mask = CAN_SFF_MASK;
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters));
    }

    g_fd = fd;
    return 0;
}

int can_open(const char *ifname)
{
    if (ifname) {
        strncpy(g_ifname, ifname, IFNAMSIZ - 1);
    }

    if (can_try_open() < 0) {
        printf("[can] 打开 %s 失败: %s\n", g_ifname, strerror(errno));
        return -1;
    }

    printf("[can] %s 已就绪\n", g_ifname);
    return 0;
}

int can_read_msg(struct gw_can *out, int timeout_ms)
{
    struct pollfd pfd;
    struct can_frame frame;
    int rc;

    if (NULL == out) {
        return -1;
    }

    if (g_fd < 0) {
        if (can_try_open() < 0) {
            return -1;
        }
    }

    pfd.fd = g_fd;
    pfd.events = POLLIN;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    if (0 == rc) {
        return 0;
    }

    memset(&frame, 0, sizeof(frame));
    rc = (int)read(g_fd, &frame, sizeof(frame));
    if (rc < (int)sizeof(struct can_frame)) {
        printf("[can] read 出错: %s，准备重连\n", strerror(errno));
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    if (frame.can_id & CAN_ERR_FLAG) {
        return 0;
    }

    if (frame.can_id == GW_CAN_ID_PERSON) {
        out->person = frame.data[0] ? 1 : 0;
        return 1;
    }
    if (frame.can_id == GW_CAN_ID_CONF) {
        out->conf = frame.data[0];
        if (out->conf > 100) {
            out->conf = 100;
        }
        return 1;
    }
    if (frame.can_id == GW_CAN_ID_NODE) {
        out->node_state = frame.data[0];
        return 1;
    }

    return 0;
}

void can_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void *th_can(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg m;
    struct gw_can can;
    int rc;

    memset(&can, 0, sizeof(can));

    while (g_run) {
        rc = can_read_msg(&can, 200);
        if (rc <= 0) {
            continue;
        }

        memset(&m, 0, sizeof(m));
        m.type = GW_MSG_CAN;
        m.ts = (long)time(NULL);
        m.u.can = can;
        gw_send(ctx->mbox, "judge", &m);
    }

    can_close();
    return NULL;
}
