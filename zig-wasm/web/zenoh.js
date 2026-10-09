// Browser API for zenoh-pico.wasm.
//
//   import { Session } from './zenoh.js';
//   const session = await Session.open({ connect: 'ws/127.0.0.1:10000' });
//   await session.declareSubscriber('demo/**', (sample) => console.log(sample.keyexpr, sample.text()));
//   await session.put('demo/hello', 'world');
//
// Each Session runs zenoh-pico in its own Web Worker and owns that worker's WebSockets.
// The page must be cross-origin isolated (COOP: same-origin, COEP: require-corp) because
// the worker blocks on SharedArrayBuffer + Atomics.wait; see tools/serve.py.

import { CTRL, RING, SOCK_STATE } from './zenoh-shared.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const ERROR_NAMES = {
    [-128]: 'GENERIC',
    [-109]: 'KEYEXPR_UNKNOWN',
    [-104]: 'TRANSPORT_OPEN_PARTIAL_CONNECTIVITY',
    [-103]: 'TRANSPORT_NOT_AVAILABLE',
    [-102]: 'TRANSPORT_OPEN_FAILED',
    [-101]: 'TRANSPORT_OPEN_SN_RESOLUTION',
    [-100]: 'TRANSPORT_TX_FAILED',
    [-99]: 'TRANSPORT_RX_FAILED',
    [-91]: 'CONFIG_LOCATOR_INVALID',
    [-78]: 'SYSTEM_OUT_OF_MEMORY',
    [-75]: 'EINVAL',
    [-73]: 'SESSION_CLOSED',
};

export class ZenohError extends Error {
    constructor(operation, code) {
        super(`${operation} failed: ${ERROR_NAMES[code] ?? 'error'} (${code})`);
        this.name = 'ZenohError';
        this.code = code;
    }
}

export class Sample {
    constructor({ keyexpr, kind, payload, encoding }) {
        this.keyexpr = keyexpr;
        this.kind = kind === 1 ? 'delete' : 'put';
        this.payload = payload;
        this.encoding = encoding;
    }

    text() {
        return decoder.decode(this.payload);
    }
}

export class Reply {
    constructor({ ok, keyexpr, payload, encoding }) {
        this.ok = ok;
        this.keyexpr = keyexpr;
        this.payload = payload;
        this.encoding = encoding;
    }

    text() {
        return decoder.decode(this.payload);
    }
}

function toBytes(payload) {
    if (typeof payload === 'string') return encoder.encode(payload);
    if (payload instanceof Uint8Array) return payload;
    if (payload instanceof ArrayBuffer) return new Uint8Array(payload);
    if (ArrayBuffer.isView(payload)) return new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
    throw new TypeError('payload must be a string, ArrayBuffer or typed array');
}

function defaultWsUrl(host, port) {
    const secure = globalThis.location?.protocol === 'https:';
    const h = host.includes(':') ? `[${host}]` : host;
    return `${secure ? 'wss' : 'ws'}://${h}:${port}/`;
}

const moduleCache = new Map();

function compileWasm(url) {
    const key = String(url);
    if (!moduleCache.has(key)) {
        const promise = WebAssembly.compileStreaming(fetch(key)).catch(async () => {
            // Some servers don't send application/wasm; compile from bytes instead.
            const res = await fetch(key);
            if (!res.ok) throw new Error(`fetching ${key}: HTTP ${res.status}`);
            return WebAssembly.compile(await res.arrayBuffer());
        });
        moduleCache.set(key, promise);
        promise.catch(() => moduleCache.delete(key));
    }
    return moduleCache.get(key);
}

export class Subscriber {
    #session;
    #id;

    constructor(session, id, keyexpr) {
        this.#session = session;
        this.#id = id;
        this.keyexpr = keyexpr;
    }

    async undeclare() {
        await this.#session._undeclareSubscriber(this.#id);
    }
}

export class Publisher {
    #session;
    #id;

    constructor(session, id, keyexpr) {
        this.#session = session;
        this.#id = id;
        this.keyexpr = keyexpr;
    }

    async put(payload, { encoding = '' } = {}) {
        await this.#session._call('publisherPut', 'publisher put', this.#id, toBytes(payload), encoding);
    }

    async undeclare() {
        await this.#session._call('undeclarePublisher', 'undeclare publisher', this.#id);
    }
}

export class Session {
    #worker;
    #ctrl;
    #wsUrl;
    #nextCall = 0;
    #calls = new Map();
    #sockets = new Map();
    #subscribers = new Map();
    #nextGet = 0;
    #gets = new Map();
    #closed = false;

    /** Called with no arguments if the session ends without close() (e.g. the lease expired). */
    onclose = null;

    /**
     * Opens a client session.
     *
     * @param {object} options
     * @param {string} [options.connect='ws/127.0.0.1:10000'] zenoh locator of a router's WebSocket listener
     * @param {(host: string, port: string) => string} [options.wsUrl] maps the locator to a WebSocket URL
     *   (default: ws:// on http pages, wss:// on https pages)
     * @param {string|URL} [options.wasmUrl] location of zenoh-pico.wasm
     * @param {string|URL} [options.workerUrl] location of zenoh-worker.js
     */
    static async open(options = {}) {
        const { connect = 'ws/127.0.0.1:10000' } = options;
        const session = await Session._spawn(options);
        try {
            await session._call('open', `open ${connect}`, connect);
            session.zid = await session.#callRaw('zid');
        } catch (err) {
            session.#teardown();
            throw err;
        }
        return session;
    }

