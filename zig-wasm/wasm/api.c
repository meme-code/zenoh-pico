//
// The C API exported to JavaScript (web/zenoh-worker.js) from zenoh-pico.wasm.
//
// One wasm instance holds one session. Declared entities are referred to by handles, which
// are pointers to heap-allocated owned objects (> 0); calls return a z_result_t (< 0) on
// error. Callbacks carry an id chosen by JavaScript before the declaration, so it can route
// data that arrives before the call returns (in multi-threaded builds, callbacks run on
// zenoh-pico's own threads). Samples, replies and queries are described by a zw_msg_t whose
// pointers are only valid during the callback.
//
// Options that JavaScript may leave unset are passed as -1 (consolidation: INT32_MIN, since -1
// is AUTO) and keep zenoh-pico's defaults.
//

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zenoh-pico.h"
#include "zenoh_wasm_host.h"

#define ZW_EXPORT(name) __attribute__((export_name(#name)))

// Layout shared with web/zenoh-shared.js (MSG).
typedef struct {
    int32_t kind;  // sample: 0 put, 1 delete; reply: 1 ok, 0 error
    const char *keyexpr;
    uint32_t keyexpr_len;
    const uint8_t *payload;
    uint32_t payload_len;
    const char *encoding;
    uint32_t encoding_len;
    const uint8_t *attachment;  // NULL when there is none
    uint32_t attachment_len;
    const char *parameters;  // queries only
    uint32_t parameters_len;
    int32_t has_payload;  // queries may carry no payload
    int32_t priority;     // -1 if unknown
    int32_t congestion_control;
    int32_t express;
    int32_t has_timestamp;
    uint64_t timestamp;  // NTP64
    char timestamp_id[36];
} zw_msg_t;
_Static_assert(offsetof(zw_msg_t, timestamp) == 64, "keep web/zenoh-shared.js in sync");
_Static_assert(offsetof(zw_msg_t, timestamp_id) == 72, "keep web/zenoh-shared.js in sync");

static z_owned_session_t zw_session;
static bool zw_session_open = false;

/*------------------ Memory, for passing strings and buffers from JS ------------------*/
ZW_EXPORT(zw_alloc) void *zw_alloc(size_t len) { return malloc(len); }

ZW_EXPORT(zw_free) void zw_free(void *ptr) { free(ptr); }

/*------------------ Helpers ------------------*/
#define ZW_CHECK_SESSION()                    \
    do {                                      \
        if (!zw_session_open) {               \
            return _Z_ERR_SESSION_CLOSED;     \
        }                                     \
    } while (0)

// Allocates an owned object for a new handle; freed again on failure or undeclare.
#define ZW_NEW(type, var)                         \
    type *var = malloc(sizeof(type));             \
    if (var == NULL) {                            \
        return _Z_ERR_SYSTEM_OUT_OF_MEMORY;       \
    }

static int32_t zw_handle(void *obj) { return (int32_t)(intptr_t)obj; }

static void zw_zid_hex(const z_id_t *zid, char *out) {
    static const char hex[] = "0123456789abcdef";
    // Like zenoh's Display for ZenohId: little-endian, so print from the last byte.
    for (int i = 0; i < 16; i++) {
        uint8_t b = zid->id[15 - i];
        out[2 * i] = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0f];
    }
    out[32] = '\0';
}

static z_result_t zw_bytes(z_owned_bytes_t *bytes, const uint8_t *data, size_t len) {
    return z_bytes_copy_from_buf(bytes, data, len);
}

// An encoding from a non-empty string, or false to keep the default.
static bool zw_encoding(z_owned_encoding_t *encoding, const char *str) {
    return str != NULL && str[0] != '\0' && z_encoding_from_str(encoding, str) == Z_OK;
}

static bool zw_attachment(z_owned_bytes_t *attachment, const uint8_t *data, size_t len, int32_t has) {
    return has && zw_bytes(attachment, data, len) == Z_OK;
}

