//
// Exercises the browser's POSIX threads (wasm/pthread.c, wasm/heap.zig) and multi-threaded
// zenoh-pico sessions. Built as a browser program and run by tests/run-browser-test.mjs:
//
//   threads-test <locator>      e.g. threads-test ws/127.0.0.1:10000
//
// Prints a PASS/FAIL line per check; the exit code is the number of failures.
//

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zenoh-pico.h>

static int failures = 0;

static void check(const char *name, bool ok, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  " : "", detail);
    if (!ok) {
        failures++;
    }
}

static char detail[256];

/*------------------ Mutex contention ------------------*/
#define WORKERS 4
#define INCREMENTS 100000

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void *increment(void *arg) {
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        pthread_mutex_lock(&counter_mutex);
        counter++;
        pthread_mutex_unlock(&counter_mutex);
    }
    return NULL;
}

static void test_mutex(void) {
    pthread_t threads[WORKERS];
    int created = 0;
    for (int i = 0; i < WORKERS; i++) {
        created += pthread_create(&threads[i], NULL, increment, NULL) == 0;
    }
    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }
    snprintf(detail, sizeof(detail), "%d threads, counter %ld", created, counter);
    check("mutex keeps a shared counter exact", created == WORKERS && counter == (long)WORKERS * INCREMENTS, detail);
}

/*------------------ Recursive mutex ------------------*/
static pthread_mutex_t rec;

static void *try_rec(void *arg) {
    (void)arg;
    return (void *)(intptr_t)pthread_mutex_trylock(&rec);
}

static void test_recursive(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec, &attr);
    bool ok = pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0;
    pthread_t t;
    void *other = NULL;
    pthread_create(&t, NULL, try_rec, NULL);
    pthread_join(t, &other);
    ok = ok && (intptr_t)other == EBUSY;
    ok = ok && pthread_mutex_unlock(&rec) == 0 && pthread_mutex_unlock(&rec) == 0;
    pthread_create(&t, NULL, try_rec, NULL);
    pthread_join(t, &other);
    ok = ok && (intptr_t)other == 0;
    check("recursive mutex", ok, "");
}

/*------------------ Condition variables ------------------*/
#define ITEMS 1000

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static int queue[16];
static int queue_len = 0;

static void *produce(void *arg) {
    (void)arg;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&queue_mutex);
        while (queue_len == 16) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        queue[queue_len++] = i;
        pthread_cond_broadcast(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }
    return NULL;
}

static void test_condvar(void) {
    pthread_t t;
    pthread_create(&t, NULL, produce, NULL);
    long sum = 0;
    for (int n = 0; n < ITEMS; n++) {
        pthread_mutex_lock(&queue_mutex);
        while (queue_len == 0) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        sum += queue[--queue_len];
        pthread_cond_broadcast(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "sum %ld", sum);
    check("condition variable producer/consumer", sum == (long)ITEMS * (ITEMS + 1) / 2, detail);

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_t cond;
    pthread_cond_init(&cond, &attr);
    struct timespec start, deadline, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    deadline = start;
    deadline.tv_nsec += 50 * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&queue_mutex);
    int res = pthread_cond_timedwait(&cond, &queue_mutex, &deadline);
    pthread_mutex_unlock(&queue_mutex);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    snprintf(detail, sizeof(detail), "returned %d after %ld ms", res, ms);
    check("timed wait times out", res == ETIMEDOUT && ms >= 45 && ms < 1000, detail);
}

/*------------------ Thread-local storage ------------------*/
static _Thread_local int tls_value = 7;

static void *tls_thread(void *arg) {
    int before = tls_value;
    tls_value = (int)(intptr_t)arg;
    usleep(20000);
    return (void *)(intptr_t)(before == 7 && tls_value == (int)(intptr_t)arg);
}

static void test_tls(void) {
    pthread_t a, b;
    void *ra = NULL, *rb = NULL;
    tls_value = 1;
    pthread_create(&a, NULL, tls_thread, (void *)100);
    pthread_create(&b, NULL, tls_thread, (void *)200);
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    check("thread-local storage is per thread", ra && rb && tls_value == 1, "");
}

