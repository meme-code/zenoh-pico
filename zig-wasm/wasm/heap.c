//
// malloc and friends for multi-threaded browser builds.
//
// Zig's libc for wasm32-wasi only has a single-threaded allocator, whose symbols are weak.
// These strong definitions replace them with the same allocator (wasm/heap.zig) behind a
// lock, so every thread can share the heap. Like Zig's, each block starts with a header
// holding what the allocator needs back on free.
//

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zenoh_wasm_sync.h"

void *zw_brk_alloc(size_t len, uint8_t log2_align);
void zw_brk_free(void *ptr, size_t len, uint8_t log2_align);
void *zw_brk_remap(void *ptr, size_t len, uint8_t log2_align, size_t new_len);

#define ZW_MIN_ALIGN 16  // max_align_t; also leaves room for the header

typedef struct {
    size_t align;
    size_t size;
} zw_block_t;

static zw_lock_t zw_heap;

static zw_block_t *zw_header(void *base) { return (zw_block_t *)((uint8_t *)base - sizeof(zw_block_t)); }

static uint8_t zw_log2(size_t align) { return (uint8_t)__builtin_ctz((unsigned)align); }

// Called with the lock held. `align` is a power of two >= ZW_MIN_ALIGN.
static void *zw_alloc_locked(size_t align, size_t n) {
    if (n > SIZE_MAX - align) {
        return NULL;
    }
    uint8_t *p = zw_brk_alloc(n + align, zw_log2(align));
    if (p == NULL) {
        return NULL;
    }
    uint8_t *base = p + align;
    *zw_header(base) = (zw_block_t){align, n};
    return base;
}

static void zw_free_locked(void *base) {
    zw_block_t h = *zw_header(base);
    zw_brk_free((uint8_t *)base - h.align, h.size + h.align, zw_log2(h.align));
}

static void *zw_aligned(size_t align, size_t n) {
    if (align < ZW_MIN_ALIGN) {
        align = ZW_MIN_ALIGN;
    }
    zw_lock(&zw_heap);
    void *p = zw_alloc_locked(align, n);
    zw_unlock(&zw_heap);
    return p;
}

void *malloc(size_t n) {
    void *p = zw_aligned(ZW_MIN_ALIGN, n);
    if (p == NULL) {
        errno = ENOMEM;
    }
    return p;
}

void free(void *p) {
    if (p == NULL) {
        return;
    }
    zw_lock(&zw_heap);
    zw_free_locked(p);
    zw_unlock(&zw_heap);
}

void *calloc(size_t count, size_t size) {
    size_t n;
    if (__builtin_mul_overflow(count, size, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

void *realloc(void *old, size_t n) {
    if (old == NULL) {
        return malloc(n);
    }
    if (n == 0) {
        free(old);
        return NULL;
    }
    zw_lock(&zw_heap);
    zw_block_t h = *zw_header(old);
    void *p = NULL;
    if (n <= SIZE_MAX - h.align) {
        uint8_t *start = (uint8_t *)old - h.align;
        if (zw_brk_remap(start, h.size + h.align, zw_log2(h.align), n + h.align) != NULL) {
            zw_header(old)->size = n;
            p = old;
        } else if ((p = zw_alloc_locked(h.align, n)) != NULL) {
            memcpy(p, old, n < h.size ? n : h.size);
            zw_free_locked(old);
        }
    }
    zw_unlock(&zw_heap);
    if (p == NULL) {
        errno = ENOMEM;
    }
    return p;
}

void *reallocarray(void *old, size_t count, size_t size) {
    size_t n;
    if (__builtin_mul_overflow(count, size, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(old, n);
}

void *aligned_alloc(size_t align, size_t n) {
    if (align == 0 || (align & (align - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    void *p = zw_aligned(align, n);
    if (p == NULL) {
        errno = ENOMEM;
    }
    return p;
}

void *memalign(size_t align, size_t n) { return aligned_alloc(align, n); }

int posix_memalign(void **out, size_t align, size_t n) {
    if (align < sizeof(void *) || (align & (align - 1)) != 0) {
        return EINVAL;
    }
    void *p = zw_aligned(align, n);
    if (p == NULL) {
        return ENOMEM;
    }
    *out = p;
    return 0;
}

size_t malloc_usable_size(void *p) { return p != NULL ? zw_header(p)->size : 0; }
