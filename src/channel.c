/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
   Alsa component
   Copyright (C) , Digium, Inc.

   By Matthew Fredrickson <creslin@digium.com>
*/

#include "ast_config.h"

#include <asterisk/callerid.h> /*  AST_PRES_* */
#include <asterisk/causes.h>
#include <asterisk/format_cache.h>
#include <asterisk/json.h>
#include <asterisk/lock.h>        /* AST_MUTEX_DEFINE_STATIC */
#include <asterisk/module.h>      /* ast_module_ref() ast_module_info = shit */
#include <asterisk/musiconhold.h> /* ast_moh_start() ast_moh_stop() */
#include <asterisk/pbx.h>         /* pbx_builtin_setvar_helper() */
#include <asterisk/stasis_channels.h>
#include <asterisk/timing.h> /* ast_timer_fd() ast_timer_set_rate() ast_timer_ack() */

#include "channel.h"

#include "at_command.h"
#include "at_queue.h" /* write_all() TODO: move out */
#include "at_read.h"
#include "chan_quectel.h"
#include "helpers.h" /* get_at_clir_value()  */

#ifndef ESTRPIPE
#define ESTRPIPE EPIPE
#endif

static int setvar_helper(const struct pvt* const pvt, struct ast_channel* chan, const char* name, const char* value)
{
    if (ast_strlen_zero(name) || ast_strlen_zero(value)) {
        ast_debug(3, "[%s] Skipping chanvar %s = %s\n", PVT_ID(pvt), S_OR(name, "(null)"), S_OR(value, "(null)"));
        return -1;
    }
    ast_debug(1, "[%s] Setting chanvar %s = %s\n", PVT_ID(pvt), name, value);
    return pbx_builtin_setvar_helper(chan, name, value);
}

static int setvar_helper_json(const struct pvt* const pvt, struct ast_channel* chan, const char* name, struct ast_json* const value)
{
    if (ast_strlen_zero(name) || !value) {
        ast_debug(3, "[%s] Skipping chanvar %s = (null)\n", PVT_ID(pvt), S_OR(name, "(null)"));
    }

    RAII_VAR(char* const, jv, ast_json_dump_string(value), ast_json_free);
    return pbx_builtin_setvar_helper(chan, name, jv);
}

#/* */

static int parse_dial_string(char* dialstr, const char** number, int* opts)
{
    char* options;
    char* dest_num;
    int lopts = 0;

    options = strchr(dialstr, '/');
    if (!options) {
        ast_log(LOG_WARNING, "Can't determine destination in chan_quectel\n");
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }
    *options++ = '\0';

    dest_num = strchr(options, ':');
    if (!dest_num) {
        dest_num = options;
    } else {
        *dest_num++ = '\0';

        if (!strcasecmp(options, "holdother")) {
            lopts = CALL_FLAG_HOLD_OTHER;
        } else if (!strcasecmp(options, "conference")) {
            lopts = CALL_FLAG_HOLD_OTHER | CALL_FLAG_CONFERENCE;
        } else {
            ast_log(LOG_WARNING, "Invalid options in chan_quectel\n");
            return AST_CAUSE_INCOMPATIBLE_DESTINATION;
        }
    }

    if (*dest_num == '\0') {
        ast_log(LOG_WARNING, "Empty destination in chan_quectel\n");
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }
    if (!is_valid_phone_number(dest_num)) {
        ast_log(LOG_WARNING, "Invalid destination '%s' in chan_quectel, only 0123456789*#+ABC allowed\n", dest_num);
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }

    *number = dest_num;
    *opts   = lopts;
    return 0;
}

#/* */

int channel_self_request(struct pvt* pvt, const struct ast_channel* requestor)
{
    /* not allow hold requester channel :) */
    /* FIXME: requestor may be just proxy/masquerade for real channel */
    //	use ast_bridged_channel(chan) ?
    //	use requestor->tech->get_base_channel() ?
    struct cpvt* tmp;
    return (requestor && ast_channel_tech(requestor) == &channel_tech && (tmp = ast_channel_tech_pvt(requestor)) && tmp->pvt == pvt) ? 1 : 0;
}

static struct ast_channel* channel_request(attribute_unused const char* type, struct ast_format_cap* cap, const struct ast_assigned_ids* assignedids,
                                           const struct ast_channel* requestor, const char* data, int* cause)
{
    /* TODO: simplify by moving common code to functions */
    /* TODO: add check when request 'holdother' what requestor is not on same device for 1.6 */
    const char* dest_num;
    struct ast_channel* channel = NULL;
    int opts                    = CALL_FLAG_NONE;
    int exists;

    if (!data) {
        ast_log(LOG_WARNING, "Channel requested with no data\n");
        *cause = AST_CAUSE_INCOMPATIBLE_DESTINATION;
        return NULL;
    }

    char* const dest_dev = ast_strdupa(data);

    *cause = parse_dial_string(dest_dev, &dest_num, &opts);
    if (*cause) {
        return NULL;
    }

    RAII_VAR(struct pvt*, pvt, pvt_find_by_resource(dest_dev, opts, requestor, &exists), pvt_unlock);

    unsigned local_channel = 0;

    if (pvt) {
        local_channel = (unsigned)ast_format_cap_has_type(cap, AST_MEDIA_TYPE_TEXT);
        if (!local_channel) {
            const struct ast_format* const fmt = pvt_get_audio_format(pvt);
            const enum ast_format_cmp_res res  = ast_format_cap_iscompatible_format(cap, fmt);
            if (res != AST_FORMAT_CMP_EQUAL && res != AST_FORMAT_CMP_SUBSET) {
                struct ast_str* codec_buf = ast_str_alloca(64);
                ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_format_cap_get_names(cap, &codec_buf));
#ifdef WRONG_CODEC_FAILURE
                *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
                return NULL;
#endif
            }
        }
    } else {
        if (ast_format_cap_iscompatible_format(cap, ast_format_slin) != AST_FORMAT_CMP_EQUAL &&
            ast_format_cap_iscompatible_format(cap, ast_format_slin16) != AST_FORMAT_CMP_EQUAL &&
            ast_format_cap_iscompatible_format(cap, ast_format_slin48) != AST_FORMAT_CMP_EQUAL) {
            struct ast_str* codec_buf = ast_str_alloca(64);
            ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_format_cap_get_names(cap, &codec_buf));
#ifdef WRONG_CODEC_FAILURE
            *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
            return NULL;
#endif
        }
    }

    if (pvt) {
        channel = channel_new(pvt, AST_STATE_DOWN, NULL, pvt_get_pseudo_call_idx(pvt), CALL_DIR_OUTGOING, CALL_STATE_INIT, NULL, assignedids, requestor,
                              local_channel);
        if (!channel) {
            ast_log(LOG_WARNING, "Unable to allocate channel structure\n");
            *cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
        }
    } else {
        ast_log(LOG_WARNING, "[%s] Request to call on device %s\n", dest_dev, exists ? "which can not make call at this moment" : "not exists");
        *cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
    }

    return channel;
}

