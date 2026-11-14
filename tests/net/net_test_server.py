#!/usr/bin/env python3
"""A small HTTP server on 127.0.0.1 for the tests of quench_net's platform backends (tests/net/AppleNetTests.cpp).

    python3 tests/net/net_test_server.py --run EXE [ARG...]
        starts the server on a free port of 127.0.0.1, runs EXE with QUENCH_TEST_NET_URL=http://127.0.0.1:<port>
        in its environment, stops the server and exits with EXE's exit code (CTest's launcher of the cases)
    python3 tests/net/net_test_server.py [--port N]
        serves until interrupted (by hand)

Paths (the server never echoes a header's value that could be a secret; it says whether one arrived):
    GET  /hello                  200 "hello", X-Test: yes
    POST /echo                   200 with the request's body; X-Echo-Method, X-Echo-Custom (the X-Custom header),
                                 X-Echo-Length
    GET  /status/<code>          that status with a short body
    GET  /chunked?n=N&gap=S      N chunks of 64 bytes, S seconds apart (Transfer-Encoding: chunked)
    GET  /slow?seconds=S         the answer after S seconds
    GET  /stall                  one chunk, then nothing for 30 s (a cancel in the middle)
    GET  /redirect?to=URL        302 to URL
    GET  /secrets                200 "authorization=present|absent;x-api-key=present|absent;cookie=present|absent"
    GET  /set-cookie             200 with Set-Cookie: session=abc
    GET  /big?bytes=N[&chunked=1] N bytes, with a Content-Length or chunked without one

WebSocket paths (an upgrade request, RFC 6455 framing written with the standard library; ws://127.0.0.1:<port>):
    /ws/echo                     every text or binary message back; pings answered; a close answered with its code
    /ws/script                   "one", 0.2 s later "two", a binary message 00 01 02, then a close 4000 "done"
    /ws/big?bytes=N              a text message of N bytes, then as /ws/echo
    /ws/secrets                  one text message "authorization=present|absent;protocol=<offered or none>"
    /ws/nopong                   as /ws/echo without answering pings
    /ws/drop                     0.2 s after the upgrade the TCP connection closes without a close frame
    /ws/status/<code>            the upgrade refused with that status
"""