    /**
     * @internal Starts the worker and wasm instance without opening a session. Any extra
     * `zw_<name>` export of a custom wasm can then be called with `_call('<name>', ...)`
     * (the benchmark uses this for `zw_bench`).
     */
    static async _spawn({
        wsUrl = defaultWsUrl,
        wasmUrl = new URL('./zenoh-pico.wasm', import.meta.url),
        workerUrl = new URL('./zenoh-worker.js', import.meta.url),
    } = {}) {
        if (!globalThis.crossOriginIsolated) {
            throw new Error(
                'zenoh-pico needs a cross-origin isolated page (SharedArrayBuffer). Serve it with ' +
                '"Cross-Origin-Opener-Policy: same-origin" and "Cross-Origin-Embedder-Policy: require-corp".');
        }
        const module = await compileWasm(wasmUrl);
        const session = new Session(new Worker(workerUrl, { type: 'module' }), wsUrl);
        try {
            await session.#init(module);
        } catch (err) {
            session.#teardown();
            throw err;
        }
        return session;
    }

    constructor(worker, wsUrl) {
        this.#worker = worker;
        this.#wsUrl = wsUrl;
        this.#ctrl = new Int32Array(new SharedArrayBuffer(CTRL.INTS * 4));
        this.zid = null;
    }

