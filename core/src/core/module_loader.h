// core/module_loader.h
#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Module {
    void *handle;
    char *path;
    char *name;
    char *description;
    char **function_names;
    int function_count;
} Module;

Module *load_module(const char *path, char **error_message);
void free_module(Module *module);
void *module_get_symbol(const Module *module, const char *symbol_name);

#ifdef __cplusplus
}
#endif

#endif // MODULE_LOADER_H
