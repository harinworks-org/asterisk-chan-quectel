/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
*/
#include <alsa/global.h>
#include <alsa/version.h>
#include <sqlite3.h>

#include "ast_config.h"

#include <asterisk.h>

#include <asterisk/callerid.h> /* ast_describe_caller_presentation() */
#include <asterisk/cli.h>      /* struct ast_cli_entry; struct ast_cli_args */

#include "cli.h"

#include "chan_quectel.h" /* devices */
#include "error.h"
#include "helpers.h" /* ARRAY_LEN() send_ccwa_set() send_reset() send_sms() send_ussd() */

#define CLI_ALIASES(fn, cmdd, usage1, usage2)                                           \
    static char* fn##_quectel(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a) \
    {                                                                                   \
        switch (cmd) {                                                                  \
            case CLI_INIT:                                                              \
                e->command = "quectel " cmdd "\n";                                      \
                e->usage   = "Usage: quectel " usage1 "\n       " usage2 ".\n";         \
                return NULL;                                                            \
        }                                                                               \
        return fn(e, cmd, a);                                                           \
    }                                                                                   \
    static char* fn##_simcom(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)  \
    {                                                                                   \
        switch (cmd) {                                                                  \
            case CLI_INIT:                                                              \
                e->command = "simcom " cmdd "\n";                                       \
                e->usage   = "Usage: simcom " usage1 "\n       " usage2 ".\n";          \
                return NULL;                                                            \
        }                                                                               \
        return fn(e, cmd, a);                                                           \
    }

#define CLI_DEF_ENTRIES(fn, desc) AST_CLI_DEFINE(fn##_quectel, desc), AST_CLI_DEFINE(fn##_simcom, desc),

static char* complete_device(const char* word, int state)
{
    char* res            = NULL;
    int which            = 0;
    const size_t wordlen = strlen(word);
    struct pvt* pvt;

    struct ao2_iterator i = ao2_iterator_init(gpublic->pvts, 0);
    while ((pvt = ao2_iterator_next(&i))) {
        if (ao2_lock(pvt)) {
            ao2_ref(pvt, -1);
            continue;
        }
        if (!strncasecmp(PVT_ID(pvt), word, wordlen) && ++which > state) {
            res = ast_strdup(PVT_ID(pvt));
            AO2_UNLOCK_AND_UNREF(pvt);
            break;
        }

        AO2_UNLOCK_AND_UNREF(pvt);
    }
    ao2_iterator_destroy(&i);
    return res;
}

static char* cli_show_devices(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    struct pvt* pvt;

    static const char FORMAT1[] = "%-12.12s %-5.5s %-10.10s %-4.4s %-4.4s %-7.7s %-14.14s %-17.17s %-14.14s\n";
    static const char FORMAT2[] = "%-12.12s %-5d %-10.10s %-4d %-4d %-7s %-14.14s %-17.17s %-16.16s %-16.16s %-14.14s\n";

    switch (cmd) {
        case CLI_GENERATE:
            return NULL;
    }

    if (a->argc != 3) {
        return CLI_SHOWUSAGE;
    }

    ast_cli(a->fd, FORMAT1, "ID", "Group", "State", "RSSI", "Mode", "Provider Name", "Model", "Firmware", "Number");

    struct ao2_iterator i = ao2_iterator_init(gpublic->pvts, 0);
    while ((pvt = ao2_iterator_next(&i))) {
        SCOPED_AO2LOCK(pvtl, pvt);
        ast_cli(a->fd, FORMAT2, PVT_ID(pvt), CONF_SHARED(pvt, group), pvt_str_state(pvt), pvt->rssi, pvt->act, pvt->provider_name, pvt->model, pvt->firmware,
                pvt->imei, pvt->imsi, pvt->subscriber_number);
    }
    ao2_iterator_destroy(&i);
    return CLI_SUCCESS;
}

CLI_ALIASES(cli_show_devices, "show devices", "show devices", "Shows the state of devices")

