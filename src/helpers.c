/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
*/
#include <signal.h> /* SIGURG */

#include "ast_config.h"

#include "helpers.h"

#include "at_command.h"
#include "chan_quectel.h" /* devices */
#include "error.h"
#include "smsdb.h"

// #include "pdu.h"				/* pdu_digit2code() */

static int is_valid_ussd_string(const char* number)
{
    for (; *number; number++) {
        if ((*number >= '0' && *number <= '9') || *number == '*' || *number == '#') {
            continue;
        }
        return 0;
    }
    return 1;
}

#/* */

int is_valid_phone_number(const char* number)
{
    if (number[0] == '+') {
        number++;
    }
    for (; *number; number++) {
        if (*number >= '0' && *number <= '9') {
            continue;
        }
        return 0;
    }
    return 1;
}

static struct pvt* get_pvt(const char* dev_name, int online)
{
    struct pvt* const pvt = pvt_find_by_ext(dev_name);

    if (!pvt) {
        chan_quectel_err = E_DEVICE_NOT_FOUND;
        return NULL;
    }

    if (!pvt->connected || (online && !pvt->initialized)) {
        ast_mutex_unlock(&pvt->lock);
        chan_quectel_err = E_DEVICE_DISABLED;
        return NULL;
    }

    return pvt;
}

static struct pvt* get_msg_pvt(const char* resource)
{
    int exists;
    struct pvt* const pvt = pvt_msg_find_by_resource(resource, 0, NULL, &exists);

    if (!pvt) {
        chan_quectel_err = E_DEVICE_NOT_FOUND;
        return NULL;
    }

    return pvt;
}

#/* */

int send_ussd(const char* dev_name, const char* ussd)
{
    if (!is_valid_ussd_string(ussd)) {
        chan_quectel_err = E_INVALID_USSD;
        return -1;
    }

    RAII_VAR(struct pvt*, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_ussd(&pvt->sys_chan, ussd, 0);
    return res;
}

#/* */

int send_sms(const char* const resource, const char* const sca, const char* const destination, const char* const message, int validity, int report)
{
    if (!is_valid_phone_number(destination)) {
        chan_quectel_err = E_INVALID_PHONE_NUMBER;
        return -1;
    }

    RAII_VAR(struct pvt* const, pvt, get_msg_pvt(resource), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_sms(&pvt->sys_chan, sca, destination, message, validity, report);
    return res;
}

int list_sms(const char* const dev_name, enum msg_status_t stat)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_list_messages(&pvt->sys_chan, stat);
    return res;
}

int delete_sms(const char* const dev_name, unsigned int idx, int delflag)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_cmgd(&pvt->sys_chan, idx, delflag);
    return res;
}

int sms_direct(const char* const dev_name, int directflag)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    if (directflag) {
        const int res = at_enqueue_msg_direct(&pvt->sys_chan, directflag > 0);
        return res;
    }

    if (!CONF_SHARED(pvt, msg_direct)) {
        chan_quectel_err = E_UNKNOWN;
        return -1;
    }

    const int res = at_enqueue_msg_direct(&pvt->sys_chan, CONF_SHARED(pvt, msg_direct) > 0);
    return res;
}

int smsdb_backup()
{
    static const size_t FN_DEF_LEN = 64;

    if (ast_strlen_zero(CONF_GLOBAL(sms_backup_db))) {
        return 0;
    }

    RAII_VAR(struct ast_str*, backup_file, ast_str_create(FN_DEF_LEN), ast_free);
    ast_str_set(&backup_file, 0, "%s.sqlite3", CONF_GLOBAL(sms_backup_db));

    if (smsdb_vacuum_into(ast_str_buffer(backup_file))) {
        return 0;
    }

    return 1;
}

#/* */

int send_reset(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 0), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_reset(&pvt->sys_chan);
    return res;
}

#/* */

int send_ccwa_set(const char* dev_name, call_waiting_t enable)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_set_ccwa(&pvt->sys_chan, enable);
    return res;
}

#/* */

