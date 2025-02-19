/*
 * chan_quectel
 *
 * Copyright (C) 2011-2015
 * bg <bg_one@mail.ru>
 * http://www.e1550.mobi
 *
 * chan_quectel is based on chan_datacard by
 *
 * Artem Makhutov <artem@makhutov.org>
 * http://www.makhutov.org
 *
 * Dmitry Vagin <dmitry2004@yandex.ru>
 *
 * chan_datacard is based on chan_mobile by Digium
 * (Mark Spencer <markster@digium.com>)
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief UMTS Voice Quectel channel driver
 *
 * \author Artem Makhutov <artem@makhutov.org>
 * \author Dave Bowerman <david.bowerman@gmail.com>
 * \author Dmitry Vagin <dmitry2004@yandex.ru>
 * \author bg <bg_one@mail.ru>
 * \author Max von Buelow <max@m9x.de>
 *
 * \ingroup channel_drivers
 */

#include <signal.h>

#include "ast_config.h"

#include <asterisk/callerid.h>
#include <asterisk/causes.h>
#include <asterisk/format_cache.h>
#include <asterisk/manager.h>
#include <asterisk/module.h> /* AST_MODULE_LOAD_DECLINE ... */
#include <asterisk/stasis_channels.h>
#include <asterisk/stringfields.h> /* AST_DECLARE_STRING_FIELDS for asterisk/manager.h */
#include <asterisk/timing.h>       /* ast_timer_open() ast_timer_fd() */

#include "chan_quectel.h"

#include "app.h"
#include "at_command.h" /* at_cmd2str() */
#include "at_queue.h"   /* struct at_queue_task_cmd at_queue_head_cmd() */
#include "at_read.h"
#include "at_response.h" /* at_res_t */
#include "channel.h"     /* channel_queue_hangup() */
#include "cli.h"
#include "dc_config.h" /* dc_uconfig_fill() dc_gconfig_fill() dc_sconfig_fill()  */
#include "errno.h"
#include "error.h"
#include "eventfd.h"
#include "helpers.h"
#include "monitor_thread.h"
#include "msg_tech.h"
#include "mutils.h" /* ARRAY_LEN() */
#include "pcm.h"
#include "smsdb.h"
#include "tty.h"

static int soundcard_init(struct pvt* pvt)
{
    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    unsigned int channels;

    if (pcm_init(CONF_UNIQ(pvt, alsadev), SND_PCM_STREAM_CAPTURE, fmt, &pvt->icard, &channels, &pvt->audio_fd)) {
        ast_log(LOG_ERROR, "[%s][ALSA] Problem opening capture device '%s'\n", PVT_ID(pvt), CONF_UNIQ(pvt, alsadev));
        return -1;
    }

    if (pcm_init(CONF_UNIQ(pvt, alsadev), SND_PCM_STREAM_PLAYBACK, fmt, &pvt->ocard, &pvt->ocard_channels, NULL)) {
        ast_log(LOG_ERROR, "[%s][ALSA] Problem opening playback device '%s'\n", PVT_ID(pvt), CONF_UNIQ(pvt, alsadev));
        return -1;
    }

    const int err = snd_pcm_link(pvt->icard, pvt->ocard);
    if (err < 0) {
        ast_log(LOG_ERROR, "[%s][ALSA] Couldn't link devices: %s\n", PVT_ID(pvt), snd_strerror(err));
        snd_pcm_close(pvt->icard);
        pvt->icard = NULL;
        snd_pcm_close(pvt->ocard);
        pvt->ocard          = NULL;
        pvt->ocard_channels = 0u;
        return -1;
    }

    ast_verb(2, "[%s][ALSA] Sound card '%s' initialized\n", PVT_ID(pvt), CONF_UNIQ(pvt, alsadev));
    return 0;
}

static int public_state_init(struct public_state* state);

#/* phone monitor thread pvt cleanup */

void pvt_disconnect(struct pvt* pvt)
{
    if (!PVT_NO_CHANS(pvt)) {
        struct cpvt* cpvt;
        AST_LIST_TRAVERSE(&(pvt->chans), cpvt, entry) {
            at_hangup_immediately(cpvt, AST_CAUSE_NORMAL_UNSPECIFIED);
            CPVT_RESET_FLAG(cpvt, CALL_FLAG_NEED_HANGUP);
            cpvt_change_state(cpvt, CALL_STATE_RELEASED, AST_CAUSE_NORMAL_UNSPECIFIED);
        }
    }

    if (pvt->initialized) {
        if (!pvt->is_simcom && CONF_UNIQ(pvt, uac) == TRIBOOL_TRUE) {
            at_disable_uac_immediately(pvt);
        }

        if (pvt->is_simcom && CONF_UNIQ(pvt, uac) == TRIBOOL_TRUE && pvt->has_voice) {
            at_cpcmreg_immediately(pvt, 0);
        }

        at_queue_run_immediately(pvt);
    }

    at_queue_flush(pvt);

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
        if (pvt->icard) {
            const int err = snd_pcm_unlink(pvt->icard);
            if (err < 0) {
                ast_log(LOG_WARNING, "[%s][ALSA] Couldn't unlink devices: %s", PVT_ID(pvt), snd_strerror(err));
            }
            pcm_close(CONF_UNIQ(pvt, alsadev), &pvt->icard, SND_PCM_STREAM_CAPTURE);
        }
        if (pvt->ocard) {
            pcm_close(CONF_UNIQ(pvt, alsadev), &pvt->ocard, SND_PCM_STREAM_PLAYBACK);
            pvt->ocard_channels = 0;
        }
    } else {
        tty_close(CONF_UNIQ(pvt, audio_tty), pvt->audio_fd);
    }

    tty_close(CONF_UNIQ(pvt, data_tty), pvt->data_fd);

    pvt->data_fd  = -1;
    pvt->audio_fd = -1;

    pvt_on_remove_last_channel(pvt);

    ast_debug(1, "[%s] Disconnecting - cleaning up\n", PVT_ID(pvt));

    /* unaffected in case of restart */
    pvt->gsm_reg_status = -1;
    pvt->rssi           = 0;
    pvt->act            = 0;
    pvt->operator= 0;

    memset(&pvt->module_time, 0, sizeof(pvt->module_time));

    ast_string_field_set(pvt, manufacturer, NULL);
    ast_string_field_set(pvt, model, NULL);
    ast_string_field_set(pvt, firmware, NULL);
    ast_string_field_set(pvt, imei, NULL);
    ast_string_field_set(pvt, imsi, NULL);
    ast_string_field_set(pvt, iccid, NULL);
    ast_string_field_set(pvt, location_area_code, NULL);
    ast_string_field_set(pvt, network_name, NULL);
    ast_string_field_set(pvt, short_network_name, NULL);
    ast_string_field_set(pvt, provider_name, "NONE");
    ast_string_field_set(pvt, band, NULL);
    ast_string_field_set(pvt, cell_id, NULL);
    ast_string_field_set(pvt, sms_scenter, NULL);
    ast_string_field_set(pvt, subscriber_number, NULL);

    pvt->has_subscriber_number = 0;

    pvt->gsm_registered   = 0;
    pvt->has_sms          = CONF_SHARED(pvt, msg_direct) ? 0 : 1;
    pvt->has_voice        = 0;
    pvt->has_call_waiting = 0;

    pvt->connected        = 0;
    pvt->initialized      = 0;
    pvt->has_call_waiting = 0;

    /* FIXME: LOST real device state */
    pvt->dialing            = 0;
    pvt->ring               = 0;
    pvt->cwaiting           = 0;
    pvt->outgoing_sms       = 0;
    pvt->incoming_sms_index = -1;
    pvt->incoming_sms_type  = RES_UNKNOWN;
    pvt->volume_sync_step   = VOLUME_SYNC_BEGIN;

    pvt->current_state = DEV_STATE_STOPPED;

    /* clear statictics */
    memset(&pvt->stat, 0, sizeof(pvt->stat));

    if (pvt->local_format_cap) {
        ao2_cleanup(pvt->local_format_cap);
        pvt->local_format_cap = NULL;
    }

    ast_verb(3, "[%s] Disconnected\n", PVT_ID(pvt));
}

