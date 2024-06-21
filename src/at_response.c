/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>

   Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/
#include <sys/sysinfo.h>

#include "ast_config.h"

#include <asterisk/causes.h> /* AST_CAUSE_... definitions */
#include <asterisk/channel.h>
#include <asterisk/json.h>
#include <asterisk/logger.h> /* ast_debug() */
#include <asterisk/pbx.h>    /* ast_pbx_start() */

#include "at_response.h"

#include "at_parse.h"
#include "at_queue.h"
#include "at_read.h"
#include "chan_quectel.h"
#include "channel.h" /* channel_queue_hangup() channel_queue_control() */
#include "char_conv.h"
#include "error.h"
#include "eventfd.h"
#include "helpers.h"
#include "mutils.h" /* STRLEN() */
#include "smsdb.h"

// ================================================================

static const unsigned int CCWA_STATUS_NOT_ACTIVE = 0u;
static const unsigned int CCWA_STATUS_ACTIVE     = 1u;

static const unsigned int CLCC_CALL_TYPE_VOICE = 0u;
/*
static const unsigned int CLCC_CALL_TYPE_DATA = 1;
static const unsigned int CLCC_CALL_TYPE_FAX = 2;
*/

static const char MANUFACTURER_QUECTEL[] = "Quectel";
static const char MANUFACTURER_SIMCOM[]  = "SimCom";

static const int DST_DEF_LEN       = 32;
static const size_t AST_TM_MAX_LEN = 64;

// ================================================================

static const at_response_t at_responses_list[] = {

    AT_RESPONSES_TABLE(AT_RES_AS_STRUCTLIST)
/* clang-format off */
    /* The hackish way to define the duplicated responses in the meantime */
#define DEF_STR(str) str, STRLEN(str)
    {RES_CNUM,  "+CNUM", DEF_STR("ERROR+CNUM:")          },
    {RES_ERROR, "ERROR", DEF_STR("COMMAND NOT SUPPORT\r")},
#undef DEF_STR
    /* clang-format on */
};

const at_responses_t at_responses = {at_responses_list, 3, ARRAY_LEN(at_responses_list), RES_MIN, RES_MAX};

/*!
 * \brief Get the string representation of the given AT response
 * \param res -- the response to process
 * \return a string describing the given response
 */

const char* at_res2str(at_res_t res)
{
    if ((int)res >= at_responses.name_first && (int)res <= at_responses.name_last) {
        return at_responses.responses[res - at_responses.name_first].name;
    }
    return "UNDEFINED";
}

at_res_t at_str2res(const struct ast_str* const result)
{
    at_res_t at_res  = RES_UNKNOWN;
    const size_t len = ast_str_strlen(result);
    if (!len) {
        return at_res;
    }
    const char* const buf = ast_str_buffer(result);

    for (unsigned i = at_responses.ids_first; i < at_responses.ids; ++i) {
        if (at_responses.responses[i].idlen) {
            const at_response_t* const resp = &at_responses.responses[i];
            const size_t idlen1             = resp->idlen - 1;
            const char lc                   = buf[len - 1];
            if (resp->id[idlen1] == '\r' && lc != '\r') {
                if (idlen1 != len || memcmp(buf, resp->id, idlen1)) {
                    continue;
                }
                at_res = resp->res;
                break;
            }
        }

        if (len < at_responses.responses[i].idlen || memcmp(buf, at_responses.responses[i].id, at_responses.responses[i].idlen)) {
            continue;
        }

        at_res = at_responses.responses[i].res;
        break;
    }

    return at_res;
}

static int safe_task_uid(const at_queue_task_t* const task) { return task ? task->uid : -1; }

static struct cpvt* safe_get_cpvt(const at_queue_task_t* const task, struct pvt* const pvt)
{
    if (task && task->cpvt) {
        return task->cpvt;
    }
    return &pvt->sys_chan;
}

static int from_ucs2(const char* const hucs2, char* const utf8_str, size_t utf8_str_size)
{
    const int nibbles = unhex(hucs2, (uint8_t*)hucs2);
    const ssize_t res = ucs2_to_utf8((const uint16_t*)hucs2, (nibbles + 1) / 4, utf8_str, utf8_str_size);
    if (res < 0) {
        return -1;
    }
    utf8_str[res] = '\000';
    return 0;
}

static void request_clcc(struct pvt* pvt)
{
    if (at_enqueue_clcc(&pvt->sys_chan)) {
        ast_log(LOG_ERROR, "[%s] Error enqueue List Current Calls request\n", PVT_ID(pvt));
    }
}

static int at_response_cmgs_error(struct pvt*, const at_queue_task_t* const);

static void __attribute__((format(printf, 7, 8))) at_ok_response_log(int level, const char* file, int line, const char* function, const struct pvt* const pvt,
                                                                     const at_queue_cmd_t* const ecmd, const char* const fmt, ...)
{
    static const ssize_t MSG_DEF_LEN = 128;
    // U+2713 : Check mark : 0xE2 0x9C 0x93

    RAII_VAR(struct ast_str*, msg, ast_str_create(MSG_DEF_LEN), ast_free);

    if (ecmd) {
        ast_str_set(&msg, 0, "[%s][%s] \xE2\x9C\x93", PVT_ID(pvt), at_cmd2str(ecmd->cmd));
    } else {
        ast_str_set(&msg, 0, "[%s] \xE2\x9C\x93", PVT_ID(pvt));
    }

    if (fmt) {
        ast_str_append(&msg, 0, " ");
        va_list ap;
        va_start(ap, fmt);
        ast_str_append_va(&msg, 0, fmt, ap);
        va_end(ap);
    }

    ast_log(level, file, line, function, "%s\n", ast_str_buffer(msg));
}

#define at_ok_response_dbg(level, pvt, ecmd, ...)                      \
    do {                                                               \
        if (DEBUG_ATLEAST(level)) {                                    \
            at_ok_response_log(AST_LOG_DEBUG, pvt, ecmd, __VA_ARGS__); \
        }                                                              \
    } while (0)

#define at_ok_response_err(pvt, ecmd, ...) at_ok_response_log(LOG_ERROR, pvt, ecmd, __VA_ARGS__)
#define at_ok_response_wrn(pvt, ecmd, ...) at_ok_response_log(LOG_WARNING, pvt, ecmd, __VA_ARGS__)
#define at_ok_response_notice(pvt, ecmd, ...) at_ok_response_log(LOG_NOTICE, pvt, ecmd, __VA_ARGS__)

static int at_response_ok(struct pvt* const pvt, const at_res_t at_res, const at_queue_task_t* const task, const at_queue_cmd_t* const ecmd)
{
    if (!ecmd) {
        at_ok_response_err(pvt, ecmd, "Received unexpected [%s], ignoring", at_res2str(at_res));
        return 0;
    }

    if (ecmd->res != at_res) {
        at_ok_response_wrn(pvt, ecmd, "Received unexpected [%s], ignoring", at_res2str(at_res));
        return 0;
    }

    switch (ecmd->cmd) {
        case CMD_AT:
        case CMD_AT_Z:
        case CMD_AT_E:
        case CMD_AT_CGMI:
        case CMD_AT_CGMM:
        case CMD_AT_CGMR:
        case CMD_AT_CMEE:
        case CMD_AT_CGSN:
        case CMD_AT_CIMI:
        case CMD_AT_CPIN:
        case CMD_AT_CCWA_SET:
        case CMD_AT_CCWA_STATUS:
        case CMD_AT_CHLD_2:
        case CMD_AT_CHLD_3:
        case CMD_AT_CSCA:
        case CMD_AT_CLCC:
        case CMD_AT_CLIR:
        case CMD_AT_QINDCFG_CSQ:
        case CMD_AT_QINDCFG_ACT:
        case CMD_AT_QINDCFG_RING:
        case CMD_AT_QINDCFG_CC:
        case CMD_AT_QINDCFG_CC_OFF:
        case CMD_AT_DSCI:
        case CMD_AT_DSCI_OFF:
        case CMD_AT_QLTS:
        case CMD_AT_QLTS_1:
        case CMD_AT_CCLK:
            // U+2713 : Check mark
            at_ok_response_dbg(3, pvt, ecmd, NULL);
            break;

        case CMD_AT_FINAL:
            ast_verb(1, "[%s] Channel initialized\n", PVT_ID(pvt));
            if (CONF_UNIQ(pvt, uac) == TRIBOOL_NONE) {
                pvt_set_act(pvt, 1);  // GSM
            }
            if (pvt->has_sms) {
                at_enqueue_list_messages(task->cpvt, MSG_STAT_REC_UNREAD);
            }
            at_enqueue_csq(task->cpvt);
            pvt->initialized = 1;
            break;

        case CMD_AT_COPS_INIT:
            at_ok_response_dbg(1, pvt, ecmd, "Operator select parameters set");
            break;

        case CMD_AT_CREG_INIT:
        case CMD_AT_CEREG_INIT:
            at_ok_response_dbg(1, pvt, ecmd, "Registration info enabled");
            break;

        case CMD_AT_CREG:
        case CMD_AT_CEREG:
            at_ok_response_dbg(1, pvt, ecmd, "Registration query sent");
            break;

        case CMD_AT_CNUM:
            at_ok_response_dbg(1, pvt, ecmd, "Subscriber phone number query successed");
            break;

        case CMD_AT_CVOICE:
            at_ok_response_dbg(1, pvt, ecmd, "Voice calls supported");

            switch (CONF_UNIQ(pvt, uac)) {
                case TRIBOOL_TRUE:
                    at_enqueue_enable_uac(&pvt->sys_chan);
                    break;

                case TRIBOOL_FALSE:
                    at_enqueue_enable_tty(&pvt->sys_chan);
                    break;

                default:
                    break;
            }
            break;

        case CMD_AT_CPCMREG:
            at_ok_response_dbg(1, pvt, ecmd, "Voice calls supported");

            pvt->has_voice = 1;
            at_enqueue_cpcmfrm(task->cpvt, CONF_UNIQ(pvt, slin16));
            at_enqueue_cpcmreg(task->cpvt, 0);
            at_enqueue_cgains(task->cpvt, CONF_SHARED(pvt, txgain), CONF_SHARED(pvt, rxgain));
            at_enqueue_query_cgains(task->cpvt);
            break;

        case CMD_AT_QPCMV_0:
            at_ok_response_dbg(3, pvt, ecmd, NULL);

            pvt->has_voice = 0;
            break;

        case CMD_AT_QPCMV_TTY:
        case CMD_AT_QPCMV_UAC:
            at_ok_response_dbg(3, pvt, ecmd, NULL);

            pvt->has_voice = 1;
            at_enqueue_qgains(&pvt->sys_chan, CONF_SHARED(pvt, txgain), CONF_SHARED(pvt, rxgain));
            at_enqueue_query_qgains(&pvt->sys_chan);
            break;

        case CMD_AT_CSSN:
            at_ok_response_dbg(1, pvt, ecmd, "Supplementary Service Notification disabled");
            break;

        case CMD_AT_CMGF:
            at_ok_response_dbg(1, pvt, ecmd, "SMS operation mode set to PDU");
            break;

        case CMD_AT_CSCS:
            at_ok_response_dbg(2, pvt, ecmd, "UCS-2 text encoding enabled");
            break;

        case CMD_AT_CPMS:
            at_ok_response_dbg(1, pvt, ecmd, "Message storage location is configured");
            break;

        case CMD_AT_CNMI:
            pvt->has_sms = 1;
            if (!pvt->initialized) {
                ast_debug(1, "[%s] SMS supported\n", PVT_ID(pvt));
                at_ok_response_dbg(2, pvt, ecmd, "SMS indication mode configured");
            } else {
                at_ok_response_dbg(2, pvt, ecmd, "SMS indication mode configured");
                ast_verb(2, "[%s] Message indication mode configured\n", PVT_ID(pvt));
            }
            break;

        case CMD_AT_D:
            pvt->dialing = 1;
            cpvt_change_state(task->cpvt, CALL_STATE_DIALING, 0);
        /* fall through */
        case CMD_AT_A:
        case CMD_AT_CHLD_2x:
            /* not work, ^CONN: appear before OK for CHLD_ANSWER
                            task->cpvt->answered = 1;
                            task->cpvt->needhangup = 1;
            */
            CPVT_SET_FLAG(task->cpvt, CALL_FLAG_NEED_HANGUP);
            at_ok_response_dbg(3, pvt, ecmd, "Call id:%d\n", task->cpvt->call_idx);
            break;

        case CMD_AT_CFUN:
            /* in case of reset */
            pvt->ring     = 0;
            pvt->dialing  = 0;
            pvt->cwaiting = 0;
            break;

        case CMD_AT_CPCMREG1:
            at_ok_response_dbg(3, pvt, ecmd, NULL);
            if (!pvt->initialized) {
                pvt->initialized = 1;
                ast_verb(3, "[%s] SimCom initialized and ready\n", PVT_ID(pvt));
            }
            break;

        case CMD_AT_CPCMREG0:
            at_ok_response_dbg(4, pvt, ecmd, NULL);
            break;

        case CMD_AT_CHUP:
        case CMD_AT_QHUP:
        case CMD_AT_CHLD_1x:
            CPVT_RESET_FLAG(task->cpvt, CALL_FLAG_NEED_HANGUP);
            at_ok_response_dbg(1, pvt, ecmd, "Successful hangup for call idx:%d", task->cpvt->call_idx);
            break;

        case CMD_AT_CMGS:
            at_ok_response_dbg(3, pvt, ecmd, "Sending message in progress");
            break;

        case CMD_AT_SMSTEXT: {
            const at_cmd_t cmd = task->cmds[0].cmd;
            if (cmd == CMD_AT_CMGS) {
                at_ok_response_dbg(3, pvt, ecmd, "Sending SMS message in progress");
            } else if (cmd == CMD_AT_CNMA) {
                at_ok_response_dbg(1, pvt, ecmd, "[SMS:%d] Message confirmed\n", safe_task_uid(task));
            } else {
                at_ok_response_err(pvt, ecmd, "Unexpected message text response");
            }
            break;
        }

        case CMD_AT_DTMF:
            at_ok_response_dbg(3, pvt, ecmd, "DTMF sent successfully for call idx:%d", task->cpvt->call_idx);
            break;

        case CMD_AT_CUSD:
            at_ok_response_dbg(3, pvt, ecmd, "Successfully sent USSD %p", task);
            ast_verb(3, "[%s] Successfully sent USSD %p\n", PVT_ID(pvt), task);
            break;

        case CMD_AT_COPS:
        case CMD_AT_QSPN:
        case CMD_AT_CSPN:
            at_ok_response_dbg(3, pvt, ecmd, "Successfull provider query");
            break;

        case CMD_AT_CMGR:
            at_ok_response_dbg(3, pvt, ecmd, NULL);
            at_sms_retrieved(&pvt->sys_chan, 1);
            break;

        case CMD_AT_CMGD:
            at_ok_response_dbg(3, pvt, ecmd, "Message deleted successfully");
            break;

        case CMD_AT_CSQ:
            at_ok_response_dbg(3, pvt, ecmd, "Got signal strength");
            break;

        case CMD_AT_AUTOCSQ_INIT:
        case CMD_AT_EXUNSOL_INIT:
            at_ok_response_dbg(1, pvt, ecmd, "Signal strength change notifications enabled");
            break;

        case CMD_AT_CLTS_INIT:
            at_ok_response_dbg(1, pvt, ecmd, "Time update notifications enabled");
            break;

        case CMD_AT_CLVL:
            pvt->volume_sync_step++;
            if (pvt->volume_sync_step == VOLUME_SYNC_DONE) {
                at_ok_response_dbg(1, pvt, ecmd, "Volume level synchronized");
                pvt->volume_sync_step = VOLUME_SYNC_BEGIN;
            }
            break;

        case CMD_AT_CMUT_0:
            at_ok_response_dbg(1, pvt, ecmd, "Uplink voice unmuted");
            break;

        case CMD_AT_CMUT_1:
            at_ok_response_dbg(1, pvt, ecmd, "Uplink voice muted");
            break;

        case CMD_AT_QTONEDET_0:
        case CMD_AT_DDET_0:
            at_ok_response_dbg(1, pvt, ecmd, "Tone detection disabled");
            break;

        case CMD_AT_QTONEDET_1:
        case CMD_AT_DDET_1:
            at_ok_response_dbg(1, pvt, ecmd, "Tone detection enabled");
            break;

        case CMD_AT_QMIC:
        case CMD_AT_QRXGAIN:
        case CMD_AT_CMICGAIN:
        case CMD_AT_COUTGAIN:
            at_ok_response_dbg(1, pvt, ecmd, "TX/RX gains updated");
            break;

        case CMD_AT_CRXVOL:
        case CMD_AT_CTXVOL:
            at_ok_response_dbg(3, pvt, ecmd, "TX/RX volume updated");
            break;

        case CMD_AT_CMGL:
            at_ok_response_dbg(1, pvt, ecmd, "Messages listed");
            break;

        case CMD_AT_CNMA:
            at_ok_response_dbg(1, pvt, ecmd, "[SMS:%d] Message confirmed", safe_task_uid(task));
            break;

        case CMD_AT_CSMS:
            at_ok_response_dbg(1, pvt, ecmd, "Message service channel configured");
            break;

        case CMD_AT_QAUDLOOP:
            at_ok_response_dbg(1, pvt, ecmd, "Audio loop configured");
            break;

        case CMD_AT_QAUDMOD:
            at_ok_response_dbg(1, pvt, ecmd, "Audio mode configured");
            break;

        case CMD_AT_CNSMOD_0:
            at_ok_response_dbg(1, pvt, ecmd, "Network mode notifications disabled");
            break;

        case CMD_AT_CNSMOD_1:
            at_ok_response_dbg(1, pvt, ecmd, "Network mode notifications enabled");
            break;

        case CMD_AT_CPCMFRM_8K:
            at_ok_response_notice(pvt, ecmd, "Audio sample rate set to 8kHz");
            break;

        case CMD_AT_CPCMFRM_16K:
            at_ok_response_notice(pvt, ecmd, "Audio sample rate set to 16kHz");
            break;

        case CMD_AT_VTD:
            at_ok_response_dbg(2, pvt, ecmd, "Tone duration updated");
            break;

        case CMD_AT_CCID:
            at_ok_response_dbg(3, pvt, ecmd, "ICCID obtained");
            break;

        case CMD_AT_CICCID:
        case CMD_AT_QCCID:
            at_ok_response_dbg(3, pvt, ecmd, "ICCID obtained");
            break;

        case CMD_ESC:
            at_ok_response_dbg(1, pvt, ecmd, "[SMS:%d] Message confirmed", safe_task_uid(task));
            break;

        case CMD_USER:
            break;

        default:
            at_ok_response_err(pvt, ecmd, "Unhandled command");
            break;
    }

    at_queue_handle_result(pvt, at_res);
    return 0;
}

