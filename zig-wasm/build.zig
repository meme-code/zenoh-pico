//! Builds zenoh-pico as a WebAssembly module for web browsers, using only Zig.
//!
//!   zig build          -> zig-out/web/{zenoh-pico.wasm, zenoh.js, zenoh-worker.js, index.html}
//!   zig build serve    -> serves zig-out/web with the COOP/COEP headers the runtime needs
//!   zig build test     -> end-to-end browser test against a local zenohd
//!   zig build bench    -> Zig side of the Zig-vs-Emscripten benchmark (see bench/run.sh)
//!
//! Run from this directory (zig-wasm/). zenoh-pico's own sources in the parent directory are
//! compiled unmodified; the browser platform layer (clock, random, sleep and a WebSocket link
//! backed by the browser's WebSocket API) lives in ./wasm and is plugged in through
//! zenoh-pico's ZP_SYSTEM_PLATFORM_HEADER hook.

const std = @import("std");

/// The zenoh-pico repository this directory lives in.
const zenoh_pico_root = "..";

/// Mirrors the source globs in ../CMakeLists.txt.
const recursive_src_dirs = [_][]const u8{
    "src/api",
    "src/collections",
    "src/net",
    "src/protocol",
    "src/runtime",
    "src/session",
    "src/transport",
    "src/utils",
    "src/system/common",
};
const flat_src_dirs = [_][]const u8{
    "src/link/config",
    "src/link/multicast",
    "src/link/unicast",
    "src/link/transport/common",
};
const extra_src_files = [_][]const u8{
    "src/link/link.c",
    "src/link/endpoint.c",
    "src/link/transport/tcp/address.c",
    "src/link/transport/udp/address.c",
    "src/link/transport/upper/serial_protocol.c",
    "src/link/transport/upper/tls_stream.c",
};

const platform_src_files = [_][]const u8{
    "wasm/system.c",
    "wasm/ws.c",
    "wasm/api.c",
};

