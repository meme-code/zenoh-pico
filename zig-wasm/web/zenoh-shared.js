// Layout of the shared memory between zenoh.js (main thread) and zenoh-worker.js.

// Int32 slots of the per-session control SharedArrayBuffer.
export const CTRL = {
    WAKE: 0,      // bumped + notified by the main thread on every event the worker may wait for
    INTERRUPT: 1, // set by the main thread after posting a call, to cut short an idle read
    SLEEP: 2,     // never notified; z_sleep_ms waits on it
    INTS: 4,
};

// Per-socket receive ring: Int32 header followed by CAPACITY bytes of [u32 len][message] records.
export const RING = {
    WRITE: 0,
    READ: 1,
    STATE: 2,
    HEADER_BYTES: 16,
    CAPACITY: 1 << 20, // must be a power of two
};

export const SOCK_STATE = {
    CONNECTING: 1,
    OPEN: 2,
    CLOSED: 3,
};
