// core/module_loader.c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "core/module_loader.h"

// Вспомогательная функция для копирования массива строк
static char **duplicate_string_array(const char **src, int *out_count) {
    if (!src) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    int count = 0;
    while (src[count]) count++;
    if (out_count) *out_count = count;

    char **copy = calloc((size_t)(count + 1), sizeof(char *));
    if (!copy) return NULL;

    for (int i = 0; i < count; ++i) {
        copy[i] = src[i] ? strdup(src[i]) : strdup("");
    }
    copy[count] = NULL;
    return copy;
}

Module *load_module(const char *path, char **error_message) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (error_message) *error_message = strdup(dlerror());
        return NULL;
    }

    Module *module = calloc(1, sizeof(*module));
    if (!module) return NULL;
    module->handle = handle;
    module->path = strdup(path);

    // 1. Получаем имя модуля (безопасный кастинг функции)
    void *ptr_name = dlsym(handle, "module_name");
    if (!ptr_name) {
        dlclose(handle);
        free(module);
        return NULL;
    }
    // Объявляем тип: функция, не принимающая аргументов, возвращающая const char*
    const char* (*get_name_func)(void) = (const char* (*)(void))(intptr_t)ptr_name;
    module->name = strdup(get_name_func());

    // 2. Получаем описание
    void *ptr_desc = dlsym(handle, "module_description");
    if (ptr_desc) {
        const char* (*get_desc_func)(void) = (const char* (*)(void))(intptr_t)ptr_desc;
        module->description = strdup(get_desc_func());
    } else {
        module->description = strdup("No description");
    }

    // 3. Получаем список функций
    void *ptr_funcs = dlsym(handle, "module_function_names");
    if (ptr_funcs) {
        // Объявляем тип: функция, возвращающая массив строк (const char**)
        const char** (*get_funcs_func)(void) = (const char** (*)(void))(intptr_t)ptr_funcs;
        const char **names = get_funcs_func();
        module->function_names = duplicate_string_array(names, &module->function_count);
    }

    return module;
}

void free_module(Module *module) {
    if (!module) return;
    dlclose(module->handle);
    free(module->path);
    free(module->name);
    free(module->description);
    if (module->function_names) {
        for(int i = 0; module->function_names[i]; i++) free(module->function_names[i]);
        free(module->function_names);
    }
    free(module);
}

void* module_get_symbol(const Module *module, const char *symbol_name) {
    return dlsym(module->handle, symbol_name);
}

// const char *executor_module_str = "./plugins/executor_module.so\0";
// // Глобальные переменные для модулей
// static Module *g_agency_module = NULL;
// static int (*g_executor_enqueue_script)(const char *, const char *, char *const[], int *) = NULL;
// static int (*g_executor_get_result)(int, char **, int *, int *) = NULL;

// // === ИНИЦИАЛИЗАЦИЯ МОДУЛЕЙ ===
// void load_agency_module(void) {
//     char *error = NULL;
//     g_agency_module = load_module(executor_module_str, &error);

//     if (!g_agency_module) {
//         printf("\033[33m[ПРЕДУПРЕЖДЕНИЕ] Модуль agency не загружен: %s\033[0m\n", error);
//         free(error);
//         return;
//     }

//     // Загружаем функции из модуля
//     g_executor_enqueue_script = (int (*)(const char *, const char *, char *const[], int *))
//         module_get_symbol(g_agency_module, "executor_enqueue_script");
//     g_executor_get_result = (int (*)(int, char **, int *, int *))
//         module_get_symbol(g_agency_module, "executor_get_result");

//     if (!g_executor_enqueue_script || !g_executor_get_result) {
//         printf("\033[33m[ПРЕДУПРЕЖДЕНИЕ] Не удалось загрузить символы из %s\033[0m\n", executor_module_str);
//         free_module(g_agency_module);
//         g_agency_module = NULL;
//         return;
//     }

//     printf("\033[92m[СИСТЕМА] Agency модуль загружен успешно\033[0m\n");
// }