static void __attribute__((format(printf, 7, 8))) at_err_response_log(int level, const char* file, int line, const char* function, const struct pvt* const pvt,
                                                                      const at_queue_cmd_t* const ecmd, const char* const fmt, ...)
{
    static const ssize_t MSG_DEF_LEN = 128;
    // U+237B: Not check mark

    RAII_VAR(struct ast_str*, msg, ast_str_create(MSG_DEF_LEN), ast_free);

    if (ecmd) {
        ast_str_set(&msg, 0, "[%s][%s] \xE2\x8D\xBB", PVT_ID(pvt), at_cmd2str(ecmd->cmd));
    } else {
        ast_str_set(&msg, 0, "[%s] \xE2\x8D\xBB", PVT_ID(pvt));
    }

    if (fmt) {
        ast_str_append(&msg, 0, " ");
        va_list ap;
        va_start(ap, fmt);
        ast_str_append_va(&msg, 0, fmt, ap);
        va_end(ap);
    }

    ast_log(level, file, line, function, "%s\n", ast_str_buffer(msg));
}

#define at_err_response_dbg(level, pvt, ecmd, ...)                      \
    do {                                                                \
        if (DEBUG_ATLEAST(level)) {                                     \
            at_err_response_log(AST_LOG_DEBUG, pvt, ecmd, __VA_ARGS__); \
        }                                                               \
    } while (0)

#define at_err_response_err(pvt, ecmd, ...) at_err_response_log(LOG_ERROR, pvt, ecmd, __VA_ARGS__)
#define at_err_response_wrn(pvt, ecmd, ...) at_err_response_log(LOG_WARNING, pvt, ecmd, __VA_ARGS__)

