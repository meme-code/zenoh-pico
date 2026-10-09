// Runs tests/browser-test.html in headless Chrome against a local zenohd.
//
//   node tests/run-browser-test.mjs <site-dir>       (or: zig build test)
//
// <site-dir> holds zenoh-pico.wasm, the web/ files and browser-test.html.
// Environment:
//   ZENOHD  path to zenohd (default: zenohd on PATH). Its directory is also searched for the
//           storage_manager plugin, which the query test needs.
//   CHROME  path to Chrome/Chromium (default: the usual macOS/Linux locations)

import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
    cleanupAll, failAndExit, launchChrome, openPage, randomPort, sleep, startProcess, waitForPort,
} from '../tools/headless.mjs';

const siteDir = process.argv[2];
if (!siteDir) {
    console.error('usage: run-browser-test.mjs <site-dir>');
    process.exit(2);
}

const WS_PORT = randomPort(10000);
const HTTP_PORT = randomPort(18000);
const timer = setTimeout(() => failAndExit('timed out after 120s'), 120_000);

const zenohd = process.env.ZENOHD ?? 'zenohd';
const zenohdDir = zenohd.includes('/') ? dirname(zenohd) : null;
const startZenohd = () => startProcess(zenohd, [
    '-l', `ws/127.0.0.1:${WS_PORT}`,
    '--no-multicast-scouting',
    ...(zenohdDir ? ['--plugin-search-dir', zenohdDir] : []),
    '--cfg', 'plugins/storage_manager/storages/demo:{key_expr:"demo/**",volume:"memory"}',
], 'zenohd');

const root = dirname(fileURLToPath(import.meta.url));
let router = startZenohd();
startProcess('python3', [join(root, '..', 'tools', 'serve.py'), siteDir, String(HTTP_PORT)], 'serve.py');
await waitForPort(WS_PORT, 'zenohd');
await waitForPort(HTTP_PORT, 'serve.py');
const { cdp, browser } = await launchChrome();
console.log(`zenohd on ws/127.0.0.1:${WS_PORT}; ${browser}`);

let resolveResult;
const result = new Promise((resolve) => (resolveResult = resolve));
const url = `http://127.0.0.1:${HTTP_PORT}/browser-test.html?connect=${encodeURIComponent(`ws/127.0.0.1:${WS_PORT}`)}`;
const page = await openPage(cdp, url, (source, type, text) => {
    if (source === 'page' && text.startsWith('RESULT ')) resolveResult(JSON.parse(text.slice(7)));
    else console.log(`[${source} ${type}] ${text}`);
});

const { ok, checks, metrics } = await result;
for (const c of checks) console.log(`${c.ok ? 'PASS' : 'FAIL'}  ${c.name}${c.detail ? `  (${c.detail})` : ''}`);
console.log(`metrics: ${JSON.stringify(metrics)}`);
if (!ok) failAndExit('some checks failed');

// Router restart: a session must survive the router going away and coming back
// (Z_FEATURE_AUTO_RECONNECT), and its subscriber must be redeclared.
const connect = JSON.stringify(`ws/127.0.0.1:${WS_PORT}`);
await page.evaluate(`(async () => {
    const { Session } = await import('./zenoh.js');
    window.rt = { got: [], closed: false };
    rt.a = await Session.open({ connect: ${connect} });
    rt.a.onclose = () => { rt.closed = true; };
    await rt.a.declareSubscriber('restart/**', (s) => rt.got.push(s.text()));
})()`);
router.kill('SIGKILL');
await new Promise((resolve) => router.once('exit', resolve));
await sleep(2000);
router = startZenohd();
await waitForPort(WS_PORT, 'restarted zenohd');
const t0 = Date.now();
const restart = await page.evaluate(`(async () => {
    const { Session } = await import('./zenoh.js');
    const b = await Session.open({ connect: ${connect} });
    const end = performance.now() + 30000;
    while (performance.now() < end && !rt.got.includes('after restart')) {
        await b.put('restart/x', 'after restart');
        await new Promise((r) => setTimeout(r, 250));
    }
    await b.close();
    return { received: rt.got.includes('after restart'), closed: rt.closed };
})()`);
const recoverMs = Date.now() - t0;
const restartOk = restart.received && !restart.closed;
console.log(`${restartOk ? 'PASS' : 'FAIL'}  session recovers after router restart  (received=${restart.received}, closed=${restart.closed}, ${recoverMs} ms after restart)`);
clearTimeout(timer);
if (!restartOk) failAndExit('router restart check failed');
cleanupAll();
console.log('All browser checks passed.');
process.exit(0);