static void zw_qos(z_congestion_control_t *cc, z_priority_t *prio, bool *express, int32_t congestion_control,
                   int32_t priority, int32_t is_express) {
    if (congestion_control >= 0) {
        *cc = (z_congestion_control_t)congestion_control;
    }
    if (priority >= Z_PRIORITY_REAL_TIME && priority <= Z_PRIORITY_BACKGROUND) {
        *prio = (z_priority_t)priority;
    }
    if (is_express >= 0) {
        *express = is_express != 0;
    }
}

// Owns the flattened copies behind a zw_msg_t until zw_msg_clear.
typedef struct {
    zw_msg_t msg;
    z_owned_slice_t payload;
    z_owned_slice_t attachment;
    z_owned_string_t encoding;
    bool has_payload, has_attachment, has_encoding;
} zw_msg_buf_t;

static void zw_msg_init(zw_msg_buf_t *b, int32_t kind, const z_loaned_keyexpr_t *keyexpr,
                        const z_loaned_bytes_t *payload, const z_loaned_encoding_t *encoding,
                        const z_loaned_bytes_t *attachment) {
    memset(b, 0, sizeof(*b));
    b->msg.kind = kind;
    b->msg.keyexpr = "";
    b->msg.encoding = "";
    b->msg.parameters = "";
    b->msg.priority = -1;
    b->msg.congestion_control = -1;
    b->msg.express = -1;
    z_view_string_t ke;
    if (keyexpr != NULL && z_keyexpr_as_view_string(keyexpr, &ke) == Z_OK) {
        b->msg.keyexpr = z_string_data(z_loan(ke));
        b->msg.keyexpr_len = (uint32_t)z_string_len(z_loan(ke));
    }
    if (payload != NULL && z_bytes_to_slice(payload, &b->payload) == Z_OK) {
        b->has_payload = true;
        b->msg.has_payload = 1;
        b->msg.payload = z_slice_data(z_loan(b->payload));
        b->msg.payload_len = (uint32_t)z_slice_len(z_loan(b->payload));
    }
    if (encoding != NULL && z_encoding_to_string(encoding, &b->encoding) == Z_OK) {
        b->has_encoding = true;
        b->msg.encoding = z_string_data(z_loan(b->encoding));
        b->msg.encoding_len = (uint32_t)z_string_len(z_loan(b->encoding));
    }
    if (attachment != NULL && z_bytes_to_slice(attachment, &b->attachment) == Z_OK) {
        b->has_attachment = true;
        b->msg.attachment = z_slice_data(z_loan(b->attachment));
        b->msg.attachment_len = (uint32_t)z_slice_len(z_loan(b->attachment));
        if (b->msg.attachment == NULL) {
            b->msg.attachment = (const uint8_t *)"";  // present but empty
        }
    }
}

static void zw_msg_clear(zw_msg_buf_t *b) {
    if (b->has_payload) {
        z_drop(z_move(b->payload));
    }
    if (b->has_attachment) {
        z_drop(z_move(b->attachment));
    }
    if (b->has_encoding) {
        z_drop(z_move(b->encoding));
    }
}

static void zw_msg_from_sample(zw_msg_buf_t *b, const z_loaned_sample_t *sample) {
    zw_msg_init(b, (int32_t)z_sample_kind(sample), z_sample_keyexpr(sample), z_sample_payload(sample),
                z_sample_encoding(sample), z_sample_attachment(sample));
    b->msg.priority = (int32_t)z_sample_priority(sample);
    b->msg.congestion_control = (int32_t)z_sample_congestion_control(sample);
    b->msg.express = z_sample_express(sample) ? 1 : 0;
    const z_timestamp_t *ts = z_sample_timestamp(sample);
    if (ts != NULL) {
        b->msg.has_timestamp = 1;
        b->msg.timestamp = z_timestamp_ntp64_time(ts);
        z_id_t id = z_timestamp_id(ts);
        zw_zid_hex(&id, b->msg.timestamp_id);
    }
}