static char* cli_show_device_settings(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 5) {
        return CLI_SHOWUSAGE;
    }

    RAII_VAR(struct pvt* const, pvt, pvt_find(a->argv[4]), pvt_unlock);

    if (pvt) {
        const struct ast_format* const fmt = pvt_get_audio_format(pvt);
        const char* const codec_name       = ast_format_get_name(fmt);

        ast_cli(a->fd, "------------- Settings ------------\n");
        ast_cli(a->fd, "  Device                  : %s\n", PVT_ID(pvt));
        if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
            ast_cli(a->fd, "  Audio UAC               : %s\n", CONF_UNIQ(pvt, alsadev));
        } else {
            ast_cli(a->fd, "  Audio                   : %s\n", CONF_UNIQ(pvt, audio_tty));
        }
        ast_cli(a->fd, "  Audio format            : %s\n", codec_name);
        ast_cli(a->fd, "  Data                    : %s\n", CONF_UNIQ(pvt, data_tty));
        ast_cli(a->fd, "  Channel Language        : %s\n", CONF_SHARED(pvt, language));
        ast_cli(a->fd, "  Context                 : %s\n", CONF_SHARED(pvt, context));
        ast_cli(a->fd, "  Exten                   : %s\n", CONF_SHARED(pvt, exten));
        ast_cli(a->fd, "  Group                   : %d\n", CONF_SHARED(pvt, group));
        ast_cli(a->fd, "  Used Notifications      : %s\n", S_COR(CONF_SHARED(pvt, dsci), "DSCI", "CCINFO"));
        ast_cli(a->fd, "  16kHz audio             : %s\n", AST_CLI_YESNO(CONF_UNIQ(pvt, slin16)));
        ast_cli(a->fd, "  RX gain                 : %d\n", CONF_SHARED(pvt, rxgain));
        ast_cli(a->fd, "  TX gain                 : %d\n", CONF_SHARED(pvt, txgain));
        ast_cli(a->fd, "  Use CallingPres         : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, use_calling_pres)));
        ast_cli(a->fd, "  Default CallingPres     : %s\n",
                S_COR(CONF_SHARED(pvt, calling_pres) < 0, "<Not set>", ast_describe_caller_presentation(CONF_SHARED(pvt, calling_pres))));
        ast_cli(a->fd, "  Message Service         : %d\n", CONF_SHARED(pvt, msg_service));
        ast_cli(a->fd, "  Message Storage         : %s\n", dc_msgstor2str(CONF_SHARED(pvt, msg_storage)));
        ast_cli(a->fd, "  Direct Message          : %s\n", dc_3stbool2str_capitalized(CONF_SHARED(pvt, msg_direct)));
        ast_cli(a->fd, "  Auto Delete SMS         : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, sms_autodelete)));
        ast_cli(a->fd, "  Reset Modem             : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, reset_modem)));
        ast_cli(a->fd, "  Call Waiting            : %s\n", dc_cw_setting2str(CONF_SHARED(pvt, call_waiting)));
        ast_cli(a->fd, "  Multiparty Calls        : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, multiparty)));
        ast_cli(a->fd, "  DTMF Detection          : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, dtmf)));
        ast_cli(a->fd, "  DTMF Duration           : %ld\n", CONF_SHARED(pvt, dtmf_duration));
        ast_cli(a->fd, "  Hold/Unhold Action      : %s\n", S_COR(CONF_SHARED(pvt, dtmf), "MOH", "Mute"));
        ast_cli(a->fd, "  Query Time              : %s\n", AST_CLI_YESNO(CONF_SHARED(pvt, query_time)));
        ast_cli(a->fd, "  Initial Device State    : %s\n", dev_state2str_capitalized(CONF_SHARED(pvt, init_state)));
        ast_cli(a->fd, "  Use QHUP Command        : %s\n\n", AST_CLI_YESNO(CONF_SHARED(pvt, qhup)));
    } else {
        ast_cli(a->fd, "Device %s not found\n", a->argv[4]);
    }

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_show_device_settings, "show device settings", "show device settings <device>", "Shows the settings device")

static char* cli_show_device_state(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 5) {
        return CLI_SHOWUSAGE;
    }

    RAII_VAR(struct pvt* const, pvt, pvt_find(a->argv[4]), pvt_unlock);

    if (pvt) {
        RAII_VAR(struct ast_str*, state_str, pvt_str_state_ex(pvt), ast_free);
        RAII_VAR(struct ast_str*, rssi_str, rssi2dBm(pvt->rssi), ast_free);

        ast_cli(a->fd, "-------------- Status -------------\n");
        ast_cli(a->fd, "  Device                  : %s\n", PVT_ID(pvt));
        ast_cli(a->fd, "  State                   : %s\n", ast_str_buffer(state_str));
        if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
            ast_cli(a->fd, "  Audio UAC               : %s\n", CONF_UNIQ(pvt, alsadev));
        } else {
            ast_cli(a->fd, "  Audio                   : %s\n", CONF_UNIQ(pvt, audio_tty));
        }
        ast_cli(a->fd, "  Data                    : %s\n", CONF_UNIQ(pvt, data_tty));
        ast_cli(a->fd, "  Voice                   : %s\n", AST_CLI_YESNO(pvt->has_voice));
        ast_cli(a->fd, "  SMS                     : %s\n", AST_CLI_YESNO(pvt->has_sms));
        ast_cli(a->fd, "  Manufacturer            : %s\n", pvt->manufacturer);
        ast_cli(a->fd, "  Model                   : %s\n", pvt->model);
        ast_cli(a->fd, "  Firmware                : %s\n", pvt->firmware);
        ast_cli(a->fd, "  IMEI                    : %s\n", pvt->imei);
        ast_cli(a->fd, "  IMSI                    : %s\n", pvt->imsi);
        ast_cli(a->fd, "  ICCID                   : %s\n", pvt->iccid);
        ast_cli(a->fd, "  GSM Registration Status : %s\n", gsm_regstate2str(pvt->gsm_reg_status));
        ast_cli(a->fd, "  RSSI                    : %d, %s\n", pvt->rssi, ast_str_buffer(rssi_str));
        ast_cli(a->fd, "  Access technology       : %s\n", sys_act2str(pvt->act));
        ast_cli(a->fd, "  Network Name            : %s\n", pvt->network_name);
        ast_cli(a->fd, "  Short Network Name      : %s\n", pvt->short_network_name);
        ast_cli(a->fd, "  Registered PLMN         : %d\n", pvt->operator);
        ast_cli(a->fd, "  Provider Name           : %s\n", pvt->provider_name);
        ast_cli(a->fd, "  Band                    : %s\n", pvt->band);
        ast_cli(a->fd, "  Location area code      : %s\n", pvt->location_area_code);
        ast_cli(a->fd, "  Cell ID                 : %s\n", pvt->cell_id);
        ast_cli(a->fd, "  Subscriber Number       : %s\n", S_OR(pvt->subscriber_number, "Unknown"));
        ast_cli(a->fd, "  SMS Service Center      : %s\n", pvt->sms_scenter);
        if (CONF_SHARED(pvt, query_time) && pvt->module_time.tm_year) {
            struct ast_str* mt = ast_str_alloca(64);
            format_ast_tm(&pvt->module_time, mt);
            ast_cli(a->fd, "  Module time             : %s\n", ast_str_buffer(mt));
        }
        ast_cli(a->fd, "  Tasks in queue          : %u\n", PVT_STATE(pvt, at_tasks));
        ast_cli(a->fd, "  Commands in queue       : %u\n", PVT_STATE(pvt, at_cmds));
        ast_cli(a->fd, "  Call Waiting            : %s\n", AST_CLI_ONOFF(pvt->has_call_waiting));
        ast_cli(a->fd, "  Current device state    : %s\n", dev_state2str_capitalized(pvt->current_state));
        ast_cli(a->fd, "  Desired device state    : %s\n", dev_state2str_capitalized(pvt->desired_state));
        ast_cli(a->fd, "  When change state       : %s\n", restate2str_msg(pvt->restart_time));

        ast_cli(a->fd, "  Calls/Channels          : %u\n", PVT_STATE(pvt, chansno));
        ast_cli(a->fd, "    Active                : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_ACTIVE]));
        ast_cli(a->fd, "    Held                  : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_ONHOLD]));
        ast_cli(a->fd, "    Dialing               : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_DIALING]));
        ast_cli(a->fd, "    Alerting              : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_ALERTING]));
        ast_cli(a->fd, "    Incoming              : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_INCOMING]));
        ast_cli(a->fd, "    Waiting               : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_WAITING]));
        ast_cli(a->fd, "    Releasing             : %u\n", PVT_STATE(pvt, chan_count[CALL_STATE_RELEASED]));
        ast_cli(a->fd, "    Initializing          : %u\n\n", PVT_STATE(pvt, chan_count[CALL_STATE_INIT]));
        /* TODO: show call waiting  network setting and local config value */
    } else {
        ast_cli(a->fd, "Device %s not found\n", a->argv[4]);
    }

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_show_device_state, "show device state", "show device state <device>", "Shows the state of device")

#/* */

static int32_t getACD(uint32_t calls, uint32_t duration)
{
    int32_t acd;

    if (calls) {
        acd = duration / calls;
    } else {
        acd = -1;
    }

    return acd;
}

#/* */

static int32_t getASR(uint32_t total, uint32_t handled)
{
    int32_t asr;

    if (total) {
        asr = handled * 100 / total;
    } else {
        asr = -1;
    }

    return asr;
}

static char* cli_show_device_statistics(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 5) {
        return CLI_SHOWUSAGE;
    }

    RAII_VAR(struct pvt* const, pvt, pvt_find(a->argv[4]), pvt_unlock);

    if (pvt) {
        ast_cli(a->fd, "-------------- Statistics -------------\n");
        ast_cli(a->fd, "  Device                      : %s\n", PVT_ID(pvt));
        ast_cli(a->fd, "  Queue tasks                 : %u\n", PVT_STAT(pvt, at_tasks));
        ast_cli(a->fd, "  Queue commands              : %u\n", PVT_STAT(pvt, at_cmds));
        ast_cli(a->fd, "  Responses                   : %u\n", PVT_STAT(pvt, at_responses));
        ast_cli(a->fd, "  Bytes of read responses     : %u\n", PVT_STAT(pvt, d_read_bytes));
        ast_cli(a->fd, "  Bytes of written commands   : %u\n", PVT_STAT(pvt, d_write_bytes));
        ast_cli(a->fd, "  Bytes of read audio         : %llu\n", (unsigned long long int)PVT_STAT(pvt, a_read_bytes));
        ast_cli(a->fd, "  Bytes of written audio      : %llu\n", (unsigned long long int)PVT_STAT(pvt, a_write_bytes));
        ast_cli(a->fd, "  Readed frames               : %u\n", PVT_STAT(pvt, read_frames));
        ast_cli(a->fd, "  Readed short frames         : %u\n", PVT_STAT(pvt, read_sframes));
        ast_cli(a->fd, "  Wrote frames                : %u\n", PVT_STAT(pvt, write_frames));
        ast_cli(a->fd, "  Wrote short frames          : %u\n", PVT_STAT(pvt, write_tframes));
        ast_cli(a->fd, "  Wrote silence frames        : %u\n", PVT_STAT(pvt, write_sframes));
        ast_cli(a->fd, "  Write buffer overflow bytes : %llu\n", (unsigned long long int)PVT_STAT(pvt, write_rb_overflow_bytes));
        ast_cli(a->fd, "  Write buffer overflow count : %u\n", PVT_STAT(pvt, write_rb_overflow));
        ast_cli(a->fd, "  Incoming calls              : %u\n", PVT_STAT(pvt, in_calls));
        ast_cli(a->fd, "  Waiting calls               : %u\n", PVT_STAT(pvt, cw_calls));
        ast_cli(a->fd, "  Handled input calls         : %u\n", PVT_STAT(pvt, in_calls_handled));
        ast_cli(a->fd, "  Fails to PBX run            : %u\n", PVT_STAT(pvt, in_pbx_fails));
        ast_cli(a->fd, "  Attempts to outgoing calls  : %u\n", PVT_STAT(pvt, out_calls));
        ast_cli(a->fd, "  Answered outgoing calls     : %u\n", PVT_STAT(pvt, calls_answered[CALL_DIR_OUTGOING]));
        ast_cli(a->fd, "  Answered incoming calls     : %u\n", PVT_STAT(pvt, calls_answered[CALL_DIR_INCOMING]));
        ast_cli(a->fd, "  Seconds of outgoing calls   : %u\n", PVT_STAT(pvt, calls_duration[CALL_DIR_OUTGOING]));
        ast_cli(a->fd, "  Seconds of incoming calls   : %u\n", PVT_STAT(pvt, calls_duration[CALL_DIR_INCOMING]));
        ast_cli(a->fd, "  ACD for incoming calls      : %d\n",
                getACD(PVT_STAT(pvt, calls_answered[CALL_DIR_INCOMING]), PVT_STAT(pvt, calls_duration[CALL_DIR_INCOMING])));
        ast_cli(a->fd, "  ACD for outgoing calls      : %d\n",
                getACD(PVT_STAT(pvt, calls_answered[CALL_DIR_OUTGOING]), PVT_STAT(pvt, calls_duration[CALL_DIR_OUTGOING])));
        /*
                ast_cli (a->fd, "  ACD                         : %d\n",
                    getACD(
                        PVT_STAT(pvt, calls_answered[CALL_DIR_OUTGOING])
                        + PVT_STAT(pvt, calls_answered[CALL_DIR_INCOMING]),

                        PVT_STAT(pvt, calls_duration[CALL_DIR_OUTGOING])
                        + PVT_STAT(pvt, calls_duration[CALL_DIR_INCOMING])
                        )
                    );
        */
        ast_cli(a->fd, "  ASR for incoming calls      : %d\n",
                getASR(PVT_STAT(pvt, in_calls) + PVT_STAT(pvt, cw_calls), PVT_STAT(pvt, calls_answered[CALL_DIR_INCOMING])));
        ast_cli(a->fd, "  ASR for outgoing calls      : %d\n\n", getASR(PVT_STAT(pvt, out_calls), PVT_STAT(pvt, calls_answered[CALL_DIR_OUTGOING])));
        /*
                ast_cli (a->fd, "  ASR                         : %d\n\n",
                    getASR(
                        PVT_STAT(pvt, out_calls)
                        + PVT_STAT(pvt, in_calls)
                        + PVT_STAT(pvt, cw_calls),

                        PVT_STAT(pvt, calls_answered[CALL_DIR_OUTGOING])
                        + PVT_STAT(pvt, calls_answered[CALL_DIR_INCOMING])
                        )
                    );
        */
    } else {
        ast_cli(a->fd, "Device %s not found\n", a->argv[4]);
    }

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_show_device_statistics, "show device statistics", "show device statistics <device>", "Shows the statistics of device")

static char* cli_show_version(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            return NULL;
    }

    if (a->argc != 3) {
        return CLI_SHOWUSAGE;
    }

    ast_cli(a->fd,
            "\n" AST_MODULE ": " MODULE_DESCRIPTION ", Version " MODULE_VERSION ", Revision " PACKAGE_REVISION "\nProject Home: " MODULE_URL
            "\nBug Reporting: " MODULE_BUGREPORT "\nALSA version: %s [" SND_LIB_VERSION_STR "], SQLite version: %s [" SQLITE_VERSION "]\n\n",
            snd_asoundlib_version(), sqlite3_libversion());

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_show_version, "show version", "show version", "Shows the version of module")

static char* cli_cmd(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 2) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 4) {
        return CLI_SHOWUSAGE;
    }

    const int res = send_at_command(a->argv[2], a->argv[3]);
    ast_cli(a->fd, "[%s] '%s' %s\n", a->argv[2], a->argv[3], res < 0 ? error2str(chan_quectel_err) : "AT command queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_cmd, "cmd", "cmd <device> <command>", "Send <command> to the rfcomm port on the device with the specified <device>")

static char* cli_ussd(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 2) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 4) {
        return CLI_SHOWUSAGE;
    }

    int res = send_ussd(a->argv[2], a->argv[3]);
    ast_cli(a->fd, "[%s] %s\n", a->argv[2], res < 0 ? error2str(chan_quectel_err) : "USSD queued for send");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_ussd, "ussd", "ussd <device> <command>", "Send ussd <command> with the specified <device>")

static char* cli_sms_send(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    static const int DEF_VALIDITY    = 15;
    static const int DEF_REPORT      = 1;
    static const ssize_t MSG_DEF_LEN = 160;

    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 6) {
        return CLI_SHOWUSAGE;
    }

    RAII_VAR(struct ast_str*, buf, ast_str_create(MSG_DEF_LEN), ast_free);

    for (int i = 5; i < a->argc; ++i) {
        if (i < (a->argc - 1)) {
            ast_str_append(&buf, 0, "%s ", a->argv[i]);
        } else {
            ast_str_append(&buf, 0, "%s", a->argv[i]);
        }
    }

    const int res = send_sms(a->argv[3], "", a->argv[4], ast_str_buffer(buf), DEF_VALIDITY, DEF_REPORT);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "SMS queued for send");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_send, "sms send", "sms send <resource> <number> <message>", "Send a SMS to <number> with the <message> from device <resource>")

static char* cli_sms_list_received_unread(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 5) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 6) {
        return CLI_SHOWUSAGE;
    }

    const int res = list_sms(a->argv[5], MSG_STAT_REC_UNREAD);
    ast_cli(a->fd, "[%s] %s\n", a->argv[5], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_list_received_unread, "sms list received unread", "sms list received unread <device>", "List unread received mesages from <device>")

