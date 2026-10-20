// Page side of a zenoh-pico wasm instance: the shared memory, the workers that run wasm on it
// (zenoh-worker.js), and the WebSockets, which only exist on the page here.
//
// Used by zenoh.js for both Sessions and C programs. One Runtime is one "process": a main
// worker plus one worker per thread the program starts (wasi-threads), all on the same
// WebAssembly.Memory. Received WebSocket messages are written straight into the socket's ring
// in that memory (struct zw_ws, wasm/ws.c) and wasm is woken with Atomics.notify.

import { CTRL, SHARED, WS, WS_STATE } from './zenoh-shared.js';

const moduleCache = new Map();

// Compiles (once per URL) and returns { module, memoryLimits, threads }.
export function loadWasm(url) {
    const key = String(url);
    if (!moduleCache.has(key)) {
        const promise = (async () => {
            const res = await fetch(key);
            if (!res.ok) throw new Error(`fetching ${key}: HTTP ${res.status}`);
            const bytes = new Uint8Array(await res.arrayBuffer());
            const module = await WebAssembly.compile(bytes);
            const imports = WebAssembly.Module.imports(module);
            return {
                module,
                memoryLimits: importedMemoryLimits(bytes),
                threads: imports.some((i) => i.module === 'wasi' && i.name === 'thread-spawn'),
            };
        })();
        moduleCache.set(key, promise);
        promise.catch(() => moduleCache.delete(key));
    }
    return moduleCache.get(key);
}

// The (initial, maximum) pages of the module's imported memory, from its import section:
// the page has to create a memory that satisfies them.
function importedMemoryLimits(bytes) {
    let p = 8;
    const leb = () => {
        let result = 0;
        let shift = 0;
        let byte;
        do {
            byte = bytes[p++];
            result += (byte & 0x7f) * 2 ** shift;
            shift += 7;
        } while (byte & 0x80);
        return result;
    };
    while (p < bytes.length) {
        const skip = (n) => {
            p += n;
        };
        const id = bytes[p++];
        const size = leb();
        const end = p + size;
        if (id === 2) {
            for (let n = leb(); n > 0; n--) {
                skip(leb()); // module name
                skip(leb()); // field name
                const kind = bytes[p++];
                if (kind === 0) { // function
                    leb();
                } else if (kind === 1) { // table
                    p++;
                    const flags = bytes[p++];
                    leb();
                    if (flags & 1) leb();
                } else if (kind === 2) { // memory
                    const flags = bytes[p++];
                    const initial = leb();
                    const maximum = flags & 1 ? leb() : undefined;
                    return { initial, maximum, shared: (flags & 2) !== 0 };
                } else if (kind === 3) { // global
                    p += 2;
                } else {
                    break;
                }
            }
        }
        p = end;
    }
    throw new Error('the wasm module does not import its memory');
}

export function defaultWsUrl(host, port) {
    const secure = globalThis.location?.protocol === 'https:';
    const h = host.includes(':') ? `[${host}]` : host;
    return `${secure ? 'wss' : 'ws'}://${h}:${port}/`;
}

export class Runtime {
    #wasm;
    #workerUrl;
    #wsUrl;
    #onEvent;
    #memory;
    #shared;
    #stdin = null;
    #main;
    #idleThreads = [];
    #threadWorkers = new Set();
    #sockets = new Map(); // struct zw_ws address -> { socket, pending, timer, released }
    #ctrl = 0;            // address of zw_ctrl, learned from the first socket
    #i32 = null;
    #u8 = null;
    #dead = false;