import base64
import hashlib
import http.server
import os
import struct
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WsConnection:
    """The server's side of one WebSocket connection: frames read and written on the handler's streams."""

    def __init__(self, rfile, wfile):
        self.rfile, self.wfile = rfile, wfile
        self.lock = threading.Lock()

    def read_exact(self, n):
        data = b""
        while len(data) < n:
            piece = self.rfile.read(n - len(data))
            if not piece:
                raise ConnectionResetError()
            data += piece
        return data

    def read_frame(self):
        b0, b1 = self.read_exact(2)
        fin, opcode, masked, length = b0 & 0x80, b0 & 0x0F, b1 & 0x80, b1 & 0x7F
        if length == 126:
            length = struct.unpack(">H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self.read_exact(8))[0]
        mask = self.read_exact(4) if masked else b"\0\0\0\0"
        payload = bytearray(self.read_exact(length))
        for i in range(length):
            payload[i] ^= mask[i % 4]
        return bool(fin), opcode, bytes(payload)

    def write_frame(self, opcode, payload):
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([n])
        elif n < 65536:
            header += bytes([126]) + struct.pack(">H", n)
        else:
            header += bytes([127]) + struct.pack(">Q", n)
        with self.lock:
            self.wfile.write(header + payload)
            self.wfile.flush()

    def send_text(self, text):
        self.write_frame(0x1, text.encode() if isinstance(text, str) else text)

    def send_binary(self, data):
        self.write_frame(0x2, data)

    def send_close(self, code, reason=b""):
        self.write_frame(0x8, struct.pack(">H", code) + reason)

    def serve(self, answer_pings=True):
        """Echoes messages until a close; answers the close with the same code."""
        message, kind = b"", None
        while True:
            fin, opcode, payload = self.read_frame()
            if opcode == 0x9:
                if answer_pings:
                    self.write_frame(0xA, payload)
            elif opcode == 0x8:
                self.write_frame(0x8, payload)
                return
            elif opcode in (0x1, 0x2, 0x0):
                if opcode != 0x0:
                    kind = opcode
                message += payload
                if fin:
                    self.write_frame(kind, message)
                    message = b""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # quiet
        pass

    def send_body(self, status, body, headers=None, content_type="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def start_chunked(self, content_type="application/octet-stream"):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        # NSURLSession holds back the first 512 bytes of a body to sniff its type unless told not to.
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def chunk(self, data):
        self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
        self.wfile.flush()

    def end_chunked(self):
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length > 0 else b""

    def do_POST(self):
        self.route(self.read_body())

    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            self.websocket()
            return
        self.route(b"")

    def websocket(self):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        path = url.path
        if path.startswith("/ws/status/"):
            self.send_body(int(path[len("/ws/status/"):]), b"refused")
            return
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_body(400, b"no Sec-WebSocket-Key")
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        offered = self.headers.get("Sec-WebSocket-Protocol")
        if offered:
            self.send_header("Sec-WebSocket-Protocol", offered.split(",")[0].strip())
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        ws = WsConnection(self.rfile, self.wfile)
        try:
            if path == "/ws/echo":
                ws.serve()
            elif path == "/ws/script":
                ws.send_text("one")
                time.sleep(0.2)
                ws.send_text("two")
                ws.send_binary(bytes([0, 1, 2]))
                ws.send_close(4000, b"done")
                ws.read_frame()  # the client's answer
            elif path == "/ws/big":
                ws.send_text("b" * int(query.get("bytes", "100000")))
                ws.serve()
            elif path == "/ws/secrets":
                seen = "present" if self.headers.get("Authorization") is not None else "absent"
                ws.send_text("authorization=%s;protocol=%s" % (seen, offered or "none"))
                ws.serve()
            elif path == "/ws/nopong":
                ws.serve(answer_pings=False)
            elif path == "/ws/drop":
                time.sleep(0.2)
                self.connection.shutdown(2)
            else:
                ws.send_close(1008, b"unknown path")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def route(self, body):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        path = url.path
        try:
            if path == "/hello":
                self.send_body(200, b"hello", {"X-Test": "yes"})
            elif path == "/echo":
                self.send_body(200, body, {
                    "X-Echo-Method": self.command,
                    "X-Echo-Custom": self.headers.get("X-Custom", ""),
                    "X-Echo-Length": str(len(body)),
                }, content_type=self.headers.get("Content-Type", "application/octet-stream"))
            elif path.startswith("/status/"):
                code = int(path[len("/status/"):])
                self.send_body(code, ("status %d" % code).encode())
            elif path == "/chunked":
                count, gap = int(query.get("n", "5")), float(query.get("gap", "0.2"))
                self.start_chunked("text/event-stream")
                for i in range(count):
                    if i:
                        time.sleep(gap)
                    self.chunk(("data: %d " % i).encode().ljust(63, b".") + b"\n")
                self.end_chunked()
            elif path == "/slow":
                time.sleep(float(query.get("seconds", "3")))
                self.send_body(200, b"late")
            elif path == "/stall":
                self.start_chunked()
                self.chunk(b"first piece")
                time.sleep(30)
                self.end_chunked()
            elif path == "/redirect":
                self.send_response(302)
                self.send_header("Location", query.get("to", "/hello"))
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif path == "/secrets":
                def seen(name):
                    return "present" if self.headers.get(name) is not None else "absent"
                text = "authorization=%s;x-api-key=%s;cookie=%s" % (seen("Authorization"), seen("X-API-Key"),
                                                                   seen("Cookie"))
                self.send_body(200, text.encode())
            elif path == "/set-cookie":
                self.send_body(200, b"cookie set", {"Set-Cookie": "session=abc; Path=/"})
            elif path == "/big":
                size = int(query.get("bytes", "1000000"))
                if query.get("chunked") == "1":
                    self.start_chunked()
                    sent = 0
                    while sent < size:
                        piece = min(16384, size - sent)
                        self.chunk(b"b" * piece)
                        sent += piece
                    self.end_chunked()
                else:
                    self.send_body(200, b"b" * size)
            else:
                self.send_body(404, b"not found")
        except (BrokenPipeError, ConnectionResetError):
            pass  # the client went away (a cancel, a size limit)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, client_address)  # a client going away is expected (cancels, limits)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--run":
        server = Server(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, args=(0.05,), daemon=True)
        thread.start()
        env = dict(os.environ, QUENCH_TEST_NET_URL="http://127.0.0.1:%d" % server.server_address[1])
        code = subprocess.call(args[1:], env=env)
        server.shutdown()
        sys.exit(code)
    port = int(args[1]) if len(args) == 2 and args[0] == "--port" else 8765
    server = Server(("127.0.0.1", port), Handler)
    print("serving on http://127.0.0.1:%d" % port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
