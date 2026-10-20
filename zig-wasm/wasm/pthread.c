//
// POSIX threads for multi-threaded browser builds, on wasi-threads.
//
// Zig's wasi-libc is built single-threaded: its pthread functions are stubs (weak aliases)
// and pthread_create always fails. The strong definitions below replace them. A new thread
// is a Web Worker running another instance of this module over the same shared memory: the
// "wasi"."thread-spawn" import asks the page for one, and the worker enters through
// wasi_thread_start (thread_start.s). Waiting uses memory.atomic.wait32/notify.
//
// Covered: create/join/detach/self/equal/exit, attributes (stack size, detach state), normal,
// recursive and error-checking mutexes, condition variables with either clock.
// Not covered: cancellation, thread-specific data destructors, barriers, rwlocks.
//

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zenoh_wasm_host.h"
#include "zenoh_wasm_sync.h"

#define ZW_THREAD_STACK_SIZE (1u << 20)
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 2048
#endif

// Asks the host to start a worker that calls wasi_thread_start(tid, start). Returns the new
// thread's id (> 0), or < 0 if no thread could be created.
__attribute__((import_module("wasi"), import_name("thread-spawn"))) int32_t zw_wasi_thread_spawn(void *start);

struct zw_thread {
    // Read by wasi_thread_start before any C code runs on the new thread.
    uint8_t *stack_top;
    uint8_t *tls;
    // Set to 1 and notified by the host once wasi_thread_start has returned (or the thread
    // called pthread_exit): nothing runs on this thread's stack any more, so it can be freed.
    _Atomic int32_t exited;
    _Atomic int32_t detached;
    void *(*fun)(void *);
    void *arg;
    void *result;
    struct zw_thread *next_detached;
};
_Static_assert(offsetof(struct zw_thread, stack_top) == 0, "thread_start.s reads the stack top at offset 0");
_Static_assert(offsetof(struct zw_thread, tls) == 4, "thread_start.s reads the TLS block at offset 4");
_Static_assert(offsetof(struct zw_thread, exited) == 8, "the host sets `exited` at offset 8");

static struct zw_thread zw_main_thread;
static _Thread_local struct zw_thread *zw_self;

// Detached threads cannot free their own stack; they are freed once the host reports them gone.
static zw_lock_t zw_detached_lock;
static struct zw_thread *zw_detached;

static void zw_reap_detached(void) {
    zw_lock(&zw_detached_lock);
    struct zw_thread **link = &zw_detached;
    while (*link != NULL) {
        struct zw_thread *t = *link;
        if (atomic_load(&t->exited)) {
            *link = t->next_detached;
            free(t);
        } else {
            link = &t->next_detached;
        }
    }
    zw_unlock(&zw_detached_lock);
}

static void zw_add_detached(struct zw_thread *t) {
    zw_lock(&zw_detached_lock);
    t->next_detached = zw_detached;
    zw_detached = t;
    zw_unlock(&zw_detached_lock);
}

// Called by wasi_thread_start once the thread's stack and TLS are set up.
void zw_thread_main(int32_t tid, struct zw_thread *t) {
    (void)tid;
    zw_self = t;
    t->result = t->fun(t->arg);
}

