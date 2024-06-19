/*
   Copyright (C) 2009-2015

   bg <bg_one@mail.ru>
   http://www.e1550.mobi

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>
*/
#ifndef CHAN_QUECTEL_H_INCLUDED
#define CHAN_QUECTEL_H_INCLUDED

#include <sys/time.h>

#include "ast_config.h"

#include <asterisk/astobj2.h>
#include <asterisk/json.h>
#include <asterisk/localtime.h>
#include <asterisk/lock.h>
#include <asterisk/strings.h>
#include <asterisk/threadpool.h>

#include "at_command.h"
#include "cpvt.h"      /* struct cpvt */
#include "dc_config.h" /* pvt_config_t */
#include "mixbuffer.h" /* struct mixbuffer */
#include "pcm.h"

#define MAX_BUFFER_SIZE 100
#define MODULE_DESCRIPTION "Channel Driver for Mobile Telephony"
#define MAXQUECTELDEVICES 128

const char* dev_state2str(dev_state_t state);
const char* dev_state2str_capitalized(dev_state_t state);
dev_state_t str2dev_state(const char*);
const char* dev_state2str_msg(dev_state_t state);

typedef enum {
    RESTATE_TIME_NOW = 0,
    RESTATE_TIME_GRACEFULLY,
    RESTATE_TIME_CONVENIENT,
} restate_time_t;

/* state */
typedef struct pvt_state {
    uint32_t at_tasks;                      /*!< number of active tasks in at_queue */
    uint32_t at_cmds;                       /*!< number of active commands in at_queue */
    uint32_t chansno;                       /*!< number of channels in channels list */
    uint8_t chan_count[CALL_STATES_NUMBER]; /*!< channel number grouped by state */
} pvt_state_t;

#define PVT_STATE_T(state, name) ((state)->name)

/* statictics */
typedef struct pvt_stat {
    uint32_t at_tasks;     /*!< number of tasks added to queue */
    uint32_t at_cmds;      /*!< number of commands added to queue */
    uint32_t at_responses; /*!< number of responses handled */

    uint32_t d_read_bytes;  /*!< number of bytes of commands actually read from device */
    uint32_t d_write_bytes; /*!< number of bytes of commands actually written to device */

    uint64_t a_read_bytes;  /*!< number of bytes of audio read from device */
    uint64_t a_write_bytes; /*!< number of bytes of audio written to device */

    uint32_t read_frames;  /*!< number of frames read from device */
    uint32_t read_sframes; /*!< number of truncated frames read from device */

    uint32_t write_frames;  /*!< number of tries to frame write */
    uint32_t write_tframes; /*!< number of truncated frames to write */
    uint32_t write_sframes; /*!< number of silence frames to write */

    uint64_t write_rb_overflow_bytes; /*!< number of overflow bytes */
    uint32_t write_rb_overflow;       /*!< number of times when a_write_rb overflowed */

    uint32_t in_calls;         /*!< number of incoming calls not including waiting */
    uint32_t cw_calls;         /*!< number of waiting calls */
    uint32_t out_calls;        /*!< number of all outgoing calls attempts */
    uint32_t in_calls_handled; /*!< number of ncoming/waiting calls passed to dialplan */
    uint32_t in_pbx_fails;     /*!< number of start_pbx fails */

    uint32_t calls_answered[2]; /*!< number of outgoing and incoming/waiting calls answered */
    uint32_t calls_duration[2]; /*!< seconds of outgoing and incoming/waiting calls */
} pvt_stat_t;

#define PVT_STAT_T(stat, name) ((stat)->name)

struct at_queue_task;

