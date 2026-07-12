// восприятие
#ifndef PERCEPTION_H
#define PERCEPTION_H

#include <lmdb.h>

#include "memory/working.h"

// Загрузка знаний из JSON напрямую в Рабочую Память (Working Memory)
int perceive_and_activate(const char *json_str, WorkingMemory *wm, MDB_txn *txn);

#endif // PERCEPTION_H
