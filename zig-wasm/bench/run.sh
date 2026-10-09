#!/bin/sh
# Zig port vs. Emscripten port of zenoh-pico in the browser, timed with hyperfine.
#
#   ZENOHD=/path/to/zenohd bench/run.sh
#
# Needs: zig, podman (or CONTAINER=docker), node, python3, Chrome, hyperfine, zenohd 1.10.x.
# Output: zig-out/bench/results/REPORT.md plus hyperfine's JSON/Markdown exports.
# Tunables (env): RUNS=10 WARMUP=2 IDLE_SECONDS=10 IDLE_RUNS=3 EMSDK_IMAGE CONTAINER
set -eu
cd "$(dirname "$0")/.."   # zig-wasm/, inside the zenoh-pico repository

RUNS=${RUNS:-10}
WARMUP=${WARMUP:-2}
IDLE_SECONDS=${IDLE_SECONDS:-10}
IDLE_RUNS=${IDLE_RUNS:-3}
EMSDK_IMAGE=${EMSDK_IMAGE:-docker.io/emscripten/emsdk:6.0.12}
CONTAINER=${CONTAINER:-podman}
ZENOHD=${ZENOHD:-zenohd}
export ZENOHD

OUT=zig-out/bench
RESULTS=$OUT/results
STATE=$OUT/state.json

# name|label|mode|n|size
SCENARIOS='open|Open a session|open|0|0
ping|200 round trips (64 B)|ping|200|64
pub|Send 500 × 64 B, one put each|pub|500|64
pubbatch|Send 10,000 × 64 B, batched|pubbatch|10000|64
sub|Receive 5,000 × 64 B|sub|5000|64'

echo "==> Building the Zig side (benchmark wasm, release site, native peers)"
zig build bench -Drelease

echo "==> Building the Emscripten side in $EMSDK_IMAGE"
"$CONTAINER" run --rm -v "$(cd .. && pwd):/repo" -w /repo/zig-wasm "$EMSDK_IMAGE" sh bench/build-emscripten.sh

rm -rf "$OUT/www" "$RESULTS" "$STATE"
cp -R "$OUT/site" "$OUT/www"
chmod -R u+w "$OUT/www"
cp -R "$OUT/emscripten" "$OUT/www/emscripten"
mkdir -p "$RESULTS"

echo "==> Starting zenohd, the web server, Chrome and z_pong"
node bench/services.mjs "$OUT/www" "$OUT/native" "$STATE" > "$RESULTS/services.log" 2>&1 &
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

echo "$SCENARIOS" | while IFS='|' read -r name label mode n size; do
    echo "==> hyperfine: $label"
    hyperfine --warmup "$WARMUP" --runs "$RUNS" \
        --export-json "$RESULTS/$name.json" --export-markdown "$RESULTS/$name.md" \
        -n zig "node bench/run-one.mjs $STATE zig $mode $n $size $RESULTS/inpage.jsonl" \
        -n emscripten "node bench/run-one.mjs $STATE emscripten $mode $n $size $RESULTS/inpage.jsonl"
done

echo "==> Idle CPU"
node bench/idle-cpu.mjs "$STATE" "$RESULTS/idle.json" "$IDLE_SECONDS" "$IDLE_RUNS"

# Environment for the report.
node -e '
const [stateFile, envFile, runs, warmup, emcc, zenohd, zig, commit, machine, scenarios] = process.argv.slice(1);
const fs = require("fs");
const state = JSON.parse(fs.readFileSync(stateFile, "utf8"));
fs.writeFileSync(envFile, JSON.stringify({
    date: new Date().toISOString().slice(0, 10), machine, browser: state.browser,
    zenohd, zig, emcc, zenohPico: commit, runs: Number(runs), warmup: Number(warmup),
    scenarios: scenarios.trim().split("\n").map((l) => {
        const [name, label, mode, n, size] = l.split("|");
        return { name, label, mode, n: Number(n), size: Number(size) };
    }),
}, null, 2));
' "$STATE" "$RESULTS/env.json" "$RUNS" "$WARMUP" \
    "$(sed -E 's/^emcc \(.*\) ([0-9.]+).*/Emscripten \1/' "$OUT/emscripten/emcc-version.txt")" \
    "$("$ZENOHD" --version 2>/dev/null | tail -1 | sed -E 's/^zenohd v?([^ ]+).*/\1/')" \
    "$(zig version)" \
    "$(git -C .. describe --tags --always 2>/dev/null || git -C .. rev-parse --short HEAD)" \
    "$( (sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m) | head -1), $(uname -s) $(uname -r)" \
    "$SCENARIOS"

node bench/report.mjs "$RESULTS" "$OUT" > "$RESULTS/REPORT.md"
echo "==> Report: $RESULTS/REPORT.md"