static char* cli_sms_list_received_read(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 5) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 6) {
        return CLI_SHOWUSAGE;
    }

    const int res = list_sms(a->argv[5], MSG_STAT_REC_READ);
    ast_cli(a->fd, "[%s] %s\n", a->argv[5], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_list_received_read, "sms list received read", "sms list received read <device>", "List read received mesages from <device>")

static char* cli_sms_list_all(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int res = list_sms(a->argv[4], MSG_STAT_ALL);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_list_all, "sms list all", "sms list all <device>", "List all mesages from <device>")

static char* cli_sms_delete_received_read(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 5) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 6) {
        return CLI_SHOWUSAGE;
    }

    const int res = delete_sms(a->argv[5], 0, 1);
    ast_cli(a->fd, "[%s] %s\n", a->argv[5], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_delete_received_read, "sms delete received read", "sms delete received read <device>", "Delete read mesages from <device>")

static char* cli_sms_delete_all(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int res = delete_sms(a->argv[4], 0, 4);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_delete_all, "sms delete all", "sms delete all <device>", "Delete all mesages from <device>")

static char* cli_sms_delete(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const unsigned long idx = strtoul(a->argv[4], NULL, 10);
    if (errno == ERANGE) {
        ast_cli(a->fd, "[%s] Invalid message index\n", a->argv[3]);
        return CLI_FAILURE;
    }

    const int res = delete_sms(a->argv[3], (unsigned int)idx, 0);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_delete, "sms delete", "sms delete <device> <idx>", "Delete specified mesage from <device>")

