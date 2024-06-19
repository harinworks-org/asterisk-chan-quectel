/*
   Copyright (C) 2010,2011 bg <bg_one@mail.ru>
*/
#include <fcntl.h>
#include <unistd.h>

#include "ast_config.h"

#include <asterisk/causes.h>
#include <asterisk/utils.h>

#include "cpvt.h"

#include "at_queue.h"     /* struct at_queue_task */
#include "chan_quectel.h" /* struct pvt */
#include "channel.h"
#include "helpers.h"
#include "mutils.h" /* ARRAY_LEN() */

const char* call_state2str(call_state_t state)
{
    static const char* const states[] = {/* real device states */
                                         "active", "held", "dialing", "alerting", "incoming", "waiting",

                                         /* pseudo states */
                                         "released", "init"};

    return enum2str(state, states, ARRAY_LEN(states));
}

// TODO: move to activation time, save resources
static int init_pipe(int filedes[2])
{
    int rv = pipe(filedes);
    if (!rv) {
        for (int x = 0; x < 2; ++x) {
            rv              = fcntl(filedes[x], F_GETFL);
            const int flags = fcntl(filedes[x], F_GETFD);
            if (rv == -1 || flags == -1 || (rv = fcntl(filedes[x], F_SETFL, O_NONBLOCK | rv)) == -1 ||
                (rv = fcntl(filedes[x], F_SETFD, flags | FD_CLOEXEC)) == -1) {
                goto bad;
            }
        }
        return 0;
bad:
        close(filedes[0]);
        close(filedes[1]);
    }
    return rv;
}

#/* */

struct cpvt* cpvt_alloc(struct pvt* pvt, int call_idx, unsigned dir, call_state_t state, unsigned local_channel)
{
    int fd[2] = {-1, -1};

    if (CONF_SHARED(pvt, multiparty)) {
        if (init_pipe(fd)) {
            return NULL;
        }
    }

    struct cpvt* const cpvt = ast_calloc(1, sizeof(*cpvt));
    if (!cpvt) {
        if (CONF_SHARED(pvt, multiparty)) {
            close(fd[0]);
            close(fd[1]);
        }
        return NULL;
    }

    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t buffer_size           = pvt_get_audio_frame_size(PTIME_CAPTURE, fmt);

    cpvt->pvt        = pvt;
    cpvt->call_idx   = call_idx;
    cpvt->state      = state;
    cpvt->rd_pipe[0] = fd[0];
    cpvt->rd_pipe[1] = fd[1];
    cpvt->buffer     = ast_calloc(1, buffer_size);

    CPVT_SET_DIRECTION(cpvt, dir);
    CPVT_SET_LOCAL(cpvt, local_channel);

    AST_LIST_INSERT_TAIL(&pvt->chans, cpvt, entry);
    if (PVT_NO_CHANS(pvt)) {
        pvt_on_create_1st_channel(pvt);
    }
    PVT_STATE(pvt, chansno)++;
    PVT_STATE(pvt, chan_count[cpvt->state])++;

    ast_debug(3, "[%s] Create cpvt - idx:%d dir:%d state:%s buffer_len:%u\n", PVT_ID(pvt), call_idx, dir, call_state2str(state), (unsigned int)buffer_size);
    return cpvt;
}

static void decrease_chan_counters(const struct cpvt* const cpvt, struct pvt* const pvt)
{
    struct cpvt* found;

    AST_LIST_TRAVERSE_SAFE_BEGIN(&pvt->chans, found, entry)
        if (found == cpvt) {
            AST_LIST_REMOVE_CURRENT(entry);
            PVT_STATE(pvt, chan_count[cpvt->state])--;
            PVT_STATE(pvt, chansno)--;
            break;
        }
    AST_LIST_TRAVERSE_SAFE_END;
}

static void relink_to_sys_chan(const struct cpvt* const cpvt, struct pvt* const pvt)
{
    struct at_queue_task* task;

    /* relink task to sys_chan */
    AST_LIST_TRAVERSE(&pvt->at_queue, task, entry) {
        if (task->cpvt == cpvt) {
            task->cpvt = &pvt->sys_chan;
        }
    }
}

void cpvt_free(struct cpvt* cpvt)
{
    struct pvt* const pvt = cpvt->pvt;

    ast_debug(3, "[%s] Destroy cpvt - idx:%d dir:%d state:%s flags:%d channel:%s\n", PVT_ID(pvt), cpvt->call_idx, CPVT_DIRECTION(cpvt),
              call_state2str(cpvt->state), cpvt->flags, cpvt->channel ? "attached" : "detached");

    if (!CPVT_TEST_FLAG(cpvt, CALL_FLAG_DISCONNECTING)) {
        if (PVT_NO_CHANS(pvt)) {
            pvt_on_remove_last_channel(pvt);
            pvt_try_restate(pvt);
        }

        decrease_chan_counters(cpvt, pvt);
    }

    relink_to_sys_chan(cpvt, pvt);

    ast_free(cpvt->buffer);

    close(cpvt->rd_pipe[1]);
    close(cpvt->rd_pipe[0]);

    ast_free(cpvt);
}

