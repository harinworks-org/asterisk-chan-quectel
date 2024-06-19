/*
    pcm.c
*/

#include <ptime-config.h>

#include "ast_config.h"

#include <asterisk/app.h>
#include <asterisk/logger.h>

#include "pcm.h"

static const snd_pcm_format_t pcm_format = SND_PCM_FORMAT_S16_LE;

void _pcm_show_state(int attribute_unused lvl, const char* file, int line, const char* function, const char* const pcm_desc, const char* const pvt_id,
                     snd_pcm_t* const pcm)
{
    const size_t ss                    = snd_pcm_status_sizeof();
    snd_pcm_status_t* const pcm_status = (snd_pcm_status_t*)ast_alloca(ss);

    int res = snd_pcm_status(pcm, pcm_status);
    if (res < 0) {
        ast_log(__LOG_ERROR, file, line, function, "[%s][ALSA][%s] Unable to get device status: %s", pvt_id, pcm_desc, snd_strerror(res));
        return;
    }

    const snd_pcm_state_t pcm_state = snd_pcm_status_get_state(pcm_status);
    snd_pcm_sframes_t delay         = snd_pcm_status_get_delay(pcm_status);
    snd_pcm_uframes_t avail         = snd_pcm_status_get_avail(pcm_status);

    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;

    res = snd_pcm_get_params(pcm, &buffer_size, &period_size);
    if (res < 0) {
        ast_log(__LOG_ERROR, file, line, function, "[%s][ALSA][%s] Unable to get buffer sizes: %s", pvt_id, pcm_desc, snd_strerror(res));
    }

    snd_pcm_uframes_t avail_period_buffers = 0;
    snd_pcm_uframes_t delay_period_buffers = 0;
    if (res >= 0) {
        avail_period_buffers  = avail / period_size;
        avail                %= period_size;
        delay_period_buffers  = delay / period_size;
        delay                %= period_size;
    }

    ast_log(__LOG_DEBUG, file, line, function, "[%s][ALSA][%s] Status - state:%s delay:%ld:%ld avail:%lu:%lu\n", pvt_id, pcm_desc,
            snd_pcm_state_name(pcm_state), delay_period_buffers, delay, avail_period_buffers, avail);
}

static snd_pcm_uframes_t adjust_uframes(snd_pcm_uframes_t ptime, unsigned int rate)
{
    snd_pcm_uframes_t res  = ptime;
    res                   *= rate / 1000;

    return res;
}

static snd_pcm_uframes_t adjust_start_threshold(snd_pcm_uframes_t ptime)
{
    static const size_t PTIME_MIN_START_THRESHOLD = 100u;
    static const size_t PTIME_MAX_START_THRESHOLD = 250u;

    if (ptime < PTIME_MIN_START_THRESHOLD) {
        return PTIME_MAX_START_THRESHOLD;
    }
    if (ptime > PTIME_MIN_START_THRESHOLD) {
        return PTIME_MAX_START_THRESHOLD;
    }

    return ptime;
}

static unsigned int hw_params_get_rate(const snd_pcm_hw_params_t* const params)
{
    static const unsigned int UNKNOWN_RATE = 0xffff;

    unsigned int rate;
    const int res = snd_pcm_hw_params_get_rate(params, &rate, NULL);
    if (res >= 0) {
        return rate;
    } else {
        return UNKNOWN_RATE;
    }
}

