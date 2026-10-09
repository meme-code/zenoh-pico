// Starts the long-lived processes the benchmark runs against and keeps them up until killed:
// zenohd (ws/ for the browser, tcp/ for native peers), tools/serve.py, headless Chrome and a
// native z_pong. Their coordinates are written to <state-file> for bench/run-one.mjs.
//
//   node bench/services.mjs <site-dir> <native-dir> <state-file>
// Environment: ZENOHD (default: zenohd on PATH), CHROME.

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { cleanupAll, launchChrome, randomPort, startProcess, waitForPort } from '../tools/headless.mjs';

const [siteDir, nativeDir, stateFile] = process.argv.slice(2);
if (!stateFile) {
    console.error('usage: services.mjs <site-dir> <native-dir> <state-file>');
    process.exit(2);
}
for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.on(sig, () => {
        cleanupAll();
        process.exit(0);
    });
}
process.on('uncaughtException', (err) => {
    console.error(err);
    cleanupAll();
    process.exit(1);
});
process.on('unhandledRejection', (err) => {
    console.error(err);
    cleanupAll();
    process.exit(1);
});

const root = dirname(fileURLToPath(import.meta.url));
const wsPort = randomPort(10000);
const tcpPort = randomPort(11000);
const httpPort = randomPort(18000);

startProcess(process.env.ZENOHD ?? 'zenohd', [
    '-l', `ws/127.0.0.1:${wsPort}`,
    '-l', `tcp/127.0.0.1:${tcpPort}`,
    '--no-multicast-scouting',
], 'zenohd');
startProcess('python3', [join(root, '..', 'tools', 'serve.py'), siteDir, String(httpPort)], 'serve.py');
await waitForPort(wsPort, 'zenohd');
await waitForPort(httpPort, 'serve.py');
const chrome = await launchChrome();

// z_pong waits for 'q' on stdin; keep stdin open so it blocks instead of spinning on EOF.
// (Its banner is not flushed when stdout is a pipe; the ping benchmark retries until it answers.)
startProcess(join(nativeDir, 'z_pong'), ['-e', `tcp/127.0.0.1:${tcpPort}`], 'z_pong', { keepStdinOpen: true });
await new Promise((r) => setTimeout(r, 500));

writeFileSync(stateFile, JSON.stringify({
    wsPort, tcpPort, httpPort,
    chromeWsUrl: chrome.wsUrl,
    chromeProfileDir: chrome.profileDir,
    browser: chrome.browser,
    nativeDir,
}, null, 2));
console.log(`services ready: zenohd ws/${wsPort} tcp/${tcpPort}, http ${httpPort}, ${chrome.browser}`);
setInterval(() => {}, 1 << 30);
