"""Exercise real TLS and HTTP without a device, service, or browser trust changes."""
import pathlib
import socket
import ssl
import subprocess
import tempfile
import time
import threading
import json
import struct

binary = pathlib.Path(__file__).resolve().parent / "test_daemon"
with tempfile.TemporaryDirectory(prefix="spnav-test-") as state:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([str(binary), "--host", "127.0.0.1",
                                 "--port", str(port), "--state-dir", state],
                                stdout=log, stderr=log)
        stalled = None
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    stalled = socket.create_connection(("127.0.0.1", port), timeout=1)
                    break
                except ConnectionRefusedError:
                    assert proc.poll() is None, "daemon exited during startup"
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.02)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.load_verify_locations(state + "/ca.crt.pem")

            def connect():
                return ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=2),
                                       server_hostname="127.0.0.1")

            def request(raw):
                with connect() as client:
                    client.sendall(raw)
                    data = b""
                    while True:
                        chunk = client.recv(4096)
                        if not chunk:
                            return data
                        data += chunk

            discovery = (b"GET /3dconnexion/nlproxy HTTP/1.1\r\n"
                         b"Host: localhost\r\nOrigin: https://cad.onshape.com\r\n\r\n")
            # A TCP client that sends no TLS bytes must not stall another client.
            assert b"200 OK" in request(discovery)
            # An incomplete application request must not stall another client either.
            with connect() as partial:
                partial.sendall(b"GET /")
                assert b"200 OK" in request(discovery)
            upgrade = (b"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                       b"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       b"Sec-WebSocket-Protocol: wamp\r\nOrigin: ")
            assert b"403 Forbidden" in request(upgrade + b"https://example.org\r\n\r\n")
            with connect() as ws:
                ws.sendall(upgrade + b"https://cad.onshape.com\r\n\r\n")
                response = ws.recv(4096)
                assert b"101 Switching Protocols" in response
                buffered = bytearray(response.split(b"\r\n\r\n", 1)[1])

                def take(n):
                    while len(buffered) < n:
                        chunk = ws.recv(max(4096, n - len(buffered)))
                        assert chunk, "connection closed before response"
                        buffered.extend(chunk)
                    result = bytes(buffered[:n])
                    del buffered[:n]
                    return result

                def receive_json():
                    header = take(2)
                    assert header[0] == 0x81 and not header[1] & 0x80
                    length = header[1] & 127
                    if length == 126:
                        length = struct.unpack("!H", take(2))[0]
                    elif length == 127:
                        length = struct.unpack("!Q", take(8))[0]
                    return json.loads(take(length))

                def frame(payload, opcode=1, fin=True):
                    length = len(payload)
                    header = bytes([(0x80 if fin else 0) | opcode])
                    if length < 126:
                        header += bytes([0x80 | length])
                    elif length <= 65535:
                        header += b"\xfe" + struct.pack("!H", length)
                    else:
                        header += b"\xff" + struct.pack("!Q", length)
                    return header + b"\0\0\0\0" + payload

                def send_json(message):
                    ws.sendall(frame(json.dumps(message).encode()))

                assert receive_json()[0] == 0
                send_json([1, "3dx_rpc", "wss://127.51.68.120/3dconnexion#"])
                send_json([2, "mouse", "3dx_rpc:create", "3dconnexion:3dmouse", "0.6.0"])
                assert receive_json() == [3, "mouse", {"connexion": "mouse0"}]
                send_json([2, "controller", "3dx_rpc:create",
                           "3dconnexion:3dcontroller", "mouse0", {"name": "Onshape"}])
                assert receive_json() == [3, "controller", {"instance": "controller0"}]
                # Real Onshape sends ~374 KB of command icon images after its
                # much smaller command tree. Cover both single and split frames.
                for fragmented in (False, True):
                    call_id = "icons-split" if fragmented else "icons"
                    payload = json.dumps([2, call_id, "3dx_rpc:update",
                        "3dconnexion:3dcontroller/controller0",
                        {"images": [{"id": "test-icon", "data": "A" * 373500}]}]).encode()
                    if fragmented:
                        chunks = [payload[i:i+48000] for i in range(0, len(payload), 48000)]
                        for i, chunk in enumerate(chunks):
                            ws.sendall(frame(chunk, opcode=1 if i == 0 else 0,
                                             fin=i == len(chunks)-1))
                    else:
                        ws.sendall(frame(payload))
                    assert receive_json() == [3, call_id, None]
                ws.sendall(bytes([0x81, 0xff] + [0xff] * 8 + [1, 2, 3, 4]))
                while ws.recv(4096):
                    pass
            assert b"200 OK" in request(discovery), "bad frame killed daemon"
            # A client that floods ping requests without reading pongs must
            # neither block the server nor retain an unbounded output queue.
            raw_slow = socket.socket()
            raw_slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            raw_slow.settimeout(2)
            raw_slow.connect(("127.0.0.1", port))
            with ctx.wrap_socket(raw_slow, server_hostname="127.0.0.1") as slow:
                slow.sendall(upgrade + b"https://cad.onshape.com\r\n\r\n")
                assert b"101 Switching Protocols" in slow.recv(4096)
                def flood():
                    ping = bytes([0x89, 0xfd, 0, 0, 0, 0]) + b"x" * 125
                    try:
                        for _ in range(100):
                            slow.sendall(ping * 1000)
                    except (OSError, ssl.SSLError):
                        pass
                sender = threading.Thread(target=flood)
                sender.start()
                try:
                    time.sleep(0.1)
                    assert b"200 OK" in request(discovery), "slow reader blocked daemon"
                finally:
                    sender.join(timeout=4)
                    assert not sender.is_alive(), "slow writer did not terminate"
            # Handshake timeout releases slots even without socket readiness.
            stalled.settimeout(12)
            assert stalled.recv(1) == b""
            print("transport tests passed")
        finally:
            if stalled:
                stalled.close()
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
