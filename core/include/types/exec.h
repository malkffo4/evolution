// include/types/exec.h
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

typedef enum {
    EXEC_OK              = 0,
    EXEC_PENDING         = 1,

    EXEC_ERR_INVALID     = -1,
    EXEC_ERR_NOMEM       = -2,
    EXEC_ERR_FORK        = -3,
    EXEC_ERR_PIPE        = -4,
    EXEC_ERR_EXEC        = -5,
    EXEC_ERR_TIMEOUT     = -6,
    EXEC_ERR_CANCELLED   = -7,
    EXEC_ERR_NOTFOUND    = -8,
    EXEC_ERR_SHUTDOWN    = -9
} ExecStatus;

// TODO
// ExecTask
// ExecProcess
// ExecRequest
// ExecResult
// ExecCapability
// ExecPolicy

#endif
