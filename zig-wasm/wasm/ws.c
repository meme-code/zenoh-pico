//
// zenoh-pico WebSocket link for web browsers.
//
// Each zenoh batch travels as one binary WebSocket message (the ws link is a datagram link),
// so a read returns exactly one message and a write sends exactly one.
//
// The WebSocket object lives on the page (web/zenoh-runtime.js). Its data moves through two
// rings in this module's shared memory: the page appends received messages to `rx` and sends
// whatever wasm appends to `tx`. Any thread can therefore use any socket, and a blocked read
// just waits on zw_ctrl.wake, which the page bumps after every change.
//

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zenoh-pico/config.h"
#include "zenoh-pico/link/endpoint.h"
#include "zenoh-pico/link/transport/socket.h"
#include "zenoh-pico/link/transport/ws.h"
#include "zenoh-pico/system/platform.h"
#include "zenoh-pico/utils/logging.h"
#include "zenoh-pico/utils/pointers.h"
#include "zenoh_wasm_host.h"
#include "zenoh_wasm_sync.h"

zw_ctrl_t zw_ctrl;
bool zw_interruptible;

#if Z_FEATURE_LINK_WS == 1

// Socket states; also in web/zenoh-shared.js.
#define ZW_WS_CONNECTING 1
#define ZW_WS_OPEN 2
#define ZW_WS_CLOSED 3
#define ZW_WS_RELEASED 4  // the page has dropped the socket and will not touch it again

#define ZW_WS_RX_CAPACITY (1u << 20)
#define ZW_WS_TX_CAPACITY (1u << 18)
#define ZW_WS_RELEASE_TIMEOUT_MS 2000

// Shared with the page: the offsets of every field up to `tx` are in web/zenoh-shared.js.
// Ring records are [u32 little-endian length][message]; the counters are free-running.
struct zw_ws {
    zw_ctrl_t *ctrl;
    _Atomic int32_t state;
    _Atomic int32_t tx_kick;  // 1 while a ws_kick for this socket is pending on the page
    _Atomic uint32_t rx_write;
    _Atomic uint32_t rx_read;
    _Atomic uint32_t tx_write;
    _Atomic uint32_t tx_read;
    uint32_t rx_capacity;
    uint32_t tx_capacity;
    uint8_t *rx;
    uint8_t *tx;
    zw_lock_t tx_lock;
};
_Static_assert(offsetof(struct zw_ws, tx) == 40, "keep web/zenoh-shared.js in sync");

static int64_t zw_remaining_ns(z_clock_t deadline) {
    z_clock_t now = z_clock_now();
    return deadline > now ? (int64_t)(deadline - now) * 1000 : 0;
}

// Takes the interrupt the page raises when a call from JavaScript is waiting (single-threaded
// builds, where reads happen inside zw_spin).
static bool zw_take_interrupt(void) { return zw_interruptible && atomic_exchange(&zw_ctrl.interrupt, 0) != 0; }

static void zw_ring_copy_in(uint8_t *ring, uint32_t capacity, uint32_t pos, const uint8_t *src, size_t len) {
    uint32_t start = pos & (capacity - 1);
    size_t first = len < capacity - start ? len : capacity - start;
    memcpy(ring + start, src, first);
    memcpy(ring, src + first, len - first);
}

static void zw_ring_copy_out(const uint8_t *ring, uint32_t capacity, uint32_t pos, uint8_t *dst, size_t len) {
    uint32_t start = pos & (capacity - 1);
    size_t first = len < capacity - start ? len : capacity - start;
    memcpy(dst, ring + start, first);
    memcpy(dst + first, ring, len - first);
}

static zw_ws_t *zw_ws_new(void) {
    zw_ws_t *ws = malloc(sizeof(zw_ws_t) + ZW_WS_RX_CAPACITY + ZW_WS_TX_CAPACITY);
    if (ws == NULL) {
        return NULL;
    }
    memset(ws, 0, sizeof(*ws));
    ws->ctrl = &zw_ctrl;
    ws->state = ZW_WS_CONNECTING;
    ws->rx_capacity = ZW_WS_RX_CAPACITY;
    ws->tx_capacity = ZW_WS_TX_CAPACITY;
    ws->rx = (uint8_t *)(ws + 1);
    ws->tx = ws->rx + ZW_WS_RX_CAPACITY;
    return ws;
}

// Waits until `state` leaves `from`, for at most timeout_ms. Returns the last state seen.
static int32_t zw_ws_wait_state(zw_ws_t *ws, int32_t from, uint32_t timeout_ms) {
    z_clock_t deadline = z_clock_now() + (z_clock_t)timeout_ms * 1000;
    for (;;) {
        int32_t seq = atomic_load(&zw_ctrl.wake);
        int32_t state = atomic_load(&ws->state);
        int64_t ns = zw_remaining_ns(deadline);
        if (state != from || ns == 0) {
            return state;
        }
        zw_futex_wait(&zw_ctrl.wake, seq, ns);
    }
}

