// Runs zenoh-pico wasm inside a dedicated Web Worker. One script, three roles:
//
//   session  the instance behind a zenoh.js Session: a reactor whose zw_* exports are called
//            on request. Single-threaded builds are driven by calling zw_spin in between.
//   program  a C program with main() (zig build app / the upstream examples): runs _start.
//   thread   one extra thread of either (wasi-threads): runs wasi_thread_start(tid, arg) each
//            time the page hands it a thread to run.
//
// All instances of one runtime share the WebAssembly.Memory created by the page. zenoh-pico
// blocks with memory.atomic.wait32 inside wasm; the WebSockets live on the page
// (zenoh-runtime.js), which fills the rings in that memory and notifies.
//
// Protocol with zenoh-runtime.js:
//   page -> worker  {type: 'init', role, module, memory, shared, clockBase, args}
//                   {type: 'call', id, method, args}            (session)
//                   {type: 'start', tid, arg}                   (thread)
//   worker -> page  {type: 'ready'} | {type: 'init-error', message}
//                   {type: 'result', id, value} | {type: 'error', id, message}
//                   {type: 'spawn', tid, arg} | {type: 'thread-done'} | {type: 'fatal', message}
//                   {type: 'ws-open', ws, host, port} | {type: 'ws-kick', ws} | {type: 'ws-close', ws}
//                   {type: 'sample' | 'reply' | 'reply-done' | 'query' | 'matching', ...}
//                   {type: 'log', fd, line} | {type: 'exit', code} | {type: 'closed'}

import { MSG, SHARED, THREAD } from './zenoh-shared.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

let role = null;
let wasm = null;          // instance exports
let memory = null;        // the shared WebAssembly.Memory
let shared = null;        // Int32Array over the runtime's control SharedArrayBuffer
let stdin = null;         // { hdr: Int32Array, data: Uint8Array } (programs)
let clockBase = 0;
let args = [];

// Thrown through wasm to unwind a thread (pthread_exit) or the program (exit).
class ThreadExit extends Error {}
class ProcExit extends Error {
    constructor(code) {
        super(`exit(${code})`);
        this.code = code;
    }
}

const u8 = () => new Uint8Array(memory.buffer);
const view = () => new DataView(memory.buffer);
// TextDecoder refuses views of shared memory, so strings are copied out first.
const readStr = (ptr, len) => decoder.decode(u8().slice(ptr, ptr + len));
const copyBytes = (ptr, len) => u8().slice(ptr, ptr + len);
const sleepCell = new Int32Array(new SharedArrayBuffer(4));

/*------------------ zenoh_host imports ------------------*/

// Copies a zw_msg_t (wasm/api.c) out of wasm memory. Byte arrays are listed in `transfer`.
function readMsg(ptr, transfer) {
    const v = view();
    const u32 = (off) => v.getUint32(ptr + off, true);
    const i32 = (off) => v.getInt32(ptr + off, true);
    const bytes = (off, lenOff) => {
        const b = copyBytes(u32(off), u32(lenOff));
        transfer.push(b.buffer);
        return b;
    };
    return {
        kind: i32(MSG.KIND),
        keyexpr: readStr(u32(MSG.KEYEXPR), u32(MSG.KEYEXPR_LEN)),
        payload: bytes(MSG.PAYLOAD, MSG.PAYLOAD_LEN),
        hasPayload: i32(MSG.HAS_PAYLOAD) !== 0,
        encoding: readStr(u32(MSG.ENCODING), u32(MSG.ENCODING_LEN)),
        attachment: u32(MSG.ATTACHMENT) !== 0 ? bytes(MSG.ATTACHMENT, MSG.ATTACHMENT_LEN) : null,
        parameters: readStr(u32(MSG.PARAMETERS), u32(MSG.PARAMETERS_LEN)),
        priority: i32(MSG.PRIORITY),
        congestionControl: i32(MSG.CONGESTION_CONTROL),
        express: i32(MSG.EXPRESS),
        timestamp: i32(MSG.HAS_TIMESTAMP) !== 0
            ? { ntp64: v.getBigUint64(ptr + MSG.TIMESTAMP, true), id: readStr(ptr + MSG.TIMESTAMP_ID, 32) }
            : null,
    };
}

