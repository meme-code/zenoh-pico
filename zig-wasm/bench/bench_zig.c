// Zig/WASI build entry point: the worker calls this export through zenoh.js.
#include "bench.h"

__attribute__((export_name("zw_bench"))) int zw_bench(const char *connect, const char *mode, int n, int size) {
    return bench_run(connect, mode, n, size);
}