/*!
 * \brief Handle ERROR response
 * \param pvt -- pvt structure
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_error(struct pvt* const pvt, const at_res_t at_res, const at_queue_task_t* const task, const at_queue_cmd_t* const ecmd)
{
    if (!ecmd) {
        at_err_response_err(pvt, ecmd, "Received unexpected [%s], ignoring", at_res2str(at_res));
        return 0;
    }

    if (!(ecmd->res == RES_OK || ecmd->res == RES_SMS_PROMPT)) {
        at_err_response_err(pvt, ecmd, "Received unexpected [%s], ignoring", at_res2str(at_res));
        return 0;
    }

    switch (ecmd->cmd) {
        /* critical errors */
        case CMD_AT:
        case CMD_AT_Z:
        case CMD_AT_E:
        case CMD_AT_CLCC:
            at_err_response_err(pvt, ecmd, NULL);
            /* mean disconnected from device */
            goto e_return;

        case CMD_AT_FINAL:
            at_err_response_err(pvt, ecmd, "Channel not initialized");
            pvt->initialized = 0;
            goto e_return;

        /* not critical errors */
        case CMD_AT_CCWA_SET:
        case CMD_AT_CCWA_STATUS:
        case CMD_AT_CNUM:
            at_err_response_err(pvt, ecmd, NULL);
            /* mean ignore error */
            break;

        case CMD_AT_CGMI:
            at_err_response_err(pvt, ecmd, "Unable to get manufacturer info");
            goto e_return;

        case CMD_AT_CGMM:
            at_err_response_err(pvt, ecmd, "Unable to get model info");
            goto e_return;

        case CMD_AT_CGMR:
            at_err_response_err(pvt, ecmd, "Unable to get firmware info");
            goto e_return;

        case CMD_AT_CMEE:
            at_err_response_err(pvt, ecmd, "Fail to set verbosity level");
            goto e_return;

        case CMD_AT_CGSN:
            at_err_response_err(pvt, ecmd, "Unable to get IMEI number");
            goto e_return;

        case CMD_AT_CIMI:
            at_err_response_err(pvt, ecmd, "Unable to get IMSI number");
            goto e_return;

        case CMD_AT_CPIN:
            at_err_response_err(pvt, ecmd, "Error checking PIN state");
            goto e_return;

        case CMD_AT_COPS_INIT:
            at_err_response_err(pvt, ecmd, "Error setting operator select parameters");
            goto e_return;

        case CMD_AT_CREG_INIT:
            at_err_response_err(pvt, ecmd, "Error enabling registration info");
            goto e_return;

        case CMD_AT_CEREG_INIT:
            at_err_response_dbg(1, pvt, ecmd, "Error enabling registration info");
            break;

        case CMD_AT_AUTOCSQ_INIT:
        case CMD_AT_EXUNSOL_INIT:
            at_err_response_dbg(1, pvt, ecmd, "Error enabling CSQ(E) report");
            break;

        case CMD_AT_CLTS_INIT:
            at_err_response_dbg(2, pvt, ecmd, "Time update notifications not available");
            break;

        case CMD_AT_CREG:
            at_err_response_dbg(1, pvt, ecmd, "Error getting registration info");
            break;

        case CMD_AT_QINDCFG_CSQ:
        case CMD_AT_QINDCFG_ACT:
        case CMD_AT_QINDCFG_RING:
        case CMD_AT_QINDCFG_CC:
        case CMD_AT_DSCI:
            at_err_response_dbg(1, pvt, ecmd, "Error enabling indications");
            break;

        case CMD_AT_QINDCFG_CC_OFF:
            at_err_response_dbg(CONF_SHARED(pvt, dsci) ? 1 : 4, pvt, ecmd, "Error disabling indications");
            break;

        case CMD_AT_DSCI_OFF:
            at_err_response_dbg(CONF_SHARED(pvt, dsci) ? 4 : 1, pvt, ecmd, "Error disabling indications");

        case CMD_AT_CVOICE:
            at_err_response_dbg(1, pvt, ecmd, "Voice calls not supported");
            pvt->has_voice = 0;
            break;

        case CMD_AT_CPCMREG:
            if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
                pvt->has_voice = 1;
            } else {
                at_err_response_dbg(1, pvt, ecmd, "Voice calls not supported");
                pvt->has_voice = 0;
            }
            break;

        case CMD_AT_CSSN:
            at_err_response_err(pvt, ecmd, "Unable to activate Supplementary Service Notifications");
            goto e_return;

        case CMD_AT_CSCA:
        case CMD_AT_CMGF:
        case CMD_AT_CPMS:
        case CMD_AT_CNMI:
            at_err_response_dbg(1, pvt, ecmd, "No SMS support");
            pvt->has_sms = 0;
            break;

        case CMD_AT_CSCS:
            at_err_response_err(pvt, ecmd, "No UCS-2 encoding support");
            goto e_return;

        case CMD_AT_A:
        case CMD_AT_CHLD_2x:
            at_err_response_err(pvt, ecmd, "Answer failed for call idx:%d", task->cpvt->call_idx);
            channel_enqueue_hangup(task->cpvt->channel, AST_CAUSE_CALL_REJECTED);
            break;

        case CMD_AT_CHLD_3:
            at_err_response_err(pvt, ecmd, "Can't begin conference call idx:%d", task->cpvt->call_idx);
            channel_enqueue_hangup(task->cpvt->channel, AST_CAUSE_CALL_REJECTED);
            break;

        case CMD_AT_CLIR:
            at_err_response_err(pvt, ecmd, "Setting CLIR failed");
            break;

        case CMD_AT_CHLD_2:
            if (!CPVT_TEST_FLAG(task->cpvt, CALL_FLAG_HOLD_OTHER) || task->cpvt->state != CALL_STATE_INIT) {
                break;
            }
            /* fall through */
        case CMD_AT_D:
            at_err_response_err(pvt, ecmd, "Dial failed");
            cpvt_change_state(task->cpvt, CALL_STATE_RELEASED, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
            break;

        case CMD_AT_CPCMREG1:
            if (CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE && CPVT_IS_ACTIVE(task->cpvt)) {
                at_err_response_dbg(3, pvt, ecmd, "Trying to activate audio stream again");
                at_enqueue_cpcmreg(task->cpvt, 1);
            } else {
                at_err_response_err(pvt, ecmd, "Could not activate audio stream");
            }
            break;

        case CMD_AT_CPCMREG0:
            at_err_response_err(pvt, ecmd, "Could not deactivate audio stream");
            break;

        case CMD_AT_CHUP:
        case CMD_AT_QHUP:
        case CMD_AT_CHLD_1x:
            at_err_response_err(pvt, ecmd, "Error sending hangup for call idx:%d", task->cpvt->call_idx);
            break;

        case CMD_AT_CMGR:
            // log_cmd_response(LOG_ERROR, pvt, ecmd, "Error reading SMS message, resetting index");
            at_err_response_dbg(1, pvt, ecmd, "[SMS:%d] Fail to read message", pvt->incoming_sms_index);
            at_sms_retrieved(&pvt->sys_chan, 0);
            break;

        case CMD_AT_CMGD:
            at_err_response_err(pvt, ecmd, "Error deleting message");
            break;

        case CMD_AT_CMGS:
            at_err_response_err(pvt, ecmd, "[SMS:%d] Error sending message", task->uid);
            at_response_cmgs_error(pvt, task);
            pvt_try_restate(pvt);
            break;

        case CMD_AT_SMSTEXT: {
            const at_cmd_t cmd = task->cmds[0].cmd;
            if (cmd == CMD_AT_CMGS) {
                at_err_response_err(pvt, ecmd, "[SMS:%d] Error sending message", task->uid);
                at_response_cmgs_error(pvt, task);
                pvt_try_restate(pvt);
            } else if (cmd == CMD_AT_CNMA) {
                at_err_response_err(pvt, ecmd, "[SMS:%d] Cannot acknowledge message", task->uid);
            } else {
                at_err_response_err(pvt, ecmd, "Unexpected SMS text prompt");
            }
            break;
        }

        case CMD_ESC:
            at_err_response_err(pvt, ecmd, "Cannot acknowledge message");
            break;

        case CMD_AT_DTMF:
            at_err_response_err(pvt, ecmd, "Error sending DTMF");
            break;

        case CMD_AT_COPS:
        case CMD_AT_QSPN:
        case CMD_AT_CSPN:
            at_err_response_dbg(1, pvt, ecmd, "Could not get provider name");
            break;

        case CMD_AT_QLTS:
        case CMD_AT_QLTS_1:
        case CMD_AT_CCLK:
            at_err_response_dbg(2, pvt, ecmd, "Could not query time");
            break;

        case CMD_AT_CLVL:
            at_err_response_dbg(1, pvt, ecmd, "Audio level synchronization failed at step %d/%d", pvt->volume_sync_step, VOLUME_SYNC_DONE - 1);
            pvt->volume_sync_step = VOLUME_SYNC_BEGIN;
            break;

        case CMD_AT_CUSD:
            ast_verb(3, "[%s] Error sending USSD %p\n", PVT_ID(pvt), task);
            at_err_response_err(pvt, ecmd, "Error sending USSD %p", task);
            break;

        case CMD_AT_CMUT_0:
            at_err_response_dbg(1, pvt, ecmd, "Cannot unmute uplink voice");
            break;

        case CMD_AT_CMUT_1:
            at_err_response_dbg(1, pvt, ecmd, "Cannot mute uplink voice");
            break;

        case CMD_AT_QPCMV_0:
            at_err_response_wrn(pvt, ecmd, "Cannot disable UAC");
            break;

        case CMD_AT_QPCMV_TTY:
            at_err_response_wrn(pvt, ecmd, "Cannot enable audio on serial port");
            break;

        case CMD_AT_QPCMV_UAC:
            at_err_response_wrn(pvt, ecmd, "Cannot enable UAC");
            break;

        case CMD_AT_QTONEDET_0:
        case CMD_AT_DDET_0:
            at_err_response_wrn(pvt, ecmd, "Cannot disable tone detection");
            break;

        case CMD_AT_QTONEDET_1:
        case CMD_AT_DDET_1:
            at_err_response_wrn(pvt, ecmd, "Cannot enable tone detection");
            break;

        case CMD_AT_QMIC:
        case CMD_AT_QRXGAIN:
        case CMD_AT_COUTGAIN:
        case CMD_AT_CMICGAIN:
            at_err_response_wrn(pvt, ecmd, "Cannot update TX/RG gain");
            break;

        case CMD_AT_CTXVOL:
        case CMD_AT_CRXVOL:
            at_err_response_wrn(pvt, ecmd, "Cannot update TX/RG volume");
            break;

        case CMD_AT_CMGL:
            at_err_response_dbg(1, pvt, ecmd, "Cannot list messages");
            break;

        case CMD_AT_CNMA:
            at_err_response_wrn(pvt, ecmd, "Cannot confirm message reception");
            break;

        case CMD_AT_CSMS:
            at_err_response_wrn(pvt, ecmd, "Message service channel not configured");
            break;

        case CMD_AT_QAUDLOOP:
            at_err_response_wrn(pvt, ecmd, "Audio loop not configured");
            break;

        case CMD_AT_QAUDMOD:
            at_err_response_wrn(pvt, ecmd, "Audio mode not configured");
            break;

        case CMD_AT_CNSMOD_0:
            at_err_response_wrn(pvt, ecmd, "Could not disable network mode notifications");
            break;

        case CMD_AT_CNSMOD_1:
            at_err_response_wrn(pvt, ecmd, "Could not enable network mode notifications");
            break;

        case CMD_AT_CPCMFRM_8K:
        case CMD_AT_CPCMFRM_16K:
            at_err_response_wrn(pvt, ecmd, "Could not set audio sample rate");
            break;

        case CMD_AT_VTD:
            at_err_response_wrn(pvt, ecmd, "Could not set tone duration");
            break;

        case CMD_AT_CCID:
            at_err_response_dbg(2, pvt, ecmd, "Could not get ICCID");
            break;

        case CMD_AT_CICCID:
        case CMD_AT_QCCID:
            at_err_response_wrn(pvt, ecmd, "Could not get ICCID");
            break;

        case CMD_USER:
            break;

        default:
            at_err_response_err(pvt, ecmd, "Unhandled command");
            break;
    }

    at_queue_handle_result(pvt, at_res);
    return 0;

e_return:
    at_queue_handle_result(pvt, at_res);
    return -1;
}

static int start_pbx(struct pvt* const pvt, const char* const number, const int call_idx, const call_state_t state)
{
    /* TODO: pass also Subscriber number or other DID info for exten  */
    struct ast_channel* channel = channel_new(pvt, AST_STATE_RING, number, call_idx, CALL_DIR_INCOMING, state,
                                              pvt->has_subscriber_number ? pvt->subscriber_number : CONF_SHARED(pvt, exten), NULL, NULL, 0);

    if (!channel) {
        ast_log(LOG_ERROR, "[%s] Unable to allocate channel for incoming call\n", PVT_ID(pvt));

        if (at_enqueue_hangup(&pvt->sys_chan, call_idx, AST_CAUSE_DESTINATION_OUT_OF_ORDER)) {
            ast_log(LOG_ERROR, "[%s] Error sending AT+CHUP command\n", PVT_ID(pvt));
        }

        return -1;
    }

    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    // FIXME: not execute if channel_new() failed
    CPVT_SET_FLAG(cpvt, CALL_FLAG_NEED_HANGUP);

    /* ast_pbx_start() usually failed if asterisk.conf minmemfree
     * set too low, try drop buffer cache
     * sync && echo 3 >/proc/sys/vm/drop_caches
     */
    if (ast_pbx_start(channel)) {
        ast_channel_tech_pvt_set(channel, NULL);
        cpvt_free(cpvt);

        ast_hangup(channel);
        ast_log(LOG_ERROR, "[%s] Unable to start pbx on incoming call\n", PVT_ID(pvt));
        // TODO: count fails and reset incoming when count reach limit ?
        return -1;
    }

    return 0;
}

static void handle_clcc(struct pvt* const pvt, const unsigned int call_idx, const unsigned int dir, const unsigned int state, const unsigned int mode,
                        const tristate_bool_t mpty, const char* const number, const unsigned int type)
{
    struct cpvt* cpvt = pvt_channel_find_by_call_idx(pvt, (int)call_idx);
    int process_state = 1;

    if (cpvt) {
        /* cpvt alive */
        CPVT_SET_FLAG(cpvt, CALL_FLAG_ALIVE);

        if (dir != CPVT_DIRECTION(cpvt)) {
            ast_log(LOG_ERROR, "[%s] CLCC call idx:%d - direction mismatch %d/%d\n", PVT_ID(pvt), cpvt->call_idx, dir, CPVT_DIRECTION(cpvt));
            return;
        }

        if (mpty) {
            if (CONF_SHARED(pvt, multiparty)) {
                if (mpty > 0) {
                    CPVT_SET_FLAG(cpvt, CALL_FLAG_MULTIPARTY);
                } else {
                    CPVT_RESET_FLAG(cpvt, CALL_FLAG_MULTIPARTY);
                }
            } else {
                if (!CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && mpty > 0) {
                    ast_log(LOG_ERROR, "[%s] Rejecting multiparty call - idx:%d\n", PVT_ID(pvt), call_idx);
                    at_enqueue_hangup(&pvt->sys_chan, call_idx, AST_CAUSE_CALL_REJECTED);
                    process_state = 0;
                }
            }
        }

        if (process_state) {
            process_state = cpvt_change_state(cpvt, state, 0);
        }
    } else {
        switch (state) {
            case CALL_STATE_DIALING:
            case CALL_STATE_ALERTING:
                cpvt = pvt_channel_find_last_initialized(pvt);

                if (mpty) {
                    if (!CONF_SHARED(pvt, multiparty)) {
                        if (cpvt && !CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && mpty > 0) {
                            cpvt = NULL;
                        }
                    }
                }

                if (cpvt) {
                    cpvt->call_idx = (short)call_idx;
                    process_state  = cpvt_change_state(cpvt, state, 0);
                } else {
                    at_enqueue_hangup(&pvt->sys_chan, call_idx, AST_CAUSE_CALL_REJECTED);
                    ast_log(LOG_ERROR, "[%s] Answered unexisting or multiparty incoming call - idx:%d, hanging up!\n", PVT_ID(pvt), call_idx);
                    process_state = 0;
                }
                break;
        }
    }

    if (!process_state) {
        return;
    }

    if (cpvt || state == CALL_STATE_INCOMING) {
        ast_debug(3, "[%s] CLCC idx:%u dir:%u state:%u mode:%u mpty:%s number:%s type:%u\n", PVT_ID(pvt), call_idx, dir, state, mode, dc_3stbool2str(mpty),
                  number, type);
    } else {
        ast_log(LOG_WARNING, "[%s] CLCC (not found) idx:%u dir:%u state:%u mode:%u mpty:%s number:%s type:%u\n", PVT_ID(pvt), call_idx, dir, state, mode,
                dc_3stbool2str(mpty), number, type);
    }

    switch (state) {
        case CALL_STATE_ACTIVE:
            if (cpvt && pvt->is_simcom && CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE && pvt->has_voice) {
                at_enqueue_cpcmreg(cpvt, 1);
            }
            break;

        case CALL_STATE_DIALING: {
            pvt->dialing  = 1;
            pvt->cwaiting = 0;
            pvt->ring     = 0;
            break;
        }

        case CALL_STATE_ALERTING: {
            pvt->dialing  = 1;
            pvt->cwaiting = 0;
            pvt->ring     = 0;

            ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, calls_answered[CPVT_DIRECTION(cpvt)]), 1);
            if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_CONFERENCE)) {
                at_enqueue_conference(cpvt);
            }
            break;
        }

        case CALL_STATE_INCOMING: {
            pvt->ring     = 1;
            pvt->dialing  = 0;
            pvt->cwaiting = 0;

            ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, in_calls), 1);

            if (pvt_enabled(pvt)) {
                /* TODO: give dialplan level user tool for checking device is voice enabled or not  */
                if (start_pbx(pvt, number, call_idx, state)) {
                    ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, in_pbx_fails), 1);
                } else {
                    ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, in_calls_handled), 1);
                    if (!pvt->has_voice) {
                        ast_log(LOG_WARNING, "[%s] pbx started for device not voice capable\n", PVT_ID(pvt));
                    }
                }
            }
            break;
        }

        case CALL_STATE_WAITING: {
            pvt->cwaiting = 1;
            pvt->ring     = 0;
            pvt->dialing  = 0;

            ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, cw_calls), 1);

            if (dir == CALL_DIR_INCOMING) {
                if (pvt_enabled(pvt)) {
                    /* TODO: give dialplan level user tool for checking device is voice enabled or not  */
                    if (start_pbx(pvt, number, call_idx, state) == 0) {
                        ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, in_calls_handled), 1);
                        if (!pvt->has_voice) {
                            ast_log(LOG_WARNING, "[%s] pbx started for device not voice capable\n", PVT_ID(pvt));
                        }
                    } else {
                        ast_atomic_fetchadd_uint32(&PVT_STAT(pvt, in_pbx_fails), 1);
                    }
                }
            }
            break;
        }

        case CALL_STATE_RELEASED: {  // CALL END
            pvt->ring     = 0;
            pvt->dialing  = 0;
            pvt->cwaiting = 0;

            if (cpvt && pvt->is_simcom && CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE && pvt->has_voice) {
                at_enqueue_cpcmreg(&pvt->sys_chan, 0);
            }
            break;
        }

        default:
            ast_log(LOG_WARNING, "[%s] Unhandled call state event - idx:%u call_state:%s-%u)\n", PVT_ID(pvt), call_idx, call_state2str((call_state_t)state),
                    state);
            break;
    }
}

