/*
   Copyright (C) 2010 bg <bg_one@mail.ru>
*/
#include <asterisk/callerid.h> /* ast_parse_caller_presentation() */

#include "dc_config.h"

#include "helpers.h"

static const char DEFAULT_ALSADEV[]       = "hw:Android";
static const char DEFAULT_ALSADEV_EXT[]   = "hw:0";
static const int DEFAULT_MANAGER_INTERVAL = 15;
static const char DEFAULT_SMS_DB[]        = ":memory:";
static const char DEFAULT_SMS_BACKUP_DB[] = "/var/lib/asterisk/smsdb-backup";
static const int DEFAULT_SMS_TTL          = 600;

const static long DEF_DTMF_DURATION = 120;

const char* dc_cw_setting2str(call_waiting_t cw)
{
    static const char* const options[] = {"disabled", "allowed", "auto"};
    return enum2str(cw, options, ARRAY_LEN(options));
}

tristate_bool_t dc_str23stbool(const char* str)
{
    if (!str) {
        return TRIBOOL_NONE;
    }

    if (!strcasecmp(str, "on") || !strcasecmp(str, "true") || !strcasecmp(str, "yes")) {
        return TRIBOOL_TRUE;
    } else if (!strcasecmp(str, "off") || !strcasecmp(str, "false") || !strcasecmp(str, "no")) {
        return TRIBOOL_FALSE;
    } else {
        return TRIBOOL_NONE;
    }
}

int dc_str23stbool_ex(const char* str, tristate_bool_t* res, const char* none_val)
{
    if (!str) {
        return -1;
    }
    if (!res) {
        return -2;
    }
    if (!none_val) {
        return -3;
    }

    if (!strcasecmp(str, "on") || !strcasecmp(str, "true") || !strcasecmp(str, "yes")) {
        *res = TRIBOOL_TRUE;
        return 0;
    } else if (!strcasecmp(str, "off") || !strcasecmp(str, "false") || !strcasecmp(str, "no")) {
        *res = TRIBOOL_FALSE;
        return 0;
    } else if (!strcasecmp(str, none_val)) {
        *res = TRIBOOL_NONE;
        return 0;
    } else {
        return 1;
    }
}

static unsigned int int23statebool(int v)
{
    if (!v) {
        return 1;
    } else {
        return (v < 0) ? 0 : 2;
    }
}

const char* dc_3stbool2str(int v)
{
    static const char* const strs[] = {"off", "none", "on"};

    const unsigned b = int23statebool(v);
    return enum2str_def(b, strs, ARRAY_LEN(strs), "none");
}

const char* dc_3stbool2str_ex(int v, const char* none_val)
{
    const char* const strs[] = {"off", S_OR(none_val, "none"), "on"};

    const unsigned b = int23statebool(v);
    return enum2str_def(b, strs, ARRAY_LEN(strs), S_OR(none_val, "none"));
}

const char* dc_3stbool2str_capitalized(int v)
{
    static const char* const strs[] = {"Off", "None", "On"};

    unsigned b = int23statebool(v);
    return enum2str_def(b, strs, ARRAY_LEN(strs), "None");
}

static unsigned int parse_on_off(const char* const name, const char* const value, unsigned int defval)
{
    if (!value) {
        return defval;
    }
    if (ast_true(value)) {
        return 1u;
    }
    if (ast_false(value)) {
        return 0u;
    }

    ast_log(LOG_ERROR, "Invalid value '%s' for configuration option '%s'\n", value, name);
    return defval;
}

static const char* const msgstor_strs[] = {"AUTO", "SM", "ME", "MT", "SR"};

message_storage_t dc_str2msgstor(const char* stor)
{
    const int res = str2enum(stor, msgstor_strs, ARRAY_LEN(msgstor_strs));
    if (res < 0) {
        return MESSAGE_STORAGE_AUTO;
    }
    return (message_storage_t)res;
}