static int get_at_clir_value(const int clir)
{
    int res = 0;

    switch (clir) {
        case AST_PRES_ALLOWED_NETWORK_NUMBER:
        case AST_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
        case AST_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
        case AST_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
        case AST_PRES_NUMBER_NOT_AVAILABLE:
            res = 2;
            break;

        case AST_PRES_PROHIB_NETWORK_NUMBER:
        case AST_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
        case AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
        case AST_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
            res = 1;
            break;

        default:
            if ((clir & AST_PRES_RESTRICTION) != AST_PRES_ALLOWED) {
                res = 0;
            } else {
                res = 2;
            }
            break;
    }

    return res;
}

static int channel_call(struct ast_channel* channel, const char* dest, attribute_unused int timeout)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }

    char* const dest_dev = ast_strdupa(dest);

    const char* dest_num;
    int opts;

    if (parse_dial_string(dest_dev, &dest_num, &opts)) {
        return -1;
    }

    if ((ast_channel_state(channel) != AST_STATE_DOWN) && (ast_channel_state(channel) != AST_STATE_RESERVED)) {
        ast_log(LOG_WARNING, "channel_call called on %s, neither down nor reserved\n", ast_channel_name(channel));
        return -1;
    }

    SCOPED_CPVT(cpvtl, cpvt);
    struct pvt* const pvt = cpvt->pvt;

    // FIXME: check if bridged on same device with CALL_FLAG_HOLD_OTHER
    if (!pvt_ready4voice_call(pvt, cpvt, opts)) {
        ast_log(LOG_ERROR, "[%s] Error device already in use or uninitialized\n", PVT_ID(pvt));
        return -1;
    }

    CPVT_SET_FLAGS(cpvt, opts);
    ast_debug(1, "[%s] Calling %s on %s\n", PVT_ID(pvt), dest, ast_channel_name(channel));

    int clir = 0;

    if (CONF_SHARED(pvt, use_calling_pres)) {
        if (CONF_SHARED(pvt, calling_pres) < 0) {
            clir = ast_channel_connected(channel)->id.number.presentation;
        } else {
            clir = CONF_SHARED(pvt, calling_pres);
        }

        ast_debug(4, "[%s] Caller presentation: %s [%d]\n", PVT_ID(pvt), ast_describe_caller_presentation(clir), clir);
        clir = get_at_clir_value(clir);
    } else {
        clir = -1;
    }

    PVT_STAT(pvt, out_calls)++;
    if (at_enqueue_dial(cpvt, dest_num, clir)) {
        ast_log(LOG_ERROR, "[%s] Error sending ATD command\n", PVT_ID(pvt));
        return -1;
    }

    return 0;
}

#/* we has 2 case of call this function, when local side want terminate call and when called for cleanup after remote side alreay terminate call, CEND received and cpvt destroyed */

static int channel_hangup(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    /* its possible call with channel w/o tech_pvt */
    if (cpvt && cpvt->channel == channel && cpvt->pvt) {
        SCOPED_CPVT(cpvtl, cpvt);
        struct pvt* const pvt = cpvt->pvt;

        const int need_hangup  = CPVT_TEST_FLAG(cpvt, CALL_FLAG_NEED_HANGUP) ? 1 : 0;
        const int hangup_cause = ast_channel_hangupcause(channel);

        ast_debug(1, "[%s] Hanging up call - idx:%d cause:%d needed:%d\n", PVT_ID(pvt), cpvt->call_idx, hangup_cause, need_hangup);

        if (need_hangup) {
            if (at_enqueue_hangup(cpvt, cpvt->call_idx, hangup_cause)) {
                ast_log(LOG_ERROR, "[%s] Error adding AT+CHUP command to queue, call not terminated!\n", PVT_ID(pvt));
            } else {
                CPVT_RESET_FLAG(cpvt, CALL_FLAG_NEED_HANGUP);
            }
        }

        cpvt_call_disactivate(cpvt);

        /* drop cpvt->channel reference */
        cpvt->channel = NULL;
    }

    /* drop channel -> cpvt reference */
    ast_channel_tech_pvt_set(channel, NULL);
    ast_setstate(channel, AST_STATE_DOWN);
    return 0;
}