/*------------------ Heap ------------------*/
static void *heap_worker(void *arg) {
    uint32_t seed = (uint32_t)(intptr_t)arg;
    void *slots[64] = {0};
    size_t sizes[64] = {0};
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) % 64;
        if (slots[slot] != NULL) {
            uint8_t *p = slots[slot];
            for (size_t j = 0; j < sizes[slot]; j++) {
                if (p[j] != (uint8_t)(slot ^ (int)j)) {
                    return (void *)1;
                }
            }
            free(p);
        }
        sizes[slot] = 1 + (seed >> 16) % 3000;
        uint8_t *p = malloc(sizes[slot]);
        if (p == NULL) {
            return (void *)1;
        }
        for (size_t j = 0; j < sizes[slot]; j++) {
            p[j] = (uint8_t)(slot ^ (int)j);
        }
        slots[slot] = p;
    }
    for (int i = 0; i < 64; i++) {
        free(slots[i]);
    }
    return NULL;
}

static void test_heap(void) {
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        pthread_create(&threads[i], NULL, heap_worker, (void *)(intptr_t)(i + 1));
    }
    bool ok = true;
    for (int i = 0; i < WORKERS; i++) {
        void *res = NULL;
        pthread_join(threads[i], &res);
        ok = ok && res == NULL;
    }
    check("malloc/free from 4 threads at once", ok, "");
}

/*------------------ Detach and exit ------------------*/
static atomic_int detached_ran = 0;

static void *detached(void *arg) {
    (void)arg;
    atomic_store(&detached_ran, 1);
    return NULL;
}

static void *exiting(void *arg) {
    (void)arg;
    pthread_exit((void *)42);
    return NULL;
}

static void test_detach_exit(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    bool ok = pthread_create(&t, &attr, detached, NULL) == 0;
    for (int i = 0; i < 200 && !atomic_load(&detached_ran); i++) {
        usleep(10000);
    }
    check("detached thread runs", ok && atomic_load(&detached_ran), "");

    void *res = NULL;
    pthread_create(&t, NULL, exiting, NULL);
    pthread_join(t, &res);
    snprintf(detail, sizeof(detail), "join returned %ld", (long)(intptr_t)res);
    check("pthread_exit value reaches pthread_join", res == (void *)42, detail);
}

/*------------------ zenoh-pico on threads ------------------*/
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int samples;
    int replies;
    bool done;
} zenoh_state_t;

static zenoh_state_t zs = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, false};

static void on_sample(z_loaned_sample_t *sample, void *ctx) {
    (void)sample;
    (void)ctx;
    pthread_mutex_lock(&zs.mutex);
    zs.samples++;
    pthread_cond_broadcast(&zs.cond);
    pthread_mutex_unlock(&zs.mutex);
}

static void on_query(z_loaned_query_t *query, void *ctx) {
    (void)ctx;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "threads/q");
    z_owned_bytes_t payload;
    z_bytes_copy_from_str(&payload, "answer");
    z_query_reply(query, z_loan(ke), z_move(payload), NULL);
}

static void on_reply(z_loaned_reply_t *reply, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&zs.mutex);
    if (z_reply_is_ok(reply)) {
        zs.replies++;
    }
    pthread_mutex_unlock(&zs.mutex);
}

static void on_reply_done(void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&zs.mutex);
    zs.done = true;
    pthread_cond_broadcast(&zs.cond);
    pthread_mutex_unlock(&zs.mutex);
}

static bool wait_for(bool (*cond)(void), int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&zs.mutex);
    while (!cond()) {
        if (pthread_cond_timedwait(&zs.cond, &zs.mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    bool ok = cond();
    pthread_mutex_unlock(&zs.mutex);
    return ok;
}

#define PUTS 200
#define BURST 10000
static bool all_samples(void) { return zs.samples >= PUTS; }
static bool all_burst(void) { return zs.samples >= BURST; }
static bool query_done(void) { return zs.done; }

static bool open_session(z_owned_session_t *s, const char *locator, const char *mode) {
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, mode);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, locator);
    return z_open(s, z_move(config), NULL) == Z_OK;
}

