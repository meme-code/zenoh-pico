//! Builds zenoh-pico as a WebAssembly module for web browsers, using only Zig.
//!
//!   zig build          -> zig-out/web: zenoh-pico.wasm (single-threaded), zenoh-pico-mt.wasm
//!                         (threads), the JS runtime, the demo page, and every upstream C example
//!                         in examples/ (run them with examples.html)
//!   zig build serve    -> serves zig-out/web with the COOP/COEP headers the runtime needs
//!   zig build test     -> end-to-end browser tests against a local zenohd
//!   zig build bench    -> Zig side of the Zig-vs-Emscripten benchmark (see bench/run.sh)
//!   zig build app -Dapp=main.c [-Dapp-name=name]
//!                      -> your own zenoh-pico C program as zig-out/web/apps/<name>.wasm
//!
//! Run from this directory (zig-wasm/). zenoh-pico's own sources in the parent directory are
//! compiled unmodified; the browser platform layer (clock, random, sleep, threads, and a
//! WebSocket link backed by the browser's WebSocket API) lives in ./wasm and is plugged in
//! through zenoh-pico's ZP_SYSTEM_PLATFORM_HEADER hook.

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
};
/// Real threads (wasi-threads on Web Workers) for multi-threaded builds.
const thread_src_files = [_][]const u8{
    "wasm/pthread.c",
    "wasm/heap.c",
    "wasm/stdio.c",
};

/// Upstream examples that cannot do anything useful in a browser: TLS links and UDP scouting
/// are not available there.
const skipped_examples = [_][]const u8{ "z_pub_tls.c", "z_sub_tls.c", "z_scout.c" };

/// What differs between the browser builds and the native benchmark peers.
const Flavor = struct {
    platform_header: ?[]const u8,
    multi_thread: u1,
    link_tcp: u1,
    link_ws: u1,
    unicast_peer: u1,
    /// Unstable API plus what depends on it: advanced pub/sub, admin space, connectivity.
    unstable_api: bool = false,
    /// Deliver a session's own puts and queries to its own subscribers and queryables, as
    /// Rust zenoh does (zenoh-pico leaves this off by default).
    local_delivery: u1 = 0,
    socket_timeout: u32 = 100,
    batch_size: u32 = 2048,
    frag_max_size: u32 = 4096,
};