typedef struct pvt {
    AST_LIST_HEAD_NOLOCK(, at_queue_task) at_queue; /*!< queue for commands to modem */

    AST_LIST_HEAD_NOLOCK(, cpvt) chans; /*!< list of channels */
    struct cpvt sys_chan;               /*!< system channel */

    unsigned long channel_instance; /*!< number of channels created on this device */
    pthread_t monitor_thread;       /*!< monitor (at commands reader) thread handle */
    int monitor_thread_event;

    int audio_fd; /*!< audio descriptor */
    snd_pcm_t* icard;
    snd_pcm_t* ocard;
    unsigned int ocard_channels;

    int data_fd; /*!< data descriptor */

    struct ast_timer* a_timer;   /*!< audio write timer */
    void* silence_buf;           //[FRAME_SIZE_PLAYBACK * 2];
    void* write_buf;             //[FRAME_SIZE_PLAYBACK * 5]; /*!< audio write buffer */
    struct mixbuffer write_mixb; /*!< audio mix buffer */

    /* device state */
    int gsm_reg_status;
    int act;
    int operator;
    int rssi;

    struct ast_format_cap* local_format_cap;

    /* SMS support */
    int incoming_sms_index;
    int incoming_sms_type;

    struct ast_tm module_time;

    // clang-format off
	/* string fields */
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(manufacturer);
		AST_STRING_FIELD(model);
		AST_STRING_FIELD(firmware);
		AST_STRING_FIELD(imei);
		AST_STRING_FIELD(imsi);
		AST_STRING_FIELD(iccid);
		AST_STRING_FIELD(network_name);
		AST_STRING_FIELD(short_network_name);
		AST_STRING_FIELD(provider_name);
		AST_STRING_FIELD(location_area_code);
		AST_STRING_FIELD(cell_id);
		AST_STRING_FIELD(band);
		AST_STRING_FIELD(sms_scenter);
		AST_STRING_FIELD(subscriber_number);
	);
    // clang-format on

    /* flags */
    volatile unsigned int connected:1; /*!< do we have an connection to a device */
    unsigned int initialized       :1; /*!< whether a service level connection exists or not */
    unsigned int gsm_registered    :1; /*!< do we have an registration to a GSM */
    unsigned int dialing           :1; /*!< HW state; true from ATD response OK until CEND or CONN for this call idx */
    unsigned int ring              :1; /*!< HW state; true if has incoming call from first RING until CEND or CONN */
    unsigned int cwaiting          :1; /*!< HW state; true if has incoming call waiting from first CCWA until CEND or CONN for */
    unsigned int outgoing_sms      :1; /*!< outgoing sms */
    unsigned int volume_sync_step  :2; /*!< volume synchronized stage */
#define VOLUME_SYNC_BEGIN 0
#define VOLUME_SYNC_DONE 3

    unsigned int has_sms         :1; /*!< device has SMS support */
    unsigned int has_voice       :1; /*!< device has voice call support */
    unsigned int is_simcom       :1; /*!< device is a simcom module */
    unsigned int has_call_waiting:1; /*!< call waiting enabled on device */

    unsigned int group_last_used:1; /*!< mark the last used device */
    unsigned int prov_last_used :1; /*!< mark the last used device */
    unsigned int sim_last_used  :1; /*!< mark the last used device */

    unsigned int has_subscriber_number:1; /*!< subscriber_number field is valid */
    unsigned int must_remove          :1; /*!< mean must removed from list: NOT FULLY THREADSAFE */

    volatile dev_state_t desired_state;   /*!< desired state */
    volatile restate_time_t restart_time; /*!< time when change state */
    volatile dev_state_t current_state;   /*!< current state */

    pvt_config_t settings; /*!< all device settings from config file */
    pvt_state_t state;     /*!< state */
    pvt_stat_t stat;       /*!< various statistics */

    struct ast_str empty_str; /*!< empty string */
} pvt_t;

#define CONF_GLOBAL(name) (gpublic->global_settings.name)
#define SCONF_GLOBAL(state, name) ((state)->global_settings.name)

#define CONF_SHARED(pvt, name) SCONFIG(&((pvt)->settings), name)
#define CONF_UNIQ(pvt, name) UCONFIG(&((pvt)->settings), name)
#define PVT_ID(pvt) UCONFIG(&((pvt)->settings), id)

