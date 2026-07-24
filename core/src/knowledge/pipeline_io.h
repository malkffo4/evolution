// knowledge/pipeline_io.h
#ifndef PIPELINE_IO_H
#define PIPELINE_IO_H

#include <lmdb.h>

#include "runtime/compiler/pipeline.h"

int pipeline_import_json(const char *json_str, MDB_txn *txn);

#endif
