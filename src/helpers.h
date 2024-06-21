/*
   Copyright (C) 2010,2011 bg <bg_one@mail.ru>
*/
#ifndef CHAN_QUECTEL_HELPERS_H_INCLUDED
#define CHAN_QUECTEL_HELPERS_H_INCLUDED

#include <asterisk/astobj2.h>
#include <asterisk/localtime.h>
#include <asterisk/lock.h>
#include <asterisk/strings.h>

#include "chan_quectel.h" /* restate_time_t */
#include "dc_config.h"    /* call_waiting_t */

int __ao2_unlock_and_unref(void* obj, const char* file, const char* func, int line, const char* var);
#define AO2_UNLOCK_AND_UNREF(a) __ao2_unlock_and_unref(a, __FILE__, __PRETTY_FUNCTION__, __LINE__, #a)

int __ao2_ref_and_lock(void* obj, const char* file, const char* func, int line, const char* var);
#define AO2_REF_AND_LOCK(a) __ao2_ref_and_lock(a, __FILE__, __PRETTY_FUNCTION__, __LINE__, #a)

static inline uint32_t ast_atomic_fetchadd_uint32(volatile uint32_t* p, uint32_t v) { return ast_atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

static inline uint32_t ast_atomic_fetchsub_uint32(volatile uint32_t* p, uint32_t v) { return ast_atomic_fetch_sub(p, v, __ATOMIC_RELAXED); }

static inline uint32_t ast_atomic_fetch_uint32(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

static inline uint64_t ast_atomic_fetchadd_uint64(volatile uint64_t* p, uint64_t v) { return ast_atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

static inline uint64_t ast_atomic_fetchsub_uint64(volatile uint64_t* p, uint64_t v) { return ast_atomic_fetch_sub(p, v, __ATOMIC_RELAXED); }

static inline uint64_t ast_atomic_fetch_uint64(const volatile uint64_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

/* return status string of sending, status arg is optional */
int send_ussd(const char* dev_name, const char* ussd);
int send_sms(const char* const resource, const char* const sca, const char* const destination, const char* const message, int validity, int report);
int list_sms(const char* const dev_name, enum msg_status_t stat);
int delete_sms(const char* const dev_name, unsigned int idx, int delflag);
int sms_direct(const char* const dev_name, int directflag);
int smsdb_backup();
int send_reset(const char* dev_name);
int send_ccwa_set(const char* dev_name, call_waiting_t enable);
int query_qaudloop(const char* dev_name);
int send_qaudloop(const char* dev_name, int aloop);
int query_qaudmod(const char* dev_name);
int send_qaudmod(const char* dev_name, int amode);
int query_micgain(const char* dev_name);
int send_micgain(const char* dev_name, int gain);
int query_rxgain(const char* dev_name);
int send_rxgain(const char* dev_name, int gain);
int send_uac_apply(const char* dev_name);
int send_at_command(const char* dev_name, const char* command);
int schedule_restart_event(dev_state_t event, restate_time_t when, const char* dev_name);
int is_valid_phone_number(const char* number);

int str2gain(const char*, int*);
struct ast_str* const gain2str(int);

int str2gain_simcom(const char*, int*);
struct ast_str* const gain2str_simcom(int);

size_t get_esc_str_buffer_size(size_t);
struct ast_str* escape_nstr(const char*, size_t);
struct ast_str* escape_str(const struct ast_str* const);

const char* escape_nstr_ex(struct ast_str*, const char*, size_t);
const char* escape_str_ex(struct ast_str*, const struct ast_str* const);

#define tmp_esc_str(str)                                                         \
    ({                                                                           \
        const size_t STR_TMP_LEN = get_esc_str_buffer_size(ast_str_strlen(str)); \
        struct ast_str* STR_TMP  = ast_str_alloca(STR_TMP_LEN);                  \
        escape_str_ex(STR_TMP, str);                                             \
    })

#define tmp_esc_nstr(str, len)                                   \
    ({                                                           \
        const size_t STR_TMP_LEN = get_esc_str_buffer_size(len); \
        struct ast_str* STR_TMP  = ast_str_alloca(STR_TMP_LEN);  \
        escape_nstr_ex(STR_TMP, str, len);                       \
    })

#define AST_JSON_OBJECT_SET(j, s) \
    if (s && ast_str_strlen(s)) ast_json_object_set(j, #s, ast_json_string_create(ast_str_buffer(s)));


int gsm_is_registered(int stat);
const char* gsm_regstate2str(int gsm_reg_status);
const char* gsm_regstate2str_json(int gsm_reg_status);
int map_creg_act(int act);
const char* sys_act2str(int sys_submode);
struct ast_str* rssi2dBm(int rssi);

const char* dev_state2str(dev_state_t state);
const char* dev_state2str_capitalized(dev_state_t state);
dev_state_t str2dev_state(const char* str);
const char* dev_state2str_msg(dev_state_t state);

const char* restate2str_msg(restate_time_t when);

int format_ast_tm(const struct ast_tm* const tm, struct ast_str* str);
struct ast_tm* ast_tm_normalize(struct ast_tm* const tm);

size_t fd_write_all(int fd, const char* buf, size_t count);

#endif /* CHAN_QUECTEL_HELPERS_H_INCLUDED */
