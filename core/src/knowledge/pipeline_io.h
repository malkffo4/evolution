// knowledge/pipeline_io.h
#ifndef PIPELINE_IO_H
#define PIPELINE_IO_H

#include <lmdb.h>
#include <cjson/cJSON.h>

#include "runtime/compiler/pipeline.h"

Pipeline* pipeline_from_json(cJSON *root, uint64_t *out_algo_id);

#endif
