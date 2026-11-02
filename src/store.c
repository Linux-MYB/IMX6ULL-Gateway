/*
 * store.c —— SQLite 本地存储
 *
 * 两张逻辑：环形表 + 待上传标记
 *   - frames 表按 id 自增，超过 GW_RING_MAX 条就从最旧的开始删，防止把板子存储写满
 *   - uploaded 字段标记是否已上报；断网时新数据 uploaded=0，
 *     网络恢复后按 ts 升序取出补传，这就是“先落本地、恢复后有序补传”
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "gw.h"

static sqlite3 *g_db = NULL;
static sqlite3_stmt *g_st_insert = NULL;
static sqlite3_stmt *g_st_pending = NULL;
static sqlite3_stmt *g_st_mark = NULL;
static int g_insert_cnt = 0;

static int exec_sql(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        printf("[store] SQL 失败: %s (%s)\n", sql, err ? err : "");
        if (err) {
            sqlite3_free(err);
        }
        return -1;
    }
    return 0;
}

int store_open(const char *path)
{
    int rc;

    rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        printf("[store] 打开 %s 失败: %s\n", path, sqlite3_errmsg(g_db));
        return -1;
    }

    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");

    if (exec_sql(
        "CREATE TABLE IF NOT EXISTS frames("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL,"
        " reg0 INTEGER, reg1 INTEGER, reg2 INTEGER, reg3 INTEGER,"
        " person INTEGER, conf INTEGER, node_state INTEGER,"
        " temp REAL, smoke INTEGER, smoke_mv INTEGER,"
        " level INTEGER, plc_ok INTEGER, can_ok INTEGER,"
        " uploaded INTEGER DEFAULT 0"
        ");") < 0) {
        return -1;
    }

    exec_sql("CREATE INDEX IF NOT EXISTS idx_frames_ts ON frames(ts);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_frames_up ON frames(uploaded);");

    rc = sqlite3_prepare_v2(g_db,
        "INSERT INTO frames(ts,reg0,reg1,reg2,reg3,person,conf,node_state,"
        "temp,smoke,smoke_mv,level,plc_ok,can_ok,uploaded)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,0);",
        -1, &g_st_insert, NULL);
    if (rc != SQLITE_OK) {
        printf("[store] 准备插入语句失败\n");
        return -1;
    }

    rc = sqlite3_prepare_v2(g_db,
        "SELECT id,ts,reg0,reg1,reg2,reg3,person,conf,node_state,"
        "temp,smoke,smoke_mv,level,plc_ok,can_ok"
        " FROM frames WHERE uploaded=0 ORDER BY ts ASC LIMIT ?;",
        -1, &g_st_pending, NULL);
    if (rc != SQLITE_OK) {
        printf("[store] 准备查询语句失败\n");
        return -1;
    }

    rc = sqlite3_prepare_v2(g_db, "UPDATE frames SET uploaded=1 WHERE id=?;",
                            -1, &g_st_mark, NULL);
    if (rc != SQLITE_OK) {
        printf("[store] 准备更新语句失败\n");
        return -1;
    }

    printf("[store] %s 已就绪\n", path);
    return 0;
}

/* 环形表：只保留最近 GW_RING_MAX 条 */
static void ring_trim(void)
{
    char sql[160];

    snprintf(sql, sizeof(sql),
             "DELETE FROM frames WHERE id <= "
             "(SELECT MAX(id) FROM frames) - %d;", GW_RING_MAX);
    exec_sql(sql);
}