    /**
     * @param {object} options
     * @param {'session'|'program'} options.role
     * @param {{module, memoryLimits, threads}} options.wasm from loadWasm()
     * @param {(msg: object) => void} options.onEvent everything that is not runtime plumbing:
     *   call results, callbacks, logs, 'exit', 'closed', and 'fatal' when a worker crashed
     */
    constructor({ role, wasm, workerUrl, wsUrl = defaultWsUrl, onEvent, args = [] }) {
        if (!globalThis.crossOriginIsolated) {
            throw new Error(
                'zenoh-pico needs a cross-origin isolated page (SharedArrayBuffer). Serve it with ' +
                '"Cross-Origin-Opener-Policy: same-origin" and "Cross-Origin-Embedder-Policy: require-corp".');
        }
        this.#wasm = wasm;
        this.#workerUrl = workerUrl;
        this.#wsUrl = wsUrl;
        this.#onEvent = onEvent;
        const { initial, maximum } = wasm.memoryLimits;
        this.#memory = new WebAssembly.Memory({ initial, maximum, shared: true });
        this.#shared = new Int32Array(new SharedArrayBuffer(SHARED.INTS * 4));
        if (role === 'program') {
            // [WRITE, READ, CLOSED, -] then 64 KiB of data
            const sab = new SharedArrayBuffer(16 + 65536);
            this.#stdin = { sab, hdr: new Int32Array(sab, 0, 4), data: new Uint8Array(sab, 16) };
        }
        this.#main = this.#startWorker({ role, args });
        // zenoh-pico starts its threads while opening a session; have a couple ready.
        if (wasm.threads) for (let i = 0; i < 2; i++) this.#idleThreads.push(this.#startThreadWorker());
    }

    /** Resolves once the main instance is ready (for a program: once main() is running). */
    get ready() {
        return this.#main.ready;
    }

