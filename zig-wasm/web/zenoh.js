// Browser API for zenoh-pico.wasm.
//
//   import { Session } from './zenoh.js';
//   const session = await Session.open({ connect: 'ws/127.0.0.1:10000' });
//   await session.declareSubscriber('demo/**', (sample) => console.log(sample.keyexpr, sample.text()));
//   await session.put('demo/hello', 'world');
//
// and for C programs built with `zig build app` (or the upstream examples):
//
//   import { runProgram } from './zenoh.js';
//   const program = await runProgram({ wasmUrl: 'examples/z_sub.wasm', args: ['-e', 'ws/127.0.0.1:10000'] });
//   const exitCode = await program.exited;
//
// Each Session or program runs zenoh-pico in Web Workers (one per thread) on one shared
// memory; the page owns their WebSockets. The page must be cross-origin isolated
// (COOP: same-origin, COEP: require-corp) because of that SharedArrayBuffer; see tools/serve.py.

import { Runtime, defaultWsUrl, loadWasm } from './zenoh-runtime.js';

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

/*------------------ Options ------------------*/

const PRIORITIES = {
    'real-time': 1, 'interactive-high': 2, 'interactive-low': 3, 'data-high': 4, data: 5, 'data-low': 6, background: 7,
};
const PRIORITY_NAMES = Object.fromEntries(Object.entries(PRIORITIES).map(([k, v]) => [v, k]));
const CONGESTION_CONTROLS = { drop: 0, block: 1 };
const RELIABILITIES = { reliable: 0, 'best-effort': 1 };
const TARGETS = { 'best-matching': 0, all: 1, 'all-complete': 2 };
const CONSOLIDATIONS = { auto: -1, none: 0, monotonic: 1, latest: 2 };
const UNSET = -1;
const UNSET_CONSOLIDATION = -2147483648;

function lookup(table, value, what, unset = UNSET) {
    if (value === undefined || value === null) return unset;
    if (typeof value === 'number' && Object.values(table).includes(value)) return value;
    if (value in table) return table[value];
    throw new TypeError(`unknown ${what} ${JSON.stringify(value)}; expected one of ${Object.keys(table).join(', ')}`);
}

const qos = ({ congestionControl, priority, express } = {}) => [
    lookup(CONGESTION_CONTROLS, congestionControl, 'congestionControl'),
    lookup(PRIORITIES, priority, 'priority'),
    express === undefined ? UNSET : express ? 1 : 0,
];

function toBytes(payload) {
    if (typeof payload === 'string') return encoder.encode(payload);
    if (payload instanceof Uint8Array) return payload;
    if (payload instanceof ArrayBuffer) return new Uint8Array(payload);
    if (ArrayBuffer.isView(payload)) return new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
    throw new TypeError('payload must be a string, ArrayBuffer or typed array');
}

// (bytes, has) argument pair for optional byte arguments such as attachments.
const optionalBytes = (value) => (value === undefined || value === null ? [new Uint8Array(0), false] : [toBytes(value), true]);

/*------------------ Data ------------------*/

/** NTP64 time (seconds since 1970 in the high 32 bits, fraction in the low 32) to ms. */
function ntp64ToMs(ntp64) {
    return Number(ntp64 >> 32n) * 1000 + (Number(ntp64 & 0xffffffffn) / 2 ** 32) * 1000;
}

export class Sample {
    constructor(msg) {
        this.keyexpr = msg.keyexpr;
        this.kind = msg.kind === 1 ? 'delete' : 'put';
        this.payload = msg.payload;
        this.encoding = msg.encoding;
        this.attachment = msg.attachment;
        this.priority = PRIORITY_NAMES[msg.priority] ?? null;
        this.congestionControl = msg.congestionControl === 1 ? 'block' : msg.congestionControl === 0 ? 'drop' : null;
        this.express = msg.express < 0 ? null : msg.express === 1;
        this.timestamp = msg.timestamp ? { ms: ntp64ToMs(msg.timestamp.ntp64), ntp64: msg.timestamp.ntp64, id: msg.timestamp.id } : null;
    }

    text() {
        return decoder.decode(this.payload);
    }
}

export class Reply {
    constructor(msg) {
        this.ok = msg.kind === 1;
        this.keyexpr = msg.keyexpr;
        this.payload = msg.payload;
        this.encoding = msg.encoding;
        this.attachment = msg.attachment;
        this.sample = this.ok ? new Sample({ ...msg, kind: 0 }) : null;
    }

    text() {
        return decoder.decode(this.payload);
    }
}

