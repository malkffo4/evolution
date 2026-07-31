// core/globals.h
#pragma once

#include <signal.h>

#include "storage/hyper_atom/hyper_atom.h"
#include "memory/working.h"
#include "memory/homeostasis.h"

extern HyperMemory *global_hyper_mem;

extern WorkingMemory global_wm;

extern volatile sig_atomic_t g_running;

extern HomeostasisState g_homeostasis;