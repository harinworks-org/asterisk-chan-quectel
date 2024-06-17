/*
    msg_tech.c
*/

#include "ast_config.h"

#include <asterisk/app.h> /* AST_DECLARE_APP_ARGS() ... */
#include <asterisk/json.h>
#include <asterisk/message.h>
#include <asterisk/module.h> /* ast_register_application2() ast_unregister_application() */
#include <asterisk/pbx.h>    /* pbx_builtin_setvar_helper() */
#include <asterisk/strings.h>

#include "msg_tech.h" /* app_register() app_unregister() */

#include "chan_quectel.h" /* struct pvt */
#include "error.h"
#include "helpers.h" /* send_sms() ITEMS_OF() */

static int parse_validity(const char* const arg)
{
    static const int DEF_VALIDITY = 15;

    if (ast_strlen_zero(arg)) {
        return DEF_VALIDITY;
    }

    int validity;
    const int res = sscanf(arg, "%d", &validity);

    return res && (validity > 0) ? validity : DEF_VALIDITY;
}

static int parse_report_flag(const char* arg)
{
    static const int DEF_REPORT_FLAG = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_REPORT_FLAG;
    }

    if (ast_true(arg)) {
        return 1;
    }

    if (ast_false(arg)) {
        return 0;
    }

    return DEF_REPORT_FLAG;
}

static void parse_msg_vars(const struct ast_msg* msg, int* validity, int* report)
{
    RAII_VAR(struct ast_msg_var_iterator*, iter, ast_msg_var_iterator_init(msg), ast_msg_var_iterator_destroy);
    const char* var_name;
    const char* var_value;

    while (ast_msg_var_iterator_next(msg, iter, &var_name, &var_value)) {
        ast_verb(1, "MSGVAR: %s = [%s]\n", var_name, var_value);
        if (!strcasecmp(var_name, "validity")) {
            *validity = parse_validity(var_value);
        } else if (strcasecmp(var_name, "report")) {
            *report = parse_report_flag(var_value);
        }
    }
}

static int msg_send(const struct ast_msg* msg, const char* to, attribute_unused const char* from)
{
    char* dest = ast_strdupa(to);
    strsep(&dest, ":");

    if (ast_msg_has_destination(msg)) {
        ast_log(LOG_ERROR, "Destination number not specified\n");
        return -1;
    }

    const char* const msg_to   = ast_msg_get_to(msg);
    const char* const msg_body = ast_msg_get_body(msg);

    int validity = parse_validity(NULL);
    int report   = parse_report_flag(NULL);
    parse_msg_vars(msg, &validity, &report);

    ast_verb(1, "MSG[%s]: <%s>: [%s]\n", dest, msg_to, S_OR(msg_body, ""));
    if (send_sms(dest, "", msg_to, S_OR(msg_body, ""), validity, report)) {
        ast_log(LOG_ERROR, "[%s] %s\n", dest, error2str(chan_quectel_err));
        return 1;
    }
    return 0;
}

static const struct ast_msg_tech msg_tech = {.name = "mobile", .msg_send = &msg_send};

int msg_tech_register()
{
    const int res = ast_msg_tech_register(&msg_tech);
    if (res) {
        ast_log(LOG_WARNING, "Unable to register msg tech\n");
    }
    return res;
}

int msg_tech_unregister()
{
    const int res = ast_msg_tech_unregister(&msg_tech);
    if (res) {
        ast_log(LOG_WARNING, "Unable to unregister msg tech\n");
    }
    return res;
}