int query_qaudloop(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_query_qaudloop(&pvt->sys_chan);
    return res;
}

#/* */

int send_qaudloop(const char* dev_name, int aloop)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_qaudloop(&pvt->sys_chan, aloop);
    return res;
}

#/* */

int query_qaudmod(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_query_qaudmod(&pvt->sys_chan);
    return res;
}

#/* */

int send_qaudmod(const char* dev_name, int amod)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_qaudmod(&pvt->sys_chan, amod);
    return res;
}

static const int MAX_GAIN     = 65535;
static const float MAX_GAIN_F = 65535.0f;

static const int MAX_GAIN_SIMCOM     = 8;
static const float MAX_GAIN_SIMCOM_F = 8.0f;

static int to_simcom_gain(int gain) { return gain * MAX_GAIN_SIMCOM / MAX_GAIN; }

#/* */

int query_micgain(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    int res;
    if (pvt->is_simcom) {
        res = at_enqueue_query_cmicgain(&pvt->sys_chan);
    } else {
        res = at_enqueue_query_qmic(&pvt->sys_chan);
    }
    return res;
}

#/* */

int send_micgain(const char* dev_name, int gain)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    int res;
    if (pvt->is_simcom) {
        res = at_enqueue_cmicgain(&pvt->sys_chan, to_simcom_gain(gain));
    } else {
        res = at_enqueue_qmic(&pvt->sys_chan, gain);
    }
    return res;
}

#/* */

int query_rxgain(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    int res;
    if (pvt->is_simcom) {
        res = at_enqueue_query_coutgain(&pvt->sys_chan);
    } else {
        res = at_enqueue_query_qrxgain(&pvt->sys_chan);
    }
    return res;
}

#/* */

int send_rxgain(const char* dev_name, int gain)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    int res;
    if (pvt->is_simcom) {
        res = at_enqueue_coutgain(&pvt->sys_chan, to_simcom_gain(gain));
    } else {
        res = at_enqueue_qrxgain(&pvt->sys_chan, gain);
    }
    return res;
}

int send_uac_apply(const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 1), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    if (pvt->is_simcom) {
        return -1;
    }

    const int res = at_enqueue_uac_apply(&pvt->sys_chan);
    return res;
}

#/* */

int send_at_command(const char* dev_name, const char* command)
{
    RAII_VAR(struct pvt* const, pvt, get_pvt(dev_name, 0), pvt_unlock);

    if (!pvt) {
        return -1;
    }

    const int res = at_enqueue_user_cmd(&pvt->sys_chan, command);
    return res;
}

int schedule_restart_event(dev_state_t event, restate_time_t when, const char* dev_name)
{
    RAII_VAR(struct pvt* const, pvt, pvt_find(dev_name), pvt_unlock);

    if (!pvt) {
        chan_quectel_err = E_DEVICE_NOT_FOUND;
        return -1;
    }

    pvt->desired_state = event;
    pvt->restart_time  = when;

    pvt_try_restate(pvt);
    return 0;
}

int str2gain(const char* s, int* gain)
{
    if (!s) {
        return -1;
    }

    const size_t len = strlen(s);
    if (!len) {
        return -1;
    }

    if (!strcasecmp(s, "off") || !strcasecmp(s, "mute")) {
        *gain = 0;
        return 0;
    } else if (!strcasecmp(s, "half")) {
        *gain = 32767;
        return 0;
    } else if (!strcasecmp(s, "full")) {
        *gain = 65535;
        return 0;
    }

    if (s[len - 1] == '%') {
        char* const ss        = ast_strndup(s, len - 1);
        const unsigned long p = strtoul(ss, NULL, 10);
        if (errno == ERANGE || p > 100u) {
            ast_free(ss);
            return -1;
        }
        ast_free(ss);
        *gain = (int)(MAX_GAIN_F * p / 100.0f);
        return 0;
    }


    const int g = (int)strtol(s, NULL, 10);
    if (errno == ERANGE || g < 0 || g > MAX_GAIN) {
        return -1;
    }
    *gain = g;
    return 0;
}

