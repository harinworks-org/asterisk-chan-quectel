/*
    app.c
*/

#include "ast_config.h"

#include <asterisk/app.h> /* AST_DECLARE_APP_ARGS() ... */
#include <asterisk/json.h>
#include <asterisk/module.h> /* ast_register_application2() ast_unregister_application() */
#include <asterisk/pbx.h>    /* pbx_builtin_setvar_helper() */
#include <asterisk/strings.h>

#include "app.h" /* app_register() app_unregister() */

#include "chan_quectel.h" /* struct pvt */
#include "error.h"
#include "helpers.h" /* send_sms() ITEMS_OF() */

struct ast_channel;

/*** DOCUMENTATION
    <function name="QUECTEL_STATUS" language="en_US">
        <synopsis>
            Gets status of a channel in a numerical value.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
        </syntax>
        <description>
            <para>Returns integer value:<para>
            <para>1 - not started</para>
            <para>2 - available</para>
            <para>3 - unabailable</para>
        </description>
    </function>
    <function name="QUECTEL_STATUS_EX" language="en_US">
        <synopsis>
            Gets status of a channel in a JSON value.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
        </syntax>
    </function>
    <application name="QUECTEL_SEND_SMS" language="en_US">
        <synopsis>
            Sends a SMS on specified device.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
            <parameter name="destination" required="true">
                <para>Recipient.</para>
            </parameter>
            <parameter name="message" required="true">
                <para>Text of the message.</para>
            </parameter>
            <parameter name="validity" required="false">
                <para>Validity period in minutes.</para>
            </parameter>
            <parameter name="report" required="false">
                <para>Boolean flag for report request.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_SEND_USSD</ref>
        </see-also>
    </application>
    <application name="QUECTEL_SEND_USSD" language="en_US">
        <synopsis>
            Sends a USSD on specified device.
        </synopsis>
        <syntax>
            <parameter name="device" required="true">
                <para>Id of device from configuration file.</para>
            </parameter>
            <parameter name="destination" required="true">
                <para>Recipient.</para>
            </parameter>
            <parameter name="ussd" required="true">
                <para>USSD command.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_SEND_SMS</ref>
        </see-also>
    </application>
    <application name="QUECTEL_DELETE_SMS" language="en_US">
        <synopsis>
            Deletes a SMS on specified device.
        </synopsis>
        <syntax>
            <parameter name="device" required="true">
                <para>Id of device from configuration file.</para>
            </parameter>
            <parameter name="idx" required="true">
                <para>Index of the message.</para>
            </parameter>
            <parameter name="delflag" required="false">
                <para>Deletion flag.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_DELETE_SMS</ref>
        </see-also>
    </application>
 ***/

static int app_status_read(struct ast_channel* channel, const char* cmd, char* data, struct ast_str** str, ssize_t maxlen)
{
    int stat;
    int exists = 0;

    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        return -1;
    }

    RAII_VAR(struct pvt* const, pvt, pvt_find_by_resource(args.resource, 0, NULL, &exists), pvt_unlock);
    if (pvt) {
        stat = 2;
    } else {
        stat = exists ? 3 : 1;
    }

    ast_str_set(str, maxlen, "%d", stat);
    return 0;
}

static int app_status_ex_read(struct ast_channel* channel, const char* cmd, char* data, struct ast_str** str, ssize_t maxlen)
{
    int exists = 0;

    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        return -1;
    }

    RAII_VAR(struct ast_json* const, status, ast_json_object_create(), ast_json_unref);
    RAII_VAR(struct pvt* const, pvt, pvt_find_by_resource(args.resource, CALL_FLAG_INTERNAL_REQUEST, NULL, &exists), pvt_unlock);
    if (pvt) {
        ast_json_object_set(status, "resource", ast_json_string_create(args.resource));
        pvt_get_status(pvt, status);
    } else {
        ast_json_object_set(status, "resource", ast_json_string_create(args.resource));
        ast_json_object_set(status, "exists", ast_json_integer_create(0));
    }

    ast_json_dump_str_format(status, str, AST_JSON_COMPACT);
    return 0;
}

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

