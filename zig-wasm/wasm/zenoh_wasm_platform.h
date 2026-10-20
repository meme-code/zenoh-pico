//
// zenoh-pico platform types for web browsers (wasm32, built with Zig).
//
// Selected through ZP_SYSTEM_PLATFORM_HEADER, so the upstream tree needs no changes.
// Only the WebSocket link is available: a browser cannot open raw TCP/UDP sockets or listen.
//

#ifndef ZENOH_WASM_PLATFORM_H
#define ZENOH_WASM_PLATFORM_H

#include <stdint.h>

#include "zenoh-pico/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if Z_FEATURE_MULTI_THREAD == 1
// Real threads: each one is a Web Worker on the same shared memory (wasm/pthread.c).
#include <pthread.h>

typedef pthread_t _z_task_t;
typedef pthread_attr_t z_task_attr_t;
typedef pthread_mutex_t _z_mutex_t;
typedef pthread_mutex_t _z_mutex_rec_t;
typedef pthread_cond_t _z_condvar_t;
typedef pthread_t _z_task_id_t;

// wasi-libc's pthread.h leaves this out unless libc itself is built with threads.
_Noreturn void pthread_exit(void *);
#endif

typedef uint64_t z_clock_t;  // monotonic clock, microseconds
typedef uint64_t z_time_t;   // wall clock, microseconds since the UNIX epoch

// Words the page bumps or sets to wake up wasm threads; see wasm/ws.c.
typedef struct {
    _Atomic int32_t wake;       // bumped and notified after every change to any socket
    _Atomic int32_t interrupt;  // set when a call from JavaScript waits for zw_spin to return
    _Atomic int32_t tx_bell;    // bumped and notified by wasm when a socket has data to send...
    _Atomic int32_t doorbell;   // ...if the page listens to it (Atomics.waitAsync); else ws_kick
} zw_ctrl_t;

typedef struct zw_ws zw_ws_t;  // a WebSocket's shared state and rings (wasm/ws.c)

typedef struct {
#if Z_FEATURE_LINK_WS == 1
    zw_ws_t *_ws;    // NULL when closed
    uint32_t _tout;  // read timeout in milliseconds
#else
    uint8_t _unused;
#endif
} _z_sys_net_socket_t;

typedef struct {
#if Z_FEATURE_LINK_WS == 1
    char *_host;
    char *_port;
#else
    uint8_t _unused;
#endif
} _z_sys_net_endpoint_t;

#ifdef __cplusplus
}
#endif

#endif /* ZENOH_WASM_PLATFORM_H */