/*------------------ Session ------------------*/
// Opens a session. `mode` is "client" (default) or "peer"; `connect` is one locator such as
// "ws/127.0.0.1:10000", or several separated by commas. Blocks for the handshake.
ZW_EXPORT(zw_open) int32_t zw_open(const char *connect, const char *mode) {
    if (zw_session_open) {
        return Z_EINVAL;
    }
    z_owned_config_t config;
    z_config_default(&config);
    z_result_t res = zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY,
                                      (mode != NULL && mode[0] != '\0') ? mode : Z_CONFIG_MODE_CLIENT);
    // zp_config_insert appends to the list of connect endpoints.
    const char *start = connect;
    while (res == Z_OK && start != NULL && *start != '\0') {
        const char *comma = strchr(start, ',');
        size_t len = comma != NULL ? (size_t)(comma - start) : strlen(start);
        char *endpoint = malloc(len + 1);
        if (endpoint == NULL) {
            res = _Z_ERR_SYSTEM_OUT_OF_MEMORY;
            break;
        }
        memcpy(endpoint, start, len);
        endpoint[len] = '\0';
        if (len > 0) {
            res = zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, endpoint);
        }
        free(endpoint);
        start = comma != NULL ? comma + 1 : NULL;
    }
    if (res != Z_OK) {
        z_drop(z_move(config));
        return res;
    }
    res = z_open(&zw_session, z_move(config), NULL);
    zw_session_open = res == Z_OK;
    return res;
}

#if Z_FEATURE_MULTI_THREAD == 0
extern bool zw_interruptible;

// Single-threaded builds: runs one pending zenoh task (read, keep-alive, lease, reconnect...).
// Returns 1 if a task ran, 0 if none was ready, -1 if there is no open session. Blocking reads
// made from here end early when the page raises zw_ctrl.interrupt.
ZW_EXPORT(zw_spin) int32_t zw_spin(void) {
    if (!zw_session_open || z_session_is_closed(z_loan(zw_session))) {
        return -1;
    }
    zw_interruptible = true;
    bool ran = zp_spin_once(z_loan(zw_session));
    zw_interruptible = false;
    return ran ? 1 : 0;
}
#endif

// Returns -1 if the session is gone (e.g. its lease expired), else 0.
ZW_EXPORT(zw_session_check) int32_t zw_session_check(void) {
    return zw_session_open && !z_session_is_closed(z_loan(zw_session)) ? 0 : -1;
}

ZW_EXPORT(zw_close) void zw_close(void) {
    if (!zw_session_open) {
        return;
    }
    z_drop(z_move(zw_session));
    zw_session_open = false;
}

// Writes this session's id as 32 lowercase hex characters plus a NUL into `out` (33 bytes).
ZW_EXPORT(zw_zid) int32_t zw_zid(char *out) {
    ZW_CHECK_SESSION();
    z_id_t zid = z_info_zid(z_loan(zw_session));
    zw_zid_hex(&zid, out);
    return Z_OK;
}

typedef struct {
    char *out;
    int32_t max, count;
} zw_zid_list_t;

static void zw_collect_zid(const z_id_t *zid, void *ctx) {
    zw_zid_list_t *list = ctx;
    if (list->count < list->max) {
        zw_zid_hex(zid, list->out + 33 * list->count);
    }
    list->count++;
}

// Writes the ids of the connected routers (`peers` == 0) or peers into `out` as consecutive
// 33-byte strings, at most `max` of them. Returns how many there are.
ZW_EXPORT(zw_info) int32_t zw_info(int32_t peers, char *out, int32_t max) {
    ZW_CHECK_SESSION();
    zw_zid_list_t list = {out, max, 0};
    z_owned_closure_zid_t callback;
    z_closure(&callback, zw_collect_zid, NULL, &list);
    z_result_t res = peers ? z_info_peers_zid(z_loan(zw_session), z_move(callback))
                           : z_info_routers_zid(z_loan(zw_session), z_move(callback));
    return res != Z_OK ? res : list.count;
}

/*------------------ Key expressions ------------------*/
// 1 if a and b intersect (`includes` == 0) or a includes b, 0 if not; < 0 if either is invalid.
ZW_EXPORT(zw_keyexpr_relation) int32_t zw_keyexpr_relation(const char *a, const char *b, int32_t includes) {
    z_view_keyexpr_t ka, kb;
    z_result_t res = z_view_keyexpr_from_str(&ka, a);
    if (res == Z_OK) {
        res = z_view_keyexpr_from_str(&kb, b);
    }
    if (res != Z_OK) {
        return res;
    }
    return (includes ? z_keyexpr_includes(z_loan(ka), z_loan(kb)) : z_keyexpr_intersects(z_loan(ka), z_loan(kb)))
               ? 1
               : 0;
}

