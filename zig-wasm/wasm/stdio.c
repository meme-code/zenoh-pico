//
// Thread-safe printing to stdout/stderr for multi-threaded browser builds.
//
// Zig's wasi-libc is built single-threaded, so its FILE functions take no locks: two threads
// printing at once corrupt each other's output (and the FILE's buffer pointers). These strong
// definitions format into a private buffer and hand each call to the host as one write
// (fd_write), which web/zenoh-worker.js assembles into lines per thread. Other streams (there
// are no files in a browser, but e.g. fmemopen) still go through libc's vfprintf.
//
// fwrite and the wide-character functions are not covered.
//

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static int zw_std_fd(FILE *f) { return f == stdout ? 1 : f == stderr ? 2 : -1; }

static int zw_write_all(int fd, struct iovec *iov, int iovcnt) {
    while (iovcnt > 0) {
        ssize_t n = writev(fd, iov, iovcnt);
        if (n < 0) {
            return -1;
        }
        while (iovcnt > 0 && (size_t)n >= iov->iov_len) {
            n -= (ssize_t)iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0) {
            iov->iov_base = (char *)iov->iov_base + n;
            iov->iov_len -= (size_t)n;
        }
    }
    return 0;
}

static int zw_write(int fd, const void *buf, size_t len) {
    struct iovec iov = {(void *)buf, len};
    return zw_write_all(fd, &iov, 1);
}

static int zw_vprint(int fd, const char *fmt, va_list ap) {
    char stack[1024];
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    char *buf = stack;
    if (n >= (int)sizeof(stack)) {
        buf = malloc((size_t)n + 1);
        if (buf != NULL) {
            vsnprintf(buf, (size_t)n + 1, fmt, again);
        }
    }
    va_end(again);
    if (n < 0 || buf == NULL) {
        return -1;
    }
    int res = zw_write(fd, buf, (size_t)n) == 0 ? n : -1;
    if (buf != stack) {
        free(buf);
    }
    return res;
}

int vprintf(const char *restrict fmt, va_list ap) { return zw_vprint(1, fmt, ap); }

int printf(const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = zw_vprint(1, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *restrict f, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int fd = zw_std_fd(f);
    int n = fd >= 0 ? zw_vprint(fd, fmt, ap) : vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    struct iovec iov[2] = {{(void *)s, strlen(s)}, {"\n", 1}};
    return zw_write_all(1, iov, 2) == 0 ? 0 : EOF;
}

int fputs(const char *restrict s, FILE *restrict f) {
    int fd = zw_std_fd(f);
    if (fd < 0) {
        return fprintf(f, "%s", s) < 0 ? EOF : 0;
    }
    return zw_write(fd, s, strlen(s)) == 0 ? 0 : EOF;
}

int fputc(int c, FILE *f) {
    int fd = zw_std_fd(f);
    unsigned char b = (unsigned char)c;
    if (fd < 0) {
        return fprintf(f, "%c", b) < 0 ? EOF : b;
    }
    return zw_write(fd, &b, 1) == 0 ? b : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int putchar(int c) { return fputc(c, stdout); }
