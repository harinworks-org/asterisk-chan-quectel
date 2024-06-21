/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   Copyright (C) 2010 - 2011
   bg <bg_one@mail.ru>
*/
#include "ast_config.h"

#include <asterisk/utils.h> /* ast_free() */

#include "at_queue.h"

#include "chan_quectel.h" /* struct pvt */
#include "helpers.h"

void at_queue_free_data(at_queue_cmd_t* const cmd)
{
    if (cmd->data) {
        if (!(cmd->flags & ATQ_CMD_FLAG_STATIC)) {
            ast_free(cmd->data);
            cmd->data = NULL;
        }
        /* right because work with copy of static data */
    }
    cmd->length = 0;
}

static void at_queue_free(at_queue_task_t* const task)
{
    for (unsigned i = 0; i < task->cmdsno; ++i) {
        at_queue_free_data(&task->cmds[i]);
    }
    ast_free(task);
}

static void at_queue_remove(struct pvt* const pvt)
{
    // U+21B3 : Downwards Arrow with Tip Rightwards : 0xE2 0x86 0xB3
    at_queue_task_t* const task = AST_LIST_REMOVE_HEAD(&pvt->at_queue, entry);

    if (!task) {
        return;
    }

    ast_atomic_fetchsub_uint32(&PVT_STATE(pvt, at_tasks), 1);
    ast_atomic_fetchsub_uint32(&PVT_STATE(pvt, at_cmds), task->cmdsno - task->cindex);

    if (task->cmdsno == 1u) {
        ast_debug(4, "[%s][%s] \xE2\x86\xB3 [%s] tasks:%lu \n", PVT_ID(pvt), at_cmd2str(task->cmds[0].cmd), at_res2str(task->cmds[0].res),
                  (unsigned long)ast_atomic_fetch_uint32(&PVT_STATE(pvt, at_tasks)));
    } else if (task->cindex >= task->cmdsno) {
        ast_debug(4, "[%s][%s] \xE2\x86\xB3 [%s] cmds:%u tasks:%lu\n", PVT_ID(pvt), at_cmd2str(task->cmds[0].cmd), at_res2str(task->cmds[0].res), task->cmdsno,
                  (unsigned long)ast_atomic_fetch_uint32(&PVT_STATE(pvt, at_tasks)));
    } else {
        ast_debug(3, "[%s][%s] \xE2\x86\xB3 [%s] cmds:%u/%u tasks:%lu\n", PVT_ID(pvt), at_cmd2str(task->cmds[0].cmd), at_res2str(task->cmds[0].res),
                  task->cindex, task->cmdsno, (unsigned long)ast_atomic_fetch_uint32(&PVT_STATE(pvt, at_tasks)));
    }

    at_queue_free(task);
}

at_queue_task_t* at_queue_add(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int prio, unsigned at_once)
{
    // U+21B5 : Downwards Arrow with Corner Leftwards : 0xE2 0x86 0xB5

    if (!cmdsno) {
        return NULL;
    }

    at_queue_task_t* const e = ast_calloc(1, sizeof(*e) + cmdsno * sizeof(*cmds));
    if (!e) {
        return NULL;
    }

    e->cmdsno  = cmdsno;
    e->cpvt    = cpvt;
    e->at_once = at_once;

    memcpy(&e->cmds[0], cmds, cmdsno * sizeof(*cmds));

    struct pvt* const pvt        = cpvt->pvt;
    at_queue_task_t* const first = AST_LIST_FIRST(&pvt->at_queue);

    if (prio && first) {
        AST_LIST_INSERT_AFTER(&pvt->at_queue, first, e, entry);
    } else {
        AST_LIST_INSERT_TAIL(&pvt->at_queue, e, entry);
    }

    ast_atomic_fetchadd_uint32(&PVT_STATE(pvt, at_tasks), 1);
    ast_atomic_fetchadd_uint32(&PVT_STATE(pvt, at_cmds), cmdsno);

    if (e->cmdsno == 1u) {
        ast_debug(4, "[%s][%s] \xE2\x86\xB5 [%s][%s] %s%s\n", PVT_ID(pvt), at_cmd2str(e->cmds[0].cmd), at_res2str(e->cmds[0].res),
                  tmp_esc_nstr(e->cmds[0].data, e->cmds[0].length), prio ? "after head" : "at tail", at_once ? " at once" : "");
    } else {
        ast_debug(4, "[%s][%s] \xE2\x86\xB5 [%s] cmds:%u %s%s\n", PVT_ID(pvt), at_cmd2str(e->cmds[0].cmd), at_res2str(e->cmds[0].res), e->cmdsno,
                  prio ? "after head" : "at tail", at_once ? " at once" : "");
    }

    return e;
}

