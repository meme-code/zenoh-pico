# zenoh-pico for the browser, built with Zig

[zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) 1.10.1 compiled to WebAssembly with
Zig 0.17 alone (no Emscripten), plus a small JavaScript API. A browser page opens a client session
to a zenoh router over WebSocket and can put, delete, subscribe, publish and query.

```js
import { Session } from './zenoh.js';

const session = await Session.open({ connect: 'ws/127.0.0.1:10000' });
await session.declareSubscriber('demo/**', (s) => console.log(s.kind, s.keyexpr, s.text()));
await session.put('demo/hello', 'world', { encoding: 'text/plain' });
const replies = await session.get('demo/**', { timeoutMs: 3000 });
await session.close();
```

## Build and run

Everything lives in this directory (`zig-wasm/`) of the zenoh-pico repository and builds zenoh-pico's own
sources from `..` without modifying them. Needs Zig 0.17.

```sh
cd zig-wasm
zig build -Drelease            # -> zig-out/web/ (zenoh-pico.wasm, zenoh.js, zenoh-worker.js, zenoh-shared.js, index.html)
zenohd -l ws/127.0.0.1:10000   # a zenoh router with a WebSocket listener (zenoh 1.10.x)
zig build serve -Drelease      # http://127.0.0.1:8000/ demo page
```

`zig build` without `-Drelease` gives a Debug build with UBSan (about 3.9 MB). `-Drelease` is
ReleaseSmall: 103 KB, 38 KB gzipped, and it imports nothing but the ten `zenoh_host` functions.

| Option | Default | |
|---|---|---|
| `-Drelease` | off | ReleaseSmall |
| `-Dlog-level=N` | 0 | zenoh-pico logging to the console: 1 error, 2 info, 3 debug |
| `-Dsocket-timeout=MS` | 100 | read timeout per spin; also the handshake read timeout |
| `-Dbatch-size=B` | 2048 | unicast batch size |
| `-Dfrag-max-size=B` | 4096 | largest reassembled message |
| `-Dport=P` | 8000 | port for `zig build serve` |

The per-read timeout can also be set on the locator: `ws/example.com:10000#tout=1000`.

### Hosting requirement

The page must be **cross-origin isolated**, because the worker blocks with `Atomics.wait` on a
`SharedArrayBuffer`. Serve it with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`tools/serve.py` (used by `zig build serve`) does this. `Session.open` throws a clear error when the
headers are missing. Hosts that cannot set headers (e.g. GitHub Pages) need a service-worker shim
such as coi-serviceworker.

On an `https:` page the default WebSocket URL is `wss://host:port/`, otherwise `ws://`. Pass
`wsUrl: (host, port) => '...'` to `Session.open` to map the locator to any URL (path, proxy, TLS).

## Test

```sh
ZENOHD=/path/to/zenohd zig build test -Drelease   # or without -Drelease for the UBSan build
```

This starts `zenohd` (with an in-memory storage on `demo/**`, so `zenohd`'s directory must contain
the `storage_manager` plugin), `tools/serve.py` and headless Chrome. It then runs
`tests/browser-test.html`, where two browser sessions talk through the router, followed by a router
restart. Needs node, python3 and Chrome (`CHROME=` to override).

Checked: ordered delivery of 50 puts, encodings, publisher binary payloads, deletes, a fragmented
3000-byte payload, 100 ping/pong round trips, a 2000-message burst, `get` against the router's
storage, undeclare, close, distinct fully random session ids, and recovery after `zenohd` is killed
and restarted (the subscriber is redeclared automatically). On an M-series Mac with a local router,
round trips take about 1 ms (median) and bursts reach roughly 30–47k messages/s across runs.

## How it works

```
 page (main thread)                           Web Worker
 ┌──────────────────────────┐                ┌───────────────────────────────────┐
 │ zenoh.js  Session API    │  postMessage   │ zenoh-worker.js                   │
 │  ├─ calls  ─────────────────────────────▶ │  └─ zenoh-pico.wasm (Zig-built)   │
 │  │  ◀──────────── samples, replies, sends│      zp_spin_once() in a loop     │
 │  └─ WebSocket(s) ────┐   │                │      blocking reads/sleeps:       │
 │      onmessage ──▶ SharedArrayBuffer ring ──▶  Atomics.wait / Atomics.notify  │
 └──────────────────────────┘                └───────────────────────────────────┘
```

