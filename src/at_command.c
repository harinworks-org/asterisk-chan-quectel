/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
*/

/*
   Copyright (C) 2009 - 2010 Artem Makhutov
   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org
   Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/

#include "ast_config.h"

#include <asterisk/causes.h>
#include <asterisk/utils.h>

#include "at_command.h"

#include "at_queue.h"
#include "chan_quectel.h" /* struct pvt */
#include "channel.h"
#include "char_conv.h" /* char_to_hexstr_7bit() */
#include "error.h"
#include "helpers.h"
#include "pdu.h" /* build_pdu() */
#include "smsdb.h"

DECLARE_AT_CMD(at, "");
DECLARE_AT_CMD(chld2, "+CHLD=2");

/*!
 * \brief Get the string representation of the given AT command
 * \param cmd -- the command to process
 * \return a string describing the given command
 */

const char* at_cmd2str(at_cmd_t cmd)
{
    static const char* const cmds[] = {AT_COMMANDS_TABLE(AT_CMD_AS_STRING)};

    return enum2str_def(cmd, cmds, ARRAY_LEN(cmds), "UNDEFINED");
}

/*!
 * \brief Format and fill generic command
 * \param cmd -- the command structure
 * \param format -- printf format string
 * \param ap -- list of arguments
 * \return 0 on success
 */

static int __attribute__((format(printf, 2, 0))) at_fill_generic_cmd_va(at_queue_cmd_t* cmd, const char* format, va_list ap)
{
    static const ssize_t AT_CMD_DEF_LEN = 32;

    RAII_VAR(struct ast_str*, cmdstr, ast_str_create(AT_CMD_DEF_LEN), ast_free);

    ast_str_set_va(&cmdstr, 0, format, ap);
    const size_t cmdlen = ast_str_strlen(cmdstr);

    if (!cmdlen) {
        return -1;
    }

    cmd->data    = ast_strndup(ast_str_buffer(cmdstr), cmdlen);
    cmd->length  = (unsigned int)cmdlen;
    cmd->flags  &= ~ATQ_CMD_FLAG_STATIC;

    return 0;
}

/*!
 * \brief Format and fill generic command
 * \param cmd -- the command structure
 * \param format -- printf format string
 * \return 0 on success
 */

static int __attribute__((format(printf, 2, 3))) at_fill_generic_cmd(at_queue_cmd_t* cmd, const char* format, ...)
{
    va_list ap;

    va_start(ap, format);
    const int rv = at_fill_generic_cmd_va(cmd, format, ap);
    va_end(ap);

    return rv;
}

/*!
 * \brief Enqueue generic command
 * \param pvt -- pvt structure
 * \param cmd -- at command
 * \param prio -- queue priority of this command
 * \param format -- printf format string including AT command text
 * \return 0 on success
 */

static int __attribute__((format(printf, 4, 5))) at_enqueue_generic(struct cpvt* cpvt, at_cmd_t at_cmd, int prio, const char* format, ...)
{
    va_list ap;

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(at_cmd);

    va_start(ap, format);
    const int rv = at_fill_generic_cmd_va(&cmd, format, ap);
    va_end(ap);

    if (rv) {
        return rv;
    }

    return at_queue_insert(cpvt, &cmd, 1u, prio);
}

