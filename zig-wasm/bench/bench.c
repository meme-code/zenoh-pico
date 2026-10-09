//
// Browser benchmark workload, compiled unchanged into both the Zig build (bench_zig.c) and the
// Emscripten build (bench_emscripten.c). Only the zenoh-pico platform layer underneath differs.
//
// Results are printed as one line: "BENCH {json}". Timing uses clock_gettime(CLOCK_MONOTONIC),
// which is performance.now() in both builds.
//

#include "bench.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zenoh-pico.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static volatile unsigned long g_pongs = 0;
static volatile unsigned long g_rx = 0;
static double g_first_rx_us = 0;
static double g_last_rx_us = 0;

static void on_pong(z_loaned_sample_t *sample, void *ctx) {
    (void)sample;
    (void)ctx;
    g_pongs++;
}

static void on_thr(z_loaned_sample_t *sample, void *ctx) {
    (void)sample;
    (void)ctx;
    double t = now_us();
    if (g_rx == 0) g_first_rx_us = t;
    g_last_rx_us = t;
    g_rx++;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int put(const z_loaned_publisher_t *pub, const uint8_t *data, size_t len) {
    z_owned_bytes_t payload;
    z_bytes_copy_from_buf(&payload, data, len);
    return z_publisher_put(pub, z_move(payload), NULL);
}

// Latency: n round trips through the router to a native z_pong (test/ping -> test/pong).
static int bench_ping(const z_loaned_session_t *s, int n, int size, double open_ms) {
    z_owned_publisher_t pub;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "test/ping");
    if (z_declare_publisher(s, &pub, z_loan(ke), NULL) < 0) return -1;
    z_owned_subscriber_t sub;
    z_owned_closure_sample_t cb;
    z_closure(&cb, on_pong, NULL, NULL);
    z_view_keyexpr_from_str(&ke, "test/pong");
    if (z_declare_subscriber(s, &sub, z_loan(ke), z_move(cb), NULL) < 0) return -1;

    uint8_t *data = malloc((size_t)size);
    memset(data, 1, (size_t)size);

    // Until the pong peer and the publisher's write filter are ready, pings may be dropped.
    double start = now_us();
    while (g_pongs == 0 && now_us() - start < 10e6) {
        put(z_loan(pub), data, (size_t)size);
        double t = now_us();
        while (g_pongs == 0 && now_us() - t < 100e3) zp_spin_once(s);
    }
    if (g_pongs == 0) {
        printf("BENCH {\"mode\":\"ping\",\"error\":\"no pong\"}\n");
        return -1;
    }
    // Absorb pongs for any other pings still in flight from the loop above.
    double drain = now_us();
    while (now_us() - drain < 300e3) zp_spin_once(s);

    const int warmup = 50;
    double *rtt = malloc(sizeof(double) * (size_t)n);
    int got = 0, lost = 0;
    for (int i = 0; i < warmup + n; i++) {
        unsigned long before = g_pongs;
        double t = now_us();
        put(z_loan(pub), data, (size_t)size);
        while (g_pongs == before && now_us() - t < 1e6) zp_spin_once(s);
        double dt = now_us() - t;
        if (i < warmup) continue;
        if (g_pongs == before) {
            lost++;
        } else {
            rtt[got++] = dt;
        }
    }
    qsort(rtt, (size_t)got, sizeof(double), cmp_double);
    double sum = 0;
    for (int i = 0; i < got; i++) sum += rtt[i];
    if (got > 0) {
        printf("BENCH {\"mode\":\"ping\",\"n\":%d,\"size\":%d,\"lost\":%d,\"open_ms\":%.2f,\"min_us\":%.0f,"
               "\"p50_us\":%.0f,\"p90_us\":%.0f,\"p99_us\":%.0f,\"max_us\":%.0f,\"mean_us\":%.0f}\n",
               got, size, lost, open_ms, rtt[0], rtt[got / 2], rtt[got * 9 / 10], rtt[got * 99 / 100], rtt[got - 1],
               sum / got);
    } else {
        printf("BENCH {\"mode\":\"ping\",\"error\":\"all pings lost\"}\n");
    }
    free(rtt);
    free(data);
    z_drop(z_move(sub));
    z_drop(z_move(pub));
    return 0;
}

// Spins until the router has reported a subscriber for `pub` (its write filter is open).
static bool wait_matching(const z_loaned_session_t *s, const z_loaned_publisher_t *pub, double timeout_us) {
    double t = now_us();
    while (now_us() - t < timeout_us) {
        z_matching_status_t status;
        if (z_publisher_get_matching_status(pub, &status) == Z_OK && status.matching) return true;
        zp_spin_once(s);
    }
    return false;
}