static char* cli_sms_direct_on(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int res = sms_direct(a->argv[4], 1);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_direct_on, "sms direct on", "sms direct on <device>", "Receive messages from <device> directly")

static char* cli_sms_direct_off(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int res = sms_direct(a->argv[4], -1);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_direct_off, "sms direct off", "sms direct off <device>", "Receive messages from <device> indirectly")

static char* cli_sms_direct_auto(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int res = sms_direct(a->argv[4], 0);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Operation queued");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_direct_auto, "sms direct auto", "sms direct auto <device>", "Receive messages from <device> in a way specified in configuration")

static char* cli_sms_db_backup(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    if (a->argc < 4) {
        return CLI_SHOWUSAGE;
    }

    const int res = smsdb_backup();
    ast_cli(a->fd, "%s\n", res ? "Backup of SMS database created" : "Cannot create backup of SMS database");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_sms_db_backup, "sms db backup", "sms db backup", "Backup SMS database")

typedef const char* const* ast_cli_complete2_t;

static char* cli_ccwa_set(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    static const char* const choices[] = {"enable", "disable", NULL};
    call_waiting_t enable;

    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 2) {
                return ast_cli_complete(a->word, (ast_cli_complete2_t)choices, a->n);
            }
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 4) {
        return CLI_SHOWUSAGE;
    }
    if (!strcasecmp("disable", a->argv[2])) {
        enable = CALL_WAITING_DISALLOWED;
    } else if (!strcasecmp("enable", a->argv[2])) {
        enable = CALL_WAITING_ALLOWED;
    } else {
        return CLI_SHOWUSAGE;
    }

    int res = send_ccwa_set(a->argv[3], enable);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Call-Waiting commands queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_ccwa_set, "callwaiting", "callwaiting disable|enable <device>", "Disable/Enable Call-Waiting on <device>")

