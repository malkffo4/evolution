//  слово -> node_id

#ifndef LEXICON_H
#define LEXICON_H

#include <stdint.h>
#include <lmdb.h>

#include "semantic.h"

// extern MDB_dbi dbi_lexicon;

int init_lexicon(MDB_env *env);
simhash256_t compute_semantic_hash(const char *word);
int save_lexicon_entry(MDB_txn *txn, const char *word, const simhash256_t *hash, uint64_t target_node_id);

#endif // LEXICON_H