export class Query {
    #session;
    #handle;
    #finished = false;

    constructor(session, handle, msg) {
        this.#session = session;
        this.#handle = handle;
        this.keyexpr = msg.keyexpr;
        this.parameters = msg.parameters;
        this.payload = msg.hasPayload ? msg.payload : null;
        this.encoding = msg.encoding;
        this.attachment = msg.attachment;
    }

    text() {
        return this.payload ? decoder.decode(this.payload) : '';
    }

    async reply(keyexpr, payload, { encoding = '', attachment } = {}) {
        await this.#session._call('queryReply', `reply on ${keyexpr}`, this.#handle, keyexpr, toBytes(payload), encoding,
            ...optionalBytes(attachment));
    }

    async replyErr(payload, { encoding = '' } = {}) {
        await this.#session._call('queryReplyErr', 'reply error', this.#handle, toBytes(payload), encoding);
    }

    async replyDelete(keyexpr, { attachment } = {}) {
        await this.#session._call('queryReplyDel', `reply delete on ${keyexpr}`, this.#handle, keyexpr,
            ...optionalBytes(attachment));
    }

    /** @internal Ends the query: the querier gets its final reply. */
    async _finish() {
        if (this.#finished) return;
        this.#finished = true;
        await this.#session._callRaw('queryDrop', this.#handle);
    }
}

/*------------------ Entities ------------------*/

class Entity {
    constructor(session, handle, undeclareMethod, keyexpr) {
        this._session = session;
        this._handle = handle;
        this._undeclareMethod = undeclareMethod;
        this.keyexpr = keyexpr;
    }

    async undeclare() {
        if (this._handle === null) return;
        const handle = this._handle;
        this._handle = null;
        this._session._forget(this);
        await this._session._call(this._undeclareMethod, `undeclare ${this.keyexpr ?? ''}`, handle);
    }
}

export class Subscriber extends Entity {}

export class Queryable extends Entity {}

export class LivelinessToken extends Entity {}

export class MatchingListener extends Entity {}

export class Publisher extends Entity {
    async put(payload, { encoding = '', attachment } = {}) {
        await this._session._call('publisherPut', `put ${this.keyexpr}`, this._handle, toBytes(payload), encoding,
            ...optionalBytes(attachment));
    }

    async delete() {
        await this._session._call('publisherDelete', `delete ${this.keyexpr}`, this._handle);
    }

    /** Whether subscribers currently match this publisher. */
    async matchingStatus() {
        return (await this._session._call('publisherMatchingStatus', 'matching status', this._handle)) === 1;
    }

    /** @param {(matching: boolean) => void} handler called whenever the matching status changes */
    async declareMatchingListener(handler) {
        return this._session._declare(MatchingListener, 'declareMatchingListener', 'undeclareMatchingListener',
            'declare matching listener', null, handler, (id) => [id, this._handle]);
    }
}

export class Querier extends Entity {
    /** Like Session.get, for the querier's key expression and options. */
    async get(parameters = '', { payload, encoding = '', attachment, onReply } = {}) {
        return this._session._query(`get ${this.keyexpr}`, onReply, (id) => [
            'querierGet', this._handle, id, parameters, ...optionalBytes(payload), encoding, ...optionalBytes(attachment),
        ]);
    }
}

/*------------------ Session ------------------*/

export class Session {
    #runtime;
    #nextCall = 0;
    #calls = new Map();
    #nextId = 1;
    #handlers = new Map();   // callback id -> handler (subscribers, queryables, matching listeners)
    #queries = new Map();    // callback id -> { replies, onReply, resolve }
    #entities = new Set();
    #closed = false;
    #threads;

    /** Called with no arguments if the session ends without close() (e.g. the lease expired). */
    onclose = null;

    /**
     * Opens a session.
     *
     * @param {object} options
     * @param {string|string[]} [options.connect='ws/127.0.0.1:10000'] zenoh locator(s) of WebSocket listeners
     * @param {'client'|'peer'} [options.mode='client'] peer mode needs `threads`
     * @param {boolean} [options.threads=false] run zenoh-pico's own tasks on threads
     *   (zenoh-pico-mt.wasm) instead of between calls on one worker (zenoh-pico.wasm)
     * @param {(host: string, port: string) => string} [options.wsUrl] maps a locator to a WebSocket URL
     *   (default: ws:// on http pages, wss:// on https pages)
     * @param {string|URL} [options.wasmUrl] location of the wasm (default: next to this file)
     * @param {string|URL} [options.workerUrl] location of zenoh-worker.js
     */
    static async open(options = {}) {
        const { connect = 'ws/127.0.0.1:10000', mode = 'client' } = options;
        const locators = Array.isArray(connect) ? connect.join(',') : connect;
        const session = await Session._spawn(options);
        try {
            await session._call('open', `open ${locators}`, locators, mode);
            session.zid = await session._callRaw('zid');
        } catch (err) {
            session.#teardown();
            throw err;
        }
        return session;
    }