    #init(module) {
        return new Promise((resolve, reject) => {
            this.#worker.onmessage = ({ data }) => {
                if (data.type === 'ready') {
                    this.#worker.onmessage = ({ data: msg }) => this.#onWorkerMessage(msg);
                    // A crashed worker must not leave calls pending forever.
                    this.#worker.onerror = (ev) => {
                        console.error('zenoh: worker failed', ev.message);
                        if (!this.#closed) {
                            this.#teardown();
                            this.onclose?.();
                        }
                    };
                    resolve();
                } else if (data.type === 'init-error') {
                    reject(new Error(data.message));
                }
            };
            this.#worker.onerror = (ev) => reject(new Error(ev.message ?? 'zenoh worker failed to start'));
            this.#worker.postMessage({ type: 'init', module, ctrl: this.#ctrl.buffer });
        });
    }

    #wake() {
        Atomics.add(this.#ctrl, CTRL.WAKE, 1);
        Atomics.notify(this.#ctrl, CTRL.WAKE);
    }

    #callRaw(method, ...args) {
        if (this.#closed && method !== 'close') return Promise.reject(new ZenohError(method, -73));
        const id = this.#nextCall++;
        return new Promise((resolve, reject) => {
            this.#calls.set(id, { resolve, reject });
            this.#worker.postMessage({ type: 'call', id, method, args });
            // The worker may be parked in an idle read; let it return to its event loop.
            Atomics.store(this.#ctrl, CTRL.INTERRUPT, 1);
            this.#wake();
        });
    }

    /** @internal Calls a worker method whose result is a z_result_t, or a handle when >= 0. */
    async _call(method, operation, ...args) {
        const res = await this.#callRaw(method, ...args);
        if (res < 0) throw new ZenohError(operation, res);
        return res;
    }

    async put(keyexpr, payload, { encoding = '' } = {}) {
        await this._call('put', `put ${keyexpr}`, keyexpr, toBytes(payload), encoding);
    }

    async delete(keyexpr) {
        await this._call('delete', `delete ${keyexpr}`, keyexpr);
    }

    /** @param {(sample: Sample) => void} handler */
    async declareSubscriber(keyexpr, handler) {
        const id = await this._call('declareSubscriber', `declare subscriber ${keyexpr}`, keyexpr);
        this.#subscribers.set(id, handler);
        return new Subscriber(this, id, keyexpr);
    }

    /** @internal */
    async _undeclareSubscriber(id) {
        this.#subscribers.delete(id);
        await this._call('undeclareSubscriber', 'undeclare subscriber', id);
    }

    async declarePublisher(keyexpr) {
        const id = await this._call('declarePublisher', `declare publisher ${keyexpr}`, keyexpr);
        return new Publisher(this, id, keyexpr);
    }

    /**
     * Queries `keyexpr` and resolves with every reply once the query completes.
     * @param {object} [options]
     * @param {string} [options.parameters]
     * @param {number} [options.timeoutMs] 0 uses zenoh-pico's default
     * @param {(reply: Reply) => void} [options.onReply] called for each reply as it arrives
     * @returns {Promise<Reply[]>}
     */
    async get(keyexpr, { parameters = '', timeoutMs = 0, onReply } = {}) {
        const id = this.#nextGet++;
        const replies = [];
        const done = new Promise((resolve) => this.#gets.set(id, { replies, onReply, resolve }));
        try {
            await this._call('get', `get ${keyexpr}`, id, keyexpr, parameters, timeoutMs);
        } catch (err) {
            this.#gets.delete(id);
            throw err;
        }
        return done;
    }

    async close() {
        if (this.#closed) return;
        try {
            await this.#callRaw('close');
        } finally {
            this.#teardown();
        }
    }

    #teardown() {
        this.#closed = true;
        this.#worker.terminate();
        for (const { ws } of this.#sockets.values()) ws?.close();
        this.#sockets.clear();
        for (const { reject } of this.#calls.values()) reject(new ZenohError('call', -73));
        this.#calls.clear();
        for (const { replies, resolve } of this.#gets.values()) resolve(replies);
        this.#gets.clear();
    }

    #onWorkerMessage(msg) {
        switch (msg.type) {
            case 'result': {
                const call = this.#calls.get(msg.id);
                this.#calls.delete(msg.id);
                call?.resolve(msg.value);
                break;
            }
            case 'error': {
                const call = this.#calls.get(msg.id);
                this.#calls.delete(msg.id);
                call?.reject(new Error(msg.message));
                break;
            }
            case 'ws-open':
                this.#openSocket(msg);
                break;
            case 'ws-send': {
                const s = this.#sockets.get(msg.sock);
                if (s?.ws.readyState === WebSocket.OPEN) s.ws.send(msg.data);
                break;
            }
            case 'ws-close': {
                const s = this.#sockets.get(msg.sock);
                this.#sockets.delete(msg.sock);
                s?.ws.close();
                break;
            }
            case 'sample': {
                const handler = this.#subscribers.get(msg.sub);
                if (handler) {
                    try {
                        handler(new Sample(msg));
                    } catch (err) {
                        console.error('zenoh: subscriber callback threw', err);
                    }
                }
                break;
            }
            case 'reply': {
                const get = this.#gets.get(msg.get);
                if (get) {
                    const reply = new Reply(msg);
                    get.replies.push(reply);
                    try {
                        get.onReply?.(reply);
                    } catch (err) {
                        console.error('zenoh: onReply callback threw', err);
                    }
                }
                break;
            }
            case 'reply-done': {
                const get = this.#gets.get(msg.get);
                this.#gets.delete(msg.get);
                get?.resolve(get.replies);
                break;
            }
            case 'log':
                (msg.fd === 2 ? console.error : console.log)(msg.line);
                break;
            case 'closed':
                if (!this.#closed) {
                    this.#teardown();
                    this.onclose?.();
                }
                break;
        }
    }

    #openSocket({ sock, host, port, ring }) {
        const hdr = new Int32Array(ring, 0, RING.HEADER_BYTES / 4);
        const data = new Uint8Array(ring, RING.HEADER_BYTES);
        const setState = (state) => {
            Atomics.store(hdr, RING.STATE, state);
            this.#wake();
        };
        let ws;
        try {
            ws = new WebSocket(this.#wsUrl(host, port));
        } catch (err) {
            console.error('zenoh: cannot open WebSocket', err);
            setState(SOCK_STATE.CLOSED);
            return;
        }
        ws.binaryType = 'arraybuffer';
        const s = { ws, hdr, data, pending: [], flushTimer: 0 };
        this.#sockets.set(sock, s);
        ws.onopen = () => setState(SOCK_STATE.OPEN);
        ws.onclose = () => setState(SOCK_STATE.CLOSED);
        ws.onerror = () => setState(SOCK_STATE.CLOSED);
        ws.onmessage = ({ data: buf }) => {
            if (!(buf instanceof ArrayBuffer)) return; // zenoh only sends binary frames
            s.pending.push(new Uint8Array(buf));
            this.#flush(s);
        };
    }

    // Moves received messages into the socket's ring, retrying shortly if the worker is behind.
    #flush(s) {
        const mask = RING.CAPACITY - 1;
        let wrote = false;
        while (s.pending.length > 0) {
            const msg = s.pending[0];
            const need = 4 + msg.length;
            if (need > RING.CAPACITY) {
                console.error(`zenoh: dropping a ${msg.length}-byte message larger than the receive ring`);
                s.pending.shift();
                continue;
            }
            const w = Atomics.load(s.hdr, RING.WRITE) >>> 0;
            const r = Atomics.load(s.hdr, RING.READ) >>> 0;
            if (RING.CAPACITY - ((w - r) >>> 0) < need) break;
            for (let i = 0; i < 4; i++) s.data[(w + i) & mask] = (msg.length >>> (8 * i)) & 0xff;
            const start = (w + 4) & mask;
            const first = Math.min(msg.length, RING.CAPACITY - start);
            s.data.set(msg.subarray(0, first), start);
            s.data.set(msg.subarray(first), 0);
            Atomics.store(s.hdr, RING.WRITE, (w + need) | 0);
            s.pending.shift();
            wrote = true;
        }
        if (wrote) this.#wake();
        if (s.pending.length > 0 && !s.flushTimer) {
            s.flushTimer = setTimeout(() => {
                s.flushTimer = 0;
                this.#flush(s);
            }, 1);
        }
    }
}
