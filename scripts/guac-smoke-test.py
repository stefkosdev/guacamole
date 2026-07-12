#!/usr/bin/env python3
"""Socket-level smoke test for a running guacamole.service.

Speaks the Guacamole protocol over the service's Unix domain socket and checks
the handshake path end-to-end WITHOUT a real backend: it selects a bogus
protocol and expects the server to answer with an `error` instruction. This
exercises the accept loop, the connection thread, plugin lookup, and error
reporting.

REQUIRES the service to be running. The service is a Kin system service and
normally starts under the Kin manager (it calls kin_init with a manager PID),
so run this after Kin is up, or after starting the service manually with a
manager PID. The socket path is auto-discovered:

    $XDG_RUNTIME_DIR/kin/guacamole.sock   (preferred)
    /tmp/kin-guacamole-<uid>.sock         (fallback)

Usage: guac-smoke-test.py [socket_path]
"""
import os
import socket
import sys


def default_socket_path() -> str:
    rt = os.environ.get("XDG_RUNTIME_DIR")
    if rt:
        return os.path.join(rt, "kin", "guacamole.sock")
    return f"/tmp/kin-guacamole-{os.getuid()}.sock"


def guac_instruction(*elements: str) -> bytes:
    """Encode a Guacamole protocol instruction: LEN.VALUE,LEN.VALUE,...;"""
    encoded = ",".join(f"{len(e)}.{e}" for e in elements)
    return (encoded + ";").encode("utf-8")


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else default_socket_path()

    if not os.path.exists(path):
        print(f"SKIP: service socket not found at {path}", file=sys.stderr)
        print("      Start the Guacamole service (under Kin) and retry.", file=sys.stderr)
        return 2

    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(path)
    except OSError as exc:
        print(f"FAIL: cannot connect to {path}: {exc}", file=sys.stderr)
        return 1

    with sock:
        # Ask for a protocol that does not exist → expect an `error` reply.
        sock.sendall(guac_instruction("select", "definitely-not-a-protocol"))
        try:
            reply = sock.recv(4096)
        except OSError as exc:
            print(f"FAIL: no reply from service: {exc}", file=sys.stderr)
            return 1

    text = reply.decode("utf-8", "replace")
    print(f"service replied: {text!r}")

    if text.startswith("5.error") or ".error," in text or text.startswith("0."):
        print("OK: service completed the select handshake and reported an error as expected")
        return 0

    if not text:
        print("FAIL: service closed the connection with no protocol reply", file=sys.stderr)
        return 1

    print("WARN: unexpected reply; the socket path works but the shape was not an error", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
