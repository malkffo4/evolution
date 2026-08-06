// runtime/logging/logging.c
#define _POSIX_C_SOURCE 200809L   // должно быть самым первым, до #include <time.h>
#define _DEFAULT_SOURCE           // для glibc, без значения
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <string.h>

#include "runtime/logging/logging.h"

Logger logger = {0};

static FILE *open_log(const char *dir, const char *name)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, name);

    return fopen(path, "a");
}

int log_init(const char *directory)
{
    mkdir(directory, 0755);

    logger.system      = open_log(directory, "system.log");
    logger.reasoner    = open_log(directory, "reasoner.log");
    logger.memory      = open_log(directory, "memory.log");
    logger.graph       = open_log(directory, "graph.log");
    logger.ipc         = open_log(directory, "ipc.log");
    logger.planner     = open_log(directory, "planner.log");
    logger.debug       = open_log(directory, "debug.log");
    logger.database    = open_log(directory, "database.log");
    logger.perception  = open_log(directory, "perception.log");
    logger.performance = open_log(directory, "performance.log");

    if (!logger.system ||
        !logger.reasoner ||
        !logger.memory ||
        !logger.graph ||
        !logger.ipc ||
        !logger.planner ||
        !logger.debug ||
        !logger.database ||
        !logger.perception ||
        !logger.performance)
    {
        log_shutdown();
        return -1;
    }

    return 0;
}

void log_shutdown(void)
{
    if (logger.system) fclose(logger.system);
    if (logger.reasoner) fclose(logger.reasoner);
    if (logger.memory) fclose(logger.memory);
    if (logger.graph) fclose(logger.graph);
    if (logger.ipc) fclose(logger.ipc);
    if (logger.planner) fclose(logger.planner);
    if (logger.debug) fclose(logger.debug);
    if (logger.database) fclose(logger.database);
    if (logger.perception) fclose(logger.perception);
    if (logger.performance) fclose(logger.performance);

    logger = (Logger){0};
}

static const char *short_file(const char *path)
{
    const char *p = strrchr(path, '/');

#ifdef _WIN32
    const char *q = strrchr(path, '\\');
    if (!p || (q && q > p))
        p = q;
#endif

    return p ? p + 1 : path;
}

void log_write(FILE *fp, const char *level, const char *file, const char *func, int line, const char *fmt, ...) {
    if (!fp)
        return;

    time_t now = time(NULL);

    struct tm tm_now;

#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif

    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] " "[%-5s] " "[%s:%s:%d] ",
        tm_now.tm_year + 1900,
        tm_now.tm_mon + 1,
        tm_now.tm_mday,
        tm_now.tm_hour,
        tm_now.tm_min,
        tm_now.tm_sec,
        level,
        short_file(file),
        func,
        line);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fputc('\n', fp);

    fflush(fp);
}
