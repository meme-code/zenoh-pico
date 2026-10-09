//
// zenoh-pico WebSocket link for web browsers.
//
// Each zenoh batch travels as one binary WebSocket message (the ws link is a datagram link),
// so a read returns exactly one message and a write sends exactly one.
//

#include <stdint.h>
#include <string.h>

#include "zenoh-pico/config.h"
#include "zenoh-pico/link/endpoint.h"
#include "zenoh-pico/link/transport/ws.h"
#include "zenoh-pico/system/platform.h"
#include "zenoh-pico/utils/logging.h"
#include "zenoh-pico/utils/pointers.h"
#include "zenoh_wasm_host.h"

#if Z_FEATURE_LINK_WS == 1

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
    // tout is the per-read timeout; establishing the WebSocket (DNS, TCP, maybe TLS) gets the
    // longer connect timeout.
    int32_t ws = zw_host_ws_open(rep->_host, strlen(rep->_host), rep->_port, strlen(rep->_port),
                                 Z_TRANSPORT_CONNECT_TIMEOUT);
    if (ws < 0) {
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
    if (socket._ws < 0) {
        return SIZE_MAX;
    }
    int32_t rb = zw_host_ws_recv(socket._ws, ptr, len, socket._tout);
    // Like a datagram socket with a receive timeout: no data within tout is SIZE_MAX.
    return rb > 0 ? (size_t)rb : SIZE_MAX;
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
    if (sock->_sock._ws < 0) {
        return SIZE_MAX;
    }
    int32_t sb = zw_host_ws_send(sock->_sock._ws, ptr, len);
    return sb < 0 ? SIZE_MAX : (size_t)sb;
}

#else
typedef int _zw_ws_transport_disabled_t;
#endif