static void fd_set_nonblock(const int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void pvt_start(struct pvt* const pvt)
{
    /* prevent start_monitor() multiple times and on turned off devices */
    if (pvt->connected || pvt->desired_state != DEV_STATE_STARTED) {
        // || (pvt->monitor_thread != AST_PTHREADT_NULL &&
        //     (pthread_kill(pvt->monitor_thread, 0) == 0 || errno != ESRCH))
        return;
    }

    pvt_monitor_stop(pvt);

    ast_verb(3, "[%s] Trying to connect data port %s...\n", PVT_ID(pvt), CONF_UNIQ(pvt, alsadev));
    pvt->data_fd = tty_open(CONF_UNIQ(pvt, data_tty), (CONF_UNIQ(pvt, uac) == TRIBOOL_NONE) ? 2 : 0);
    if (pvt->data_fd < 0) {
        return;
    }

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
        if (soundcard_init(pvt) < 0) {
            pvt_disconnect(pvt);
            goto cleanup_datafd;
        }
    } else {
        // TODO: delay until device activate voice call or at pvt_on_create_1st_channel()
        ast_verb(3, "[%s] Trying to open audio port %s...\n", PVT_ID(pvt), CONF_UNIQ(pvt, audio_tty));
        pvt->audio_fd = tty_open(CONF_UNIQ(pvt, audio_tty), pvt->is_simcom);
        if (pvt->audio_fd < 0) {
            goto cleanup_datafd;
        }
    }

    if ((pvt->local_format_cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT))) {
        ast_format_cap_append_by_type(pvt->local_format_cap, AST_MEDIA_TYPE_TEXT);
    }

    if (!pvt_monitor_start(pvt)) {
        if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
            goto cleanup_datafd;
        } else {
            goto cleanup_audiofd;
        }
    }


    /*
     * Set data_fd and audio_fd to non-blocking. This appears to fix
     * incidental deadlocks occurring with Asterisk 12+ or with
     * jitterbuffer enabled. Apparently Asterisk can call the
     * (audio) read function for sockets that don't have data to
     * read().
     */
    fd_set_nonblock(pvt->data_fd);

    if (CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE) {
        fd_set_nonblock(pvt->audio_fd);
    }

    pvt->connected     = 1;
    pvt->current_state = DEV_STATE_STARTED;
    ast_verb(3, "[%s] Connected, initializing...\n", PVT_ID(pvt));
    return;

cleanup_audiofd:
    if (pvt->audio_fd > 0) {
        tty_close(CONF_UNIQ(pvt, audio_tty), pvt->audio_fd);
    }

cleanup_datafd:
    tty_close(CONF_UNIQ(pvt, data_tty), pvt->data_fd);
}

#/* */

static void pvt_free(struct pvt* const pvt)
{
    at_queue_flush(pvt);
    ast_string_field_free_memory(pvt);
    ast_mutex_unlock(&pvt->lock);
    ast_mutex_destroy(&pvt->lock);
    ast_free(pvt);
}

#/* */

static void pvt_destroy(struct pvt* const pvt)
{
    ast_mutex_lock(&pvt->lock);
    pvt_monitor_stop(pvt);
    pvt_free(pvt);
}

// device manager

static const eventfd_t DEV_MANAGER_CMD_SCAN = 1;
static const eventfd_t DEV_MANAGER_CMD_STOP = 2;

