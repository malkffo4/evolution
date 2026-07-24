// core/globals.h
#ifndef CORE_GLOBALS_H
#define CORE_GLOBALS_H

#include <signal.h>

#include "storage/hyper_atom/hyper_atom.h"
#include "memory/working.h"

extern HyperMemory *global_hyper_mem;
extern WorkingMemory global_wm;
extern volatile sig_atomic_t g_running;

#endif
