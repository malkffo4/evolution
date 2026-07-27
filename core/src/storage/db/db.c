// storage/db/db.c
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h> // mkdir

#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/vector_store/vector_store.h"

Database db = {0};

int init_lmdb(const char *db_path) {
    int rc;

    // Пытаемся создать папку. Игнорируем ошибку, если она уже существует.
    #ifdef _WIN32
    mkdir(db_path);
    #else
    mkdir(db_path, 0755);
    #endif

    rc = mdb_env_create(&db.env);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_env_create(&db.env) failed: %s", mdb_strerror(rc));
        return rc;
    }

    mdb_env_set_maxdbs(db.env, LMDB_MAX_DBS); // Increased from 5 to 8 for new databases
    // mdb_env_set_mapsize(db.env, LMDB_MAPSIZE_MB * 1024 * 1024); /* 100MB */
    // 20 ГБ виртуальной памяти (она не занимает RAM, пока не запишешь данные)
    mdb_env_set_mapsize(db.env, 20ULL * 1024 * 1024 * 1024);

    rc = mdb_env_open(db.env, db_path, 0, 0664);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_env_open(db.env) failed: %s", mdb_strerror(rc));
        mdb_env_close(db.env);
        return rc;
    }

    MDB_txn *txn;
    rc = mdb_txn_begin(db.env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_txn_begin(db.env) failed: %s", mdb_strerror(rc));
        return rc;
    }

    rc = mdb_dbi_open(txn, "nodes", MDB_CREATE, &db.graph.nodes);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "frames", MDB_CREATE, &db.graph.frames);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "strings", MDB_CREATE, &db.graph.strings);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "edges", MDB_CREATE, &db.graph.edges);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "algorithms", MDB_CREATE, &db.graph.algorithms);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "properties", MDB_CREATE, &db.graph.properties);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "edges_by_source", MDB_CREATE | MDB_DUPSORT, &db.graph.index.edges_by_source);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "edges_by_target", MDB_CREATE | MDB_DUPSORT, &db.graph.index.edges_by_target);
    if (rc != MDB_SUCCESS) goto fail;

    // Open new databases for vector store
    rc = mdb_dbi_open(txn, "embeddings", MDB_CREATE, &db.vectors.embeddings);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "simhash_index", MDB_CREATE | MDB_DUPSORT, &db.vectors.simhash_index);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "simhash_config", MDB_CREATE, &db.vectors.simhash_config);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "episodes", MDB_CREATE, &db.memory.episodes);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "working", MDB_CREATE, &db.memory.working);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "attention", MDB_CREATE, &db.memory.attention);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "hyper_atoms", MDB_CREATE, &db.graph.hyper.atoms);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "hyper_idx_process", MDB_CREATE | MDB_DUPSORT, &db.graph.hyper.idx_process);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "hyper_idx_args", MDB_CREATE | MDB_DUPSORT, &db.graph.hyper.idx_args);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "hyper_idx_context", MDB_CREATE | MDB_DUPSORT, &db.graph.hyper.idx_context);
    if (rc != MDB_SUCCESS) goto fail;

    rc = mdb_dbi_open(txn, "hyper_patterns", MDB_CREATE, &db.graph.hyper.patterns);
    if (rc != MDB_SUCCESS) goto fail;

    // Initialize SimHash projection matrix
    rc = init_simhash(txn);
    if (rc != 0) {
        LOG_ERROR("init_simhash(txn) Failed to initialize SimHash");
        goto fail;
    }

    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_txn_commit(txn) Failed to commit");
        return rc;
    }

    LOG_DATABASE("LMDB initialized: path=%s maxdbs=%u mapsize=%uMB", db_path, LMDB_MAX_DBS, LMDB_MAPSIZE_MB);
    return MDB_SUCCESS;

fail:
    LOG_ERROR("[db] fail: %s", mdb_strerror(rc));
    mdb_txn_abort(txn);
    return rc;
}

void close_lmdb(void) {
    if (db.env) {
        mdb_env_close(db.env);
        db.env = NULL;
    }
}
