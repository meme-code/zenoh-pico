// Browser CPU use while a session sits idle. hyperfine can only time the client command, not
// Chrome, so this is measured separately: for each port the bench page runs mode "idle" (an open
// session with a subscriber and no traffic) and we sum the CPU time of all processes of this
// Chrome instance between IDLE_START and IDLE_END. A blank page gives the baseline.
//
//   node bench/idle-cpu.mjs <state-file> <out.json> [seconds=10] [runs=3]

import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';

import { cleanupAll, connectChrome, failAndExit, openPage, sleep } from '../tools/headless.mjs';

const [stateFile, outFile, secondsArg = '10', runsArg = '3'] = process.argv.slice(2);
if (!outFile) {
    console.error('usage: idle-cpu.mjs <state-file> <out.json> [seconds] [runs]');
    process.exit(2);
}
const state = JSON.parse(readFileSync(stateFile, 'utf8'));
const seconds = Number(secondsArg);
const runs = Number(runsArg);

// Total CPU seconds (user+system) of every process whose command line names this profile dir.
function chromeCpuSeconds() {
    const out = execFileSync('ps', ['-A', '-o', 'time=,command='], { encoding: 'utf8' });
    let total = 0;
    for (const line of out.split('\n')) {
        if (!line.includes(state.chromeProfileDir)) continue;
        const time = line.trim().split(/\s+/)[0];
        total += time.split(':').reduce((acc, part) => acc * 60 + Number(part), 0);
    }
    return total;
}

const cdp = await connectChrome(state.chromeWsUrl);

async function measurePort(port) {
    let startCpu, startWall;
    let resolveDone, rejectDone;
    const done = new Promise((res, rej) => {
        resolveDone = res;
        rejectDone = rej;
    });
    const params = new URLSearchParams({ connect: `ws/127.0.0.1:${state.wsPort}`, mode: 'idle', n: seconds * 1000, size: 0 });
    const page = await openPage(cdp, `http://127.0.0.1:${state.httpPort}/bench-${port}.html?${params}`, (source, type, text) => {
        if (text === 'IDLE_START') {
            startCpu = chromeCpuSeconds();
            startWall = performance.now();
        } else if (text === 'IDLE_END') {
            const cpu = chromeCpuSeconds() - startCpu;
            const wall = (performance.now() - startWall) / 1000;
            resolveDone({ cpuSeconds: cpu, wallSeconds: wall, cpuPercent: (100 * cpu) / wall });
        } else if (type === 'exception' || text.startsWith('bench failed')) {
            rejectDone(new Error(text));
        }
    });
    const timer = setTimeout(() => rejectDone(new Error('timed out')), (seconds + 60) * 1000);
    try {
        return await done;
    } finally {
        clearTimeout(timer);
        await sleep(500); // let the page finish closing its session
        await page.close();
    }
}

async function measureBaseline() {
    const page = await openPage(cdp, 'about:blank', () => {});
    await sleep(1000);
    const startCpu = chromeCpuSeconds();
    const startWall = performance.now();
    await sleep(seconds * 1000);
    const cpu = chromeCpuSeconds() - startCpu;
    const wall = (performance.now() - startWall) / 1000;
    await page.close();
    return { cpuSeconds: cpu, wallSeconds: wall, cpuPercent: (100 * cpu) / wall };
}

const results = { seconds, runs, baseline: [], zig: [], emscripten: [] };
try {
    for (let i = 0; i < runs; i++) {
        results.baseline.push(await measureBaseline());
        for (const port of ['zig', 'emscripten']) {
            const r = await measurePort(port);
            results[port].push(r);
            console.log(`idle ${port} run ${i + 1}/${runs}: ${r.cpuPercent.toFixed(1)}% of one core`);
        }
    }
} catch (err) {
    failAndExit(`idle-cpu: ${err.message}`);
}
writeFileSync(outFile, JSON.stringify(results, null, 2));
cdp.close();
cleanupAll();
process.exit(0);