static char* cli_reset(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 2) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 3) {
        return CLI_SHOWUSAGE;
    }

    const int res = send_reset(a->argv[2]);
    ast_cli(a->fd, "[%s] %s\n", a->argv[2], res < 0 ? error2str(chan_quectel_err) : "Reset command queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_reset, "reset", "reset <device>", "Reset <device>")

static const char* const a_choices[]  = {"now", "gracefully", "when", NULL};
static const char* const a_choices2[] = {"convenient", NULL};

#/* */

static char* cli_restart_event(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a, dev_state_t event)
{
    const char* device = NULL;
    int res;
    int i;

    switch (cmd) {
        case CLI_GENERATE:
            switch (a->pos) {
                case 2:
                    return ast_cli_complete(a->word, (ast_cli_complete2_t)a_choices, a->n);

                case 3:
                    if (!strcasecmp(a->argv[2], "when")) {
                        return ast_cli_complete(a->word, (ast_cli_complete2_t)a_choices2, a->n);
                    }
                    return complete_device(a->word, a->n);

                case 4:
                    if (!strcasecmp(a->argv[2], "when") && !strcasecmp(a->argv[3], "convenient")) {
                        return complete_device(a->word, a->n);
                    }
                    break;
            }
            return NULL;
    }

    if (a->argc != 4 && a->argc != 5) {
        return CLI_SHOWUSAGE;
    }

    for (i = 0; a_choices[i]; i++) {
        if (!strcasecmp(a->argv[2], a_choices[i])) {
            if (i == RESTATE_TIME_CONVENIENT) {
                if (a->argc == 5 && !strcasecmp(a->argv[3], a_choices2[0])) {
                    device = a->argv[4];
                }
            } else if (a->argc == 4) {
                device = a->argv[3];
            }

            if (device) {
                res = schedule_restart_event(event, i, device);
                ast_cli(a->fd, "[%s] %s\n", device, res < 0 ? error2str(chan_quectel_err) : dev_state2str_msg(event));
                return CLI_SUCCESS;
            }
            break;
        }
    }
    return CLI_SHOWUSAGE;
}

#/* */

static char* cli_stop(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a) { return cli_restart_event(e, cmd, a, DEV_STATE_STOPPED); }

CLI_ALIASES(cli_stop, "stop", "stop < now | gracefully | when convenient > <device>", "Stop <device>")

#/* */

static char* cli_restart(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a) { return cli_restart_event(e, cmd, a, DEV_STATE_RESTARTED); }

CLI_ALIASES(cli_restart, "restart", "restart < now | gracefully | when convenient > <device>", "Restart <device>")

#/* */

static char* cli_remove(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a) { return cli_restart_event(e, cmd, a, DEV_STATE_REMOVED); }

CLI_ALIASES(cli_remove, "remove", "remove < now | gracefully | when convenient > <device>", "Remove <device>")

#/* */

static char* cli_start(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 2) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 3) {
        return CLI_SHOWUSAGE;
    }

    int res = schedule_restart_event(DEV_STATE_STARTED, RESTATE_TIME_NOW, a->argv[2]);
    ast_cli(a->fd, "[%s] %s\n", a->argv[2], res < 0 ? error2str(chan_quectel_err) : dev_state2str_msg(DEV_STATE_STARTED));

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_start, "start", "start <device>", "Start <device>")

