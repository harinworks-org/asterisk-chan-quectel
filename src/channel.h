/*
   Copyright (C) 2010 bg <bg_one@mail.ru>
*/
#ifndef CHAN_QUECTEL_CHANNEL_H_INCLUDED
#define CHAN_QUECTEL_CHANNEL_H_INCLUDED

#include "ast_config.h"

#include <asterisk/frame.h> /* enum ast_control_frame_type */

typedef struct channel_var {
    const char* name;
    const char* value;
} channel_var_t;

struct pvt;
struct cpvt;
struct ast_json;

typedef enum local_report_direction { LOCAL_REPORT_DIRECTION_UNKNOWN, LOCAL_REPORT_DIRECTION_INCOMING, LOCAL_REPORT_DIRECTION_OUTGOING } local_report_direction;

extern struct ast_channel_tech channel_tech;

struct ast_channel* channel_new(struct pvt* pvt, int ast_state, const char* cid_num, int call_idx, unsigned dir, int state, const char* exten,
                                const struct ast_assigned_ids* assignedids, const struct ast_channel* requestor, unsigned local_channel);

int channel_self_request(struct pvt* pvt, const struct ast_channel* requestor);

int channel_enqueue_hangup(struct ast_channel* channel, int hangupcause);

int channel_start_local(struct pvt* pvt, const char* exten, const char* number, const channel_var_t* const vars, const size_t varscnt);
int channel_start_local_json(struct pvt* pvt, const char* exten, const char* number, const char* const jname, const struct ast_json* const jvar);
int channel_start_local_report(struct pvt* pvt, const char* subject, local_report_direction direction, const char* number, const struct ast_tm* const ts,
                                const struct ast_tm* const dt, int success, struct ast_json* const report);

#endif /* CHAN_QUECTEL_CHANNEL_H_INCLUDED */