void cpvt_call_disactivate(struct cpvt* const cpvt)
{
    if (!(cpvt->pvt && CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED))) {
        return;
    }

    struct pvt* const pvt = cpvt->pvt;

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
        // snd_pcm_drop(pvt->icard);
        // snd_pcm_drop(pvt->ocard);
    } else {
        if (CONF_SHARED(pvt, multiparty)) {
            mixb_detach(&pvt->write_mixb, &cpvt->mixstream);
        }
    }

    CPVT_RESET_FLAGS(cpvt, CALL_FLAG_ACTIVATED | CALL_FLAG_MASTER);
    ast_debug(6, "[%s] Call idx:%d disactivated\n", PVT_ID(pvt), cpvt->call_idx);
}

void cpvt_call_activate(struct cpvt* const cpvt)
{
    struct cpvt* cpvt2;

    /* nothing todo, already main */
    if (CPVT_IS_MASTER(cpvt)) {
        return;
    }

    /* drop any other from MASTER, any set pipe for actives */
    struct pvt* const pvt = cpvt->pvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt2, entry) {
        if (cpvt2 == cpvt) {
            continue;
        }

        if (CPVT_IS_MASTER(cpvt)) {
            ast_debug(6, "[%s] Call idx:%d gave master\n", PVT_ID(pvt), cpvt2->call_idx);
        }

        CPVT_RESET_FLAG(cpvt2, CALL_FLAG_MASTER);

        if (!cpvt2->channel) {
            continue;
        }

        ast_channel_set_fd(cpvt2->channel, 1, -1);
        if (!CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED)) {
            continue;
        }

        ast_channel_set_fd(cpvt2->channel, 0, cpvt2->rd_pipe[PIPE_READ]);
        ast_debug(6, "[%s] Call idx:%d FD:%d still active\n", PVT_ID(pvt), cpvt2->call_idx, cpvt2->rd_pipe[PIPE_READ]);
    }

    /* setup call local write possition */
    if (!CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED)) {
        // FIXME: reset possition?
        if (CONF_SHARED(pvt, multiparty)) {
            mixb_attach(&pvt->write_mixb, &cpvt->mixstream);
        }
    }

    if (pvt->audio_fd >= 0) {
        CPVT_SET_FLAGS(cpvt, CALL_FLAG_ACTIVATED | CALL_FLAG_MASTER);
        ast_debug(6, "[%s] Call idx:%d was master\n", PVT_ID(pvt), cpvt->call_idx);
    }
}

/* NOTE: bg: hmm ast_queue_control() say no need channel lock, trylock got deadlock up to 30 seconds here */
/* NOTE: called from device and current levels with pvt locked */
int cpvt_control(const struct cpvt* const cpvt, enum ast_control_frame_type control)
{
    if (!cpvt || !cpvt->channel) {
        return -1;
    }

    return ast_queue_control(cpvt->channel, control);
}