// Canonizes the NUL-terminated key expression in place. Returns its new length.
ZW_EXPORT(zw_keyexpr_canonize) int32_t zw_keyexpr_canonize(char *ke) {
    size_t len = strlen(ke);
    z_result_t res = z_keyexpr_canonize(ke, &len);
    if (res != Z_OK) {
        return res;
    }
    ke[len] = '\0';
    return (int32_t)len;
}

/*------------------ Put / delete ------------------*/
ZW_EXPORT(zw_put)
int32_t zw_put(const char *keyexpr, const uint8_t *data, size_t len, const char *encoding, const uint8_t *att,
               size_t att_len, int32_t has_att, int32_t congestion_control, int32_t priority, int32_t express) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_owned_bytes_t payload;
    res = zw_bytes(&payload, data, len);
    if (res != Z_OK) {
        return res;
    }
    z_put_options_t options;
    z_put_options_default(&options);
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    zw_qos(&options.congestion_control, &options.priority, &options.is_express, congestion_control, priority,
           express);
    return z_put(z_loan(zw_session), z_loan(ke), z_move(payload), &options);
}

ZW_EXPORT(zw_delete)
int32_t zw_delete(const char *keyexpr, int32_t congestion_control, int32_t priority, int32_t express) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_delete_options_t options;
    z_delete_options_default(&options);
    zw_qos(&options.congestion_control, &options.priority, &options.is_express, congestion_control, priority,
           express);
    return z_delete(z_loan(zw_session), z_loan(ke), &options);
}

/*------------------ Subscribers ------------------*/
static void zw_on_sample(z_loaned_sample_t *sample, void *ctx) {
    zw_msg_buf_t b;
    zw_msg_from_sample(&b, sample);
    zw_host_on_sample((int32_t)(intptr_t)ctx, &b.msg);
    zw_msg_clear(&b);
}

// Samples arrive through zenoh_host.on_sample(id, msg). `liveliness` != 0 declares a
// liveliness subscriber (`history`: also get the tokens that are already alive).
ZW_EXPORT(zw_declare_subscriber)
int32_t zw_declare_subscriber(int32_t id, const char *keyexpr, int32_t liveliness, int32_t history) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    ZW_NEW(z_owned_subscriber_t, sub);
    z_owned_closure_sample_t callback;
    z_closure(&callback, zw_on_sample, NULL, (void *)(intptr_t)id);
    if (liveliness) {
        z_liveliness_subscriber_options_t options;
        z_liveliness_subscriber_options_default(&options);
        options.history = history != 0;
        res = z_liveliness_declare_subscriber(z_loan(zw_session), sub, z_loan(ke), z_move(callback), &options);
    } else {
        res = z_declare_subscriber(z_loan(zw_session), sub, z_loan(ke), z_move(callback), NULL);
    }
    if (res != Z_OK) {
        free(sub);
        return res;
    }
    return zw_handle(sub);
}

ZW_EXPORT(zw_undeclare_subscriber) int32_t zw_undeclare_subscriber(z_owned_subscriber_t *sub) {
    z_result_t res = z_undeclare_subscriber(z_move(*sub));
    free(sub);
    return res;
}

/*------------------ Publishers ------------------*/
ZW_EXPORT(zw_declare_publisher)
int32_t zw_declare_publisher(const char *keyexpr, const char *encoding, int32_t congestion_control, int32_t priority,
                             int32_t express, int32_t reliability) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_publisher_options_t options;
    z_publisher_options_default(&options);
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    zw_qos(&options.congestion_control, &options.priority, &options.is_express, congestion_control, priority,
           express);
#ifdef Z_FEATURE_UNSTABLE_API
    if (reliability >= 0) {
        options.reliability = (z_reliability_t)reliability;
    }
#else
    _ZP_UNUSED(reliability);
