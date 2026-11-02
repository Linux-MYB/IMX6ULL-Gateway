#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

#include "gw.h"

#define MB_RETRY      3
#define MB_TIMEOUT_MS 200

static int  g_fd = -1;
static int  g_slave = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned short crc16(const unsigned char *buf, int len)
{
    unsigned short crc = 0xFFFF;
    int i, b;

    for (i = 0; i < len; i++) {
        crc ^= (unsigned short)buf[i];
        for (b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static int speed_to_baud(int baud, speed_t *out)
{
    switch (baud) {
    case 4800:   *out = B4800;   return 0;
    case 9600:   *out = B9600;   return 0;
    case 19200:  *out = B19200;  return 0;
    case 38400:  *out = B38400;  return 0;
    case 115200: *out = B115200; return 0;
    default:     *out = B9600;   return -1;
    }
}

int modbus_open(const char *dev, int baud, int slave)
{
    struct termios tio;
    speed_t sp;
    int fd;

    fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        printf("[modbus] 打开 %s 失败: %s\n", dev, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        printf("[modbus] tcgetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    speed_to_baud(baud, &sp);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        printf("[modbus] tcsetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);

    g_fd = fd;
    g_slave = slave;
    printf("[modbus] %s 已打开，%d 8N1，从站 %d\n", dev, baud, slave);
    return 0;
}

static int read_exact(unsigned char *buf, int need, int timeout_ms)
{
    int got = 0;
    struct timeval tv;
    fd_set rset;
    int remain = timeout_ms;

    while (got < need && remain > 0) {
        int rc;

        FD_ZERO(&rset);
        FD_SET(g_fd, &rset);
        tv.tv_sec  = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;

        rc = select(g_fd + 1, &rset, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return got;
        }
        if (0 == rc) {
            break;
        }

        rc = (int)read(g_fd, buf + got, (size_t)(need - got));
        if (rc <= 0) {
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        got += rc;
        remain = 60;
    }

    return got;
}

static int tx_and_rx(const unsigned char *tx, int txlen,
                     unsigned char *rx, int rxneed)
{
    int attempt;

    for (attempt = 0; attempt < MB_RETRY; attempt++) {
        int n;

        tcflush(g_fd, TCIFLUSH);

        if (write(g_fd, tx, (size_t)txlen) != txlen) {
            continue;
        }
        tcdrain(g_fd);

        n = read_exact(rx, rxneed, MB_TIMEOUT_MS);
        if (n == rxneed) {
            return 0;
        }

        printf("[modbus] 第 %d 次超时（收到 %d / %d 字节）\n", attempt + 1, n, rxneed);
    }

    return -1;
}

int modbus_read_regs(unsigned short start, unsigned short count, unsigned short *out)
{
    unsigned char tx[16];
    unsigned char rx[64];
    unsigned short crc;
    int need, i, ret;

    if (g_fd < 0 || NULL == out || count == 0 || count > 16) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    tx[0] = (unsigned char)g_slave;
    tx[1] = 0x03;
    tx[2] = (unsigned char)(start >> 8);
    tx[3] = (unsigned char)(start & 0xFF);
    tx[4] = (unsigned char)(count >> 8);
    tx[5] = (unsigned char)(count & 0xFF);

    crc = crc16(tx, 6);
    tx[6] = (unsigned char)(crc & 0xFF);
    tx[7] = (unsigned char)(crc >> 8);

    need = 5 + 2 * count;

    ret = tx_and_rx(tx, 8, rx, need);
    if (ret < 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    crc = crc16(rx, need - 2);
    if ((unsigned char)(crc & 0xFF) != rx[need - 2] ||
        (unsigned char)(crc >> 8)   != rx[need - 1]) {
        printf("[modbus] CRC 校验失败\n");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    if (rx[0] != (unsigned char)g_slave || rx[1] != 0x03) {
        printf("[modbus] 应答头不对: 0x%02X 0x%02X\n", rx[0], rx[1]);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    if (rx[2] != (unsigned char)(2 * count)) {
        printf("[modbus] 字节数不对: %d\n", rx[2]);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    for (i = 0; i < count; i++) {
        out[i] = (unsigned short)((rx[3 + 2 * i] << 8) | rx[4 + 2 * i]);
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

int modbus_write_reg(unsigned short reg, unsigned short val)
{
    unsigned char tx[16];
    unsigned char rx[16];
    unsigned short crc;
    int ret;

    if (g_fd < 0) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    tx[0] = (unsigned char)g_slave;
    tx[1] = 0x06;
    tx[2] = (unsigned char)(reg >> 8);
    tx[3] = (unsigned char)(reg & 0xFF);
    tx[4] = (unsigned char)(val >> 8);
    tx[5] = (unsigned char)(val & 0xFF);

    crc = crc16(tx, 6);
    tx[6] = (unsigned char)(crc & 0xFF);
    tx[7] = (unsigned char)(crc >> 8);

    ret = tx_and_rx(tx, 8, rx, 8);
    if (ret < 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    crc = crc16(rx, 6);
    if ((unsigned char)(crc & 0xFF) != rx[6] || (unsigned char)(crc >> 8) != rx[7]) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_lock);
    printf("[modbus] 写寄存器 0x%04X = %u 成功\n", reg, val);
    return 0;
}

void modbus_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void *th_modbus(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg m;

    while (g_run) {
        memset(&m, 0, sizeof(m));
        m.type = GW_MSG_PLC;
        m.ts = (long)time(NULL);

        if (modbus_read_regs(GW_PLC_REG_START, GW_PLC_REG_COUNT, m.u.plc.reg) == 0) {
            m.u.plc.ok = 1;
        } else {
            m.u.plc.ok = 0;
        }

        gw_send(ctx->mbox, "judge", &m);
        usleep(GW_POLL_MS * 1000);
    }

    return NULL;
}
