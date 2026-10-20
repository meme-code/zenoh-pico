#!/bin/sh
# Zig port of zenoh-pico vs. Rust zenoh in the browser, timed with hyperfine.
#
#   ZENOHD=/path/to/zenohd bench/run-rust.sh
#
# The Rust side is the community wasm port of Rust zenoh (github.com/0x53A/zenoh, branch `wasm`;
# not upstream), running bench.c's workload as bench/rust, in two builds: "rust" (the port's
# SharedArrayBuffer worker pool) and "rust-st" (single-threaded, on the page's event loop).
#
# Needs: zig, podman (or CONTAINER=docker), git, node, python3, Chrome, hyperfine, zenohd 1.10.x.
# Output: zig-out/bench/results-rust/REPORT.md plus hyperfine's JSON/Markdown exports.
# Tunables (env): RUNS=10 WARMUP=2 IDLE_SECONDS=10 IDLE_RUNS=3 CONTAINER RUST_IMAGE RUST_TOOLCHAIN
#   RUST_CACHE (CARGO_HOME/RUSTUP_HOME for the container) ZENOH_WASM_REPO ZENOH_WASM_REV
set -eu
cd "$(dirname "$0")/.."

RUNS=${RUNS:-10}
WARMUP=${WARMUP:-2}
IDLE_SECONDS=${IDLE_SECONDS:-10}
IDLE_RUNS=${IDLE_RUNS:-3}
CONTAINER=${CONTAINER:-podman}
RUST_IMAGE=${RUST_IMAGE:-public.ecr.aws/docker/library/rust:1-slim-bookworm}
RUST_TOOLCHAIN=${RUST_TOOLCHAIN:-nightly-2026-10-20}
RUST_CACHE=${RUST_CACHE:-$PWD/zig-out/bench/rust-cache}
ZENOH_WASM_REPO=${ZENOH_WASM_REPO:-https://github.com/0x53A/zenoh.git}
ZENOH_WASM_REV=${ZENOH_WASM_REV:-419088e38c1b96a9c4c45cae06785167128dd444}
ZENOHD=${ZENOHD:-zenohd}
export ZENOHD

OUT=zig-out/bench
WWW=$OUT/www-rust
RESULTS=$OUT/results-rust
STATE=$OUT/state-rust.json

# name|label|mode|n|size|ports
# rust-st is left out of the send scenarios: on the page thread the port's publisher cannot wait
# for the transmit queue, so a put loop drops most messages (put still returns Ok).
SCENARIOS='open|Open a session|open|0|0|zig rust rust-st
ping|200 round trips (64 B)|ping|200|64|zig rust rust-st
pub|Send 500 × 64 B, one put each|pub|500|64|zig rust
pubbatch|Send 10,000 × 64 B, batched|pubbatch|10000|64|zig rust
sub|Receive 5,000 × 64 B|sub|5000|64|zig rust rust-st'

echo "==> Building the Zig side (benchmark wasm, release site, native peers)"
zig build bench -Drelease

echo "==> Building the Rust side in $RUST_IMAGE ($RUST_TOOLCHAIN)"
if [ ! -d bench/rust/zenoh-wasm/.git ]; then
    git init -q bench/rust/zenoh-wasm
    git -C bench/rust/zenoh-wasm fetch -q --depth 1 "$ZENOH_WASM_REPO" "$ZENOH_WASM_REV"
    git -C bench/rust/zenoh-wasm checkout -q FETCH_HEAD
fi
if [ "$(git -C bench/rust/zenoh-wasm rev-parse HEAD)" != "$ZENOH_WASM_REV" ]; then
    echo "bench/rust/zenoh-wasm is not at $ZENOH_WASM_REV; remove it to re-fetch" >&2
    exit 1
fi
mkdir -p "$RUST_CACHE"
"$CONTAINER" run --rm -v "$PWD:$PWD" -v "$RUST_CACHE:$RUST_CACHE" -w "$PWD" \
    -e CARGO_HOME="$RUST_CACHE/cargo" -e RUSTUP_HOME="$RUST_CACHE/rustup" -e RUSTUP_TOOLCHAIN="$RUST_TOOLCHAIN" \
    -e PATH="$RUST_CACHE/cargo/bin:/usr/local/cargo/bin:/usr/local/bin:/usr/bin:/bin" \
    "$RUST_IMAGE" sh -c '
        rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal --component rust-src --target wasm32-unknown-unknown >/dev/null
        command -v wasm-bindgen >/dev/null || cargo install wasm-bindgen-cli --version 0.2.127 --locked
        sh bench/build-rust.sh'

rm -rf "$WWW" "$RESULTS" "$STATE"
cp -R "$OUT/site" "$WWW"
chmod -R u+w "$WWW"
cp -R "$OUT/rust" "$WWW/rust"
cp bench/bench-rust.html bench/bench-rust-st.html "$WWW/"
mkdir -p "$RESULTS"

echo "==> Starting zenohd, the web server, Chrome and z_pong"
node bench/services.mjs "$WWW" "$OUT/native" "$STATE" > "$RESULTS/services.log" 2>&1 &
SERVICES=$!
trap 'kill $SERVICES 2>/dev/null || true' EXIT INT TERM
i=0
while [ ! -f "$STATE" ]; do
    i=$((i + 1))
    if [ $i -gt 300 ] || ! kill -0 $SERVICES 2>/dev/null; then
        cat "$RESULTS/services.log"
        echo "services failed to start" >&2
        exit 1
    fi
    sleep 0.1
done
cat "$RESULTS/services.log"

echo "$SCENARIOS" | while IFS='|' read -r name label mode n size ports; do
    echo "==> hyperfine: $label"
    set --
    for port in $ports; do
        set -- "$@" -n "$port" "node bench/run-one.mjs $STATE $port $mode $n $size $RESULTS/inpage.jsonl"
    done
    # A Rust run can lose messages (see REPORT.md); keep going and let the report count failures.
    hyperfine --warmup "$WARMUP" --runs "$RUNS" --ignore-failure \
        --export-json "$RESULTS/$name.json" --export-markdown "$RESULTS/$name.md" "$@"
done

echo "==> Idle CPU"
node bench/idle-cpu.mjs "$STATE" "$RESULTS/idle.json" "$IDLE_SECONDS" "$IDLE_RUNS" zig,rust,rust-st

# Environment for the report.
node -e '
const [stateFile, envFile, runs, warmup, rust, zenohWasm, zenohd, zig, commit, machine, scenarios] = process.argv.slice(1);
const fs = require("fs");
const state = JSON.parse(fs.readFileSync(stateFile, "utf8"));
fs.writeFileSync(envFile, JSON.stringify({
    date: new Date().toISOString().slice(0, 10), machine, browser: state.browser,
    zenohd, zig, rust, zenohWasm, zenohPico: commit, runs: Number(runs), warmup: Number(warmup),
    scenarios: scenarios.trim().split("\n").map((l) => {
        const [name, label, mode, n, size, ports] = l.split("|");
        return { name, label, mode, n: Number(n), size: Number(size), ports: ports.split(" ") };
    }),
}, null, 2));
' "$STATE" "$RESULTS/env.json" "$RUNS" "$WARMUP" \
    "$(tr '\n' ',' < "$OUT/rust/versions.txt" | sed 's/,$//; s/,/, /')" \
    "$ZENOH_WASM_REPO@$(echo "$ZENOH_WASM_REV" | cut -c1-12)" \
    "$("$ZENOHD" --version 2>/dev/null | tail -1 | sed -E 's/^zenohd v?([^ ]+).*/\1/')" \
    "$(zig version)" \
    "$(git describe --tags --always 2>/dev/null || git rev-parse --short HEAD)" \
    "$( (sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m) | head -1), $(uname -s) $(uname -r)" \
    "$SCENARIOS"

node bench/report-rust.mjs "$RESULTS" "$OUT" > "$RESULTS/REPORT.md"
echo "==> Report: $RESULTS/REPORT.md"
