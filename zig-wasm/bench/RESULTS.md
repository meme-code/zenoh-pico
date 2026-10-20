# zenoh-pico in the browser: Zig port vs. Emscripten port

> Measured on the first version of this port (commit 03c330d3: single-threaded only, WebSocket rings in
> separate SharedArrayBuffers). The Zig numbers in `RESULTS-rust.md` are from the current version, which
> keeps the rings in wasm memory and adds threads; its in-page Zig figures are in the same range.

Measured 2026-10-09 on Apple M5, Darwin 25.6.0. Chrome/154.0.8037.98 (headless, background throttling off), zenohd 1.10.1, zenoh-pico 1.10.1-1-gd9b4eea4. Zig 0.17.0 (ReleaseFast), Emscripten 6.0.12 (-O3, Asyncify).

Both pages run the same `bench/bench.c` against one local `zenohd` (WebSocket listener). The other end is
native zenoh-pico over TCP: upstream `z_pong` for latency, `bench/peer.c` for throughput. Timing is wall clock
from [hyperfine](https://github.com/sharkdp/hyperfine) (2 warmups, 10 runs per command). Each run is one
`node bench/run-one.mjs` that opens a fresh page in an already running Chrome and waits until the work is done,
so every time also includes starting Node, loading the page, compiling the wasm and opening the session; the
"Open a session" row is that fixed cost on its own.

## End-to-end time (hyperfine)

| Scenario | Zig | Emscripten | Zig is |
|---|---:|---:|---:|
| Open a session | 83.3 ms ± 1.1 ms | 195.3 ms ± 7.1 ms | **2.34 ± 0.09× faster** |
| 200 round trips (64 B) | 553.1 ms ± 3.0 ms | 3.007 s ± 6.9 ms | **5.44 ± 0.03× faster** |
| Send 500 × 64 B, one put each | 91.9 ms ± 3.6 ms | 2.498 s ± 7.7 ms | **27.2 ± 1.1× faster** |
| Send 10,000 × 64 B, batched | 94.1 ms ± 1.6 ms | 1.757 s ± 2.3 ms | **18.7 ± 0.3× faster** |
| Receive 5,000 × 64 B | 105.0 ms ± 2.4 ms | 6.123 s ± 318.4 ms | **58.3 ± 3.3× faster** |

Mean ± standard deviation. Raw hyperfine exports (all runs, min/max/median, CPU and RSS of the client) are
next to this report as `<scenario>.json` / `.md`.

## Inside the page

Measured by `bench.c` itself with `clock_gettime(CLOCK_MONOTONIC)` (performance.now() in both builds);
medians over the measured hyperfine runs.

| Metric | Zig | Emscripten | Zig is |
|---|---:|---:|---:|
| `z_open` (session handshake) | 1.75 ms | 107.85 ms | 62× faster |
| Round trip p50 (64 B, via router to native z_pong) | 0.18 ms | 9.42 ms | 52× faster |
| Round trip p99 | 0.37 ms | 9.93 ms | 27× faster |
| Send, one put per message (64 B): delivered to native peer | 94,541 msg/s | 221 msg/s | 428× more |
| Send with zp_batch_start (64 B): delivered | 1,195,317 msg/s | 6,574 msg/s | 182× more |
| Receive from native publisher (64 B) | 301,650 msg/s | 858 msg/s | 352× more |

## Idle CPU

CPU time of all of this Chrome's processes while one session sits idle (subscriber declared, no traffic),
10 s windows, median of 3. Not measurable with hyperfine, which only sees the client command.

| | % of one core |
|---|---:|
| Blank page (baseline) | 0.5 |
| Zig | 0.9 |
| Emscripten | 2.0 |

## Download size

| Build | wasm | JS | total gzipped |
|---|---:|---:|---:|
| Zig, ReleaseSmall (`zenoh-pico.wasm`, the shipped build) | 101.0 KiB (37.3 KiB gz) | 29.7 KiB (9.3 KiB gz) | **46.7 KiB** |
| Zig, ReleaseFast (benchmark build) | 151.0 KiB (53.9 KiB gz) | 29.7 KiB (9.3 KiB gz) | **63.3 KiB** |
| Emscripten, -Oz link | 177.6 KiB (63.2 KiB gz) | 74.9 KiB (21.3 KiB gz) | **84.5 KiB** |
| Emscripten, -O3 (benchmark build) | 181.4 KiB (64.0 KiB gz) | 75.3 KiB (21.5 KiB gz) | **85.5 KiB** |

The Zig benchmark wasm also contains `bench.c`; the shipped `zenoh-pico.wasm` contains the JS API exports instead.
The Emscripten builds are the benchmark program (no JS API exists for that port). Upstream CMake compiles the
library with `-O3` regardless, so `-Oz` only affects the final link.

## What was needed to run the Emscripten port

* `-DZ_FEATURE_MULTI_THREAD=0`: single-threaded like the Zig build (no SharedArrayBuffer/pthreads needed).
  With it, upstream fails to compile (`size_t` unknown in `platform.h`); worked around with `-include stddef.h`.
* `-sASYNCIFY`: the port blocks with `emscripten_sleep`.
* `-sWEBSOCKET_SUBPROTOCOL=null`: by default Emscripten requests the `binary` subprotocol, zenohd answers none,
  and Chrome fails the handshake ("Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received");
  `z_open` then hangs without an error.

## Why the numbers look like this

* The Emscripten port waits by sleeping: every WebSocket read and write first calls `emscripten_sleep(1)`
  (`src/link/transport/upper/ws_emscripten.c`), i.e. `await new Promise(r => setTimeout(r, 1))` resumed through
  Asyncify. Browsers clamp nested timers to at least 4 ms, consistent with the ~5 ms per unbatched send and
  ~10 ms round trips measured here. `z_open` also sleeps a fixed 100 ms after `connect()`.
* The Zig port blocks a Web Worker with `Atomics.wait` and is woken by `Atomics.notify` as soon as the page
  thread puts a received WebSocket message into the shared ring, so reads complete without timer delays.
* Batching (`zp_batch_start`) helps Emscripten most because it amortizes the per-write sleep over many messages.

Caveats: one machine, loopback networking, headless Chrome only. Real networks add the same RTT to both ports, so
the relative gap in latency shrinks on slow links; the per-write sleep cap on Emscripten throughput does not.
