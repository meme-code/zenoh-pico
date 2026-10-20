//
// Futex-style waiting and a small lock on the module's shared memory.
//
// zenoh-pico.wasm imports a shared WebAssembly.Memory so that the page can write received
// WebSocket messages straight into it and, in multi-threaded builds, so that every thread
// (one Web Worker each) runs on the same heap. memory.atomic.wait32 traps on the browser's
// main thread, but wasm code only ever runs inside workers.
//

#ifndef ZENOH_WASM_SYNC_H
#define ZENOH_WASM_SYNC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_WAIT_FOREVER (-1LL)

// Blocks while *addr == expected, for at most timeout_ns (ZW_WAIT_FOREVER: no limit).
// Returns 0 when woken, 1 if *addr != expected on entry, 2 on timeout.
static inline int zw_futex_wait(_Atomic int32_t *addr, int32_t expected, int64_t timeout_ns) {
    return __builtin_wasm_memory_atomic_wait32((int32_t *)addr, expected, timeout_ns);
}

static inline void zw_futex_wake(_Atomic int32_t *addr, uint32_t count) {
    __builtin_wasm_memory_atomic_notify((int32_t *)addr, count);
}

// 0: unlocked, 1: locked, 2: locked and maybe contended (Drepper, "Futexes Are Tricky").
typedef struct {
    _Atomic int32_t state;
} zw_lock_t;

static inline bool zw_trylock(zw_lock_t *l) {
    int32_t expected = 0;
    return atomic_compare_exchange_strong(&l->state, &expected, 1);
}

static inline void zw_lock(zw_lock_t *l) {
    int32_t c = 0;
    if (atomic_compare_exchange_strong(&l->state, &c, 1)) {
        return;
    }
    if (c != 2) {
        c = atomic_exchange(&l->state, 2);
    }
    while (c != 0) {
        zw_futex_wait(&l->state, 2, ZW_WAIT_FOREVER);
        c = atomic_exchange(&l->state, 2);
    }
}

static inline void zw_unlock(zw_lock_t *l) {
    if (atomic_exchange(&l->state, 0) == 2) {
        zw_futex_wake(&l->state, 1);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ZENOH_WASM_SYNC_H */