#/* */

static int channel_answer(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return 0;
    }

    SCOPED_CPVT(cpvtl, cpvt);
    struct pvt* const pvt = cpvt->pvt;

    if (CPVT_DIR_INCOMING(cpvt)) {
        if (at_enqueue_answer(cpvt)) {
            ast_log(LOG_ERROR, "[%s] Error sending answer commands\n", PVT_ID(pvt));
        }
    }

    return 0;
}

#/* */

static int channel_digit_begin(struct ast_channel* channel, char digit)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }
    SCOPED_CPVT(cpvtl, cpvt);
    struct pvt* const pvt = cpvt->pvt;

    const int rv = at_enqueue_dtmf(cpvt, digit);
    if (rv) {
        if (rv == -1974) {
            ast_log(LOG_WARNING, "[%s] Sending DTMF %c not supported. Tell Asterisk to generate inband\n", PVT_ID(pvt), digit);
        } else {
            ast_log(LOG_ERROR, "[%s] Error adding DTMF %c command to queue\n", PVT_ID(pvt), digit);
        }
        return -1;
    }

    ast_verb(1, "[%s] Send DTMF: %c\n", PVT_ID(pvt), digit);
    return 0;
}

#/* */

static int channel_digit_end(attribute_unused struct ast_channel* channel, attribute_unused char digit, attribute_unused unsigned int duration) { return 0; }

#/* ARCH: move to cpvt level */

static ssize_t iov_write(struct pvt* pvt, int fd, const struct iovec* const iov, int iovcnt)
{
    const ssize_t len = (ssize_t)at_get_iov_size_n(iov, iovcnt);
    const ssize_t w   = writev(fd, iov, iovcnt);

    if (w < 0) {
        const int err = errno;
        if (err == EINTR || err == EAGAIN) {
            ast_debug(3, "[%s][TTY] Write error: %s\n", PVT_ID(pvt), strerror(err));
        } else {
            ast_log(LOG_WARNING, "[%s][TTY] Write error: %s\n", PVT_ID(pvt), strerror(err));
        }
        return -err;
    } else if (w && w != len) {
        ast_log(LOG_WARNING, "[%s][TTY] Incomplete frame written: %ld/%ld\n", PVT_ID(pvt), (long)w, (long)len);
    }

    return w;
}

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline void change_audio_endianness_to_le(attribute_unused struct iovec* iov, attribute_unused int iovcnt) {}
#else
static void change_audio_endianness_to_le(struct iovec* iov, int iovcnt)
{
    for (; iovcnt-- > 0; ++iov) {
        ast_swapcopy_samples(iov->iov_base, iov->iov_base, iov->iov_len / 2);
    }
}
#endif

#/* */

static void timing_write_tty(struct pvt* pvt, size_t frame_size)
{
    int iovcnt;
    struct iovec iov[3];

    const char* msg   = NULL;
    const size_t used = mixb_used(&pvt->write_mixb);

    if (used >= frame_size) {
        iovcnt = mixb_read_n_iov(&pvt->write_mixb, iov, frame_size);
        mixb_read_n_iov(&pvt->write_mixb, iov, frame_size);
        mixb_read_upd(&pvt->write_mixb, frame_size);
        change_audio_endianness_to_le(iov, iovcnt);
    } else if (used > 0) {
        PVT_STAT(pvt, write_tframes)++;
        msg = "[%s] write truncated frame\n";

        iovcnt = mixb_read_all_iov(&pvt->write_mixb, iov);
        mixb_read_all_iov(&pvt->write_mixb, iov);
        mixb_read_upd(&pvt->write_mixb, used);

        iov[iovcnt].iov_base = pvt_get_silence_buffer(pvt);
        iov[iovcnt].iov_len  = frame_size - used;
        iovcnt++;
        change_audio_endianness_to_le(iov, iovcnt);
    } else {
        PVT_STAT(pvt, write_sframes)++;
        msg = "[%s] write silence\n";

        iov[0].iov_base = pvt_get_silence_buffer(pvt);
        iov[0].iov_len  = frame_size;
        iovcnt          = 1;
    }

    if (msg) {
        ast_debug(7, msg, PVT_ID(pvt));
    }

    if (iov_write(pvt, pvt->audio_fd, iov, iovcnt) >= 0) {
        PVT_STAT(pvt, write_frames)++;
    }
}

#/* copy voice data from device to each channel in conference */

static void write_conference(struct pvt* pvt, const char* const buffer, size_t length)
{
    struct cpvt* cpvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (CPVT_IS_ACTIVE(cpvt) && !CPVT_IS_MASTER(cpvt) && CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && cpvt->rd_pipe[PIPE_WRITE] >= 0) {
            const size_t wr = fd_write_all(cpvt->rd_pipe[PIPE_WRITE], buffer, length);
            //			ast_debug (6, "[%s] write2 | call idx %d pipe fd %d wrote %d bytes\n", PVT_ID(pvt),
            // cpvt->call_idx, cpvt->rd_pipe[PIPE_WRITE], wr);
            if (wr != length) {
                ast_debug(1, "[%s][PIPE] Write error: %s\n", PVT_ID(pvt), strerror(errno));
            }
        }
    }
}

static struct ast_frame* channel_read_tty(struct cpvt* cpvt, struct pvt* pvt, size_t frame_size, const struct ast_format* const fmt)
{
    void* const buf = cpvt_get_buffer(cpvt);
    const int fd    = CPVT_IS_MASTER(cpvt) ? pvt->audio_fd : cpvt->rd_pipe[PIPE_READ];

    if (fd < 0) {
        return NULL;
    }