// Send: put n `size`-byte messages as fast as possible; the native peer counts what arrives.
static int bench_pub(const z_loaned_session_t *s, int n, int size, bool batch) {
    const char *mode = batch ? "pubbatch" : "pub";
    z_owned_publisher_t pub;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "thr");
    if (z_declare_publisher(s, &pub, z_loan(ke), NULL) < 0) return -1;
    if (!wait_matching(s, z_loan(pub), 10e6)) {
        printf("BENCH {\"mode\":\"%s\",\"error\":\"no matching subscriber\"}\n", mode);
        z_drop(z_move(pub));
        return -1;
    }
    uint8_t *data = malloc((size_t)size);
    memset(data, 1, (size_t)size);

    if (batch) zp_batch_start(s);
    unsigned long failed = 0;
    double start = now_us();
    for (int i = 0; i < n; i++) {
        if (put(z_loan(pub), data, (size_t)size) != Z_OK) failed++;
    }
    if (batch) zp_batch_stop(s);
    double send_ms = (now_us() - start) / 1e3;
    printf("BENCH {\"mode\":\"%s\",\"n\":%d,\"size\":%d,\"failed\":%lu,\"send_ms\":%.2f,\"send_rate\":%.0f}\n",
           mode, n, size, failed, send_ms, n / (send_ms / 1e3));
    free(data);
    z_drop(z_move(pub));
    return 0;
}

// Receive: wait for n messages from a native publisher (which starts after READY).
static int bench_sub(const z_loaned_session_t *s, int n) {
    z_owned_subscriber_t sub;
    z_owned_closure_sample_t cb;
    z_closure(&cb, on_thr, NULL, NULL);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "thr");
    if (z_declare_subscriber(s, &sub, z_loan(ke), z_move(cb), NULL) < 0) return -1;
    printf("READY\n");

    double start = now_us();
    while (g_rx < (unsigned long)n) {
        zp_spin_once(s);
        double now = now_us();
        if (g_rx == 0 && now - start > 30e6) break;
        if (g_rx > 0 && now - g_last_rx_us > 5e6) break;
    }
    double span_ms = (g_last_rx_us - g_first_rx_us) / 1e3;
    printf("BENCH {\"mode\":\"sub\",\"n\":%d,\"received\":%lu,\"recv_ms\":%.2f,\"recv_rate\":%.0f}\n", n,
           (unsigned long)g_rx, span_ms, g_rx > 1 && span_ms > 0 ? (g_rx - 1) / (span_ms / 1e3) : 0.0);
    if (g_rx < (unsigned long)n) {
        z_drop(z_move(sub));
        return -1;
    }
    z_drop(z_move(sub));
    return 0;
}

// Idle: an open session with a subscriber and no traffic, spinning for duration_ms.
// The driver measures the browser's CPU time between IDLE_START and IDLE_END.
static int bench_idle(const z_loaned_session_t *s, int duration_ms) {
    z_owned_subscriber_t sub;
    z_owned_closure_sample_t cb;
    z_closure(&cb, on_thr, NULL, NULL);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "idle/**");
    if (z_declare_subscriber(s, &sub, z_loan(ke), z_move(cb), NULL) < 0) return -1;
    printf("IDLE_START\n");
    unsigned long spins = 0;
    double start = now_us();
    while (now_us() - start < duration_ms * 1e3) {
        zp_spin_once(s);
        spins++;
    }
    printf("IDLE_END\n");
    printf("BENCH {\"mode\":\"idle\",\"seconds\":%.3f,\"spins\":%lu}\n", (now_us() - start) / 1e6, spins);
    z_drop(z_move(sub));
    return 0;
}

int bench_run(const char *connect, const char *mode, int n, int size) {
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, Z_CONFIG_MODE_CLIENT);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, connect);

    double t = now_us();
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        printf("BENCH {\"mode\":\"%s\",\"error\":\"z_open failed\"}\n", mode);
        return -1;
    }
    double open_ms = (now_us() - t) / 1e3;

    int res;
    if (strcmp(mode, "open") == 0) {
        printf("BENCH {\"mode\":\"open\",\"open_ms\":%.2f}\n", open_ms);
        res = 0;
    } else if (strcmp(mode, "ping") == 0) {
        res = bench_ping(z_loan(s), n, size, open_ms);
    } else if (strcmp(mode, "pub") == 0) {
        res = bench_pub(z_loan(s), n, size, false);
    } else if (strcmp(mode, "pubbatch") == 0) {
        res = bench_pub(z_loan(s), n, size, true);
    } else if (strcmp(mode, "sub") == 0) {
        res = bench_sub(z_loan(s), n);
    } else if (strcmp(mode, "idle") == 0) {
        res = bench_idle(z_loan(s), n);
    } else {
        printf("BENCH {\"mode\":\"%s\",\"error\":\"unknown mode\"}\n", mode);
        res = -1;
    }
    z_drop(z_move(s));
    printf("DONE\n");
    return res;
}
