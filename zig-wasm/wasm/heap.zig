//! The page allocator under wasm/heap.c's thread-safe malloc: std.heap.BrkAllocator, which
//! is also what Zig's own libc malloc uses on wasm32 (single-threaded). heap.c serializes
//! every call with a lock, so it can be shared by all threads.
//!
//! The libc symbols themselves (malloc, free, ...) are defined in C on purpose: Zig's libc
//! defines them weakly in Zig, and exporting the same names from this module would collide
//! with those inside one compilation unit instead of overriding them at link time.

const std = @import("std");
const Alignment = std.mem.Alignment;

const vtable = std.heap.BrkAllocator.vtable;
const no_context: *anyopaque = undefined;

export fn zw_brk_alloc(len: usize, log2_align: u8) ?[*]u8 {
    return vtable.alloc(no_context, len, @enumFromInt(log2_align), 0);
}

export fn zw_brk_free(ptr: [*]u8, len: usize, log2_align: u8) void {
    vtable.free(no_context, ptr[0..len], @enumFromInt(log2_align), 0);
}

/// Grows or shrinks in place if possible; null if the block would have to move.
export fn zw_brk_remap(ptr: [*]u8, len: usize, log2_align: u8, new_len: usize) ?[*]u8 {
    return vtable.remap(no_context, ptr[0..len], @enumFromInt(log2_align), new_len, 0);
}