static char* cli_reload(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    int ok = 0;
    int i;

    switch (cmd) {
        case CLI_GENERATE:
            switch (a->pos) {
                case 2:
                    return ast_cli_complete(a->word, (ast_cli_complete2_t)a_choices, a->n);
                case 3:
                    if (strcasecmp(a->argv[2], "when") == 0) {
                        return ast_cli_complete(a->word, (ast_cli_complete2_t)a_choices2, a->n);
                    }
            }
            return NULL;
    }

    if (a->argc != 3 && a->argc != 4) {
        return CLI_SHOWUSAGE;
    }

    for (i = 0; a_choices[i]; i++) {
        if (!strcasecmp(a->argv[2], a_choices[i])) {
            if (i == RESTATE_TIME_CONVENIENT) {
                if (a->argc == 4 && !strcasecmp(a->argv[3], a_choices2[0])) {
                    ok = 1;
                }
            } else if (a->argc == 3) {
                ok = 1;
            }

            if (ok) {
                pvt_reload(i);
                return CLI_SUCCESS;
            }
            break;
        }
    }
    return CLI_SHOWUSAGE;
}

CLI_ALIASES(cli_reload, "reload", "reload < now | gracefully | when convenient <device>", "Reloads the configuration")

static char* cli_uac_apply(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel uac apply\n";
            e->usage   = "Usage: quectel uac apply <device>\n       Apply UAC mode and restart.\n";
            return NULL;

        case CLI_GENERATE:
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc != 4) {
        return CLI_SHOWUSAGE;
    }

    int res = send_uac_apply(a->argv[3]);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : dev_state2str_msg(DEV_STATE_STARTED));

    return CLI_SUCCESS;
}

