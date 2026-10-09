//
// The C API exported to JavaScript (web/zenoh-worker.js) from zenoh-pico.wasm.
//
// One wasm instance holds one client session. Entities are referred to by small integer
// handles; samples and query replies are pushed back through the zenoh_host imports.
// Every function returns a z_result_t (< 0 on error) unless documented otherwise.
//

#include <stdint.h>
#include <stdlib.h>

#include "zenoh-pico.h"
#include "zenoh_wasm_host.h"

#define ZW_EXPORT(name) __attribute__((export_name(#name)))

#define ZW_MAX_SUBSCRIBERS 64
#define ZW_MAX_PUBLISHERS 64

static z_owned_session_t zw_session;
static bool zw_session_open = false;

static z_owned_subscriber_t zw_subscribers[ZW_MAX_SUBSCRIBERS];
static bool zw_subscriber_used[ZW_MAX_SUBSCRIBERS];

static z_owned_publisher_t zw_publishers[ZW_MAX_PUBLISHERS];
static bool zw_publisher_used[ZW_MAX_PUBLISHERS];

/*------------------ Memory, for passing strings and buffers from JS ------------------*/
ZW_EXPORT(zw_alloc) void *zw_alloc(size_t len) { return malloc(len); }

ZW_EXPORT(zw_free) void zw_free(void *ptr) { free(ptr); }

/*------------------ Helpers ------------------*/
static int32_t zw_find_free(const bool *used, int32_t len) {
    for (int32_t i = 0; i < len; i++) {
        if (!used[i]) {
            return i;
        }
    }
    return -1;
}

// Builds the payload and, if `encoding` is a non-empty string, the encoding to send it with.
static z_result_t zw_make_payload(z_owned_bytes_t *payload, z_owned_encoding_t *encoding, bool *has_encoding,
                                  const uint8_t *data, size_t len, const char *encoding_str) {
    *has_encoding = false;
    z_result_t res = z_bytes_copy_from_buf(payload, data, len);
    if (res != Z_OK) {
        return res;
    }
    if (encoding_str != NULL && encoding_str[0] != '\0') {
        res = z_encoding_from_str(encoding, encoding_str);
        if (res != Z_OK) {
            z_drop(z_move(*payload));
            return res;
        }
        *has_encoding = true;
    }
    return Z_OK;
}

// Flattens a sample (or error reply) into contiguous buffers and passes them to on_sample/on_reply.
static void zw_forward(int32_t id, int32_t kind, const z_loaned_keyexpr_t *keyexpr, const z_loaned_bytes_t *payload,
                       const z_loaned_encoding_t *encoding, bool is_reply) {
    z_view_string_t ke;
    const char *ke_data = "";
    size_t ke_len = 0;
    if (keyexpr != NULL && z_keyexpr_as_view_string(keyexpr, &ke) == Z_OK) {
        ke_data = z_string_data(z_loan(ke));
        ke_len = z_string_len(z_loan(ke));
    }

    z_owned_slice_t bytes;
    bool has_bytes = z_bytes_to_slice(payload, &bytes) == Z_OK;
    z_owned_string_t enc;
    bool has_enc = encoding != NULL && z_encoding_to_string(encoding, &enc) == Z_OK;

    const uint8_t *bytes_data = has_bytes ? z_slice_data(z_loan(bytes)) : NULL;
    size_t bytes_len = has_bytes ? z_slice_len(z_loan(bytes)) : 0;
    const char *enc_data = has_enc ? z_string_data(z_loan(enc)) : "";
    size_t enc_len = has_enc ? z_string_len(z_loan(enc)) : 0;

    if (is_reply) {
        zw_host_on_reply(id, kind, ke_data, ke_len, bytes_data, bytes_len, enc_data, enc_len);
    } else {
        zw_host_on_sample(id, kind, ke_data, ke_len, bytes_data, bytes_len, enc_data, enc_len);
    }

    if (has_enc) {
        z_drop(z_move(enc));
    }
    if (has_bytes) {
        z_drop(z_move(bytes));
    }
}

