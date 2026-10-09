// Runs one zenoh-pico session (zenoh-pico.wasm) inside a dedicated Web Worker.
//
// zenoh-pico is written against blocking I/O. A worker may block with Atomics.wait, so the
// zenoh_host imports below block for real: WebSockets live on the main thread (zenoh.js),
// which pushes received messages into a SharedArrayBuffer ring per socket and wakes us.
//
// Protocol with zenoh.js:
//   main -> worker  {type: 'init', module, ctrl}            -> {type: 'ready'}
//                   {type: 'call', id, method, args}        -> {type: 'result', id, value} | {type: 'error', id, ...}
//   worker -> main  {type: 'ws-open', sock, host, port, ring}
//                   {type: 'ws-send', sock, data}
//                   {type: 'ws-close', sock}
//                   {type: 'sample' | 'reply' | 'reply-done' | 'closed' | 'log', ...}

import { CTRL, RING, SOCK_STATE } from './zenoh-shared.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

let wasm = null;          // instance exports
let ctrl = null;          // Int32Array over the SharedArrayBuffer from zenoh.js
let sessionOpen = false;
let inSpin = false;       // interrupts from the main thread only cut short reads made by zw_spin
let nextSock = 0;
const sockets = new Map(); // sock -> { hdr: Int32Array, data: Uint8Array, capacity }

const u8 = () => new Uint8Array(wasm.memory.buffer);
const readStr = (ptr, len) => decoder.decode(u8().subarray(ptr, ptr + len));
const copyBytes = (ptr, len) => u8().slice(ptr, ptr + len);

/*------------------ Blocking helpers ------------------*/

// Waits until `ready()` holds or `ms` elapse. Returns ready()'s last value.
function waitFor(ready, ms) {
    const deadline = performance.now() + ms;
    for (;;) {
        const seq = Atomics.load(ctrl, CTRL.WAKE);
        const r = ready();
        if (r) return r;
        const remaining = deadline - performance.now();
        if (remaining <= 0) return r;
        Atomics.wait(ctrl, CTRL.WAKE, seq, remaining);
    }
}

function takeInterrupt() {
    return inSpin && Atomics.exchange(ctrl, CTRL.INTERRUPT, 0) !== 0;
}

/*------------------ Socket rings ------------------*/
// Ring layout: Int32 header [WRITE, READ, STATE] followed by `capacity` bytes holding
// records of [u32 little-endian length][message]. WRITE/READ are free-running byte counters.

function ringHasMessage(s) {
    return Atomics.load(s.hdr, RING.WRITE) !== Atomics.load(s.hdr, RING.READ);
}

function ringRead(s, dst, dstLen) {
    const mask = s.capacity - 1;
    let r = Atomics.load(s.hdr, RING.READ) >>> 0;
    let len = 0;
    for (let i = 0; i < 4; i++) len |= s.data[(r + i) & mask] << (8 * i);
    len >>>= 0;
    const n = Math.min(len, dstLen);
    if (n < len) console.warn(`zenoh: ${len - n} bytes of a WebSocket message did not fit in the read buffer`);
    const mem = u8();
    const start = (r + 4) & mask;
    const first = Math.min(n, s.capacity - start);
    mem.set(s.data.subarray(start, start + first), dst);
    mem.set(s.data.subarray(0, n - first), dst + first);
    Atomics.store(s.hdr, RING.READ, (r + 4 + len) | 0);
    return n;
}

/*------------------ zenoh_host imports ------------------*/