const host = {
    // performance.now() counts from each worker's own start; every thread needs the same clock.
    now_ms: () => performance.timeOrigin + performance.now() - clockBase,
    date_now_ms: () => Date.now(),
    random_fill(ptr, len) {
        // getRandomValues refuses views over shared memory and caps each call at 64 KiB.
        for (let off = 0; off < len; off += 65536) {
            const chunk = crypto.getRandomValues(new Uint8Array(Math.min(65536, len - off)));
            u8().set(chunk, ptr + off);
        }
    },
    thread_exit() {
        throw new ThreadExit();
    },

    ws_open(ws, hostPtr, hostLen, portPtr, portLen) {
        postMessage({ type: 'ws-open', ws, host: readStr(hostPtr, hostLen), port: readStr(portPtr, portLen) });
    },
    ws_kick(ws) {
        postMessage({ type: 'ws-kick', ws });
    },
    ws_close(ws) {
        postMessage({ type: 'ws-close', ws });
    },

    on_sample(id, msgPtr) {
        const transfer = [];
        postMessage({ type: 'sample', id, msg: readMsg(msgPtr, transfer) }, transfer);
    },
    on_reply(id, msgPtr) {
        const transfer = [];
        postMessage({ type: 'reply', id, msg: readMsg(msgPtr, transfer) }, transfer);
    },
    on_reply_done(id) {
        postMessage({ type: 'reply-done', id });
    },
    on_query(id, query, msgPtr) {
        const transfer = [];
        postMessage({ type: 'query', id, query, msg: readMsg(msgPtr, transfer) }, transfer);
    },
    on_matching(id, matching) {
        postMessage({ type: 'matching', id, matching: matching !== 0 });
    },
};

const wasiThreads = {
    'thread-spawn'(arg) {
        // Thread ids are allocated here, synchronously; the page starts the worker.
        const tid = Atomics.add(shared, SHARED.NEXT_TID, 1) + 1;
        postMessage({ type: 'spawn', tid, arg });
        return tid;
    },
};