int store_insert(const struct gw_frame *f)
{
    if (NULL == g_db || NULL == f) {
        return 0;
    }

    sqlite3_reset(g_st_insert);
    sqlite3_clear_bindings(g_st_insert);

    sqlite3_bind_int64(g_st_insert, 1, (sqlite3_int64)f->ts);
    sqlite3_bind_int(g_st_insert, 2, f->reg[0]);
    sqlite3_bind_int(g_st_insert, 3, f->reg[1]);
    sqlite3_bind_int(g_st_insert, 4, f->reg[2]);
    sqlite3_bind_int(g_st_insert, 5, f->reg[3]);
    sqlite3_bind_int(g_st_insert, 6, f->person);
    sqlite3_bind_int(g_st_insert, 7, f->conf);
    sqlite3_bind_int(g_st_insert, 8, f->node_state);
    sqlite3_bind_double(g_st_insert, 9, (double)f->temp);
    sqlite3_bind_int(g_st_insert, 10, f->smoke_raw);
    sqlite3_bind_int(g_st_insert, 11, f->smoke_mv);
    sqlite3_bind_int(g_st_insert, 12, f->level);
    sqlite3_bind_int(g_st_insert, 13, f->plc_ok);
    sqlite3_bind_int(g_st_insert, 14, f->can_ok);

    if (sqlite3_step(g_st_insert) != SQLITE_DONE) {
        printf("[store] 插入失败: %s\n", sqlite3_errmsg(g_db));
        return 0;
    }

    if (++g_insert_cnt >= 200) {
        g_insert_cnt = 0;
        ring_trim();
    }

    return 1;
}

int store_fetch_pending(struct gw_frame *out, long *ids, int max, int *got)
{
    int n = 0;

    if (NULL == g_db || NULL == out || NULL == ids || NULL == got) {
        return -1;
    }
    *got = 0;

    sqlite3_reset(g_st_pending);
    sqlite3_clear_bindings(g_st_pending);
    sqlite3_bind_int(g_st_pending, 1, max);

    while (sqlite3_step(g_st_pending) == SQLITE_ROW) {
        struct gw_frame *f = &out[n];

        memset(f, 0, sizeof(*f));
        ids[n] = (long)sqlite3_column_int64(g_st_pending, 0);
        f->ts       = (long)sqlite3_column_int64(g_st_pending, 1);
        f->reg[0]   = (unsigned short)sqlite3_column_int(g_st_pending, 2);
        f->reg[1]   = (unsigned short)sqlite3_column_int(g_st_pending, 3);
        f->reg[2]   = (unsigned short)sqlite3_column_int(g_st_pending, 4);
        f->reg[3]   = (unsigned short)sqlite3_column_int(g_st_pending, 5);
        f->person   = sqlite3_column_int(g_st_pending, 6);
        f->conf     = sqlite3_column_int(g_st_pending, 7);
        f->node_state = sqlite3_column_int(g_st_pending, 8);
        f->temp     = (float)sqlite3_column_double(g_st_pending, 9);
        f->smoke_raw = sqlite3_column_int(g_st_pending, 10);
        f->smoke_mv  = sqlite3_column_int(g_st_pending, 11);
        f->level    = sqlite3_column_int(g_st_pending, 12);
        f->plc_ok   = sqlite3_column_int(g_st_pending, 13);
        f->can_ok   = sqlite3_column_int(g_st_pending, 14);

        n++;
        if (n >= max) {
            break;
        }
    }

    sqlite3_reset(g_st_pending);
    *got = n;
    return 0;
}

int store_mark_uploaded(long id)
{
    if (NULL == g_db) {
        return -1;
    }

    sqlite3_reset(g_st_mark);
    sqlite3_clear_bindings(g_st_mark);
    sqlite3_bind_int64(g_st_mark, 1, (sqlite3_int64)id);

    if (sqlite3_step(g_st_mark) != SQLITE_DONE) {
        sqlite3_reset(g_st_mark);
        return -1;
    }

    sqlite3_reset(g_st_mark);
    return 0;
}

void store_close(void)
{
    if (g_st_insert)  { sqlite3_finalize(g_st_insert);  g_st_insert = NULL; }
    if (g_st_pending) { sqlite3_finalize(g_st_pending); g_st_pending = NULL; }
    if (g_st_mark)    { sqlite3_finalize(g_st_mark);    g_st_mark = NULL; }

    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/* 存储线程：收到完整帧就落库（上传与否由 upload 线程回填标记） */
void *th_store(void *arg)
{
    struct gw_ctx *ctx = (struct gw_ctx *)arg;
    struct gw_msg m;

    while (g_run) {
        if (gw_recv(ctx, &m, 300) != 0) {
            continue;
        }
        if (m.type != GW_MSG_FRAME) {
            continue;
        }
        store_insert(&m.u.frame);
    }

    store_close();
    return NULL;
}