static void current_line(const char* const str, struct ast_str** line)
{
    const char* const p = strchr(str, '\r');
    if (p) {
        ast_str_set_substr(line, 0, str, p - str);
    } else {
        ast_str_set(line, 0, "%s", str);
    }
}

static const char* next_line(const char* const str)
{
    const char* p = strchr(str, '\r');
    if (p) {
        ++p;
        if (p[0] == '\n') {
            ++p;
        }
        if (p[0]) {
            return p;
        }
    }
    return NULL;
}

/*!
 * \brief Handle +CLCC response
 * \param pvt -- pvt structure
 * \param result -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_clcc(struct pvt* const pvt, const struct ast_str* const response)
{
    static const ssize_t LINE_DEF_LEN = 128;

    if (!pvt->initialized) {
        return 0;
    }

    struct cpvt* cpvt;
    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        CPVT_RESET_FLAG(cpvt, CALL_FLAG_ALIVE);
    }

    RAII_VAR(struct ast_str*, line, ast_str_create(LINE_DEF_LEN), ast_free);
    for (const char* str = ast_str_buffer(response); str; str = next_line(str)) {
        current_line(str, &line);

        unsigned call_idx, dir, state, mode, mpty, type;
        char* number;

        if (at_parse_clcc(ast_str_buffer(line), &call_idx, &dir, &state, &mode, &mpty, &number, &type)) {
            ast_log(LOG_ERROR, "[%s] CLCC - can't parse line '%s'\n", PVT_ID(pvt), ast_str_buffer(line));
            continue;
        }

        if (mode != CLCC_CALL_TYPE_VOICE) {
            ast_debug(4, "[%s] CLCC - non-voice call, idx:%u dir:%u state:%u nubmer:%s\n", PVT_ID(pvt), call_idx, dir, state, number);
            continue;
        }

        if (mode > CALL_STATE_WAITING) {
            ast_debug(4, "[%s] CLCC - invalid call state, idx:%u dir:%u state:%u nubmer:%s\n", PVT_ID(pvt), call_idx, dir, state, number);
            continue;
        }

        handle_clcc(pvt, call_idx, dir, state, mode, mpty ? TRIBOOL_TRUE : TRIBOOL_FALSE, number, type);
    }

    return 0;
}

static unsigned int map_dsci(const unsigned int dsci)
{
    switch (dsci) {
        case 3u:  // connect
            return CALL_STATE_ACTIVE;

        case 7u:  // allerting
            return CALL_STATE_ALERTING;

        default:
            return dsci;
    }
}

/*!
 * \brief Handle ^DSCI response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */
