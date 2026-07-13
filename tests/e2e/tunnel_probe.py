#!/usr/bin/env python3
"""Probe the live WebSocket viewer tunnel like the browser does.

Connects to /api/guacamole/tunnel-ws?id=<connid> with a kin_session cookie and,
WITHOUT sending any Guacamole handshake (the browser's guacamole-common-js does
not — the tunnel performs the guacd handshake server-side), checks that the
server streams real display data (ready/img/blob/sync/cursor).

Exit 0 if display data arrives, 1 otherwise.

Usage: tunnel_probe.py <host:port> <connid> <kin_session-cookie>
"""
import base64
import os
import socket
import struct
import sys
import time
from collections import Counter


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    hp, connid, cookie = sys.argv[1], sys.argv[2], sys.argv[3]
    host, port = hp.split(":")
    port = int(port)

    s = socket.create_connection((host, port), timeout=6)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((
        f"GET /api/guacamole/tunnel-ws?id={connid} HTTP/1.1\r\nHost: {hp}\r\n"
        f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Protocol: guacamole\r\n"
        f"Origin: http://{hp}\r\nCookie: kin_session={cookie}\r\n\r\n"
    ).encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            print("FAIL: no upgrade response")
            return 1
        buf += d
    status_line = buf.split(b"\r\n", 1)[0].decode("ascii", "replace")
    if "101" not in status_line:
        print(f"FAIL: no WebSocket upgrade ({status_line})")
        return 1

    rb = bytearray(buf.split(b"\r\n\r\n", 1)[1])

    def more():
        d = s.recv(65536)
        if not d:
            raise ConnectionError("closed")
        rb.extend(d)

    def frame():
        s.settimeout(8)
        while len(rb) < 2:
            more()
        ln = rb[1] & 0x7f
        idx = 2
        if ln == 126:
            while len(rb) < 4:
                more()
            ln = struct.unpack(">H", rb[2:4])[0]
            idx = 4
        elif ln == 127:
            while len(rb) < 10:
                more()
            ln = struct.unpack(">Q", rb[2:10])[0]
            idx = 10
        while len(rb) < idx + ln:
            more()
        payload = bytes(rb[idx:idx + ln])
        del rb[:idx + ln]
        return payload

    opcodes = Counter()
    deadline = time.time() + 8
    try:
        while time.time() < deadline:
            for chunk in frame().decode("utf-8", "replace").split(";"):
                if not chunk:
                    continue
                dot = chunk.find(".")
                if dot < 0:
                    continue
                n = int(chunk[:dot])
                opcodes[chunk[dot + 1:dot + 1 + n]] += 1
    except Exception:
        pass

    print("opcodes:", dict(opcodes))
    display = {"img", "blob", "sync", "cursor", "rect", "cfill", "png"}
    if display & set(opcodes):
        print("PASS: tunnel streamed live display data")
        return 0
    print("FAIL: no display data (viewer would be black)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