#endif
    ZW_NEW(z_owned_publisher_t, pub);
    res = z_declare_publisher(z_loan(zw_session), pub, z_loan(ke), &options);
    if (res != Z_OK) {
        free(pub);
        return res;
    }
    return zw_handle(pub);
}

ZW_EXPORT(zw_publisher_put)
int32_t zw_publisher_put(z_owned_publisher_t *pub, const uint8_t *data, size_t len, const char *encoding,
                         const uint8_t *att, size_t att_len, int32_t has_att) {
    z_owned_bytes_t payload;
    z_result_t res = zw_bytes(&payload, data, len);
    if (res != Z_OK) {
        return res;
    }
    z_publisher_put_options_t options;
    z_publisher_put_options_default(&options);
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    return z_publisher_put(z_loan(*pub), z_move(payload), &options);
}

ZW_EXPORT(zw_publisher_delete) int32_t zw_publisher_delete(z_owned_publisher_t *pub) {
    return z_publisher_delete(z_loan(*pub), NULL);
}

ZW_EXPORT(zw_undeclare_publisher) int32_t zw_undeclare_publisher(z_owned_publisher_t *pub) {
    z_result_t res = z_undeclare_publisher(z_move(*pub));
    free(pub);
    return res;
}

// 1 if subscribers currently match the publisher, else 0.
ZW_EXPORT(zw_publisher_matching_status) int32_t zw_publisher_matching_status(z_owned_publisher_t *pub) {
    z_matching_status_t status;
    z_result_t res = z_publisher_get_matching_status(z_loan(*pub), &status);
    return res != Z_OK ? res : (status.matching ? 1 : 0);
}

static void zw_on_matching(const z_matching_status_t *status, void *ctx) {
    zw_host_on_matching((int32_t)(intptr_t)ctx, status->matching ? 1 : 0);
}

// Matching status changes arrive through zenoh_host.on_matching(id, matching).
ZW_EXPORT(zw_declare_matching_listener) int32_t zw_declare_matching_listener(int32_t id, z_owned_publisher_t *pub) {
    ZW_NEW(z_owned_matching_listener_t, listener);
    z_owned_closure_matching_status_t callback;
    z_closure(&callback, zw_on_matching, NULL, (void *)(intptr_t)id);
    z_result_t res = z_publisher_declare_matching_listener(z_loan(*pub), listener, z_move(callback));
    if (res != Z_OK) {
        free(listener);
        return res;
    }
    return zw_handle(listener);
}

ZW_EXPORT(zw_undeclare_matching_listener)
int32_t zw_undeclare_matching_listener(z_owned_matching_listener_t *listener) {
    z_result_t res = z_undeclare_matching_listener(z_move(*listener));
    free(listener);
    return res;
}

/*------------------ Queryables ------------------*/
static void zw_on_query(z_loaned_query_t *query, void *ctx) {
    // JavaScript answers later, so keep a reference; the query is finalized (the querier gets
    // its final reply) when JavaScript calls zw_query_drop.
    z_owned_query_t *owned = malloc(sizeof(z_owned_query_t));
    if (owned == NULL || z_query_clone(owned, query) != Z_OK) {
        free(owned);
        return;
    }
    zw_msg_buf_t b;
    zw_msg_init(&b, 0, z_query_keyexpr(query), z_query_payload(query), z_query_encoding(query),
                z_query_attachment(query));
    z_view_string_t params;
    z_query_parameters(query, &params);
    b.msg.parameters = z_string_data(z_loan(params));
    b.msg.parameters_len = (uint32_t)z_string_len(z_loan(params));
    if (b.msg.parameters == NULL) {
        b.msg.parameters = "";
    }
    zw_host_on_query((int32_t)(intptr_t)ctx, zw_handle(owned), &b.msg);
    zw_msg_clear(&b);
}