int at_enqueue_at(struct cpvt* cpvt)
{
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_STFT(CMD_AT, RES_OK, AT_CMD(at), 0, ATQ_CMD_TIMEOUT_SHORT, 0);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue initialization commands
 * \param cpvt -- cpvt structure
 * \param from_command -- begin initialization from this command in list
 * \return 0 on success
 */
int at_enqueue_initialization(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(at, "");
    DECLARE_AT_CMD(z, "Z");
    DECLARE_AT_CMD(ate0, "E0");

    DECLARE_AT_CMD(cgmi, "+CGMI");
    DECLARE_AT_CMD(csca, "+CSCA?");
    DECLARE_AT_CMD(cgmm, "+CGMM");
    DECLARE_AT_CMD(cgmr, "+CGMR");

    DECLARE_AT_CMD(cmee, "+CMEE=0");
    DECLARE_AT_CMD(cgsn, "+CGSN");
    DECLARE_AT_CMD(cimi, "+CIMI");
    DECLARE_AT_CMD(cpin, "+CPIN?");

    DECLARE_AT_CMD(cops, "+COPS=3,0");
    DECLARE_AT_CMD(creg_2, "+CREG=2");
    DECLARE_AT_CMD(creg, "+CREG?");
    DECLARE_AT_CMD(cnum, "+CNUM");

    DECLARE_AT_CMD(cssn, "+CSSN=0,0");
    DECLARE_AT_CMD(cmgf, "+CMGF=0");
    DECLARE_AT_CMD(cscs, "+CSCS=\"UCS2\"");

    static const at_queue_cmd_t st_cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT, at),        /* Auto sense */
        ATQ_CMD_DECLARE_ST(CMD_AT_Z, z),       /* Restore default settings */
        ATQ_CMD_DECLARE_ST(CMD_AT_E, ate0),    /* Disable echo */
        ATQ_CMD_DECLARE_ST(CMD_AT_CSCS, cscs), /* Set UCS-2 text encoding */

        ATQ_CMD_DECLARE_ST(CMD_AT_CGMI, cgmi),      /* Getting manufacturer info */
        ATQ_CMD_DECLARE_ST(CMD_AT_CGMM, cgmm),      /* Get Product name */
        ATQ_CMD_DECLARE_ST(CMD_AT_CGMR, cgmr),      /* Get software version */
        ATQ_CMD_DECLARE_ST(CMD_AT_CMEE, cmee),      /* Set MS Error Report to 'ERROR' only, TODO: change to 1 or 2 and add support in response handlers */
        ATQ_CMD_DECLARE_ST(CMD_AT_CGSN, cgsn),      /* IMEI Read */
        ATQ_CMD_DECLARE_ST(CMD_AT_CIMI, cimi),      /* IMSI Read */
        ATQ_CMD_DECLARE_ST(CMD_AT_CPIN, cpin),      /* Check is password authentication requirement and the remainder validation times */
        ATQ_CMD_DECLARE_ST(CMD_AT_COPS_INIT, cops), /* Read operator name */

        ATQ_CMD_DECLARE_STI(CMD_AT_CREG_INIT, creg_2), /* GSM registration status setting */
        ATQ_CMD_DECLARE_ST(CMD_AT_CREG, creg),         /* GSM registration status */

        ATQ_CMD_DECLARE_STI(CMD_AT_CNUM, cnum), /* Get Subscriber number */

        ATQ_CMD_DECLARE_ST(CMD_AT_CSCA, csca), /* Get SMS Service center address */

        ATQ_CMD_DECLARE_ST(CMD_AT_CSSN, cssn), /* Activate Supplementary Service Notification with CSSI and CSSU */
        ATQ_CMD_DECLARE_ST(CMD_AT_CMGF, cmgf), /* Set Message Format */

        ATQ_CMD_DECLARE_DYN(CMD_AT_CNMI), /* SMS Event Reporting Configuration */
        ATQ_CMD_DECLARE_DYN(CMD_AT_CPMS), /* SMS Storage Selection */
        ATQ_CMD_DECLARE_DYN(CMD_AT_CSMS), /* Select Message Service */
        ATQ_CMD_DECLARE_DYNI(CMD_AT_VTD), /* Set tone duration */
    };

    unsigned in, out;
    struct pvt* const pvt = cpvt->pvt;
    at_queue_cmd_t cmds[ARRAY_LEN(st_cmds)];
    at_queue_cmd_t dyn_cmd;
    int dyn_handled;

    /* customize list */
    for (in = out = 0; in < ARRAY_LEN(st_cmds); ++in) {
        if (st_cmds[in].cmd == CMD_AT_Z && !CONF_SHARED(pvt, reset_modem)) {
            continue;
        }

        if (!(st_cmds[in].flags & ATQ_CMD_FLAG_STATIC)) {
            dyn_cmd     = st_cmds[in];
            dyn_handled = 0;

            if (st_cmds[in].cmd == CMD_AT_CPMS) {
                if (CONF_SHARED(pvt, msg_storage) == MESSAGE_STORAGE_AUTO) {
                    continue;
                }

                const char* const stor = dc_msgstor2str(CONF_SHARED(pvt, msg_storage));
                if (at_fill_generic_cmd(&dyn_cmd, "AT+CPMS=\"%s\",\"%s\",\"%s\"\r", stor, stor, stor)) {
                    ast_log(LOG_ERROR, "[%s] Device initialization - unable to create AT+CPMS command\n", PVT_ID(pvt));
                    continue;
                }
                dyn_handled = 1;
            }

            if (st_cmds[in].cmd == CMD_AT_CNMI) {
                int err = 0;
                if (!CONF_SHARED(pvt, msg_direct)) {
                    continue;
                }

                if (CONF_SHARED(pvt, msg_direct) > 0) {
                    err = at_fill_generic_cmd(&dyn_cmd, "AT+CNMI=2,2,2,0,%d\r", CONF_SHARED(pvt, reset_modem) ? 1 : 0);
                } else {
                    err = at_fill_generic_cmd(&dyn_cmd, "AT+CNMI=2,1,0,2,%d\r", CONF_SHARED(pvt, reset_modem) ? 1 : 0);
                }

                if (err) {
                    ast_log(LOG_ERROR, "[%s] Device initialization - unable to create AT+CNMI command\n", PVT_ID(pvt));
                    continue;
                }

                dyn_handled = 1;
            }

            if (st_cmds[in].cmd == CMD_AT_CSMS) {
                DECLARE_AT_CMDNT(csms, "+CSMS=%d");
                if (CONF_SHARED(pvt, msg_service) < 0) {
                    continue;
                }

                if (at_fill_generic_cmd(&dyn_cmd, AT_CMD(csms), CONF_SHARED(pvt, msg_service))) {
                    ast_log(LOG_ERROR, "[%s] Device initialization - unable to create AT+CSMS command\n", PVT_ID(pvt));
                    continue;
                }

                dyn_handled = 1;
            }

            if (st_cmds[in].cmd == CMD_AT_VTD) {
                DECLARE_AT_CMDNT(vtd, "+VTD=%d");
                if (CONF_SHARED(pvt, dtmf_duration) < 0) {
                    continue;
                }

                const int vtd = (int)(CONF_SHARED(pvt, dtmf_duration) / 100l);
                if (at_fill_generic_cmd(&dyn_cmd, AT_CMD(vtd), vtd)) {
                    ast_log(LOG_ERROR, "[%s] Device initialization - unable to create AT+VTD command\n", PVT_ID(pvt));
                    continue;
                }

                dyn_handled = 1;
            }
        }

        if (st_cmds[in].flags & ATQ_CMD_FLAG_STATIC) {
            cmds[out] = st_cmds[in];
        } else {
            if (dyn_handled) {
                cmds[out] = dyn_cmd;
            } else {
                ast_log(LOG_ERROR, "[%s] Device initialization - unhanled dynamic command: [%u] %s\n", PVT_ID(pvt), in, at_cmd2str(dyn_cmd.cmd));
                continue;
            }
        }
        out++;
    }

    if (out > 0) {
        return at_queue_insert(cpvt, cmds, out, 0);
    }
    return 0;
}

int at_enqueue_initialization_quectel(struct cpvt* cpvt, unsigned int dsci)
{
    DECLARE_AT_CMD(qpcmv, "+QPCMV?");

    DECLARE_AT_CMD(qindcfg_cc, "+QINDCFG=\"ccinfo\",1,0");
    DECLARE_AT_CMD(qindcfg_cc_off, "+QINDCFG=\"ccinfo\",0,0");

    DECLARE_AT_CMD(dsci, "^DSCI=1");
    DECLARE_AT_CMD(dsci_off, "^DSCI=0");

    DECLARE_AT_CMD(qindcfg_csq, "+QINDCFG=\"csq\",1,0");
    DECLARE_AT_CMD(qindcfg_act, "+QINDCFG=\"act\",1,0");
    DECLARE_AT_CMD(qindcfg_ring, "+QINDCFG=\"ring\",0,0");

    DECLARE_AT_CMD(qtonedet_0, "+QTONEDET=0");
    DECLARE_AT_CMD(qtonedet_1, "+QTONEDET=1");

    DECLARE_AT_CMD(qccid, "+QCCID");

    static const at_queue_cmd_t ccinfo_cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT_QINDCFG_CC, qindcfg_cc),
        ATQ_CMD_DECLARE_ST(CMD_AT_DSCI, dsci),
        ATQ_CMD_DECLARE_STI(CMD_AT_QINDCFG_CC_OFF, qindcfg_cc_off),
        ATQ_CMD_DECLARE_STI(CMD_AT_DSCI_OFF, dsci_off),
    };

    static const at_queue_cmd_t tonedet_cmds[] = {
        ATQ_CMD_DECLARE_STI(CMD_AT_QTONEDET_0, qtonedet_0),
        ATQ_CMD_DECLARE_STI(CMD_AT_QTONEDET_1, qtonedet_1),
    };

    DECLARE_AT_CMD(cereg_init, "+CEREG=2");

    struct pvt* const pvt   = cpvt->pvt;
    const unsigned int dtmf = CONF_SHARED(pvt, dtmf);

    const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_STI(CMD_AT_QCCID, qccid),
        ATQ_CMD_DECLARE_STI(CMD_AT_QTONEDET_1, qtonedet_1),
        ATQ_CMD_DECLARE_ST(CMD_AT_CVOICE, qpcmv), /* read the current voice mode, and return sampling rate、data bit、frame period */
        ccinfo_cmds[dsci ? 2 : 3],
        ccinfo_cmds[dsci ? 1 : 0],
        ATQ_CMD_DECLARE_ST(CMD_AT_QINDCFG_CSQ, qindcfg_csq),
        ATQ_CMD_DECLARE_ST(CMD_AT_QINDCFG_ACT, qindcfg_act),
        ATQ_CMD_DECLARE_ST(CMD_AT_QINDCFG_RING, qindcfg_ring),
        tonedet_cmds[dtmf ? 1 : 0],
        ATQ_CMD_DECLARE_ST(CMD_AT_CEREG_INIT, cereg_init),
        ATQ_CMD_DECLARE_ST(CMD_AT_FINAL, at),
    };

    return at_queue_insert_const(cpvt, cmds, ARRAY_LEN(cmds), 0);
}