/* update bits of devstate cache */
static void pvt_update_state_flags(struct pvt* const pvt, const call_state_t oldstate, const call_state_t newstate)
{
    PVT_STATE(pvt, chan_count[oldstate])--;
    PVT_STATE(pvt, chan_count[newstate])++;

    switch (newstate) {
        case CALL_STATE_ACTIVE:
        case CALL_STATE_RELEASED:
            /* no split to incoming/outgoing because these states not intersect */
            switch (oldstate) {
                case CALL_STATE_INIT:
                case CALL_STATE_DIALING:
                case CALL_STATE_ALERTING:
                    pvt->dialing = 0;
                    break;
                case CALL_STATE_INCOMING:
                    pvt->ring = 0;
                    break;
                case CALL_STATE_WAITING:
                    pvt->cwaiting = 0;
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
}

static void change_state_no_channel(struct cpvt* const cpvt, struct pvt* const pvt, const call_state_t state)
{
    switch (state) {
        case CALL_STATE_RELEASED:
            cpvt_free(cpvt);
            break;

        case CALL_STATE_INIT:
        case CALL_STATE_ONHOLD:
        case CALL_STATE_DIALING:
        case CALL_STATE_ALERTING:
        case CALL_STATE_INCOMING:
        case CALL_STATE_WAITING:
            if (at_enqueue_hangup(&pvt->sys_chan, cpvt->call_idx, AST_CAUSE_NORMAL_UNSPECIFIED)) {
                ast_log(LOG_ERROR, "[%s] Unable to hangup call id:%d\n", PVT_ID(pvt), cpvt->call_idx);
            }
            break;

        default:
            break;
    };
}

static void change_state(struct cpvt* const cpvt, struct pvt* const pvt, struct ast_channel* const channel, const call_state_t oldstate,
                         const call_state_t newstate, const short call_idx, const int cause)
{
    /* for live channel */
    switch (newstate) {
        case CALL_STATE_DIALING:
            /* from ^ORIG:idx,y */
            cpvt_call_activate(cpvt);
            cpvt_control(cpvt, AST_CONTROL_PROGRESS);
            ast_setstate(channel, AST_STATE_DIALING);
            break;

        case CALL_STATE_ALERTING:
            cpvt_call_activate(cpvt);
            cpvt_control(cpvt, AST_CONTROL_RINGING);
            ast_setstate(channel, AST_STATE_RINGING);
            break;

        case CALL_STATE_INCOMING:
            cpvt_call_activate(cpvt);
            break;

        case CALL_STATE_ACTIVE:
            cpvt_call_activate(cpvt);
            if (oldstate == CALL_STATE_ONHOLD) {
                ast_debug(1, "[%s] Unhold call idx:%d\n", PVT_ID(pvt), call_idx);
                cpvt_control(cpvt, AST_CONTROL_UNHOLD);
            } else if (CPVT_DIR_OUTGOING(cpvt)) {
                ast_debug(1, "[%s] Remote end answered on call idx:%d\n", PVT_ID(pvt), call_idx);
                cpvt_control(cpvt, AST_CONTROL_ANSWER);
            } else { /* if (cpvt->answered) */
                ast_debug(1, "[%s] Call idx:%d answer\n", PVT_ID(pvt), call_idx);
                ast_setstate(channel, AST_STATE_UP);
            }
            break;

        case CALL_STATE_ONHOLD:
            cpvt_call_disactivate(cpvt);
            ast_debug(1, "[%s] Hold call idx:%d\n", PVT_ID(pvt), call_idx);
            cpvt_control(cpvt, AST_CONTROL_HOLD);
            break;

        case CALL_STATE_RELEASED:
            cpvt_call_disactivate(cpvt);
            /* from +CEND, restart or disconnect */
            /* drop channel -> cpvt reference */
            ast_channel_tech_pvt_set(channel, NULL);
            cpvt_free(cpvt);
            if (channel_enqueue_hangup(channel, cause)) {
                ast_log(LOG_ERROR, "[%s] Error queueing hangup...\n", PVT_ID(pvt));
            }
            break;

        default:
            break;
    }
}

/* FIXME: protection for cpvt->channel if exists */
/* NOTE: called from device level with locked pvt */
int cpvt_change_state(struct cpvt* const cpvt, call_state_t newstate, int cause)
{
    const call_state_t oldstate = cpvt->state;
    if (newstate == oldstate) {
        return 0;
    }

    struct pvt* const pvt             = cpvt->pvt;
    struct ast_channel* const channel = cpvt->channel;
    const short call_idx              = cpvt->call_idx;
    cpvt->state                       = newstate;

    pvt_update_state_flags(pvt, oldstate, newstate);

    // U+2192 : Rightwards Arrow : 0xE2 0x86 0x92
    if (CONF_SHARED(pvt, multiparty)) {
        ast_debug(1, "[%s] Call - idx:%d channel:%s mpty:%d [%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), call_idx, channel ? "attached" : "detached",
                  CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) ? 1 : 0, call_state2str(oldstate), call_state2str(newstate));
    } else {
        ast_debug(1, "[%s] Call - idx:%d channel:%s [%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), call_idx, channel ? "attached" : "detached",
                  call_state2str(oldstate), call_state2str(newstate));
    }

    if (channel) {
        change_state(cpvt, pvt, channel, oldstate, newstate, call_idx, cause);
    } else {
        change_state_no_channel(cpvt, pvt, newstate);
    }
    return 1;
}

int cpvt_lock(struct cpvt* const cpvt)
{
    if (!cpvt) {
        return -1;
    }

    struct pvt* const pvt = cpvt->pvt;
    if (!pvt) {
        return -1;
    }

    return pvt_lock(pvt);
}

int cpvt_unlock(struct cpvt* const cpvt)
{
    if (!cpvt) {
        return -1;
    }
    return pvt_unlock(cpvt->pvt);
}

void* cpvt_get_buffer(struct cpvt* const cpvt) { return cpvt->buffer; }

struct ast_frame* cpvt_prepare_voice_frame(struct cpvt* const cpvt, void* const buf, int samples, const struct ast_format* const fmt)
{
    struct ast_frame* const f = &cpvt->frame;

    memset(f, 0, sizeof(struct ast_frame));

    f->frametype       = AST_FRAME_VOICE;
    f->subclass.format = (struct ast_format*)fmt;
    f->samples         = samples;
    f->datalen         = samples * sizeof(int16_t);
    f->data.ptr        = buf;
    f->src             = AST_MODULE;

    ast_frame_byteswap_le(f);
    return f;
}

struct ast_frame* cpvt_prepare_silence_voice_frame(struct cpvt* const cpvt, int samples, const struct ast_format* const fmt)
{
    return cpvt_prepare_voice_frame(cpvt, pvt_get_silence_buffer(cpvt->pvt), samples, fmt);
}
