//
// Native counterpart for the browser benchmark (zenoh-pico, multi-threaded, TCP).
//
//   peer count <connect> <n>          subscribes "thr", prints READY, exits once n samples arrived
//                                     (or after 5 s of silence): PEER {"received":N,"recv_ms":T,...}
//   peer flood <connect> <n> <size>   waits for a matching subscriber, publishes n samples on "thr"
//                                     with congestion control BLOCK (nothing dropped), then exits
//
// Latency uses zenoh-pico's own examples/unix/c11/z_pong.c.
//

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zenoh-pico.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static atomic_ulong g_count;
static _Atomic double g_first, g_last;

static void on_sample(z_loaned_sample_t *sample, void *ctx) {
    (void)sample;
    (void)ctx;
    double t = now_s();
    if (atomic_fetch_add(&g_count, 1) == 0) atomic_store(&g_first, t);
    atomic_store(&g_last, t);
}

static int count(const z_loaned_session_t *s, unsigned long expected) {
    z_owned_closure_sample_t cb;
    z_closure(&cb, on_sample, NULL, NULL);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "thr");
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(s, &sub, z_loan(ke), z_move(cb), NULL) < 0) return -1;
    printf("READY\n");
    fflush(stdout);
    double start = now_s();
    for (;;) {
        z_sleep_ms(1);
        unsigned long n = atomic_load(&g_count);
        if (n >= expected) break;
        if (n == 0 && now_s() - start > 60) break;
        if (n > 0 && now_s() - atomic_load(&g_last) > 5) break;
    }
    unsigned long n = atomic_load(&g_count);
    double span = atomic_load(&g_last) - atomic_load(&g_first);
    printf("PEER {\"received\":%lu,\"recv_ms\":%.2f,\"recv_rate\":%.0f}\n", n, span * 1e3,
           n > 1 && span > 0 ? (n - 1) / span : 0.0);
    z_drop(z_move(sub));
    return n >= expected ? 0 : 1;
}

static int flood(const z_loaned_session_t *s, unsigned long n, int size) {
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "thr");
    z_publisher_options_t opts;
    z_publisher_options_default(&opts);
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;  // deliver all n, however slow the reader
    z_owned_publisher_t pub;
    if (z_declare_publisher(s, &pub, z_loan(ke), &opts) < 0) return -1;
    double start = now_s();
    for (;;) {
        z_matching_status_t status;
        if (z_publisher_get_matching_status(z_loan(pub), &status) == Z_OK && status.matching) break;
        if (now_s() - start > 10) {
            printf("PEER {\"error\":\"no matching subscriber\"}\n");
            return 1;
        }
        z_sleep_ms(1);
    }
    uint8_t *data = malloc((size_t)size);
    memset(data, 1, (size_t)size);
    unsigned long sent = 0;
    start = now_s();
    for (unsigned long i = 0; i < n; i++) {
        z_owned_bytes_t payload;
        z_bytes_copy_from_buf(&payload, data, (size_t)size);
        if (z_publisher_put(z_loan(pub), z_move(payload), NULL) == Z_OK) sent++;
    }
    double secs = now_s() - start;
    printf("PEER {\"sent\":%lu,\"send_ms\":%.2f,\"send_rate\":%.0f}\n", sent, secs * 1e3, sent / secs);
    free(data);
    z_drop(z_move(pub));
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: peer count <connect> <n> | peer flood <connect> <n> <size>\n");
        return 2;
    }
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, argv[2]);
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        printf("PEER {\"error\":\"z_open failed\"}\n");
        return 1;
    }
    int res;
    if (strcmp(argv[1], "count") == 0 && argc >= 4) {
        res = count(z_loan(s), strtoul(argv[3], NULL, 10));
    } else if (strcmp(argv[1], "flood") == 0 && argc >= 5) {
        res = flood(z_loan(s), strtoul(argv[3], NULL, 10), atoi(argv[4]));
    } else {
        fprintf(stderr, "unknown mode\n");
        res = 2;
    }
    fflush(stdout);
    // Let queued samples reach the router before the session closes.
    z_sleep_ms(200);
    z_drop(z_move(s));
    return res;
}