static void dev_manager_threadproc_state(struct public_state* const state)
{
    int manager_interval = SCONF_GLOBAL(state, manager_interval);
    const int fd         = state->dev_manager_event;

    auto int ev_wait()
    {
        int t = manager_interval * 1000;
        return at_wait(fd, &t);
    }

    while (1) {
        if (ev_wait() == fd) {
            eventfd_t val = 0;
            if (eventfd_read(fd, &val)) {
                ast_log(LOG_ERROR, "[dev-manager] Fail to read command - exiting\n");
                break;
            }

            if (val == DEV_MANAGER_CMD_SCAN) {
                ast_debug(3, "[dev-manager] Got scan event\n");
                manager_interval = SCONF_GLOBAL(state, manager_interval);
            } else if (val == DEV_MANAGER_CMD_STOP) {
                ast_debug(3, "[dev-manager] Got exit event\n");
                break;
            } else {
                ast_log(LOG_WARNING, "[dev-manager] Unknown command: %d - exiting\n", (int)val);
                continue;
            }
        }

        // timeout
        struct pvt* pvt;
        /* read lock for avoid deadlock when IMEI/IMSI discovery */
        AST_RWLIST_RDLOCK(&state->devices);
        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            SCOPED_MUTEX(pvt_lock, &pvt->lock);

            if (pvt->must_remove) {
                continue;
            }

            if (pvt->restart_time != RESTATE_TIME_NOW) {
                continue;
            }
            if (pvt->desired_state == pvt->current_state) {
                continue;
            }

            switch (pvt->desired_state) {
                case DEV_STATE_RESTARTED:
                    ast_debug(4, "[dev-manager][%s] Restarting device\n", PVT_ID(pvt));
                    pvt_monitor_stop(pvt);
                    pvt->desired_state = DEV_STATE_STARTED;
                    /* fall through */

                case DEV_STATE_STARTED:
                    ast_debug(4, "[dev-manager][%s] Starting device\n", PVT_ID(pvt));
                    pvt_start(pvt);
                    break;

                case DEV_STATE_REMOVED:
                    ast_debug(4, "[dev-manager][%s] Removing device\n", PVT_ID(pvt));
                    pvt_monitor_stop(pvt);
                    pvt->must_remove = 1;
                    break;

                case DEV_STATE_STOPPED:
                    ast_debug(4, "[dev-manager][%s] Stopping device\n", PVT_ID(pvt));
                    pvt_monitor_stop(pvt);
                    break;
            }
        }
        AST_RWLIST_UNLOCK(&state->devices);

        /* actual device removal here for avoid long (discovery) time write lock on device list in loop above */

        if (AST_RWLIST_TRYWRLOCK(&state->devices)) {
            continue;
        }

        AST_RWLIST_TRAVERSE_SAFE_BEGIN(&state->devices, pvt, entry)
            {
                if (ast_mutex_trylock(&pvt->lock)) {
                    continue;
                }

                if (pvt->must_remove) {
                    ast_debug(4, "[dev-manager][%s] Freeing device\n", PVT_ID(pvt));
                    AST_RWLIST_REMOVE_CURRENT(entry);
                    pvt_free(pvt);
                } else {
                    ast_mutex_unlock(&pvt->lock);
                }
            }
        AST_RWLIST_TRAVERSE_SAFE_END;
        AST_RWLIST_UNLOCK(&state->devices);
    }
}

static void* dev_manager_threadproc(void* arg)
{
    struct public_state* const state = (struct public_state* const)arg;
    dev_manager_threadproc_state(state);
    return NULL;
}

static int dev_manager_start(struct public_state* const state)
{
    if (ast_pthread_create_background(&state->dev_manager_thread, NULL, dev_manager_threadproc, state) < 0) {
        state->dev_manager_thread = AST_PTHREADT_NULL;
        return -1;
    }

    return 0;
}

static void dev_manager_scan(const struct public_state* const state)
{
    if (eventfd_write(state->dev_manager_event, DEV_MANAGER_CMD_SCAN)) {
        ast_log(LOG_ERROR, "Unable to signal device manager thread\n");
    }
}

static void dev_manager_stop(struct public_state* const state)
{
    if (eventfd_write(state->dev_manager_event, DEV_MANAGER_CMD_STOP)) {
        ast_log(LOG_ERROR, "Unable to signal device manager thread\n");
    }

    if (state->dev_manager_thread && (state->dev_manager_thread != AST_PTHREADT_STOP) && (state->dev_manager_thread != AST_PTHREADT_NULL)) {
        pthread_join(state->dev_manager_thread, NULL);
    }

    state->dev_manager_thread = AST_PTHREADT_NULL;
}

#/* */

void pvt_on_create_1st_channel(struct pvt* pvt)
{
    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t silence_buf_size      = 2u * pvt_get_audio_frame_size(PTIME_PLAYBACK, fmt);
    pvt->silence_buf                   = ast_calloc(1, silence_buf_size + AST_FRIENDLY_OFFSET);

    if (CONF_SHARED(pvt, multiparty)) {
        if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
            ast_log(LOG_ERROR, "[%s] Multiparty mode not supported in UAC mode\n", PVT_ID(pvt));
        } else {
            const size_t write_buf_size = 5u * pvt_get_audio_frame_size(PTIME_PLAYBACK, fmt);
            pvt->write_buf              = ast_calloc(1, write_buf_size);
            mixb_init(&pvt->write_mixb, pvt->write_buf, write_buf_size);

            pvt->a_timer = ast_timer_open();
        }
    }
}

#/* */

void pvt_on_remove_last_channel(struct pvt* pvt)
{
    if (pvt->a_timer) {
        ast_timer_close(pvt->a_timer);
        pvt->a_timer = NULL;
    }

    ast_free(pvt->silence_buf);
    ast_free(pvt->write_buf);
    pvt->silence_buf = NULL;
    pvt->write_buf   = NULL;
}

#define SET_BIT(dw_array, bitno)                         \
    do {                                                 \
        (dw_array)[(bitno) >> 5] |= 1 << ((bitno) & 31); \
    } while (0)
#define TEST_BIT(dw_array, bitno) ((dw_array)[(bitno) >> 5] & 1 << ((bitno) & 31))
#/* */

int pvt_get_pseudo_call_idx(const struct pvt* pvt)
{
    const struct cpvt* cpvt;
    int* bits;
    int dwords = ((MAX_CALL_IDX + sizeof(*bits) - 1) / sizeof(*bits));

    bits = ast_alloca(dwords * sizeof(*bits));
    memset(bits, 0, dwords * sizeof(*bits));

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        SET_BIT(bits, cpvt->call_idx);
    }

    for (dwords = 1; dwords <= MAX_CALL_IDX; dwords++) {
        if (!TEST_BIT(bits, dwords)) {
            return dwords;
        }
    }
    return 0;
}

#undef TEST_BIT
#undef SET_BIT

#/* */

static int is_dial_possible2(const struct pvt* pvt, unsigned int opts, const struct cpvt* ignore_cpvt)
{
    const struct cpvt* cpvt;
    int hold   = 0;
    int active = 0;
    // FIXME: allow HOLD states for CONFERENCE
    int use_call_waiting = opts & CALL_FLAG_HOLD_OTHER;

    if (pvt->ring || pvt->cwaiting || pvt->dialing) {
        return 0;
    }

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        switch (cpvt->state) {
            case CALL_STATE_INIT:
                if (cpvt != ignore_cpvt) {
                    return 0;
                }
                break;

            case CALL_STATE_DIALING:
            case CALL_STATE_ALERTING:
            case CALL_STATE_INCOMING:
            case CALL_STATE_WAITING:
                return 0;

            case CALL_STATE_ACTIVE:
                if (hold || !use_call_waiting) {
                    return 0;
                }
                active++;
                break;

            case CALL_STATE_ONHOLD:
                if (active || !use_call_waiting) {
                    return 0;
                }
                hold++;
                break;

            case CALL_STATE_RELEASED:;
        }
    }
    return 1;
}

#/* */