    const int res = read(fd, buf, frame_size);
    if (res <= 0) {
        if (errno && errno != EAGAIN && errno != EINTR) {
            ast_debug(1, "[%s][TTY] Read error: %s\n", PVT_ID(pvt), strerror(errno));
        } else if (!res) {
            ast_debug(8, "[%s][TTY] Zero bytes returned\n", PVT_ID(pvt));
        }
        return NULL;
    }

    // ast_debug(7, "[%s] call idx %d read %u\n", PVT_ID(pvt), cpvt->call_idx, (unsigned)res);
    // ast_debug(6, "[%s] read | call idx %d fd %d read %d bytes\n", PVT_ID(pvt), cpvt->call_idx, pvt->audio_fd, res);

    if (CPVT_IS_MASTER(cpvt)) {
        if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY)) {
            write_conference(pvt, buf, res);
        }

        PVT_STAT(pvt, a_read_bytes) += res;
        PVT_STAT(pvt, read_frames)++;
        if (res < frame_size) {
            PVT_STAT(pvt, read_sframes)++;
        }
    }

    return cpvt_prepare_voice_frame(cpvt, buf, res / 2, fmt);
}

static struct ast_frame* channel_read_uac(struct cpvt* cpvt, struct pvt* pvt, size_t frames, const struct ast_format* const fmt)
{
    pcm_show_state(6, "CAPTURE", PVT_ID(pvt), pvt->icard);

    const snd_pcm_state_t state = snd_pcm_state(pvt->icard);
    switch (state) {
        case SND_PCM_STATE_XRUN: {
            const int res = snd_pcm_prepare(pvt->ocard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][PLAYBACK] Prepare failed - err:'%s'\n", PVT_ID(pvt), snd_strerror(res));
            }
        }

        case SND_PCM_STATE_SETUP: {
            const int res = snd_pcm_prepare(pvt->icard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Prepare failed - state:%s err:'%s'\n", PVT_ID(pvt), snd_pcm_state_name(state), snd_strerror(res));
            }

            return NULL;
        }

        case SND_PCM_STATE_PREPARED:
        case SND_PCM_STATE_RUNNING:
            break;

        default:
            ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Device state: %s\n", PVT_ID(pvt), snd_pcm_state_name(state));
            return NULL;
    }

#if 1
    const snd_pcm_sframes_t avail_frames = snd_pcm_avail_update(pvt->icard);
    if (avail_frames < 0) {
        ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Cannot determine number of available audio frames: %s\n", PVT_ID(pvt), snd_strerror((int)avail_frames));
        return NULL;
    } else if (frames > (size_t)avail_frames) {
        ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE][F:%d] Not enough audio frames: %d\n", PVT_ID(pvt), (int)frames, (int)avail_frames);
        return NULL;
    } else {
        const snd_pcm_sframes_t limit = 4 * frames;
        if (avail_frames >= limit) {
            const snd_pcm_sframes_t skipped = snd_pcm_forward(pvt->icard, avail_frames - (2 * frames));
            ast_log(LOG_NOTICE, "[%s][ALSA][CAPTURE][F:%d] Too many audio frames available: %d, skipped %d\n", PVT_ID(pvt), (int)frames, (int)avail_frames,
                    (int)skipped);
        }
    }
#endif

    void* const buf = cpvt_get_buffer(cpvt);
    const int res   = snd_pcm_mmap_readi(pvt->icard, buf, frames);

    switch (res) {
        case -EAGAIN:
            ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE] Error - try again later\n", PVT_ID(pvt));
            break;

        case -EPIPE:
        case -ESTRPIPE:
            break;

        default:
            if (res > 0) {
                if (CPVT_IS_MASTER(cpvt)) {
                    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY)) {
                        write_conference(pvt, buf, res);
                    }

                    PVT_STAT(pvt, a_read_bytes) += res * sizeof(int16_t);
                    PVT_STAT(pvt, read_frames)++;
                    if (res < frames) {
                        PVT_STAT(pvt, read_sframes)++;
                    }
                }

                if (res < frames) {
                    ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE] Short frame: %d/%d\n", PVT_ID(pvt), res, (int)frames);
                }

                return cpvt_prepare_voice_frame(cpvt, buf, res, fmt);
            } else if (res < 0) {
                ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Read error: %s\n", PVT_ID(pvt), snd_strerror(res));
            }
            break;
    }

    return NULL;
}

static struct ast_frame* channel_read(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    struct ast_frame* f     = NULL;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt || CPVT_IS_LOCAL(cpvt)) {
        return &ast_null_frame;
    }

    SCOPED_CPVT(cpvtl, cpvt);
    struct pvt* const pvt = cpvt->pvt;

    ast_debug(8, "[%s] Read - idx:%d state:%s audio_fd:%d\n", PVT_ID(pvt), cpvt->call_idx, call_state2str(cpvt->state), pvt->audio_fd);

    const int fdno                     = ast_channel_fdno(channel);
    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t frame_size            = pvt_get_audio_frame_size(PTIME_CAPTURE, fmt);

    /* FIXME: move down for enable timing_write() to device ? */
    if (CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE && (!CPVT_IS_SOUND_SOURCE(cpvt) || pvt->audio_fd < 0)) {
        goto f_ret;
    }

    if (fdno == 1) {
        ast_timer_ack(pvt->a_timer, 1);
        if (CPVT_IS_MASTER(cpvt)) {
            if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
                // TODO: implement timing_write_uac
                ast_log(LOG_WARNING, "[%s] Multiparty calls not supported in UAC mode\n", PVT_ID(pvt));
            } else {
                timing_write_tty(pvt, frame_size);
            }
            ast_debug(7, "[%s] *** timing ***\n", PVT_ID(pvt));
        }
        goto f_ret;
    }

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE && CPVT_IS_MASTER(cpvt)) {
        f = channel_read_uac(cpvt, pvt, frame_size / sizeof(int16_t), fmt);
    } else {
        f = channel_read_tty(cpvt, pvt, frame_size, fmt);
    }