/// What differs between the browser build and the native benchmark peers.
const Flavor = struct {
    platform_header: ?[]const u8,
    multi_thread: u1,
    link_tcp: u1,
    link_ws: u1,
    unicast_peer: u1,
    socket_timeout: u32 = 100,
    batch_size: u32 = 2048,
    frag_max_size: u32 = 4096,
};

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseSmall });
    const log_level = b.option(u8, "log-level", "zenoh-pico log level: 0=off, 1=error, 2=info, 3=debug (default 0)") orelse 0;
    const socket_timeout = b.option(u32, "socket-timeout", "Socket read timeout in ms (default 100)") orelse 100;
    const batch_size = b.option(u32, "batch-size", "Maximum unicast batch size in bytes (default 2048)") orelse 2048;
    const frag_max_size = b.option(u32, "frag-max-size", "Maximum size of a fragmented message in bytes (default 4096)") orelse 4096;

    const sources = collectZenohPicoSources(b);

    // Browsers give a wasm module one thread here; zenoh runs on zp_spin_once instead.
    // A browser can only open outgoing WebSockets: no TCP/UDP/serial, no scouting,
    // no listening, so client mode over ws/ is the only option.
    const browser_config = zenohConfigHeader(b, .{
        .platform_header = "zenoh_wasm_platform.h",
        .multi_thread = 0,
        .link_tcp = 0,
        .link_ws = 1,
        .unicast_peer = 0,
        .socket_timeout = socket_timeout,
        .batch_size = batch_size,
        .frag_max_size = frag_max_size,
    });

    const wasm = addBrowserWasm(b, "zenoh-pico", optimize, browser_config, sources, log_level, &.{});

    const site = b.addWriteFiles();
    _ = site.addCopyFile(wasm.getEmittedBin(), "zenoh-pico.wasm");
    _ = site.addCopyDirectory(b.path("web"), "", .{});

    b.installDirectory(.{
        .source_dir = site.getDirectory(),
        .install_dir = .prefix,
        .install_subdir = "web",
    });

    const port = b.option([]const u8, "port", "Port for `zig build serve` (default 8000)") orelse "8000";
    const serve = b.addSystemCommand(&.{"python3"});
    serve.addFileArg(b.path("tools/serve.py"));
    serve.addDirectoryArg(site.getDirectory());
    serve.addArg(port);
    serve.has_side_effects = true;
    b.step("serve", "Serve the demo page with cross-origin isolation headers").dependOn(&serve.step);

    // End-to-end: two browser sessions talking through a real zenohd, in headless Chrome.
    // Needs node, python3, Chrome and zenohd (set ZENOHD=/path/to/zenohd if not on PATH).
    const test_site = b.addWriteFiles();
    _ = test_site.addCopyDirectory(site.getDirectory(), "", .{});
    _ = test_site.addCopyFile(b.path("tests/browser-test.html"), "browser-test.html");
    const browser_test = b.addSystemCommand(&.{"node"});
    browser_test.addFileArg(b.path("tests/run-browser-test.mjs"));
    browser_test.addDirectoryArg(test_site.getDirectory());
    browser_test.addFileInput(b.path("tools/serve.py"));
    browser_test.addFileInput(b.path("tools/headless.mjs"));
    browser_test.has_side_effects = true;
    b.step("test", "Run the end-to-end browser test against a local zenohd").dependOn(&browser_test.step);

    // Benchmark, Zig side: the same bench/bench.c that the Emscripten build runs, linked into
    // this port's wasm, plus native zenoh-pico peers (z_pong and bench/peer.c) for the host.
    const bench_optimize = b.option(std.builtin.OptimizeMode, "bench-optimize", "Optimize mode of the benchmark wasm: debug, safe, fast, small (default fast)") orelse .fast;
    const bench_wasm = addBrowserWasm(b, "zenoh-pico-bench", bench_optimize, browser_config, sources, 0, &.{
        "bench/bench.c",
        "bench/bench_zig.c",
    });
    const bench_site = b.addWriteFiles();
    _ = bench_site.addCopyDirectory(site.getDirectory(), "", .{});
    _ = bench_site.addCopyFile(bench_wasm.getEmittedBin(), "zenoh-pico-bench.wasm");
    _ = bench_site.addCopyFile(b.path("bench/bench-zig.html"), "bench-zig.html");
    _ = bench_site.addCopyFile(b.path("bench/bench-emscripten.html"), "bench-emscripten.html");

    const native_config = zenohConfigHeader(b, .{
        .platform_header = null,
        .multi_thread = 1,
        .link_tcp = 1,
        .link_ws = 0,
        .unicast_peer = 1,
    });
    const bench_step = b.step("bench", "Build the Zig side of the Zig-vs-Emscripten benchmark into zig-out/bench");
    bench_step.dependOn(&b.addInstallDirectory(.{
        .source_dir = bench_site.getDirectory(),
        .install_dir = .prefix,
        .install_subdir = "bench/site",
    }).step);
    for ([_][2][]const u8{
        .{ "z_pong", "../examples/unix/c11/z_pong.c" },
        .{ "peer", "bench/peer.c" },
    }) |exe| {
        const native = addNativePeer(b, exe[0], native_config, sources, exe[1]);
        bench_step.dependOn(&b.addInstallArtifact(native, .{
            .dest_dir = .{ .override = .{ .custom = "bench/native" } },
        }).step);
    }
}