const char* dc_msgstor2str(message_storage_t stor) { return enum2str_def(stor, msgstor_strs, ARRAY_LEN(msgstor_strs), "AUTO"); }

#/* assume config is zerofill */

static int dc_uconfig_fill(struct ast_config* cfg, const char* cat, struct dc_uconfig* config)
{
    tristate_bool_t uac = TRIBOOL_FALSE;
    int slin16          = 0;

    const char* const audio_tty  = ast_variable_retrieve(cfg, cat, "audio");
    const char* const data_tty   = ast_variable_retrieve(cfg, cat, "data");
    const char* const alsadev    = ast_variable_retrieve(cfg, cat, "alsadev");
    const char* const uac_str    = ast_variable_retrieve(cfg, cat, "uac");
    const char* const slin16_str = ast_variable_retrieve(cfg, cat, "slin16");

    if (uac_str) {
        if (dc_str23stbool_ex(uac_str, &uac, "ext")) {
            ast_log(LOG_WARNING, "[%s] Ignore invalid value of UAC mode '%s'\n", cat, uac_str);
            uac = TRIBOOL_FALSE;
        }
    }

    if (slin16_str) {
        slin16 = parse_on_off("slin16", slin16_str, 0u);
    }

    if (!data_tty) {
        ast_log(LOG_ERROR, "Skipping device %s. Missing required data_tty setting\n", cat);
        return 1;
    }

    ast_copy_string(config->id, cat, sizeof(config->id));
    ast_copy_string(config->data_tty, S_OR(data_tty, ""), sizeof(config->data_tty));
    ast_copy_string(config->audio_tty, S_OR(audio_tty, ""), sizeof(config->audio_tty));
    config->uac = uac;
    switch (uac) {
        case TRIBOOL_FALSE:
            ast_copy_string(config->alsadev, S_OR(alsadev, ""), sizeof(config->alsadev));
            break;

        case TRIBOOL_NONE:
            ast_copy_string(config->alsadev, S_OR(alsadev, DEFAULT_ALSADEV_EXT), sizeof(config->alsadev));
            break;

        case TRIBOOL_TRUE:
            ast_copy_string(config->alsadev, S_OR(alsadev, DEFAULT_ALSADEV), sizeof(config->alsadev));
            break;
    }
    config->slin16 = (unsigned int)slin16;

    return 0;
}

#/* */

void dc_sconfig_fill_defaults(struct dc_sconfig* config)
{
    /* first set default values */
    memset(config, 0, sizeof(*config));

    ast_copy_string(config->context, "default", sizeof(config->context));
    ast_copy_string(config->exten, "", sizeof(config->exten));
    ast_copy_string(config->language, DEFAULT_LANGUAGE, sizeof(config->language));

    config->reset_modem   = 1;
    config->calling_pres  = -1;
    config->init_state    = DEV_STATE_STARTED;
    config->call_waiting  = CALL_WAITING_AUTO;
    config->moh           = 1;
    config->rxgain        = -1;
    config->txgain        = -1;
    config->msg_service   = -1;
    config->dtmf_duration = DEF_DTMF_DURATION;
    config->qhup          = 1u;
}

#/* */