    /**
     * @internal Starts the workers and wasm instance without opening a session. Any extra
     * `zw_<name>` export of a custom wasm can then be called with `_call('<name>', ...)`
     * (the benchmark uses this for `zw_bench`).
     */
    static async _spawn({
        threads = false,
        wsUrl = defaultWsUrl,
        wasmUrl = new URL(threads ? './zenoh-pico-mt.wasm' : './zenoh-pico.wasm', import.meta.url),
        workerUrl = new URL('./zenoh-worker.js', import.meta.url),
    } = {}) {
        const wasm = await loadWasm(wasmUrl);
        const session = new Session();
        session.#threads = wasm.threads;
        session.#runtime = new Runtime({
            role: 'session', wasm, workerUrl, wsUrl, onEvent: (msg) => session.#onEvent(msg),
        });
        try {
            await session.#runtime.ready;
        } catch (err) {
            session.#teardown();
            throw err;
        }
        return session;
    }

    constructor() {
        this.zid = null;
        this.liveliness = new Liveliness(this);
        this.keyexpr = new KeyExprs(this);
    }

    /** Whether zenoh-pico runs on its own threads in this session. */
    get threads() {
        return this.#threads;
    }

    /** @internal */
    _callRaw(method, ...args) {
        if (this.#closed && method !== 'close') return Promise.reject(new ZenohError(method, -73));
        const id = this.#nextCall++;
        return new Promise((resolve, reject) => {
            this.#calls.set(id, { resolve, reject });
            this.#runtime.post({ type: 'call', id, method, args });
            // A single-threaded worker may be parked in a read inside zw_spin; let it return.
            if (!this.#threads) this.#runtime.interrupt();
        });
    }

    /** @internal Calls a worker method whose result is a z_result_t, or a handle when >= 0. */
    async _call(method, operation, ...args) {
        const res = await this._callRaw(method, ...args);
        if (typeof res === 'number' && res < 0) throw new ZenohError(operation, res);
        return res;
    }

    /** @internal Declares an entity whose callbacks are routed by an id chosen here. */
    async _declare(Class, method, undeclareMethod, operation, keyexpr, handler, makeArgs) {
        const id = this.#nextId++;
        if (handler) this.#handlers.set(id, handler);
        try {
            const handle = await this._call(method, operation, ...makeArgs(id));
            const entity = new Class(this, handle, undeclareMethod, keyexpr);
            entity._id = id;
            this.#entities.add(entity);
            return entity;
        } catch (err) {
            this.#handlers.delete(id);
            throw err;
        }
    }

    /** @internal */
    _forget(entity) {
        this.#entities.delete(entity);
        this.#handlers.delete(entity._id);
    }

    /** @internal Runs a query; resolves with all replies once it is complete. */
    async _query(operation, onReply, makeCall) {
        const id = this.#nextId++;
        const replies = [];
        const done = new Promise((resolve) => this.#queries.set(id, { replies, onReply, resolve }));
        try {
            const [method, ...args] = makeCall(id);
            await this._call(method, operation, ...args);
        } catch (err) {
            this.#queries.delete(id);
            throw err;
        }
        return done;
    }

    /**
     * @param {string} keyexpr
     * @param {string|Uint8Array|ArrayBuffer} payload
     * @param {object} [options] encoding, attachment, congestionControl ('drop'|'block'),
     *   priority ('real-time' ... 'background' or 1-7), express
     */
    async put(keyexpr, payload, options = {}) {
        const { encoding = '', attachment } = options;
        await this._call('put', `put ${keyexpr}`, keyexpr, toBytes(payload), encoding, ...optionalBytes(attachment),
            ...qos(options));
    }

    async delete(keyexpr, options = {}) {
        await this._call('delete', `delete ${keyexpr}`, keyexpr, ...qos(options));
    }

    /** @param {(sample: Sample) => void} handler */
    async declareSubscriber(keyexpr, handler) {
        return this._declare(Subscriber, 'declareSubscriber', 'undeclareSubscriber', `declare subscriber ${keyexpr}`,
            keyexpr, handler, (id) => [id, keyexpr, 0, 0]);
    }

    /**
     * @param {object} [options] encoding, congestionControl, priority, express,
     *   reliability ('reliable'|'best-effort', needs the unstable API)
     */
    async declarePublisher(keyexpr, options = {}) {
        const { encoding = '', reliability } = options;
        return this._declare(Publisher, 'declarePublisher', 'undeclarePublisher', `declare publisher ${keyexpr}`,
            keyexpr, null, () => [keyexpr, encoding, ...qos(options), lookup(RELIABILITIES, reliability, 'reliability')]);
    }

    /**
     * Answers queries on `keyexpr`. The handler gets a Query to reply to (possibly several
     * times); the query is finished when the handler returns, or when its promise settles.
     *
     * @param {(query: Query) => void | Promise<void>} handler
     * @param {object} [options]
     * @param {boolean} [options.complete=false]
     */
    async declareQueryable(keyexpr, handler, { complete = false } = {}) {
        return this._declare(Queryable, 'declareQueryable', 'undeclareQueryable', `declare queryable ${keyexpr}`,
            keyexpr, handler, (id) => [id, keyexpr, complete]);
    }

    /**
     * Queries `keyexpr` and resolves with every reply once the query completes.
     * @param {object} [options]
     * @param {string} [options.parameters]
     * @param {string|Uint8Array} [options.payload]
     * @param {string} [options.encoding]
     * @param {string|Uint8Array} [options.attachment]
     * @param {'best-matching'|'all'|'all-complete'} [options.target]
     * @param {'auto'|'none'|'monotonic'|'latest'} [options.consolidation]
     * @param {number} [options.timeoutMs] 0 uses zenoh-pico's default
     * @param {(reply: Reply) => void} [options.onReply] called for each reply as it arrives
     * @returns {Promise<Reply[]>}
     */
    async get(keyexpr, options = {}) {
        const { parameters = '', payload, encoding = '', attachment, target, consolidation, timeoutMs = 0, onReply } = options;
        return this._query(`get ${keyexpr}`, onReply, (id) => [
            'get', id, keyexpr, parameters, ...optionalBytes(payload), encoding, ...optionalBytes(attachment),
            lookup(TARGETS, target, 'target'), lookup(CONSOLIDATIONS, consolidation, 'consolidation', UNSET_CONSOLIDATION),
            timeoutMs, ...qos(options),
        ]);
    }

    /** A declared querier: repeated gets on one key expression with fixed options. */
    async declareQuerier(keyexpr, options = {}) {
        const { target, consolidation, timeoutMs = 0 } = options;
        return this._declare(Querier, 'declareQuerier', 'undeclareQuerier', `declare querier ${keyexpr}`, keyexpr, null,
            () => [keyexpr, lookup(TARGETS, target, 'target'),
                lookup(CONSOLIDATIONS, consolidation, 'consolidation', UNSET_CONSOLIDATION), timeoutMs, ...qos(options)]);
    }

    /** This session's id and those of the routers and peers it is connected to. */
    async info() {
        const [routers, peers] = await Promise.all([this._call('info', 'info', false), this._call('info', 'info', true)]);
        return { zid: this.zid, routers, peers };
    }

    async close() {
        if (this.#closed) return;
        try {
            await this._callRaw('close');
        } finally {
            this.#teardown();
        }
    }

    #teardown() {
        this.#closed = true;
        this.#runtime?.terminate();
        for (const { reject } of this.#calls.values()) reject(new ZenohError('call', -73));
        this.#calls.clear();
        for (const { replies, resolve } of this.#queries.values()) resolve(replies);
        this.#queries.clear();
        this.#handlers.clear();
    }

    #invoke(what, fn) {
        try {
            const res = fn();
            if (res instanceof Promise) res.catch((err) => console.error(`zenoh: ${what} threw`, err));
        } catch (err) {
            console.error(`zenoh: ${what} threw`, err);
        }
    }

    async #answer(handler, query) {
        try {
            await handler(query);
        } catch (err) {
            console.error('zenoh: queryable callback threw', err);
        } finally {
            query._finish().catch(() => {});
        }
    }

    #onEvent(msg) {
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
            case 'sample': {
                const handler = this.#handlers.get(msg.id);
                if (handler) this.#invoke('subscriber callback', () => handler(new Sample(msg.msg)));
                break;
            }
            case 'query': {
                const handler = this.#handlers.get(msg.id);
                const query = new Query(this, msg.query, msg.msg);
                if (handler) this.#answer(handler, query);
                else query._finish().catch(() => {});
                break;
            }
            case 'matching': {
                const handler = this.#handlers.get(msg.id);
                if (handler) this.#invoke('matching listener', () => handler(msg.matching));
                break;
            }
            case 'reply': {
                const q = this.#queries.get(msg.id);
                if (q) {
                    const reply = new Reply(msg.msg);
                    q.replies.push(reply);
                    if (q.onReply) this.#invoke('onReply callback', () => q.onReply(reply));
                }
                break;
            }
            case 'reply-done': {
                const q = this.#queries.get(msg.id);
                this.#queries.delete(msg.id);
                q?.resolve(q.replies);
                break;
            }
            case 'log':
                (msg.fd === 2 ? console.error : console.log)(msg.line);
                break;
            case 'closed':
            case 'fatal':
                if (!this.#closed) {
                    this.#teardown();
                    this.onclose?.();
                }
                break;
        }
    }
}