int pvt_is_dial_possible(const struct pvt* pvt, unsigned int opts) { return is_dial_possible2(pvt, opts, NULL); }

#/* */

int pvt_enabled(const struct pvt* pvt)
{
    return pvt->current_state == DEV_STATE_STARTED && (pvt->desired_state == pvt->current_state || pvt->restart_time == RESTATE_TIME_CONVENIENT);
}

#/* */

int pvt_ready4voice_call(const struct pvt* pvt, const struct cpvt* current_cpvt, unsigned int opts)
{
    if (!pvt->connected || !pvt->initialized || !pvt->has_voice || !pvt_enabled(pvt)) {
        return 0;
    }

    return is_dial_possible2(pvt, opts, current_cpvt);
}

#/* */

static int can_dial(struct pvt* pvt, unsigned int opts)
{
    /* not allow hold requester channel :) */
    /* FIXME: requestor may be just proxy/masquerade for real channel */
    //	use ast_bridged_channel(chan) ?
    //	use requestor->tech->get_base_channel() ?

    return pvt_ready4voice_call(pvt, NULL, opts);
}

static int can_send_message(struct pvt* pvt, attribute_unused unsigned int opts)
{
    if (!pvt->connected || !pvt->initialized || !pvt->has_sms || !pvt_enabled(pvt)) {
        return 0;
    }

    return 1;
}

void pvt_unlock(struct pvt* const pvt)
{
    if (!pvt) {
        return;
    }

    ast_mutex_unlock(&pvt->lock);
}

int pvt_taskproc_trylock_and_execute(struct pvt* pvt, void (*task_exe)(struct pvt* pvt), const char* task_name)
{
    if (!pvt) {
        return 0;
    }

    if (ast_mutex_trylock(&pvt->lock)) {
        ast_debug(4, "[%s] Task skipping: no lock\n", S_OR(task_name, "UNKNOWN"));
        return 0;
    }

    if (pvt->terminate_monitor) {
        ast_debug(5, "[%s][%s] Task skipping: monitor thread terminated\n", PVT_ID(pvt), S_OR(task_name, "UNKNOWN"));
        ast_mutex_unlock(&pvt->lock);
        return 0;
    }

    ast_debug(5, "[%s][%s] Task executing\n", PVT_ID(pvt), S_OR(task_name, "UNKNOWN"));
    task_exe(pvt);
    ast_debug(6, "[%s][%s] Task executed\n", PVT_ID(pvt), S_OR(task_name, "UNKNOWN"));
    ast_mutex_unlock(&pvt->lock);
    return 0;
}

int pvt_taskproc_lock_and_execute(struct pvt_taskproc_data* ptd, void (*task_exe)(struct pvt_taskproc_data* ptd), const char* task_name)
{
    if (!ptd || !ptd->pvt) {
        return 0;
    }

    SCOPED_MUTEX(plock, &ptd->pvt->lock);

    if (ptd->pvt->terminate_monitor) {
        ast_debug(5, "[%s][%s] Task skipping: monitor thread terminated\n", PVT_ID(ptd->pvt), S_OR(task_name, "UNKNOWN"));
        return 0;
    }

    ast_debug(5, "[%s][%s] Task executing\n", PVT_ID(ptd->pvt), S_OR(task_name, "UNKNOWN"));
    task_exe(ptd);
    ast_debug(6, "[%s][%s] Task executed\n", PVT_ID(ptd->pvt), S_OR(task_name, "UNKNOWN"));
    return 0;
}

#/* return locked pvt or NULL */

struct pvt* pvt_find_ex(struct public_state* state, const char* name)
{
    struct pvt* pvt;

    AST_RWLIST_RDLOCK(&state->devices);
    AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
        ast_mutex_lock(&pvt->lock);
        if (!strcmp(PVT_ID(pvt), name)) {
            break;
        }
        ast_mutex_unlock(&pvt->lock);
    }
    AST_RWLIST_UNLOCK(&state->devices);

    return pvt;
}

#/* return locked pvt or NULL */

struct pvt* pvt_find_by_ext(const char* name)
{
    struct pvt* pvt = pvt_find(name);

    if (pvt) {
        if (!pvt_enabled(pvt)) {
            ast_mutex_unlock(&pvt->lock);
            chan_quectel_err = E_DEVICE_DISABLED;
            pvt              = NULL;
        }
    } else {
        chan_quectel_err = E_DEVICE_NOT_FOUND;
    }
    return pvt;
}