struct ast_str* const gain2str(int gain)
{
    struct ast_str* res = ast_str_create(5);
    ast_str_set(&res, 0, "%.0f%%", gain * 100.0f / MAX_GAIN_F);
    return res;
}

int str2gain_simcom(const char* s, int* gain)
{
    if (!s) {
        return -1;
    }

    const size_t len = strlen(s);
    if (!len) {
        return -1;
    }

    if (!strcasecmp(s, "off") || !strcasecmp(s, "mute")) {
        *gain = 0;
        return 0;
    } else if (!strcasecmp(s, "half")) {
        *gain = 4;
        return 0;
    } else if (!strcasecmp(s, "full")) {
        *gain = 8;
        return 0;
    }

    if (s[len - 1] == '%') {
        char* const ss        = ast_strndup(s, len - 1);
        const unsigned long p = strtoul(ss, NULL, 10);
        if (errno == ERANGE || p > 100u) {
            ast_free(ss);
            return -1;
        }
        ast_free(ss);
        *gain = (int)(MAX_GAIN_SIMCOM_F * p / 100.0f);
        return 0;
    }

    const int g = (int)strtol(s, NULL, 10);
    if (errno == ERANGE || g < 0 || g > MAX_GAIN_SIMCOM) {
        return -1;
    }
    *gain = g;
    return 0;
}

struct ast_str* const gain2str_simcom(int gain)
{
    struct ast_str* res = ast_str_create(5);
    ast_str_set(&res, 0, "%.0f%%", gain * 100.0f / MAX_GAIN_SIMCOM_F);
    return res;
}

static char escape_sequences[] = {0x1A, 0x1B, '\a', '\b', '\f', '\n', '\r', '\t', '\v', '\0'};

static char escape_sequences_map[] = {'z', 'e', 'a', 'b', 'f', 'n', 'r', 't', 'v', '\0'};

static char* escape_c(char* dest, const char* s, size_t size)
{
    /*
     * Note - This is an optimized version of ast_escape. When looking only
     * for escape_sequences a couple of checks used in the generic case can
     * be left out thus making it slightly more efficient.
     */
    char* p;
    char* c;

    if (!dest || !size) {
        return dest;
    }
    if (ast_strlen_zero(s)) {
        *dest = '\0';
        return dest;
    }

    for (p = dest; *s && --size; ++s, ++p) {
        /*
         * See if the character to escape is part of the standard escape
         * sequences. If so use its mapped counterpart.
         */
        c = strchr(escape_sequences, *s);
        if (c) {
            if (!--size) {
                /* Not enough room left for the escape sequence. */
                break;
            }

            *p++ = '\\';
            *p   = escape_sequences_map[c - escape_sequences];
        } else {
            *p = *s;
        }
    }
    *p = '\0';

    return dest;
}

size_t get_esc_str_buffer_size(size_t len) { return (len * 2u) + 1u; }

struct ast_str* escape_nstr(const char* buf, size_t cnt)
{
    if (!buf || !cnt) {  // build empty string
        struct ast_str* nbuf = ast_str_create(1);
        ast_str_truncate(nbuf, 0);
        return nbuf;
    }

    // build null-terminated string
    struct ast_str* nbuf = ast_str_create(cnt + 1u);
    memcpy(ast_str_buffer(nbuf), buf, cnt);
    ast_str_truncate(nbuf, cnt);

    // unescape string
    struct ast_str* const ebuf = ast_str_create(get_esc_str_buffer_size(cnt));
    escape_c(ast_str_buffer(ebuf), ast_str_buffer(nbuf), ast_str_size(ebuf));
    ast_str_update(ebuf);

    ast_free(nbuf);
    return ebuf;
}

const char* escape_nstr_ex(struct ast_str* ebuf, const char* buf, size_t cnt)
{
    if (!cnt) {
        ast_str_truncate(ebuf, 0);
        return ast_str_buffer(ebuf);
    }

    // build null-terminated string
    struct ast_str* nbuf = ast_str_create(cnt + 1u);
    memcpy(ast_str_buffer(nbuf), buf, cnt);
    ast_str_truncate(nbuf, cnt);

    // unescape string
    const char* const res = escape_str_ex(ebuf, nbuf);
    ast_free(nbuf);
    return res;
}