const host = {
    now_ms: () => performance.now(),
    date_now_ms: () => Date.now(),
    random_fill(ptr, len) {
        // getRandomValues refuses views over shared memory and caps each call at 64 KiB.
        for (let off = 0; off < len; off += 65536) {
            const chunk = crypto.getRandomValues(new Uint8Array(Math.min(65536, len - off)));
            u8().set(chunk, ptr + off);
        }
    },
    sleep_ms(ms) {
        if (ms > 0) Atomics.wait(ctrl, CTRL.SLEEP, 0, ms);
    },

    ws_open(hostPtr, hostLen, portPtr, portLen, timeoutMs) {
        const sock = nextSock++;
        const capacity = RING.CAPACITY;
        const ring = new SharedArrayBuffer(RING.HEADER_BYTES + capacity);
        const s = {
            hdr: new Int32Array(ring, 0, RING.HEADER_BYTES / 4),
            data: new Uint8Array(ring, RING.HEADER_BYTES, capacity),
            capacity,
        };
        Atomics.store(s.hdr, RING.STATE, SOCK_STATE.CONNECTING);
        sockets.set(sock, s);
        postMessage({ type: 'ws-open', sock, host: readStr(hostPtr, hostLen), port: readStr(portPtr, portLen), ring });

        const state = waitFor(() => {
            const st = Atomics.load(s.hdr, RING.STATE);
            return st === SOCK_STATE.CONNECTING ? 0 : st;
        }, timeoutMs);
        if (state === SOCK_STATE.OPEN) return sock;
        host.ws_close(sock);
        return -1;
    },

    ws_send(sock, ptr, len) {
        const s = sockets.get(sock);
        if (!s || Atomics.load(s.hdr, RING.STATE) !== SOCK_STATE.OPEN) return -1;
        const data = copyBytes(ptr, len);
        postMessage({ type: 'ws-send', sock, data }, [data.buffer]);
        return len;
    },

    ws_recv(sock, ptr, len, timeoutMs) {
        const s = sockets.get(sock);
        if (!s) return -1;
        // Messages already received are delivered even if the socket has since closed.
        const ready = () => {
            if (ringHasMessage(s)) return 'data';
            if (Atomics.load(s.hdr, RING.STATE) !== SOCK_STATE.OPEN) return 'closed';
            if (takeInterrupt()) return 'interrupted';
            return null;
        };
        const what = waitFor(ready, timeoutMs);
        if (what === 'data') return ringRead(s, ptr, len);
        if (what === 'closed') {
            // Behave like a timed-out read so the read task does not spin hot while the lease
            // (or auto-reconnect) notices that the link is gone.
            waitFor(takeInterrupt, timeoutMs);
            return -1;
        }
        return 0;
    },

    ws_close(sock) {
        if (sockets.delete(sock)) postMessage({ type: 'ws-close', sock });
    },

    on_sample(sub, kind, kePtr, keLen, payloadPtr, payloadLen, encPtr, encLen) {
        const payload = copyBytes(payloadPtr, payloadLen);
        postMessage({
            type: 'sample', sub, kind,
            keyexpr: readStr(kePtr, keLen), encoding: readStr(encPtr, encLen), payload,
        }, [payload.buffer]);
    },

    on_reply(get, ok, kePtr, keLen, payloadPtr, payloadLen, encPtr, encLen) {
        const payload = copyBytes(payloadPtr, payloadLen);
        postMessage({
            type: 'reply', get, ok: ok !== 0,
            keyexpr: readStr(kePtr, keLen), encoding: readStr(encPtr, encLen), payload,
        }, [payload.buffer]);
    },

    on_reply_done(get) {
        postMessage({ type: 'reply-done', get });
    },
};

// Release builds import nothing from WASI. Debug builds (UBSan runtime) and builds with
// -Dlog-level pull in a few calls: route stdout/stderr to the page's console and stub the rest.
function makeWasi() {
    const EBADF = 8;
    const ENOSYS = 52;
    const lines = { 1: '', 2: '' };
    const zeroCounts = (countPtr, sizePtr) => {
        const view = new DataView(wasm.memory.buffer);
        view.setUint32(countPtr, 0, true);
        view.setUint32(sizePtr, 0, true);
        return 0;
    };
    const impl = {
        // No preopened directories: libc's startup scan stops at the first EBADF.
        fd_prestat_get: () => EBADF,
        // stdout/stderr look like a tty so libc keeps them line-buffered: wasi-libc's isatty()
        // wants a character device *without* the fd_seek/fd_tell rights. Otherwise only the
        // first line is flushed and later output sits in libc's buffer.
        fd_fdstat_get(fd, ptr) {
            if (fd !== 1 && fd !== 2) return EBADF;
            const RIGHTS_FD_SEEK = 1n << 2n;
            const RIGHTS_FD_TELL = 1n << 5n;
            const rights = 0xffffffffffffffffn & ~(RIGHTS_FD_SEEK | RIGHTS_FD_TELL);
            const view = new DataView(wasm.memory.buffer);
            view.setUint8(ptr, 2); // filetype: character_device
            view.setUint16(ptr + 2, 0, true);
            view.setBigUint64(ptr + 8, rights, true);
            view.setBigUint64(ptr + 16, rights, true);
            return 0;
        },
        args_sizes_get: zeroCounts,
        args_get: () => 0,
        environ_sizes_get: zeroCounts,
        environ_get: () => 0,
        fd_write(fd, iovs, iovsLen, nwrittenPtr) {
            const view = new DataView(wasm.memory.buffer);
            let written = 0;
            for (let i = 0; i < iovsLen; i++) {
                const ptr = view.getUint32(iovs + i * 8, true);
                const len = view.getUint32(iovs + i * 8 + 4, true);
                lines[fd] = (lines[fd] ?? '') + readStr(ptr, len);
                written += len;
            }
            const parts = lines[fd].split(/\r?\n/);
            lines[fd] = parts.pop();
            // Logged by the page (zenoh.js): ordered with call results, and not lost while this
            // worker is blocked inside a long wasm call.
            for (const line of parts) postMessage({ type: 'log', fd, line });
            new DataView(wasm.memory.buffer).setUint32(nwrittenPtr, written, true);
            return 0;
        },
        clock_time_get(id, _precision, outPtr) {
            const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
            new DataView(wasm.memory.buffer).setBigUint64(outPtr, ns, true);
            return 0;
        },
        random_get(ptr, len) {
            host.random_fill(ptr, len);
            return 0;
        },
        proc_exit(code) {
            throw new Error(`zenoh-pico.wasm exited with code ${code}`);
        },
    };
    return new Proxy(impl, { get: (target, name) => target[name] ?? (() => ENOSYS) });
}

