// storage/db/db.h
#ifndef DB_H
#define DB_H

#include <lmdb.h>
#include <stdint.h>

typedef struct {
    MDB_env *env;
    struct {
        MDB_dbi nodes;
        MDB_dbi edges;
        MDB_dbi properties;
        MDB_dbi strings;
        MDB_dbi frames;
        MDB_dbi algorithms;
        struct {
            MDB_dbi atoms;
            MDB_dbi idx_process;
            MDB_dbi idx_args;
            MDB_dbi idx_context;
            MDB_dbi idx_causal;   // NEW: source_atom_id -> cause_atom_id (DUPSORT)
            MDB_dbi patterns;
            MDB_dbi archive;
        } hyper;
        struct {
            MDB_dbi edges_by_source;
            MDB_dbi edges_by_target;
            // MDB_dbi nodes_by_name;
            // MDB_dbi nodes_by_type;
            // MDB_dbi strings_by_hash;
        } index;
    } graph;
    struct {
        MDB_dbi embeddings;
        MDB_dbi simhash_index;
        MDB_dbi simhash_config;
    } vectors;
    struct {
        MDB_dbi episodes;
        MDB_dbi working;
        MDB_dbi attention;
    } memory;
} Database;

extern Database db;

#define LMDB_MAX_DBS      24
#define LMDB_MAPSIZE_MB   100

int init_lmdb(const char *db_path);
void close_lmdb(void);

#endif // DB_H