// Closes the WebSocket and frees the rings once the page has let go of them.
static void zw_ws_release(zw_ws_t *ws) {
    zw_host_ws_close(ws);
    for (;;) {
        int32_t state = atomic_load(&ws->state);
        if (state == ZW_WS_RELEASED) {
            free(ws);
            return;
        }
        if (zw_ws_wait_state(ws, state, ZW_WS_RELEASE_TIMEOUT_MS) == state) {
            // The page never answered; leaking is safer than freeing memory it may still write.
            _Z_ERROR("WebSocket was not released by the page");
            return;
        }
    }
}

// Gets the page to send what is queued in ws->tx. Ringing the doorbell is just an atomic
// notify; calling into JavaScript to post a message takes far longer, and this runs with
// zenoh-pico's transmit mutex held (concurrent puts with congestion control "drop" give up
// when they find it taken).
static void zw_ws_kick(zw_ws_t *ws) {
    if (atomic_exchange(&ws->tx_kick, 1) != 0) {
        return;  // the page has not drained this socket since the last kick
    }
    if (atomic_load(&zw_ctrl.doorbell)) {
        atomic_fetch_add(&zw_ctrl.tx_bell, 1);
        zw_futex_wake(&zw_ctrl.tx_bell, 1);
    } else {
        zw_host_ws_kick(ws);
    }
}

z_result_t _z_ws_endpoint_init(_z_sys_net_endpoint_t *ep, const _z_string_t *address) {
    ep->_host = _z_endpoint_parse_host(address);
    ep->_port = _z_endpoint_parse_port(address);
    if ((ep->_host == NULL) || (ep->_port == NULL)) {
        _z_ws_endpoint_clear(ep);
        _Z_ERROR_RETURN(_Z_ERR_CONFIG_LOCATOR_INVALID);
    }
    return _Z_RES_OK;
}

void _z_ws_endpoint_clear(_z_sys_net_endpoint_t *ep) {
    z_free(ep->_host);
    z_free(ep->_port);
    ep->_host = NULL;
    ep->_port = NULL;
}

z_result_t _z_ws_transport_open(_z_ws_socket_t *sock, uint32_t tout) {
    const _z_sys_net_endpoint_t *rep = &sock->_rep;
    zw_ws_t *ws = zw_ws_new();
    if (ws == NULL) {
        _Z_ERROR_RETURN(_Z_ERR_SYSTEM_OUT_OF_MEMORY);
    }
    zw_host_ws_open(ws, rep->_host, strlen(rep->_host), rep->_port, strlen(rep->_port));
    // tout is the per-read timeout; establishing the WebSocket (DNS, TCP, maybe TLS) gets the
    // longer connect timeout.
    if (zw_ws_wait_state(ws, ZW_WS_CONNECTING, Z_TRANSPORT_CONNECT_TIMEOUT) != ZW_WS_OPEN) {
        zw_ws_release(ws);
        _Z_ERROR("Unable to open WebSocket to %s:%s", rep->_host, rep->_port);
        _Z_ERROR_RETURN(_Z_ERR_TRANSPORT_OPEN_FAILED);
    }
    sock->_sock._ws = ws;
    sock->_sock._tout = tout;
    return _Z_RES_OK;
}

z_result_t _z_ws_transport_listen(_z_ws_socket_t *sock) {
    _ZP_UNUSED(sock);
    _Z_ERROR("Browsers cannot listen for incoming WebSocket connections");
    _Z_ERROR_RETURN(_Z_ERR_GENERIC);
}

void _z_ws_transport_close(_z_ws_socket_t *sock) { _z_socket_close(&sock->_sock); }

size_t _z_ws_transport_read_socket(const _z_sys_net_socket_t socket, uint8_t *ptr, size_t len) {
    zw_ws_t *ws = socket._ws;
    if (ws == NULL) {
        return SIZE_MAX;
    }
    // Like a datagram socket with a receive timeout: no data within tout is SIZE_MAX.
    z_clock_t deadline = z_clock_now() + (z_clock_t)socket._tout * 1000;
    for (;;) {
        int32_t seq = atomic_load(&zw_ctrl.wake);
        uint32_t r = atomic_load_explicit(&ws->rx_read, memory_order_relaxed);
        if (atomic_load(&ws->rx_write) != r) {
            uint32_t msg_len;
            zw_ring_copy_out(ws->rx, ws->rx_capacity, r, (uint8_t *)&msg_len, sizeof(msg_len));
            size_t n = msg_len < len ? msg_len : len;
            if (n < msg_len) {
                _Z_ERROR("%u bytes of a WebSocket message did not fit in the read buffer", (unsigned)(msg_len - n));
            }
            zw_ring_copy_out(ws->rx, ws->rx_capacity, r + sizeof(msg_len), ptr, n);
            atomic_store(&ws->rx_read, r + (uint32_t)sizeof(msg_len) + msg_len);
            return n;
        }
        // Messages received before the socket closed were delivered above. After that, behave
        // like a timed-out read so the read task does not spin while the lease (or
        // auto-reconnect) notices that the link is gone.
        int64_t ns = zw_remaining_ns(deadline);
        if (ns == 0 || zw_take_interrupt()) {
            return SIZE_MAX;
        }
        zw_futex_wait(&zw_ctrl.wake, seq, ns);
    }
}