struct ast_str* escape_str(const struct ast_str* const str)
{
    if (!str || !ast_str_strlen(str)) {
        struct ast_str* nbuf = ast_str_create(1);
        ast_str_truncate(nbuf, 0);
        return nbuf;
    }

    const size_t len           = ast_str_strlen(str);
    struct ast_str* const ebuf = ast_str_create(get_esc_str_buffer_size(len));
    escape_c(ast_str_buffer(ebuf), ast_str_buffer(str), ast_str_size(ebuf));
    ast_str_update(ebuf);

    return ebuf;
}

const char* escape_str_ex(struct ast_str* ebuf, const struct ast_str* const str)
{
    const size_t len = ast_str_strlen(str);
    if (ast_str_make_space(&ebuf, get_esc_str_buffer_size(len))) {
        return ast_str_buffer(str);
    }
    escape_c(ast_str_buffer(ebuf), ast_str_buffer(str), ast_str_size(ebuf));
    ast_str_update(ebuf);
    return ast_str_buffer(ebuf);
}

#/* */

int gsm_is_registered(int stat)
{
    switch (stat) {
        case 0:  // not registered, MT is not currently searching an operator to register to
        case 2:  // not registered, but MT is currently trying to attach or searching an operator to register to
        case 3:  // registration denied
            return 0;

        case 1:  // registered, home network
        case 4:  // unknown (e.g. out of E-UTRAN coverage)
        case 5:  // registered, roaming
        case 6:  // registered for "SMS only", home network (not applicable)
        case 7:  // registered for "SMS only", roaming (not applicable)
        case 8:  // attached for emergency bearer services only
            return 1;

        default:
            return 0;
    }
}

const char* gsm_regstate2str(int gsm_reg_status)
{
    static const char* const gsm_states[] = {"Not registered, not searching",
                                             "Registered, home network",
                                             "Not registered, but searching",
                                             "Registration denied",
                                             "Unknown",
                                             "Registered, roaming",
                                             "Registered for<SMS only, home network",
                                             "Registered for “SMS only”, roaming",
                                             "Attached for emergency bearer services only"};
    return enum2str_def(gsm_reg_status, gsm_states, ARRAY_LEN(gsm_states), "Unknown");
}

const char* gsm_regstate2str_json(int gsm_reg_status)
{
    static const char* const gsm_states[] = {
        "not_registered_not_searching", "registered", "not_registered_searching", "registration_denied", "unknown", "registered_roaming", "registered_sms_only",
        "registered_sms_only_roaming",  "emergency"};
    return enum2str_def(gsm_reg_status, gsm_states, ARRAY_LEN(gsm_states), "unknown");
}

#/* */

int map_creg_act(int act)
{
    switch (act) {
        case 0:  // GSM
        case 1:  // GSM Compact
        case 2:  // UTRAN
        case 3:  // GSM w/EGPRS
        case 4:  // UTRAN w/HSDPA
        case 5:  // UTRAN w/HSUPA
        case 6:  // UTRAN w/HSDPA and HSUPA
        case 7:  // E-UTRAN
            return act + 1;

        default:
            return -1;
    }
}

const char* sys_act2str(int act)
{
    static const char* const sys_acts[] = {
        "No service",
        "GSM",
        "GPRS",
        "EDGE",
        "WCDMA",
        "HSDPA",
        "HSUPA",
        "HSDPA and HSUPA",
        "LTE",
        "TDS-CDMA",
        "TDS-HSDPA only",
        "TDS-HSUPA only",
        "TDS-HSPA (HSDPA and HSUPA)",
        "CDMA",
        "EVDO",
        "CDMA and EVDO",
        "CDMA and LTE",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "Ehrpd",
        "CDMA and Ehrpd",
    };

    return enum2str_def(act, sys_acts, ARRAY_LEN(sys_acts), "Unknown");
}

