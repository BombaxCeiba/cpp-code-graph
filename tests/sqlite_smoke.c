/* 自取件 sqlite3 合并源的冒烟测试（迁移前工程风格 CPM
   self-fetch, cmake/CcgDeps.cmake): runs on EVERY toolchain - the exact
   same in-tree static build the presets link. Version identity, :memory:
   open, DDL, bound insert, select round-trip, close. Plain C by test-data
   sanction (like floodstub.c); links cg::sqlite3 only. */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    if (strcmp(sqlite3_libversion(), "3.54.0") != 0)
    {
        printf("unexpected amalgamation version: %s\n", sqlite3_libversion());
        return 1;
    }
    sqlite3* db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
    {
        printf("open failed: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    if (sqlite3_exec(
            db,
            "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT NOT NULL)",
            NULL,
            NULL,
            NULL)
        != SQLITE_OK)
    {
        return 1;
    }
    sqlite3_stmt* insert = NULL;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO t (v) VALUES (?)",
            -1,
            &insert,
            NULL)
        != SQLITE_OK)
    {
        return 1;
    }
    sqlite3_bind_text(insert, 1, "self-fetched", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert) != SQLITE_DONE)
    {
        return 1;
    }
    sqlite3_finalize(insert);

    sqlite3_stmt* select = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, v FROM t", -1, &select, NULL)
        != SQLITE_OK)
    {
        return 1;
    }
    int rows = 0;
    int seen_id = 0;
    int seen_value = 0;
    while (sqlite3_step(select) == SQLITE_ROW)
    {
        ++rows;
        seen_id = sqlite3_column_int64(select, 0) == 1;
        const unsigned char* v = sqlite3_column_text(select, 1);
        seen_value = v != NULL && strcmp((const char*)v, "self-fetched") == 0;
    }
    sqlite3_finalize(select);
    sqlite3_close_v2(db);
    if (rows != 1 || !seen_id || !seen_value)
    {
        printf(
            "round-trip mismatch: rows=%d id=%d value=%d\n",
            rows,
            seen_id,
            seen_value);
        return 1;
    }
    printf("sqlite self-fetch smoke OK (%s)\n", sqlite3_libversion());
    return 0;
}
