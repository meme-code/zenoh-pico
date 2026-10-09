//
// zenoh-pico system layer for web browsers: memory, randomness, sleep and clocks.
//

#include <stdio.h>
#include <stdlib.h>

#include "zenoh-pico/config.h"
#include "zenoh-pico/link/transport/socket.h"
#include "zenoh-pico/system/platform.h"
#include "zenoh-pico/utils/logging.h"
#include "zenoh_wasm_host.h"

/*------------------ Random ------------------*/
uint8_t z_random_u8(void) {
    uint8_t v;
    zw_host_random_fill(&v, sizeof(v));
    return v;
}

uint16_t z_random_u16(void) {
    uint16_t v;
    zw_host_random_fill(&v, sizeof(v));
    return v;
}

uint32_t z_random_u32(void) {
    uint32_t v;
    zw_host_random_fill(&v, sizeof(v));
    return v;
}

uint64_t z_random_u64(void) {
    uint64_t v;
    zw_host_random_fill(&v, sizeof(v));
    return v;
}

void z_random_fill(void *buf, size_t len) { zw_host_random_fill(buf, len); }

/*------------------ Memory ------------------*/
void *z_malloc(size_t size) { return malloc(size); }

void *z_realloc(void *ptr, size_t size) { return realloc(ptr, size); }

void z_free(void *ptr) { free(ptr); }

/*------------------ Sleep ------------------*/
z_result_t z_sleep_us(size_t time) {
    zw_host_sleep_ms((double)time / 1000.0);
    return _Z_RES_OK;
}

z_result_t z_sleep_ms(size_t time) {
    zw_host_sleep_ms((double)time);
    return _Z_RES_OK;
}

z_result_t z_sleep_s(size_t time) {
    zw_host_sleep_ms((double)time * 1000.0);
    return _Z_RES_OK;
}

/*------------------ Clock ------------------*/
static uint64_t _zw_elapsed_us(const uint64_t *now, const uint64_t *since) { return *now > *since ? *now - *since : 0; }

z_clock_t z_clock_now(void) { return (z_clock_t)(zw_host_now_ms() * 1000.0); }

unsigned long zp_clock_elapsed_us_since(z_clock_t *instant, z_clock_t *epoch) {
    return (unsigned long)_zw_elapsed_us(instant, epoch);
}

unsigned long zp_clock_elapsed_ms_since(z_clock_t *instant, z_clock_t *epoch) {
    return (unsigned long)(_zw_elapsed_us(instant, epoch) / 1000);
}

unsigned long zp_clock_elapsed_s_since(z_clock_t *instant, z_clock_t *epoch) {
    return (unsigned long)(_zw_elapsed_us(instant, epoch) / 1000000);
}

unsigned long z_clock_elapsed_us(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_us_since(&now, instant);
}

unsigned long z_clock_elapsed_ms(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_ms_since(&now, instant);
}

unsigned long z_clock_elapsed_s(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_s_since(&now, instant);
}

void z_clock_advance_us(z_clock_t *clock, unsigned long duration) { *clock += (z_clock_t)duration; }

void z_clock_advance_ms(z_clock_t *clock, unsigned long duration) { *clock += (z_clock_t)duration * 1000; }

void z_clock_advance_s(z_clock_t *clock, unsigned long duration) { *clock += (z_clock_t)duration * 1000000; }

/*------------------ Time ------------------*/
z_time_t z_time_now(void) { return (z_time_t)(zw_host_date_now_ms() * 1000.0); }

const char *z_time_now_as_str(char *const buf, unsigned long buflen) {
    z_time_t now = z_time_now();
    snprintf(buf, buflen, "%llu.%06llu", (unsigned long long)(now / 1000000), (unsigned long long)(now % 1000000));
    return buf;
}

unsigned long z_time_elapsed_us(z_time_t *time) {
    z_time_t now = z_time_now();
    return (unsigned long)_zw_elapsed_us(&now, time);
}

unsigned long z_time_elapsed_ms(z_time_t *time) { return z_time_elapsed_us(time) / 1000; }

unsigned long z_time_elapsed_s(z_time_t *time) { return z_time_elapsed_us(time) / 1000000; }

z_result_t _z_get_time_since_epoch(_z_time_since_epoch *t) {
    z_time_t now = z_time_now();
    t->secs = (uint32_t)(now / 1000000);
    t->nanos = (uint32_t)((now % 1000000) * 1000);
    return _Z_RES_OK;
}

/*------------------ Sockets ------------------*/
// WebSocket handles are always non-blocking with an explicit wait in zw_host_ws_recv, and
// waiting on several sockets is only needed for peer mode, which browsers cannot support.
z_result_t _z_socket_set_blocking(const _z_sys_net_socket_t *sock, bool blocking) {
    _ZP_UNUSED(sock);
    _ZP_UNUSED(blocking);
    return _Z_RES_OK;
}

void _z_socket_close(_z_sys_net_socket_t *sock) {
#if Z_FEATURE_LINK_WS == 1
    if (sock->_ws >= 0) {
        zw_host_ws_close(sock->_ws);
        sock->_ws = -1;
    }
#else
    _ZP_UNUSED(sock);
#endif
}

z_result_t _z_socket_wait_readable(_z_socket_wait_iter_t *iter, uint32_t timeout_ms) {
    _ZP_UNUSED(iter);
    _ZP_UNUSED(timeout_ms);
    _Z_ERROR("Waiting on multiple sockets is not supported in the browser");
    _Z_ERROR_RETURN(_Z_ERR_GENERIC);
}