#/* BUGFIX of https://code.google.com/p/asterisk-chan-quectel/issues/detail?id=118 */

struct ast_str* rssi2dBm(int rssi)
{
    static const size_t DEF_STR_LEN = 32;

    static const char BELOW_113DBM[]         = "<= -113 dBm";
    static const char UNDER_51DBM[]          = ">= -51 dBm";
    static const char UNKNOWN_UNMEASURABLE[] = "unknown or unmeasurable";

    struct ast_str* res = ast_str_create(DEF_STR_LEN);
    if (rssi <= 0) {
        ast_str_set_substr(&res, 0, BELOW_113DBM, STRLEN(BELOW_113DBM));
    } else if (rssi <= 30) {
        ast_str_set(&res, 0, "%d dBm", 2 * rssi - 113);
    } else if (rssi == 31) {
        ast_str_set_substr(&res, 0, UNDER_51DBM, STRLEN(UNDER_51DBM));
    } else {
        ast_str_set_substr(&res, 0, UNKNOWN_UNMEASURABLE, STRLEN(UNKNOWN_UNMEASURABLE));
    }
    return res;
}

size_t fd_write_all(int fd, const char* buf, size_t count)
{
    size_t total  = 0;
    unsigned errs = 10;

    while (count > 0) {
        const ssize_t out_count = write(fd, buf, count);
        if (out_count <= 0) {
            if (errno == EINTR || errno == EAGAIN) {
                errs--;
                if (errs) {
                    continue;
                }
            }
            break;
        }

        errs   = 10;
        count -= out_count;
        buf   += out_count;
        total += out_count;
    }

    return total;
}

static const char* const dev_state_strs[4]             = {"stop", "restart", "remove", "start"};
static const char* const dev_state_strs_capitalized[4] = {"Stop", "Restart", "Remove", "Start"};

public_state_t* gpublic;

const char* dev_state2str(dev_state_t state) { return enum2str(state, dev_state_strs, ARRAY_LEN(dev_state_strs)); }

const char* dev_state2str_capitalized(dev_state_t state) { return enum2str(state, dev_state_strs_capitalized, ARRAY_LEN(dev_state_strs_capitalized)); }

dev_state_t str2dev_state(const char* str)
{
    if (!str) {
        return DEV_STATE_STOPPED;
    }

    const int res = str2enum(str, dev_state_strs, ARRAY_LEN(dev_state_strs));
    if (res < 0) {
        return DEV_STATE_STOPPED;
    } else {
        return (dev_state_t)res;
    }
}

const char* dev_state2str_msg(dev_state_t state)
{
    static const char* const states[] = {"Stop scheduled", "Restart scheduled", "Removal scheduled", "Start scheduled"};
    return enum2str(state, states, ARRAY_LEN(states));
}

const char* restate2str_msg(restate_time_t when)
{
    static const char* const choices[] = {"Now", "Gracefully", "When convenient"};
    return enum2str(when, choices, ARRAY_LEN(choices));
}

int format_ast_tm(const struct ast_tm* const tm, struct ast_str* str)
{
    static char AST_TM_FMT[]  = "%FT%T%z";
    static char AST_TM_FMTZ[] = "%FT%TZ";
    const int res             = ast_strftime(ast_str_buffer(str), ast_str_size(str), tm->tm_gmtoff ? AST_TM_FMT : AST_TM_FMTZ, tm);
    if (res >= 0) {
        ast_str_truncate(str, res);
        return 0;
    } else {
        ast_str_reset(str);
        return -1;
    }
}

struct ast_tm* ast_tm_normalize(struct ast_tm* const tm)
{
    const long gmtoff    = tm->tm_gmtoff;
    struct timeval tmval = ast_mktime(tm, "UTC");

    tmval.tv_sec -= gmtoff;
    ast_localtime(&tmval, tm, "UTC");
    tm->tm_gmtoff = gmtoff;
    return tm;
}