static struct pvt* pvt_find_by_resource_fn(struct public_state* state, const char* resource, unsigned int opts, int (*pvt_test_fn)(struct pvt*, unsigned int),
                                           const struct ast_channel* requestor, int* exists)
{
    auto int test_fn(struct pvt * pvt)
    {
        if (opts & CALL_FLAG_INTERNAL_REQUEST) {
            return 1;
        }

        if ((opts & CALL_FLAG_HOLD_OTHER) == CALL_FLAG_HOLD_OTHER && channel_self_request(pvt, requestor)) {
            return 0;
        }

        return (*pvt_test_fn)(pvt, opts);
    }

    int group;
    size_t i;
    size_t j;
    size_t c;
    size_t last_used;
    struct pvt* pvt;
    struct pvt* found = NULL;
    struct pvt* round_robin[MAXQUECTELDEVICES];

    *exists = 0;
    /* Find requested device and make sure it's connected and initialized. */
    AST_RWLIST_RDLOCK(&state->devices);

    if (((resource[0] == 'g') || (resource[0] == 'G')) && ((resource[1] >= '0') && (resource[1] <= '9'))) {
        errno = 0;
        group = (int)strtol(&resource[1], (char**)NULL, 10);
        if (errno != EINVAL) {
            AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
                ast_mutex_lock(&pvt->lock);

                if (CONF_SHARED(pvt, group) == group) {
                    *exists = 1;
                    if (test_fn(pvt)) {
                        found = pvt;
                        break;
                    }
                }
                ast_mutex_unlock(&pvt->lock);
            }
        }
    } else if (((resource[0] == 'r') || (resource[0] == 'R')) && ((resource[1] >= '0') && (resource[1] <= '9'))) {
        errno = 0;
        group = (int)strtol(&resource[1], (char**)NULL, 10);
        if (errno != EINVAL) {
            /* Generate a list of all available devices */
            j         = ARRAY_LEN(round_robin);
            c         = 0;
            last_used = 0;
            AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
                SCOPED_MUTEX(pvt_lock, &pvt->lock);
                if (CONF_SHARED(pvt, group) == group) {
                    round_robin[c] = pvt;
                    if (pvt->group_last_used == 1) {
                        pvt->group_last_used = 0;
                        last_used            = c;
                    }

                    ++c;

                    if (c == j) {
                        break;
                    }
                }
            }

            /* Search for a available device starting at the last used device */
            for (i = 0, j = last_used + 1; i < c; i++, j++) {
                if (j == c) {
                    j = 0;
                }

                pvt     = round_robin[j];
                *exists = 1;

                ast_mutex_lock(&pvt->lock);
                if (test_fn(pvt)) {
                    pvt->group_last_used = 1;
                    found                = pvt;
                    break;
                }
                ast_mutex_unlock(&pvt->lock);
            }
        }
    } else if (((resource[0] == 'p') || (resource[0] == 'P')) && resource[1] == ':') {
        /* Generate a list of all available devices */
        j         = ARRAY_LEN(round_robin);
        c         = 0;
        last_used = 0;
        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            SCOPED_MUTEX(pvt_lock, &pvt->lock);
            if (!strcmp(pvt->provider_name, &resource[2])) {
                round_robin[c] = pvt;
                if (pvt->prov_last_used == 1) {
                    pvt->prov_last_used = 0;
                    last_used           = c;
                }

                ++c;

                if (c == j) {
                    break;
                }
            }
        }

        /* Search for a available device starting at the last used device */
        for (i = 0, j = last_used + 1; i < c; ++i, ++j) {
            if (j == c) {
                j = 0;
            }

            pvt     = round_robin[j];
            *exists = 1;

            ast_mutex_lock(&pvt->lock);
            if (test_fn(pvt)) {
                pvt->prov_last_used = 1;
                found               = pvt;
                break;
            }
            ast_mutex_unlock(&pvt->lock);
        }
    } else if (((resource[0] == 's') || (resource[0] == 'S')) && resource[1] == ':') {
        /* Generate a list of all available devices */
        j         = ARRAY_LEN(round_robin);
        c         = 0;
        last_used = 0;
        i         = strlen(&resource[2]);

        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            SCOPED_MUTEX(pvt_lock, &pvt->lock);
            if (!strncmp(pvt->imsi, &resource[2], i)) {
                round_robin[c] = pvt;
                if (pvt->sim_last_used == 1) {
                    pvt->sim_last_used = 0;
                    last_used          = c;
                }

                ++c;

                if (c == j) {
                    break;
                }
            }
        }

        /* Search for a available device starting at the last used device */
        for (i = 0, j = last_used + 1; i < c; ++i, ++j) {
            if (j == c) {
                j = 0;
            }

            pvt     = round_robin[j];
            *exists = 1;

            ast_mutex_lock(&pvt->lock);
            if (test_fn(pvt)) {
                pvt->sim_last_used = 1;
                found              = pvt;
                break;
            }
            ast_mutex_unlock(&pvt->lock);
        }
    } else if (((resource[0] == 'i') || (resource[0] == 'I')) && resource[1] == ':') {
        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            ast_mutex_lock(&pvt->lock);
            if (!strcmp(pvt->imei, &resource[2])) {
                *exists = 1;
                if (test_fn(pvt)) {
                    found = pvt;
                    break;
                }
            }
            ast_mutex_unlock(&pvt->lock);
        }
    } else if (((resource[0] == 'j') || (resource[0] == 'J')) && resource[1] == ':') {
        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            ast_mutex_lock(&pvt->lock);
            if (!strcmp(pvt->iccid, &resource[2])) {
                *exists = 1;
                if (test_fn(pvt)) {
                    found = pvt;
                    break;
                }
            }
            ast_mutex_unlock(&pvt->lock);
        }
    } else {
        AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
            ast_mutex_lock(&pvt->lock);
            if (!strcmp(PVT_ID(pvt), resource)) {
                *exists = 1;
                if (test_fn(pvt)) {
                    found = pvt;
                    break;
                }
            }
            ast_mutex_unlock(&pvt->lock);
        }
    }

    AST_RWLIST_UNLOCK(&state->devices);
    return found;
}

struct pvt* pvt_find_by_resource_ex(struct public_state* state, const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists)
{
    return pvt_find_by_resource_fn(state, resource, opts, &can_dial, requestor, exists);
}

struct pvt* pvt_msg_find_by_resource_ex(struct public_state* state, const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists)
{
    return pvt_find_by_resource_fn(state, resource, opts, &can_send_message, requestor, exists);
}

struct cpvt* pvt_channel_find_by_call_idx(struct pvt* pvt, int call_idx)
{
    struct cpvt* cpvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (call_idx == cpvt->call_idx) {
            return cpvt;
        }
    }

    return 0;
}

struct cpvt* pvt_channel_find_active(struct pvt* pvt)
{
    struct cpvt* cpvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (CPVT_IS_SOUND_SOURCE(cpvt)) {
            return cpvt;
        }
    }

    return 0;
}

struct cpvt* pvt_channel_find_last_initialized(struct pvt* pvt)
{
    struct cpvt* cpvt;
    struct cpvt* res = NULL;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (CPVT_IS_SOUND_SOURCE(cpvt) || (cpvt)->state == CALL_STATE_INIT) {
            res = cpvt;
        }
    }

    return res;
}

#/* */

static const char* pvt_state_base(const struct pvt* const pvt)
{
    const char* state = NULL;
    // length is "AAAAAAAAAA"
    if (pvt->current_state == DEV_STATE_STOPPED && pvt->desired_state == DEV_STATE_STOPPED) {
        state = "Stopped";
    } else if (!pvt->connected) {
        state = "Not connected";
    } else if (!pvt->initialized) {
        state = "Not initialized";
    }
    return state;
}

#/* */

const char* pvt_str_state(const struct pvt* pvt)
{
    const char* state = pvt_state_base(pvt);
    if (state) {
        return state;
    }

    if (pvt->ring || PVT_STATE(pvt, chan_count[CALL_STATE_INCOMING])) {
        state = "Ring";
    } else if (pvt->cwaiting || PVT_STATE(pvt, chan_count[CALL_STATE_WAITING])) {
        state = "Waiting";
    } else if (pvt->dialing || (PVT_STATE(pvt, chan_count[CALL_STATE_INIT]) + PVT_STATE(pvt, chan_count[CALL_STATE_DIALING]) +
                                PVT_STATE(pvt, chan_count[CALL_STATE_ALERTING])) > 0) {
        state = "Dialing";
    } else if (PVT_STATE(pvt, chan_count[CALL_STATE_ACTIVE]) > 0) {
        //			state = "Active";
        state = pvt_str_call_dir(pvt);
    } else if (PVT_STATE(pvt, chan_count[CALL_STATE_ONHOLD]) > 0) {
        state = "Held";
    } else if (pvt->outgoing_sms || pvt->incoming_sms_index >= 0) {
        state = "SMS";
    } else {
        state = "Free";
    }

    return state;
}

