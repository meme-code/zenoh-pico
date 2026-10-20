// Memory layouts shared between the page (zenoh-runtime.js), the workers (zenoh-worker.js)
// and the wasm side (wasm/ws.c, wasm/pthread.c, wasm/api.c). Offsets are in bytes.

// struct zw_ws (wasm/ws.c): one WebSocket and its two rings of [u32 length][message] records.
export const WS = {
    CTRL: 0,        // zw_ctrl_t*: { wake, interrupt }
    STATE: 4,
    TX_KICK: 8,
    RX_WRITE: 12,   // page
    RX_READ: 16,    // wasm
    TX_WRITE: 20,   // wasm
    TX_READ: 24,    // page
    RX_CAPACITY: 28,
    TX_CAPACITY: 32,
    RX: 36,
    TX: 40,
};

// zw_ctrl_t (wasm/zenoh_wasm_platform.h)
export const CTRL = {
    WAKE: 0,
    INTERRUPT: 4,
    TX_BELL: 8,
    DOORBELL: 12,
};

export const WS_STATE = {
    CONNECTING: 1,
    OPEN: 2,
    CLOSED: 3,
    RELEASED: 4,
};

// struct zw_thread (wasm/pthread.c): the worker sets EXITED once wasi_thread_start returns.
export const THREAD = {
    EXITED: 8,
};

// zw_msg_t (wasm/api.c): a sample, reply or query handed to a callback.
export const MSG = {
    KIND: 0,
    KEYEXPR: 4,
    KEYEXPR_LEN: 8,
    PAYLOAD: 12,
    PAYLOAD_LEN: 16,
    ENCODING: 20,
    ENCODING_LEN: 24,
    ATTACHMENT: 28,
    ATTACHMENT_LEN: 32,
    PARAMETERS: 36,
    PARAMETERS_LEN: 40,
    HAS_PAYLOAD: 44,
    PRIORITY: 48,
    CONGESTION_CONTROL: 52,
    EXPRESS: 56,
    HAS_TIMESTAMP: 60,
    TIMESTAMP: 64,
    TIMESTAMP_ID: 72,
};

// Int32 slots of the small control SharedArrayBuffer every worker of one runtime gets.
export const SHARED = {
    NEXT_TID: 0,
    STDIN_STATE: 1,  // bumped + notified whenever stdin data arrives or stdin is closed
    INTS: 4,
};