static char* cli_audio_loop(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    static const char* const choices[] = {"on", "off", NULL};
    int aloop                          = 0;

    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return ast_cli_complete(a->word, (ast_cli_complete2_t)choices, a->n);
            }
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 4) {
        return CLI_SHOWUSAGE;
    }

    if (a->argc == 4) {  // query
        int res = query_qaudloop(a->argv[3]);
        ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");
        return CLI_SUCCESS;
    }

    // write
    if (!strcasecmp(choices[1], a->argv[4])) {
        aloop = 0;
    } else if (!strcasecmp(choices[0], a->argv[4])) {
        aloop = 1;
    } else {
        return CLI_SHOWUSAGE;
    }

    int res = send_qaudloop(a->argv[3], aloop);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_audio_loop, "audio loop", "audio loop <device> [on|off]", "Query/disable/enable audio loop on <device>")

static char* cli_audio_mode(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    static const char* const choices[] = {"handset", "headset", "speaker", "off", "bluetooth", "general", NULL};
    int amode                          = -1;

    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 4) {
                return ast_cli_complete(a->word, (ast_cli_complete2_t)choices, a->n);
            }
            if (a->pos == 3) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 4) {
        return CLI_SHOWUSAGE;
    }

    if (a->argc == 4) {  // query
        int res = query_qaudmod(a->argv[3]);
        ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");
        return CLI_SUCCESS;
    }

    // write
    if (!strcasecmp(choices[0], a->argv[4])) {
        amode = 0;
    } else if (!strcasecmp(choices[1], a->argv[4])) {
        amode = 1;
    } else if (!strcasecmp(choices[2], a->argv[4])) {
        amode = 2;
    } else if (!strcasecmp(choices[3], a->argv[4])) {
        amode = 3;
    } else if (!strcasecmp(choices[4], a->argv[4])) {
        amode = 4;
    } else if (!strcasecmp(choices[5], a->argv[4])) {
        amode = 5;
    } else {
        return CLI_SHOWUSAGE;
    }

    int res = send_qaudmod(a->argv[3], amode);
    ast_cli(a->fd, "[%s] %s\n", a->argv[3], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_audio_mode, "audio mode", "audio mode <device> [off|general|handset|headset|speaker|bluetooth]", "Query/set audio mode on <device>")

static const char* const audio_gain_choices[] = {"off", "mute", "full", "half", "0%",  "10%", "20%", "25%",  "30%",
                                                 "40%", "50%",  "60%",  "70%",  "75%", "80%", "90%", "100%", NULL};

static char* cli_audio_gain_tx(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 5) {
                return ast_cli_complete(a->word, (ast_cli_complete2_t)audio_gain_choices, a->n);
            }
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    if (a->argc == 5) {  // query
        int res = query_micgain(a->argv[4]);
        ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");
        return CLI_SUCCESS;
    }

    // write
    int gain;
    if (str2gain(a->argv[5], &gain)) {
        return CLI_SHOWUSAGE;
    }

    int res = send_micgain(a->argv[4], gain);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_audio_gain_tx, "audio gain tx", "audio gain tx <device> [level]", "Query/set microphone audio gain on <device>")

