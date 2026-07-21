// storage/string_pool/string_pool.h
#ifndef STRING_POOL_H
#define STRING_POOL_H

#include <lmdb.h>
#include <stdint.h>

/* Строковый пул */
uint64_t add_string_to_pool(MDB_txn *txn, const char *str);
const char *get_string_from_pool(MDB_txn *txn, uint64_t hash);

#endif // STRING_POOL_H