int at_enqueue_initialization_simcom(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(ccid, "+CCID");
    DECLARE_AT_CMD(ciccid, "+CICCID");

    DECLARE_AT_CMD(cpcmreg, "+CPCMREG?");
    DECLARE_AT_CMD(clcc_1, "+CLCC=1");
    DECLARE_AT_CMD(cnsmod_1, "+CNSMOD=1");
    DECLARE_AT_CMD(cereg_init, "+CEREG=2");
    DECLARE_AT_CMD(cereg_query, "+CEREG?");
    DECLARE_AT_CMD(autocsq_init, "+AUTOCSQ=1,1");
    DECLARE_AT_CMD(exunsol_init, "+EXUNSOL=\"SQ\",1");
    DECLARE_AT_CMD(clts_init, "+CLTS=1");

    DECLARE_AT_CMD(ddet_0, "+DDET=0");
    DECLARE_AT_CMD(ddet_1, "+DDET=1");

    static const at_queue_cmd_t ddet_cmds[] = {
        ATQ_CMD_DECLARE_STI(CMD_AT_DDET_0, ddet_0),
        ATQ_CMD_DECLARE_STI(CMD_AT_DDET_1, ddet_1),
    };

    struct pvt* const pvt   = cpvt->pvt;
    const unsigned int dtmf = CONF_SHARED(pvt, dtmf);

    const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_STI(CMD_AT_CCID, ccid),
        ATQ_CMD_DECLARE_STI(CMD_AT_CICCID, ciccid),
        ATQ_CMD_DECLARE_STI(CMD_AT_CPCMREG, cpcmreg),
        ATQ_CMD_DECLARE_ST(CMD_AT_CLCC, clcc_1),
        ATQ_CMD_DECLARE_STI(CMD_AT_CEREG_INIT, cereg_init),
        ATQ_CMD_DECLARE_STI(CMD_AT_CEREG, cereg_query),
        ATQ_CMD_DECLARE_STI(CMD_AT_CNSMOD_1, cnsmod_1),
        ATQ_CMD_DECLARE_STI(CMD_AT_AUTOCSQ_INIT, autocsq_init),
        ATQ_CMD_DECLARE_STI(CMD_AT_EXUNSOL_INIT, exunsol_init),
        ATQ_CMD_DECLARE_STI(CMD_AT_CLTS_INIT, clts_init),
        ddet_cmds[dtmf ? 1 : 0],
        ATQ_CMD_DECLARE_ST(CMD_AT_FINAL, at),
    };

    return at_queue_insert_const(cpvt, cmds, ARRAY_LEN(cmds), 0);
}

int at_enqueue_initialization_other(struct cpvt* cpvt)
{
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_FINAL, at);

    return at_queue_insert_const(cpvt, &cmd, 1u, 0);
}

/*!
 * \brief Enqueue the AT+COPS? command
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */

