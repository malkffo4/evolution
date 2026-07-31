// knowledge/pipeline_io.h
#pragma once

#include <lmdb.h>
#include <cjson/cJSON.h>

#include "runtime/compiler/pipeline.h"
#include "storage/hyper_atom/hyper_pattern.h"

Pipeline* pipeline_from_json(cJSON *root, uint64_t *out_algo_id);
int hyper_pattern_from_json(cJSON *root, HyperPattern *out);
