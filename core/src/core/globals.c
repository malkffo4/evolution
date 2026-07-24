// core/globals.c
#include <signal.h>

#include "core/globals.h"

HyperMemory *global_hyper_mem = NULL;
WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;
