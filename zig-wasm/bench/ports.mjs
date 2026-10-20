// The browser builds the benchmark can drive, and the page that runs bench.c's workload on each.
export const benchPages = {
    zig: 'bench-zig.html',               // this port
    emscripten: 'bench-emscripten.html', // zenoh-pico's in-tree Emscripten port
    rust: 'bench-rust.html',             // Rust zenoh, community wasm port, worker pool
    'rust-st': 'bench-rust-st.html',     // the same, single-threaded build
};