f_ret:
    if (f == NULL || f->frametype == AST_FRAME_NULL) {
        const int fd = ast_channel_fd(channel, 0);
        ast_debug(5, "[%s] Read - idx:%d state:%s audio:%d:%d - returning SILENCE frame\n", PVT_ID(pvt), cpvt->call_idx, call_state2str(cpvt->state), fd,
                  pvt->audio_fd);
        return cpvt_prepare_silence_voice_frame(cpvt, frame_size / sizeof(int16_t), fmt);
    } else {
        ast_debug(8, "[%s] Read - idx:%d state:%s samples:%d\n", PVT_ID(pvt), cpvt->call_idx, call_state2str(cpvt->state), f->samples);
        return f;
    }
}

static int channel_write_tty(struct ast_channel* channel, struct ast_frame* f, struct cpvt* cpvt, struct pvt* pvt)
{
    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && !CPVT_TEST_FLAG(cpvt, CALL_FLAG_BRIDGE_CHECK)) {
        RAII_VAR(struct ast_channel*, bridged, ast_channel_bridge_peer(channel), ast_channel_cleanup);
        struct cpvt* tmp_cpvt;

        CPVT_SET_FLAG(cpvt, CALL_FLAG_BRIDGE_CHECK);

        if (bridged && ast_channel_tech(bridged) == &channel_tech && (tmp_cpvt = ast_channel_tech_pvt(bridged)) && tmp_cpvt->pvt == pvt) {
            CPVT_SET_FLAG(cpvt, CALL_FLAG_BRIDGE_LOOP);
            CPVT_SET_FLAG((struct cpvt*)ast_channel_tech_pvt(bridged), CALL_FLAG_BRIDGE_LOOP);
            ast_log(LOG_WARNING, "[%s] Bridged channels %s and %s working on same device, discard writes to avoid voice loop\n", PVT_ID(pvt),
                    ast_channel_name(channel), ast_channel_name(bridged));
            return 0;
        }
    }

    if (CONF_SHARED(pvt, multiparty) && f->datalen) {
        /** try to minimize of ast_frame_adjust_volume() calls:
         *  one hand we must obey txgain but with other divide gain to
         *  number of mixed channels. In some cases one call of ast_frame_adjust_volume() enough
         */

        int gains[2];

        gains[1] = mixb_streams(&pvt->write_mixb);
        if (gains[1] < 1 || pvt->a_timer == NULL) {
            gains[1] = 1;
        }

        gains[0] = 0;

        for (size_t count = 0; count < ARRAY_LEN(gains); ++count) {
            if (gains[count] > 1 || gains[count] < -1) {
                if (ast_frame_adjust_volume(f, gains[count]) == -1) {
                    ast_debug(1, "[%s] Volume could not be adjusted!\n", PVT_ID(pvt));
                }
            }
        }
    }

    if (CONF_SHARED(pvt, multiparty)) {  // use mix buffer
        const size_t count = mixb_free(&pvt->write_mixb, &cpvt->mixstream);

        if (count < (size_t)f->datalen) {
            mixb_read_upd(&pvt->write_mixb, f->datalen - count);

            PVT_STAT(pvt, write_rb_overflow_bytes) += f->datalen - count;
            PVT_STAT(pvt, write_rb_overflow)++;
        }

        mixb_write(&pvt->write_mixb, &cpvt->mixstream, f->data.ptr, f->datalen);

        /*
                ast_debug (6, "[%s] write | call idx %d, %d bytes lwrite %d lused %d write %d used %d\n", PVT_ID(pvt),
           cpvt->call_idx, f->datalen, cpvt->write, cpvt->used, pvt->a_write_rb.write, pvt->a_write_rb.used);
                rb_tetris(&pvt->a_write_rb, f->data.ptr, f->datalen, &cpvt->write, &cpvt->used);
                ast_debug (6, "[%s] write | lwrite %d lused %d write %d used %d\n", PVT_ID(pvt), cpvt->write,
           cpvt->used, pvt->a_write_rb.write, pvt->a_write_rb.used);
        */
    } else if (CPVT_IS_ACTIVE(cpvt)) {  // direct write
        ast_frame_byteswap_le(f);

        struct iovec const iov = {.iov_base = f->data.ptr, .iov_len = f->datalen};
        const ssize_t res      = iov_write(pvt, pvt->audio_fd, &iov, 1);

        if (res >= 0) {
            PVT_STAT(pvt, write_frames)  += 1;
            PVT_STAT(pvt, a_write_bytes) += res;
            if (res != f->datalen) {
                PVT_STAT(pvt, write_tframes)++;
            }
        }
    }

    ast_debug(7, "[%s] Write frame - samples:%d bytes:%d\n", PVT_ID(pvt), f->samples, f->datalen);
    return 0;
}

