"""Shared TLS/WS/WAMP test helpers, factored out for tests/test_robustness.py.

tests/test_transport.py intentionally does not use this module - it predates
it and works, so it's left alone to avoid risking its already-verified
behavior. New integration tests should use this instead of re-deriving the
same framing code a third time.
"""
import contextlib
import json
import os
import pathlib
import socket
import ssl
import struct
import subprocess
import tempfile
import time

binary = pathlib.Path(__file__).resolve().parent / "test_daemon"


@contextlib.contextmanager
def daemon(extra_env=None, extra_args=None):
    """Starts tests/test_daemon on a free port with a fresh state dir.

    Yields (port, state_dir, ssl_context, log_file). log_file is the
    daemon's combined stdout/stderr, seekable at any point (e.g.
    `log_file.seek(0); log_file.read()`) to inspect what it has logged so
    far. Terminates the daemon and asserts it exited promptly on the way
    out.
    """
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    with tempfile.TemporaryDirectory(prefix="spnav-test-") as state:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryFile() as log:
            proc = subprocess.Popen([str(binary), "--host", "127.0.0.1",
                                      "--port", str(port), "--state-dir", state] +
                                     list(extra_args or []),
                                     stdout=log, stderr=log, env=env)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        socket.create_connection(("127.0.0.1", port), timeout=1).close()
                        break
                    except ConnectionRefusedError:
                        assert proc.poll() is None, "daemon exited during startup"
                        if time.monotonic() > deadline:
                            raise
                        time.sleep(0.02)
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                ctx.load_verify_locations(state + "/ca.crt.pem")
                yield port, state, ctx, log
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise AssertionError("daemon did not stop promptly")
                if proc.returncode:
                    log.seek(0)
                    print(log.read().decode())


def connect(port, ctx):
    return ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=2),
                            server_hostname="127.0.0.1")


UPGRADE = (b"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
           b"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           b"Sec-WebSocket-Protocol: wamp\r\nOrigin: https://cad.onshape.com\r\n\r\n")

DISCOVERY = (b"GET /3dconnexion/nlproxy HTTP/1.1\r\n"
             b"Host: localhost\r\nOrigin: https://cad.onshape.com\r\n\r\n")


def ws_frame(payload, opcode=1, fin=True):
    length = len(payload)
    header = bytes([(0x80 if fin else 0) | opcode])
    if length < 126:
        header += bytes([0x80 | length])
    elif length <= 65535:
        header += b"\xfe" + struct.pack("!H", length)
    else:
        header += b"\xff" + struct.pack("!Q", length)
    return header + b"\0\0\0\0" + payload


class WsClient:
    """A connected, upgraded WAMP-over-WebSocket client for test use."""

    def __init__(self, port, ctx):
        self.sock = connect(port, ctx)
        self.sock.sendall(UPGRADE)
        response = self.sock.recv(4096)
        assert b"101 Switching Protocols" in response, response
        self.buffered = bytearray(response.split(b"\r\n\r\n", 1)[1])

    def close(self):
        self.sock.close()

    def take(self, n):
        while len(self.buffered) < n:
            chunk = self.sock.recv(max(4096, n - len(self.buffered)))
            assert chunk, "connection closed before response"
            self.buffered.extend(chunk)
        result = bytes(self.buffered[:n])
        del self.buffered[:n]
        return result

    def receive_json(self, timeout=2):
        self.sock.settimeout(timeout)
        header = self.take(2)
        assert header[0] == 0x81 and not header[1] & 0x80
        length = header[1] & 127
        if length == 126:
            length = struct.unpack("!H", self.take(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.take(8))[0]
        return json.loads(self.take(length))

    def send_json(self, message):
        self.sock.sendall(ws_frame(json.dumps(message).encode()))

    def handshake_as_onshape(self, controller_id="controller0", mouse_id="mouse0"):
        """Completes create-mouse/create-controller/prefix/subscribe, the
        same sequence a real Onshape client performs, so the server starts
        forwarding spacenavd motion/button events to this client."""
        assert self.receive_json()[0] == 0  # WELCOME
        self.send_json([1, "ctrl", "wss://127.51.68.120/3dconnexion3dcontroller/"])
        self.send_json([2, "m", "3dx_rpc:create", "3dconnexion:3dmouse", "1.0"])
        assert self.receive_json() == [3, "m", {"connexion": mouse_id}]
        self.send_json([2, "c", "3dx_rpc:create", "3dconnexion:3dcontroller",
                         mouse_id, {"name": "Onshape"}])
        assert self.receive_json() == [3, "c", {"instance": controller_id}]
        self.send_json([5, "ctrl:" + controller_id])


def inject_event(fifo_path, *, type_=0, x=0, y=0, z=0, rx=0, ry=0, rz=0,
                  period=16, bnum=0, pressed=0):
    """Writes one synthetic spacenavd event to a SPNAV_TEST_EVENTS FIFO -
    see tests/stub_spnav.c for the receiving side and wire format."""
    with open(fifo_path, "wb", buffering=0) as f:
        f.write(struct.pack("<10i", type_, x, y, z, rx, ry, rz, period, bnum, pressed))