// Queries arrive through zenoh_host.on_query(id, query, msg); each must be answered with
// zw_query_reply* and released with zw_query_drop.
ZW_EXPORT(zw_declare_queryable) int32_t zw_declare_queryable(int32_t id, const char *keyexpr, int32_t complete) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_queryable_options_t options;
    z_queryable_options_default(&options);
    options.complete = complete != 0;
    ZW_NEW(z_owned_queryable_t, queryable);
    z_owned_closure_query_t callback;
    z_closure(&callback, zw_on_query, NULL, (void *)(intptr_t)id);
    res = z_declare_queryable(z_loan(zw_session), queryable, z_loan(ke), z_move(callback), &options);
    if (res != Z_OK) {
        free(queryable);
        return res;
    }
    return zw_handle(queryable);
}

ZW_EXPORT(zw_undeclare_queryable) int32_t zw_undeclare_queryable(z_owned_queryable_t *queryable) {
    z_result_t res = z_undeclare_queryable(z_move(*queryable));
    free(queryable);
    return res;
}

ZW_EXPORT(zw_query_reply)
int32_t zw_query_reply(z_owned_query_t *query, const char *keyexpr, const uint8_t *data, size_t len,
                       const char *encoding, const uint8_t *att, size_t att_len, int32_t has_att) {
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_owned_bytes_t payload;
    res = zw_bytes(&payload, data, len);
    if (res != Z_OK) {
        return res;
    }
    z_query_reply_options_t options;
    z_query_reply_options_default(&options);
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    return z_query_reply(z_loan(*query), z_loan(ke), z_move(payload), &options);
}

ZW_EXPORT(zw_query_reply_err)
int32_t zw_query_reply_err(z_owned_query_t *query, const uint8_t *data, size_t len, const char *encoding) {
    z_owned_bytes_t payload;
    z_result_t res = zw_bytes(&payload, data, len);
    if (res != Z_OK) {
        return res;
    }
    z_query_reply_err_options_t options;
    z_query_reply_err_options_default(&options);
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    return z_query_reply_err(z_loan(*query), z_move(payload), &options);
}

ZW_EXPORT(zw_query_reply_del)
int32_t zw_query_reply_del(z_owned_query_t *query, const char *keyexpr, const uint8_t *att, size_t att_len,
                           int32_t has_att) {
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_query_reply_del_options_t options;
    z_query_reply_del_options_default(&options);
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    return z_query_reply_del(z_loan(*query), z_loan(ke), &options);
}

ZW_EXPORT(zw_query_drop) void zw_query_drop(z_owned_query_t *query) {
    z_drop(z_move(*query));
    free(query);
}

/*------------------ Queries ------------------*/
static void zw_on_reply(z_loaned_reply_t *reply, void *ctx) {
    zw_msg_buf_t b;
    if (z_reply_is_ok(reply)) {
        zw_msg_from_sample(&b, z_reply_ok(reply));
        b.msg.kind = 1;
    } else {
        const z_loaned_reply_err_t *err = z_reply_err(reply);
        zw_msg_init(&b, 0, NULL, z_reply_err_payload(err), z_reply_err_encoding(err), NULL);
    }
    zw_host_on_reply((int32_t)(intptr_t)ctx, &b.msg);
    zw_msg_clear(&b);
}

static void zw_on_reply_dropped(void *ctx) { zw_host_on_reply_done((int32_t)(intptr_t)ctx); }

// Sends a query. Replies come through zenoh_host.on_reply(id, msg) and the end through
// zenoh_host.on_reply_done(id). timeout_ms == 0 uses zenoh-pico's default query timeout.
ZW_EXPORT(zw_get)
int32_t zw_get(int32_t id, const char *keyexpr, const char *parameters, const uint8_t *data, size_t len,
               int32_t has_payload, const char *encoding, const uint8_t *att, size_t att_len, int32_t has_att,
               int32_t target, int32_t consolidation, uint32_t timeout_ms, int32_t congestion_control,
               int32_t priority, int32_t express) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_get_options_t options;
    z_get_options_default(&options);
    z_owned_bytes_t payload;
    if (has_payload && zw_bytes(&payload, data, len) == Z_OK) {
        options.payload = z_move(payload);
    }
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    if (target >= 0) {
        options.target = (z_query_target_t)target;
    }
    if (consolidation != INT32_MIN) {
        options.consolidation.mode = (z_consolidation_mode_t)consolidation;
    }
    if (timeout_ms > 0) {
        options.timeout_ms = timeout_ms;
    }
    zw_qos(&options.congestion_control, &options.priority, &options.is_express, congestion_control, priority,
           express);
    z_owned_closure_reply_t callback;
    z_closure(&callback, zw_on_reply, zw_on_reply_dropped, (void *)(intptr_t)id);
    return z_get(z_loan(zw_session), z_loan(ke), parameters, z_move(callback), &options);
}