static size_t zw_align_up(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

/*------------------ Threads ------------------*/
// musl's pthread_attr_t fields, which libc's pthread_attr_get* also read.
#define ZW_ATTR_STACKSIZE(a) ((a)->__u.__s[0])
#define ZW_ATTR_DETACH(a) ((a)->__u.__i[3])

int pthread_attr_init(pthread_attr_t *a) {
    memset(a, 0, sizeof(*a));
    ZW_ATTR_STACKSIZE(a) = ZW_THREAD_STACK_SIZE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (size < PTHREAD_STACK_MIN) {
        return EINVAL;
    }
    ZW_ATTR_STACKSIZE(a) = size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    ZW_ATTR_DETACH(a) = state;
    return 0;
}

int pthread_create(pthread_t *restrict out, const pthread_attr_t *restrict attr, void *(*fun)(void *),
                   void *restrict arg) {
    zw_reap_detached();
    size_t stack_size = (attr != NULL && ZW_ATTR_STACKSIZE(attr) != 0) ? ZW_ATTR_STACKSIZE(attr) : ZW_THREAD_STACK_SIZE;
    bool detached = attr != NULL && ZW_ATTR_DETACH(attr) == PTHREAD_CREATE_DETACHED;

    // One block: the thread record, its TLS block, then its stack (growing down from the end).
    size_t tls_align = __builtin_wasm_tls_align();
    size_t align = tls_align > 16 ? tls_align : 16;
    size_t tls_offset = zw_align_up(sizeof(struct zw_thread), align);
    size_t stack_offset = zw_align_up(tls_offset + __builtin_wasm_tls_size(), 16);
    size_t total = stack_offset + zw_align_up(stack_size, 16);
    uint8_t *block = aligned_alloc(align, zw_align_up(total, align));
    if (block == NULL) {
        return EAGAIN;
    }
    struct zw_thread *t = (struct zw_thread *)block;
    memset(t, 0, sizeof(*t));
    t->stack_top = block + total;
    t->tls = block + tls_offset;
    t->fun = fun;
    t->arg = arg;
    atomic_store(&t->detached, detached);
    if (detached) {
        zw_add_detached(t);
    }
    if (zw_wasi_thread_spawn(t) < 0) {
        if (detached) {
            // Not started, so `exited` must be set for the reaper to free it.
            atomic_store(&t->exited, 1);
        } else {
            free(block);
        }
        return EAGAIN;
    }
    *out = (pthread_t)t;
    return 0;
}

int pthread_join(pthread_t thread, void **result) {
    struct zw_thread *t = (struct zw_thread *)thread;
    if (t == zw_self || t == &zw_main_thread) {
        return EDEADLK;
    }
    while (atomic_load(&t->exited) == 0) {
        zw_futex_wait(&t->exited, 0, ZW_WAIT_FOREVER);
    }
    if (result != NULL) {
        *result = t->result;
    }
    free(t);
    return 0;
}

int pthread_detach(pthread_t thread) {
    struct zw_thread *t = (struct zw_thread *)thread;
    if (t == &zw_main_thread) {
        return 0;
    }
    if (atomic_exchange(&t->detached, 1) == 0) {
        zw_add_detached(t);
    }
    zw_reap_detached();
    return 0;
}

pthread_t pthread_self(void) { return (pthread_t)(zw_self != NULL ? zw_self : &zw_main_thread); }

int(pthread_equal)(pthread_t a, pthread_t b) { return a == b; }

_Noreturn void pthread_exit(void *result) {
    if (zw_self == NULL) {
        // The main thread: end the program like returning from main() would.
        exit(0);
    }
    zw_self->result = result;
    zw_host_thread_exit();
}

/*------------------ Mutexes ------------------*/
// Fields inside musl's pthread_mutex_t; all-zero (PTHREAD_MUTEX_INITIALIZER) is an unlocked
// normal mutex. The attribute keeps the type in its low two bits, like musl.
#define ZW_MUTEX_TYPE(m) ((m)->__u.__i[0] & 3)
#define ZW_MUTEX_LOCK(m) ((zw_lock_t *)&(m)->__u.__vi[1])
#define ZW_MUTEX_OWNER(m) ((m)->__u.__p[2])
#define ZW_MUTEX_COUNT(m) ((m)->__u.__i[5])

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
    a->__attr = 0;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a) {
    (void)a;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if ((unsigned)type > 2) {
        return EINVAL;
    }
    a->__attr = (a->__attr & ~3u) | (unsigned)type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *restrict a, int *restrict type) {
    *type = (int)(a->__attr & 3);
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *restrict m, const pthread_mutexattr_t *restrict a) {
    memset(m, 0, sizeof(*m));
    if (a != NULL) {
        m->__u.__i[0] = (int)(a->__attr & 3);
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    void *self = (void *)pthread_self();
    int type = ZW_MUTEX_TYPE(m);
    if (type != PTHREAD_MUTEX_NORMAL && ZW_MUTEX_OWNER(m) == self) {
        if (type == PTHREAD_MUTEX_ERRORCHECK) {
            return EDEADLK;
        }
        ZW_MUTEX_COUNT(m)++;
        return 0;
    }
    zw_lock(ZW_MUTEX_LOCK(m));
    ZW_MUTEX_OWNER(m) = self;
    ZW_MUTEX_COUNT(m) = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    void *self = (void *)pthread_self();
    if (ZW_MUTEX_TYPE(m) == PTHREAD_MUTEX_RECURSIVE && ZW_MUTEX_OWNER(m) == self) {
        ZW_MUTEX_COUNT(m)++;
        return 0;
    }
    if (!zw_trylock(ZW_MUTEX_LOCK(m))) {
        return EBUSY;
    }
    ZW_MUTEX_OWNER(m) = self;
    ZW_MUTEX_COUNT(m) = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    int type = ZW_MUTEX_TYPE(m);
    if (type != PTHREAD_MUTEX_NORMAL) {
        if (ZW_MUTEX_OWNER(m) != (void *)pthread_self()) {
            return EPERM;
        }
        if (--ZW_MUTEX_COUNT(m) > 0) {
            return 0;
        }
    }
    ZW_MUTEX_OWNER(m) = NULL;
    ZW_MUTEX_COUNT(m) = 0;
    zw_unlock(ZW_MUTEX_LOCK(m));
    return 0;
}

/*------------------ Condition variables ------------------*/
// A sequence counter that waiters sleep on; signal/broadcast bump it. The clock id of the
// attribute (0 realtime, 1 monotonic) is kept next to it for pthread_cond_timedwait.
#define ZW_COND_SEQ(c) ((_Atomic int32_t *)&(c)->__u.__vi[0])
#define ZW_COND_CLOCK(c) ((c)->__u.__i[1])

int pthread_condattr_init(pthread_condattr_t *a) {
    a->__attr = 0;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a) {
    (void)a;
    return 0;
}

int pthread_cond_init(pthread_cond_t *restrict c, const pthread_condattr_t *restrict a) {
    memset(c, 0, sizeof(*c));
    if (a != NULL) {
        ZW_COND_CLOCK(c) = (int)(a->__attr & 0x7fffffff);
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    atomic_fetch_add(ZW_COND_SEQ(c), 1);
    zw_futex_wake(ZW_COND_SEQ(c), 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    atomic_fetch_add(ZW_COND_SEQ(c), 1);
    zw_futex_wake(ZW_COND_SEQ(c), UINT32_MAX);
    return 0;
}

// Waits for a signal for at most timeout_ns (ZW_WAIT_FOREVER: no limit). The mutex is released
// entirely while waiting, even if it is a recursive mutex locked several times.
int zw_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t timeout_ns) {
    if (ZW_MUTEX_TYPE(m) != PTHREAD_MUTEX_NORMAL && ZW_MUTEX_OWNER(m) != (void *)pthread_self()) {
        return EPERM;
    }
    int32_t seq = atomic_load(ZW_COND_SEQ(c));
    int count = ZW_MUTEX_COUNT(m);
    ZW_MUTEX_OWNER(m) = NULL;
    ZW_MUTEX_COUNT(m) = 0;
    zw_unlock(ZW_MUTEX_LOCK(m));

    int res = zw_futex_wait(ZW_COND_SEQ(c), seq, timeout_ns);

    zw_lock(ZW_MUTEX_LOCK(m));
    ZW_MUTEX_OWNER(m) = (void *)pthread_self();
    ZW_MUTEX_COUNT(m) = count;
    return res == 2 ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m) {
    return zw_cond_wait_ns(c, m, ZW_WAIT_FOREVER);
}

int pthread_cond_timedwait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m,
                           const struct timespec *restrict abstime) {
    struct timespec now;
    clock_gettime(ZW_COND_CLOCK(c) == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
    int64_t ns = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000000 + (abstime->tv_nsec - now.tv_nsec);
    if (ns <= 0) {
        return ETIMEDOUT;
    }
    return zw_cond_wait_ns(c, m, ns);
}