int at_enqueue_cspn_cops(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(cspn, "+CSPN?");
    DECLARE_AT_CMD(cops, "+COPS?");

    static at_queue_cmd_t cmds[] = {ATQ_CMD_DECLARE_STI(CMD_AT_CSPN, cspn), ATQ_CMD_DECLARE_STI(CMD_AT_COPS, cops)};

    if (at_queue_insert_const(cpvt, cmds, ARRAY_LEN(cmds), 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qspn_qnwinfo(struct cpvt* cpvt)
{
    DECLARE_NAKED_AT_CMD(qspn, "+QSPN");
    DECLARE_NAKED_AT_CMD(qnwinfo, "+QNWINFO");

    static at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_STI(CMD_AT_QSPN, qspn),
        ATQ_CMD_DECLARE_STI(CMD_AT, qnwinfo),
    };

    if (at_queue_insert_const_at_once(cpvt, cmds, ARRAY_LEN(cmds), 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/* SMS sending */
static int at_enqueue_pdu(const char* pdu, size_t length, size_t tpdulen, at_queue_cmd_t* const cmds)
{
    DECLARE_AT_CMDNT(cmgs, "+CMGS=%d");

    static const at_queue_cmd_t st_cmds[] = {
        {CMD_AT_CMGS,    RES_SMS_PROMPT, ATQ_CMD_FLAG_DEFAULT, {ATQ_CMD_TIMEOUT_MEDIUM, 0}, NULL, 0},
        {CMD_AT_SMSTEXT, RES_OK,         ATQ_CMD_FLAG_DEFAULT, {ATQ_CMD_TIMEOUT_LONG, 0},   NULL, 0},
    };

    // DATA
    cmds[1]           = st_cmds[1];
    char* const cdata = cmds[1].data = ast_malloc(length + 2);
    if (!cdata) {
        return -ENOMEM;
    }

    cmds[1].length = length + 1;
    memcpy(cdata, pdu, length);
    cdata[length]     = 0x1A;
    cdata[length + 1] = 0x0;

    // AT
    cmds[0] = st_cmds[0];
    if (at_fill_generic_cmd(&cmds[0], AT_CMD(cmgs), (int)tpdulen)) {
        at_queue_free_data(&cmds[1]);
        return E_CMD_FORMAT;
    }

    return 0;
}

static void pdus_clear(at_queue_cmd_t* cmds, ssize_t idx)
{
    if (idx <= 0) {
        return;
    }

    const ssize_t u = idx * 2;
    for (ssize_t i = 0; i < u; ++i) {
        ast_free(cmds[i].data);
    }
}

static int pdus_build(pdu_part_t* const pdus, const char* sca, const char* msg, int csmsref, const char* destination, unsigned validity, int report_req)
{
    if (!pdus) {
        chan_quectel_err = E_BUILD_PDU;
        return -1;
    }

    const int msg_len             = strlen(msg);
    const size_t msg_ucs2_buf_len = sizeof(uint16_t) * msg_len * 2;
    RAII_VAR(uint16_t*, msg_ucs2, ast_calloc(sizeof(uint16_t), msg_len * 2), ast_free);

    if (!msg_ucs2) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    const ssize_t msg_ucs2_len = utf8_to_ucs2(msg, msg_len, msg_ucs2, msg_ucs2_buf_len);
    if (msg_ucs2_len <= 0) {
        chan_quectel_err = E_PARSE_UTF8;
        return msg_ucs2_len;
    }

    const int res = pdu_build_mult(pdus, sca, destination, msg_ucs2, msg_ucs2_len, validity, report_req, csmsref);
    return res;
}

static int pdus_enqueue(struct cpvt* const cpvt, const pdu_part_t* const pdus, ssize_t len, const int uid)
{
    RAII_VAR(at_queue_cmd_t*, cmds, ast_calloc(sizeof(at_queue_cmd_t), len * 2), ast_free);
    if (!cmds) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    RAII_VAR(char*, hexbuf, ast_malloc(PDU_LENGTH * 2 + 1), ast_free);
    if (!hexbuf) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    at_queue_cmd_t* pcmds = cmds;

    for (ssize_t i = 0; i < len; ++i, pcmds += 2) {
        hexify(pdus[i].buffer, pdus[i].length, hexbuf);
        if (at_enqueue_pdu(hexbuf, pdus[i].length * 2, pdus[i].tpdu_length, pcmds) < 0) {
            pdus_clear(cmds, i);
            return -1;
        }
    }

    if (at_queue_insert_uid(cpvt, cmds, len * 2, 0, uid)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue a SMS message(s)
 * \param cpvt -- cpvt structure
 * \param number -- the destination of the message
 * \param msg -- utf-8 encoded message
 */
int at_enqueue_sms(struct cpvt* cpvt, const char* sca, const char* destination, const char* msg, unsigned validity_minutes, int report_req)
{
    struct pvt* const pvt = cpvt->pvt;

    /* set default validity period */
    if (validity_minutes <= 0) {
        validity_minutes = 3 * 24 * 60;
    }

    const int csmsref = smsdb_get_refid(pvt->imsi, destination);
    if (csmsref < 0) {
        chan_quectel_err = E_SMSDB;
        return -1;
    }

    RAII_VAR(pdu_part_t*, pdus, ast_calloc(sizeof(pdu_part_t), 255), ast_free);
    const int pdus_len = pdus_build(pdus, sca, msg, csmsref, destination, validity_minutes, !!report_req);
    if (pdus_len <= 0) {
        return pdus_len;
    }

    const int uid = smsdb_outgoing_add(pvt->imsi, destination, msg, pdus_len, validity_minutes * 60, report_req);
    if (uid <= 0) {
        chan_quectel_err = E_SMSDB;
        return -1;
    }

    if (pdus_enqueue(cpvt, pdus, pdus_len, uid)) {
        return -1;
    }

    if (pdus_len <= 1) {
        ast_verb(1, "[%s][SMS:%d] Message enqueued\n", PVT_ID(pvt), uid);
    } else {
        ast_verb(1, "[%s][SMS:%d] Message enqueued in %d parts\n", PVT_ID(pvt), uid, pdus_len);
    }

    RAII_VAR(struct ast_json*, report, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(report, "info", ast_json_string_create("Message enqueued"));
    ast_json_object_set(report, "uid", ast_json_integer_create(uid));
    ast_json_object_set(report, "refid", ast_json_integer_create(csmsref));
    if (!ast_strlen_zero(msg)) {
        ast_json_object_set(report, "msg", ast_json_string_create(msg));
    }
    if (pdus_len > 1) {
        ast_json_object_set(report, "parts", ast_json_integer_create(pdus_len));
    }
    channel_start_local_report(pvt, "sms", LOCAL_REPORT_DIRECTION_OUTGOING, destination, NULL, NULL, 1, report);

    return 0;
}

/*!
 * \brief Enqueue AT+CUSD.
 * \param cpvt -- cpvt structure
 * \param code the CUSD code to send
 */

int at_enqueue_ussd(struct cpvt* cpvt, const char* code, int gsm7)
{
    static const size_t USSD_DEF_LEN = 64;

    static const char at_cmd[]     = "AT+CUSD=1,\"";
    static const char at_cmd_end[] = "\",15\r";

    RAII_VAR(struct ast_str*, buf, ast_str_create(USSD_DEF_LEN), ast_free);
    if (!buf) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    ast_str_set(&buf, 0, at_cmd);

    const size_t code_len = strlen(code);

    // use 7 bit encoding. 15 is 00001111 in binary and means 'Language using the GSM 7 bit default alphabet; Language
    // unspecified' accodring to GSM 23.038
    const size_t code16_buf_len = sizeof(uint16_t) * code_len * 2;
    RAII_VAR(uint16_t*, code16, ast_calloc(sizeof(uint16_t), code_len * 2), ast_free);
    if (!code16) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    const ssize_t code16_len = utf8_to_ucs2(code, code_len, code16, code16_buf_len);
    if (code16_len < 0) {
        chan_quectel_err = E_PARSE_UTF8;
        return -1;
    }

    if (gsm7) {
        const size_t code_packed_buf_len = sizeof(uint8_t) * 4069;
        RAII_VAR(uint8_t*, code_packed, ast_calloc(sizeof(uint8_t), 4069), ast_free);
        if (code_packed) {
            chan_quectel_err = E_MALLOC;
            return -1;
        }

        ssize_t res = gsm7_encode(code16, code16_len, code16);
        if (res < 0) {
            chan_quectel_err = E_ENCODE_GSM7;
            return -1;
        }

        res = gsm7_pack(code16, res, (char*)code_packed, code_packed_buf_len, 0);
        if (res < 0) {
            chan_quectel_err = E_PACK_GSM7;
            return -1;
        }

        res = (res + 1) / 2;
        ast_str_make_space(&buf, ast_str_strlen(buf) + (res * 2) + 1u);
        hexify(code_packed, res, ast_str_buffer(buf) + ast_str_strlen(buf));
    } else {
        ast_str_make_space(&buf, ast_str_strlen(buf) + (code16_len * 4) + 1u);
        hexify((const uint8_t*)code16, code16_len * 2, ast_str_buffer(buf) + ast_str_strlen(buf));
    }

    ast_str_update(buf);
    ast_str_append(&buf, 0, at_cmd_end);

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_CUSD);

    cmd.length = ast_str_strlen(buf);
    cmd.data   = ast_strndup(ast_str_buffer(buf), ast_str_strlen(buf));
    if (!cmd.data) {
        chan_quectel_err = E_MALLOC;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue a DTMF command
 * \param cpvt -- cpvt structure
 * \param digit -- the dtmf digit to send
 * \return -2 if digis is invalid, 0 on success
 */

int at_enqueue_dtmf(struct cpvt* cpvt, char digit)
{
    DECLARE_AT_CMDNT(vts, "+VTS=\"%c\"");

    switch (digit) {
        case 'a':
        case 'b':
        case 'c':
        case 'd':
            return at_enqueue_generic(cpvt, CMD_AT_DTMF, 1, AT_CMD(vts), toupper(digit));

        case 'A':
        case 'B':
        case 'C':
        case 'D':

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':

        case '*':
        case '#':
            return at_enqueue_generic(cpvt, CMD_AT_DTMF, 1, AT_CMD(vts), digit);
    }
    return -1;
}

/*!
 * \brief Enqueue the AT+CCWA command (configure call waiting)
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */

int at_enqueue_set_ccwa(struct cpvt* cpvt, unsigned call_waiting)
{
    DECLARE_AT_CMD(ccwa_get, "+CCWA=1,2,1");
    DECLARE_AT_CMDNT(ccwa_set, "+CCWA=%d,%d,%d");

    call_waiting_t cw;
    at_queue_cmd_t cmds[] = {
        /* Set Call-Waiting On/Off */
        ATQ_CMD_DECLARE_DYNIT(CMD_AT_CCWA_SET, ATQ_CMD_TIMEOUT_MEDIUM, 0),
        /* Query CCWA Status for Voice Call  */
        ATQ_CMD_DECLARE_STIT(CMD_AT_CCWA_STATUS, ccwa_get, ATQ_CMD_TIMEOUT_MEDIUM, 0),
    };
    at_queue_cmd_t* pcmd = cmds;
    unsigned cnt         = ARRAY_LEN(cmds);

    if (call_waiting == CALL_WAITING_DISALLOWED || call_waiting == CALL_WAITING_ALLOWED) {
        cw            = call_waiting;
        const int err = call_waiting == CALL_WAITING_ALLOWED ? 1 : 0;

        if (at_fill_generic_cmd(&cmds[0], AT_CMD(ccwa_set), err, err, CCWA_CLASS_VOICE)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }
    } else {
        cw = CALL_WAITING_AUTO;
        pcmd++;
        cnt--;
    }

    if (at_queue_insert(cpvt, pcmd, cnt, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    CONF_SHARED(cpvt->pvt, call_waiting) = cw;
    return 0;
}

/*!
 * \brief Enqueue the device reset command (AT+CFUN Operation Mode Setting)
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */

int at_enqueue_reset(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(cmd, "+CFUN=1,1");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CFUN, cmd);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue a dial commands
 * \param cpvt -- cpvt structure
 * \param number -- the called number
 * \param clir -- value of clir
 * \return 0 on success
 */
int at_enqueue_dial(struct cpvt* cpvt, const char* number, int clir)
{
    DECLARE_AT_CMDNT(clir, "+CLIR=%d");
    DECLARE_AT_CMDNT(atd, "D%s;");

    struct pvt* const pvt = cpvt->pvt;
    unsigned int cnt      = 0;
    char* tmp             = NULL;

    at_queue_cmd_t cmds[6];

    if (PVT_STATE(pvt, chan_count[CALL_STATE_ACTIVE]) > 0 && CPVT_TEST_FLAG(cpvt, CALL_FLAG_HOLD_OTHER)) {
        ATQ_CMD_INIT_ST(cmds[cnt], CMD_AT_CHLD_2, AT_CMD(chld2));
        /*  enable this cause response_clcc() see all calls are held and insert 'AT+CHLD=2'
            ATQ_CMD_INIT_ST(cmds[1], CMD_AT_CLCC, cmd_clcc);
        */
        cnt++;
    }

    if (clir != -1) {
        if (at_fill_generic_cmd(&cmds[cnt], AT_CMD(clir), clir)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }
        tmp = cmds[cnt].data;
        ATQ_CMD_INIT_DYNI(cmds[cnt], CMD_AT_CLIR);
        cnt++;
    }

    if (at_fill_generic_cmd(&cmds[cnt], AT_CMD(atd), number)) {
        ast_free(tmp);
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    ATQ_CMD_INIT_DYNI(cmds[cnt], CMD_AT_D);
    cnt++;

    if (at_queue_insert(cpvt, cmds, cnt, 1u)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    /* set CALL_FLAG_NEED_HANGUP early because ATD may be still in queue while local hangup called */
    CPVT_SET_FLAG(cpvt, CALL_FLAG_NEED_HANGUP);
    return 0;
}

/*!
 * \brief Enqueue a answer commands
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */
int at_enqueue_answer(struct cpvt* cpvt)
{
    DECLARE_AT_CMDNT(a, "A");
    DECLARE_AT_CMDNT(chld, "+CHLD=2%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_A);
    const char* fmt;

    switch (cpvt->state) {
        case CALL_STATE_INCOMING:
            fmt = AT_CMD(a);
            break;

        case CALL_STATE_WAITING:
            cmd.cmd = CMD_AT_CHLD_2x;
            fmt     = AT_CMD(chld);
            /* no need CMD_AT_DDSETEX in this case? */
            break;

        default:
            ast_log(LOG_ERROR, "[%s] Request answer for call idx %d with state '%s'\n", PVT_ID(cpvt->pvt), cpvt->call_idx, call_state2str(cpvt->state));
            chan_quectel_err = E_UNKNOWN;
            return -1;
    }

    if (at_fill_generic_cmd(&cmd, fmt, cpvt->call_idx)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue an activate commands 'Put active calls on hold and activate call x.'
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */
int at_enqueue_activate(struct cpvt* cpvt)
{
    DECLARE_AT_CMDNT(chld, "+CHLD=2%d");

    if (cpvt->state == CALL_STATE_ACTIVE) {
        return 0;
    }

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_CHLD_2x);

    if (cpvt->state != CALL_STATE_ONHOLD && cpvt->state != CALL_STATE_WAITING) {
        ast_log(LOG_ERROR, "[%s] Imposible activate call idx %d from state '%s'\n", PVT_ID(cpvt->pvt), cpvt->call_idx, call_state2str(cpvt->state));
        return -1;
    }

    if (at_fill_generic_cmd(&cmd, AT_CMD(chld), cpvt->call_idx)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }
    return 0;
}

/*!
 * \brief Enqueue an commands for 'Put active calls on hold and activate the waiting or held call.'
 * \param pvt -- pvt structure
 * \return 0 on success
 */
int at_enqueue_flip_hold(struct cpvt* cpvt)
{
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CHLD_2, chld2);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

enum at_ping_method_t { PING_AT, PING_QUECTEL, PING_SIMCOM };

/*!
 * \brief Enqueue ping command
 * \param pvt -- pvt structure
 * \return 0 on success
 */
int at_enqueue_ping(struct cpvt* cpvt)
{
    struct pvt* const pvt          = cpvt->pvt;
    enum at_ping_method_t ping_cmd = PING_AT;

    if (CONF_SHARED(pvt, query_time)) {
        ping_cmd = pvt->is_simcom ? PING_SIMCOM : PING_QUECTEL;
    }

    switch (ping_cmd) {
        case PING_AT:
            return at_enqueue_at(cpvt);

        case PING_QUECTEL:
            return at_enqueue_qlts_1(cpvt);

        case PING_SIMCOM:
            return at_enqueue_cclk_query(cpvt);
    }

    return -1;
}

static void at_enqueue_ping_sys_chan(struct pvt* pvt) { at_enqueue_ping(&pvt->sys_chan); }

int at_enqueue_ping_taskproc(void* tpdata) { return PVT_TASKPROC_TRYLOCK_AND_EXECUTE(tpdata, at_enqueue_ping_sys_chan); }

/*!
 * \brief Enqueue user-specified command
 * \param cpvt -- cpvt structure
 * \param input -- user's command
 * \return 0 on success
 */
int at_enqueue_user_cmd(struct cpvt* cpvt, const char* input)
{
    if (at_enqueue_generic(cpvt, CMD_USER, 1, "%s\r", input)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Start reading next SMS, if any
 * \param cpvt -- cpvt structure
 */
void at_sms_retrieved(struct cpvt* cpvt, int confirm)
{
    struct pvt* const pvt = cpvt->pvt;

    if (pvt->incoming_sms_index >= 0) {
        if (CONF_SHARED(pvt, sms_autodelete)) {
            at_enqueue_delete_sms(cpvt, pvt->incoming_sms_index, TRIBOOL_NONE);
        }
        if (confirm) {
            switch (pvt->incoming_sms_type) {
                case RES_CDSI:
                    break;

                default:
                    ast_log(LOG_WARNING, "[%s][SMS:%d] Message not retrieved\n", PVT_ID(pvt), pvt->incoming_sms_index);
                    break;
            }
        }
    }

    pvt->incoming_sms_index = -1;
    pvt->incoming_sms_type  = RES_UNKNOWN;
}

int at_enqueue_list_messages(struct cpvt* cpvt, enum msg_status_t stat)
{
    DECLARE_AT_CMDNT(cmgl, "+CMGL=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_CMGL);

    if (at_fill_generic_cmd(&cmd, AT_CMD(cmgl), (int)stat)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue commands for reading SMS
 * \param cpvt -- cpvt structure
 * \param index -- index of message in store
 * \return 0 on success
 */
int at_enqueue_retrieve_sms(struct cpvt* cpvt, int idx, int sms_type)
{
    DECLARE_AT_CMDNT(cmgr, "+CMGR=%d");

    struct pvt* const pvt = cpvt->pvt;
    at_queue_cmd_t cmd    = ATQ_CMD_DECLARE_DYN(CMD_AT_CMGR);

    /* check if message is already being received */
    if (pvt->incoming_sms_index >= 0) {
        ast_debug(4, "[%s] SMS retrieve of [%d] already in progress\n", PVT_ID(pvt), pvt->incoming_sms_index);
        return 0;
    }

    pvt->incoming_sms_index = idx;
    pvt->incoming_sms_type  = sms_type;

    if (at_fill_generic_cmd(&cmd, AT_CMD(cmgr), idx)) {
        chan_quectel_err = E_CMD_FORMAT;
        goto error;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        goto error;
    }

    return 0;

error:
    ast_log(LOG_WARNING, "[%s] Unable to read message %d\n", PVT_ID(pvt), idx);

    pvt->incoming_sms_index = -1;
    pvt->incoming_sms_type  = RES_UNKNOWN;
    return -1;
}

int at_enqueue_cmgd(struct cpvt* cpvt, unsigned int idx, int delflag)
{
    DECLARE_AT_CMDNT(cmgd, "+CMGD=%u,%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYNI(CMD_AT_CMGD);

    if (at_fill_generic_cmd(&cmd, AT_CMD(cmgd), idx, delflag)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

DECLARE_AT_CMD(cnma, "+CNMA");

/*!
 * \brief Enqueue commands for deleting SMS
 * \param cpvt -- cpvt structure
 * \param index -- index of message in store
 * \return 0 on success
 */
int at_enqueue_delete_sms(struct cpvt* cpvt, int idx, tristate_bool_t ack)
{
    DECLARE_AT_CMDNT(cmgd, "+CMGD=%d");

    at_queue_cmd_t cmds[] = {ATQ_CMD_DECLARE_STI(CMD_AT_CNMA, cnma), ATQ_CMD_DECLARE_DYNI(CMD_AT_CMGD)};

    if (idx < 0) {
        return 0;
    }

    if (at_fill_generic_cmd(&cmds[1], AT_CMD(cmgd), idx)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmds[ack ? 0 : 1], ack ? 2u : 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_delete_sms_n(struct cpvt* cpvt, int idx, tristate_bool_t ack)
{
    DECLARE_AT_CMDNT(cnma, "+CNMA=%d");
    DECLARE_AT_CMDNT(cmgd, "+CMGD=%d");

    at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_DYNI(CMD_AT_CNMA),
        ATQ_CMD_DECLARE_DYNI(CMD_AT_CMGD),
    };

    if (idx < 0) {
        return 0;
    }

    if (ack) {
        if (at_fill_generic_cmd(&cmds[0], AT_CMD(cnma), (ack < 0) ? 2 : 1)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }
    }

    if (at_fill_generic_cmd(&cmds[1], AT_CMD(cmgd), idx)) {
        at_queue_free_data(cmds);
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmds[ack ? 0 : 1], ack ? 2u : 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_msg_direct(struct cpvt* cpvt, int directflag)
{
    DECLARE_AT_CMD(cnmi_direct, "+CNMI=2,2,2,0,0");
    DECLARE_AT_CMD(cnmi_indirect, "+CNMI=2,1,0,2,0");

    static const at_queue_cmd_t cmd_direct   = ATQ_CMD_DECLARE_STI(CMD_AT_CNMI, cnmi_direct);
    static const at_queue_cmd_t cmd_indirect = ATQ_CMD_DECLARE_STI(CMD_AT_CNMI, cnmi_indirect);

    if (at_queue_insert_const(cpvt, directflag ? &cmd_direct : &cmd_indirect, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_msg_ack(struct cpvt* cpvt)
{
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_STI(CMD_AT_CNMA, cnma);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_msg_ack_n(struct cpvt* cpvt, int n, int uid)
{
    DECLARE_AT_CMDNT(cnma, "+CNMA=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYNI(CMD_AT_CNMA);

    if (at_fill_generic_cmd(&cmd, AT_CMD(cnma), n)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert_uid(cpvt, &cmd, 1u, 1, uid)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

#if 0
static int at_enqueue_msg_ack_n(struct cpvt *cpvt, int n, int uid)
{
	static const char ctrl_z[2] = { 0x1A, 0x00 };

	at_queue_cmd_t at_cmds[] = {
		{ CMD_AT_CNMA,    RES_SMS_PROMPT, ATQ_CMD_FLAG_DEFAULT, { ATQ_CMD_TIMEOUT_MEDIUM, 0}, NULL, 0 },
		{ CMD_AT_SMSTEXT, RES_OK,         ATQ_CMD_FLAG_STATIC, { ATQ_CMD_TIMEOUT_LONG, 0},   NULL, 0 }
	};

	if (at_fill_generic_cmd(&at_cmds[0], "AT+CNMA=%d,0\r", n)) {
		chan_quectel_err = E_CMD_FORMAT;
		return err;
	}

	at_cmds[1].length = 1;
	at_cmds[1].data = (void*)ctrl_z;

	if (at_queue_insert_uid(cpvt, at_cmds, ARRAY_LEN(at_cmds), 1, uid)) {
		chan_quectel_err = E_QUEUE;
		return -1;
	}

	return 0;
}
#endif

static int map_hangup_cause(int hangup_cause)
{
    switch (hangup_cause) {
        // list of supported cause codes
        case AST_CAUSE_UNALLOCATED:
        case AST_CAUSE_NORMAL_CLEARING:
        case AST_CAUSE_USER_BUSY:
        case AST_CAUSE_NO_USER_RESPONSE:
        case AST_CAUSE_CALL_REJECTED:
        case AST_CAUSE_DESTINATION_OUT_OF_ORDER:
        case AST_CAUSE_NORMAL_UNSPECIFIED:
        case AST_CAUSE_INCOMPATIBLE_DESTINATION:
            return hangup_cause;

        case AST_CAUSE_NO_ANSWER:
            return AST_CAUSE_NO_USER_RESPONSE;

        default:  // use default one
            return AST_CAUSE_NORMAL_CLEARING;
    }
}

static int at_enqueue_chup(struct cpvt* const cpvt)
{
    DECLARE_AT_CMD(chup, "+CHUP");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_STFT(CMD_AT_CHUP, RES_OK, AT_CMD(chup), ATQ_CMD_FLAG_DEFAULT, ATQ_CMD_TIMEOUT_LONG, 0);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

static int at_enqueue_qhup(struct cpvt* const cpvt, int call_idx, int release_cause)
{
    // AT+QHUP=<cause>,<idx>
    DECLARE_AT_CMDNT(qhup, "+QHUP=%d,%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYNFT(CMD_AT_QHUP, RES_OK, ATQ_CMD_FLAG_DEFAULT, ATQ_CMD_TIMEOUT_LONG, 0);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qhup), map_hangup_cause(release_cause), call_idx)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue AT+CHLD1x or AT+CHUP hangup command
 * \param cpvt -- channel_pvt structure
 * \param call_idx -- call id
 * \return 0 on success
 */

int at_enqueue_hangup(struct cpvt* cpvt, int call_idx, int release_cause)
{
    struct pvt* const pvt = cpvt->pvt;

    if (cpvt == &pvt->sys_chan || CPVT_DIR_INCOMING(cpvt) || (cpvt->state != CALL_STATE_INIT && cpvt->state != CALL_STATE_DIALING)) {
        /* FIXME: other channels may be in RELEASED or INIT state */
        if (ast_atomic_fetch_uint32(&PVT_STATE(pvt, chansno)) > 1) {
            DECLARE_AT_CMD(chld1x, "+CHLD=1%d");
            static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_STFT(CMD_AT_CHLD_1x, RES_OK, AT_CMD(chld1x), ATQ_CMD_FLAG_DEFAULT, ATQ_CMD_TIMEOUT_LONG, 0);

            if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
                chan_quectel_err = E_QUEUE;
                return -1;
            }

            return 0;
        }
    }

    if (pvt->is_simcom) {  // AT+CHUP
        return at_enqueue_chup(cpvt);
    } else {
        return (CONF_SHARED(pvt, qhup)) ? at_enqueue_qhup(cpvt, call_idx, release_cause) : at_enqueue_chup(cpvt);
    }
}

/*!
 * \brief Enqueue AT+CLVL commands for volume synchronization
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */

int at_enqueue_volsync(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(cmd1, "+CLVL=1");
    DECLARE_AT_CMD(cmd2, "+CLVL=5");

    static const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT_CLVL, cmd1),
        ATQ_CMD_DECLARE_ST(CMD_AT_CLVL, cmd2),
    };

    if (at_queue_insert_const(cpvt, cmds, ARRAY_LEN(cmds), 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue AT+CLCC command
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */
int at_enqueue_clcc(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(clcc, "+CLCC");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CLCC, clcc);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief Enqueue AT+CHLD=3 command
 * \param cpvt -- cpvt structure
 * \return 0 on success
 */
int at_enqueue_conference(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(chld3, "+CHLD=3");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CHLD_3, chld3);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

/*!
 * \brief SEND AT+CHUP command to device IMMEDIALITY
 * \param cpvt -- cpvt structure
 */
int at_hangup_immediately(struct cpvt* cpvt, int release_cause)
{
    DECLARE_NAKED_AT_CMD(chup, "+CHUP");
    DECLARE_NAKED_AT_CMDNT(qhup, "+QHUP=%d,%d");

    struct pvt* const pvt = cpvt->pvt;

    if (pvt->is_simcom) {  // AT+CHUP
        static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CHUP, chup);

        if (at_queue_add(cpvt, &cmd, 1u, 0, 1u) == NULL) {
            chan_quectel_err = E_QUEUE;
            return -1;
        }
    } else {  // AT+QHUP=<cause>,<idx>
        at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QHUP);

        if (at_fill_generic_cmd(&cmd, AT_CMD(qhup), map_hangup_cause(release_cause), cpvt->call_idx)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }

        if (at_queue_add(cpvt, &cmd, 1u, 0, 1u) == NULL) {
            chan_quectel_err = E_QUEUE;
            return -1;
        }
    }

    return 0;
}

int at_disable_uac_immediately(struct pvt* pvt)
{
    DECLARE_NAKED_AT_CMD(qpcmv, "+QPCMV=0");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QPCMV_0, qpcmv);

    if (at_queue_add(&pvt->sys_chan, &cmd, 1u, 0, 1u) == NULL) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_mute(struct cpvt* cpvt, int mute)
{
    DECLARE_AT_CMD(cmut0, "+CMUT=0");
    DECLARE_AT_CMD(cmut1, "+CMUT=1");

    static const at_queue_cmd_t cmut0 = ATQ_CMD_DECLARE_ST(CMD_AT_CMUT_0, cmut0);
    static const at_queue_cmd_t cmut1 = ATQ_CMD_DECLARE_ST(CMD_AT_CMUT_1, cmut1);

    const at_queue_cmd_t* cmds = mute ? &cmut1 : &cmut0;

    if (at_queue_insert_const(cpvt, cmds, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_enable_tty(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qpcmv, "+QPCMV=1,0");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QPCMV_TTY, qpcmv);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_enable_uac(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qpcmv, "+QPCMV=1,2");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QPCMV_UAC, qpcmv);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_uac_apply(struct cpvt* cpvt)
{
    DECLARE_NAKED_AT_CMD(qpcmv0, "+QPCMV=0");
    DECLARE_NAKED_AT_CMD(cfun, "+CFUN=1,1");

    static const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT_QPCMV_0, qpcmv0),
        ATQ_CMD_DECLARE_ST(CMD_AT_QRXGAIN, cfun),
    };

    if (at_queue_insert_const_at_once(cpvt, cmds, ARRAY_LEN(cmds), 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qlts(struct cpvt* cpvt, int mode)
{
    DECLARE_AT_CMDNT(qlts, "+QLTS=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QLTS);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qlts), mode)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qlts_1(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qlts, "+QLTS=1");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QLTS_1, qlts);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_cclk_query(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(cclk, "+CCLK?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CCLK, cclk);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_qgains(struct cpvt* cpvt)
{
    DECLARE_NAKED_AT_CMD(qmic, "+QMIC?");
    DECLARE_NAKED_AT_CMD(qrxgain, "+QRXGAIN?");

    static const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT_QMIC, qmic),
        ATQ_CMD_DECLARE_ST(CMD_AT_QRXGAIN, qrxgain),
    };

    if (at_queue_insert_const_at_once(cpvt, cmds, ARRAY_LEN(cmds), 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qgains(struct cpvt* cpvt, int txgain, int rxgain)
{
    DECLARE_NAKED_AT_CMDNT(qmic, "+QMIC=%d");
    DECLARE_NAKED_AT_CMDNT(qrxgain, "+QRXGAIN=%d");

    int pos               = 0;
    unsigned int cnt      = 0;
    at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_DYN(CMD_AT_QMIC),
        ATQ_CMD_DECLARE_DYN(CMD_AT_QRXGAIN),
    };

    if (txgain >= 0) {
        if (at_fill_generic_cmd(&cmds[0], AT_CMD(qmic), txgain)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }

        cnt += 1;
    } else {
        pos += 1;
    }

    if (rxgain >= 0) {
        if (at_fill_generic_cmd(&cmds[1], AT_CMD(qrxgain), rxgain)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }

        cnt += 1;
    }

    if (at_queue_add(cpvt, &cmds[pos], cnt, 0, 1u) == NULL) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_cgains(struct cpvt* cpvt)
{
    DECLARE_NAKED_AT_CMD(coutgain, "+COUTGAIN?");
    DECLARE_NAKED_AT_CMD(cmicgain, "+CMICGAIN?");

    static const at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_ST(CMD_AT_COUTGAIN, coutgain),
        ATQ_CMD_DECLARE_ST(CMD_AT_CMICGAIN, cmicgain),
    };

    if (at_queue_insert_const_at_once(cpvt, cmds, ARRAY_LEN(cmds), 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_cgains(struct cpvt* cpvt, int txgain, int rxgain)
{
    DECLARE_NAKED_AT_CMDNT(coutgain, "+COUTGAIN=%d");
    DECLARE_NAKED_AT_CMDNT(cmicgain, "+CMICGAIN=%d");

    int pos               = 0;
    unsigned int cnt      = 0;
    at_queue_cmd_t cmds[] = {
        ATQ_CMD_DECLARE_DYN(CMD_AT_COUTGAIN),
        ATQ_CMD_DECLARE_DYN(CMD_AT_CMICGAIN),
    };

    if (txgain >= 0) {
        if (at_fill_generic_cmd(&cmds[0], AT_CMD(coutgain), txgain)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }

        cnt += 1;
    } else {
        pos += 1;
    }

    if (rxgain >= 0) {
        if (at_fill_generic_cmd(&cmds[1], AT_CMD(cmicgain), rxgain)) {
            chan_quectel_err = E_CMD_FORMAT;
            return -1;
        }

        cnt += 1;
    }

    if (at_queue_add(cpvt, &cmds[pos], cnt, 0, 1u) == NULL) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_qaudloop(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qaudloop, "+QAUDLOOP?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QAUDLOOP, qaudloop);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qaudloop(struct cpvt* cpvt, int aloop)
{
    DECLARE_AT_CMDNT(qaudloop, "+QAUDLOOP=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QAUDLOOP);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qaudloop), aloop)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_qaudmod(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qaudmod, "+QAUDMOD?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QAUDMOD, qaudmod);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qaudmod(struct cpvt* cpvt, int amode)
{
    DECLARE_AT_CMDNT(qaudmod, "+QAUDMOD=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QAUDMOD);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qaudmod), amode)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_qmic(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qmic, "+QMIC?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QMIC, qmic);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qmic(struct cpvt* cpvt, int gain)
{
    DECLARE_AT_CMDNT(qmic, "+QMIC=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QMIC);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qmic), gain)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_cmicgain(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(cmicgain, "+CMICGAIN?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_CMICGAIN, cmicgain);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_cmicgain(struct cpvt* cpvt, int gain)
{
    DECLARE_AT_CMDNT(cmicgain, "+CMICGAIN=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_CMICGAIN);

    if (at_fill_generic_cmd(&cmd, AT_CMD(cmicgain), gain)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_qrxgain(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(qrxgain, "+QRXGAIN?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QRXGAIN, qrxgain);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_qrxgain(struct cpvt* cpvt, int gain)
{
    DECLARE_AT_CMDNT(qrxgain, "+QRXGAIN=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_QRXGAIN);

    if (at_fill_generic_cmd(&cmd, AT_CMD(qrxgain), gain)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_query_coutgain(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(coutgain, "+COUTGAIN?");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_AT_QRXGAIN, coutgain);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_coutgain(struct cpvt* cpvt, int gain)
{
    DECLARE_AT_CMDNT(coutgain, "+COUTGAIN=%d");

    at_queue_cmd_t cmd = ATQ_CMD_DECLARE_DYN(CMD_AT_COUTGAIN);

    if (at_fill_generic_cmd(&cmd, AT_CMD(coutgain), gain)) {
        chan_quectel_err = E_CMD_FORMAT;
        return -1;
    }

    if (at_queue_insert(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_cpcmreg(struct cpvt* cpvt, int reg)
{
    DECLARE_AT_CMD(cpcmreg_0, "+CPCMREG=0");
    DECLARE_AT_CMD(cpcmreg_1, "+CPCMREG=1");

    static const at_queue_cmd_t cmd0 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMREG0, cpcmreg_0);
    static const at_queue_cmd_t cmd1 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMREG1, cpcmreg_1);

    if (at_queue_insert_const(cpvt, reg ? &cmd1 : &cmd0, 1u, 1)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_cpcmreg_immediately(struct pvt* pvt, int reg)
{
    DECLARE_NAKED_AT_CMD(cpcmreg_0, "+CPCMREG=0,0");
    DECLARE_NAKED_AT_CMD(cpcmreg_1, "+CPCMREG=1");

    static const at_queue_cmd_t cmd0 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMREG0, cpcmreg_0);
    static const at_queue_cmd_t cmd1 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMREG1, cpcmreg_1);

    if (at_queue_add(&pvt->sys_chan, reg ? &cmd1 : &cmd0, 1u, 0, 1u) == NULL) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_cpcmfrm(struct cpvt* cpvt, int frm)
{
    DECLARE_AT_CMD(cpcmfrm_0, "+CPCMFRM=0");
    DECLARE_AT_CMD(cpcmfrm_1, "+CPCMFRM=1");

    static const at_queue_cmd_t cmd0 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMFRM_8K, cpcmfrm_0);
    static const at_queue_cmd_t cmd1 = ATQ_CMD_DECLARE_ST(CMD_AT_CPCMFRM_16K, cpcmfrm_1);

    if (at_queue_insert_const(cpvt, frm ? &cmd1 : &cmd0, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_csq(struct cpvt* cpvt)
{
    DECLARE_AT_CMD(csq, "+CSQ");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_STI(CMD_AT_CSQ, csq);

    if (at_queue_insert_const(cpvt, &cmd, 1u, 0)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}

int at_enqueue_escape(struct cpvt* cpvt, int uid)
{
    DECLARE_NAKED_AT_CMD(esc, "\x1B");
    static const at_queue_cmd_t cmd = ATQ_CMD_DECLARE_ST(CMD_ESC, esc);

    if (at_queue_insert_uid(cpvt, (at_queue_cmd_t*)&cmd, 1u, 1, uid)) {
        chan_quectel_err = E_QUEUE;
        return -1;
    }

    return 0;
}