/*------------------ Liveliness ------------------*/
ZW_EXPORT(zw_liveliness_declare_token) int32_t zw_liveliness_declare_token(const char *keyexpr) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    ZW_NEW(z_owned_liveliness_token_t, token);
    res = z_liveliness_declare_token(z_loan(zw_session), token, z_loan(ke), NULL);
    if (res != Z_OK) {
        free(token);
        return res;
    }
    return zw_handle(token);
}

ZW_EXPORT(zw_liveliness_undeclare_token) int32_t zw_liveliness_undeclare_token(z_owned_liveliness_token_t *token) {
    z_result_t res = z_liveliness_undeclare_token(z_move(*token));
    free(token);
    return res;
}

// Asks which liveliness tokens matching `keyexpr` are alive; answers arrive like zw_get's.
ZW_EXPORT(zw_liveliness_get) int32_t zw_liveliness_get(int32_t id, const char *keyexpr, uint32_t timeout_ms) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_liveliness_get_options_t options;
    z_liveliness_get_options_default(&options);
    if (timeout_ms > 0) {
        options.timeout_ms = timeout_ms;
    }
    z_owned_closure_reply_t callback;
    z_closure(&callback, zw_on_reply, zw_on_reply_dropped, (void *)(intptr_t)id);
    return z_liveliness_get(z_loan(zw_session), z_loan(ke), z_move(callback), &options);
}

/*------------------ Queriers ------------------*/
ZW_EXPORT(zw_declare_querier)
int32_t zw_declare_querier(const char *keyexpr, int32_t target, int32_t consolidation, uint32_t timeout_ms,
                           int32_t congestion_control, int32_t priority, int32_t express) {
    ZW_CHECK_SESSION();
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_querier_options_t options;
    z_querier_options_default(&options);
    if (target >= 0) {
        options.target = (z_query_target_t)target;
    }
    if (consolidation != INT32_MIN) {
        options.consolidation.mode = (z_consolidation_mode_t)consolidation;
    }
    if (timeout_ms > 0) {
        options.timeout_ms = timeout_ms;
    }
    zw_qos(&options.congestion_control, &options.priority, &options.is_express, congestion_control, priority,
           express);
    ZW_NEW(z_owned_querier_t, querier);
    res = z_declare_querier(z_loan(zw_session), querier, z_loan(ke), &options);
    if (res != Z_OK) {
        free(querier);
        return res;
    }
    return zw_handle(querier);
}

ZW_EXPORT(zw_querier_get)
int32_t zw_querier_get(z_owned_querier_t *querier, int32_t id, const char *parameters, const uint8_t *data,
                       size_t len, int32_t has_payload, const char *encoding, const uint8_t *att, size_t att_len,
                       int32_t has_att) {
    z_querier_get_options_t options;
    z_querier_get_options_default(&options);
    z_owned_bytes_t payload;
    if (has_payload && zw_bytes(&payload, data, len) == Z_OK) {
        options.payload = z_move(payload);
    }
    z_owned_encoding_t enc;
    if (zw_encoding(&enc, encoding)) {
        options.encoding = z_move(enc);
    }
    z_owned_bytes_t attachment;
    if (zw_attachment(&attachment, att, att_len, has_att)) {
        options.attachment = z_move(attachment);
    }
    z_owned_closure_reply_t callback;
    z_closure(&callback, zw_on_reply, zw_on_reply_dropped, (void *)(intptr_t)id);
    return z_querier_get(z_loan(*querier), parameters, z_move(callback), &options);
}

ZW_EXPORT(zw_undeclare_querier) int32_t zw_undeclare_querier(z_owned_querier_t *querier) {
    z_result_t res = z_undeclare_querier(z_move(*querier));
    free(querier);
    return res;
}
