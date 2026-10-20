// Turns the benchmark results into a Markdown report.
//
//   node bench/report.mjs <results-dir> <bench-out-dir> > REPORT.md
//
// Reads hyperfine's JSON exports (<scenario>.json), the in-page metrics (inpage.jsonl),
// idle.json, env.json, and the built artifacts for the size table.

import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync } from 'node:zlib';

const [resultsDir, outDir] = process.argv.slice(2);
const env = JSON.parse(readFileSync(join(resultsDir, 'env.json'), 'utf8'));
const scenarios = env.scenarios;

const median = (xs) => {
    const s = [...xs].sort((a, b) => a - b);
    return s.length ? (s.length % 2 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2) : NaN;
};
const fmt = (x, digits = 0) => (Number.isFinite(x) ? x.toLocaleString('en-US', { maximumFractionDigits: digits, minimumFractionDigits: digits }) : '–');
const fmtTime = (s) => (s < 1 ? `${fmt(s * 1000, 1)} ms` : `${fmt(s, 3)} s`);

const lines = [];
const out = (s = '') => lines.push(s);

out('# zenoh-pico in the browser: Zig port vs. Emscripten port');
out();
out(`Measured ${env.date} on ${env.machine}. ${env.browser} (headless, background throttling off), ` +
    `zenohd ${env.zenohd}, zenoh-pico ${env.zenohPico}. Zig ${env.zig} (ReleaseFast), ${env.emcc} (-O3, Asyncify).`);
out();
out('Both pages run the same `bench/bench.c` against one local `zenohd` (WebSocket listener). The other end is');
out('native zenoh-pico over TCP: upstream `z_pong` for latency, `bench/peer.c` for throughput. Timing is wall clock');
out(`from [hyperfine](https://github.com/sharkdp/hyperfine) (${env.warmup} warmups, ${env.runs} runs per command). Each run is one`);
out('`node bench/run-one.mjs` that opens a fresh page in an already running Chrome and waits until the work is done,');
out('so every time also includes starting Node, loading the page, compiling the wasm and opening the session; the');
out('"Open a session" row is that fixed cost on its own.');
out();

// --- hyperfine
out('## End-to-end time (hyperfine)');
out();
out('| Scenario | Zig | Emscripten | Zig is |');
out('|---|---:|---:|---:|');
const hyper = {};
for (const sc of scenarios) {
    const file = join(resultsDir, `${sc.name}.json`);
    if (!existsSync(file)) continue;
    const data = JSON.parse(readFileSync(file, 'utf8'));
    const by = Object.fromEntries(data.results.map((r) => [r.name, r.summary.time_wall_clock]));
    hyper[sc.name] = by;
    const z = by.zig, e = by.emscripten;
    const ratio = e.mean / z.mean;
    const ratioSd = ratio * Math.sqrt((z.stddev / z.mean) ** 2 + (e.stddev / e.mean) ** 2);
    out(`| ${sc.label} | ${fmtTime(z.mean)} ± ${fmtTime(z.stddev)} | ${fmtTime(e.mean)} ± ${fmtTime(e.stddev)} | ` +
        `**${fmt(ratio, ratio < 10 ? 2 : 1)} ± ${fmt(ratioSd, ratio < 10 ? 2 : 1)}× faster** |`);
}
out();
out('Mean ± standard deviation. Raw hyperfine exports (all runs, min/max/median, CPU and RSS of the client) are');
out('next to this report as `<scenario>.json` / `.md`.');
out();

