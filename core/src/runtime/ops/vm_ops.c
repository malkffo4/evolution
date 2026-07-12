// Используем соглашение:
// ins->a – обычно регистр-приёмник.
// ins->b, ins->c – регистры-источники.
// ins->d – дополнительный операнд (например, индекс константы).
// Константы достаём из pipeline->constants.
// Флаги ins->flags пока не используем, подразумеваем, что операнды – это всегда номера регистров (кроме загрузки констант).
// #include <stdbool.h>
// // #include "runtime/vm/vm.h"
// #include "runtime/ops/vm_ops.h"

// bool vm_check_register(uint32_t reg) {
//     return reg < VM_MAX_REGISTERS;
// }

// bool vm_check_type(*reg, VMType type) {
//     return reg->type == type;
// }

// void vm_register_clear(VMRegister *reg) {
//     reg->type = VM_EMPTY;
// }
void *p;
