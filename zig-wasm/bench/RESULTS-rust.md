# zenoh in the browser: Zig port of zenoh-pico vs. Rust zenoh

Measured 2026-10-20 on Apple M5, Darwin 25.6.0. Chrome/155.0.8059.85 (headless, background throttling off), zenohd 1.10.1. Zig side: zenoh-pico 1.10.1 with this port as of the commit that adds this file, Zig 0.17.0 (ReleaseFast). Rust side: https://github.com/0x53A/zenoh.git@419088e38c1b (community wasm port of Rust zenoh, not upstream), rustc 1.101.0-nightly (07e526db9 2026-10-19), wasm-bindgen 0.2.127, release with LTO.

All pages run the same workload (`bench/bench.c`; `bench/rust` is its Rust translation) against one local
`zenohd` (WebSocket listener). The other end is native zenoh-pico over TCP: upstream `z_pong` for latency,
`bench/peer.c` for throughput. Timing is wall clock from [hyperfine](https://github.com/sharkdp/hyperfine)
(2 warmups, 10 runs per command). Each run is one `node bench/run-one.mjs` that opens a fresh
page in an already running Chrome and waits until the work is done, so every time also includes starting
Node, loading the page, compiling the wasm (and, for the worker pool, starting its workers) and opening the
session; the "Open a session" row is that fixed cost on its own.

Columns: **Zig (zenoh-pico)** is this port. **Rust, worker pool** is the Rust port with its SharedArrayBuffer
worker pool (the workload runs on its Application worker). **Rust, single thread** is the same code built
without the pool, running on the page's event loop. Ratios are relative to Zig (>1× means Zig is better).

## End-to-end time (hyperfine)

| Scenario | Zig (zenoh-pico) | Rust, worker pool | Rust, single thread |
|---|---:|---:|---:|
| Open a session | 85.6 ms ± 1.3 ms | 110.7 ms ± 11.3 ms (1.3×) | 92.0 ms ± 3.6 ms (1.1×) |
| 200 round trips (64 B) | 554.9 ms ± 6.4 ms | 795.9 ms ± 6.7 ms (1.4×) | 1.652 s ± 10.3 ms (3.0×) |
| Send 500 × 64 B, one put each | 93.0 ms ± 1.2 ms | 777.6 ms ± 4.5 ms (8.4×) | not run |
| Send 10,000 × 64 B, batched | 96.9 ms ± 3.2 ms | 127.0 ms ± 1.4 ms (1.3×) | not run |
| Receive 5,000 × 64 B | 106.5 ms ± 2.0 ms | 131.2 ms ± 2.4 ms (1.2×) | 120.0 ms ± 5.1 ms (1.1×) |

Mean ± standard deviation of the successful runs; in parentheses, how many times longer than Zig. Raw
hyperfine exports (all runs, min/max/median, CPU and RSS of the client) are next to this report as
`<scenario>.json` / `.md`.

## Inside the page

Measured by the workload itself with `performance.now()`; medians over the successful measured runs.

| Metric | Zig (zenoh-pico) | Rust, worker pool | Rust, single thread |
|---|---:|---:|---:|
| Session open (`z_open` / `zenoh::open`) | 1.76 ms | 13.06 ms (7.4×) | 10.82 ms (6.1×) |
| Worker pool start-up (before open) | not run | 10.82 ms | 0.00 ms |
| Round trip p50 (64 B, via router to native z_pong) | 0.17 ms | 1.47 ms (8.9×) | 4.86 ms (29×) |
| Round trip p99 | 0.39 ms | 1.79 ms (4.6×) | 6.21 ms (16×) |
| Send, one message per frame (64 B): delivered to native peer | 111,050 msg/s | 782 msg/s (142×) | not run |
| Send, batched (64 B): delivered | 1,156,156 msg/s | 677,898 msg/s (1.7×) | not run |
| Receive from native publisher (64 B) | 299,837 msg/s | 327,330 msg/s (0.9×) | 334,608 msg/s (0.9×) |

Ratios: how many times slower (or lower) than Zig. "Worker pool start-up" only exists for the Rust pool;
Zig starts one worker, which is part of the end-to-end time but not of any in-page metric.

## Idle CPU

CPU time of all of this Chrome's processes while one session sits idle (subscriber declared, no traffic),
10 s windows, median of 3. Not measurable with hyperfine, which only sees the client command.

| | % of one core |
|---|---:|
| Blank page (baseline) | 0.5 |
| Zig (zenoh-pico) | 1.1 |
| Rust, worker pool | 9.3 |
| Rust, single thread | 1.4 |

## Download size

| Build | wasm | JS | total gzipped |
|---|---:|---:|---:|
| Zig, ReleaseSmall (`zenoh-pico.wasm`, the shipped build) | 123.0 KiB (45.0 KiB gz) | 43.6 KiB (13.1 KiB gz) | **58.0 KiB** |
| Zig, ReleaseFast (benchmark build) | 174.4 KiB (62.0 KiB gz) | 43.6 KiB (13.1 KiB gz) | **75.1 KiB** |
| Rust, worker pool (benchmark build) | 4.80 MiB (1.32 MiB gz) | 38.7 KiB (7.7 KiB gz) | **1.33 MiB** |
| Rust, single thread (benchmark build) | 4.42 MiB (1.27 MiB gz) | 26.9 KiB (5.6 KiB gz) | **1.27 MiB** |

The Rust builds are opt-level 3 with fat LTO and no wasm-opt pass, like the port's own published demos.
Each also contains its benchmark workload; the Zig benchmark wasm contains `bench.c`.

## How the workload maps onto Rust zenoh

* "One put each" uses `express(true)`: each message is sent in its own frame, as zenoh-pico does without
  `zp_batch_start`. "Batched" uses Rust zenoh's automatic batching (zenoh-pico: `zp_batch_start`).
* Publishers use congestion control BLOCK in both, so the native peer must receive every message.
* The single-threaded Rust build is not run in the send scenarios: on the page thread the port's publisher
  cannot wait for a free transmit batch (`lock_for_publish` / `wait_refill` in its `pipeline.rs`), so a tight
  put loop drops most messages even with BLOCK while `put` still returns `Ok` (2 of 500 and 1,872 of 10,000
  delivered in a trial run).
* Receiving, the port copies each WebSocket frame into a queue bounded at 128 frames and fails the link when
  it is full ("WebSocket receive backlog exceeded"; browsers have no receive-side backpressure, see its
  `WASM.md`). A fast native publisher can therefore cost a Rust run messages; such runs count as failed above.

## What the Rust port needs

* The `wasm` branch of a fork of Rust zenoh (`0x53A/zenoh`), not a released crate.
* Worker pool: nightly Rust with `-Zbuild-std`, atomics/shared-memory link flags, wasm-bindgen 0.2.127, and a
  cross-origin isolated page (COOP/COEP), like the Zig port.

Caveats: one machine, loopback networking, headless Chrome only.