#/* */

struct ast_str* pvt_str_state_ex(const struct pvt* pvt)
{
    static const size_t DEF_STATE_LEN = 64;

    struct ast_str* buf     = ast_str_create(DEF_STATE_LEN);
    const char* const state = pvt_state_base(pvt);

    if (state) {
        ast_str_append(&buf, 0, "%s", state);
    } else {
        if (pvt->ring || PVT_STATE(pvt, chan_count[CALL_STATE_INCOMING])) {
            ast_str_append(&buf, 0, "Ring");
        }

        if (pvt->dialing || (PVT_STATE(pvt, chan_count[CALL_STATE_INIT]) + PVT_STATE(pvt, chan_count[CALL_STATE_DIALING]) +
                             PVT_STATE(pvt, chan_count[CALL_STATE_ALERTING])) > 0) {
            ast_str_append(&buf, 0, "Dialing");
        }

        if (pvt->cwaiting || PVT_STATE(pvt, chan_count[CALL_STATE_WAITING])) {
            ast_str_append(&buf, 0, "Waiting");
        }

        if (PVT_STATE(pvt, chan_count[CALL_STATE_ACTIVE]) > 0) {
            ast_str_append(&buf, 0, "Active %u", PVT_STATE(pvt, chan_count[CALL_STATE_ACTIVE]));
        }

        if (PVT_STATE(pvt, chan_count[CALL_STATE_ONHOLD]) > 0) {
            ast_str_append(&buf, 0, "Held %u", PVT_STATE(pvt, chan_count[CALL_STATE_ONHOLD]));
        }

        if (pvt->incoming_sms_index >= 0) {
            ast_str_append(&buf, 0, "Incoming SMS");
        }

        if (pvt->outgoing_sms) {
            ast_str_append(&buf, 0, "Outgoing SMS");
        }

        if (!ast_str_strlen(buf)) {
            ast_str_append(&buf, 0, "Free");
        }
    }

    if (pvt->desired_state != pvt->current_state) {
        ast_str_append(&buf, 0, " %s", dev_state2str_msg(pvt->desired_state));
    }

    return buf;
}

const char* pvt_str_call_dir(const struct pvt* pvt)
{
    static const char* dirs[] = {"Active", "Outgoing", "Incoming", "Both"};

    int index = 0;
    const struct cpvt* cpvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (CPVT_DIR_OUTGOING(cpvt)) {
            index |= 0x1;
        } else {
            index |= 0x2;
        }
    }

    return dirs[index];
}

#define SET_PVT_STRING_FIELD(j, f) \
    if (!ast_strlen_zero(pvt->f)) ast_json_object_set(j, #f, ast_json_string_create(pvt->f))

static void set_state_str(const struct pvt* const pvt, struct ast_json* status)
{
    RAII_VAR(struct ast_str*, state_str, pvt_str_state_ex(pvt), ast_free);
    ast_json_object_set(status, "state", ast_json_string_create(ast_str_buffer(state_str)));
}

void pvt_get_status(const struct pvt* const pvt, struct ast_json* status)
{
    ast_json_object_set(status, "name", ast_json_string_create(PVT_ID(pvt)));
    set_state_str(pvt, status);
    ast_json_object_set(status, "gsm", ast_json_string_create(gsm_regstate2str_json(pvt->gsm_reg_status)));

    SET_PVT_STRING_FIELD(status, subscriber_number);
    SET_PVT_STRING_FIELD(status, network_name);
    SET_PVT_STRING_FIELD(status, short_network_name);
    SET_PVT_STRING_FIELD(status, provider_name);
    SET_PVT_STRING_FIELD(status, imei);
    SET_PVT_STRING_FIELD(status, imsi);
    SET_PVT_STRING_FIELD(status, iccid);
    SET_PVT_STRING_FIELD(status, location_area_code);
    SET_PVT_STRING_FIELD(status, cell_id);
    SET_PVT_STRING_FIELD(status, band);

    if (pvt->operator) {
        struct ast_json* const plmn = ast_json_object_create();
        ast_json_object_set(plmn, "value", ast_json_integer_create(pvt->operator));
        ast_json_object_set(plmn, "mcc", ast_json_integer_create(pvt->operator/ 100));
        ast_json_object_set(plmn, "mnc", ast_json_stringf("%02d", pvt->operator% 100));
        ast_json_object_set(status, "plmn", plmn);
    }
}

/* Module */

static struct pvt* pvt_create(const pvt_config_t* settings)
{
    struct pvt* const pvt = ast_calloc(1, sizeof(*pvt) + 1u);

    if (!pvt) {
        ast_log(LOG_ERROR, "[%s] Skipping device: Error allocating memory\n", UCONFIG(settings, id));
        return NULL;
    }

    ast_mutex_init(&pvt->lock);

    AST_LIST_HEAD_INIT_NOLOCK(&pvt->at_queue);
    AST_LIST_HEAD_INIT_NOLOCK(&pvt->chans);

    pvt->monitor_thread     = AST_PTHREADT_NULL;
    pvt->sys_chan.pvt       = pvt;
    pvt->sys_chan.state     = CALL_STATE_RELEASED;
    pvt->audio_fd           = -1;
    pvt->data_fd            = -1;
    pvt->gsm_reg_status     = -1;
    pvt->has_sms            = SCONFIG(settings, msg_direct) ? 0 : 1;
    pvt->incoming_sms_index = -1;
    pvt->incoming_sms_type  = RES_UNKNOWN;
    pvt->desired_state      = SCONFIG(settings, init_state);

    ast_string_field_init(pvt, 15);
    ast_string_field_set(pvt, provider_name, "NONE");
    ast_string_field_set(pvt, subscriber_number, NULL);

    /* and copy settings */
    pvt->settings = *settings;

    pvt->empty_str.__AST_STR_LEN = 1;
    pvt->empty_str.__AST_STR_TS  = DS_STATIC;
    return pvt;
}

#/* */

static int pvt_time4restate(const struct pvt* const pvt)
{
    if (pvt->desired_state != pvt->current_state) {
        if (pvt->restart_time == RESTATE_TIME_NOW || (PVT_NO_CHANS(pvt) && !pvt->outgoing_sms && pvt->incoming_sms_index >= 0)) {
            return 1;
        }
    }
    return 0;
}

#/* */

void pvt_try_restate(struct pvt* pvt)
{
    if (pvt_time4restate(pvt)) {
        pvt->restart_time = RESTATE_TIME_NOW;
        dev_manager_scan(gpublic);
    }
}

#/* assume caller hold lock */

static int pvt_reconfigure(struct pvt* pvt, const pvt_config_t* settings, restate_time_t when)
{
    int rv = 0;

    if (SCONFIG(settings, init_state) == DEV_STATE_REMOVED) {
        /* handle later, in one place */
        pvt->must_remove = 1;
    } else {
        /* check what changes require starting or stopping */
        if (pvt->desired_state != SCONFIG(settings, init_state)) {
            pvt->desired_state = SCONFIG(settings, init_state);

            rv                = pvt_time4restate(pvt);
            pvt->restart_time = rv ? RESTATE_TIME_NOW : when;
        }

        /* check what config changes require restaring */
        else if (pvt_config_compare(settings, &pvt->settings)) {
            /* TODO: schedule restart */
            pvt->desired_state = DEV_STATE_RESTARTED;

            rv                = pvt_time4restate(pvt);
            pvt->restart_time = rv ? RESTATE_TIME_NOW : when;
        }

        /* and copy settings */
        pvt->settings = *settings;
    }
    return rv;
}

int pvt_set_act(struct pvt* pvt, int act)
{
    if (pvt->act == act) {
        return act;
    }

    pvt->act  = act;
    pvt->rssi = 0;
    ast_string_field_set(pvt, band, NULL);
    return act;
}

#/* */

static void mark_must_remove(public_state_t* const state)
{
    struct pvt* pvt;

    /* FIXME: deadlock avoid ? */
    AST_RWLIST_RDLOCK(&state->devices);
    AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
        SCOPED_MUTEX(pvt_lock, &pvt->lock);
        pvt->must_remove = 1;
    }
    AST_RWLIST_UNLOCK(&state->devices);
}