void dc_sconfig_fill(struct ast_config* cfg, const char* cat, struct dc_sconfig* config)
{
    struct ast_variable* v;

    /*  read config and translate to values */
    for (v = ast_variable_browse(cfg, cat); v; v = v->next) {
        if (!strcasecmp(v->name, "context")) {
            ast_copy_string(config->context, v->value, sizeof(config->context));
        } else if (!strcasecmp(v->name, "exten")) {
            ast_copy_string(config->exten, v->value, sizeof(config->exten));
        } else if (!strcasecmp(v->name, "always_use_exten")) {
            config->always_use_exten = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "language")) {
            ast_copy_string(config->language, v->value, sizeof(config->language)); /* set channel language */
        } else if (!strcasecmp(v->name, "group")) {
            config->group = (int)strtol(v->value, (char**)NULL, 10); /* group is set to 0 if invalid */
        } else if (!strcasecmp(v->name, "rxgain")) {
            if (str2gain(v->value, &config->rxgain)) {
                config->rxgain = -1;
            }
        } else if (!strcasecmp(v->name, "txgain")) {
            if (str2gain(v->value, &config->txgain)) {
                config->txgain = -1;
            }
        } else if (!strcasecmp(v->name, "callingpres")) {
            config->calling_pres = ast_parse_caller_presentation(v->value);
            if (config->calling_pres == -1) {
                errno                = 0;
                config->calling_pres = (int)strtol(v->value, (char**)NULL, 10); /* callingpres is set to -1 if invalid */
                if (!config->calling_pres && errno == EINVAL) {
                    config->calling_pres = -1;
                }
            }
        } else if (!strcasecmp(v->name, "usecallingpres")) {
            config->use_calling_pres = parse_on_off(v->name, v->value, 0u); /* usecallingpres is set to 0 if invalid */
        } else if (!strcasecmp(v->name, "autodeletesms")) {
            config->sms_autodelete = parse_on_off(v->name, v->value, 0u); /* autodeletesms is set to 0 if invalid */
        } else if (!strcasecmp(v->name, "resetmodem")) {
            config->reset_modem = parse_on_off(v->name, v->value, 0u); /* resetmodem is set to 0 if invalid */
        } else if (!strcasecmp(v->name, "disable")) {
            const unsigned int is = parse_on_off(v->name, v->value, 0u);
            config->init_state    = is ? DEV_STATE_REMOVED : DEV_STATE_STARTED;
        } else if (!strcasecmp(v->name, "initstate")) {
            const dev_state_t val = str2dev_state(v->value);
            if (val == DEV_STATE_STOPPED || val == DEV_STATE_STARTED || val == DEV_STATE_REMOVED) {
                config->init_state = val;
            } else {
                ast_log(LOG_ERROR, "Invalid value for 'initstate': '%s', must be one of 'stop' 'start' 'remove' default is 'start'\n", v->value);
            }
        } else if (!strcasecmp(v->name, "callwaiting")) {
            if (strcasecmp(v->value, "auto")) {
                config->call_waiting = parse_on_off(v->name, v->value, 0u);
            }
        } else if (!strcasecmp(v->name, "multiparty")) {
            config->multiparty = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "dtmf")) {
            config->dtmf = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "dtmf_duration")) {
            config->dtmf_duration = strtol(v->value, (char**)NULL, 10);
            if (config->dtmf_duration <= 0 && errno == EINVAL) {
                config->dtmf_duration = DEF_DTMF_DURATION;
            }
        } else if (!strcasecmp(v->name, "moh")) {
            config->moh = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "query_time")) {
            config->query_time = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "dsci")) {
            config->dsci = parse_on_off(v->name, v->value, 0u);
        } else if (!strcasecmp(v->name, "qhup")) {
            config->qhup = parse_on_off(v->name, v->value, 1u);
        } else if (!strcasecmp(v->name, "msg_direct")) {
            config->msg_direct = dc_str23stbool(v->value);
        } else if (!strcasecmp(v->name, "msg_storage")) {
            config->msg_storage = dc_str2msgstor(v->value);
        } else if (!strcasecmp(v->name, "msg_service")) {
            config->msg_service = (int)strtol(v->value, (char**)NULL, 10);
        }
    }
}

#/* */