/*------------------ Session ------------------*/
// Opens a client session to `connect`, e.g. "ws/127.0.0.1:10000". Blocks for the handshake.
ZW_EXPORT(zw_open) int32_t zw_open(const char *connect) {
    if (zw_session_open) {
        return Z_EINVAL;
    }
    z_owned_config_t config;
    z_config_default(&config);
    z_result_t res = zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, Z_CONFIG_MODE_CLIENT);
    if (res == Z_OK) {
        res = zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, connect);
    }
    if (res != Z_OK) {
        z_drop(z_move(config));
        return res;
    }
    res = z_open(&zw_session, z_move(config), NULL);
    zw_session_open = res == Z_OK;
    return res;
}

// Runs one pending zenoh task (read, keep-alive, lease, reconnect...).
// Returns 1 if a task ran, 0 if none was ready, -1 if there is no open session.
ZW_EXPORT(zw_spin) int32_t zw_spin(void) {
    if (!zw_session_open || z_session_is_closed(z_loan(zw_session))) {
        return -1;
    }
    return zp_spin_once(z_loan(zw_session)) ? 1 : 0;
}

ZW_EXPORT(zw_close) void zw_close(void) {
    if (!zw_session_open) {
        return;
    }
    for (int32_t i = 0; i < ZW_MAX_SUBSCRIBERS; i++) {
        if (zw_subscriber_used[i]) {
            z_drop(z_move(zw_subscribers[i]));
            zw_subscriber_used[i] = false;
        }
    }
    for (int32_t i = 0; i < ZW_MAX_PUBLISHERS; i++) {
        if (zw_publisher_used[i]) {
            z_drop(z_move(zw_publishers[i]));
            zw_publisher_used[i] = false;
        }
    }
    z_drop(z_move(zw_session));
    zw_session_open = false;
}

// Writes this session's id as 32 lowercase hex characters plus a NUL into `out` (33 bytes).
ZW_EXPORT(zw_zid) int32_t zw_zid(char *out) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    z_id_t zid = z_info_zid(z_loan(zw_session));
    static const char hex[] = "0123456789abcdef";
    // Like zenoh's Display for ZenohId: little-endian, so print from the last byte.
    for (int i = 0; i < 16; i++) {
        uint8_t b = zid.id[15 - i];
        out[2 * i] = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0f];
    }
    out[32] = '\0';
    return Z_OK;
}

/*------------------ Put / delete ------------------*/
ZW_EXPORT(zw_put)
int32_t zw_put(const char *keyexpr, const uint8_t *data, size_t len, const char *encoding) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_owned_bytes_t payload;
    z_owned_encoding_t enc;
    bool has_enc;
    res = zw_make_payload(&payload, &enc, &has_enc, data, len, encoding);
    if (res != Z_OK) {
        return res;
    }
    z_put_options_t options;
    z_put_options_default(&options);
    if (has_enc) {
        options.encoding = z_move(enc);
    }
    return z_put(z_loan(zw_session), z_loan(ke), z_move(payload), &options);
}

ZW_EXPORT(zw_delete) int32_t zw_delete(const char *keyexpr) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    return z_delete(z_loan(zw_session), z_loan(ke), NULL);
}

/*------------------ Subscribers ------------------*/
static void zw_on_sample(z_loaned_sample_t *sample, void *ctx) {
    zw_forward((int32_t)(intptr_t)ctx, (int32_t)z_sample_kind(sample), z_sample_keyexpr(sample),
               z_sample_payload(sample), z_sample_encoding(sample), false);
}

// Returns a subscriber handle >= 0; its samples arrive through zenoh_host.on_sample.
ZW_EXPORT(zw_declare_subscriber) int32_t zw_declare_subscriber(const char *keyexpr) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    int32_t id = zw_find_free(zw_subscriber_used, ZW_MAX_SUBSCRIBERS);
    if (id < 0) {
        return _Z_ERR_SYSTEM_OUT_OF_MEMORY;
    }
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_owned_closure_sample_t callback;
    z_closure(&callback, zw_on_sample, NULL, (void *)(intptr_t)id);
    res = z_declare_subscriber(z_loan(zw_session), &zw_subscribers[id], z_loan(ke), z_move(callback), NULL);
    if (res != Z_OK) {
        return res;
    }
    zw_subscriber_used[id] = true;
    return id;
}