static void mark_remove(public_state_t* const state, const restate_time_t when, unsigned int* reload_cnt)
{
    struct pvt* pvt;

    /* FIXME: deadlock avoid ? */
    /* schedule removal of devices not listed in config file or disabled */
    AST_RWLIST_RDLOCK(&state->devices);
    AST_RWLIST_TRAVERSE(&state->devices, pvt, entry) {
        SCOPED_MUTEX(pvt_lock, &pvt->lock);
        if (!pvt->must_remove) {
            continue;
        }

        pvt->desired_state = DEV_STATE_REMOVED;
        if (pvt_time4restate(pvt)) {
            pvt->restart_time = RESTATE_TIME_NOW;
            (*reload_cnt)++;
        } else {
            pvt->restart_time = when;
        }
    }
    AST_RWLIST_UNLOCK(&state->devices);
}

static int reload_config(public_state_t* state, int recofigure, restate_time_t when, unsigned* reload_immediality)
{
    const char* cat;
    struct ast_flags config_flags = {0};
    struct dc_sconfig config_defaults;
    unsigned reload_now = 0;

    RAII_VAR(struct ast_config*, cfg, ast_config_load(CONFIG_FILE, config_flags), ast_config_destroy);

    if (!cfg) {
        return -1;
    }

    /* read global config */
    dc_gconfig_fill(cfg, "general", &state->global_settings);

    /* read defaults */
    dc_sconfig_fill_defaults(&config_defaults);
    dc_sconfig_fill(cfg, "defaults", &config_defaults);

    mark_must_remove(state);

    /* now load devices */
    for (cat = ast_category_browse(cfg, NULL); cat; cat = ast_category_browse(cfg, cat)) {
        if (strcasecmp(cat, "general") && strcasecmp(cat, "defaults")) {
            pvt_config_t settings;
            const int err = dc_config_fill(cfg, cat, &config_defaults, &settings);
            if (err) {
                continue;
            }
            RAII_VAR(struct pvt* const, pvt, pvt_find(UCONFIG(&settings, id)), pvt_unlock);

            if (pvt) {
                if (!recofigure) {
                    ast_log(LOG_ERROR, "device %s already exists, duplicate in config file\n", cat);
                } else {
                    pvt->must_remove  = 0;
                    reload_now       += pvt_reconfigure(pvt, &settings, when);
                }
                continue;
            }

            /* new device */
            if (SCONFIG(&settings, init_state) == DEV_STATE_REMOVED) {
                ast_log(LOG_NOTICE, "Skipping device %s as disabled\n", cat);
                continue;
            }

            struct pvt* const new_pvt = pvt_create(&settings);
            if (!new_pvt) {
                continue;
            }
            /* FIXME: deadlock avoid ? */
            AST_RWLIST_WRLOCK(&state->devices);
            AST_RWLIST_INSERT_TAIL(&state->devices, new_pvt, entry);
            AST_RWLIST_UNLOCK(&state->devices);
            reload_now++;

            ast_log(LOG_NOTICE, "[%s] Loaded device\n", PVT_ID(new_pvt));
        }
    }

    mark_remove(state, when, &reload_now);

    if (reload_immediality) {
        *reload_immediality = reload_now;
    }

    return 0;
}

#/* */

static void devices_destroy(public_state_t* state)
{
    struct pvt* pvt;

    /* Destroy the device list */
    AST_RWLIST_WRLOCK(&state->devices);
    while ((pvt = AST_RWLIST_REMOVE_HEAD(&state->devices, entry))) {
        pvt_destroy(pvt);
    }
    AST_RWLIST_UNLOCK(&state->devices);
}

const struct ast_format* pvt_get_audio_format(const struct pvt* const pvt)
{
    if (pvt->is_simcom) {
        switch (CONF_UNIQ(pvt, uac)) {
            case TRIBOOL_NONE:
                return ast_format_slin48;

            default:
                return CONF_UNIQ(pvt, slin16) ? ast_format_slin16 : ast_format_slin;
        }
    } else {
        switch (CONF_UNIQ(pvt, uac)) {
            case TRIBOOL_NONE:
                return ast_format_slin48;

            default:
                return ast_format_slin;
        }
    }
}

static size_t pvt_get_audio_frame_size_r(unsigned int ptime, const unsigned int sr)
{
    size_t res  = ptime;
    res        *= sr / 1000;
    res        *= sizeof(int16_t);

    return res;
}

#if PTIME_USE_DEFAULT

