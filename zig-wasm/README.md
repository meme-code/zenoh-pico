# zenoh-pico for the browser, built with Zig

[zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) 1.10.1 compiled to WebAssembly with
Zig 0.17 alone (no Emscripten). It gives you two ways in:

* a **JavaScript API**: sessions, put/delete, subscribers, publishers (with matching listeners),
  queryables, get, queriers, liveliness, attachments, QoS, session info and key expression
  helpers; single-threaded or on real threads;
* **C programs**: any zenoh-pico program with `main()` (the upstream examples, unmodified, or your
  own) runs in the page with POSIX threads, stdout/stderr and stdin.

Both speak the zenoh protocol directly to a router's WebSocket listener: no plugin, no proxy.

```js
import { Session } from './zenoh.js';

const session = await Session.open({ connect: 'ws/127.0.0.1:10000' });
await session.declareSubscriber('demo/**', (s) => console.log(s.kind, s.keyexpr, s.text()));
await session.put('demo/hello', 'world', { encoding: 'text/plain', priority: 'real-time' });
await session.declareQueryable('demo/answer', (query) => query.reply('demo/answer', '42'));
const replies = await session.get('demo/**', { timeoutMs: 3000 });
await session.liveliness.declareToken('group1/browser');
await session.close();
```

```js
import { runProgram } from './zenoh.js';

const program = await runProgram({
    wasmUrl: 'examples/z_sub.wasm',
    args: ['-e', 'ws/127.0.0.1:10000'],
    onStdout: (line) => console.log(line),
});
console.log('exit code', await program.exited);
```

## Build and run

Everything lives in this directory (`zig-wasm/`) of the zenoh-pico repository and builds zenoh-pico's own
sources from `..` without modifying them. Needs Zig 0.17.

```sh
cd zig-wasm
zig build -Drelease            # -> zig-out/web/
zenohd -l ws/127.0.0.1:10000   # a zenoh router with a WebSocket listener (zenoh 1.10.x)
zig build serve -Drelease      # http://127.0.0.1:8000/ (API demo), /examples.html (C examples)
```

`zig-out/web/` then holds:

| File | |
|---|---|
| `zenoh-pico.wasm` | JS API, single-threaded: zenoh-pico runs between calls on one worker. 124 KB, 45 KB gzipped |
| `zenoh-pico-mt.wasm` | JS API with threads (`Session.open({ threads: true })`); peer mode too. 136 KB, 49 KB gzipped |
| `examples/*.wasm` | the 29 upstream C examples that make sense in a browser (all but TLS and scouting); 34–192 KB, most about 160 KB (58 KB gzipped) |
| `zenoh.js`, `zenoh-runtime.js`, `zenoh-worker.js`, `zenoh-shared.js` | the runtime: 61 KB, 17 KB gzipped |
| `index.html`, `examples.html` | demo pages |

Your own program: `zig build app -Dapp=main.c [-Dapp-name=name] -Drelease` writes
`zig-out/web/apps/<name>.wasm`, built with the same configuration as the examples.

`zig build` without `-Drelease` gives Debug builds with UBSan.

| Option | Default | |
|---|---|---|
| `-Drelease` | off | ReleaseSmall |
| `-Dlog-level=N` | 0 | zenoh-pico logging to the console: 1 error, 2 info, 3 debug |
| `-Dsocket-timeout=MS` | 100 | read timeout per spin; also the handshake read timeout |
| `-Dbatch-size=B` | 2048 | unicast batch size |
| `-Dfrag-max-size=B` | 4096 | largest reassembled message |
| `-Dport=P` | 8000 | port for `zig build serve` |

The per-read timeout can also be set on the locator: `ws/example.com:10000#tout=1000`.

### Configurations

