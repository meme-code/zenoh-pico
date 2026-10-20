#!/bin/sh
# Builds the Rust side of the Zig-vs-Rust benchmark: bench/rust (the bench.c workload in Rust)
# against the community Rust zenoh wasm port, which bench/run-rust.sh clones into
# bench/rust/zenoh-wasm. Runs in a Rust image with nightly + rust-src and wasm-bindgen-cli
# 0.2.127 (the version in bench/rust/Cargo.lock); see bench/run-rust.sh.
#
# Two builds, each run through wasm-bindgen --target no-modules:
#   threads  the port's SharedArrayBuffer worker pool (atomics, shared memory, -Zbuild-std)
#   single   no worker pool; zenoh runs on the page's event loop
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)   # zig-wasm/
OUT=$HERE/zig-out/bench/rust
TARGET=$HERE/zig-out/bench/rust-target
cd "$HERE/bench/rust"

build() {
  variant=$1
  shift
  cargo --config "$variant.toml" build --locked --release --target wasm32-unknown-unknown \
    --target-dir "$TARGET/$variant" "$@"
  rm -rf "$OUT/$variant"
  wasm-bindgen --target no-modules --out-dir "$OUT/$variant" --no-typescript \
    "$TARGET/$variant/wasm32-unknown-unknown/release/zenoh_rust_bench.wasm"
}
build threads --features threads
build single

{ rustc -V; wasm-bindgen --version; } > "$OUT/versions.txt"
echo "built $(ls "$OUT")"
