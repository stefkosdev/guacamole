#!/usr/bin/env python3
"""Minimal Guacamole protocol client that drives a stored connection.

Connects to guacamole.service over its Unix socket, selects a stored connection
by id (`select $<id>`), completes the client handshake, and sends `connect`.
By default it sends EMPTY argv: if the service's stored connection actually
reaches the backend plugin, the session still comes up (host/port/credentials
come from the store, not the client), and the server answers with `ready`.

Exit 0 if the server reached `ready` (session established), else 1.

Usage: guac_client_probe.py <socket_path> <$id> [empty|real]
  real → also send hostname/port/password in argv (values below), to prove the
         backend + target work via the client-supplied path.
"""
import socket
import sys
import time
from collections import Counter

REAL_VALUES = {"hostname": "127.0.0.1", "port": "5905", "password": "guacvnc"}


def enc(*els):
    return (",".join(f"{len(e)}.{e}" for e in els) + ";").encode()


def parse_instr(raw):
    els, i = [], 0
    while i < len(raw):
        dot = raw.find(b".", i)
        if dot == -1:
            break
        n = int(raw[i:dot])
        els.append(raw[dot + 1:dot + 1 + n].decode("utf-8", "replace"))
        i = dot + 1 + n
        if i < len(raw) and raw[i:i + 1] == b",":
            i += 1
    return els


class Reader:
    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def next_instr(self, timeout=4.0):
        self.sock.settimeout(timeout)
        while True:
            i = self.buf.find(b";")
            if i != -1:
                raw, self.buf = self.buf[:i], self.buf[i + 1:]
                return parse_instr(raw)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, ident = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else "empty"

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    r = Reader(s)

    print(f"-> select {ident}")
    s.sendall(enc("select", ident))

    argnames = None
    for _ in range(10):
        instr = r.next_instr()
        if instr is None:
            break
        if instr[0] == "args":
            argnames = instr[2:]  # [0]=args, [1]=protocol version
            break
        if instr[0] in ("error", "disconnect"):
            print(f"<- {instr[0]}: {instr[1:]}")
            return 1
    if argnames is None:
        print("RESULT: FAIL — no `args` from server")
        return 1
    print(f"<- args ({len(argnames)} params)")

    values = [REAL_VALUES.get(a, "") if mode == "real" else "" for a in argnames]

    s.sendall(enc("size", "1024", "768", "96"))
    s.sendall(enc("audio"))
    s.sendall(enc("video"))
    s.sendall(enc("image"))
    print(f"-> connect ({mode} argv)")
    s.sendall(enc("connect", *values))

    opcodes, ready, deadline = [], False, time.time() + 6.0
    while time.time() < deadline:
        instr = r.next_instr(timeout=2.0)
        if instr is None:
            break
        opcodes.append(instr[0])
        if instr[0] == "ready":
            ready = True
            print(f"<- ready: {instr[1:]}")
        elif instr[0] in ("error", "disconnect"):
            print(f"<- {instr[0]}: {instr[1:]}")
            break
    s.close()

    print(f"opcodes: {dict(Counter(opcodes))}")
    if ready:
        print("RESULT: PASS — session reached `ready` (stored connection accepted)")
        return 0
    print("RESULT: FAIL — session did not reach `ready`")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
