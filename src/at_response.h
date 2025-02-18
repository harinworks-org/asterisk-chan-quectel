/*
   Copyright (C) 2010 bg <bg_one@mail.ru>
*/
#ifndef CHAN_QUECTEL_AT_RESPONSE_H_INCLUDED
#define CHAN_QUECTEL_AT_RESPONSE_H_INCLUDED

struct pvt;
struct iovec;

#include "chan_quectel.h"

/* AT_RESPONSES_TABLE */
#define AT_RES_AS_ENUM(res, desc, str) RES_##res,
#define AT_RES_AS_STRUCTLIST(res, desc, str) {RES_##res, desc, str, (sizeof(str) - 1)},

#define AT_RESPONSES_TABLE(_)                          \
    _(PARSE_ERROR, "PARSE ERROR", "")                  \
    _(UNKNOWN, "UNK", "")                              \
    _(TIMEOUT, "TIMEOUT", "")                          \
                                                       \
    _(BOOT, "^BOOT", "^BOOT:")                         \
    _(BUSY, "BUSY", "BUSY\r")                          \
    _(COPS, "+COPS", "+COPS:")                         \
                                                       \
    _(CMGR, "+CMGR", "+CMGR:")                         \
    _(CMGL, "+CMGL", "+CMGL:")                         \
    _(CMS_ERROR, "+CMS ERROR", "+CMS ERROR:")          \
    _(CMTI, "+CMTI", "+CMTI:")                         \
    _(CMT, "+CMT", "+CMT:")                            \
    _(CDSI, "+CDSI", "+CDSI:")                         \
    _(CDS, "+CDS", "+CDS:")                            \
    _(CBM, "+CBM", "+CBM:")                            \
    _(CLASS0, "+CLASS0", "+CLASS0:")                   \
                                                       \
    _(CNUM, "+CNUM", "+CNUM:")                         \
    /* and "ERROR+CNUM:", hacked later on */           \
                                                       \
    _(DSCI, "^DSCI", "^DSCI:")                         \
    _(VOICE_CALL, "VOICE CALL", "VOICE CALL:")         \
    _(CPIN, "+CPIN", "+CPIN:")                         \
                                                       \
    _(CREG, "+CREG", "+CREG:")                         \
    _(CEREG, "+CEREG", "+CEREG:")                      \
    _(CSQ, "+CSQ", "+CSQ:")                            \
    _(CSQN, "+CSQN", "+CSQN:")                         \
    _(CSSI, "+CSSI", "+CSSI:")                         \
    _(CSSU, "+CSSU", "+CSSU:")                         \
                                                       \
    _(CUSD, "+CUSD", "+CUSD:")                         \
    _(ERROR, "ERROR", "ERROR\r")                       \
    /* and "COMMAND NOT SUPPORT\r", hacked later on */ \
                                                       \
    _(QIND, "+QIND", "+QIND:")                         \
    _(NO_CARRIER, "NO CARRIER", "NO CARRIER\r")        \
                                                       \
    _(NO_DIALTONE, "NO DIALTONE", "NO DIALTONE\r")     \
    _(NO_ANSWER, "NO ANSWER", "NO ANSWER\r")           \
    _(OK, "OK", "OK\r")                                \
    _(MISSED_CALL, "MISSED CALL", "MISSED_CALL:")      \
    _(RING, "RING", "RING\r")                          \
    _(RING_DS, "RING DS", "RING DS\r")                 \
    _(RING0, "RING##0", "RING##0\r")                   \
    _(RING1, "RING##1", "RING##1\r")                   \
                                                       \
    _(SMMEMFULL, "^SMMEMFULL", "^SMMEMFULL:")          \
    _(SMS_PROMPT, "> ", "> ")                          \
    _(SRVST, "^SRVST", "^SRVST:")                      \
                                                       \
    _(CVOICE, "^CVOICE", "^CVOICE:")                   \
    _(CSMS, "+CSMS", "+CSMS:")                         \
    _(CMGS, "+CMGS", "+CMGS:")                         \
    _(CPMS, "+CPMS", "+CPMS:")                         \
    _(CSCA, "+CSCA", "+CSCA:")                         \
                                                       \
    _(CLCC, "+CLCC", "+CLCC:")                         \
    _(RCEND, "CALLEND", "REMOTE CALL END")             \
    _(CCWA, "+CCWA", "+CCWA:")                         \
    _(QSPN, "+QSPN", "+QSPN:")                         \
    _(CSPN, "+CSPN", "+CSPN:")                         \
    _(QNWINFO, "+QNWINFO", "+QNWINFO:")                \
    _(QTONEDET, "+QTONEDET", "+QTONEDET:")             \
    _(RXDTMF, "+RXDTMF", "+RXDTMF:")                   \
    _(DTMF, "+DTMF", "+DTMF:")                         \
    _(QAUDLOOP, "+QAUDLOOP", "+QAUDLOOP:")             \
    _(QAUDMOD, "+QAUDMOD", "+QAUDMOD:")                \
    _(QPCMV, "+QPCMV", "+QPCMV:")                      \
    _(QLTS, "+QLTS", "+QLTS:")                         \
    _(CCLK, "+CCLK", "+CCLK:")                         \
    _(QMIC, "+QMIC", "+QMIC:")                         \
    _(QRXGAIN, "+QRXGAIN", "+QRXGAIN:")                \
    _(CMICGAIN, "+CMICGAIN", "+CMICGAIN:")             \
    _(COUTGAIN, "+COUTGAIN", "+COUTGAIN:")             \
    _(CTXVOL, "+CTXVOL", "+CTXVOL:")                   \
    _(CRXVOL, "+CRXVOL", "+CRXVOL:")                   \
    _(CGMR, "+CGMR", "+CGMR:")                         \
    _(CPCMREG, "+CPCMREG", "+CPCMREG:")                \
    _(CNSMOD, "+CNSMOD", "+CNSMOD:")                   \
    _(CRING, "+CRING", "+CRING:")                      \
    _(PSNWID, "*PSNWID", "*PSNWID:")                   \
    _(PSUTTZ, "*PSUTTZ", "*PSUTTZ:")                   \
    _(DST, "DST", "DST:")                              \
    _(REVISION, "Revision", "Revision:")               \
    _(ICCID, "+ICCID", "+ICCID:")                      \
    _(QCCID, "+QCCID", "+QCCID:")                      \
    _(CIEV, "+CIEV", "+CIEV:")