static int channel_write_uac(struct ast_channel* attribute_unused(channel), struct ast_frame* f, struct cpvt* attribute_unused cpvt, struct pvt* pvt)
{
    const int samples = f->samples;
    int res           = 0;

    pcm_show_state(6, "PLAYBACK", PVT_ID(pvt), pvt->ocard);

    const snd_pcm_state_t state = snd_pcm_state(pvt->ocard);
    switch (state) {
        case SND_PCM_STATE_XRUN: {
            res = snd_pcm_prepare(pvt->icard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Prepare failed - err:'%s'\n", PVT_ID(pvt), snd_strerror(res));
                goto w_finish;
            }
        }
        case SND_PCM_STATE_SETUP:
            res = snd_pcm_prepare(pvt->ocard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][PLAYBACK] Prepare failed - state:%s err:'%s'\n", PVT_ID(pvt), snd_pcm_state_name(state), snd_strerror(res));
                goto w_finish;
            }
            break;

        case SND_PCM_STATE_PREPARED:
        case SND_PCM_STATE_RUNNING:
            break;

        default:
            ast_log(LOG_ERROR, "[%s][ALSA][PLAYBACK] Device state: %s\n", PVT_ID(pvt), snd_pcm_state_name(state));
            res = -1;
            goto w_finish;
    }

    ast_frame_byteswap_le(f);
    if (pvt->ocard_channels == 1u) {
        res = snd_pcm_mmap_writei(pvt->ocard, f->data.ptr, samples);
    } else {
        void* d[pvt->ocard_channels];
        for (unsigned int i = 0; i < pvt->ocard_channels; ++i) {
            d[i] = f->data.ptr;
        }
        res = snd_pcm_mmap_writen(pvt->ocard, (void**)&d, samples);
    }

    switch (res) {
        case -EAGAIN:
            ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] Error - try again later\n", PVT_ID(pvt));
            res = 0;
            break;

        case -EPIPE:
        case -ESTRPIPE:
            res = 0;
            break;

        default:
            if (res >= 0) {
                PVT_STAT(pvt, write_frames)  += 1;
                PVT_STAT(pvt, a_write_bytes) += res * sizeof(int16_t);
                if (res != samples) {
                    PVT_STAT(pvt, write_tframes)++;
                    ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] Write: %d/%d\n", PVT_ID(pvt), res, samples);
                }
            }
            break;
    }

w_finish:

    return res;
}

#/* */

static int channel_write(struct ast_channel* channel, struct ast_frame* f)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    int res                 = -1;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt || CPVT_IS_LOCAL(cpvt)) {
        return 0;
    }

    /* TODO: write silence better ? */
    /* TODO: check end of bridge loop condition */
    /* never write to same device from other channel its possible for call hold or conference */
    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_BRIDGE_LOOP)) {
        return 0;
    }

    SCOPED_CPVT(cpvtl, cpvt);
    struct pvt* const pvt = cpvt->pvt;

    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t frame_size            = pvt_get_audio_frame_size(PTIME_PLAYBACK, fmt);

    if (f->frametype != AST_FRAME_VOICE || ast_format_cmp(f->subclass.format, fmt) != AST_FORMAT_CMP_EQUAL) {
        ast_debug(1, "[%s] Unsupported audio codec: %s\n", PVT_ID(pvt), ast_format_get_name(f->subclass.format));
        return 0;
    }

    ast_debug(8, "[%s] Write - idx:%d state:%s\n", PVT_ID(pvt), cpvt->call_idx, call_state2str(cpvt->state));

    if (f->datalen < frame_size) {
        ast_debug(8, "[%s] Short voice frame: %d/%d, samples:%d\n", PVT_ID(pvt), f->datalen, (int)frame_size, f->samples);
        PVT_STAT(pvt, write_tframes)++;
    } else if (f->datalen > frame_size) {
        ast_debug(8, "[%s] Large voice frame: %d/%d, samples: %d\n", PVT_ID(pvt), f->datalen, (int)frame_size, f->samples);
    }

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE && CPVT_IS_MASTER(cpvt)) {
        res = channel_write_uac(channel, f, cpvt, pvt);
    } else {
        res = channel_write_tty(channel, f, cpvt, pvt);
    }

    return res >= 0 ? 0 : -1;
}

#/* */

static int channel_fixup(struct ast_channel* oldchannel, struct ast_channel* newchannel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(newchannel);

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(newchannel));
        return -1;
    }

    SCOPED_CPVT(cpvtl, cpvt);

    if (cpvt->channel == oldchannel) {
        cpvt->channel = newchannel;
    }

    return 0;
}

/* FIXME: must modify in conjuction with state on call not whole device? */
static int channel_devicestate(const char* data)
{
    int res = AST_DEVICE_INVALID;

    const char* const device = ast_strdupa(S_OR(data, ""));
    ast_debug(1, "[%s] Checking device state\n", device);


    RAII_VAR(struct pvt* const, pvt, pvt_find_by_ext(device), pvt_unlock);

    if (!pvt) {
        return res;
    }

    if (pvt->connected) {
        if (pvt_is_dial_possible(pvt, CALL_FLAG_NONE)) {
            res = AST_DEVICE_NOT_INUSE;
        } else {
            res = AST_DEVICE_INUSE;
        }
    } else {
        res = AST_DEVICE_UNAVAILABLE;
    }
    return res;
}

#/* */