size_t _z_ws_transport_read(const _z_ws_socket_t *sock, uint8_t *ptr, size_t len) {
    return _z_ws_transport_read_socket(sock->_sock, ptr, len);
}

size_t _z_ws_transport_read_exact(const _z_ws_socket_t *sock, uint8_t *ptr, size_t len) {
    size_t n = 0;
    uint8_t *pos = ptr;
    do {
        size_t rb = _z_ws_transport_read(sock, pos, len - n);
        if (rb == SIZE_MAX) {
            return SIZE_MAX;
        }
        n += rb;
        pos = _z_ptr_u8_offset(pos, (ptrdiff_t)rb);
    } while (n != len);
    return n;
}

size_t _z_ws_transport_write(const _z_ws_socket_t *sock, const uint8_t *ptr, size_t len) {
    zw_ws_t *ws = sock->_sock._ws;
    uint32_t msg_len = (uint32_t)len;
    size_t need = sizeof(msg_len) + len;
    if (ws == NULL || need > ws->tx_capacity) {
        return SIZE_MAX;
    }
    zw_lock(&ws->tx_lock);
    uint32_t w = atomic_load_explicit(&ws->tx_write, memory_order_relaxed);
    z_clock_t deadline = z_clock_now() + (z_clock_t)Z_TRANSPORT_CONNECT_TIMEOUT * 1000;
    for (;;) {
        int32_t seq = atomic_load(&zw_ctrl.wake);
        if (atomic_load(&ws->state) != ZW_WS_OPEN) {
            zw_unlock(&ws->tx_lock);
            return SIZE_MAX;
        }
        if (ws->tx_capacity - (w - atomic_load(&ws->tx_read)) >= need) {
            break;
        }
        // Full: the page is behind on sending. Make sure it is on its way, then wait.
        zw_ws_kick(ws);
        int64_t ns = zw_remaining_ns(deadline);
        if (ns == 0) {
            zw_unlock(&ws->tx_lock);
            _Z_ERROR("WebSocket send buffer stayed full");
            return SIZE_MAX;
        }
        zw_futex_wait(&zw_ctrl.wake, seq, ns);
    }
    zw_ring_copy_in(ws->tx, ws->tx_capacity, w, (const uint8_t *)&msg_len, sizeof(msg_len));
    zw_ring_copy_in(ws->tx, ws->tx_capacity, w + sizeof(msg_len), ptr, len);
    atomic_store(&ws->tx_write, w + (uint32_t)need);
    zw_unlock(&ws->tx_lock);
    zw_ws_kick(ws);
    return len;
}

/*------------------ Sockets ------------------*/
// WebSocket handles are always non-blocking with an explicit wait in the read.
z_result_t _z_socket_set_blocking(const _z_sys_net_socket_t *sock, bool blocking) {
    _ZP_UNUSED(sock);
    _ZP_UNUSED(blocking);
    return _Z_RES_OK;
}

void _z_socket_close(_z_sys_net_socket_t *sock) {
    if (sock->_ws != NULL) {
        zw_ws_release(sock->_ws);
        sock->_ws = NULL;
    }
}

static bool zw_ws_readable(const zw_ws_t *ws) {
    return ws != NULL && (atomic_load(&ws->rx_write) != atomic_load(&ws->rx_read) ||
                          atomic_load(&ws->state) != ZW_WS_OPEN);
}

// Peer mode: waits until one of several sockets has a message (or has closed).
z_result_t _z_socket_wait_readable(_z_socket_wait_iter_t *iter, uint32_t timeout_ms) {
    z_clock_t deadline = z_clock_now() + (z_clock_t)timeout_ms * 1000;
    for (;;) {
        int32_t seq = atomic_load(&zw_ctrl.wake);
        bool has_sockets = false;
        bool has_data = false;
        _z_socket_wait_iter_reset(iter);
        while (_z_socket_wait_iter_next(iter)) {
            bool ready = zw_ws_readable(_z_socket_wait_iter_get_socket(iter)->_ws);
            _z_socket_wait_iter_set_ready(iter, ready);
            has_sockets = true;
            has_data |= ready;
        }
        if (!has_sockets || has_data) {
            return _Z_RES_OK;
        }
        int64_t ns = zw_remaining_ns(deadline);
        if (ns == 0 || zw_take_interrupt()) {
            return _Z_NO_DATA_PROCESSED;
        }
        zw_futex_wait(&zw_ctrl.wake, seq, ns);
    }
}

#else
z_result_t _z_socket_set_blocking(const _z_sys_net_socket_t *sock, bool blocking) {
    _ZP_UNUSED(sock);
    _ZP_UNUSED(blocking);
    return _Z_RES_OK;
}

void _z_socket_close(_z_sys_net_socket_t *sock) { _ZP_UNUSED(sock); }

z_result_t _z_socket_wait_readable(_z_socket_wait_iter_t *iter, uint32_t timeout_ms) {
    _ZP_UNUSED(iter);
    _ZP_UNUSED(timeout_ms);
    return _Z_RES_OK;
}
#endif