#define PVT_STATE(pvt, name) PVT_STATE_T(&(pvt)->state, name)
#define PVT_STAT(pvt, name) PVT_STAT_T(&(pvt)->stat, name)

typedef struct public_state {
    struct ao2_container* pvts;
    struct ast_threadpool* threadpool;
    pthread_t dev_manager_thread;
    int dev_manager_event;
    struct dc_gconfig global_settings;
} public_state_t;

extern public_state_t* gpublic;

int pvt_get_pseudo_call_idx(const struct pvt* pvt);
int pvt_ready4voice_call(const struct pvt* pvt, const struct cpvt* current_cpvt, unsigned int opts);
int pvt_is_dial_possible(const struct pvt* pvt, unsigned int opts);

const char* pvt_str_state(const struct pvt* pvt);
struct ast_str* pvt_str_state_ex(const struct pvt* pvt);
const char* pvt_str_call_dir(const struct pvt* pvt);

void pvt_get_status(const struct pvt* const pvt, struct ast_json* status);

void pvt_on_create_1st_channel(struct pvt* pvt);
void pvt_on_remove_last_channel(struct pvt* pvt);
void pvt_reload(restate_time_t when);
int pvt_enabled(const struct pvt* pvt);
void pvt_try_restate(struct pvt* pvt);
int pvt_set_act(struct pvt* pvt, int act);

const struct ast_format* pvt_get_audio_format(const struct pvt* const);
size_t pvt_get_audio_frame_size(unsigned int, const struct ast_format* const);
void* pvt_get_silence_buffer(struct pvt* const);

/* direct device write, dangerouse */
int pvt_direct_write(struct pvt* pvt, const char* buf, size_t count);

static inline int pvt_direct_write_str(struct pvt* pvt, struct ast_str* str) { return pvt_direct_write(pvt, ast_str_buffer(str), ast_str_strlen(str)); }

void pvt_disconnect(struct pvt* pvt);

struct pvt* pvt_find_ex(struct public_state* state, const char* name);

static inline struct pvt* pvt_find(const char* name) { return pvt_find_ex(gpublic, name); }

struct pvt* pvt_find_by_ext(const char* name);
struct pvt* pvt_find_by_resource_ex(struct public_state* state, const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists);
struct pvt* pvt_msg_find_by_resource_ex(struct public_state* state, const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists);

static inline struct pvt* pvt_find_by_resource(const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists)
{
    return pvt_find_by_resource_ex(gpublic, resource, opts, requestor, exists);
}

static inline struct pvt* pvt_msg_find_by_resource(const char* resource, unsigned int opts, const struct ast_channel* requestor, int* exists)
{
    return pvt_msg_find_by_resource_ex(gpublic, resource, opts, requestor, exists);
}

struct cpvt* pvt_channel_find_by_call_idx(struct pvt* pvt, int call_idx);
struct cpvt* pvt_channel_find_active(struct pvt* pvt);
struct cpvt* pvt_channel_find_last_initialized(struct pvt* pvt);

int pvt_lock(struct pvt* const pvt);
int pvt_unlock(struct pvt* const pvt);

int pvt_taskproc_trylock_and_execute(struct pvt* pvt, void (*task_exe)(struct pvt* pvt), const char* task_name);
#define PVT_TASKPROC_TRYLOCK_AND_EXECUTE(p, t) pvt_taskproc_trylock_and_execute(p, t, #t)

typedef struct pvt_taskproc_data {
    struct pvt* pvt;
} pvt_taskproc_data_t;

int pvt_taskproc_lock_and_execute(struct pvt_taskproc_data* ptd, void (*task_exe)(struct pvt_taskproc_data* ptd), const char* task_name);
#define PVT_TASKPROC_LOCK_AND_EXECUTE(p, t) pvt_taskproc_lock_and_execute(p, t, #t)

struct ast_module* self_module();

#define PVT_NO_CHANS(pvt) (!PVT_STATE(pvt, chansno))

#endif /* CHAN_QUECTEL_H_INCLUDED */
