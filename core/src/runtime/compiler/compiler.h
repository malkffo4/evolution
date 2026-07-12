#ifndef COMPILER_H
#define COMPILER_H

#include "runtime/vm/vm.h"

ExecutionPlan *pipeline_compile(const Pipeline *pipeline);

#endif