int pcm_init(const char* dev, snd_pcm_stream_t stream, const struct ast_format* const fmt, snd_pcm_t** pcm, unsigned int* pcm_channels, int* fd)
{
    int res;
    snd_pcm_t* handle             = NULL;
    snd_pcm_hw_params_t* hwparams = NULL;
    snd_pcm_sw_params_t* swparams = NULL;
    unsigned int rate             = ast_format_get_sample_rate(fmt);

#if PTIME_USE_DEFAULT
    const size_t ptime = ast_format_get_default_ms(fmt);
#else
    const size_t ptime = (stream == SND_PCM_STREAM_CAPTURE) ? PTIME_CAPTURE : PTIME_PLAYBACK;
#endif
    snd_pcm_uframes_t period_size     = adjust_uframes(ptime + 10, rate);
    snd_pcm_uframes_t buffer_size     = adjust_uframes(PTIME_BUFFER, rate);
    snd_pcm_uframes_t start_threshold = adjust_uframes(adjust_start_threshold(ptime * 2), rate);
    snd_pcm_uframes_t stop_threshold  = buffer_size - period_size;
    snd_pcm_uframes_t boundary        = 0u;
    unsigned int hwrate               = rate;
    unsigned int channels             = 1;

    const char* const stream_str = (stream == SND_PCM_STREAM_CAPTURE) ? "CAPTURE" : "PLAYBACK";

    res = snd_pcm_open(&handle, dev, stream, SND_PCM_NONBLOCK);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] Fail to open device - dev:'%s' err:'%s'\n", stream_str, dev, snd_strerror(res));
        return res;
    } else {
        ast_debug(1, "[ALSA][%s] Device: %s\n", stream_str, dev);
    }

    hwparams = ast_alloca(snd_pcm_hw_params_sizeof());
    memset(hwparams, 0, snd_pcm_hw_params_sizeof());
    snd_pcm_hw_params_any(handle, hwparams);

    res = snd_pcm_hw_params_set_access(handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] HW Set access failed: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_set_format(handle, hwparams, pcm_format);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] HW Set format failed: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_set_channels_near(handle, hwparams, &channels);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] HW Set channels failed: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_set_rate(handle, hwparams, hwrate, 0);
    if (hwrate != rate) {
        ast_log(LOG_WARNING, "[ALSA][%s] HW Rate not correct -  requested:%d got:%u\n", stream_str, rate, hwrate);
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_set_period_size_near(handle, hwparams, &period_size, NULL);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] HW Period size (%lu frames) is bad: %s\n", stream_str, period_size, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_set_buffer_size_near(handle, hwparams, &buffer_size);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] HW Problem setting buffer size of %lu: %s\n", stream_str, buffer_size, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params(handle, hwparams);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] Couldn't set HW params: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_current(handle, hwparams);
    if (res < 0) {
        ast_log(LOG_WARNING, "[ALSA][%s] HW Couldn't get current HW params: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_hw_params_get_channels(hwparams, &channels);
    if (res >= 0) {
        const unsigned int max_channels = (stream == SND_PCM_STREAM_CAPTURE) ? 1 : 2;
        if (channels > max_channels) {
            ast_log(LOG_ERROR, "[ALSA][%s] Too many channels: %u (max %u are supported)\n", stream_str, channels, max_channels);
            goto alsa_fail;
        }
        *pcm_channels = channels;
        ast_debug(1, "[ALSA][%s] Channels: %u\n", stream_str, channels);
    } else {
        ast_log(LOG_ERROR, "[ALSA][%s] Couldn't get channel count: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    ast_debug(1, "[ALSA][%s] Rate: %u\n", stream_str, hw_params_get_rate(hwparams));

    res = snd_pcm_hw_params_get_period_size(hwparams, &period_size, NULL);
    if (res >= 0) {
        ast_debug(1, "[ALSA][%s] Period size: %lu\n", stream_str, period_size);
    }

    res = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size);
    if (res >= 0) {
        ast_debug(1, "[ALSA][%s] Buffer size: %lu\n", stream_str, buffer_size);
    }

    swparams = ast_alloca(snd_pcm_sw_params_sizeof());
    memset(swparams, 0, snd_pcm_sw_params_sizeof());
    res = snd_pcm_sw_params_current(handle, swparams);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] Couldn't get SW params: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    res = snd_pcm_sw_params_get_boundary(swparams, &boundary);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] SW Couldn't get boundary: %s\n", stream_str, snd_strerror(res));
        boundary = 0u;
    }
    ast_debug(3, "[ALSA][%s] Boundary: %lu\n", stream_str, boundary);

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        ast_debug(2, "[ALSA][%s] Start threshold: %lu\n", stream_str, start_threshold);
        res = snd_pcm_sw_params_set_start_threshold(handle, swparams, start_threshold);

        if (res < 0) {
            ast_log(LOG_ERROR, "[ALSA][%s] SW Couldn't set start threshold: %s\n", stream_str, snd_strerror(res));
            goto alsa_fail;
        }

        ast_debug(2, "[ALSA][%s] Stop threshold: %lu, free: %lu\n", stream_str, stop_threshold, buffer_size - stop_threshold);
        res = snd_pcm_sw_params_set_stop_threshold(handle, swparams, stop_threshold);
        if (res < 0) {
            ast_log(LOG_ERROR, "[ALSA][%s] SW Couldn't set stop threshold: %s\n", stream_str, snd_strerror(res));
            goto alsa_fail;
        }
    }

    if (stream == SND_PCM_STREAM_PLAYBACK && boundary > 0u) {
        res = snd_pcm_sw_params_set_silence_threshold(handle, swparams, 0);
        if (res < 0) {
            ast_log(LOG_WARNING, "[ALSA][%s] SW Couldn't set silence threshold: %s\n", stream_str, snd_strerror(res));
        } else {
            res = snd_pcm_sw_params_set_silence_size(handle, swparams, boundary);
            if (res < 0) {
                ast_log(LOG_WARNING, "[ALSA][%s] SW Couldn't set silence size: %s\n", stream_str, snd_strerror(res));
            }
        }
    }

    res = snd_pcm_sw_params(handle, swparams);
    if (res < 0) {
        ast_log(LOG_ERROR, "[ALSA][%s] Couldn't set SW params: %s\n", stream_str, snd_strerror(res));
        goto alsa_fail;
    }

    if (stream == SND_PCM_STREAM_CAPTURE) {
        res = snd_pcm_poll_descriptors_count(handle);
        if (res <= 0) {
            ast_log(LOG_ERROR, "[ALSA][%s] Unable to get a poll descriptors count: %s\n", stream_str, snd_strerror(res));
            goto alsa_fail;
        }

        struct pollfd pfd[res];

        res = snd_pcm_poll_descriptors(handle, pfd, res);
        if (res < 0) {
            ast_log(LOG_ERROR, "[ALSA][%s] Unable to get a poll descriptor(s): %s\n", stream_str, snd_strerror(res));
            goto alsa_fail;
        }
        ast_debug(1, "[ALSA][%s] Acquired FD:%d from the poll descriptor(s)\n", stream_str, pfd[0].fd);
        if (fd) {
            *fd = pfd[0].fd;
        }
    }

    *pcm = handle;
    return 0;

alsa_fail:
    snd_pcm_close(handle);
    return res;
}

int pcm_close(const char* dev, snd_pcm_t** ad, snd_pcm_stream_t stream_type)
{
    if (*ad == NULL) {
        return 0;
    }
    const int res = snd_pcm_close(*ad);
    if (res < 0) {
        switch (stream_type) {
            case SND_PCM_STREAM_PLAYBACK:
                ast_log(LOG_ERROR, "[ALSA][%s] Failed to close playback device: %s", dev, snd_strerror(res));
                break;

            case SND_PCM_STREAM_CAPTURE:
                ast_log(LOG_ERROR, "[ALSA][%s] Failed to close capture device: %s", dev, snd_strerror(res));
                break;
        }
    }
    *ad = NULL;
    return res;
}

static int is_snd_pcm_disconnected(snd_pcm_t* const pcm)
{
    const snd_pcm_state_t state = snd_pcm_state(pcm);
    return (state == SND_PCM_STATE_DISCONNECTED);
}

int pcm_status(snd_pcm_t* const pcm_playback, snd_pcm_t* const pcm_capture)
{
    if (is_snd_pcm_disconnected(pcm_playback) || is_snd_pcm_disconnected(pcm_capture)) {
        return -1;
    }

    return 0;
}