/*------------------ WASI ------------------*/
// Enough of wasi_snapshot_preview1 for wasi-libc: args, stdio (stdout/stderr go to the page,
// stdin comes from it), clocks, random, sleeping through poll_oneoff, and exit.
function makeWasi() {
    const EBADF = 8;
    const EINVAL = 28;
    const ENOSYS = 52;
    const ESPIPE = 70;
    const lines = { 1: '', 2: '' };
    const encodedArgs = () => args.map((a) => encoder.encode(`${a}\0`));
    const impl = {
        args_sizes_get(countPtr, sizePtr) {
            const enc = encodedArgs();
            view().setUint32(countPtr, enc.length, true);
            view().setUint32(sizePtr, enc.reduce((n, a) => n + a.length, 0), true);
            return 0;
        },
        args_get(argvPtr, bufPtr) {
            let p = bufPtr;
            encodedArgs().forEach((a, i) => {
                view().setUint32(argvPtr + 4 * i, p, true);
                u8().set(a, p);
                p += a.length;
            });
            return 0;
        },
        environ_sizes_get(countPtr, sizePtr) {
            view().setUint32(countPtr, 0, true);
            view().setUint32(sizePtr, 0, true);
            return 0;
        },
        environ_get: () => 0,
        // No preopened directories: libc's startup scan stops at the first EBADF.
        fd_prestat_get: () => EBADF,
        // stdio looks like a tty so libc keeps stdout line-buffered: wasi-libc's isatty() wants a
        // character device *without* the fd_seek/fd_tell rights.
        fd_fdstat_get(fd, ptr) {
            if (fd > 2) return EBADF;
            const RIGHTS_FD_SEEK = 1n << 2n;
            const RIGHTS_FD_TELL = 1n << 5n;
            const rights = 0xffffffffffffffffn & ~(RIGHTS_FD_SEEK | RIGHTS_FD_TELL);
            const v = view();
            v.setUint8(ptr, 2); // filetype: character_device
            v.setUint16(ptr + 2, 0, true);
            v.setBigUint64(ptr + 8, rights, true);
            v.setBigUint64(ptr + 16, rights, true);
            return 0;
        },
        fd_seek: (fd) => (fd <= 2 ? ESPIPE : EBADF),
        fd_close: () => 0,
        fd_write(fd, iovs, iovsLen, nwrittenPtr) {
            if (fd !== 1 && fd !== 2) return EBADF;
            const v = view();
            let written = 0;
            for (let i = 0; i < iovsLen; i++) {
                const ptr = v.getUint32(iovs + i * 8, true);
                const len = v.getUint32(iovs + i * 8 + 4, true);
                lines[fd] += readStr(ptr, len);
                written += len;
            }
            const parts = lines[fd].split(/\r?\n/);
            lines[fd] = parts.pop();
            for (const line of parts) postMessage({ type: 'log', fd, line });
            view().setUint32(nwrittenPtr, written, true);
            return 0;
        },
        fd_read(fd, iovs, iovsLen, nreadPtr) {
            if (fd !== 0) return EBADF;
            view().setUint32(nreadPtr, readStdin(iovs, iovsLen), true);
            return 0;
        },
        clock_time_get(id, _precision, outPtr) {
            // 0: realtime; 1: monotonic; 2/3: process/thread CPU time, approximated by monotonic.
            const ms = id === 0 ? Date.now() : host.now_ms();
            view().setBigUint64(outPtr, BigInt(Math.round(ms * 1e6)), true);
            return 0;
        },
        clock_res_get(_id, outPtr) {
            view().setBigUint64(outPtr, 1000n, true);
            return 0;
        },
        random_get(ptr, len) {
            host.random_fill(ptr, len);
            return 0;
        },
        // Only clock subscriptions (what sleep()/nanosleep() use): wait for the earliest one.
        poll_oneoff(inPtr, outPtr, nsubs, neventsPtr) {
            const v = view();
            let earliest = null;
            for (let i = 0; i < nsubs; i++) {
                const sub = inPtr + 48 * i;
                if (v.getUint8(sub + 8) !== 0) return ENOSYS;
                const clockId = v.getUint32(sub + 16, true);
                let ns = Number(v.getBigUint64(sub + 24, true));
                if (v.getUint16(sub + 40, true) & 1) { // absolute time
                    const now = clockId === 0 ? Date.now() : host.now_ms();
                    ns -= now * 1e6;
                }
                if (earliest === null || ns < earliest.ns) earliest = { ns, userdata: v.getBigUint64(sub, true) };
            }
            if (earliest === null) return EINVAL;
            if (earliest.ns > 0) Atomics.wait(sleepCell, 0, 0, earliest.ns / 1e6);
            const out = view();
            out.setBigUint64(outPtr, earliest.userdata, true);
            out.setUint16(outPtr + 8, 0, true);
            out.setUint8(outPtr + 10, 0); // eventtype: clock
            out.setUint32(neventsPtr, 1, true);
            return 0;
        },
        sched_yield: () => 0,
        proc_exit(code) {
            throw new ProcExit(code);
        },
    };
    return new Proxy(impl, { get: (target, name) => target[name] ?? (() => ENOSYS) });
}