static void test_zenoh(const char *locator) {
    // zenoh-pico starts its read/lease work on its own threads here, as natively.
    z_owned_session_t client, peer;
    bool opened = open_session(&client, locator, "client");
    bool peer_opened = opened && open_session(&peer, locator, "peer");
    snprintf(detail, sizeof(detail), "client %s, peer %s", opened ? "open" : "failed", peer_opened ? "open" : "failed");
    check("client and peer sessions open", opened && peer_opened, detail);
    if (!peer_opened) {
        if (opened) {
            z_drop(z_move(client));
        }
        return;
    }

    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "threads/**");
    z_owned_closure_sample_t sample_cb;
    z_closure(&sample_cb, on_sample, NULL, NULL);
    z_owned_subscriber_t sub;
    z_declare_subscriber(z_loan(peer), &sub, z_loan(ke), z_move(sample_cb), NULL);
    z_owned_closure_query_t query_cb;
    z_closure(&query_cb, on_query, NULL, NULL);
    z_owned_queryable_t queryable;
    z_declare_queryable(z_loan(peer), &queryable, z_loan(ke), z_move(query_cb), NULL);
    z_sleep_ms(300);

    z_view_keyexpr_t put_ke;
    z_view_keyexpr_from_str(&put_ke, "threads/data");
    for (int i = 0; i < PUTS; i++) {
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, "hello from a thread");
        z_put(z_loan(client), z_loan(put_ke), z_move(payload), NULL);
    }
    bool got = wait_for(all_samples, 5000);
    snprintf(detail, sizeof(detail), "%d of %d", zs.samples, PUTS);
    check("peer session receives the client's puts", got, detail);

    z_owned_closure_reply_t reply_cb;
    z_closure(&reply_cb, on_reply, on_reply_done, NULL);
    z_view_keyexpr_t q_ke;
    z_view_keyexpr_from_str(&q_ke, "threads/q");
    z_get(z_loan(client), z_loan(q_ke), "", z_move(reply_cb), NULL);
    bool done = wait_for(query_done, 5000);
    snprintf(detail, sizeof(detail), "%d replies", zs.replies);
    check("get answered by the peer's queryable", done && zs.replies == 1, detail);

    // A batched burst followed at once by closing the sender: everything must still go out
    // (the page must send what is queued before closing the WebSocket).
    z_owned_session_t sender;
    if (open_session(&sender, locator, "client")) {
        pthread_mutex_lock(&zs.mutex);
        zs.samples = 0;
        pthread_mutex_unlock(&zs.mutex);
        z_owned_publisher_t pub;
        z_publisher_options_t opts;
        z_publisher_options_default(&opts);
        opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
        z_view_keyexpr_t burst_ke;
        z_view_keyexpr_from_str(&burst_ke, "threads/burst");
        z_declare_publisher(z_loan(sender), &pub, z_loan(burst_ke), &opts);
        z_sleep_ms(300);
        zp_batch_start(z_loan(sender));
        for (int i = 0; i < BURST; i++) {
            z_owned_bytes_t payload;
            z_bytes_copy_from_str(&payload, "burst message of some length");
            z_publisher_put(z_loan(pub), z_move(payload), NULL);
        }
        zp_batch_stop(z_loan(sender));
        z_drop(z_move(pub));
        z_drop(z_move(sender));
        bool all = wait_for(all_burst, 5000);
        snprintf(detail, sizeof(detail), "%d of %d", zs.samples, BURST);
        check("a burst sent right before closing is delivered", all, detail);
    } else {
        check("a burst sent right before closing is delivered", false, "sender did not open");
    }

    z_drop(z_move(queryable));
    z_drop(z_move(sub));
    z_drop(z_move(peer));
    z_drop(z_move(client));
    check("sessions close and their threads end", true, "");
}

int main(int argc, char **argv) {
    test_mutex();
    test_recursive();
    test_condvar();
    test_tls();
    test_heap();
    test_detach_exit();
    if (argc > 1) {
        test_zenoh(argv[1]);
    }
    printf("%d failure(s)\n", failures);
    return failures;
}