* `build.zig` compiles the unmodified upstream sources (same globs as zenoh-pico's CMake),
  generates `config.h` from `config.h.in`, and plugs in the browser platform via
  zenoh-pico's `ZP_SYSTEM_PLATFORM_HEADER` hook. Target: `wasm32-wasi` reactor (Zig provides libc).
* `wasm/` is the browser platform: `system.c` (clock, random via `crypto.getRandomValues`, sleep),
  `ws.c` (zenoh's ws link: one zenoh batch per binary WebSocket message), and `api.c` (the
  `zw_*` exports used by JavaScript). `zenoh_wasm_host.h` lists the imports.
* zenoh-pico is configured single-threaded (`Z_FEATURE_MULTI_THREAD=0`), client mode only, WebSocket
  link only. The worker calls `zp_spin_once` once per macrotask, so API calls interleave with
  network I/O. The main thread interrupts an idle read whenever it posts a call.
* Each `Session` gets its own worker and wasm instance; the compiled module is shared.

## Limitations

* Client mode only: browsers can't listen, so no peer mode, no scouting, no TCP/UDP/serial.
* The JavaScript API covers put, delete, subscribers, publishers and get. Queryables, liveliness and
  advanced pub/sub are in the wasm but not exposed yet.
* A publisher drops puts until the router has reported a matching subscriber (zenoh-pico's write
  filter starts active), so don't publish in the same tick as `declarePublisher`.
* Tested in headless Chrome only. It uses standard APIs that Firefox and Safari also support
  (module workers, SharedArrayBuffer, Atomics.wait), but it hasn't been run there.

## Upstream issues found along the way

1. **Emscripten port: `z_random_fill` writes every byte to `buf[0]`**
   ([#1330](https://github.com/eclipse-zenoh/zenoh-pico/issues/1330))
   (`src/system/emscripten/system.c`). zenoh uses it for session ids (`src/session/utils.c`), so only
   one of 16 bytes is random. This port fills the whole buffer.
2. **32-bit targets: misaligned `_z_slist` values**
   ([#1331](https://github.com/eclipse-zenoh/zenoh-pico/issues/1331)). Values sit right after a 4-byte `next`
   pointer (`src/collections/list.c`, `NODE_DATA_SIZE`), so 8-byte-aligned structs are misaligned.
   This affects wasm32, 32-bit ARM and RV32. UBSan reports it in Debug builds. No misbehavior has
   been seen: wasm allows unaligned access, so the Debug build disables only `-fsanitize=alignment`.
   It is still undefined behavior that the compiler is allowed to rely on.

## Compared with the in-tree Emscripten port

zenoh-pico's own browser support (`src/system/emscripten`) needs emsdk. It blocks through Asyncify,
polls `recv` every 1 ms, and sleeps a fixed 100 ms after `connect` instead of waiting for the
WebSocket to open. It ships no JavaScript API: you write a C program and bridge it yourself.
Unlike this port, it doesn't require cross-origin isolation in single-thread mode, and it's
maintained upstream and compiled in upstream CI.

Running it against a stock `zenohd` also needed `-sWEBSOCKET_SUBPROTOCOL=null`. Without it, Chrome rejects the
WebSocket handshake and `z_open` hangs with no error. Building it single-threaded needed a `stddef.h` workaround,
because `platform.h` uses `size_t` without including it (see `bench/build-emscripten.sh`).

The only prebuilt Emscripten build available is a static library from emscripten-forge
(`zenoh-pico-1.10.1-hd68d874_0`, Emscripten 4.0.9). There's no `.wasm`/`.js` to run. It
still needs an emcc link with Asyncify, and it's configured with `Z_FEATURE_MULTI_THREAD=1` but
built without pthreads.

## Benchmark: this port vs. the Emscripten port

`bench/run.sh` builds zenoh-pico's in-tree Emscripten port (official `emscripten/emsdk` image, upstream CMake)
and this port around the same `bench/bench.c`. It runs both in headless Chrome against one `zenohd`, with native
zenoh-pico peers on the other end, timed with hyperfine. Full results with method and caveats: `bench/RESULTS.md`.

```sh
ZENOHD=/path/to/zenohd bench/run.sh      # needs zig, podman (or CONTAINER=docker), node, python3, Chrome, hyperfine
```

Apple M5, Chrome 154, loopback, 10 runs each after 2 warmups (mean ± σ, end to end per hyperfine run):

| Scenario | Zig port | Emscripten port | |
|---|---:|---:|---:|
| Open a session | 83.3 ms ± 1.1 | 195.3 ms ± 7.1 | 2.3× faster |
| 200 round trips | 553 ms ± 3 | 3.007 s ± 0.007 | 5.4× faster |
| Send 500 messages, one put each | 91.9 ms ± 3.6 | 2.498 s ± 0.008 | 27× faster |
| Send 10,000 messages, batched | 94.1 ms ± 1.6 | 1.757 s ± 0.002 | 19× faster |
| Receive 5,000 messages | 105 ms ± 2 | 6.123 s ± 0.318 | 58× faster |

Measured inside the page: round trip p50 0.18 ms vs 9.42 ms, `z_open` 1.75 ms vs 108 ms. Unbatched sends
deliver 94,541 vs 221 msg/s, and receiving runs at 301,650 vs 858 msg/s. Idle CPU is 0.9% vs 2.0% of a core
(blank page: 0.5%), and the download is 46.7 vs 84.5 KiB gzipped. The Emscripten port sleeps 1 ms (a nested
`setTimeout`, clamped to ≥ 4 ms) before every WebSocket read and write; this port blocks on `Atomics.wait`
and wakes as soon as data arrives.

## Layout

```
build.zig            build, serve, test and bench steps
wasm/                browser platform layer + exported C API
web/                 zenoh.js (API), zenoh-worker.js, zenoh-shared.js, index.html (demo)
tools/serve.py       static server with COOP/COEP headers
tools/headless.mjs   headless Chrome (DevTools Protocol) + process helpers for tests and benchmark
tests/               browser-test.html + run-browser-test.mjs
bench/               Zig-vs-Emscripten benchmark (bench.c, native peer, hyperfine driver, RESULTS.md)
..                   zenoh-pico itself, compiled unmodified
```