class Liveliness {
    #session;

    constructor(session) {
        this.#session = session;
    }

    /** Announces that this session is alive on `keyexpr` until the token is undeclared. */
    async declareToken(keyexpr) {
        return this.#session._declare(LivelinessToken, 'livelinessDeclareToken', 'livelinessUndeclareToken',
            `declare liveliness token ${keyexpr}`, keyexpr, null, () => [keyexpr]);
    }

    /**
     * Samples are puts when a matching token appears and deletes when it goes away.
     * @param {object} [options]
     * @param {boolean} [options.history=false] also report the tokens that are already alive
     */
    async declareSubscriber(keyexpr, handler, { history = false } = {}) {
        return this.#session._declare(Subscriber, 'declareSubscriber', 'undeclareSubscriber',
            `declare liveliness subscriber ${keyexpr}`, keyexpr, handler, (id) => [id, keyexpr, 1, history]);
    }

    /** The liveliness tokens currently alive on `keyexpr`, as replies. */
    async get(keyexpr, { timeoutMs = 0, onReply } = {}) {
        return this.#session._query(`liveliness get ${keyexpr}`, onReply, (id) => ['livelinessGet', id, keyexpr, timeoutMs]);
    }
}

class KeyExprs {
    #session;

    constructor(session) {
        this.#session = session;
    }

