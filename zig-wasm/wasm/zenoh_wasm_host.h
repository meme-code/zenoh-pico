//
// Functions the browser provides to zenoh-pico. They are implemented in web/zenoh-worker.js
// under the "zenoh_host" import module.
//
// The wasm instance runs inside a dedicated Web Worker so that the blocking calls below
// (sleep, ws_open, ws_recv) can park the thread with Atomics.wait while the WebSocket itself
// lives on the page's main thread.
//

#ifndef ZENOH_WASM_HOST_H
#define ZENOH_WASM_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_HOST_IMPORT(name) __attribute__((import_module("zenoh_host"), import_name(#name)))

/*------------------ System ------------------*/
// Monotonic clock (performance.now()), in milliseconds.
ZW_HOST_IMPORT(now_ms) double zw_host_now_ms(void);
// Wall clock (Date.now()), in milliseconds since the UNIX epoch.
ZW_HOST_IMPORT(date_now_ms) double zw_host_date_now_ms(void);
// Fills buf with cryptographically secure random bytes (crypto.getRandomValues).
ZW_HOST_IMPORT(random_fill) void zw_host_random_fill(void *buf, size_t len);
// Blocks the worker for the given number of milliseconds.
ZW_HOST_IMPORT(sleep_ms) void zw_host_sleep_ms(double ms);

/*------------------ WebSocket ------------------*/
// Opens ws://host:port (or whatever URL the page maps it to) and blocks until it is open.
// Returns a handle >= 0, or -1 on failure/timeout.
ZW_HOST_IMPORT(ws_open)
int32_t zw_host_ws_open(const char *host, size_t host_len, const char *port, size_t port_len, uint32_t timeout_ms);
// Sends buf as one binary WebSocket message. Returns len, or -1 if the socket is closed.
ZW_HOST_IMPORT(ws_send) int32_t zw_host_ws_send(int32_t ws, const uint8_t *buf, size_t len);
// Copies the next received message into buf, waiting up to timeout_ms for one to arrive.
// Returns the number of bytes copied, 0 on timeout, or -1 if the socket is closed.
ZW_HOST_IMPORT(ws_recv) int32_t zw_host_ws_recv(int32_t ws, uint8_t *buf, size_t len, uint32_t timeout_ms);
// Closes the socket and releases the handle.
ZW_HOST_IMPORT(ws_close) void zw_host_ws_close(int32_t ws);

/*------------------ API callbacks ------------------*/
// A sample was delivered to subscriber `sub`. `kind` is a z_sample_kind_t (0 = put, 1 = delete).
ZW_HOST_IMPORT(on_sample)
void zw_host_on_sample(int32_t sub, int32_t kind, const char *keyexpr, size_t keyexpr_len, const uint8_t *payload,
                       size_t payload_len, const char *encoding, size_t encoding_len);
// A reply to query `get`. `ok` is 0 for an error reply, in which case keyexpr is empty.
ZW_HOST_IMPORT(on_reply)
void zw_host_on_reply(int32_t get, int32_t ok, const char *keyexpr, size_t keyexpr_len, const uint8_t *payload,
                      size_t payload_len, const char *encoding, size_t encoding_len);
// Query `get` is complete: no more replies will be delivered.
ZW_HOST_IMPORT(on_reply_done) void zw_host_on_reply_done(int32_t get);

#ifdef __cplusplus
}
#endif

#endif /* ZENOH_WASM_HOST_H */
