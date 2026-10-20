//
// Functions the browser provides to zenoh-pico. They are implemented in web/zenoh-worker.js
// under the "zenoh_host" import module (plus "wasi"."thread-spawn", see wasm/pthread.c).
//
// Every wasm instance runs inside a Web Worker on the module's shared memory; the WebSockets
// live on the page. These imports never block: waiting happens in wasm, on memory the page
// notifies (wasm/ws.c).
//

#ifndef ZENOH_WASM_HOST_H
#define ZENOH_WASM_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "zenoh-pico/system/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_HOST_IMPORT(name) __attribute__((import_module("zenoh_host"), import_name(#name)))

/*------------------ System ------------------*/
// Monotonic clock in milliseconds, with the same origin in every thread.
ZW_HOST_IMPORT(now_ms) double zw_host_now_ms(void);
// Wall clock (Date.now()), in milliseconds since the UNIX epoch.
ZW_HOST_IMPORT(date_now_ms) double zw_host_date_now_ms(void);
// Fills buf with cryptographically secure random bytes (crypto.getRandomValues).
ZW_HOST_IMPORT(random_fill) void zw_host_random_fill(void *buf, size_t len);
// Ends the calling thread (pthread_exit): unwinds back to the worker, which reports it gone.
ZW_HOST_IMPORT(thread_exit) _Noreturn void zw_host_thread_exit(void);

/*------------------ WebSocket ------------------*/
// Asks the page to open ws://host:port (or whatever URL it maps that to) for `ws`; the page
// reports progress through ws->state.
ZW_HOST_IMPORT(ws_open)
void zw_host_ws_open(zw_ws_t *ws, const char *host, size_t host_len, const char *port, size_t port_len);
// Tells the page that ws->tx has messages to send.
ZW_HOST_IMPORT(ws_kick) void zw_host_ws_kick(zw_ws_t *ws);
// Asks the page to close the socket and let go of `ws` (state becomes RELEASED).
ZW_HOST_IMPORT(ws_close) void zw_host_ws_close(zw_ws_t *ws);

/*------------------ API callbacks (wasm/api.c) ------------------*/
// A sample for the subscriber JavaScript registered as `id`.
ZW_HOST_IMPORT(on_sample) void zw_host_on_sample(int32_t id, const void *msg);
// A reply to query `id`; msg->kind is 1 for a sample, 0 for an error.
ZW_HOST_IMPORT(on_reply) void zw_host_on_reply(int32_t id, const void *msg);
// Query `id` is complete: no more replies will be delivered.
ZW_HOST_IMPORT(on_reply_done) void zw_host_on_reply_done(int32_t id);
// A query for queryable `id`; `query` is the handle to answer it with.
ZW_HOST_IMPORT(on_query) void zw_host_on_query(int32_t id, int32_t query, const void *msg);
// The matching status of the publisher behind matching listener `id` changed.
ZW_HOST_IMPORT(on_matching) void zw_host_on_matching(int32_t id, int32_t matching);

#ifdef __cplusplus
}
#endif

#endif /* ZENOH_WASM_HOST_H */
