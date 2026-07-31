// storage/db/db_writer.h
#pragma once

#include <lmdb.h>

// Возвращает 0 -> транзакция коммитится. Любое другое значение -> abort,
// это же значение возвращается вызывающему db_write_sync().
typedef int (*DbWriteFn)(MDB_txn *txn, void *arg);

int  db_writer_start(void);
void db_writer_stop(void);

// Не блокирует. Возвращает -1 если очередь полна/писатель остановлен.
int db_write_async(DbWriteFn fn, void *arg);

// Блокирует вызывающего до коммита/аборта. Возвращает результат fn (или -1
// при инфраструктурной ошибке).
int db_write_sync(DbWriteFn fn, void *arg);