static int at_response_dsci(struct pvt* const pvt, const struct ast_str* const response)
{
    unsigned int call_index, call_dir, call_state, call_type, number_type;
    char* number;

    if (at_parse_dsci(ast_str_buffer(response), &call_index, &call_dir, &call_state, &call_type, &number, &number_type)) {
        ast_log(LOG_ERROR, "[%s] Fail to parse DSCI '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return 0;
    }

    if (call_type != CLCC_CALL_TYPE_VOICE) {
        ast_debug(4, "[%s] Non-voice DSCI - idx:%u dir:%d type:%u state:%u number:%s\n", PVT_ID(pvt), call_index, call_dir, call_type, call_state, number);
        return 0;
    }

    ast_debug(3, "[%s] DSCI - idx:%u dir:%u type:%u state:%u number:%s\n", PVT_ID(pvt), call_index, call_dir, call_type, call_state, number);

    switch (call_state) {
        case CALL_STATE_RELEASED:  // released call will not be listed by AT+CLCC command, handle directly
            handle_clcc(pvt, call_index, call_dir, map_dsci(call_state), call_type, TRIBOOL_NONE, number, number_type);
            break;

        default:  // request CLCC anyway
            if (CONF_SHARED(pvt, multiparty)) {
                request_clcc(pvt);
            } else {
                handle_clcc(pvt, call_index, call_dir, map_dsci(call_state), call_type, TRIBOOL_NONE, number, number_type);
            }
            break;
    }

    return 0;
}

/*!
 * \brief Handle +QIND response.
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_qind(struct pvt* const pvt, const struct ast_str* const response)
{
    qind_t qind;
    char* params;

    const int res = at_parse_qind(ast_str_buffer(response), &qind, &params);
    if (res < 0) {
        return -1;
    }

    ast_debug(4, "[%s] QIND(%s) - %s\n", PVT_ID(pvt), at_qind2str(qind), params);

    switch (qind) {
        case QIND_CSQ: {
            int rssi;

            const int res = at_parse_qind_csq(params, &rssi);
            if (res < 0) {
                ast_debug(3, "[%s] Failed to parse CSQ - %s\n", PVT_ID(pvt), params);
                break;
            }
            pvt->rssi = rssi;
            RAII_VAR(struct ast_str*, rssi_str, rssi2dBm(rssi), ast_free);
            ast_verb(3, "[%s] RSSI: %s\n", PVT_ID(pvt), ast_str_buffer(rssi_str));
            return 0;
        }

        case QIND_ACT: {
            int act;
            const int res = at_parse_qind_act(params, &act);
            if (res < 0) {
                ast_debug(3, "[%s] Failed to parse ACT - %s\n", PVT_ID(pvt), params);
                break;
            }
            ast_verb(1, "[%s] Access technology: %s\n", PVT_ID(pvt), sys_act2str(act));
            pvt_set_act(pvt, act);

            if (act) {
                if (pvt->is_simcom) {
                    if (at_enqueue_cspn_cops(&pvt->sys_chan)) {
                        ast_log(LOG_WARNING, "[%s] Error sending query for provider name\n", PVT_ID(pvt));
                    }
                } else {
                    if (at_enqueue_qspn_qnwinfo(&pvt->sys_chan)) {
                        ast_log(LOG_WARNING, "[%s] Error sending query for provider name\n", PVT_ID(pvt));
                    }
                }
            }
            return 0;
        }

        case QIND_CCINFO: {
            unsigned call_idx, dir, state, mode, mpty, toa;
            char* number;

            const int res = at_parse_qind_cc(params, &call_idx, &dir, &state, &mode, &mpty, &number, &toa);
            if (res < 0) {
                ast_log(LOG_ERROR, "[%s] Fail to parse CCINFO - %s\n", PVT_ID(pvt), params);
                break;
            }
            handle_clcc(pvt, call_idx, dir, state, mode, mpty ? TRIBOOL_TRUE : TRIBOOL_FALSE, number, toa);
            return 0;
        }

        case QIND_NONE:
            return 0;
    }

    return -1;
}

/*!
 * \brief Handle +CSCA response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \param len -- string lenght
 * \retval  0 success
 * \retval -1 error
 */
static int at_response_csca(struct pvt* const pvt, const struct ast_str* const response)
{
    char* csca;

    if (at_parse_csca(ast_str_buffer(response), &csca)) {
        ast_debug(1, "[%s] Could not parse CSCA response '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (ast_strlen_zero(csca)) {
        ast_string_field_set(pvt, sms_scenter, NULL);
        ast_verb(1, "[%s] No SMS service centre\n", PVT_ID(pvt));
        return 0;
    }

    char utf8_str[40];
    if (from_ucs2(csca, utf8_str, STRLEN(utf8_str))) {
        ast_debug(1, "[%s] Could not decode CSCA: %s", PVT_ID(pvt), csca);
        return -1;
    }

    ast_string_field_set(pvt, sms_scenter, utf8_str);
    ast_verb(2, "[%s] SMS service centre: %s\n", PVT_ID(pvt), pvt->sms_scenter);
    return 0;
}

/*!
 * \brief Handle +CCWA response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_ccwa(struct pvt* const pvt, const struct ast_str* const response)
{
    unsigned int status, class;
    ccwa_variant_t ccwa_variant;

    /*
     * CCWA may be in form:
     *	in response of AT+CCWA=?
     *		+CCWA: (0,1)
     *	in response of AT+CCWA=?
     *		+CCWA: <n>
     *	in response of "AT+CCWA=[<n>[,<mode>[,<class>]]]"
     *		+CCWA: <status>,<class1>
     *	unsolicited result code
     *		+CCWA: <number>,<type>,<class>,[<alpha>][,<CLI validity>[,<subaddr>,<satype>[,<priority>]]]
     *
     */

    if (at_parse_ccwa(ast_str_buffer(response), &ccwa_variant, &status, &class)) {
        ast_log(LOG_ERROR, "[%s] Can't parse CCWA line '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_debug(4, "[%s] Call waiting - variant:%d status:%u class:%u\n", PVT_ID(pvt), ccwa_variant, status, class);

    switch (ccwa_variant) {
        case CCWA_VARIANT_PRESENTATION_FLAG:
            ast_verb(2, "[%s] Call waiting notifications %s\n", PVT_ID(pvt), status ? "enabled" : "disabled");
            break;

        case CCWA_VARIANT_STATUS_AND_CLASS:
            if (!(class & CCWA_CLASS_VOICE)) {
                ast_log(LOG_NOTICE, "[%s] Call waiting - non-voice call, ignoring\n", PVT_ID(pvt));
                return 0;
            }
            if (!(status == CCWA_STATUS_NOT_ACTIVE || status == CCWA_STATUS_ACTIVE)) {
                ast_log(LOG_WARNING, "[%s] Call waiting - wrong status - %u, ignoring\n", PVT_ID(pvt), status);
                return 0;
            }

            pvt->has_call_waiting = status == CCWA_STATUS_ACTIVE ? 1u : 0u;
            ast_verb(1, "[%s] Call waiting is %s\n", PVT_ID(pvt), pvt->has_call_waiting ? "enabled" : "disabled");
            break;

        case CCWA_VARIANT_URC:
            if (!pvt->initialized) {
                ast_log(LOG_WARNING, "[%s] Call waiting - channel uninitialized, ignoring\n", PVT_ID(pvt));
                return 0;
            }
            if (!(class & CCWA_CLASS_VOICE)) {
                ast_log(LOG_NOTICE, "[%s] Call waiting - non-voice call, ignoring\n", PVT_ID(pvt));
                return 0;
            }
            if (!pvt->has_call_waiting) {
                ast_log(LOG_NOTICE, "[%s] Call wating disabled, ignoring\n", PVT_ID(pvt));
                return 0;
            }

            pvt->cwaiting = 1;
            request_clcc(pvt);
            break;
    }

    return 0;
}

static int at_response_cmgs(struct pvt* const pvt, const struct ast_str* const response, const at_queue_task_t* const task)
{
    const int refid = at_parse_cmgs(ast_str_buffer(response));
    if (refid < 0) {
        ast_verb(1, "[%s] Error parsing CMGS: [%s]\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    const int partcnt = task->cmdsno / 2;
    const int partno  = 1 + (task->cindex / 2);

    if (partno < partcnt) {
        ast_debug(3, "[%s][SMS:%d REF:%d] Successfully sent message part %d/%d\n", PVT_ID(pvt), task->uid, refid, partno, partcnt);
    } else {
        if (partcnt <= 1) {
            ast_verb(1, "[%s][SMS:%d REF:%d] Successfully sent message\n", PVT_ID(pvt), task->uid, refid);
        } else {
            ast_verb(1, "[%s][SMS:%d] Successfully sent message [%d parts]\n", PVT_ID(pvt), task->uid, partcnt);
        }

        pvt->outgoing_sms = 0;
        pvt_try_restate(pvt);
    }

    RAII_VAR(struct ast_str*, dst, (partno == partcnt) ? ast_str_create(DST_DEF_LEN) : NULL, ast_free);
    RAII_VAR(struct ast_str*, msg, (partno == partcnt) ? ast_str_create(DST_DEF_LEN) : NULL, ast_free);

    if (!smsdb_outgoing_part_put(task->uid, refid, &dst, &msg)) {
        ast_verb(3, "[%s][SMS:%d %s] SMS: [%s]\n", PVT_ID(pvt), task->uid, ast_str_buffer(dst), ast_str_buffer(msg));
        RAII_VAR(struct ast_json*, report, ast_json_object_create(), ast_json_unref);
        ast_json_object_set(report, "info", ast_json_string_create("Message send"));
        ast_json_object_set(report, "uid", ast_json_integer_create(task->uid));
        ast_json_object_set(report, "refid", ast_json_integer_create(refid));
        AST_JSON_OBJECT_SET(report, msg);
        if (partcnt > 1) {
            ast_json_object_set(report, "parts", ast_json_integer_create(partcnt));
        }
        channel_start_local_report(pvt, "sms", LOCAL_REPORT_DIRECTION_OUTGOING, ast_str_buffer(dst), NULL, NULL, 1, report);
    }
    return 0;
}

static int at_response_cmgs_error(struct pvt* const pvt, const at_queue_task_t* const task)
{
    RAII_VAR(struct ast_str*, dst, ast_str_create(DST_DEF_LEN), ast_free);
    RAII_VAR(struct ast_str*, msg, ast_str_create(DST_DEF_LEN), ast_free);

    if (!smsdb_outgoing_clear(task->uid, &dst, &msg)) {
        ast_verb(1, "[%s][SMS:%d] Error sending message: [%s]\n", PVT_ID(pvt), task->uid, ast_str_buffer(dst));
        RAII_VAR(struct ast_json*, report, ast_json_object_create(), ast_json_unref);
        ast_json_object_set(report, "info", ast_json_string_create("Error sending message"));
        ast_json_object_set(report, "uid", ast_json_integer_create(task->uid));
        AST_JSON_OBJECT_SET(report, msg);
        channel_start_local_report(pvt, "sms", LOCAL_REPORT_DIRECTION_OUTGOING, ast_str_buffer(dst), NULL, NULL, 0, report);
    } else {
        ast_verb(1, "[%s][SMS:%d] Error sending message\n", PVT_ID(pvt), task->uid);
    }
    pvt->outgoing_sms = 0;
    return 0;
}

/*!
 * \brief Handle +CMTI response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cmti(struct pvt* const pvt, const struct ast_str* const response)
{
    int idx;

    if (at_parse_cmti(ast_str_buffer(response), &idx)) {
        ast_log(LOG_WARNING, "[%s] Error parsing incoming message alert '%s', ignoring\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (idx < 0) {
        ast_debug(2, "[%s][SMS:%d] Negative message index, ignoring\n", PVT_ID(pvt), idx);
        return 0;
    }

    ast_debug(1, "[%s][SMS:%d] New message\n", PVT_ID(pvt), idx);

    if (at_enqueue_retrieve_sms(&pvt->sys_chan, idx, RES_CMTI)) {
        ast_log(LOG_ERROR, "[%s][SMS:%d] Could not read message\n", PVT_ID(pvt), idx);
        return -1;
    }

    return 0;
}

/*!
 * \brief Handle +CDSI response
 * \param pvt -- pvt structure
 * \param response -- string containing response (null terminated)
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cdsi(struct pvt* const pvt, const struct ast_str* const response)
{
    int idx;

    if (at_parse_cdsi(ast_str_buffer(response), &idx)) {
        ast_log(LOG_WARNING, "[%s] Error parsing incoming message alert '%s', ignoring\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (idx < 0) {
        /* Not sure why this happens, but we don't want to disconnect standing calls.
         * [Jun 14 19:57:57] ERROR[3056]: at_response.c:1173 at_response_cmti:
         *   [m1-1] Error parsing incoming sms message alert '+CMTI: "SM",-1' */
        ast_debug(2, "[%s][SMS:%d] Negative message index, ignoring\n", PVT_ID(pvt), idx);
        return 0;
    }

    ast_debug(1, "[%s][SMS:%d] Message status report\n", PVT_ID(pvt), idx);

    if (at_enqueue_retrieve_sms(&pvt->sys_chan, idx, RES_CDSI)) {
        ast_log(LOG_ERROR, "[%s][SMS:%d] Could not read message\n", PVT_ID(pvt), idx);
        return -1;
    }

    return 0;
}

/*!
 * \brief Handle +CMGR response
 * \param pvt -- pvt structure
 * \param str -- string containing response (null terminated)
 * \param len -- string lenght
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_msg(struct pvt* const pvt, const struct ast_str* const response, at_res_t cmd)
{
    static const ssize_t MSG_DEF_LEN = 64;
    static const ssize_t MSG_MAX_LEN = 4096;

    struct ast_tm scts, dt;
    int mr, st;
    int res;
    int tpdu_type, idx;
    pdu_udh_t udh;
    tristate_bool_t msg_ack = TRIBOOL_NONE;
    int msg_ack_uid         = 0;
    int msg_complete        = 0;

    pdu_udh_init(&udh);

    memset(&scts, 0, sizeof(scts));
    memset(&dt, 0, sizeof(dt));

    RAII_VAR(struct ast_str*, msg, ast_str_create(MSG_MAX_LEN), ast_free);
    RAII_VAR(struct ast_str*, oa, ast_str_create(512), ast_free);
    RAII_VAR(struct ast_str*, sca, ast_str_create(512), ast_free);
    size_t msg_len = ast_str_size(msg);

    switch (cmd) {
        case RES_CMGR:
        case RES_CLASS0:
            res = at_parse_cmgr(ast_str_buffer(response), ast_str_strlen(response), &tpdu_type, ast_str_buffer(sca), ast_str_size(sca), ast_str_buffer(oa),
                                ast_str_size(oa), &scts, &mr, &st, &dt, ast_str_buffer(msg), &msg_len, &udh);
            break;

        case RES_CMGL:
            res = at_parse_cmgl(ast_str_buffer(response), ast_str_strlen(response), &idx, &tpdu_type, ast_str_buffer(sca), ast_str_size(sca),
                                ast_str_buffer(oa), ast_str_size(oa), &scts, &mr, &st, &dt, ast_str_buffer(msg), &msg_len, &udh);
            break;

        case RES_CMT:
            res = at_parse_cmt(ast_str_buffer(response), ast_str_strlen(response), &tpdu_type, ast_str_buffer(sca), ast_str_size(sca), ast_str_buffer(oa),
                               ast_str_size(oa), &scts, &mr, &st, &dt, ast_str_buffer(msg), &msg_len, &udh);
            break;

        case RES_CBM:
            res = at_parse_cbm(ast_str_buffer(response), ast_str_strlen(response), &tpdu_type, ast_str_buffer(sca), ast_str_size(sca), ast_str_buffer(oa),
                               ast_str_size(oa), &scts, &mr, &st, &dt, ast_str_buffer(msg), &msg_len, &udh);
            break;

        case RES_CDS:
            res = at_parse_cds(ast_str_buffer(response), ast_str_strlen(response), &tpdu_type, ast_str_buffer(sca), ast_str_size(sca), ast_str_buffer(oa),
                               ast_str_size(oa), &scts, &mr, &st, &dt, ast_str_buffer(msg), &msg_len, &udh);
            break;

        default:
            res = -1;
            break;
    }

    if (res < 0) {
        ast_log(LOG_WARNING, "[%s] Error parsing incoming message: %s\n", PVT_ID(pvt), error2str(chan_quectel_err));
        msg_ack = TRIBOOL_FALSE;
        goto msg_done_ack;
    }

    ast_str_update(msg);
    switch (PDUTYPE_MTI(tpdu_type)) {
        case PDUTYPE_MTI_SMS_STATUS_REPORT: {
            ast_verb(1, "[%s][SMS:%d] Got status report from %s and status code %d\n", PVT_ID(pvt), mr, ast_str_buffer(oa), st);

            RAII_VAR(int*, status_report, ast_calloc(sizeof(int), 256), ast_free);
            if (!smsdb_outgoing_part_status(pvt->imsi, ast_str_buffer(oa), mr, st, status_report)) {
                RAII_VAR(struct ast_json*, report, ast_json_object_create(), ast_json_unref);
                ast_json_object_set(report, "info", ast_json_stringf("SMS Status"));
                if (st) {
                    ast_json_object_set(report, "status_code", ast_json_integer_create(st));
                }
                ast_json_object_set(report, "uid", ast_json_integer_create(mr));
                struct ast_json* statuses = ast_json_array_create();
                int success               = 1;
                for (int i = 0; status_report[i] != -1; ++i) {
                    success &= !(status_report[i] & 0x40);
                    ast_json_array_append(statuses, ast_json_stringf("%03d,", status_report[i]));
                }
                ast_json_object_set(report, "status", statuses);

                ast_verb(2, "[%s][SMS:%d] Report: success:%d\n", PVT_ID(pvt), mr, success);
                msg_ack      = TRIBOOL_TRUE;
                msg_ack_uid  = mr;
                msg_complete = 1;
                channel_start_local_report(pvt, "sms", LOCAL_REPORT_DIRECTION_INCOMING, ast_str_buffer(oa), &scts, &dt, success, report);
            }

            break;
        }

        case PDUTYPE_MTI_SMS_DELIVER: {
            RAII_VAR(struct ast_str*, fullmsg, ast_str_create(MSG_DEF_LEN), ast_free);
            struct ast_str* scts_str = ast_str_alloca(AST_TM_MAX_LEN);
            format_ast_tm(&scts, scts_str);

            if (udh.parts > 1) {
                ast_verb(2, "[%s][SMS:%d PART:%d/%d TS:%s] Got message part from %s: [%s]\n", PVT_ID(pvt), (int)udh.ref, (int)udh.order, (int)udh.parts,
                         ast_str_buffer(scts_str), ast_str_buffer(oa), ast_str_buffer(msg));
                int csms_cnt = smsdb_put(pvt->imsi, ast_str_buffer(oa), udh.ref, udh.parts, udh.order, ast_str_buffer(msg), &fullmsg);
                if (csms_cnt <= 0) {
                    ast_log(LOG_ERROR, "[%s][SMS:%d PART:%d/%d TS:%s] Error putting message part to database\n", PVT_ID(pvt), (int)udh.ref, (int)udh.order,
                            (int)udh.parts, ast_str_buffer(scts_str));
                    goto receive_as_is;
                }
                ast_str_update(fullmsg);
                msg_ack      = TRIBOOL_TRUE;
                msg_ack_uid  = (int)udh.ref;
                msg_complete = (csms_cnt < (int)udh.parts) ? 0 : 1;

                if (!msg_complete) {
                    if ((int)udh.order == (int)udh.parts) {
                        ast_debug(1, "[%s][SMS:%d PART:%d/%d TS:%s] Incomplete message, got %d parts\n", PVT_ID(pvt), (int)udh.ref, (int)udh.order,
                                  (int)udh.parts, ast_str_buffer(scts_str), csms_cnt);
                    }
                    goto msg_done;
                }
            } else {
receive_as_is:
                msg_ack      = TRIBOOL_TRUE;
                msg_ack_uid  = (int)udh.ref;
                msg_complete = 1;
                ast_verb(2, "[%s][SMS:%d TS:%s] Got message part from %s: [%s]\n", PVT_ID(pvt), (int)udh.ref, ast_str_buffer(scts_str), ast_str_buffer(oa),
                         ast_str_buffer(msg));
                ast_str_copy_string(&fullmsg, msg);
            }

            RAII_VAR(struct ast_json*, sms, ast_json_object_create(), ast_json_unref);
            ast_json_object_set(sms, "ts", ast_json_string_create(ast_str_buffer(scts_str)));
            if (udh.ref) {
                ast_json_object_set(sms, "ref", ast_json_integer_create((int)udh.ref));
            }
            if (udh.parts) {
                ast_json_object_set(sms, "parts", ast_json_integer_create((int)udh.parts));
            }
            ast_json_object_set(sms, "from", ast_json_string_create(ast_str_buffer(oa)));

            if (ast_str_strlen(fullmsg)) {
                ast_verb(1, "[%s][SMS:%d PARTS:%d TS:%s] Got message from %s: [%s]\n", PVT_ID(pvt), (int)udh.ref, (int)udh.parts, ast_str_buffer(scts_str),
                         ast_str_buffer(oa), ast_str_buffer(fullmsg));

                ast_json_object_set(sms, "msg", ast_json_string_create(ast_str_buffer(fullmsg)));
            } else {
                ast_verb(1, "[%s][SMS:%d PARTS:%d TS:%s] Got empty message from %s\n", PVT_ID(pvt), (int)udh.ref, (int)udh.parts, ast_str_buffer(scts_str),
                         ast_str_buffer(oa));
            }

            channel_start_local_json(pvt, "sms", ast_str_buffer(oa), "SMS", sms);
            break;
        }
    }

msg_done:

    if (CONF_SHARED(pvt, sms_autodelete) && msg_complete) {
        switch (cmd) {
            case RES_CMGL:
                at_enqueue_delete_sms(&pvt->sys_chan, idx, TRIBOOL_NONE);
                goto msg_ret;

            default:
                break;
        }
    }

msg_done_ack:
    switch (cmd) {
        case RES_CMGR:
            at_sms_retrieved(&pvt->sys_chan, 0);
            break;

        case RES_CMT:
        case RES_CDS:
            if (CONF_SHARED(pvt, msg_service) > 0) {
                switch (msg_ack) {
                    case TRIBOOL_FALSE:  // negative ACT
                        at_enqueue_msg_ack_n(&pvt->sys_chan, 0, msg_ack_uid);
                        break;

                    case TRIBOOL_TRUE:  // positive ACK
                        at_enqueue_msg_ack_n(&pvt->sys_chan, 0, msg_ack_uid);
                        break;

                    default:
                        break;
                }
            }
            break;

        case RES_CLASS0:
            if (msg_ack) {
                at_enqueue_msg_ack(&pvt->sys_chan);
            }
            break;

        default:
            break;
    }

msg_ret:

    return 0;
}

/*!
 * \brief Send an SMS message from the queue.
 * \param pvt -- pvt structure
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_sms_prompt(struct pvt* const pvt, const at_queue_task_t* const task)
{
    const struct at_queue_cmd* const ecmd = at_queue_head_cmd(pvt);

    if (!ecmd) {
        ast_log(LOG_ERROR, "[%s] Received unexpected SMS PROMPT\n", PVT_ID(pvt));
        return 0;
    }

    switch (ecmd->cmd) {
        case CMD_AT_CNMA:
            switch (ecmd->res) {
                case RES_OK:
                    // SIM800C workaround
                    at_enqueue_escape(safe_get_cpvt(task, pvt), safe_task_uid(task));
                    at_queue_handle_result(pvt, ecmd->res);
                    break;

                case RES_SMS_PROMPT:
                    at_queue_handle_result(pvt, ecmd->res);
                    break;

                default:
                    ast_log(LOG_ERROR, "[%s] Received SMS prompt when expecting '%s' response to '%s', ignoring\n", PVT_ID(pvt), at_res2str(ecmd->res),
                            at_cmd2str(ecmd->cmd));
                    break;
            }
            break;

        default:
            if (ecmd->res == RES_SMS_PROMPT) {
                at_queue_handle_result(pvt, RES_SMS_PROMPT);
            } else {
                ast_log(LOG_ERROR, "[%s] Received SMS prompt when expecting '%s' response to '%s', ignoring\n", PVT_ID(pvt), at_res2str(ecmd->res),
                        at_cmd2str(ecmd->cmd));
            }
            break;
    }

    return 0;
}

/*!
 * \brief Handle CUSD response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cusd(struct pvt* const pvt, const struct ast_str* const response, int gsm7)
{
    static const char* const types[] = {
        "USSD Notify", "USSD Request", "USSD Terminated by network", "Other local client has responded", "Operation not supported", "Network time out",
    };
    static const size_t USSD_DEF_LEN = 4096;

    int type;
    char* cusd;
    int dcs;

    if (at_parse_cusd(ast_str_buffer(response), &type, &cusd, &dcs)) {
        ast_verb(1, "[%s] Error parsing CUSD: [%s]\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (type < 0 || type >= (int)ARRAY_LEN(types)) {
        ast_log(LOG_WARNING, "[%s] Unknown CUSD type: %d\n", PVT_ID(pvt), type);
    }

    const char* typedesc = enum2str(type, types, ARRAY_LEN(types));
    RAII_VAR(struct ast_str*, cusd_str, NULL, ast_free);

    if (dcs >= 0) {
        // sanitize DCS
        if (!gsm7) {
            dcs = 2;
        } else if (dcs & 0x40) {
            dcs = (dcs & 0xc) >> 2;
            if (dcs == 3) {
                dcs = 0;
            }
        } else {
            dcs = 0;
        }

        ssize_t res;

        switch (dcs) {
            case 0: {  // GSM-7
                static const size_t OUT_UCS2_BUF_SIZE = sizeof(uint16_t) * USSD_DEF_LEN;

                RAII_VAR(uint16_t*, out_ucs2, ast_calloc(sizeof(uint16_t), USSD_DEF_LEN), ast_free);
                const int cusd_nibbles = unhex(cusd, (uint8_t*)cusd);
                res                    = gsm7_unpack_decode(cusd, cusd_nibbles, out_ucs2, OUT_UCS2_BUF_SIZE / sizeof(uint16_t), 0, 0, 0);
                if (res > 0) {
                    const size_t res_buf_size = (res * 4) + 1u;
                    cusd_str                  = ast_str_create(res_buf_size);
                    res                       = ucs2_to_utf8(out_ucs2, res, ast_str_buffer(cusd_str), ast_str_size(cusd_str));
                }
                break;
            };

            case 1: {  // ASCII
                res                       = strlen(cusd);
                const size_t res_buf_size = res + 1u;
                cusd_str                  = ast_str_create(res_buf_size);

                ast_str_set_substr(&cusd_str, 0, cusd, res);
                break;
            };

            case 2: {  // UCS-2
                const int cusd_nibbles    = unhex(cusd, (uint8_t*)cusd);
                const size_t res_buf_size = ((cusd_nibbles + 1) * 4) + 1u;
                cusd_str                  = ast_str_create(res_buf_size);
                res                       = ucs2_to_utf8((const uint16_t*)cusd, (cusd_nibbles + 1) / 4, ast_str_buffer(cusd_str), ast_str_size(cusd_str));
                break;
            }

            default:
                res = -1;
                break;
        }

        if (res < 0) {
            return -1;
        }

        ast_str_truncate(cusd_str, res);

        ast_verb(1, "[%s] Got USSD type %d '%s': [%s]\n", PVT_ID(pvt), type, typedesc, ast_str_buffer(cusd_str));
    } else {
        ast_verb(1, "[%s] Got USSD type %d '%s'\n", PVT_ID(pvt), type, typedesc);
    }

    RAII_VAR(struct ast_json*, ussd, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(ussd, "type", ast_json_integer_create(type));
    ast_json_object_set(ussd, "type_description", ast_json_string_create(typedesc));
    if (cusd_str) {
        ast_json_object_set(ussd, "ussd", ast_json_string_create(ast_str_buffer(cusd_str)));
    }

    channel_start_local_json(pvt, "ussd", "ussd", "USSD", ussd);
    return 0;
}

/*!
 * \brief Handle +CPIN response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cpin(struct pvt* const pvt, const struct ast_str* const response)
{
    const int rv = at_parse_cpin(ast_str_buffer(response), ast_str_strlen(response));
    switch (rv) {
        case -1:
            ast_log(LOG_ERROR, "[%s] Error parsing +CPIN message: %s\n", PVT_ID(pvt), ast_str_buffer(response));
            break;
        case 1:
            ast_log(LOG_ERROR, "[%s] Needs PIN code!\n", PVT_ID(pvt));
            break;
        case 2:
            ast_log(LOG_ERROR, "[%s] Needs PUK code!\n", PVT_ID(pvt));
            break;
    }
    return rv;
}

/*!
 * \brief Handle ^SMMEMFULL response This event notifies us, that the sms storage is full
 * \param pvt -- pvt structure
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_smmemfull(struct pvt* pvt)
{
    ast_log(LOG_ERROR, "[%s] SMS storage is full\n", PVT_ID(pvt));
    return 0;
}

static int at_response_csq(struct pvt* const pvt, const struct ast_str* const response)
{
    int rssi;

    if (at_parse_csq(ast_str_buffer(response), &rssi)) {
        ast_debug(2, "[%s] Error parsing +CSQ result '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    pvt->rssi = rssi;

    RAII_VAR(struct ast_str*, rssi_str, rssi2dBm(rssi), ast_free);
    ast_verb(3, "[%s] RSSI: %s\n", PVT_ID(pvt), ast_str_buffer(rssi_str));
    return 0;
}

static int at_response_csqn(struct pvt* const pvt, const struct ast_str* const response)
{
    int rssi, ber;

    if (at_parse_csqn(ast_str_buffer(response), &rssi, &ber)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    pvt->rssi = rssi;

    RAII_VAR(struct ast_str*, rssi_str, rssi2dBm(rssi), ast_free);
    ast_verb(3, "[%s] RSSI: %s\n", PVT_ID(pvt), ast_str_buffer(rssi_str));
    ast_verb(4, "[%s] BER: %d\n", PVT_ID(pvt), ber);
    return 0;
}

/*!
 * \brief Handle +CNUM response Here we get our own phone number
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cnum(struct pvt* const pvt, const struct ast_str* const response)
{
    const char* const number = at_parse_cnum(ast_str_buffer(response));

    if (ast_strlen_zero(number)) {
        ast_string_field_set(pvt, subscriber_number, NULL);
        pvt->has_subscriber_number = 0;
        ast_debug(1, "[%s] Empty subsciber number\n", PVT_ID(pvt));
        return -1;
    }

    ast_string_field_set(pvt, subscriber_number, number);
    ast_verb(2, "[%s] Subsciber number: %s\n", PVT_ID(pvt), pvt->subscriber_number);
    pvt->has_subscriber_number = 1;
    return 0;
}

/*!
 * \brief Handle +COPS response Here we get the GSM provider name
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cops(struct pvt* const pvt, const struct ast_str* const response)
{
    char* network_name;
    int act;

    if (at_parse_cops(ast_str_buffer(response), &network_name, &act)) {
        return -1;
    }

    if (ast_strlen_zero(network_name)) {
        ast_string_field_set(pvt, network_name, "NONE");
        ast_verb(2, "[%s] Operator: %s\n", PVT_ID(pvt), pvt->network_name);
    } else {
        ast_string_field_set(pvt, network_name, network_name);
        ast_verb(1, "[%s] Operator: %s\n", PVT_ID(pvt), pvt->network_name);
    }

    if (act >= 0) {
        const int mact = map_creg_act(act);
        if (mact >= 0) {
            ast_verb(1, "[%s] Access technology: %s [%d]\n", PVT_ID(pvt), sys_act2str(mact), act);
            pvt_set_act(pvt, mact);
        }
    }

    return 0;
}

static int at_response_qspn(struct pvt* const pvt, const struct ast_str* const response)
{
    char* fnn;  // full network name
    char* snn;  // short network name
    char* spn;  // service provider name

    if (at_parse_qspn(ast_str_buffer(response), &fnn, &snn, &spn)) {
        ast_log(LOG_ERROR, "[%s] Error parsing QSPN response - '%s'", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_verb(1, "[%s] Operator: %s/%s/%s\n", PVT_ID(pvt), fnn, snn, spn);

    ast_string_field_set(pvt, network_name, fnn);
    ast_string_field_set(pvt, short_network_name, snn);
    ast_string_field_set(pvt, provider_name, spn);
    return 0;
}

static int at_response_cspn(struct pvt* const pvt, const struct ast_str* const response)
{
    char* spn;  // service provider name

    if (at_parse_cspn(ast_str_buffer(response), &spn)) {
        ast_log(LOG_ERROR, "[%s] Error parsing CSPN response - '%s'", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_string_field_set(pvt, provider_name, spn);
    ast_verb(1, "[%s] Service provider: %s\n", PVT_ID(pvt), pvt->provider_name);
    return 0;
}

static int at_response_qnwinfo(struct pvt* const pvt, const struct ast_str* const response)
{
    int act;      // access technology
    int oper;     // operator in numeric format
    char* band;   // selected band
    int channel;  // channel ID

    if (at_parse_qnwinfo(ast_str_buffer(response), &act, &oper, &band, &channel)) {
        ast_log(LOG_WARNING, "[%s] Error parsing QNWINFO response - '%s'", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (act < 0) {
        return 0;
    }

    pvt_set_act(pvt, act);
    pvt->operator= oper;
    ast_string_field_set(pvt, band, band);

    ast_verb(2, "[%s] Registered PLMN: %d\n", PVT_ID(pvt), oper);
    ast_verb(2, "[%s] Band: %s\n", PVT_ID(pvt), band);
    return 0;
}

/*!
 * \brief Handle +CREG response Here we get the GSM registration status
 * \param pvt -- pvt structure
 * \param str -- string containing response (null terminated)
 * \param len -- string lenght
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_creg(struct pvt* const pvt, int cereg, const struct ast_str* const response)
{
    int reg_status;
    char* lac;
    char* ci;
    int act;

    if (at_parse_creg(ast_str_buffer(response), cereg, &reg_status, &lac, &ci, &act)) {
        ast_log(LOG_ERROR, "[%s] Error parsing %s: '%s'\n", PVT_ID(pvt), S_COR(cereg, "CEREG", "CREG"), ast_str_buffer(response));
        return 0;
    }

    if (gsm_is_registered(reg_status)) {
        int (*const query_provider_name_fn)(struct cpvt*) = pvt->is_simcom ? &at_enqueue_cspn_cops : &at_enqueue_qspn_qnwinfo;
        if ((*query_provider_name_fn)(&pvt->sys_chan)) {
            ast_log(LOG_WARNING, "[%s] Error sending query for provider name\n", PVT_ID(pvt));
        }

        // #ifdef ISSUE_CCWA_STATUS_CHECK
        /* only if gsm_registered 0 -> 1 ? */
        if (!pvt->gsm_registered && CONF_SHARED(pvt, call_waiting) != CALL_WAITING_AUTO) {
            if (at_enqueue_set_ccwa(&pvt->sys_chan, CONF_SHARED(pvt, call_waiting))) {
                ast_log(LOG_WARNING, "[%s] Error setting call waiting mode\n", PVT_ID(pvt));
            }
        }
        // #endif

        pvt->gsm_registered = 1;
        pvt->gsm_reg_status = reg_status;

        if (lac) {
            ast_string_field_set(pvt, location_area_code, lac);
            ast_verb(1, "[%s] Location area code: %s\n", PVT_ID(pvt), S_OR(lac, ""));
        }
        if (ci) {
            ast_string_field_set(pvt, cell_id, ci);
            ast_verb(1, "[%s] Cell ID: %s\n", PVT_ID(pvt), S_OR(ci, ""));
        }
        if (act >= 0) {
            const int mact = map_creg_act(act);
            if (mact >= 0) {
                ast_verb(1, "[%s] Access technology: %s [%d]\n", PVT_ID(pvt), sys_act2str(mact), act);
                pvt_set_act(pvt, mact);
            }
        }
    } else {
        pvt->gsm_registered = 0;
        pvt_set_act(pvt, 0);
        ast_string_field_set(pvt, location_area_code, NULL);
        ast_string_field_set(pvt, cell_id, NULL);
    }

    return 0;
}

/*!
 * \brief Handle AT+CGMI response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cgmi(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, manufacturer, ast_str_buffer(response));
    ast_verb(3, "[%s] Manufacturer: %s\n", PVT_ID(pvt), pvt->manufacturer);

    if (!strncasecmp(ast_str_buffer(response), MANUFACTURER_QUECTEL, STRLEN(MANUFACTURER_QUECTEL))) {
        ast_verb(1, "[%s] Quectel module\n", PVT_ID(pvt));
        pvt->is_simcom = 0;
        pvt->has_voice = 0;
        return at_enqueue_initialization_quectel(&pvt->sys_chan, CONF_SHARED(pvt, dsci));
    } else if (!strncasecmp(ast_str_buffer(response), MANUFACTURER_SIMCOM, STRLEN(MANUFACTURER_SIMCOM))) {
        ast_verb(1, "[%s] SimCOM module\n", PVT_ID(pvt));
        pvt->is_simcom = 1;
        pvt->has_voice = 0;
        return at_enqueue_initialization_simcom(&pvt->sys_chan);
    } else {
        ast_log(LOG_WARNING, "[%s] Unknown module manufacturer: %s", PVT_ID(pvt), ast_str_buffer(response));
        pvt->has_voice = 0;
        return at_enqueue_initialization_other(&pvt->sys_chan);
    }

    return 0;
}

/*!
 * \brief Handle AT+CGMM response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

#/* */

static int at_response_cgmm(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, model, ast_str_buffer(response));
    ast_verb(2, "[%s] Model: %s\n", PVT_ID(pvt), pvt->model);

    static const char MODEL_A760E[] = "A7670E";

    if (!strncasecmp(ast_str_buffer(response), MODEL_A760E, STRLEN(MODEL_A760E))) {
        ast_verb(1, "[%s] SimCOM module\n", PVT_ID(pvt));
        ast_string_field_set(pvt, manufacturer, MANUFACTURER_SIMCOM);
        pvt->is_simcom = 1;
        return at_enqueue_initialization_simcom(&pvt->sys_chan);
    }

    return 0;
}