static void at_queue_remove_cmd(struct pvt* pvt, at_res_t res)
{
    at_queue_task_t* const task = AST_LIST_FIRST(&pvt->at_queue);
    if (!task) {
        return;
    }

    if (task->at_once) {
        task->cindex = task->cmdsno;
        ast_atomic_fetchsub_uint32(&PVT_STATE(pvt, at_cmds), task->cmdsno);

        if (task->cmds[0].res == res || (task->cmds[0].flags & ATQ_CMD_FLAG_IGNORE) || res == RES_TIMEOUT) {
            at_queue_remove(pvt);
        }
    } else {
        // U+229F : Squared Minus : 0xE2 0x8A 0x9F
        const unsigned index = task->cindex;

        task->cindex++;
        ast_atomic_fetchsub_uint32(&PVT_STATE(pvt, at_cmds), -1);
        if (task->cmds[index].res == res) {
            ast_debug(6, "[%s][%s] \xE2\x8A\x9F result:[%s] cmd:%u/%u flags:%02x\n", PVT_ID(pvt), at_cmd2str(task->cmds[index].cmd), at_res2str(res),
                      task->cindex, task->cmdsno, task->cmds[index].flags);
        } else {
            ast_debug(5, "[%s][%s] \xE2\x8A\x9F result:[%s/%s] cmd:%u/%u flags:%02x\n", PVT_ID(pvt), at_cmd2str(task->cmds[index].cmd),
                      at_res2str(task->cmds[index].res), at_res2str(res), task->cindex, task->cmdsno, task->cmds[index].flags);
        }

        if ((task->cindex >= task->cmdsno) || (task->cmds[index].res != res && !(task->cmds[index].flags & ATQ_CMD_FLAG_IGNORE)) || res == RES_TIMEOUT) {
            at_queue_remove(pvt);
        }
    }
}

static void at_queue_remove_task_at_once(struct pvt* const pvt)
{
    at_queue_task_t* const task = AST_LIST_FIRST(&pvt->at_queue);

    if (task && task->at_once) {
        task->cindex = task->cmdsno;
        ast_atomic_fetchsub_uint32(&PVT_STATE(pvt, at_cmds), 1);

        if (!(task->cmds[0].flags & ATQ_CMD_FLAG_IGNORE)) {
            at_queue_remove(pvt);
        }
    }
}

static size_t at_queue_get_total_cmd_len(const at_queue_task_t* const task)
{
    size_t res = 0;

    for (unsigned i = 0; i < task->cmdsno; ++i) {
        res += task->cmds[i].length;
    }

    return res;
}

