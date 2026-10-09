#ifndef ZENOH_WASM_BENCH_H
#define ZENOH_WASM_BENCH_H

// Runs one benchmark over a fresh client session to `connect`.
//   mode "open"     just open and close the session
//   mode "ping"     n round trips with `size`-byte payloads (needs a z_pong peer)
//   mode "pub"      put n `size`-byte messages on "thr" as fast as possible (needs a counting peer)
//   mode "pubbatch" same, inside zp_batch_start/zp_batch_stop
//   mode "sub"      receive n messages on "thr" (needs a flooding peer, started after READY)
//   mode "idle"     spin an idle session for n ms
int bench_run(const char *connect, const char *mode, int n, int size);

#endif