fn zenohConfigHeader(b: *std.Build, f: Flavor) *std.Build.Step.ConfigHeader {
    return b.addConfigHeader(.{
        .style = .{ .cmake = b.path(zenoh_pico_root ++ "/include/zenoh-pico/config.h.in") },
        .include_path = "zenoh-pico/config.h",
    }, .{
        .ZP_SYSTEM_PLATFORM_HEADER = f.platform_header,
        .FRAG_MAX_SIZE = f.frag_max_size,
        .BATCH_UNICAST_SIZE = f.batch_size,
        .BATCH_MULTICAST_SIZE = f.batch_size,
        .Z_CONFIG_SOCKET_TIMEOUT = f.socket_timeout,
        .Z_TRANSPORT_LEASE = 10000,
        .Z_TRANSPORT_LEASE_EXPIRE_FACTOR = 3,
        .Z_RUNTIME_MAX_TASKS = 64,
        .Z_RUNTIME_IDLE_READ_TASK_SLEEP = 0,
        .Z_TRANSPORT_ACCEPT_TIMEOUT = 1000,
        .Z_TRANSPORT_CONNECT_TIMEOUT = 10000,
        .Z_FEATURE_UNSTABLE_API = null,
        .Z_FEATURE_CONNECTIVITY = 0,
        .Z_FEATURE_MULTI_THREAD = f.multi_thread,
        .Z_FEATURE_PUBLICATION = 1,
        .Z_FEATURE_ADVANCED_PUBLICATION = 0,
        .Z_FEATURE_SUBSCRIPTION = 1,
        .Z_FEATURE_ADVANCED_SUBSCRIPTION = 0,
        .Z_FEATURE_QUERY = 1,
        .Z_FEATURE_QUERYABLE = 1,
        .Z_FEATURE_LIVELINESS = 1,
        .Z_FEATURE_RAWETH_TRANSPORT = 0,
        .Z_FEATURE_INTEREST = 1,
        .Z_FEATURE_LINK_TCP = f.link_tcp,
        .Z_FEATURE_LINK_BLUETOOTH = 0,
        .Z_FEATURE_LINK_WS = f.link_ws,
        .Z_FEATURE_LINK_SERIAL = 0,
        .Z_FEATURE_LINK_SERIAL_USB = 0,
        .Z_FEATURE_LINK_TLS = 0,
        .Z_FEATURE_SCOUTING = 0,
        .Z_FEATURE_LINK_UDP_MULTICAST = 0,
        .Z_FEATURE_LINK_UDP_UNICAST = 0,
        .Z_FEATURE_MULTICAST_TRANSPORT = 0,
        .Z_FEATURE_UNICAST_TRANSPORT = 1,
        .Z_FEATURE_FRAGMENTATION = 1,
        .Z_FEATURE_ENCODING_VALUES = 1,
        .Z_FEATURE_TCP_NODELAY = f.link_tcp,
        .Z_FEATURE_LOCAL_SUBSCRIBER = 0,
        .Z_FEATURE_LOCAL_QUERYABLE = 0,
        .Z_FEATURE_SESSION_CHECK = 1,
        .Z_FEATURE_BATCHING = 1,
        .Z_FEATURE_BATCH_TX_MUTEX = 0,
        .Z_FEATURE_BATCH_PEER_MUTEX = 0,
        .Z_FEATURE_MATCHING = 1,
        .Z_FEATURE_RX_CACHE = 0,
        .Z_FEATURE_UNICAST_PEER = f.unicast_peer,
        .Z_FEATURE_AUTO_RECONNECT = 1,
        .Z_FEATURE_MULTICAST_DECLARATIONS = 0,
        .Z_FEATURE_ADMIN_SPACE = 0,
    });
}

const c_flags = [_][]const u8{
    // CMake default for C_STANDARD 11 (extensions on); zenoh-pico uses POSIX strnlen.
    "-std=gnu11",
    "-Wall",
    "-Wno-unused-function",
    // Upstream bug on 32-bit targets: _z_slist stores values right after a 4-byte `next`
    // pointer (zenoh-pico/src/collections/list.c, NODE_DATA_SIZE), so 8-byte-aligned
    // structs end up misaligned. wasm allows unaligned access, so this is harmless here;
    // keep the rest of UBSan in Debug builds. eclipse-zenoh/zenoh-pico#1331
    "-fno-sanitize=alignment",
};

fn addZenohPicoModule(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    config_h: *std.Build.Step.ConfigHeader,
    sources: []const []const u8,
) *std.Build.Module {
    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        // Release wasm ships without debug info (ReleaseFast would otherwise keep DWARF).
        .strip = if (optimize == .debug) null else true,
    });
    mod.addConfigHeader(config_h);
    mod.addIncludePath(b.path(zenoh_pico_root ++ "/include"));
    mod.addCMacro("ZENOH_C_STANDARD", "11");
    mod.addCMacro("ZENOH_COMPILER_CLANG", "1");
    mod.addCSourceFiles(.{
        .root = b.path(zenoh_pico_root),
        .files = sources,
        .flags = &c_flags,
    });
    return mod;
}