int at_queue_run(struct pvt* pvt)
{
    int fail                 = 0;
    at_queue_task_t* const t = AST_LIST_FIRST(&pvt->at_queue);
    if (!t) {
        return fail;
    }

    if (t->at_once) {
        size_t buflen = at_queue_get_total_cmd_len(t);
        if (!buflen) {
            return fail;  // do not send again
        }

        buflen += 2u;         // AT + (semicolon separated commands)
        buflen += t->cmdsno;  // number of semicolons

        struct ast_str* buf = ast_str_create(buflen);
        ast_str_set_substr(&buf, buflen, "AT", 2);
        for (unsigned i = 0; i < t->cmdsno; ++i) {
            ast_str_append_substr(&buf, buflen, t->cmds[i].data, t->cmds[i].length);
            ast_str_append_substr(&buf, buflen, (i < (t->cmdsno - 1u)) ? ";" : "\r", 1);
        }

        // U+2192 : Rightwards arrow : 0xE2 0x86 0x92
        ast_debug(2, "[%s][%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), at_cmd2str(t->cmds[0].cmd), tmp_esc_str(buf));

        fail = pvt_direct_write_str(pvt, buf);
        if (fail) {
            // U+2947 : Rightwards Arrow Through X : 0xE2 0xA5 0x87
            ast_log(LOG_WARNING, "[%s][%s] \xE2\xA5\x87 [%s]\n", PVT_ID(pvt), at_cmd2str(t->cmds[0].cmd), tmp_esc_str(buf));
            at_queue_remove_task_at_once(pvt);
        } else {
            for (unsigned i = 0; i < t->cmdsno; ++i) {
                at_queue_free_data(&t->cmds[i]);
            }
            at_queue_cmd_t* const cmd = &(t->cmds[0]);
            cmd->timeout              = ast_tvadd(ast_tvnow(), cmd->timeout);
        }
        ast_free(buf);
    } else {
        at_queue_cmd_t* const cmd = &(t->cmds[t->cindex]);
        if (!cmd->length) {
            return fail;
        }

        ast_debug(2, "[%s][%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), at_cmd2str(cmd->cmd), tmp_esc_nstr(cmd->data, cmd->length));

        fail = pvt_direct_write(pvt, cmd->data, cmd->length);
        if (fail) {
            ast_log(LOG_ERROR, "[%s][%s] \xE2\xA5\x87 [%s]\n", PVT_ID(pvt), at_cmd2str(cmd->cmd), tmp_esc_nstr(cmd->data, cmd->length));
            at_queue_remove_cmd(pvt, cmd->res + 1);
        } else {
            /* set expire time */
            cmd->timeout = ast_tvadd(ast_tvnow(), cmd->timeout);

            /* free data and mark as written */
            at_queue_free_data(cmd);
        }
    }

    return fail;
}

int at_queue_run_immediately(struct pvt* pvt)
{
    int fail      = 0;
    size_t buflen = 0;
    size_t cmdsno = 0;
    size_t pos    = 1u;
    at_queue_task_t* task;

    AST_LIST_TRAVERSE(&pvt->at_queue, task, entry) {
        if (!task->at_once) {
            continue;
        }
        buflen += at_queue_get_total_cmd_len(task);
        cmdsno += task->cmdsno;
    }

    if (!cmdsno) {
        return fail;
    }

    buflen              += 2u;
    buflen              += cmdsno;
    struct ast_str* buf  = ast_str_create(buflen);

    ast_str_set_substr(&buf, buflen, "AT", 2);

    AST_LIST_TRAVERSE(&pvt->at_queue, task, entry) {
        if (!task->at_once) {
            continue;
        }
        for (unsigned i = 0; i < task->cmdsno; ++i) {
            if (task->cmds[i].length) {
                ast_str_append_substr(&buf, buflen, task->cmds[i].data, task->cmds[i].length);
                ast_str_append_substr(&buf, buflen, (pos < cmdsno) ? ";" : "\r", 1);
            }
            pos += 1u;
        }
    }

    // U+21D2 : Rightwards Double Arrow : 0xE2 0x87 0x92
    ast_debug(2, "[%s] \xE2\x87\x92 [%s]\n", PVT_ID(pvt), tmp_esc_str(buf));

    fail = pvt_direct_write_str(pvt, buf);
    if (fail) {
        // U+21CF : Rightwards Double Arrow with Stroke : 0xE2 0x87 0x8F
        ast_log(LOG_WARNING, "[%s] \xE2\x87\x8F [%s]\n", PVT_ID(pvt), tmp_esc_str(buf));
    }

    ast_free(buf);
    return fail;
}

int at_queue_insert_const(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int athead)
{
    return at_queue_add(cpvt, cmds, cmdsno, athead, 0u) == NULL || at_queue_run(cpvt->pvt);
}

int at_queue_insert_const_at_once(struct cpvt* cpvt, const at_queue_cmd_t* cmds, unsigned cmdsno, int athead)
{
    return at_queue_add(cpvt, cmds, cmdsno, athead, 1u) == NULL || at_queue_run(cpvt->pvt);
}

int at_queue_insert_uid(struct cpvt* cpvt, at_queue_cmd_t* cmds, unsigned cmdsno, int athead, int uid)
{
    at_queue_task_t* const task = at_queue_add(cpvt, cmds, cmdsno, athead, 0u);

    if (task) {
        task->uid = uid;
    } else {
        for (unsigned i = 0; i < cmdsno; ++i) {
            at_queue_free_data(&cmds[i]);
        }
    }

    if (at_queue_run(cpvt->pvt)) {
        return -1;
    }

    return task == NULL;
}

int at_queue_insert(struct cpvt* cpvt, at_queue_cmd_t* cmds, unsigned cmdsno, int athead) { return at_queue_insert_uid(cpvt, cmds, cmdsno, athead, 0); }

void at_queue_handle_result(struct pvt* pvt, at_res_t res) { at_queue_remove_cmd(pvt, res); }

void at_queue_flush(struct pvt* pvt)
{
    while (!AST_LIST_EMPTY(&pvt->at_queue)) {
        at_queue_remove(pvt);
    }
}

const struct at_queue_task* at_queue_head_task(const struct pvt* pvt) { return AST_LIST_FIRST(&pvt->at_queue); }

const at_queue_cmd_t* at_queue_head_cmd(const struct pvt* pvt) { return at_queue_task_cmd(at_queue_head_task(pvt)); }

int at_queue_timeout(const struct pvt* pvt, int* diff)
{
    const at_queue_cmd_t* cmd = at_queue_head_cmd(pvt);

    if (!cmd || cmd->length) {
        return -1;
    }

    *diff = ast_tvdiff_ms(cmd->timeout, ast_tvnow());
    return 0;
}