static int channel_indicate(struct ast_channel* channel, int condition, const void* data, attribute_unused size_t datalen)
{
    ast_debug(1, "[%s] Requested indication %d\n", ast_channel_name(channel), condition);

    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    struct pvt* pvt         = NULL;
    int res                 = 0;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(channel));
    } else {
        pvt = cpvt->pvt;
    }

    switch (condition) {
        case AST_CONTROL_BUSY:
        case AST_CONTROL_CONGESTION:
        case AST_CONTROL_RINGING:
        case AST_CONTROL_MASQUERADE_NOTIFY:
        case AST_CONTROL_PVT_CAUSE_CODE:
        case -1:
            res = -1;
            break;

/* appears in r295843 */
#ifdef HAVE_AST_CONTROL_SRCCHANGE
        case AST_CONTROL_SRCCHANGE:
#endif
        case AST_CONTROL_PROGRESS:
        case AST_CONTROL_PROCEEDING:
        case AST_CONTROL_VIDUPDATE:
        case AST_CONTROL_SRCUPDATE:
            break;

        case AST_CONTROL_HOLD:
            if (!pvt || CONF_SHARED(pvt, moh)) {
                ast_moh_start(channel, data, NULL);
            } else {
                SCOPED_CPVT(cpvtl, cpvt);
                at_enqueue_mute(cpvt, 1);
            }
            break;

        case AST_CONTROL_UNHOLD:
            if (!pvt || CONF_SHARED(pvt, moh)) {
                ast_moh_stop(channel);
            } else {
                SCOPED_CPVT(cpvtl, cpvt);
                at_enqueue_mute(cpvt, 0);
            }
            break;

        case AST_CONTROL_CONNECTED_LINE: {
            struct ast_party_connected_line* const cnncd = ast_channel_connected(channel);
            SCOPED_CPVT(cpvtl, cpvt);
            ast_log(LOG_NOTICE, "[%s] Connected party is now %s <%s>\n", PVT_ID(pvt), S_COR(cnncd->id.name.valid, cnncd->id.name.str, ""),
                    S_COR(cnncd->id.number.valid, cnncd->id.number.str, ""));
            break;
        }

        default:
            ast_log(LOG_WARNING, "[%s] Don't know how to indicate condition %d\n", ast_channel_name(channel), condition);
            res = -1;
            break;
    }

    return res;
}

#/* */

static void set_channel_vars(struct pvt* pvt, struct ast_channel* channel)
{
    RAII_VAR(struct ast_json*, qdata, ast_json_object_create(), ast_json_unref);
    pvt_get_status(pvt, qdata);
    setvar_helper_json(pvt, channel, "QUECTEL", qdata);
    ast_channel_language_set(channel, CONF_SHARED(pvt, language));
}

/* NOTE: called from device and current levels with locked pvt */
struct ast_channel* channel_new(struct pvt* pvt, int ast_state, const char* cid_num, int call_idx, unsigned dir, int state, const char* dnid,
                                const struct ast_assigned_ids* assignedids, attribute_unused const struct ast_channel* requestor, unsigned local_channel)
{
    struct cpvt* const cpvt = cpvt_alloc(pvt, call_idx, dir, CALL_STATE_INIT, local_channel);
    if (!cpvt) {
        return NULL;
    }

    struct ast_channel* const channel = ast_channel_alloc(1, ast_state, cid_num, PVT_ID(pvt), NULL, dnid, CONF_SHARED(pvt, context), assignedids, requestor, 0,
                                                          "%s/%s-%02u%08lx", channel_tech.type, PVT_ID(pvt), call_idx, pvt->channel_instance);
    if (!channel) {
        cpvt_free(cpvt);
        return NULL;
    }

    cpvt->channel = channel;
    pvt->channel_instance++;

    ast_channel_stage_snapshot(channel);

    ast_channel_tech_pvt_set(channel, cpvt);
    ast_channel_tech_set(channel, &channel_tech);

    if (local_channel) {
        struct ast_format_cap* const cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
        ast_format_cap_append_by_type(cap, AST_MEDIA_TYPE_TEXT);
        ast_channel_nativeformats_set(channel, cap);
        ao2_ref(cap, -1);
    } else {
        struct ast_format* const fmt = (struct ast_format*)pvt_get_audio_format(pvt);
#if PTIME_USE_DEFAULT
        const unsigned int ms = ast_format_get_default_ms(fmt);
#else
        static const unsigned int ms = PTIME_CAPTURE;
#endif
        struct ast_format_cap* const cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
        ast_format_cap_append(cap, fmt, ms);
        ast_format_cap_set_framing(cap, ms);
        ast_channel_nativeformats_set(channel, cap);
        ao2_ref(cap, -1);

        ast_channel_set_rawreadformat(channel, fmt);
        ast_channel_set_rawwriteformat(channel, fmt);
        ast_channel_set_writeformat(channel, fmt);
        ast_channel_set_readformat(channel, fmt);

        ast_channel_set_fd(channel, 0, pvt->audio_fd);
        if (pvt->a_timer) {
            ast_channel_set_fd(channel, 1, ast_timer_fd(pvt->a_timer));
            ast_timer_set_rate(pvt->a_timer, 50);
        }
    }

    set_channel_vars(pvt, channel);

    if (!ast_strlen_zero(dnid)) {
        setvar_helper(pvt, channel, "CALLERID(dnid)", dnid);
    }

    if (state != CALL_STATE_INIT) {
        cpvt_change_state(cpvt, (call_state_t)state, AST_CAUSE_NORMAL_UNSPECIFIED);
    }

    ast_channel_stage_snapshot_done(channel);
    ast_channel_unlock(channel);
    return channel;
}

/* NOTE: bg: hmm ast_queue_hangup() say no need channel lock before call, trylock got deadlock up to 30 seconds here */
/* NOTE: bg: called from device level and change_channel_state() with pvt locked */
int channel_enqueue_hangup(struct ast_channel* channel, int hangupcause)
{
    if (!channel) {
        return -1;
    }

    if (hangupcause) {
        ast_channel_hangupcause_set(channel, hangupcause);
    }

    return ast_queue_hangup(channel);
}