// Stdin ring from the page: Int32 header [WRITE, READ, CLOSED] then bytes. Blocks until at
// least one byte arrives or stdin is closed (0 bytes: end of file).
function readStdin(iovs, iovsLen) {
    if (!stdin) return 0;
    const { hdr, data } = stdin;
    for (;;) {
        const seq = Atomics.load(shared, SHARED.STDIN_STATE);
        let r = Atomics.load(hdr, 1) >>> 0;
        const w = Atomics.load(hdr, 0) >>> 0;
        if (w !== r) {
            const v = view();
            let n = 0;
            for (let i = 0; i < iovsLen && r !== w; i++) {
                const ptr = v.getUint32(iovs + i * 8, true);
                const len = v.getUint32(iovs + i * 8 + 4, true);
                const mem = u8();
                for (let j = 0; j < len && r !== w; j++, r++, n++) mem[ptr + j] = data[r % data.length];
            }
            Atomics.store(hdr, 1, r | 0);
            return n;
        }
        if (Atomics.load(hdr, 2) !== 0) return 0;
        Atomics.wait(shared, SHARED.STDIN_STATE, seq);
    }
}

/*------------------ Calls from zenoh.js (session) ------------------*/

// Copies JS arguments into wasm memory for the duration of one call: a string becomes a
// pointer to a NUL-terminated copy, a Uint8Array a (pointer, length) pair, null a NULL pointer.
function withArgs(args, fn) {
    const allocated = [];
    const alloc = (n) => {
        const ptr = wasm.zw_alloc(n);
        if (ptr === 0) throw new Error('zenoh: out of memory');
        allocated.push(ptr);
        return ptr;
    };
    const lower = (arg) => {
        if (arg === null || arg === undefined) return [0];
        if (typeof arg === 'string') {
            const bytes = encoder.encode(arg);
            const ptr = alloc(bytes.length + 1);
            const mem = u8();
            mem.set(bytes, ptr);
            mem[ptr + bytes.length] = 0;
            return [ptr];
        }
        if (arg instanceof Uint8Array) {
            const ptr = alloc(Math.max(arg.length, 1));
            u8().set(arg, ptr);
            return [ptr, arg.length];
        }
        if (typeof arg === 'boolean') return [arg ? 1 : 0];
        return [arg];
    };
    try {
        return fn(...args.flatMap(lower));
    } finally {
        for (const ptr of allocated) wasm.zw_free(ptr);
    }
}

// Calls that need more than lowering their arguments.
const methods = {
    open(connect, mode) {
        const res = withArgs([connect, mode], wasm.zw_open);
        if (res === 0) watchSession();
        return res;
    },
    close() {
        sessionOpen = false;
        wasm.zw_close();
        return 0;
    },
    zid() {
        const ptr = wasm.zw_alloc(33);
        try {
            const res = wasm.zw_zid(ptr);
            return res < 0 ? res : readStr(ptr, 32);
        } finally {
            wasm.zw_free(ptr);
        }
    },
    info(peers) {
        const max = 64;
        const ptr = wasm.zw_alloc(33 * max);
        try {
            const n = wasm.zw_info(peers ? 1 : 0, ptr, max);
            if (n < 0) return n;
            return Array.from({ length: Math.min(n, max) }, (_, i) => readStr(ptr + 33 * i, 32));
        } finally {
            wasm.zw_free(ptr);
        }
    },
    keyexprCanonize(keyexpr) {
        const bytes = encoder.encode(keyexpr);
        const ptr = wasm.zw_alloc(bytes.length + 1);
        try {
            u8().set(bytes, ptr);
            u8()[ptr + bytes.length] = 0;
            const len = wasm.zw_keyexpr_canonize(ptr);
            return len < 0 ? len : readStr(ptr, len);
        } finally {
            wasm.zw_free(ptr);
        }
    },
};

/*------------------ Keeping a session alive ------------------*/
// Single-threaded builds make progress (reads, keep-alives, lease, reconnects) only while
// zw_spin runs. Spin once per macrotask so calls get a turn in between; a MessageChannel
// avoids the clamping applied to nested setTimeout(0). Multi-threaded builds run zenoh-pico's
// own tasks on thread workers and only need to notice when the session dies.