size_t pvt_get_audio_frame_size(unsigned int attribute_unused(ptime), const struct ast_format* const fmt)
{
    const unsigned int sr      = ast_format_get_sample_rate(fmt);
    const unsigned int framing = ast_format_get_default_ms(fmt);
    return pvt_get_audio_frame_size_r(framing, sr);
}

#else

size_t pvt_get_audio_frame_size(unsigned int ptime, const struct ast_format* const fmt)
{
    const unsigned int sr = ast_format_get_sample_rate(fmt);
    return pvt_get_audio_frame_size_r(ptime, sr);
}

#endif

void* pvt_get_silence_buffer(struct pvt* const pvt) { return pvt->silence_buf + AST_FRIENDLY_OFFSET; }

int pvt_direct_write(struct pvt* pvt, const char* buf, size_t count)
{
    ast_debug(5, "[%s] [%s]\n", PVT_ID(pvt), tmp_esc_nstr(buf, count));

    const size_t wrote            = fd_write_all(pvt->data_fd, buf, count);
    PVT_STAT(pvt, d_write_bytes) += wrote;
    if (wrote != count) {
        ast_debug(1, "[%s][DATA] Write: %s\n", PVT_ID(pvt), strerror(errno));
    }

    return wrote != count;
}

static struct ast_threadpool* threadpool_create()
{
    static const struct ast_threadpool_options options = {
        .version = AST_THREADPOOL_OPTIONS_VERSION, .idle_timeout = 300, .auto_increment = 1, .initial_size = 0, .max_size = 0};

    return ast_threadpool_create("chan-quectel", NULL, &options);
}

static int load_module()
{
    gpublic = ast_calloc(1, sizeof(*gpublic));

    if (!gpublic) {
        ast_log(LOG_ERROR, "Unable to allocate global state structure\n");
        return AST_MODULE_LOAD_DECLINE;
    }

    const int res = public_state_init(gpublic);
    if (res != AST_MODULE_LOAD_SUCCESS) {
        ast_free(gpublic);
        gpublic = NULL;
    }

    return res;
}

#/* */

#if PTIME_USE_DEFAULT

static void append_fmt(struct ast_format_cap* cap, struct ast_format* fmt)
{
    const unsigned int ms = ast_format_get_default_ms(fmt);
    ast_format_cap_append(cap, fmt, ms);
}

static unsigned int get_default_framing() { return ast_format_get_default_ms(ast_format_slin); }

#else

static void append_fmt(struct ast_format_cap* cap, struct ast_format* fmt) { ast_format_cap_append(cap, fmt, PTIME_CAPTURE); }

static unsigned int get_default_framing() { return PTIME_CAPTURE }

#endif

static int public_state_init(struct public_state* state)
{
    int rv = AST_MODULE_LOAD_DECLINE;

    state->threadpool = threadpool_create();
    if (!state->threadpool) {
        return rv;
    }

    state->dev_manager_event  = eventfd_create();
    state->dev_manager_thread = AST_PTHREADT_NULL;

    AST_RWLIST_HEAD_INIT(&state->devices);

    if (reload_config(state, 0, RESTATE_TIME_NOW, NULL)) {
        ast_log(LOG_ERROR, "Errors reading config file " CONFIG_FILE ", Not loading module\n");
        AST_RWLIST_HEAD_DESTROY(&state->devices);
        return rv;
    }

    rv = AST_MODULE_LOAD_FAILURE;

    if (dev_manager_start(state)) {
        ast_log(LOG_ERROR, "Unable to create device manager thread\n");
        devices_destroy(state);
        AST_RWLIST_HEAD_DESTROY(&state->devices);
        return rv;
    }

    /* set preferred capabilities */
    if (!(channel_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT))) {
        rv = AST_MODULE_LOAD_FAILURE;
        ast_log(LOG_ERROR, "Unable to create channel capabilities\n");
        dev_manager_stop(state);
        devices_destroy(state);
        AST_RWLIST_HEAD_DESTROY(&state->devices);
        return rv;
    }

    append_fmt(channel_tech.capabilities, ast_format_slin);
    append_fmt(channel_tech.capabilities, ast_format_slin16);
    append_fmt(channel_tech.capabilities, ast_format_slin48);
    ast_format_cap_set_framing(channel_tech.capabilities, get_default_framing());

    if (ast_channel_register(&channel_tech)) {
        ast_log(LOG_ERROR, "Unable to register channel class %s\n", channel_tech.type);
        ao2_cleanup(channel_tech.capabilities);
        channel_tech.capabilities = NULL;
        dev_manager_stop(state);
        devices_destroy(state);
        AST_RWLIST_HEAD_DESTROY(&state->devices);
        return rv;
    }

    smsdb_init();
#ifdef WITH_APPLICATIONS
    app_register();
#endif

#ifdef WITH_MSG_TECH
    msg_tech_register();
#endif
    cli_register();

    return AST_MODULE_LOAD_SUCCESS;
}

#/* */

static void public_state_fini(struct public_state* const state)
{
    /* First, take us out of the channel loop */
    ast_channel_unregister(&channel_tech);
    ao2_cleanup(channel_tech.capabilities);
    channel_tech.capabilities = NULL;

    /* Unregister the CLI */
    cli_unregister();

#ifdef WITH_MSG_TECH
    msg_tech_unregister();
#endif

#ifdef WITH_APPLICATIONS
    app_unregister();
#endif

    smsdb_atexit();

    dev_manager_stop(state);
    devices_destroy(state);

    eventfd_close(&state->dev_manager_event);
    AST_RWLIST_HEAD_DESTROY(&state->devices);

    ast_threadpool_shutdown(gpublic->threadpool);
}

static int unload_module()
{
    public_state_fini(gpublic);
    ast_free(gpublic);
    gpublic = NULL;
    return 0;
}

#/* */

void pvt_reload(restate_time_t when)
{
    unsigned dev_reload = 0;

    reload_config(gpublic, 1, when, &dev_reload);
    if (dev_reload > 0) {
        dev_manager_scan(gpublic);
    }
}

#/* */

static int reload_module()
{
    pvt_reload(RESTATE_TIME_GRACEFULLY);
    return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, MODULE_DESCRIPTION, .support_level = AST_MODULE_SUPPORT_EXTENDED, .load = load_module,
                .unload = unload_module, .reload = reload_module, .load_pri = AST_MODPRI_CHANNEL_DRIVER, );

// AST_MODULE_INFO_STANDARD (ASTERISK_GPL_KEY, MODULE_DESCRIPTION);

struct ast_module* self_module(void) { return ast_module_info->self; }