/*!
 * \brief Handle AT+CGMR response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cgmr(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, firmware, ast_str_buffer(response));
    ast_verb(2, "[%s] Revision identification: %s\n", PVT_ID(pvt), pvt->firmware);
    return 0;
}

/*!
 * \brief Handle AT+CGSN response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cgsn(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, imei, ast_str_buffer(response));
    ast_verb(2, "[%s] IMEI: %s\n", PVT_ID(pvt), pvt->imei);
    return 0;
}

/*!
 * \brief Handle AT+CIMI response
 * \param pvt -- pvt structure
 * \param response -- string containing response
 * \retval  0 success
 * \retval -1 error
 */

static int at_response_cimi(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, imsi, ast_str_buffer(response));
    ast_verb(2, "[%s] IMSI: %s\n", PVT_ID(pvt), pvt->imsi);
    return 0;
}

static int at_response_ccid(struct pvt* const pvt, const struct ast_str* const response)
{
    ast_string_field_set(pvt, iccid, ast_str_buffer(response));
    ast_verb(2, "[%s] ICCID: %s\n", PVT_ID(pvt), pvt->iccid);
    return 0;
}

static int at_response_xccid(struct pvt* const pvt, const struct ast_str* const response)
{
    char* ccid;

    if (at_parse_xccid(ast_str_buffer(response), &ccid)) {
        ast_log(LOG_ERROR, "[%s] Error parsing CCID: '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return 0;
    }

    ast_string_field_set(pvt, iccid, ccid);
    ast_verb(2, "[%s] ICCID: %s\n", PVT_ID(pvt), pvt->iccid);
    return 0;
}

static void send_dtmf_frame(struct pvt* const pvt, char c)
{
    if (!CONF_SHARED(pvt, dtmf)) {
        ast_debug(1, "[%s] Detected DTMF: %c", PVT_ID(pvt), c);
        return;
    }

    struct cpvt* const cpvt = pvt_channel_find_active(pvt);
    if (cpvt && cpvt->channel) {
        struct ast_frame f = {
            AST_FRAME_DTMF,
        };
        f.len              = CONF_SHARED(pvt, dtmf_duration);
        f.subclass.integer = c;
        if (ast_queue_frame(cpvt->channel, &f)) {
            ast_log(LOG_ERROR, "[%s] Fail to send detected DTMF: %c", PVT_ID(pvt), c);
        } else {
            ast_verb(1, "[%s] Detected DTMF: %c", PVT_ID(pvt), c);
        }
    } else {
        ast_log(LOG_WARNING, "[%s] Detected DTMF: %c", PVT_ID(pvt), c);
    }
}

static void at_response_qtonedet(struct pvt* const pvt, const struct ast_str* const response)
{
    int dtmf;
    char c = '\000';

    if (at_parse_qtonedet(ast_str_buffer(response), &dtmf)) {
        ast_log(LOG_ERROR, "[%s] Error parsing QTONEDET: '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    switch (dtmf) {
        case 48:
            c = '0';
            break;

        case 49:
            c = '1';
            break;

        case 50:
            c = '2';
            break;

        case 51:
            c = '3';
            break;

        case 52:
            c = '4';
            break;

        case 53:
            c = '5';
            break;

        case 54:
            c = '6';
            break;

        case 55:
            c = '7';
            break;

        case 56:
            c = '8';
            break;

        case 57:
            c = '9';
            break;

        case 65:
            c = 'A';
            break;

        case 66:
            c = 'B';
            break;

        case 67:
            c = 'C';
            break;

        case 68:
            c = 'D';
            break;

        case 42:
            c = '*';
            break;

        case 35:
            c = '#';
            break;
    }

    if (!c) {
        ast_log(LOG_WARNING, "[%s] Detected unknown DTMF code: %d", PVT_ID(pvt), dtmf);
        return;
    }

    send_dtmf_frame(pvt, c);
}

static void at_response_dtmf(struct pvt* const pvt, const struct ast_str* const response)
{
    char c = '\000';

    if (at_parse_dtmf(ast_str_buffer(response), &c)) {
        ast_log(LOG_ERROR, "[%s] Error parsing RXDTMF: '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }
    send_dtmf_frame(pvt, c);
}

static const char* qpcmv2str(int qpcmv)
{
    const char* const names[3] = {"USB NMEA port", "Debug UART", "USB sound card"};
    return enum2str_def((unsigned)qpcmv, names, ARRAY_LEN(names), "Unknown");
}

static void at_response_qpcmv(struct pvt* const pvt, const struct ast_str* const response)
{
    int enabled;
    int mode;

    if (at_parse_qpcmv(ast_str_buffer(response), &enabled, &mode)) {
        ast_log(LOG_ERROR, "[%s] Error parsing QPCMV: '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    ast_debug(1, "[%s] Voice configuration: %s [%s]\n", PVT_ID(pvt), qpcmv2str(mode), S_COR(enabled, "enabled", "disabled"));
}

static void show_module_time(struct pvt* const pvt)
{
    struct ast_str* ts_str = ast_str_alloca(AST_TM_MAX_LEN);
    format_ast_tm(&pvt->module_time, ts_str);
    ast_verb(3, "[%s] Module time: %s\n", PVT_ID(pvt), ast_str_buffer(ts_str));
}

static void at_response_qlts(struct pvt* const pvt, const struct ast_str* const response)
{
    if (at_parse_qlts(ast_str_buffer(response), &pvt->module_time)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    show_module_time(pvt);
}

static void at_response_cclk(struct pvt* const pvt, const struct ast_str* const response)
{
    if (at_parse_cclk(ast_str_buffer(response), &pvt->module_time)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    ast_tm_normalize(&pvt->module_time);
    show_module_time(pvt);
}

static void at_response_qrxgain(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain;

    if (at_parse_qrxgain(ast_str_buffer(response), &gain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str(gain), ast_free);
    ast_verb(2, "[%s] RX Gain: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(sgain), gain);
}

static void at_response_qmic(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain, dgain;

    if (at_parse_qmic(ast_str_buffer(response), &gain, &dgain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str(gain), ast_free);
    ast_verb(2, "[%s] Microphone Gain: %s [%d], %d\n", PVT_ID(pvt), ast_str_buffer(sgain), gain, dgain);
}

static void at_response_cmicgain(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain;

    if (at_parse_cxxxgain(ast_str_buffer(response), &gain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str_simcom(gain), ast_free);
    ast_verb(2, "[%s] RX Gain: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(sgain), gain);
}

static void at_response_coutgain(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain;

    if (at_parse_cxxxgain(ast_str_buffer(response), &gain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str_simcom(gain), ast_free);
    ast_verb(2, "[%s] TX Gain: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(sgain), gain);
}

static void at_response_crxvol(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain;

    if (at_parse_cxxvol(ast_str_buffer(response), &gain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str(gain), ast_free);
    ast_verb(2, "[%s] RX Volume: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(sgain), gain);
}

static void at_response_ctxvol(struct pvt* const pvt, const struct ast_str* const response)
{
    int gain;

    if (at_parse_cxxvol(ast_str_buffer(response), &gain)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return;
    }

    RAII_VAR(struct ast_str*, sgain, gain2str(gain), ast_free);
    ast_verb(2, "[%s] Microphone Gain: %s [%d]\n", PVT_ID(pvt), ast_str_buffer(sgain), gain);
}

static int at_response_qaudloop(struct pvt* const pvt, const struct ast_str* const response)
{
    int aloop;

    if (at_parse_qaudloop(ast_str_buffer(response), &aloop)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_verb(1, "[%s] Audio loop is %s\n", PVT_ID(pvt), S_COR(aloop, "enabled", "disabled"));
    return 0;
}

static int at_response_qaudmod(struct pvt* const pvt, const struct ast_str* const response)
{
    static const char* const amodes[] = {"handset", "headset", "speaker", "off", "bluetooth", "none"};

    int amode;

    if (at_parse_qaudmod(ast_str_buffer(response), &amode)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_verb(1, "[%s] Audio mode is %s\n", PVT_ID(pvt), enum2str_def((unsigned)amode, amodes, ARRAY_LEN(amodes), "unknown"));
    return 0;
}

static int at_response_cgmr_ex(struct pvt* const pvt, const struct ast_str* const response)
{
    char* cgmr;

    if (at_parse_cgmr(ast_str_buffer(response), &cgmr)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_string_field_set(pvt, firmware, cgmr);
    ast_verb(2, "[%s] Revision identification: %s\n", PVT_ID(pvt), pvt->firmware);
    return 0;
}

static int at_response_cpcmreg(struct pvt* const pvt, const struct ast_str* const response)
{
    int pcmreg;

    if (at_parse_cpcmreg(ast_str_buffer(response), &pcmreg)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (pcmreg) {
        ast_log(LOG_NOTICE, "[%s] SimCom - Voice channel active", PVT_ID(pvt));
    } else {
        ast_log(LOG_NOTICE, "[%s] SimCom - Voice channel inactive", PVT_ID(pvt));
    }
    return 0;
}

static int at_response_cnsmod(struct pvt* const pvt, const struct ast_str* const response)
{
    int act;

    if (at_parse_cnsmod(ast_str_buffer(response), &act)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    if (act >= 0) {
        ast_verb(1, "[%s] Access technology: %s\n", PVT_ID(pvt), sys_act2str(act));
        pvt_set_act(pvt, act);
    }
    return 0;
}

static int at_response_cring(struct pvt* const pvt, const struct ast_str* const response)
{
    char* ring_type;

    if (at_parse_cring(ast_str_buffer(response), &ring_type)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_debug(2, "[%s] Receive RING: %s\n", PVT_ID(pvt), ring_type);
    return 0;
}

static int at_response_psnwid(struct pvt* const pvt, const struct ast_str* const response)
{
    int mcc, mnc;
    char *fnn, *snn;

    if (at_parse_psnwid(ast_str_buffer(response), &mcc, &mnc, &fnn, &snn)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    char utf8_str[50];
    if (from_ucs2(fnn, utf8_str, STRLEN(utf8_str))) {
        ast_log(LOG_ERROR, "[%s] Error decoding full network name: %s\n", PVT_ID(pvt), fnn);
        return -1;
    }
    ast_string_field_set(pvt, network_name, utf8_str);

    if (from_ucs2(snn, utf8_str, STRLEN(utf8_str))) {
        ast_log(LOG_ERROR, "[%s] Error decoding short network name: %s\n", PVT_ID(pvt), snn);
        return -1;
    }
    ast_string_field_set(pvt, short_network_name, utf8_str);

    pvt->operator= mcc * 100 + mnc;
    ast_verb(1, "[%s] Operator: %s/%s\n", PVT_ID(pvt), pvt->network_name, pvt->short_network_name);
    ast_verb(1, "[%s] Registered PLMN: %d\n", PVT_ID(pvt), pvt->operator);

    return 0;
}

static int at_response_ciev(struct pvt* const pvt, const struct ast_str* const response)
{
    int plmn;
    char *fnn, *snn;

    if (at_parse_ciev_10(ast_str_buffer(response), &plmn, &fnn, &snn)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s', ignoring\n", PVT_ID(pvt), ast_str_buffer(response));
        return 0;
    }

    ast_string_field_set(pvt, network_name, fnn);
    ast_string_field_set(pvt, short_network_name, snn);
    pvt->operator= plmn;

    ast_verb(1, "[%s] Operator: %s/%s\n", PVT_ID(pvt), pvt->network_name, pvt->short_network_name);
    ast_verb(1, "[%s] Registered PLMN: %d\n", PVT_ID(pvt), pvt->operator);

    return 0;
}

static int at_response_psuttz(struct pvt* const pvt, const struct ast_str* const response)
{
    int time_zone;
    struct ast_tm ts;

    memset(&ts, 0, sizeof(ts));

    if (at_parse_psuttz(ast_str_buffer(response), &ts.tm_year, &ts.tm_mon, &ts.tm_mday, &ts.tm_hour, &ts.tm_min, &ts.tm_sec, &time_zone, &ts.tm_isdst)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ts.tm_gmtoff     = 15 * 60 * time_zone;
    pvt->module_time = *ast_tm_normalize(&ts);

    show_module_time(pvt);
    return 0;
}

static int at_response_revision(struct pvt* const pvt, const struct ast_str* const response)
{
    char* rev;

    if (at_parse_revision(ast_str_buffer(response), &rev)) {
        ast_log(LOG_ERROR, "[%s] Error parsing '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
        return -1;
    }

    ast_string_field_set(pvt, firmware, rev);
    ast_verb(2, "[%s] Revision identification: %s\n", PVT_ID(pvt), pvt->firmware);
    return 0;
}

static int check_at_res(const at_res_t at_res)
{
    switch (at_res) {
        case RES_OK:
        case RES_ERROR:
        case RES_SMS_PROMPT:
            return 1;

        default:
            return 0;
    }
}

static void show_response(const struct pvt* const pvt, const at_queue_cmd_t* const ecmd, const struct ast_str* const response, const at_res_t at_res)
{
    // U+2190 : Leftwards arrow : 0xE2 0x86 0x90
    if (ecmd && ecmd->cmd == CMD_USER) {
        if (check_at_res(at_res)) {
            ast_verb(2, "[%s][%s] \xE2\x86\x90 [%s][%s]\n", PVT_ID(pvt), at_cmd2str(ecmd->cmd), at_res2str(at_res), tmp_esc_str(response));
        } else {
            ast_verb(1, "[%s][%s] \xE2\x86\x90 [%s][%s]\n", PVT_ID(pvt), at_cmd2str(ecmd->cmd), at_res2str(at_res), tmp_esc_str(response));
        }
        return;
    }

    const int lvl = check_at_res(at_res) ? 4 : 2;
    if (ecmd) {
        ast_debug(lvl, "[%s][%s] \xE2\x86\x90 [%s][%s]\n", PVT_ID(pvt), at_cmd2str(ecmd->cmd), at_res2str(at_res), tmp_esc_str(response));
    } else {
        ast_debug(lvl, "[%s] \xE2\x86\x90 [%s][%s]\n", PVT_ID(pvt), at_res2str(at_res), tmp_esc_str(response));
    }
}

/*!
 * \brief Do response
 * \param pvt -- pvt structure
 * \param response -- result string
 * \param at_res -- result type
 * \retval  0 success
 * \retval -1 error
 */

int at_response(struct pvt* const pvt, const struct ast_str* const response, const at_res_t at_res)
{
    if (at_res != RES_TIMEOUT && !ast_str_strlen(response)) {
        return 0;
    }

    const at_queue_task_t* const task = at_queue_head_task(pvt);
    const at_queue_cmd_t* const ecmd  = at_queue_task_cmd(task);
    show_response(pvt, ecmd, response, at_res);

    switch (at_res) {
        case RES_BOOT:
        case RES_CSSI:
        case RES_CSSU:
        case RES_SRVST:
        case RES_CVOICE:
        case RES_CPMS:
        case RES_MISSED_CALL:
        case RES_DST:
        case RES_VOICE_CALL:
        case RES_RCEND:
            return 0;

        case RES_OK:
            at_response_ok(pvt, at_res, task, ecmd);
            return 0;

        case RES_CMS_ERROR:
        case RES_ERROR:
        case RES_TIMEOUT:
            return at_response_error(pvt, at_res, task, ecmd);

        case RES_QIND:
            /* An error here is not fatal. Just keep going. */
            at_response_qind(pvt, response);
            return 0;

        case RES_DSCI:
            return at_response_dsci(pvt, response);

        case RES_CREG:
            /* An error here is not fatal. Just keep going. */
            at_response_creg(pvt, 0, response);
            return 0;

        case RES_CEREG:
            /* An error here is not fatal. Just keep going. */
            at_response_creg(pvt, 1, response);
            return 0;

        case RES_COPS:
            /* An error here is not fatal. Just keep going. */
            at_response_cops(pvt, response);
            return 0;

        case RES_QSPN:
            at_response_qspn(pvt, response);
            return 0;

        case RES_CSPN:
            at_response_cspn(pvt, response);
            return 0;

        case RES_QNWINFO:
            at_response_qnwinfo(pvt, response);
            return 0;

        case RES_CSQ:
            /* An error here is not fatal. Just keep going. */
            at_response_csq(pvt, response);
            break;

        case RES_CSQN:
            /* An error here is not fatal. Just keep going. */
            at_response_csqn(pvt, response);
            break;

        case RES_SMMEMFULL:
            return at_response_smmemfull(pvt);

        case RES_CDSI:
            return at_response_cdsi(pvt, response);

        case RES_CMTI:
            return at_response_cmti(pvt, response);

        case RES_CMGR:
        case RES_CMGL:
        case RES_CMT:
        case RES_CBM:
        case RES_CDS:
        case RES_CLASS0:
            return at_response_msg(pvt, response, at_res);

        case RES_SMS_PROMPT:
            return at_response_sms_prompt(pvt, task);

        case RES_CMGS:
            at_response_cmgs(pvt, response, task);
            return 0;

        case RES_CUSD:
            /* An error here is not fatal. Just keep going. */
            at_response_cusd(pvt, response, 0);
            break;

        case RES_CLCC:
            return at_response_clcc(pvt, response);

        case RES_CCWA:
            return at_response_ccwa(pvt, response);

        case RES_CRING:
            return at_response_cring(pvt, response);

        case RES_RING:
            ast_debug(2, "[%s] Receive RING\n", PVT_ID(pvt));
            break;

        case RES_BUSY:
            ast_debug(2, "[%s] Receive BUSY\n", PVT_ID(pvt));
            return 0;

        case RES_NO_DIALTONE:
            ast_debug(2, "[%s] Receive NO DIALTONE\n", PVT_ID(pvt));
            break;

        case RES_NO_ANSWER:
            ast_debug(2, "[%s] Receive NO ANSWER\n", PVT_ID(pvt));
            return 0;

        case RES_NO_CARRIER:
            ast_debug(2, "[%s] Receive NO CARRIER\n", PVT_ID(pvt));
            return 0;

        case RES_CPIN:
            /* fatal */
            return at_response_cpin(pvt, response);

        case RES_CNUM:
            /* An error here is not fatal. Just keep going. */
            at_response_cnum(pvt, response);
            return 0;

        case RES_CSCA:
            /* An error here is not fatal. Just keep going. */
            at_response_csca(pvt, response);
            return 0;

        case RES_QTONEDET:
            at_response_qtonedet(pvt, response);
            return 0;

        case RES_DTMF:
        case RES_RXDTMF:
            at_response_dtmf(pvt, response);
            return 0;

        case RES_QPCMV:
            at_response_qpcmv(pvt, response);
            return 0;

        case RES_QLTS:
            at_response_qlts(pvt, response);
            return 0;

        case RES_CCLK:
            at_response_cclk(pvt, response);
            return 0;

        case RES_QRXGAIN:
            at_response_qrxgain(pvt, response);
            return 0;

        case RES_QMIC:
            at_response_qmic(pvt, response);
            return 0;

        case RES_CMICGAIN:
            at_response_cmicgain(pvt, response);
            return 0;

        case RES_COUTGAIN:
            at_response_coutgain(pvt, response);
            return 0;

        case RES_CTXVOL:
            at_response_ctxvol(pvt, response);
            return 0;

        case RES_CRXVOL:
            at_response_crxvol(pvt, response);
            return 0;

        case RES_CSMS:
            return 0;

        case RES_QAUDMOD:
            return at_response_qaudmod(pvt, response);

        case RES_QAUDLOOP:
            return at_response_qaudloop(pvt, response);

        case RES_CGMR:
            return at_response_cgmr_ex(pvt, response);

        case RES_CPCMREG:
            return at_response_cpcmreg(pvt, response);

        case RES_CNSMOD:
            return at_response_cnsmod(pvt, response);

        case RES_PSNWID:
            return at_response_psnwid(pvt, response);

        case RES_CIEV:
            return at_response_ciev(pvt, response);

        case RES_PSUTTZ:
            return at_response_psuttz(pvt, response);

        case RES_REVISION:
            return at_response_revision(pvt, response);

        case RES_ICCID:
        case RES_QCCID:
            return at_response_xccid(pvt, response);

        case RES_PARSE_ERROR:
            ast_log(LOG_ERROR, "[%s] Error parsing result\n", PVT_ID(pvt));
            return -1;

        case RES_UNKNOWN:
            if (ecmd) {
                switch (ecmd->cmd) {
                    case CMD_AT_CGMI:
                        ast_debug(2, "[%s] Got manufacturer info\n", PVT_ID(pvt));
                        return at_response_cgmi(pvt, response);

                    case CMD_AT_CGMM:
                        ast_debug(2, "[%s] Got model info\n", PVT_ID(pvt));
                        return at_response_cgmm(pvt, response);

                    case CMD_AT_CGMR:
                        ast_debug(2, "[%s] Got firmware info\n", PVT_ID(pvt));
                        return at_response_cgmr(pvt, response);

                    case CMD_AT_CGSN:
                        ast_debug(2, "[%s] Got IMEI number\n", PVT_ID(pvt));
                        return at_response_cgsn(pvt, response);

                    case CMD_AT_CIMI:
                        ast_debug(2, "[%s] Got IMSI number\n", PVT_ID(pvt));
                        return at_response_cimi(pvt, response);

                    case CMD_AT_CCID:
                        ast_debug(2, "[%s] Got ICCID number\n", PVT_ID(pvt));
                        return at_response_ccid(pvt, response);

                    case CMD_USER:
                        return 0;

                    default:
                        break;
                }
            }
            ast_debug(1, "[%s] Ignoring unknown result: '%s'\n", PVT_ID(pvt), ast_str_buffer(response));
            break;

        case COMPATIBILITY_RES_START_AT_MINUSONE:
            break;
    }

    return 0;
}

struct at_response_taskproc_data* at_response_taskproc_data_alloc(struct pvt* const pvt, const struct ast_str* const response)
{
    const size_t response_len = ast_str_strlen(response);

    struct at_response_taskproc_data* const res = ast_calloc(1, sizeof(struct at_response_taskproc_data) + response_len + 1u);
    res->ptd.pvt                                = pvt;
    res->response.__AST_STR_LEN                 = response_len + 1u;
    res->response.__AST_STR_USED                = response_len;
    res->response.__AST_STR_TS                  = DS_STATIC;
    ast_copy_string(ast_str_buffer(&res->response), ast_str_buffer(response), response_len + 1u);
    return res;
}

static void response_taskproc(struct pvt_taskproc_data* ptd)
{
    struct at_response_taskproc_data* const rtd = (struct at_response_taskproc_data*)ptd;
    const at_res_t at_res                       = at_str2res(&rtd->response);

    if (at_res != RES_UNKNOWN) {
        ast_str_trim_blanks(&rtd->response);
    }

    ast_atomic_fetchadd_uint32(&PVT_STAT(rtd->ptd.pvt, at_responses), 1);
    if (at_response(rtd->ptd.pvt, &rtd->response, at_res)) {
        ast_log(LOG_WARNING, "[%s] Fail to handle response\n", PVT_ID(rtd->ptd.pvt));
    }

    if (at_queue_run(rtd->ptd.pvt)) {
        ast_log(LOG_ERROR, "[%s] Fail to run command from queue\n", PVT_ID(rtd->ptd.pvt));
        eventfd_signal(rtd->ptd.pvt->monitor_thread_event);
    }
}

int at_response_taskproc(void* tpdata) { return PVT_TASKPROC_LOCK_AND_EXECUTE(tpdata, response_taskproc); }