void channel_start_local_report(struct pvt* pvt, const char* subject, local_report_direction direction, const char* number, const struct ast_tm* ts,
                                const struct ast_tm* dt, int success, struct ast_json* const report)
{
    static const size_t AST_TM_MAX_LEN = 64;

    RAII_VAR(struct ast_json*, rprt, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(rprt, "subject", ast_json_string_create(subject));
    switch (direction) {
        case LOCAL_REPORT_DIRECTION_INCOMING:
            ast_json_object_set(rprt, "direction", ast_json_string_create("incoming"));
            break;

        case LOCAL_REPORT_DIRECTION_OUTGOING:
            ast_json_object_set(rprt, "direction", ast_json_string_create("outgoing"));
            break;

        default:
            break;
    }

    if (ts && ts->tm_year) {
        struct ast_str* buf = ast_str_alloca(AST_TM_MAX_LEN);
        format_ast_tm(ts, buf);
        ast_json_object_set(rprt, "ts", ast_json_string_create(ast_str_buffer(buf)));
    }

    if (dt && dt->tm_year) {
        struct ast_str* buf = ast_str_alloca(AST_TM_MAX_LEN);
        format_ast_tm(dt, buf);
        ast_json_object_set(rprt, "dt", ast_json_string_create(ast_str_buffer(buf)));
    }

    // ast_json_object_set(rprt, "success", ast_json_boolean(success));
    ast_json_object_set(rprt, "success", ast_json_integer_create(!!success));

    if (!ast_strlen_zero(number)) {
        ast_json_object_set(rprt, "number", ast_json_string_create(number));
    }

    if (report) {
        ast_json_object_set(rprt, "report", ast_json_copy(report));
    }

    channel_start_local_json(pvt, "report", number, "REPORT", rprt);
}

#/* NOTE: bg: called from device level with pvt locked */

void channel_start_local(struct pvt* pvt, const char* exten, const char* number, const channel_var_t* const vars, const size_t varscnt)
{
    static const ssize_t CN_DEF_LEN = 64;

    RAII_VAR(struct ast_str*, channel_name, ast_str_create(CN_DEF_LEN), ast_free);
    ast_str_set(&channel_name, 0, "%s@%s", exten, CONF_SHARED(pvt, context));

    int cause = 0;

    struct ast_channel* const channel =
        ast_request("Local", pvt->local_format_cap ? pvt->local_format_cap : channel_tech.capabilities, NULL, NULL, ast_str_buffer(channel_name), &cause);
    if (!channel) {
        ast_log(LOG_ERROR, "[%s] Unable to request channel Local/%s: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(channel_name), ast_cause2str(cause), cause);
        return;
    }

    set_channel_vars(pvt, channel);
    ast_set_callerid(channel, number, PVT_ID(pvt), number);

    for (size_t i = 0; i < varscnt; ++i) {
        setvar_helper(pvt, channel, vars[i].name, vars[i].value);
    }

    if (ast_pbx_start(channel)) {
        ast_hangup(channel);
        ast_log(LOG_ERROR, "[%s] Unable to start pbx on channel Local/%s\n", PVT_ID(pvt), ast_str_buffer(channel_name));
    }
}

void channel_start_local_json(struct pvt* pvt, const char* exten, const char* number, const char* const jname, const struct ast_json* const jvar)
{
    RAII_VAR(char* const, jstr, ast_json_dump_string((struct ast_json*)jvar), ast_json_free);
    const channel_var_t var = {jname, jstr};
    channel_start_local(pvt, exten, number, &var, 1);
}

#/* */

static int channel_func_read(struct ast_channel* channel, attribute_unused const char* function, char* data, char* buf, size_t len)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    int ret                 = 0;

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }

    if (!strcasecmp(data, "callstate")) {
        SCOPED_CPVT(cpvtl, cpvt);
        call_state_t state = cpvt->state;
        ast_copy_string(buf, call_state2str(state), len);
    } else {
        ret = -1;
    }

    return ret;
}

#/* */

static int channel_func_write(struct ast_channel* channel, const char* function, char* data, const char* value)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    call_state_t newstate, oldstate;
    int ret = 0;

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }

    if (!strcasecmp(data, "callstate")) {
        if (!strcasecmp(value, "active")) {
            newstate = CALL_STATE_ACTIVE;
        } else {
            ast_log(LOG_WARNING, "Invalid value for %s(callstate).\n", function);
            return -1;
        }

        SCOPED_CPVT(cpvtl, cpvt);
        oldstate = cpvt->state;

        if (oldstate == newstate)
            ;
        else if (oldstate == CALL_STATE_ONHOLD) {
            if (at_enqueue_activate(cpvt)) {
                /* TODO: handle error */
                ast_log(LOG_ERROR, "Error state to active for call idx %d in %s(callstate).\n", cpvt->call_idx, function);
            }
        } else {
            ast_log(LOG_WARNING, "allow change state to 'active' only from 'held' in %s(callstate).\n", function);
            ret = -1;
        }
    } else {
        ret = -1;
    }

    return ret;
}

struct ast_channel_tech channel_tech = {.type               = "Quectel",
                                        .description        = MODULE_DESCRIPTION,
                                        .requester          = channel_request,
                                        .call               = channel_call,
                                        .hangup             = channel_hangup,
                                        .answer             = channel_answer,
                                        .send_digit_begin   = channel_digit_begin,
                                        .send_digit_end     = channel_digit_end,
                                        .read               = channel_read,
                                        .write              = channel_write,
                                        .exception          = channel_read,
                                        .fixup              = channel_fixup,
                                        .devicestate        = channel_devicestate,
                                        .indicate           = channel_indicate,
                                        .func_channel_read  = channel_func_read,
                                        .func_channel_write = channel_func_write};
