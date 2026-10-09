#!/usr/bin/env python3
"""Static file server for the zenoh-pico browser build.

Sends the headers that make the page cross-origin isolated, which zenoh.js needs for
SharedArrayBuffer/Atomics.wait:
    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp

Usage: serve.py <directory> [port] [bind-address]
"""

import functools
import http.server
import sys


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    directory = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    bind = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
    handler = functools.partial(IsolatedHandler, directory=directory)
    with http.server.ThreadingHTTPServer((bind, port), handler) as httpd:
        print(f"Serving {directory} at http://{bind}:{port}/ (cross-origin isolated)", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