fn addBrowserWasm(
    b: *std.Build,
    name: []const u8,
    optimize: std.builtin.OptimizeMode,
    config_h: *std.Build.Step.ConfigHeader,
    sources: []const []const u8,
    log_level: u8,
    extra_files: []const []const u8,
) *std.Build.Step.Compile {
    // Plain wasm32-wasi so Zig can supply libc (wasi-libc); the handful of WASI imports it
    // needs are shimmed in web/zenoh-worker.js. Everything else comes from "zenoh_host" imports.
    const target = b.resolveTargetQuery(.{ .cpu_arch = .wasm32, .os_tag = .wasi });
    const mod = addZenohPicoModule(b, target, optimize, config_h, sources);
    mod.addIncludePath(b.path("wasm"));
    if (log_level > 0) {
        mod.addCMacro("ZENOH_DEBUG", b.fmt("{d}", .{log_level}));
    }
    mod.addCSourceFiles(.{ .files = &platform_src_files, .flags = &c_flags });
    mod.addCSourceFiles(.{ .files = extra_files, .flags = &c_flags });

    const wasm = b.addExecutable(.{
        .name = name,
        .root_module = mod,
    });
    // Reactor: no main(); the JS side calls _initialize() once, then the zw_* exports.
    wasm.wasi_exec_model = .reactor;
    wasm.entry = .disabled;
    return wasm;
}

/// A native (host) zenoh-pico program over TCP, used as the benchmark's counterpart.
fn addNativePeer(
    b: *std.Build,
    name: []const u8,
    config_h: *std.Build.Step.ConfigHeader,
    sources: []const []const u8,
    main_file: []const u8,
) *std.Build.Step.Compile {
    const target = b.graph.host;
    const mod = addZenohPicoModule(b, target, .fast, config_h, sources);
    mod.addCMacro(switch (target.result.os.tag) {
        .macos => "ZENOH_MACOS",
        .linux => "ZENOH_LINUX",
        else => "ZENOH_UNSUPPORTED_BENCH_HOST",
    }, "1");
    mod.addCSourceFiles(.{
        .root = b.path(zenoh_pico_root),
        .files = &.{
            "src/system/unix/system.c",
            "src/system/unix/network.c",
            "src/link/transport/tcp/tcp_posix.c",
        },
        .flags = &c_flags,
    });
    mod.addCSourceFiles(.{ .files = &.{main_file}, .flags = &c_flags });
    return b.addExecutable(.{ .name = name, .root_module = mod });
}

/// Expands the CMake source globs at configure time. Each scanned directory is registered
/// with dependOnDirectoryContents so adding/removing a file re-runs configuration.
fn collectZenohPicoSources(b: *std.Build) []const []const u8 {
    var files: std.ArrayList([]const u8) = .empty;
    for (recursive_src_dirs) |dir| appendCFiles(b, &files, dir, true);
    for (flat_src_dirs) |dir| appendCFiles(b, &files, dir, false);
    for (extra_src_files) |file| files.append(b.allocator, file) catch @panic("OOM");
    std.mem.sort([]const u8, files.items, {}, struct {
        fn lessThan(_: void, lhs: []const u8, rhs: []const u8) bool {
            return std.mem.lessThan(u8, lhs, rhs);
        }
    }.lessThan);
    return files.items;
}

fn appendCFiles(b: *std.Build, files: *std.ArrayList([]const u8), sub_dir: []const u8, recursive: bool) void {
    const io = b.graph.io;
    const dir_path = b.pathJoin(&.{ zenoh_pico_root, sub_dir });
    b.dependOnDirectoryContents(b.path(dir_path));

    var dir = b.root.openDir(io, dir_path, .{ .iterate = true }) catch |err|
        std.debug.panic("unable to open {s}: {t} (build.zig expects to live in a subdirectory of zenoh-pico)", .{ dir_path, err });
    defer dir.close(io);

    var it = dir.iterate();
    while (it.next(io) catch |err| std.debug.panic("unable to list {s}: {t}", .{ dir_path, err })) |entry| {
        switch (entry.kind) {
            .file => if (std.mem.endsWith(u8, entry.name, ".c")) {
                files.append(b.allocator, b.pathJoin(&.{ sub_dir, entry.name })) catch @panic("OOM");
            },
            .directory => if (recursive) {
                appendCFiles(b, files, b.pathJoin(&.{ sub_dir, entry.name }), true);
            },
            else => {},
        }
    }
}
