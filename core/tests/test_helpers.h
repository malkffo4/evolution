#pragma once

#include <stdbool.h>

#include "types/id.h"

// Ожидает материализации факта: существует ли атом с process_id == process_id
// и args == (arg0,arg1), где participant является участником атома (index в idx_args).
// Возвращает 1 если найдено в пределах timeout_ms, иначе 0.
int wait_for_atom_with_args(ko_id_t participant, ko_id_t process_id, ko_id_t arg0, ko_id_t arg1, int timeout_ms);