/// Linear memory limit of the browser builds. Shared memory reserves its maximum up front.
const max_memory = 512 << 20;

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseSmall });
    const log_level = b.option(u8, "log-level", "zenoh-pico log level: 0=off, 1=error, 2=info, 3=debug (default 0)") orelse 0;
    const socket_timeout = b.option(u32, "socket-timeout", "Socket read timeout in ms (default 100)") orelse 100;
    const batch_size = b.option(u32, "batch-size", "Maximum unicast batch size in bytes (default 2048)") orelse 2048;
    const frag_max_size = b.option(u32, "frag-max-size", "Maximum size of a fragmented message in bytes (default 4096)") orelse 4096;

    const sources = collectZenohPicoSources(b);

    // A browser can only open outgoing WebSockets: no TCP/UDP/serial, no scouting, no
    // listening. Client mode over ws/ is the norm; peer mode can connect to several peers.
    const browser = Flavor{
        .platform_header = "zenoh_wasm_platform.h",
        .multi_thread = 0,
        .link_tcp = 0,
        .link_ws = 1,
        .unicast_peer = 0,
        .local_delivery = 1,
        .socket_timeout = socket_timeout,
        .batch_size = batch_size,
        .frag_max_size = frag_max_size,
    };
    // Single-threaded: zenoh runs on zp_spin_once, pumped by the worker between calls.
    const browser_config = browserConfig(b, browser);
    // Multi-threaded, like upstream's default configuration: zenoh-pico's own tasks run on
    // their own Web Workers, and peer mode is available.
    var browser_mt = browser;
    browser_mt.multi_thread = 1;
    browser_mt.unicast_peer = 1;
    const browser_mt_config = browserConfig(b, browser_mt);
    // Everything that can work in a browser, for C programs (the upstream examples use the
    // unstable API too: queriers, advanced pub/sub, admin space).
    var browser_full = browser_mt;
    browser_full.unstable_api = true;
    const browser_full_config = browserConfig(b, browser_full);
    // The same without threads, for programs written around zp_spin_once (examples/*_st.c).
    var browser_full_st = browser_full;
    browser_full_st.multi_thread = 0;
    const browser_full_st_config = browserConfig(b, browser_full_st);

    const wasm = addBrowserWasm(b, "zenoh-pico", optimize, browser_config, sources, log_level, .api, &.{});
    const wasm_mt = addBrowserWasm(b, "zenoh-pico-mt", optimize, browser_mt_config, sources, log_level, .api, &.{});

    const site = b.addWriteFiles();
    _ = site.addCopyFile(wasm.getEmittedBin(), "zenoh-pico.wasm");
    _ = site.addCopyFile(wasm_mt.getEmittedBin(), "zenoh-pico-mt.wasm");
    _ = site.addCopyDirectory(b.path("web"), "", .{});

    // The upstream C examples, unmodified, as browser programs (web/examples.html runs them).
    const full_lib = addZenohPicoLibrary(b, optimize, browser_full_config, sources, log_level);
    const full_st_lib = addZenohPicoLibrary(b, optimize, browser_full_st_config, sources, log_level);
    var examples_dir = b.root.openDir(b.graph.io, zenoh_pico_root ++ "/examples/unix/c11", .{ .iterate = true }) catch |err|
        std.debug.panic("unable to open the upstream examples: {t}", .{err});
    defer examples_dir.close(b.graph.io);
    b.dependOnDirectoryContents(b.path(zenoh_pico_root ++ "/examples/unix/c11"));
    var example_names: std.ArrayList([]const u8) = .empty;
    var it = examples_dir.iterate();
    while (it.next(b.graph.io) catch @panic("unable to list the upstream examples")) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".c")) continue;
        if (for (skipped_examples) |skip| (if (std.mem.eql(u8, skip, entry.name)) break true) else false) continue;
        example_names.append(b.allocator, b.dupe(entry.name[0 .. entry.name.len - 2])) catch @panic("OOM");
    }
    std.mem.sort([]const u8, example_names.items, {}, struct {
        fn lessThan(_: void, lhs: []const u8, rhs: []const u8) bool {
            return std.mem.lessThan(u8, lhs, rhs);
        }
    }.lessThan);
    var example_list: std.ArrayList(u8) = .empty;
    for (example_names.items, 0..) |name, i| {
        const single_threaded = std.mem.endsWith(u8, name, "_st");
        const exe = addBrowserProgram(
            b,
            name,
            optimize,
            if (single_threaded) browser_full_st_config else browser_full_config,
            if (single_threaded) full_st_lib else full_lib,
            b.path(b.fmt("{s}/examples/unix/c11/{s}.c", .{ zenoh_pico_root, name })),
            &.{ "-include", "wasm/zenoh_wasm_examples.h" },
        );
        _ = site.addCopyFile(exe.getEmittedBin(), b.fmt("examples/{s}.wasm", .{name}));
        example_list.appendSlice(b.allocator, b.fmt("{s}\"{s}\"", .{ if (i == 0) "" else ",", name })) catch @panic("OOM");
    }
    _ = site.add("examples/index.json", b.fmt("[{s}]\n", .{example_list.items}));

    b.installDirectory(.{
        .source_dir = site.getDirectory(),
        .install_dir = .prefix,
        .install_subdir = "web",
    });

    // Your own program: zig build app -Dapp=path/to/main.c
    if (b.option([]const u8, "app", "A zenoh-pico C program (with main) to build for the browser")) |app| {
        const app_name = b.option([]const u8, "app-name", "Name of the app's wasm (default: the file's base name)") orelse
            std.fs.path.stem(app);
        // Relative to where zig build runs, like any command-line path.
        const exe = addBrowserProgram(b, app_name, optimize, browser_full_config, full_lib, b.graph.cwdRelativePath(app), &.{});
        const install = b.addInstallFileWithDir(exe.getEmittedBin(), .{ .custom = "web/apps" }, b.fmt("{s}.wasm", .{app_name}));
        b.step("app", "Build -Dapp=<file.c> into zig-out/web/apps/<name>.wasm").dependOn(&install.step);
    }

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
    _ = test_site.addCopyFile(b.path("tests/api-test.html"), "api-test.html");
    _ = test_site.addCopyFile(b.path("tests/programs-test.html"), "programs-test.html");
    const threads_test = addBrowserProgram(b, "threads-test", optimize, browser_full_config, full_lib, b.path("tests/threads-test.c"), &.{});
    _ = test_site.addCopyFile(threads_test.getEmittedBin(), "apps/threads-test.wasm");
    const burst_test = addBrowserProgram(b, "burst-close-st", optimize, browser_full_st_config, full_st_lib, b.path("tests/burst-close-st.c"), &.{});
    _ = test_site.addCopyFile(burst_test.getEmittedBin(), "apps/burst-close-st.wasm");
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
    const bench_wasm = addBrowserWasm(b, "zenoh-pico-bench", bench_optimize, browser_config, sources, 0, .api, &.{
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
        .Z_FEATURE_UNSTABLE_API = if (f.unstable_api) true else null,
        .Z_FEATURE_CONNECTIVITY = @intFromBool(f.unstable_api),
        .Z_FEATURE_MULTI_THREAD = f.multi_thread,
        .Z_FEATURE_PUBLICATION = 1,
        .Z_FEATURE_ADVANCED_PUBLICATION = @intFromBool(f.unstable_api),
        .Z_FEATURE_SUBSCRIPTION = 1,
        .Z_FEATURE_ADVANCED_SUBSCRIPTION = @intFromBool(f.unstable_api),
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
        .Z_FEATURE_LOCAL_SUBSCRIBER = f.local_delivery,
        .Z_FEATURE_LOCAL_QUERYABLE = f.local_delivery,
        .Z_FEATURE_SESSION_CHECK = 1,
        .Z_FEATURE_BATCHING = 1,
        .Z_FEATURE_BATCH_TX_MUTEX = 0,
        .Z_FEATURE_BATCH_PEER_MUTEX = 0,
        .Z_FEATURE_MATCHING = 1,
        .Z_FEATURE_RX_CACHE = 0,
        .Z_FEATURE_UNICAST_PEER = f.unicast_peer,
        .Z_FEATURE_AUTO_RECONNECT = 1,
        .Z_FEATURE_MULTICAST_DECLARATIONS = 0,
        .Z_FEATURE_ADMIN_SPACE = @intFromBool(f.unstable_api),
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

fn addZenohPicoIncludes(b: *std.Build, mod: *std.Build.Module, config_h: *std.Build.Step.ConfigHeader) void {
    mod.addConfigHeader(config_h);
    mod.addIncludePath(b.path(zenoh_pico_root ++ "/include"));
    mod.addCMacro("ZENOH_C_STANDARD", "11");
    mod.addCMacro("ZENOH_COMPILER_CLANG", "1");
}

fn addZenohPicoSources(b: *std.Build, mod: *std.Build.Module, sources: []const []const u8) void {
    mod.addCSourceFiles(.{
        .root = b.path(zenoh_pico_root),
        .files = sources,
        .flags = &c_flags,
    });
}

const WasmKind = enum {
    /// Reactor exporting wasm/api.c for web/zenoh.js's Session.
    api,
    /// Command: a C program with main(), run by web/zenoh.js's runProgram.
    program,
};

fn browserTarget(b: *std.Build) std.Build.ResolvedTarget {
    // Plain wasm32-wasi so Zig can supply libc (wasi-libc); the handful of WASI imports it
    // needs are shimmed in web/zenoh-worker.js. Atomics and bulk memory are needed for the
    // shared memory that the page, the worker and any thread workers use together.
    return b.resolveTargetQuery(.{
        .cpu_arch = .wasm32,
        .os_tag = .wasi,
        .cpu_features_add = std.Target.wasm.featureSet(&.{ .atomics, .bulk_memory }),
    });
}

/// A browser configuration of zenoh-pico.
const BrowserConfig = struct {
    header: *std.Build.Step.ConfigHeader,
    threads: bool,
};

fn browserConfig(b: *std.Build, f: Flavor) BrowserConfig {
    return .{ .header = zenohConfigHeader(b, f), .threads = f.multi_thread == 1 };
}

fn browserModule(
    b: *std.Build,
    optimize: std.builtin.OptimizeMode,
    config: BrowserConfig,
    log_level: u8,
) *std.Build.Module {
    const threads = config.threads;
    const mod = b.createModule(.{
        // Multi-threaded builds replace libc's single-threaded malloc (wasm/heap.c, on top of this).
        .root_source_file = if (threads) b.path("wasm/heap.zig") else null,
        .target = browserTarget(b),
        .optimize = optimize,
        .link_libc = true,
        .single_threaded = true, // as far as Zig code is concerned; C threads come from pthread.c
        // Release wasm ships without debug info (ReleaseFast would otherwise keep DWARF).
        .strip = if (optimize == .debug) null else true,
    });
    addZenohPicoIncludes(b, mod, config.header);
    mod.addIncludePath(b.path("wasm"));
    if (log_level > 0) {
        mod.addCMacro("ZENOH_DEBUG", b.fmt("{d}", .{log_level}));
    }
    if (threads) {
        // What -pthread would define: wasi-libc then declares the whole pthread API.
        mod.addCMacro("_REENTRANT", "1");
        // First on the link line: libc.a also has strong definitions of a few pthread
        // functions, which the linker would pull in for any object that refers to them
        // before pthread.o has defined them.
        mod.addCSourceFiles(.{ .files = &thread_src_files, .flags = &c_flags });
        mod.addAssemblyFile(b.path("wasm/thread_start.s"));
    }
    mod.addCSourceFiles(.{ .files = &platform_src_files, .flags = &c_flags });
    return mod;
}

fn configureBrowserWasm(exe: *std.Build.Step.Compile, kind: WasmKind) void {
    // The page creates the shared memory and hands it to every worker.
    exe.import_memory = true;
    exe.shared_memory = true;
    exe.max_memory = max_memory;
    switch (kind) {
        // Reactor: no main(); the JS side calls _initialize() once, then the zw_* exports.
        .api => {
            exe.wasi_exec_model = .reactor;
            exe.entry = .disabled;
        },
        .program => exe.wasi_exec_model = .command,
    }
}

fn addBrowserWasm(
    b: *std.Build,
    name: []const u8,
    optimize: std.builtin.OptimizeMode,
    config: BrowserConfig,
    sources: []const []const u8,
    log_level: u8,
    kind: WasmKind,
    extra_files: []const []const u8,
) *std.Build.Step.Compile {
    const mod = browserModule(b, optimize, config, log_level);
    addZenohPicoSources(b, mod, sources);
    mod.addCSourceFiles(.{ .files = &.{"wasm/api.c"}, .flags = &c_flags });
    mod.addCSourceFiles(.{ .files = extra_files, .flags = &c_flags });
    const wasm = b.addExecutable(.{ .name = name, .root_module = mod });
    configureBrowserWasm(wasm, kind);
    return wasm;
}

/// zenoh-pico itself as a static library, shared by all the C programs of one configuration.
fn addZenohPicoLibrary(
    b: *std.Build,
    optimize: std.builtin.OptimizeMode,
    config: BrowserConfig,
    sources: []const []const u8,
    log_level: u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{
        .target = browserTarget(b),
        .optimize = optimize,
        .link_libc = true,
        .strip = if (optimize == .debug) null else true,
    });
    addZenohPicoIncludes(b, mod, config.header);
    mod.addIncludePath(b.path("wasm"));
    if (log_level > 0) {
        mod.addCMacro("ZENOH_DEBUG", b.fmt("{d}", .{log_level}));
    }
    if (config.threads) {
        mod.addCMacro("_REENTRANT", "1");
    }
    addZenohPicoSources(b, mod, sources);
    return b.addLibrary(.{ .name = "zenoh-pico-browser", .linkage = .static, .root_module = mod });
}

/// A C program with main() for the browser. The platform layer is linked as objects, not from
/// the library: its strong malloc/pthread symbols must win over libc's weak ones, and
/// wasi_thread_start is only referenced by the host.
fn addBrowserProgram(
    b: *std.Build,
    name: []const u8,
    optimize: std.builtin.OptimizeMode,
    config: BrowserConfig,
    lib: *std.Build.Step.Compile,
    main_file: std.Build.LazyPath,
    extra_flags: []const []const u8,
) *std.Build.Step.Compile {
    const mod = browserModule(b, optimize, config, 0);
    // Programs written for POSIX may install signal handlers or call clock(); wasi-libc
    // emulates both on request. No signal is ever delivered in a browser.
    mod.addCMacro("_WASI_EMULATED_SIGNAL", "1");
    mod.addCMacro("_WASI_EMULATED_PROCESS_CLOCKS", "1");
    mod.linkSystemLibrary("wasi-emulated-signal", .{});
    mod.linkSystemLibrary("wasi-emulated-process-clocks", .{});
    mod.addCSourceFile(.{ .file = main_file, .flags = std.mem.concat(b.allocator, []const u8, &.{ &c_flags, extra_flags }) catch @panic("OOM") });
    mod.linkLibrary(lib);
    const exe = b.addExecutable(.{ .name = name, .root_module = mod });
    configureBrowserWasm(exe, .program);
    return exe;
}

/// A native (host) zenoh-pico program over TCP, used as the benchmark's counterpart.
fn addNativePeer(
    b: *std.Build,
    name: []const u8,
    config_h: *std.Build.Step.ConfigHeader,
    sources: []const []const u8,
    main_file: []const u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .fast,
        .link_libc = true,
        .strip = true,
    });
    addZenohPicoIncludes(b, mod, config_h);
    addZenohPicoSources(b, mod, sources);
    const target = b.graph.host;
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
