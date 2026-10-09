#!/bin/sh
# Builds the Emscripten side of the benchmark: zenoh-pico's in-tree Emscripten port with
# upstream CMake, linked with the same bench/bench.c as the Zig build. Runs inside the
# official emsdk image with the zenoh-pico repository mounted (see bench/run.sh):
#   podman run --rm -v "$PWD/..:/repo" -w /repo/zig-wasm docker.io/emscripten/emsdk:6.0.12 sh bench/build-emscripten.sh
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)   # zig-wasm/
REPO=$(cd "$HERE/.." && pwd)             # zenoh-pico
OUT=$HERE/zig-out/bench/emscripten

# CMake writes include/zenoh-pico.h into the source tree, so build from a copy of upstream's files.
rm -rf /tmp/zp /tmp/build && mkdir -p /tmp/zp
tar -C "$REPO" --exclude=./zig-wasm --exclude=./.git -cf - . | tar -C /tmp/zp -xf -

# Upstream's emscripten.yaml options, Release, single-threaded and client-only like the Zig build.
emcmake cmake -S /tmp/zp -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_STANDARD=11 -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_INTEGRATION=OFF -DBUILD_TOOLS=OFF \
  -DZ_FEATURE_LINK_WS=1 -DZ_FEATURE_LINK_TCP=0 -DZ_FEATURE_LINK_UDP_MULTICAST=0 -DZ_FEATURE_LINK_UDP_UNICAST=0 \
  -DZ_FEATURE_SCOUTING=0 -DZ_FEATURE_MULTI_THREAD=0 -DZ_FEATURE_UNICAST_PEER=0 \
  -DZ_FEATURE_MULTICAST_TRANSPORT=0 \
  -DCMAKE_C_FLAGS="-include stddef.h" > /tmp/cmake.log
# ^ Upstream bug: with Z_FEATURE_MULTI_THREAD=0, src/system/common/platform.c fails to compile on
#   Emscripten (unknown type 'size_t' in platform.h); with threads on, <pthread.h> provides it.
make -C /tmp/build -j"$(nproc)" > /tmp/make.log

# The port blocks with emscripten_sleep, which needs Asyncify. Emscripten's socket emulation
# asks for the "binary" WebSocket subprotocol by default; zenohd does not answer one, so Chrome
# fails the handshake ("Sent non-empty 'Sec-WebSocket-Protocol' header but no response was
# received") and z_open hangs. WEBSOCKET_SUBPROTOCOL=null is required to reach a stock zenohd.
link() {
  emcc "$@" "$HERE/bench/bench.c" "$HERE/bench/bench_emscripten.c" \
    -I/tmp/zp/include -I/tmp/build/include -DZENOH_EMSCRIPTEN -DZENOH_C_STANDARD=11 \
    /tmp/build/lib/libzenohpico.a \
    -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 -sALLOW_MEMORY_GROWTH -sENVIRONMENT=web \
    -sWEBSOCKET_SUBPROTOCOL=null
}
rm -rf "$OUT" && mkdir -p "$OUT/size"
link -O3 -o "$OUT/bench-emscripten.js"
link -Oz -o "$OUT/size/bench-emscripten.js"
emcc --version | head -1 > "$OUT/emcc-version.txt"
echo "built $(ls "$OUT")"
