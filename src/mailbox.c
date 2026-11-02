/*
 * mailbox.c —— 线程间邮箱：每个线程一个队列，按名字投递
 *
 * 与课程里的链表版相比，这里用固定容量的环形队列 + 互斥锁 + 条件变量：
 *   - 收消息不再靠 usleep 轮询
 *   - 队列满时丢最旧的一帧，保证采集线程不被拖死
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#include "gw.h"

#define GW_QCAP      16          /* 每个线程队列容量 */
#define GW_MAX_TH    8           /* 最多注册 8 个线程 */

struct gw_queue {
    struct gw_msg buf[GW_QCAP];
    int head;
    int tail;
    int count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
};

/* 一个注册到邮箱里的线程 */
struct th_node {
    char name[32];
    struct gw_ctx ctx;
    gw_queue_t *que;
    pthread_t tid;
    int used;
};

struct th_start_arg {
    struct gw_ctx ctx;
    void *(*fn)(void *);
};

struct gw_mbox {
    struct th_node nodes[GW_MAX_TH];
    int count;
    pthread_mutex_t lock;
};

volatile int g_run = 1;

/* ---------------- 队列 ---------------- */

static gw_queue_t *queue_new(void)
{
    gw_queue_t *q = (gw_queue_t *)calloc(1, sizeof(gw_queue_t));
    if (NULL == q) {
        return NULL;
    }
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

static void queue_free(gw_queue_t *q)
{
    if (NULL == q) {
        return;
    }
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q);
}

static int queue_push(gw_queue_t *q, const struct gw_msg *m)
{
    pthread_mutex_lock(&q->lock);

    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    if (q->count == GW_QCAP) {
        /* 队列满：丢最旧的一条，保留最新数据 */
        q->head = (q->head + 1) % GW_QCAP;
        q->count--;
    }

    q->buf[q->tail] = *m;
    q->tail = (q->tail + 1) % GW_QCAP;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* 超时返回 -1，收到返回 0 */
static int queue_pop(gw_queue_t *q, struct gw_msg *m, int timeout_ms)
{
    struct timespec ts;
    int rc = 0;

    pthread_mutex_lock(&q->lock);

    while (q->count == 0 && !q->closed) {
        if (timeout_ms <= 0) {
            pthread_cond_wait(&q->not_empty, &q->lock);
            break;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    *m = q->buf[q->head];
    q->head = (q->head + 1) % GW_QCAP;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static void queue_close(gw_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

/* ---------------- 线程节点 ---------------- */


static void *thread_trampoline(void *arg)
{
    struct th_start_arg *sa = (struct th_start_arg *)arg;
    void *(*fn)(void *) = sa->fn;
    struct gw_ctx ctx = sa->ctx;
    free(sa);
    return fn(&ctx);
}

gw_mbox_t *gw_mbox_create(void)
{
    gw_mbox_t *mb = (gw_mbox_t *)calloc(1, sizeof(gw_mbox_t));
    if (NULL == mb) {
        return NULL;
    }
    pthread_mutex_init(&mb->lock, NULL);
    return mb;
}

int gw_mbox_add(gw_mbox_t *mb, const char *name, void *(*fn)(void *))
{
    struct th_start_arg *sa = NULL;

    if (NULL == mb || mb->count >= GW_MAX_TH) {
        return -1;
    }

    pthread_mutex_lock(&mb->lock);

    struct th_node *n = &mb->nodes[mb->count];
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->que = queue_new();
    if (NULL == n->que) {
        pthread_mutex_unlock(&mb->lock);
        return -1;
    }

    n->ctx.mbox = mb;
    n->ctx.self = n->que;
    strncpy(n->ctx.name, name, sizeof(n->ctx.name) - 1);

    sa = (struct th_start_arg *)malloc(sizeof(*sa));
    if (NULL == sa) {
        pthread_mutex_unlock(&mb->lock);
        return -1;
    }
    sa->ctx = n->ctx;
    sa->fn = fn;

    n->used = 1;
    mb->count++;

    if (pthread_create(&n->tid, NULL, thread_trampoline, sa) != 0) {
        mb->count--;
        n->used = 0;
        free(sa);
        pthread_mutex_unlock(&mb->lock);
        return -1;
    }

    pthread_mutex_unlock(&mb->lock);
    return 0;
}

int gw_send(gw_mbox_t *mb, const char *dst, const struct gw_msg *msg)
{
    int i;

    if (NULL == mb || NULL == msg) {
        return -1;
    }

    pthread_mutex_lock(&mb->lock);
    for (i = 0; i < mb->count; i++) {
        if (mb->nodes[i].used && 0 == strcmp(mb->nodes[i].name, dst)) {
            gw_queue_t *q = mb->nodes[i].que;
            pthread_mutex_unlock(&mb->lock);
            return queue_push(q, msg);
        }
    }
    pthread_mutex_unlock(&mb->lock);

    return -1;
}

int gw_recv(struct gw_ctx *ctx, struct gw_msg *msg, int timeout_ms)
{
    if (NULL == ctx || NULL == ctx->self || NULL == msg) {
        return -1;
    }
    return queue_pop(ctx->self, msg, timeout_ms);
}

void gw_mbox_stop(gw_mbox_t *mb)
{
    int i;

    g_run = 0;

    if (NULL == mb) {
        return;
    }

    for (i = 0; i < mb->count; i++) {
        if (mb->nodes[i].used) {
            queue_close(mb->nodes[i].que);
        }
    }
}

void gw_mbox_destroy(gw_mbox_t *mb)
{
    int i;

    if (NULL == mb) {
        return;
    }

    gw_mbox_stop(mb);

    for (i = 0; i < mb->count; i++) {
        if (mb->nodes[i].used) {
            pthread_join(mb->nodes[i].tid, NULL);
            queue_free(mb->nodes[i].que);
            mb->nodes[i].que = NULL;
            mb->nodes[i].used = 0;
        }
    }

    pthread_mutex_destroy(&mb->lock);
    free(mb);
}