/*------------------ Calls from zenoh.js ------------------*/

// Copies JS strings/bytes into wasm memory for the duration of one call.
function withArgs(args, fn) {
    const allocated = [];
    const lower = (arg) => {
        if (typeof arg === 'string') {
            const bytes = encoder.encode(arg);
            const ptr = wasm.zw_alloc(bytes.length + 1);
            if (ptr === 0) throw new Error('zenoh: out of memory');
            allocated.push(ptr);
            const mem = u8();
            mem.set(bytes, ptr);
            mem[ptr + bytes.length] = 0;
            return [ptr];
        }
        if (arg instanceof Uint8Array) {
            const ptr = wasm.zw_alloc(Math.max(arg.length, 1));
            if (ptr === 0) throw new Error('zenoh: out of memory');
            allocated.push(ptr);
            u8().set(arg, ptr);
            return [ptr, arg.length];
        }
        return [arg];
    };
    try {
        return fn(...args.flatMap(lower));
    } finally {
        for (const ptr of allocated) wasm.zw_free(ptr);
    }
}

const methods = {
    open(connect) {
        const res = withArgs([connect], wasm.zw_open);
        if (res === 0) {
            sessionOpen = true;
            schedulePump();
        }
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
    put: (keyexpr, payload, encoding) => withArgs([keyexpr, payload, encoding], wasm.zw_put),
    delete: (keyexpr) => withArgs([keyexpr], wasm.zw_delete),
    declareSubscriber: (keyexpr) => withArgs([keyexpr], wasm.zw_declare_subscriber),
    undeclareSubscriber: (id) => wasm.zw_undeclare_subscriber(id),
    declarePublisher: (keyexpr) => withArgs([keyexpr], wasm.zw_declare_publisher),
    publisherPut: (id, payload, encoding) => withArgs([id, payload, encoding], wasm.zw_publisher_put),
    undeclarePublisher: (id) => wasm.zw_undeclare_publisher(id),
    get: (id, keyexpr, parameters, timeoutMs) => withArgs([id, keyexpr, parameters, timeoutMs], wasm.zw_get),
};

/*------------------ Background work ------------------*/
// Without threads, zenoh-pico makes progress (reads, keep-alives, lease, reconnects) only
// when zp_spin_once runs. Spin once per macrotask so calls from zenoh.js get a turn in between;
// a MessageChannel avoids the clamping applied to nested setTimeout(0).

const pump = new MessageChannel();
let pumpScheduled = false;

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
    inSpin = true;
    try {
        res = wasm.zw_spin();
    } catch (err) {
        console.error('zenoh: spin failed', err);
        res = -1;
    } finally {
        inSpin = false;
    }
    if (res < 0) {
        sessionOpen = false;
        postMessage({ type: 'closed' });
        return;
    }
    schedulePump();
};

self.onmessage = async ({ data: msg }) => {
    if (msg.type === 'init') {
        try {
            ctrl = new Int32Array(msg.ctrl);
            const instance = await WebAssembly.instantiate(msg.module, {
                zenoh_host: host,
                wasi_snapshot_preview1: makeWasi(),
            });
            wasm = instance.exports;
            wasm._initialize();
            postMessage({ type: 'ready' });
        } catch (err) {
            postMessage({ type: 'init-error', message: String(err?.stack ?? err) });
        }
        return;
    }
    if (msg.type === 'call') {
        try {
            // Built-in methods, or any extra zw_<name> export of a custom build (e.g. zw_bench).
            const exported = wasm[`zw_${msg.method}`];
            const method = methods[msg.method] ?? (exported && ((...args) => withArgs(args, exported)));
            if (!method) throw new Error(`zenoh-pico.wasm has no method ${msg.method}`);
            const value = method(...msg.args);
            postMessage({ type: 'result', id: msg.id, value });
        } catch (err) {
            postMessage({ type: 'error', id: msg.id, message: String(err?.stack ?? err) });
        }
    }
};
