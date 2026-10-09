// Helpers for driving headless Chrome over the DevTools Protocol, plus the local processes
// (zenohd, serve.py, native peers) that the browser test and benchmark need.

import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { createConnection } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const children = new Set();
const cleanups = [];

/** Kills every process started with startProcess() and runs registered cleanups. */
export function cleanupAll() {
    for (const child of children) child.kill('SIGKILL');
    children.clear();
    for (const fn of cleanups.splice(0)) {
        try {
            fn();
        } catch {}
    }
}

export function failAndExit(message) {
    console.error(`FAIL: ${message}`);
    for (const child of children) {
        console.error(`--- ${child.label} output (tail) ---\n${child.output().slice(-2000)}`);
    }
    cleanupAll();
    process.exit(1);
}

/**
 * Starts a process and keeps the tail of its output. `waitForLine(prefix)` resolves with the
 * first stdout line starting with `prefix` (including lines already printed).
 */
export function startProcess(cmd, args, label, { keepStdinOpen = false } = {}) {
    const child = spawn(cmd, args, { stdio: [keepStdinOpen ? 'pipe' : 'ignore', 'pipe', 'pipe'] });
    let output = '';
    let partial = '';
    const lines = [];
    const waiters = [];
    child.stdout.on('data', (chunk) => {
        output = (output + chunk).slice(-20000);
        const parts = (partial + chunk).split('\n');
        partial = parts.pop();
        for (const line of parts) {
            lines.push(line);
            for (const w of [...waiters]) {
                if (line.startsWith(w.prefix)) {
                    waiters.splice(waiters.indexOf(w), 1);
                    w.resolve(line);
                }
            }
        }
    });
    child.stderr.on('data', (chunk) => {
        output = (output + chunk).slice(-20000);
    });
    child.on('error', (err) => failAndExit(`${label} failed to start: ${err.message}`));
    child.on('exit', () => children.delete(child));
    child.label = label;
    child.output = () => output;
    child.waitForLine = (prefix, timeoutMs = 60000) => {
        const seen = lines.find((l) => l.startsWith(prefix));
        if (seen) return Promise.resolve(seen);
        return new Promise((resolve, reject) => {
            const w = { prefix, resolve };
            waiters.push(w);
            setTimeout(() => {
                if (waiters.includes(w)) {
                    waiters.splice(waiters.indexOf(w), 1);
                    reject(new Error(`${label}: no "${prefix}" line within ${timeoutMs} ms`));
                }
            }, timeoutMs);
        });
    };
    child.stop = () =>
        new Promise((resolve) => {
            if (child.exitCode !== null || child.signalCode !== null) return resolve();
            child.once('exit', resolve);
            child.kill('SIGTERM');
        });
    children.add(child);
    return child;
}

export async function waitForPort(port, what) {
    for (let i = 0; i < 300; i++) {
        const ok = await new Promise((resolve) => {
            const s = createConnection({ host: '127.0.0.1', port }, () => {
                s.end();
                resolve(true);
            });
            s.on('error', () => resolve(false));
        });
        if (ok) return;
        await sleep(50);
    }
    failAndExit(`${what} did not come up on port ${port}`);
}

export function randomPort(base) {
    return base + Math.floor(Math.random() * 1000);
}

export function findChrome() {
    const chrome = process.env.CHROME ?? [
        '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
        '/Applications/Chromium.app/Contents/MacOS/Chromium',
        '/usr/bin/google-chrome',
        '/usr/bin/chromium',
        '/usr/bin/chromium-browser',
    ].find(existsSync);
    if (!chrome) failAndExit('Chrome not found; set CHROME=/path/to/chrome');
    return chrome;
}

/** Minimal DevTools Protocol client over Node's built-in WebSocket (browser endpoint URL). */
export async function connectChrome(url) {
    const ws = new WebSocket(url);
    await new Promise((resolve, reject) => {
        ws.onopen = resolve;
        ws.onerror = () => reject(new Error('CDP connection failed'));
    });
    let nextId = 1;
    const pending = new Map();
    const listeners = new Set();
    ws.onmessage = ({ data }) => {
        const msg = JSON.parse(data);
        if (msg.id && pending.has(msg.id)) {
            const { resolve, reject } = pending.get(msg.id);
            pending.delete(msg.id);
            msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
        } else if (msg.method) {
            for (const l of listeners) l(msg);
        }
    };
    return {
        send(method, params = {}, sessionId) {
            const id = nextId++;
            ws.send(JSON.stringify({ id, method, params, sessionId }));
            return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
        },
        on(listener) {
            listeners.add(listener);
            return () => listeners.delete(listener);
        },
        close() {
            ws.close();
        },
    };
}

/**
 * Launches headless Chrome. Background throttling is disabled so timer-driven code is not
 * penalized by the page being "hidden".
 */
export async function launchChrome() {
    const profileDir = mkdtempSync(join(tmpdir(), 'zenoh-pico-wasm-chrome-'));
    cleanups.push(() => rmSync(profileDir, { recursive: true, force: true }));
    const port = randomPort(19000);
    startProcess(findChrome(), [
        '--headless=new',
        `--remote-debugging-port=${port}`,
        `--user-data-dir=${profileDir}`,
        '--no-first-run',
        '--no-default-browser-check',
        '--disable-background-timer-throttling',
        '--disable-renderer-backgrounding',
        '--disable-backgrounding-occluded-windows',
        'about:blank',
    ], 'chrome');
    await waitForPort(port, 'chrome');
    let version;
    for (let i = 0; i < 100 && !version; i++) {
        try {
            version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
        } catch {
            await sleep(100);
        }
    }
    const cdp = await connectChrome(version.webSocketDebuggerUrl);
    cleanups.push(() => cdp.close());
    return { cdp, wsUrl: version.webSocketDebuggerUrl, browser: version.Browser, profileDir };
}

/**
 * Opens a page and reports console output from it and from its workers.
 * onConsole(source, type, text) where source is 'page' or the worker target type.
 */
export async function openPage(cdp, url, onConsole) {
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const sessions = new Map([[sessionId, 'page']]);
    const off = cdp.on((msg) => {
        if (msg.method === 'Target.attachedToTarget' && sessions.has(msg.sessionId)) {
            const child = msg.params.sessionId;
            sessions.set(child, msg.params.targetInfo.type);
            cdp.send('Runtime.enable', {}, child).catch(() => {});
            cdp.send('Runtime.runIfWaitingForDebugger', {}, child).catch(() => {});
            return;
        }
        const source = sessions.get(msg.sessionId);
        if (!source) return;
        if (msg.method === 'Runtime.consoleAPICalled') {
            const text = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
            onConsole(source, msg.params.type, text);
        } else if (msg.method === 'Runtime.exceptionThrown') {
            const d = msg.params.exceptionDetails;
            onConsole(source, 'exception', d.exception?.description ?? d.text);
        }
    });
    await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId);
    await cdp.send('Runtime.enable', {}, sessionId);
    await cdp.send('Page.navigate', { url }, sessionId);
    return {
        sessionId,
        async evaluate(expression) {
            const { result, exceptionDetails } = await cdp.send(
                'Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, sessionId);
            if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
            return result.value;
        },
        async close() {
            off();
            await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
        },
    };
}

/** Temporary directory removed by cleanupAll(). */
export function tempDir(prefix) {
    const dir = mkdtempSync(join(tmpdir(), prefix));
    cleanups.push(() => rmSync(dir, { recursive: true, force: true }));
    return dir;
}