| | `zenoh-pico.wasm` | `zenoh-pico-mt.wasm` | C programs |
|---|---|---|---|
| `Z_FEATURE_MULTI_THREAD` | 0 | 1 | 1 (`*_st` examples: 0) |
| client / peer mode | client | both | both |
| `Z_FEATURE_UNSTABLE_API` (queriers' unstable options, advanced pub/sub, admin space, connectivity API) | no | no | yes |
| local delivery (a session's own subscribers and queryables see its puts and queries, like Rust zenoh; off by default upstream) | yes | yes | yes |
| everything else (publication, subscription, query, queryable, liveliness, interest, matching, fragmentation, batching, auto-reconnect) | yes | yes | yes |
| links | ws | ws | ws |

### Hosting requirement

The page must be **cross-origin isolated**, because wasm runs on a shared `WebAssembly.Memory`
and blocks with `memory.atomic.wait32`. Serve it with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`tools/serve.py` (used by `zig build serve`) does this. `Session.open` and `runProgram` throw a clear
error when the headers are missing. Hosts that cannot set headers (e.g. GitHub Pages) need a
service-worker shim such as coi-serviceworker.

On an `https:` page the default WebSocket URL is `wss://host:port/`, otherwise `ws://`. Pass
`wsUrl: (host, port) => '...'` to `Session.open` or `runProgram` to map the locator to any URL
(path, proxy, TLS).

## JavaScript API

All methods are async. Payloads and attachments are strings, `ArrayBuffer`s or typed arrays;
received ones are `Uint8Array`s (`text()` decodes UTF-8).

| | |
|---|---|
| `Session.open({ connect, mode, threads, wsUrl, wasmUrl, workerUrl })` | `connect`: one locator or an array; `mode`: `'client'` or `'peer'` (needs `threads`) |
| `session.put(key, payload, opts)` / `delete(key, opts)` | `opts`: `encoding`, `attachment`, `congestionControl` (`'drop'`/`'block'`), `priority` (`'real-time'` … `'background'` or 1–7), `express` |
| `session.declareSubscriber(key, (sample) => …)` | `sample`: `keyexpr`, `kind`, `payload`, `encoding`, `attachment`, `priority`, `congestionControl`, `express`, `timestamp` |
| `session.declarePublisher(key, opts)` | → `put(payload, { encoding, attachment })`, `delete()`, `matchingStatus()`, `declareMatchingListener((matching) => …)`, `undeclare()` |
| `session.declareQueryable(key, (query) => …, { complete })` | `query`: `keyexpr`, `parameters`, `payload`, `encoding`, `attachment`, `reply(key, payload, opts)`, `replyErr(payload)`, `replyDelete(key)`; finished when the handler (or its promise) is done |
| `session.get(key, opts)` | → `Reply[]` (`ok`, `keyexpr`, `payload`, `encoding`, `attachment`, `sample`); `opts`: `parameters`, `payload`, `encoding`, `attachment`, `target`, `consolidation`, `timeoutMs`, `onReply`, QoS |
| `session.declareQuerier(key, opts)` | → `get(parameters, { payload, encoding, attachment, onReply })`, `undeclare()` |
| `session.liveliness.declareToken(key)` / `declareSubscriber(key, cb, { history })` / `get(key)` | |
| `session.info()` | `{ zid, routers, peers }` |
| `session.keyexpr.intersects(a, b)` / `includes(a, b)` / `canonize(k)` | |
| `session.close()`, `session.onclose`, `session.zid`, `session.threads` | `onclose` fires if the session dies on its own (lease expired) |
| `runProgram({ wasmUrl, args, onStdout, onStderr, wsUrl })` | → `{ exited, stdin: { write, end }, kill() }` |

## Test

```sh
ZENOHD=/path/to/zenohd zig build test -Drelease   # or without -Drelease for the UBSan builds
```

This starts `zenohd` (with an in-memory storage on `demo/**`, so `zenohd`'s directory must contain
the `storage_manager` plugin), `tools/serve.py` and headless Chrome, then runs 101 checks:

* `tests/browser-test.html` and `tests/api-test.html`, once with each JS build: ordered delivery,
  encodings, binary and fragmented payloads, ping/pong, a 2000-message burst, storage queries,
  queryables answering asynchronously with several replies and errors, queriers, attachments, QoS,
  publisher delete, matching status and listeners, liveliness tokens/subscribers/history/get,
  local delivery to a session's own subscribers and queryables, puts right before `close()`,
  session info, key expressions;
* a router restart for each build: `zenohd` is killed and restarted and the session must recover
  (subscriber redeclared);
* `tests/programs-test.html`: `tests/threads-test.c` (mutex contention across 4 threads, recursive
  mutexes, condition variables and timed waits, thread-local storage, `malloc`/`free` from 4
  threads at once, detached threads, `pthread_exit`, and a client and a peer session on threads),
  `tests/burst-close-st.c` (a single-threaded program that closes right after a 10,000-message
  batched burst: nothing may be lost), then pairs of unmodified upstream examples talking to each other: `z_sub`/`z_pub`,
  `z_queryable`/`z_get`, `z_liveliness`/`z_get_liveliness`, `z_pong`/`z_ping` (stopped with `q`
  on stdin), `z_sub_st`/`z_put`, `z_queryable`/`z_querier`, `z_advanced_pub`/`z_advanced_sub`
  (history), `z_info`, `z_bytes`.

Needs node, python3 and Chrome (`CHROME=` to override).

## How it works

```
 page (main thread)                                   Web Workers
 ┌────────────────────────────────┐                  ┌─────────────────────────────────────┐
 │ zenoh.js       Session API /   │  calls, results, │ zenoh-worker.js: one instance of the │
 │                runProgram      │◀── callbacks ───▶│ wasm per thread, all on one shared   │
 │ zenoh-runtime.js               │                  │ WebAssembly.Memory                   │
 │  ├─ WebSocket(s) ─ onmessage ─▶ rx ring ┐        │                                      │
 │  │    send ◀──────────────────── tx ring ┤ in the │  zenoh-pico: blocks with             │
 │  │                         shared memory ┘        │  memory.atomic.wait32, woken by      │
 │  └─ thread workers (wasi-threads "thread-spawn")  │  Atomics.notify from the page        │
 └────────────────────────────────┘                  └─────────────────────────────────────┘
```

* `build.zig` compiles the unmodified upstream sources (same globs as zenoh-pico's CMake),
  generates `config.h` from `config.h.in` per configuration, and plugs in the browser platform via
  zenoh-pico's `ZP_SYSTEM_PLATFORM_HEADER` hook. Target: `wasm32-wasi` with atomics and bulk
  memory, imported shared memory; Zig provides libc.
* `wasm/` is the browser platform:
  * `system.c`: clocks, random (`crypto.getRandomValues`), sleep, and zenoh-pico's tasks, mutexes
    and condition variables on top of pthreads.
  * `ws.c`: zenoh's ws link. Each WebSocket's receive and send rings live in wasm memory, so any
    thread can use any socket: the page appends received messages and wakes waiting threads; wasm
    appends outgoing batches and rings a doorbell the page listens to with `Atomics.waitAsync`.
    Waiting on several sockets is supported (peer mode).
  * `pthread.c` + `thread_start.s`: POSIX threads on wasi-threads. Zig's wasi-libc only ships
    single-threaded pthread stubs; these replace them. A thread is a Web Worker running another
    instance of the module; `wasi_thread_start` installs its stack and TLS block before any C runs.
  * `heap.c` + `heap.zig`: a locked `malloc` over Zig's `std.heap.BrkAllocator` (Zig's libc
    allocator for wasm is single-threaded only). `stdio.c`: thread-safe `printf` and friends.
  * `api.c`: the `zw_*` exports used by `zenoh.js`. `zenoh_wasm_host.h` lists the imports.
* Single-threaded sessions call `zp_spin_once` once per macrotask, so API calls interleave with
  network I/O, and the page interrupts an idle read whenever it posts a call. Threaded sessions
  and programs run zenoh-pico's own tasks on thread workers, exactly like natively.
* C programs are WASI commands: the worker provides argv, stdout/stderr (to the page), stdin (from
  the page), clocks, `sleep()` and `exit()`.

## Compared with the in-tree Emscripten port

zenoh-pico's own browser support (`src/system/emscripten`) needs emsdk. It blocks through Asyncify,
polls `recv` every 1 ms, and sleeps a fixed 100 ms after `connect` instead of waiting for the
WebSocket to open. It ships no JavaScript API: you write a C program and bridge it yourself.

| | Emscripten port (upstream) | this port |
|---|---|---|
| C programs with `main()` | yes | yes (`zig build app`, examples page) |
| threads (`Z_FEATURE_MULTI_THREAD=1`) | yes (Emscripten pthreads) | yes (wasi-threads) |
| single-threaded build | yes, with a `stddef.h` workaround | yes |
| peer mode | compiled, but waiting on several sockets is unimplemented | yes |
| JavaScript API | none | full (see above) |
| unstable API, advanced pub/sub, admin space | off by default | on for C programs |
| toolchain | emsdk + CMake | Zig only |
| needs cross-origin isolation | only with threads | always |
| maintained upstream | yes (compiled in CI, not run) | no |

Running the Emscripten port against a stock `zenohd` also needed `-sWEBSOCKET_SUBPROTOCOL=null`.
Without it, Chrome rejects the WebSocket handshake and `z_open` hangs with no error.

## Limitations

* No listening and no UDP in a browser: no scouting, no TCP/UDP/serial/TLS links. Peer mode can
  connect to peers and routers but cannot accept connections.
* `pthread_cancel` and thread-specific data destructors are not implemented; `fwrite` to
  stdout/stderr and wide-character output are not covered by the thread-safe stdio.
* Like natively, a publisher or querier sends nothing until the router has reported a match
  (zenoh-pico's write filter starts active), so don't publish in the same tick as declaring.
* Tested in headless Chrome only. It uses standard APIs (module workers, SharedArrayBuffer,
  wasm threads, `Atomics.waitAsync` with a fallback) that Firefox and Safari also support, but it
  hasn't been run there.

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
3. **`platform.h` uses `size_t` without `<stddef.h>`**
   ([#1339](https://github.com/eclipse-zenoh/zenoh-pico/issues/1339)), so the single-threaded Emscripten
   build fails (`bench/build-emscripten.sh` works around it).
4. **Emscripten: `z_open()` hangs against `zenohd`**
   ([#1340](https://github.com/eclipse-zenoh/zenoh-pico/issues/1340)): Emscripten's WebSockets request the
   `binary` subprotocol, `zenohd` doesn't answer with one, and the browser fails the handshake. Needs
   `-sWEBSOCKET_SUBPROTOCOL=null`.
5. **`z_info.c` doesn't compile with `Z_FEATURE_CONNECTIVITY=1` and `Z_FEATURE_SCOUTING=0`**
   ([#1341](https://github.com/eclipse-zenoh/zenoh-pico/issues/1341)): `z_whatami_to_view_string` is
   scouting-only. Worked around for the browser examples in `wasm/zenoh_wasm_examples.h`.

## Benchmarks

`bench/` times the same workload (`bench/bench.c`: open, 200 round trips to a native `z_pong`,
unbatched and batched sends to a native subscriber, receiving a native flood) in headless Chrome
against one `zenohd`, with hyperfine. Two comparisons:

* `bench/run.sh`: this port vs. zenoh-pico's Emscripten port; results in `bench/RESULTS.md`
  (measured on the first, single-threaded version of this port: 5–58× faster end to end).
* `bench/run-rust.sh`: this port vs. Rust zenoh compiled to wasm by the community port
  ([0x53A/zenoh](https://github.com/0x53A/zenoh), branch `wasm`; not upstream), built in a container
  with nightly Rust; results in `bench/RESULTS-rust.md`.

Zig vs. Rust, from `bench/RESULTS-rust.md` (Apple M5, Chrome 155, loopback, 10 runs after 2 warmups,
measured inside the page; the Zig side is the single-threaded build):

| | Zig (this port) | Rust, worker pool | Rust, single thread |
|---|---:|---:|---:|
| `z_open` / `zenoh::open` | 1.76 ms | 13.1 ms, plus 10.8 ms to start the pool | 10.8 ms |
| Round trip p50 / p99 | 0.17 / 0.39 ms | 1.47 / 1.79 ms | 4.86 / 6.21 ms |
| Send, one put each (delivered) | 111,050 msg/s | 782 msg/s | not run¹ |
| Send, batched (delivered) | 1,156,156 msg/s | 677,898 msg/s | not run¹ |
| Receive | 299,837 msg/s | 327,330 msg/s | 334,608 msg/s |
| Idle CPU (blank page: 0.5%) | 1.1% | 9.3% | 1.4% |
| Download, gzipped | 58 KiB | 1.33 MiB | 1.27 MiB |

End to end (hyperfine, including page load and wasm compile) Zig is 1.1–8.4× faster in every scenario.
Rust receives about 10% faster. ¹ On the page thread the Rust port's publisher cannot wait for a free
batch, so a put loop silently drops messages while `put` returns `Ok`. In earlier trial runs the Rust
port also lost messages when receiving fast bursts (its receive queue is capped at 128 WebSocket frames
and the link is failed when it fills).

The Rust port runs the full Rust zenoh API in the browser, which zenoh-pico does not have (zenoh-ext,
everything unstable, Rust zenoh's routing, hiroz/ROS 2), but it is a single-maintainer fork that needs
nightly Rust with `-Zbuild-std` and an exact wasm-bindgen version. Upstream Rust zenoh has no browser
support yet (a stack of draft pull requests, #2790–#2799).

## Layout

```
build.zig            build, serve, test, app and bench steps
wasm/                browser platform layer (system, ws link, pthreads, heap, stdio) + exported C API
web/                 zenoh.js (API), zenoh-runtime.js (page side), zenoh-worker.js, zenoh-shared.js,
                     index.html (API demo), examples.html (C programs)
tools/serve.py       static server with COOP/COEP headers
tools/headless.mjs   headless Chrome (DevTools Protocol) + process helpers for tests and benchmarks
tests/               test pages, threads-test.c, burst-close-st.c, run-browser-test.mjs
bench/               benchmarks: bench.c, native peer, Emscripten and Rust builds, hyperfine drivers
..                   zenoh-pico itself, compiled unmodified
```
