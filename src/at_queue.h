/*
   Copyright (C) 2010 bg <bg_one@mail.ru>
*/
#ifndef CHAN_QUECTEL_AT_CMD_QUEUE_H_INCLUDED
#define CHAN_QUECTEL_AT_CMD_QUEUE_H_INCLUDED

#include <sys/time.h> /* struct timeval */

#include <asterisk.h>

#include <asterisk/linkedlists.h> /* AST_LIST_ENTRY */

#define AT_CMD(name) at_##name

// non-null terminated string, CR terminated string
#define DECLARE_AT_CMD(name, cmd) static const char at_##name[sizeof(cmd) + 2] = "AT" cmd "\r";
// null (and CR) terminated string
#define DECLARE_AT_CMDNT(name, cmd) static const char at_##name[] = "AT" cmd "\r";

// non-null terminated string
#define DECLARE_NAKED_AT_CMD(name, cmd) static const char at_##name[sizeof(cmd) - 1] = cmd;
// null terminated string
#define DECLARE_NAKED_AT_CMDNT(name, cmd) static const char at_##name[] = cmd;

#include "at_command.h"  /* at_cmd_t */
#include "at_response.h" /* at_res_t */

typedef struct at_queue_cmd {
    at_cmd_t cmd; /*!< command code */
    at_res_t res; /*!< expected response code, can be RES_OK, RES_CMGR, RES_SMS_PROMPT */

    unsigned flags;                      /*!< flags */
#define ATQ_CMD_FLAG_DEFAULT 0x00        /*!< empty flags */
#define ATQ_CMD_FLAG_STATIC 0x01         /*!< data is static no try deallocate */
#define ATQ_CMD_FLAG_IGNORE 0x02         /*!< ignore response non match condition */
#define ATQ_CMD_FLAG_SUPPRESS_ERROR 0x04 /*!< don't print error message if command fails */

    struct timeval timeout;      /*!< timeout value, started at time when command actually written on device */
#define ATQ_CMD_TIMEOUT_SHORT 1  /*!< timeout value  1 sec */
#define ATQ_CMD_TIMEOUT_MEDIUM 5 /*!< timeout value  5 sec */
#define ATQ_CMD_TIMEOUT_LONG 40  /*!< timeout value 40 sec */

    void* data;      /*!< command and data to send in device */
    unsigned length; /*!< data length */
} at_queue_cmd_t;

/* initializers */
#define ATQ_CMD_INIT_STF(e, icmd, iflags, idata)            \
    do {                                                    \
        (e).cmd             = (icmd);                       \
        (e).res             = RES_OK;                       \
        (e).flags           = iflags | ATQ_CMD_FLAG_STATIC; \
        (e).timeout.tv_sec  = ATQ_CMD_TIMEOUT_MEDIUM;       \
        (e).timeout.tv_usec = 0;                            \
        (e).data            = (void*)(idata);               \
        (e).length          = sizeof(idata);                \
    } while (0)
#define ATQ_CMD_INIT_ST(e, icmd, idata) ATQ_CMD_INIT_STF(e, icmd, ATQ_CMD_FLAG_DEFAULT, idata)

#define ATQ_CMD_INIT_DYNF(e, icmd, iflags)                   \
    do {                                                     \
        (e).cmd             = (icmd);                        \
        (e).res             = RES_OK;                        \
        (e).flags           = iflags & ~ATQ_CMD_FLAG_STATIC; \
        (e).timeout.tv_sec  = ATQ_CMD_TIMEOUT_MEDIUM;        \
        (e).timeout.tv_usec = 0;                             \
    } while (0)
#define ATQ_CMD_INIT_DYN(e, icmd) ATQ_CMD_INIT_DYNF(e, icmd, ATQ_CMD_FLAG_DEFAULT)
#define ATQ_CMD_INIT_DYNI(e, icmd) ATQ_CMD_INIT_DYNF(e, icmd, ATQ_CMD_FLAG_IGNORE)

