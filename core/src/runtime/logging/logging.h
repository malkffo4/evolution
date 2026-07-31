// runtime/logging/logging.h
#pragma once

#include <stdio.h>

typedef struct {
    FILE *system;
    FILE *reasoner;
    FILE *memory;
    FILE *graph;
    FILE *ipc;
    FILE *planner;
    FILE *debug;
    FILE *perception;
    FILE *database;
    FILE *performance;
} Logger;

extern Logger logger;

int log_init(const char *directory);
void log_shutdown(void);

void log_write(FILE *fp, const char *level, const char *file, const char *func, int line, const char *fmt, ... );

#define LOG_INFO(...) \
    log_write(logger.system, "INFO", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_WARN(...) \
    log_write(logger.system, "WARN", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_ERROR(...) \
    log_write(logger.system, "ERROR", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_DEBUG(...) \
    log_write(logger.debug, "DEBUG", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_REASONER(...) \
    log_write(logger.reasoner, "REASONER", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_MEMORY(...) \
    log_write(logger.memory, "MEMORY", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_GRAPH(...) \
    log_write(logger.graph, "GRAPH", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_IPC(...) \
    log_write(logger.ipc, "IPC", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_PLANNER(...) \
    log_write(logger.planner, "PLANNER", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_DATABASE(...) \
    log_write(logger.database, "DATABASE", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_PERCEPTION(...) \
    log_write(logger.perception, "PERCEPTION", __FILE__, __func__, __LINE__, __VA_ARGS__)

#define LOG_PERF(...) \
    log_write(logger.performance, "PERFORMANCE", __FILE__, __func__, __LINE__, __VA_ARGS__)