ZW_EXPORT(zw_undeclare_subscriber) int32_t zw_undeclare_subscriber(int32_t id) {
    if (id < 0 || id >= ZW_MAX_SUBSCRIBERS || !zw_subscriber_used[id]) {
        return Z_EINVAL;
    }
    zw_subscriber_used[id] = false;
    return z_undeclare_subscriber(z_move(zw_subscribers[id]));
}

/*------------------ Publishers ------------------*/
ZW_EXPORT(zw_declare_publisher) int32_t zw_declare_publisher(const char *keyexpr) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    int32_t id = zw_find_free(zw_publisher_used, ZW_MAX_PUBLISHERS);
    if (id < 0) {
        return _Z_ERR_SYSTEM_OUT_OF_MEMORY;
    }
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    res = z_declare_publisher(z_loan(zw_session), &zw_publishers[id], z_loan(ke), NULL);
    if (res != Z_OK) {
        return res;
    }
    zw_publisher_used[id] = true;
    return id;
}

ZW_EXPORT(zw_publisher_put)
int32_t zw_publisher_put(int32_t id, const uint8_t *data, size_t len, const char *encoding) {
    if (id < 0 || id >= ZW_MAX_PUBLISHERS || !zw_publisher_used[id]) {
        return Z_EINVAL;
    }
    z_owned_bytes_t payload;
    z_owned_encoding_t enc;
    bool has_enc;
    z_result_t res = zw_make_payload(&payload, &enc, &has_enc, data, len, encoding);
    if (res != Z_OK) {
        return res;
    }
    z_publisher_put_options_t options;
    z_publisher_put_options_default(&options);
    if (has_enc) {
        options.encoding = z_move(enc);
    }
    return z_publisher_put(z_loan(zw_publishers[id]), z_move(payload), &options);
}

ZW_EXPORT(zw_undeclare_publisher) int32_t zw_undeclare_publisher(int32_t id) {
    if (id < 0 || id >= ZW_MAX_PUBLISHERS || !zw_publisher_used[id]) {
        return Z_EINVAL;
    }
    zw_publisher_used[id] = false;
    return z_undeclare_publisher(z_move(zw_publishers[id]));
}

/*------------------ Queries ------------------*/
static void zw_on_reply(z_loaned_reply_t *reply, void *ctx) {
    int32_t id = (int32_t)(intptr_t)ctx;
    if (z_reply_is_ok(reply)) {
        const z_loaned_sample_t *sample = z_reply_ok(reply);
        zw_forward(id, 1, z_sample_keyexpr(sample), z_sample_payload(sample), z_sample_encoding(sample), true);
    } else {
        const z_loaned_reply_err_t *err = z_reply_err(reply);
        zw_forward(id, 0, NULL, z_reply_err_payload(err), z_reply_err_encoding(err), true);
    }
}

static void zw_on_reply_dropped(void *ctx) { zw_host_on_reply_done((int32_t)(intptr_t)ctx); }

// Sends a query. `id` is chosen by the caller so it can be registered before any reply arrives;
// replies come through zenoh_host.on_reply and the end through zenoh_host.on_reply_done.
// timeout_ms == 0 uses zenoh-pico's default query timeout.
ZW_EXPORT(zw_get) int32_t zw_get(int32_t id, const char *keyexpr, const char *parameters, uint32_t timeout_ms) {
    if (!zw_session_open) {
        return _Z_ERR_SESSION_CLOSED;
    }
    z_view_keyexpr_t ke;
    z_result_t res = z_view_keyexpr_from_str(&ke, keyexpr);
    if (res != Z_OK) {
        return res;
    }
    z_owned_closure_reply_t callback;
    z_closure(&callback, zw_on_reply, zw_on_reply_dropped, (void *)(intptr_t)id);
    z_get_options_t options;
    z_get_options_default(&options);
    options.timeout_ms = timeout_ms;
    return z_get(z_loan(zw_session), z_loan(ke), parameters, z_move(callback), &options);
}