/* AT_RESPONSES_TABLE */

typedef enum {

    /* Hackish way to force RES_PARSE_ERROR = -1 for compatibility */
    COMPATIBILITY_RES_START_AT_MINUSONE = -2,

    AT_RESPONSES_TABLE(AT_RES_AS_ENUM)

    /* Hackish way to maintain MAX and MIN responses for compatibility */
    RES_MIN = RES_PARSE_ERROR,
    RES_MAX = RES_CIEV,
} at_res_t;

/*! response description */
typedef struct at_response_t {
    at_res_t res;
    const char* name;
    const char* id;
    unsigned idlen;
} at_response_t;

/*! responses control */
typedef struct at_responses_t {
    const at_response_t* responses;
    unsigned ids_first; /*!< index of first id */
    unsigned ids;       /*!< number of ids */
    int name_first;     /*!< value of enum for first name */
    int name_last;      /*!< value of enum for last name */
} at_responses_t;

/*! responses description */
extern const at_responses_t at_responses;
const char* at_res2str(at_res_t res);
at_res_t at_str2res(const struct ast_str* const);

int at_response(struct pvt* const pvt, const struct ast_str* const response, const at_res_t at_res);

typedef struct at_response_taskproc_data {
    struct pvt_taskproc_data ptd;
    struct ast_str response;
} at_response_taskproc_data_t;

struct at_response_taskproc_data* at_response_taskproc_data_alloc(struct pvt* const pvt, const struct ast_str* const response);
int at_response_taskproc(void* tpdata);

#endif /* CHAN_QUECTEL_AT_RESPONSE_H_INCLUDED */