let sessionOpen = false;
const pump = new MessageChannel();
let pumpScheduled = false;

function watchSession() {
    sessionOpen = true;
    if (wasm.zw_spin) {
        schedulePump();
    } else {
        const timer = setInterval(() => {
            if (!sessionOpen) {
                clearInterval(timer);
            } else if (wasm.zw_session_check() < 0) {
                clearInterval(timer);
                sessionOpen = false;
                postMessage({ type: 'closed' });
            }
        }, 500);
    }
}

function schedulePump() {
    if (!pumpScheduled) {
        pumpScheduled = true;
        pump.port2.postMessage(null);
    }
}

pump.port1.onmessage = () => {
    pumpScheduled = false;
    if (!sessionOpen) return;
    let res;
    try {
        res = wasm.zw_spin();
    } catch (err) {
        fatal(err);
        return;
    }
    if (res < 0) {
        sessionOpen = false;
        postMessage({ type: 'closed' });
        return;
    }
    schedulePump();
};

/*------------------ Startup ------------------*/

function fatal(err) {
    postMessage({ type: 'fatal', message: String(err?.stack ?? err) });
}

async function init(msg) {
    role = msg.role;
    memory = msg.memory;
    shared = new Int32Array(msg.shared);
    clockBase = msg.clockBase;
    args = msg.args ?? [];
    if (msg.stdin) stdin = { hdr: new Int32Array(msg.stdin, 0, 4), data: new Uint8Array(msg.stdin, 16) };
    const instance = await WebAssembly.instantiate(msg.module, {
        env: { memory },
        zenoh_host: host,
        wasi: wasiThreads,
        wasi_snapshot_preview1: makeWasi(),
    });
    wasm = instance.exports;
    if (role === 'session') wasm._initialize();
}

function runProgram() {
    let code = 0;
    try {
        wasm._start();
    } catch (err) {
        if (!(err instanceof ProcExit)) {
            fatal(err);
            return;
        }
        code = err.code;
    }
    postMessage({ type: 'exit', code });
}

function runThread(tid, arg) {
    try {
        wasm.wasi_thread_start(tid, arg);
    } catch (err) {
        if (err instanceof ProcExit) {
            postMessage({ type: 'exit', code: err.code });
            return;
        }
        if (!(err instanceof ThreadExit)) {
            fatal(err);
            return;
        }
    }
    // Nothing runs on the thread's stack any more: let pthread_join (or the reaper) free it.
    const cell = new Int32Array(memory.buffer, arg + THREAD.EXITED, 1);
    Atomics.store(cell, 0, 1);
    Atomics.notify(cell, 0);
    postMessage({ type: 'thread-done' });
}

let ready = null;

self.onmessage = async ({ data: msg }) => {
    switch (msg.type) {
        case 'init':
            ready = init(msg);
            try {
                await ready;
                postMessage({ type: 'ready' });
            } catch (err) {
                postMessage({ type: 'init-error', message: String(err?.stack ?? err) });
                return;
            }
            if (role === 'program') runProgram();
            return;
        case 'start':
            await ready;
            runThread(msg.tid, msg.arg);
            return;
        case 'call':
            try {
                // Built-in methods, or the zw_<snake_case name> export (e.g. declareSubscriber ->
                // zw_declare_subscriber, or zw_bench in the benchmark).
                const exported = wasm[`zw_${msg.method.replace(/[A-Z]/g, (c) => `_${c.toLowerCase()}`)}`];
                const method = methods[msg.method] ?? (exported && ((...a) => withArgs(a, exported)));
                if (!method) throw new Error(`zenoh-pico.wasm has no method ${msg.method}`);
                postMessage({ type: 'result', id: msg.id, value: method(...msg.args) });
            } catch (err) {
                postMessage({ type: 'error', id: msg.id, message: String(err?.stack ?? err) });
            }
            return;
    }
};
