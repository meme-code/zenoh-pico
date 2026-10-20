//
// zenoh-pico system layer for web browsers: memory, randomness, sleep, clocks and, in
// multi-threaded builds, tasks, mutexes and condition variables on top of wasm/pthread.c.
//

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "zenoh-pico/api/primitives.h"
#include "zenoh-pico/config.h"
#include "zenoh-pico/system/common/system_error.h"
#include "zenoh-pico/system/platform.h"
#include "zenoh-pico/utils/logging.h"
#include "zenoh_wasm_host.h"
#include "zenoh_wasm_sync.h"

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

#if Z_FEATURE_MULTI_THREAD == 1
/*------------------ Task ------------------*/
z_result_t _z_task_init(_z_task_t *task, z_task_attr_t *attr, void *(*fun)(void *), void *arg) {
    _Z_CHECK_SYS_ERR(pthread_create(task, attr, fun, arg));
}

z_result_t _z_task_join(_z_task_t *task) { _Z_CHECK_SYS_ERR(pthread_join(*task, NULL)); }

z_result_t _z_task_detach(_z_task_t *task) { _Z_CHECK_SYS_ERR(pthread_detach(*task)); }

z_result_t _z_task_cancel(_z_task_t *task) {
    // A Web Worker cannot be interrupted from outside while it runs wasm.
    _ZP_UNUSED(task);
    _Z_ERROR_RETURN(_Z_ERR_GENERIC);
}

void _z_task_exit(void) { pthread_exit(NULL); }

void _z_task_free(_z_task_t **task) {
    _z_task_t *ptr = *task;
    z_free(ptr);
    *task = NULL;
}

_z_task_id_t _z_task_get_id(const _z_task_t *task) { return *task; }

_z_task_id_t _z_task_current_id(void) { return pthread_self(); }

bool _z_task_id_equal(const _z_task_id_t *l, const _z_task_id_t *r) { return pthread_equal(*l, *r) != 0; }

/*------------------ Mutex ------------------*/
z_result_t _z_mutex_init(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_init(m, NULL)); }

z_result_t _z_mutex_drop(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_destroy(m)); }

z_result_t _z_mutex_lock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_lock(m)); }

// zenoh-pico try-locks its transmit mutex for messages with congestion control "drop" and
// drops them if another thread is sending. Natively such a send is over in microseconds; here
// the holder is a Web Worker that may be busy compiling code it runs for the first time, which
// right after z_open made one put in three fail. So give the holder a short grace period. A
// send never waits for the network here (it only copies into the WebSocket's ring), so this
// does not turn "drop" into "block".
#define ZW_TRY_LOCK_GRACE_US 500

z_result_t _z_mutex_try_lock(_z_mutex_t *m) {
    z_clock_t start = z_clock_now();
    int res;
    while ((res = pthread_mutex_trylock(m)) == EBUSY && z_clock_elapsed_us(&start) < ZW_TRY_LOCK_GRACE_US) {
    }
    _Z_CHECK_SYS_ERR(res);
}

z_result_t _z_mutex_unlock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_unlock(m)); }

z_result_t _z_mutex_rec_init(_z_mutex_rec_t *m) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    _Z_CHECK_SYS_ERR(pthread_mutex_init(m, &attr));
}

z_result_t _z_mutex_rec_drop(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_destroy(m)); }

z_result_t _z_mutex_rec_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_lock(m)); }

z_result_t _z_mutex_rec_try_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_trylock(m)); }

z_result_t _z_mutex_rec_unlock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_unlock(m)); }

/*------------------ Condvar ------------------*/
int zw_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t timeout_ns);  // pthread.c

z_result_t _z_condvar_init(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_init(cv, NULL)); }

z_result_t _z_condvar_drop(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_destroy(cv)); }

z_result_t _z_condvar_signal(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_signal(cv)); }

z_result_t _z_condvar_signal_all(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_broadcast(cv)); }

z_result_t _z_condvar_wait(_z_condvar_t *cv, _z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_cond_wait(cv, m)); }

// abstime is a z_clock_t, so wait on the same clock rather than through a struct timespec.
z_result_t _z_condvar_wait_until(_z_condvar_t *cv, _z_mutex_t *m, const z_clock_t *abstime) {
    z_clock_t now = z_clock_now();
    if (*abstime <= now) {
        return Z_ETIMEDOUT;
    }
    int error = zw_cond_wait_ns(cv, m, (int64_t)(*abstime - now) * 1000);
    if (error == ETIMEDOUT) {
        return Z_ETIMEDOUT;
    }
    _Z_CHECK_SYS_ERR(error);
}
#endif  // Z_FEATURE_MULTI_THREAD == 1

/*------------------ Sleep ------------------*/
static void zw_sleep_ns(int64_t ns) {
    // Nothing ever notifies this word: memory.atomic.wait32 just times out.
    _Atomic int32_t never = 0;
    if (ns > 0) {
        zw_futex_wait(&never, 0, ns);
    }
}

z_result_t z_sleep_us(size_t time) {
    zw_sleep_ns((int64_t)time * 1000);
    return _Z_RES_OK;
}

z_result_t z_sleep_ms(size_t time) {
    zw_sleep_ns((int64_t)time * 1000000);
    return _Z_RES_OK;
}

z_result_t z_sleep_s(size_t time) {
    zw_sleep_ns((int64_t)time * 1000000000);
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

#if Z_FEATURE_SCOUTING == 0 && Z_FEATURE_CONNECTIVITY == 1
/*------------------ Compatibility ------------------*/
// Upstream declares and defines this only with Z_FEATURE_SCOUTING, but the connectivity API
// (e.g. examples/unix/c11/z_info.c) uses it as well. Same mapping as src/api/api.c.
z_result_t z_whatami_to_view_string(z_whatami_t whatami, z_view_string_t *str_out);

z_result_t z_whatami_to_view_string(z_whatami_t whatami, z_view_string_t *str_out) {
    switch (whatami) {
        case Z_WHATAMI_ROUTER:
            return z_view_string_from_str(str_out, "router");
        case Z_WHATAMI_PEER:
            return z_view_string_from_str(str_out, "peer");
        case Z_WHATAMI_CLIENT:
            return z_view_string_from_str(str_out, "client");
        default:
            z_view_string_from_str(str_out, "");
            _Z_ERROR_RETURN(_Z_ERR_INVALID);
    }
}
#endif