    /** Posts a message to the main worker. */
    post(msg) {
        if (!this.#dead) this.#main.worker.postMessage(msg);
    }

    /**
     * Single-threaded sessions: asks wasm to return from a blocking read inside zw_spin so the
     * worker can take the call just posted.
     */
    interrupt() {
        if (this.#ctrl === 0 || this.#dead) return;
        Atomics.store(this.#view32(), (this.#ctrl + CTRL.INTERRUPT) >> 2, 1);
        this.#wake();
    }

    /** Programs: appends text or bytes to stdin. */
    writeStdin(data) {
        const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data;
        const { hdr, data: buf } = this.#stdin;
        let w = Atomics.load(hdr, 0) >>> 0;
        const r = Atomics.load(hdr, 1) >>> 0;
        if (bytes.length > buf.length - ((w - r) >>> 0)) throw new Error('stdin buffer full');
        for (const b of bytes) buf[w++ % buf.length] = b;
        Atomics.store(hdr, 0, w | 0);
        this.#notifyStdin();
    }

    /** Programs: end of file on stdin. */
    closeStdin() {
        Atomics.store(this.#stdin.hdr, 2, 1);
        this.#notifyStdin();
    }

    /** Stops every worker and closes every socket. */
    terminate() {
        if (this.#dead) return;
        this.#dead = true;
        this.#main.worker.terminate();
        for (const w of this.#threadWorkers) w.worker.terminate();
        this.#threadWorkers.clear();
        this.#idleThreads = [];
        for (const s of this.#sockets.values()) {
            clearTimeout(s.timer);
            s.socket?.close();
        }
        this.#sockets.clear();
    }

    /*------------------ Workers ------------------*/

    #startWorker({ role, args = [] }) {
        const worker = new Worker(this.#workerUrl, { type: 'module' });
        const w = { worker, ready: null };
        w.ready = new Promise((resolve, reject) => {
            worker.onmessage = ({ data }) => {
                if (data.type === 'ready') resolve();
                else if (data.type === 'init-error') reject(new Error(data.message));
                this.#onWorkerMessage(w, data);
            };
            worker.onerror = (ev) => {
                const message = ev.message ?? 'zenoh worker failed';
                reject(new Error(message));
                this.#fatal(message);
            };
        });
        w.ready.catch(() => {});
        worker.postMessage({
            type: 'init',
            role,
            module: this.#wasm.module,
            memory: this.#memory,
            shared: this.#shared.buffer,
            stdin: this.#stdin?.sab,
            clockBase: performance.timeOrigin,
            args,
        });
        return w;
    }

    #startThreadWorker() {
        const w = this.#startWorker({ role: 'thread' });
        this.#threadWorkers.add(w);
        return w;
    }

    #onWorkerMessage(w, msg) {
        switch (msg.type) {
            case 'ready':
            case 'init-error':
                return;
            case 'spawn': {
                const t = this.#idleThreads.pop() ?? this.#startThreadWorker();
                t.worker.postMessage({ type: 'start', tid: msg.tid, arg: msg.arg });
                return;
            }
            case 'thread-done':
                this.#idleThreads.push(w);
                return;
            case 'ws-open':
                this.#openSocket(msg);
                return;
            case 'ws-kick':
                this.#send(this.#sockets.get(msg.ws));
                return;
            case 'ws-close':
                this.#release(msg.ws);
                return;
            case 'fatal':
                this.#fatal(msg.message);
                return;
            default:
                this.#onEvent(msg);
        }
    }

    #fatal(message) {
        if (this.#dead) return;
        console.error('zenoh: a worker crashed:', message);
        this.terminate();
        this.#onEvent({ type: 'fatal', message });
    }

    #notifyStdin() {
        Atomics.add(this.#shared, SHARED.STDIN_STATE, 1);
        Atomics.notify(this.#shared, SHARED.STDIN_STATE);
    }

    /*------------------ Shared memory ------------------*/

    // Views over the memory; shared memory can grow, so recreate them when it has.
    #view32() {
        const buffer = this.#memory.buffer;
        if (this.#i32?.buffer !== buffer) {
            this.#i32 = new Int32Array(buffer);
            this.#u8 = new Uint8Array(buffer);
        }
        return this.#i32;
    }

    #view8() {
        this.#view32();
        return this.#u8;
    }

    #field(ws, offset) {
        return (ws + offset) >> 2;
    }

    #wake() {
        const i32 = this.#view32();
        Atomics.add(i32, this.#ctrl >> 2, 1);
        Atomics.notify(i32, this.#ctrl >> 2);
    }

    /*------------------ WebSockets ------------------*/

    #setState(ws, state) {
        Atomics.store(this.#view32(), this.#field(ws, WS.STATE), state);
        this.#wake();
    }

    // wasm rings zw_ctrl.tx_bell when a socket has data to send; answer with Atomics.waitAsync
    // where the browser has it (otherwise wasm posts ws-kick messages instead).
    #listenToDoorbell() {
        if (typeof Atomics.waitAsync !== 'function') return;
        const bell = (this.#ctrl + CTRL.TX_BELL) >> 2;
        const arm = () => {
            if (this.#dead) return;
            const i32 = this.#view32();
            const seen = Atomics.load(i32, bell);
            for (const s of this.#sockets.values()) this.#send(s);
            const res = Atomics.waitAsync(i32, bell, seen);
            if (res.async) res.value.then(arm);
            else setTimeout(arm, 0); // rung meanwhile: drain again without starving the page
        };
        Atomics.store(this.#view32(), (this.#ctrl + CTRL.DOORBELL) >> 2, 1);
        arm();
    }

    #openSocket({ ws, host, port }) {
        if (this.#ctrl === 0) {
            this.#ctrl = this.#view32()[this.#field(ws, WS.CTRL)];
            this.#listenToDoorbell();
        }
        const s = { ws, socket: null, pending: [], timer: 0, released: false };
        this.#sockets.set(ws, s);
        try {
            s.socket = new WebSocket(this.#wsUrl(host, port));
        } catch (err) {
            console.error('zenoh: cannot open WebSocket', err);
            this.#setState(ws, WS_STATE.CLOSED);
            return;
        }
        s.socket.binaryType = 'arraybuffer';
        const closed = () => {
            if (!s.released) this.#setState(ws, WS_STATE.CLOSED);
        };
        s.socket.onopen = () => {
            if (!s.released) this.#setState(ws, WS_STATE.OPEN);
        };
        s.socket.onclose = closed;
        s.socket.onerror = closed;
        s.socket.onmessage = ({ data }) => {
            if (s.released || !(data instanceof ArrayBuffer)) return; // zenoh only sends binary frames
            s.pending.push(new Uint8Array(data));
            this.#receive(s);
        };
    }

    // Moves received messages into the socket's rx ring, retrying shortly if wasm is behind.
    #receive(s) {
        if (s.released) return;
        const i32 = this.#view32();
        const u32 = (off) => i32[this.#field(s.ws, off)] >>> 0;
        const capacity = u32(WS.RX_CAPACITY);
        const rx = u32(WS.RX);
        const mem = this.#view8();
        let wrote = false;
        while (s.pending.length > 0) {
            const msg = s.pending[0];
            const need = 4 + msg.length;
            if (need > capacity) {
                console.error(`zenoh: dropping a ${msg.length}-byte message larger than the receive ring`);
                s.pending.shift();
                continue;
            }
            const w = Atomics.load(i32, this.#field(s.ws, WS.RX_WRITE)) >>> 0;
            const r = Atomics.load(i32, this.#field(s.ws, WS.RX_READ)) >>> 0;
            if (capacity - ((w - r) >>> 0) < need) break;
            const len = new Uint8Array(new Uint32Array([msg.length]).buffer);
            this.#copyIn(mem, rx, capacity, w, len);
            this.#copyIn(mem, rx, capacity, w + 4, msg);
            Atomics.store(i32, this.#field(s.ws, WS.RX_WRITE), (w + need) | 0);
            s.pending.shift();
            wrote = true;
        }
        if (wrote) this.#wake();
        if (s.pending.length > 0 && !s.timer) {
            s.timer = setTimeout(() => {
                s.timer = 0;
                this.#receive(s);
            }, 1);
        }
    }

    // Sends everything wasm has queued in the socket's tx ring.
    #send(s) {
        if (!s || s.released || !s.socket) return;
        const i32 = this.#view32();
        // Clear the flag first: anything queued after this point posts a new kick.
        Atomics.store(i32, this.#field(s.ws, WS.TX_KICK), 0);
        const capacity = i32[this.#field(s.ws, WS.TX_CAPACITY)] >>> 0;
        const tx = i32[this.#field(s.ws, WS.TX)] >>> 0;
        const mem = this.#view8();
        let r = Atomics.load(i32, this.#field(s.ws, WS.TX_READ)) >>> 0;
        const w = Atomics.load(i32, this.#field(s.ws, WS.TX_WRITE)) >>> 0;
        if (r === w) return;
        const open = s.socket?.readyState === WebSocket.OPEN;
        while (r !== w) {
            const lenBytes = this.#copyOut(mem, tx, capacity, r, 4);
            const len = new DataView(lenBytes.buffer).getUint32(0, true);
            // WebSocket.send refuses views of shared memory: this copy is what gets sent.
            if (open) s.socket.send(this.#copyOut(mem, tx, capacity, r + 4, len));
            r = (r + 4 + len) >>> 0;
        }
        Atomics.store(i32, this.#field(s.ws, WS.TX_READ), r | 0);
        this.#wake();
    }

    #release(ws) {
        const s = this.#sockets.get(ws);
        // Send what is still queued first: this message can overtake the doorbell for the last
        // writes (zenoh-pico's close message, the tail of a burst), and the browser sends
        // everything queued with send() before the closing handshake.
        this.#send(s);
        this.#sockets.delete(ws);
        if (s) {
            s.released = true;
            clearTimeout(s.timer);
            s.socket?.close();
        }
        // From now on the page never touches this memory again; wasm may free it.
        this.#setState(ws, WS_STATE.RELEASED);
    }

    #copyIn(mem, ring, capacity, pos, bytes) {
        const start = pos % capacity;
        const first = Math.min(bytes.length, capacity - start);
        mem.set(bytes.subarray(0, first), ring + start);
        mem.set(bytes.subarray(first), ring);
    }

    #copyOut(mem, ring, capacity, pos, len) {
        const out = new Uint8Array(len);
        const start = pos % capacity;
        const first = Math.min(len, capacity - start);
        out.set(mem.subarray(ring + start, ring + start + first), 0);
        out.set(mem.subarray(ring, ring + len - first), first);
        return out;
    }
}