static unsigned int parse_idx(const char* const arg)
{
    static const unsigned int DEF_IDX = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_IDX;
    }

    unsigned int idx;
    const int res = sscanf(arg, "%u", &idx);

    return res ? idx : DEF_IDX;
}

static int parse_delflag(const char* const arg)
{
    static const int DEF_DELFLAG = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_DELFLAG;
    }

    int delflag;
    const int res = sscanf(arg, "%d", &delflag);

    return res ? delflag : DEF_DELFLAG;
}

static int app_send_sms_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
        AST_APP_ARG(number);
        AST_APP_ARG(message);
        AST_APP_ARG(validity);
        AST_APP_ARG(report);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        ast_log(LOG_ERROR, "NULL resource for message -- SMS will not be sent\n");
        return -1;
    }

    if (ast_strlen_zero(args.number)) {
        ast_log(LOG_ERROR, "NULL destination for message -- SMS will not be sent\n");
        return -1;
    }

    if (send_sms(args.resource, "", args.number, args.message, parse_validity(args.validity), parse_report_flag(args.report))) {
        ast_log(LOG_ERROR, "[%s] %s\n", args.resource, error2str(chan_quectel_err));
        return -1;
    }

    return 0;
}

static int app_send_ussd_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(device);
        AST_APP_ARG(ussd);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.device)) {
        ast_log(LOG_ERROR, "NULL device for ussd -- USSD will not be sent\n");
        return -1;
    }

    if (ast_strlen_zero(args.ussd)) {
        ast_log(LOG_ERROR, "NULL ussd command -- USSD will not be sent\n");
        return -1;
    }

    if (send_ussd(args.device, args.ussd)) {
        ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
        return -1;
    }

    return 0;
}

static int app_delete_sms_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(device);
        AST_APP_ARG(idx);
        AST_APP_ARG(delflag);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.device)) {
        ast_log(LOG_ERROR, "NULL device for message -- SMS will not be deleted\n");
        return -1;
    }

    if (ast_strlen_zero(args.idx)) {
        ast_log(LOG_ERROR, "NULL idx for message -- SMS will not be deleted\n");
        return -1;
    }

    if (delete_sms(args.device, parse_idx(args.idx), parse_delflag(args.delflag))) {
        ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
        return -1;
    }

    return 0;
}

/* clang-format off */

static struct ast_custom_function status_function = {
    .name = "QUECTEL_STATUS",
    .read2 = app_status_read,
    .read_max = 2
};

static struct ast_custom_function status_ex_function = {
    .name = "QUECTEL_STATUS_EX",
    .read2 = app_status_ex_read,
};

/* clang-format on */

static const char APP_SEND_SMS[]  = "QUECTEL_SEND_SMS";
static const char APP_SEND_USSD[] = "QUECTEL_SEND_USSD";
static const char APP_DELETE_SMS[]  = "QUECTEL_DELETE_SMS";

int app_register()
{
    int res;

    status_function.mod    = self_module();
    status_ex_function.mod = self_module();

    res  = ast_custom_function_register(&status_function);
    res |= ast_custom_function_register(&status_ex_function);
    res |= ast_register_application2(APP_SEND_SMS, app_send_sms_exec, NULL, NULL, self_module());
    res |= ast_register_application2(APP_SEND_USSD, app_send_ussd_exec, NULL, NULL, self_module());
    res |= ast_register_application2(APP_DELETE_SMS, app_delete_sms_exec, NULL, NULL, self_module());

    return res;
}

void app_unregister()
{
    ast_unregister_application(APP_DELETE_SMS);
    ast_unregister_application(APP_SEND_USSD);
    ast_unregister_application(APP_SEND_SMS);
    ast_custom_function_unregister(&status_ex_function);
    ast_custom_function_unregister(&status_function);
}
