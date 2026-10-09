// One benchmark run; this is the command hyperfine times.
//
//   node bench/run-one.mjs <state-file> <zig|emscripten> <mode> <n> <size> <results.jsonl>
//
// Opens the port's bench page in the running headless Chrome (bench/services.mjs), starts the
// native peer the mode needs, waits until the work is done (the page's BENCH line, plus the
// native peer's count for send modes), and appends the in-page metrics to results.jsonl. Exits non-zero if anything was
// lost, so hyperfine stops instead of timing a broken run.

import { appendFileSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

import { cleanupAll, connectChrome, failAndExit, openPage, startProcess } from '../tools/headless.mjs';

const [stateFile, port, mode, n, size, resultsFile] = process.argv.slice(2);
if (!resultsFile || !['zig', 'emscripten'].includes(port)) {
    console.error('usage: run-one.mjs <state-file> <zig|emscripten> <mode> <n> <size> <results.jsonl>');
    process.exit(2);
}
const state = JSON.parse(readFileSync(stateFile, 'utf8'));
const peerBin = join(state.nativeDir, 'peer');
const tcp = `tcp/127.0.0.1:${state.tcpPort}`;

const fail = (message) => failAndExit(`run-one ${port} ${mode}: ${message}`);
const timer = setTimeout(() => fail('timed out'), Number(process.env.BENCH_TIMEOUT_MS ?? 180_000));

let peer = null;
if (mode === 'pub' || mode === 'pubbatch') {
    peer = startProcess(peerBin, ['count', tcp, n], 'peer count');
    await peer.waitForLine('READY', 10000);
}

let bench = null;
let onLine;
const done = new Promise((resolve, reject) => {
    onLine = (source, type, text) => {
        if (process.env.BENCH_DEBUG) console.error(`[${source} ${type}] ${text}`);
        // BENCH is printed as soon as the measured work is done; session teardown is not timed.
        if (text.startsWith('BENCH ')) {
            bench = JSON.parse(text.slice(6));
            bench.error ? reject(new Error(bench.error)) : resolve();
        } else if (text === 'READY' && mode === 'sub') {
            peer = startProcess(peerBin, ['flood', tcp, n, size], 'peer flood');
        } else if (type === 'exception' || text.startsWith('bench failed') || text.startsWith('Aborted')) {
            reject(new Error(`${source}: ${text}`));
        } else if (type === 'error' || type === 'warning') {
            console.error(`[${source} ${type}] ${text}`);
        }
    };
});

const cdp = await connectChrome(state.chromeWsUrl);
const params = new URLSearchParams({ connect: `ws/127.0.0.1:${state.wsPort}`, mode, n, size });
const page = await openPage(cdp, `http://127.0.0.1:${state.httpPort}/bench-${port}.html?${params}`,
    (source, type, text) => onLine(source, type, text));

let peerResult = null;
try {
    await done;
    if (mode === 'pub' || mode === 'pubbatch') {
        peerResult = JSON.parse((await peer.waitForLine('PEER ', 60000)).slice(5));
        if (peerResult.received !== Number(n)) throw new Error(`peer received ${peerResult.received} of ${n}`);
    }
    if (mode === 'sub' && bench.received !== Number(n)) throw new Error(`page received ${bench.received} of ${n}`);
} catch (err) {
    fail(err.message);
}
clearTimeout(timer);
await page.close();
cdp.close();

appendFileSync(resultsFile, JSON.stringify({
    port, mode, n: Number(n), size: Number(size),
    iteration: process.env.HYPERFINE_ITERATION ?? null,
    bench, peer: peerResult,
}) + '\n');
cleanupAll();
process.exit(0);