// --- in-page metrics
const inpage = readFileSync(join(resultsDir, 'inpage.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l))
    .filter((r) => !r.error && (r.iteration === null || !String(r.iteration).startsWith('warmup')));
const pick = (port, mode, f) => inpage.filter((r) => r.port === port && r.mode === mode).map(f).filter(Number.isFinite);

out('## Inside the page');
out();
out('Measured by `bench.c` itself with `clock_gettime(CLOCK_MONOTONIC)` (performance.now() in both builds);');
out('medians over the measured hyperfine runs.');
out();
out('| Metric | Zig | Emscripten | Zig is |');
out('|---|---:|---:|---:|');
const row = (label, mode, f, unit, higherIsBetter, digits = 0) => {
    const z = median(pick('zig', mode, f));
    const e = median(pick('emscripten', mode, f));
    if (!Number.isFinite(z) && !Number.isFinite(e)) return;
    const ratio = higherIsBetter ? z / e : e / z;
    out(`| ${label} | ${fmt(z, digits)} ${unit} | ${fmt(e, digits)} ${unit} | ${fmt(ratio, ratio < 10 ? 1 : 0)}× ${higherIsBetter ? 'more' : 'faster'} |`);
};
const ping = scenarios.find((s) => s.mode === 'ping');
const pub = scenarios.find((s) => s.mode === 'pub');
const pubbatch = scenarios.find((s) => s.mode === 'pubbatch');
const sub = scenarios.find((s) => s.mode === 'sub');
row('`z_open` (session handshake)', 'open', (r) => r.bench.open_ms, 'ms', false, 2);
if (ping) {
    row(`Round trip p50 (${ping.size} B, via router to native z_pong)`, 'ping', (r) => r.bench.p50_us / 1000, 'ms', false, 2);
    row('Round trip p99', 'ping', (r) => r.bench.p99_us / 1000, 'ms', false, 2);
}
if (pub) row(`Send, one put per message (${pub.size} B): delivered to native peer`, 'pub', (r) => r.peer?.recv_rate, 'msg/s', true);
if (pubbatch) row(`Send with zp_batch_start (${pubbatch.size} B): delivered`, 'pubbatch', (r) => r.peer?.recv_rate, 'msg/s', true);
if (sub) row(`Receive from native publisher (${sub.size} B)`, 'sub', (r) => r.bench.recv_rate, 'msg/s', true);
out();

// --- idle
const idleFile = join(resultsDir, 'idle.json');
if (existsSync(idleFile)) {
    const idle = JSON.parse(readFileSync(idleFile, 'utf8'));
    const m = (k) => median(idle[k].map((r) => r.cpuPercent));
    out('## Idle CPU');
    out();
    out(`CPU time of all of this Chrome's processes while one session sits idle (subscriber declared, no traffic),`);
    out(`${idle.seconds} s windows, median of ${idle.runs}. Not measurable with hyperfine, which only sees the client command.`);
    out();
    out('| | % of one core |');
    out('|---|---:|');
    out(`| Blank page (baseline) | ${fmt(m('baseline'), 1)} |`);
    out(`| Zig | ${fmt(m('zig'), 1)} |`);
    out(`| Emscripten | ${fmt(m('emscripten'), 1)} |`);
    out();
}

// --- sizes
const size = (path) => {
    if (!existsSync(path)) return null;
    const buf = readFileSync(path);
    return { raw: buf.length, gzip: gzipSync(buf, { level: 9 }).length };
};
const sum = (...xs) => xs.filter(Boolean).reduce((a, b) => ({ raw: a.raw + b.raw, gzip: a.gzip + b.gzip }), { raw: 0, gzip: 0 });
const kb = (n) => `${fmt(n / 1024, 1)} KiB`;
const site = join(outDir, 'site');
const em = join(outDir, 'emscripten');
const zigJs = sum(...['zenoh.js', 'zenoh-worker.js', 'zenoh-shared.js'].map((f) => size(join(site, f))));
const rowsSize = [
    ['Zig, ReleaseSmall (`zenoh-pico.wasm`, the shipped build)', size(join(site, 'zenoh-pico.wasm')), zigJs],
    ['Zig, ReleaseFast (benchmark build)', size(join(site, 'zenoh-pico-bench.wasm')), zigJs],
    ['Emscripten, -Oz link', size(join(em, 'size', 'bench-emscripten.wasm')), size(join(em, 'size', 'bench-emscripten.js'))],
    ['Emscripten, -O3 (benchmark build)', size(join(em, 'bench-emscripten.wasm')), size(join(em, 'bench-emscripten.js'))],
];
out('## Download size');
out();
out('| Build | wasm | JS | total gzipped |');
out('|---|---:|---:|---:|');
for (const [label, w, j] of rowsSize) {
    if (!w) continue;
    out(`| ${label} | ${kb(w.raw)} (${kb(w.gzip)} gz) | ${kb(j.raw)} (${kb(j.gzip)} gz) | **${kb(w.gzip + j.gzip)}** |`);
}
out();
out('The Zig benchmark wasm also contains `bench.c`; the shipped `zenoh-pico.wasm` contains the JS API exports instead.');
out('The Emscripten builds are the benchmark program (no JS API exists for that port). Upstream CMake compiles the');
out('library with `-O3` regardless, so `-Oz` only affects the final link.');
out();

// --- notes
out('## What was needed to run the Emscripten port');
out();
out('* `-DZ_FEATURE_MULTI_THREAD=0`: single-threaded like the Zig build (no SharedArrayBuffer/pthreads needed).');
out('  With it, upstream fails to compile (`size_t` unknown in `platform.h`); worked around with `-include stddef.h`.');
out('* `-sASYNCIFY`: the port blocks with `emscripten_sleep`.');
out('* `-sWEBSOCKET_SUBPROTOCOL=null`: by default Emscripten requests the `binary` subprotocol, zenohd answers none,');
out("  and Chrome fails the handshake (\"Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received\");");
out('  `z_open` then hangs without an error.');
out();
out('## Why the numbers look like this');
out();
out('* The Emscripten port waits by sleeping: every WebSocket read and write first calls `emscripten_sleep(1)`');
out('  (`src/link/transport/upper/ws_emscripten.c`), i.e. `await new Promise(r => setTimeout(r, 1))` resumed through');
out('  Asyncify. Browsers clamp nested timers to at least 4 ms, consistent with the ~5 ms per unbatched send and');
out('  ~10 ms round trips measured here. `z_open` also sleeps a fixed 100 ms after `connect()`.');
out('* The Zig port blocks a Web Worker with `Atomics.wait` and is woken by `Atomics.notify` as soon as the page');
out('  thread puts a received WebSocket message into the shared ring, so reads complete without timer delays.');
out('* Batching (`zp_batch_start`) helps Emscripten most because it amortizes the per-write sleep over many messages.');
out();
out('Caveats: one machine, loopback networking, headless Chrome only. Real networks add the same RTT to both ports, so');
out('the relative gap in latency shrinks on slow links; the per-write sleep cap on Emscripten throughput does not.');
console.log(lines.join('\n'));
