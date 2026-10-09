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
#error "The browser platform is single-threaded: build with Z_FEATURE_MULTI_THREAD=0 and drive zp_spin_once()"
#endif

typedef uint64_t z_clock_t;  // monotonic clock, microseconds
typedef uint64_t z_time_t;   // wall clock, microseconds since the UNIX epoch

typedef struct {
#if Z_FEATURE_LINK_WS == 1
    int32_t _ws;     // handle from zw_host_ws_open(), -1 when closed
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
