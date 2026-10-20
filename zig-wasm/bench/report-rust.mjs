// Turns the Zig-vs-Rust benchmark results (bench/run-rust.sh) into a Markdown report.
//
//   node bench/report-rust.mjs <results-dir> <bench-out-dir> > REPORT.md
//
// Reads hyperfine's JSON exports (<scenario>.json), the in-page metrics (inpage.jsonl),
// idle.json, env.json, and the built artifacts for the size table.

import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync } from 'node:zlib';

const [resultsDir, outDir] = process.argv.slice(2);
const env = JSON.parse(readFileSync(join(resultsDir, 'env.json'), 'utf8'));
const scenarios = env.scenarios;
const ports = ['zig', 'rust', 'rust-st'];
const names = { zig: 'Zig (zenoh-pico)', rust: 'Rust, worker pool', 'rust-st': 'Rust, single thread' };

const median = (xs) => {
    const s = [...xs].sort((a, b) => a - b);
    return s.length ? (s.length % 2 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2) : NaN;
};
const fmt = (x, digits = 0) => (Number.isFinite(x) ? x.toLocaleString('en-US', { maximumFractionDigits: digits, minimumFractionDigits: digits }) : '–');
const fmtTime = (s) => (s < 1 ? `${fmt(s * 1000, 1)} ms` : `${fmt(s, 3)} s`);
const fmtRatio = (r) => `${fmt(r, r < 10 ? 1 : 0)}×`;

const lines = [];
const out = (s = '') => lines.push(s);

out('# zenoh in the browser: Zig port of zenoh-pico vs. Rust zenoh');
out();
out(`Measured ${env.date} on ${env.machine}. ${env.browser} (headless, background throttling off), ` +
    `zenohd ${env.zenohd}. Zig side: zenoh-pico ${env.zenohPico}, Zig ${env.zig} (ReleaseFast). Rust side: ` +
    `${env.zenohWasm} (community wasm port of Rust zenoh, not upstream), ${env.rust}, release with LTO.`);
out();
out('All pages run the same workload (`bench/bench.c`; `bench/rust` is its Rust translation) against one local');
out('`zenohd` (WebSocket listener). The other end is native zenoh-pico over TCP: upstream `z_pong` for latency,');
out(`\`bench/peer.c\` for throughput. Timing is wall clock from [hyperfine](https://github.com/sharkdp/hyperfine)`);
out(`(${env.warmup} warmups, ${env.runs} runs per command). Each run is one \`node bench/run-one.mjs\` that opens a fresh`);
out('page in an already running Chrome and waits until the work is done, so every time also includes starting');
out('Node, loading the page, compiling the wasm (and, for the worker pool, starting its workers) and opening the');
out('session; the "Open a session" row is that fixed cost on its own.');
out();
out(`Columns: **${names.zig}** is this port. **${names.rust}** is the Rust port with its SharedArrayBuffer`);
out(`worker pool (the workload runs on its Application worker). **${names['rust-st']}** is the same code built`);
out('without the pool, running on the page\'s event loop. Ratios are relative to Zig (>1× means Zig is better).');
out();

// --- hyperfine
// Statistics over the runs that succeeded (hyperfine ran with --ignore-failure); failed runs are
// counted, and their errors listed below.
const stats = (xs) => {
    const mean = xs.reduce((a, b) => a + b, 0) / xs.length;
    const sd = xs.length > 1 ? Math.sqrt(xs.reduce((a, b) => a + (b - mean) ** 2, 0) / (xs.length - 1)) : 0;
    return { mean, sd };
};
out('## End-to-end time (hyperfine)');
out();
out(`| Scenario | ${ports.map((p) => names[p]).join(' | ')} |`);
out(`|---|${ports.map(() => '---:').join('|')}|`);
for (const sc of scenarios) {
    const file = join(resultsDir, `${sc.name}.json`);
    if (!existsSync(file)) continue;
    const data = JSON.parse(readFileSync(file, 'utf8'));
    const by = Object.fromEntries(data.results.map((r) => {
        const ok = r.measurements.filter((m) => m.exit_code === 0).map((m) => m.time_wall_clock.value);
        return [r.name, { ...stats(ok), failed: r.measurements.length - ok.length, total: r.measurements.length }];
    }));
    const cells = ports.map((p) => {
        const t = by[p];
        if (!t) return 'not run';
        const failed = t.failed ? `, **${t.failed} of ${t.total} runs failed**` : '';
        if (t.failed === t.total) return `all ${t.total} runs failed`;
        const ratio = p === 'zig' ? '' : ` (${fmtRatio(t.mean / by.zig.mean)})`;
        return `${fmtTime(t.mean)} ± ${fmtTime(t.sd)}${ratio}${failed}`;
    });
    out(`| ${sc.label} | ${cells.join(' | ')} |`);
}
out();
out('Mean ± standard deviation of the successful runs; in parentheses, how many times longer than Zig. Raw');
out('hyperfine exports (all runs, min/max/median, CPU and RSS of the client) are next to this report as');
out('`<scenario>.json` / `.md`.');
out();

