#!/usr/bin/env python3
"""Probe a VNC server: confirm it speaks RFB by reading the protocol banner.

A VNC/RFB server greets every new TCP connection with a 12-byte version string
like "RFB 003.008\\n". This is enough to prove a usable target exists for the
Guacamole VNC client, without pulling in a full VNC client library.

Usage: vnc-rfb-probe.py [host] [port]   (defaults: 127.0.0.1 5900)
Exit 0 and print the banner on success; non-zero on failure.
"""
import socket
import sys


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5900

    try:
        with socket.create_connection((host, port), timeout=3) as sock:
            sock.settimeout(3)
            banner = sock.recv(12)
    except OSError as exc:
        print(f"RFB probe failed for {host}:{port}: {exc}", file=sys.stderr)
        return 1

    if banner.startswith(b"RFB "):
        version = banner.decode("ascii", "replace").strip()
        print(f"OK {host}:{port} speaks RFB — {version}")
        return 0

    print(f"Not an RFB server at {host}:{port}: {banner!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