static char* cli_audio_gain_rx(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_GENERATE:
            if (a->pos == 5) {
                return ast_cli_complete(a->word, (ast_cli_complete2_t)audio_gain_choices, a->n);
            }
            if (a->pos == 4) {
                return complete_device(a->word, a->n);
            }
            return NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    if (a->argc == 5) {  // query
        int res = query_rxgain(a->argv[4]);
        ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");
        return CLI_SUCCESS;
    }

    // write
    int gain;
    if (str2gain(a->argv[5], &gain)) {
        return CLI_SHOWUSAGE;
    }

    int res = send_rxgain(a->argv[4], gain);
    ast_cli(a->fd, "[%s] %s\n", a->argv[4], res < 0 ? error2str(chan_quectel_err) : "Command queued for execute");

    return CLI_SUCCESS;
}

CLI_ALIASES(cli_audio_gain_rx, "audio gain rx", "audio gain rx <device> [level]", "Query/set RX audio gain on <device>")

static struct ast_cli_entry cli[] = {
    // clang-format off
	CLI_DEF_ENTRIES(cli_show_devices,			"Show devices state")
	CLI_DEF_ENTRIES(cli_show_device_settings,	"Show device settings")
	CLI_DEF_ENTRIES(cli_show_device_state,	 	"Show device state")
	CLI_DEF_ENTRIES(cli_show_device_statistics,	"Show device statistics")
	CLI_DEF_ENTRIES(cli_show_version,			"Show module version")
	CLI_DEF_ENTRIES(cli_cmd,					"Send commands to port for debugging")
	CLI_DEF_ENTRIES(cli_ussd,					"Send USSD commands")

	CLI_DEF_ENTRIES(cli_sms_send,					"Send message")
	CLI_DEF_ENTRIES(cli_sms_list_received_unread,	"List unread messages")
	CLI_DEF_ENTRIES(cli_sms_list_received_read,		"List read messages")
	CLI_DEF_ENTRIES(cli_sms_list_all,				"List messages")
	CLI_DEF_ENTRIES(cli_sms_delete_received_read,	"Delete read messages")
	CLI_DEF_ENTRIES(cli_sms_delete_all,				"Delete all messages")
	CLI_DEF_ENTRIES(cli_sms_delete,					"Delete message by index")
	CLI_DEF_ENTRIES(cli_sms_direct_on,				"Receive messages directly")
	CLI_DEF_ENTRIES(cli_sms_direct_off,				"Receive messages indirectly")
	CLI_DEF_ENTRIES(cli_sms_direct_auto,			"Receive messages in a way specified in configuration")
    CLI_DEF_ENTRIES(cli_sms_db_backup,			    "Backup SMS database")

	CLI_DEF_ENTRIES(cli_ccwa_set,				"Enable/Disable Call-Waiting")
	CLI_DEF_ENTRIES(cli_reset,					"Reset modem")

	CLI_DEF_ENTRIES(cli_stop,					"Stop channel")
	CLI_DEF_ENTRIES(cli_restart,				"Restart channel")
	CLI_DEF_ENTRIES(cli_remove,					"Remove channel")
	CLI_DEF_ENTRIES(cli_reload,					"Reload channel")
    AST_CLI_DEFINE(cli_uac_apply,               "Apply UAC mode"),

	CLI_DEF_ENTRIES(cli_start,					"Start channel")

	CLI_DEF_ENTRIES(cli_audio_loop,				"Query/enable/disable audio loop test")
	CLI_DEF_ENTRIES(cli_audio_mode,				"Query/set audio mode")
	CLI_DEF_ENTRIES(cli_audio_gain_rx,			"Query/set RX audio gain")
	CLI_DEF_ENTRIES(cli_audio_gain_tx,			"Query/set TX audio gain")
// clang-format on	
};

#/* */
void cli_register()
{
	ast_cli_register_multiple(cli, ARRAY_LEN(cli));
}

#/* */
void cli_unregister()
{
	ast_cli_unregister_multiple(cli, ARRAY_LEN(cli));
}

/*
static char * ast_str_truncate2(struct ast_str *buf, ssize_t len)
{
	if (len < 0) {
		buf->__AST_STR_USED += ((ssize_t) abs(len)) > ((ssize_t) buf->__AST_STR_USED) ? (ssize_t)-buf->__AST_STR_USED : (ssize_t)len;
	} else {
		buf->__AST_STR_USED = len;
	}
	buf->__AST_STR_STR[buf->__AST_STR_USED] = '\0';
	return buf->__AST_STR_STR;
}
*/