const records = readFileSync(join(resultsDir, 'inpage.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const errors = new Map();
for (const r of records.filter((r) => r.error)) {
    const key = `${names[r.port] ?? r.port}, ${r.mode}: ${r.error}`;
    errors.set(key, (errors.get(key) ?? 0) + 1);
}
if (errors.size) {
    out('Failed runs (warmups included):');
    out();
    for (const [what, count] of errors) out(`* ${what} (${count}×)`);
    out();
}

// --- in-page metrics
const inpage = records.filter((r) => !r.error && (r.iteration === null || !String(r.iteration).startsWith('warmup')));
const pick = (port, mode, f) => inpage.filter((r) => r.port === port && r.mode === mode).map(f).filter(Number.isFinite);

out('## Inside the page');
out();
out('Measured by the workload itself with `performance.now()`; medians over the successful measured runs.');
out();
out(`| Metric | ${ports.map((p) => names[p]).join(' | ')} |`);
out(`|---|${ports.map(() => '---:').join('|')}|`);
const row = (label, mode, f, unit, higherIsBetter, digits = 0) => {
    const z = median(pick('zig', mode, f));
    const cells = ports.map((p) => {
        const v = median(pick(p, mode, f));
        if (!Number.isFinite(v)) return 'not run';
        const ratio = p === 'zig' || !Number.isFinite(z) ? '' : ` (${fmtRatio(higherIsBetter ? z / v : v / z)})`;
        return `${fmt(v, digits)} ${unit}${ratio}`;
    });
    out(`| ${label} | ${cells.join(' | ')} |`);
};
const ping = scenarios.find((s) => s.mode === 'ping');
const pub = scenarios.find((s) => s.mode === 'pub');
const pubbatch = scenarios.find((s) => s.mode === 'pubbatch');
const sub = scenarios.find((s) => s.mode === 'sub');
row('Session open (`z_open` / `zenoh::open`)', 'open', (r) => r.bench.open_ms, 'ms', false, 2);
row('Worker pool start-up (before open)', 'open', (r) => r.bench.init_ms, 'ms', false, 2);
if (ping) {
    row(`Round trip p50 (${ping.size} B, via router to native z_pong)`, 'ping', (r) => r.bench.p50_us / 1000, 'ms', false, 2);
    row('Round trip p99', 'ping', (r) => r.bench.p99_us / 1000, 'ms', false, 2);
}
if (pub) row(`Send, one message per frame (${pub.size} B): delivered to native peer`, 'pub', (r) => r.peer?.recv_rate, 'msg/s', true);
if (pubbatch) row(`Send, batched (${pubbatch.size} B): delivered`, 'pubbatch', (r) => r.peer?.recv_rate, 'msg/s', true);
if (sub) row(`Receive from native publisher (${sub.size} B)`, 'sub', (r) => r.bench.recv_rate, 'msg/s', true);
out();
out('Ratios: how many times slower (or lower) than Zig. "Worker pool start-up" only exists for the Rust pool;');
out('Zig starts one worker, which is part of the end-to-end time but not of any in-page metric.');
out();

// --- idle
const idleFile = join(resultsDir, 'idle.json');
if (existsSync(idleFile)) {
    const idle = JSON.parse(readFileSync(idleFile, 'utf8'));
    const m = (k) => median((idle[k] ?? []).map((r) => r.cpuPercent));
    out('## Idle CPU');
    out();
    out(`CPU time of all of this Chrome's processes while one session sits idle (subscriber declared, no traffic),`);
    out(`${idle.seconds} s windows, median of ${idle.runs}. Not measurable with hyperfine, which only sees the client command.`);
    out();
    out('| | % of one core |');
    out('|---|---:|');
    out(`| Blank page (baseline) | ${fmt(m('baseline'), 1)} |`);
    for (const p of ports) if (idle[p]) out(`| ${names[p]} | ${fmt(m(p), 1)} |`);
    out();
}

// --- sizes
const size = (path) => {
    if (!existsSync(path)) return null;
    const buf = readFileSync(path);
    return { raw: buf.length, gzip: gzipSync(buf, { level: 9 }).length };
};
const sum = (...xs) => xs.filter(Boolean).reduce((a, b) => ({ raw: a.raw + b.raw, gzip: a.gzip + b.gzip }), { raw: 0, gzip: 0 });
const kb = (n) => (n >= 1024 * 1024 ? `${fmt(n / 1024 / 1024, 2)} MiB` : `${fmt(n / 1024, 1)} KiB`);
const site = join(outDir, 'site');
const rust = join(outDir, 'rust');
const zigJs = sum(...['zenoh.js', 'zenoh-worker.js', 'zenoh-shared.js'].map((f) => size(join(site, f))));
const rowsSize = [
    ['Zig, ReleaseSmall (`zenoh-pico.wasm`, the shipped build)', size(join(site, 'zenoh-pico.wasm')), zigJs],
    ['Zig, ReleaseFast (benchmark build)', size(join(site, 'zenoh-pico-bench.wasm')), zigJs],
    ['Rust, worker pool (benchmark build)', size(join(rust, 'threads', 'zenoh_rust_bench_bg.wasm')), size(join(rust, 'threads', 'zenoh_rust_bench.js'))],
    ['Rust, single thread (benchmark build)', size(join(rust, 'single', 'zenoh_rust_bench_bg.wasm')), size(join(rust, 'single', 'zenoh_rust_bench.js'))],
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
out('The Rust builds are opt-level 3 with fat LTO and no wasm-opt pass, like the port\'s own published demos.');
out('Each also contains its benchmark workload; the Zig benchmark wasm contains `bench.c`.');
out();

// --- notes
out('## How the workload maps onto Rust zenoh');
out();
out('* "One put each" uses `express(true)`: each message is sent in its own frame, as zenoh-pico does without');
out('  `zp_batch_start`. "Batched" uses Rust zenoh\'s automatic batching (zenoh-pico: `zp_batch_start`).');
out('* Publishers use congestion control BLOCK in both, so the native peer must receive every message.');
out('* The single-threaded Rust build is not run in the send scenarios: on the page thread the port\'s publisher');
out('  cannot wait for a free transmit batch (`lock_for_publish` / `wait_refill` in its `pipeline.rs`), so a tight');
out('  put loop drops most messages even with BLOCK while `put` still returns `Ok` (2 of 500 and 1,872 of 10,000');
out('  delivered in a trial run).');
out('* Receiving, the port copies each WebSocket frame into a queue bounded at 128 frames and fails the link when');
out('  it is full ("WebSocket receive backlog exceeded"; browsers have no receive-side backpressure, see its');
out('  `WASM.md`). A fast native publisher can therefore cost a Rust run messages; such runs count as failed above.');
out();
out('## What the Rust port needs');
out();
out('* The `wasm` branch of a fork of Rust zenoh (`0x53A/zenoh`), not a released crate.');
out('* Worker pool: nightly Rust with `-Zbuild-std`, atomics/shared-memory link flags, wasm-bindgen 0.2.127, and a');
out('  cross-origin isolated page (COOP/COEP), like the Zig port.');
out();
out('Caveats: one machine, loopback networking, headless Chrome only.');
console.log(lines.join('\n'));