/* static initializers */
#define ATQ_CMD_DECLARE_STFT(cmd, res, data, flags, s, u) \
    {                                                     \
        (cmd), (res), ATQ_CMD_FLAG_STATIC | flags, {(s), (u)}, \
             (void*)(data), sizeof(data) \
}
#define ATQ_CMD_DECLARE_STF(cmd, res, data, flags) ATQ_CMD_DECLARE_STFT(cmd, res, data, flags, ATQ_CMD_TIMEOUT_MEDIUM, 0)
// #define ATQ_CMD_DECLARE_STF(cmd,res,data,flags)	{ (cmd), (res), ATQ_CMD_FLAG_STATIC|flags, {ATQ_CMD_TIMEOUT_MEDIUM,
// 0}, (char*)(data), STRLEN(data) }
#define ATQ_CMD_DECLARE_ST(cmd, atcmd) ATQ_CMD_DECLARE_STF(cmd, RES_OK, AT_CMD(atcmd), ATQ_CMD_FLAG_DEFAULT)
#define ATQ_CMD_DECLARE_STI(cmd, atcmd) ATQ_CMD_DECLARE_STF(cmd, RES_OK, AT_CMD(atcmd), ATQ_CMD_FLAG_IGNORE)
#define ATQ_CMD_DECLARE_STIT(cmd, atcmd, s, u) ATQ_CMD_DECLARE_STFT(cmd, RES_OK, AT_CMD(atcmd), ATQ_CMD_FLAG_IGNORE, s, u)

#define ATQ_CMD_DECLARE_DYNFT(cmd, res, flags, s, u) \
    {                                                \
        (cmd), (res), flags & ~ATQ_CMD_FLAG_STATIC, {(s), (u)}, \
             0, 0 \
    }
#define ATQ_CMD_DECLARE_DYNF(cmd, res, flags) ATQ_CMD_DECLARE_DYNFT(cmd, res, flags, ATQ_CMD_TIMEOUT_MEDIUM, 0)
// #define ATQ_CMD_DECLARE_DYNF(cmd,res,flags)	{ (cmd), (res),  flags & ~ATQ_CMD_FLAG_STATIC, {ATQ_CMD_TIMEOUT_MEDIUM,
// 0}, 0,      0 }
#define ATQ_CMD_DECLARE_DYN(cmd) ATQ_CMD_DECLARE_DYNF(cmd, RES_OK, ATQ_CMD_FLAG_DEFAULT)
#define ATQ_CMD_DECLARE_DYNI(cmd) ATQ_CMD_DECLARE_DYNF(cmd, RES_OK, ATQ_CMD_FLAG_IGNORE)
#define ATQ_CMD_DECLARE_DYNIT(cmd, s, u) ATQ_CMD_DECLARE_DYNFT(cmd, RES_OK, ATQ_CMD_FLAG_IGNORE, s, u)

typedef struct at_queue_task {
    AST_LIST_ENTRY(at_queue_task) entry;

    unsigned cmdsno;
    unsigned cindex;
    struct cpvt* cpvt;
    int uid;
    unsigned at_once:1;
    at_queue_cmd_t cmds[0]; /* this field must be last */
} at_queue_task_t;

void at_queue_free_data(at_queue_cmd_t* const cmd);
at_queue_task_t* at_queue_add(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int prio, unsigned at_once);
int at_queue_insert_const(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int athead);
int at_queue_insert_const_at_once(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int athead);
int at_queue_insert(struct cpvt* cpvt, at_queue_cmd_t* cmds, unsigned cmdsno, int athead);
int at_queue_insert_uid(struct cpvt* cpvt, at_queue_cmd_t* cmds, unsigned cmdsno, int athead, int uid);
void at_queue_handle_result(struct pvt* pvt, at_res_t res);
void at_queue_flush(struct pvt* pvt);
const at_queue_task_t* at_queue_head_task(const struct pvt* pvt);
const at_queue_cmd_t* at_queue_head_cmd(const struct pvt* pvt);
int at_queue_timeout(const struct pvt* pvt, int* diff);
int at_queue_run(struct pvt* pvt);
int at_queue_run_immediately(struct pvt* pvt);

static inline const at_queue_cmd_t* at_queue_task_cmd(const at_queue_task_t* task) { return task ? &task->cmds[task->at_once ? 0u : task->cindex] : NULL; }

#endif /* CHAN_QUECTEL_AT_CMD_QUEUE_H_INCLUDED */