void dc_gconfig_fill(struct ast_config* cfg, const char* cat, struct dc_gconfig* config)
{
    config->manager_interval = DEFAULT_MANAGER_INTERVAL;
    ast_copy_string(config->sms_db, DEFAULT_SMS_DB, sizeof(config->sms_db));
    ast_copy_string(config->sms_backup_db, DEFAULT_SMS_BACKUP_DB, sizeof(config->sms_backup_db));
    config->sms_ttl = DEFAULT_SMS_TTL;

    const char* const stmp = ast_variable_retrieve(cfg, cat, "interval");
    if (stmp) {
        errno         = 0;
        const int tmp = (int)strtol(stmp, (char**)NULL, 10);
        if (!tmp && errno == EINVAL) {
            ast_log(LOG_NOTICE, "Error parsing 'interval' in general section, using default value %d\n", config->manager_interval);
        } else {
            config->manager_interval = tmp;
        }
    }

    const char* const smsdb = ast_variable_retrieve(cfg, cat, "smsdb");
    if (smsdb) {
        ast_copy_string(config->sms_db, smsdb, sizeof(config->sms_db));
    }

    const char* const smsdb_backup = ast_variable_retrieve(cfg, cat, "smsdb_backup");
    if (smsdb_backup) {
        ast_copy_string(config->sms_backup_db, smsdb_backup, sizeof(config->sms_backup_db));
    }

    const char* const smsttl = ast_variable_retrieve(cfg, cat, "smsttl");
    if (smsttl) {
        errno          = 0;
        const long tmp = strtol(smsttl, (char**)NULL, 10);
        if (!tmp && errno == EINVAL) {
            ast_log(LOG_NOTICE, "Error parsing 'smsttl' in general section, using default value %d\n", config->sms_ttl);
        } else {
            config->sms_ttl = tmp;
        }
    }
}

#/* */

int dc_config_fill(struct ast_config* cfg, const char* cat, const struct dc_sconfig* parent, struct pvt_config* config)
{
    /* try set unique first */
    int err = dc_uconfig_fill(cfg, cat, &config->unique);
    if (!err) {
        /* inherit from parent */
        config->shared = *parent;

        /* overwrite local */
        dc_sconfig_fill(cfg, cat, &config->shared);
    }

    return err;
}

static int dc_sconfig_compare(const struct dc_sconfig* const cfg1, const struct dc_sconfig* const cfg2)
{
    return strcmp(cfg1->context, cfg2->context) || strcmp(cfg1->exten, cfg2->exten) || strcmp(cfg1->language, cfg2->language) || cfg1->group != cfg2->group ||
           cfg1->rxgain != cfg2->rxgain || cfg1->txgain != cfg2->txgain || cfg1->calling_pres != cfg2->calling_pres ||
           cfg1->use_calling_pres != cfg2->use_calling_pres || cfg1->sms_autodelete != cfg2->sms_autodelete || cfg1->reset_modem != cfg2->reset_modem ||
           cfg1->multiparty != cfg2->multiparty || cfg1->dtmf != cfg2->dtmf || cfg1->moh != cfg2->moh || cfg1->query_time != cfg2->query_time ||
           cfg1->dsci != cfg2->dsci || cfg1->qhup != cfg2->qhup || cfg1->always_use_exten != cfg2->always_use_exten ||
           cfg1->dtmf_duration != cfg2->dtmf_duration || cfg1->init_state != cfg2->init_state || cfg1->call_waiting != cfg2->call_waiting ||
           cfg1->msg_service != cfg2->msg_service || cfg1->msg_direct != cfg2->msg_direct || cfg1->msg_storage != cfg2->msg_storage;
}

static int dc_uconfig_compare(const struct dc_uconfig* const cfg1, const struct dc_uconfig* const cfg2)
{
    return strcmp(cfg1->id, cfg2->id) || strcmp(cfg1->audio_tty, cfg2->audio_tty) || strcmp(cfg1->data_tty, cfg2->data_tty) ||
           strcmp(cfg1->alsadev, cfg2->alsadev) || cfg1->uac != cfg2->uac || cfg1->slin16 != cfg2->slin16;
}

int pvt_config_compare(const struct pvt_config* const cfg1, const struct pvt_config* const cfg2)
{
    if (!(cfg1 && cfg2)) {
        return -1;
    }
    return dc_sconfig_compare(&cfg1->shared, &cfg2->shared) || dc_uconfig_compare(&cfg1->unique, &cfg2->unique);
}