    async intersects(a, b) {
        return (await this.#session._call('keyexprRelation', 'key expression', a, b, 0)) === 1;
    }

    async includes(a, b) {
        return (await this.#session._call('keyexprRelation', 'key expression', a, b, 1)) === 1;
    }

    async canonize(keyexpr) {
        return this.#session._call('keyexprCanonize', `canonize ${keyexpr}`, keyexpr);
    }
}

/*------------------ C programs ------------------*/

/**
 * Runs a zenoh-pico C program (a wasm with main(), from `zig build app` or the examples).
 *
 * @param {object} options
 * @param {string|URL} options.wasmUrl
 * @param {string[]} [options.args] command line arguments (argv[1..])
 * @param {(line: string) => void} [options.onStdout] default: console.log
 * @param {(line: string) => void} [options.onStderr] default: console.error
 * @param {(host: string, port: string) => string} [options.wsUrl]
 * @param {string|URL} [options.workerUrl]
 * @returns {Promise<{exited: Promise<number>, stdin: {write(data: string|Uint8Array): void, end(): void}, kill(): void}>}
 *   `exited` resolves with the exit code; kill() stops the program (exit code 137).
 */
export async function runProgram({
    wasmUrl,
    args = [],
    onStdout = console.log,
    onStderr = console.error,
    wsUrl = defaultWsUrl,
    workerUrl = new URL('./zenoh-worker.js', import.meta.url),
}) {
    const wasm = await loadWasm(wasmUrl);
    const name = String(wasmUrl).split('/').pop().replace(/\.wasm$/, '');
    let finish;
    const exited = new Promise((resolve) => (finish = resolve));
    let done = false;
    const end = (code) => {
        if (done) return;
        done = true;
        runtime.terminate();
        finish(code);
    };
    const runtime = new Runtime({
        role: 'program',
        wasm,
        workerUrl,
        wsUrl,
        args: [name, ...args],
        onEvent(msg) {
            if (msg.type === 'log') (msg.fd === 2 ? onStderr : onStdout)(msg.line);
            else if (msg.type === 'exit') end(msg.code);
            else if (msg.type === 'fatal') {
                onStderr(`${name} crashed: ${msg.message}`);
                end(134);
            }
        },
    });
    await runtime.ready;
    return {
        exited,
        stdin: {
            write: (data) => runtime.writeStdin(data),
            end: () => runtime.closeStdin(),
        },
        kill: () => end(137),
    };
}
