"""Black-box tests for the GET/POST /spnav/sensitivity endpoint in src/main.c
(handle_sensitivity_request()) and its persistence across restarts.

Uses tests/test_daemon (see test_cli.py's docstring for why: identical
main.o, just a stubbed spacenavd connection so no real device is needed).
"""
import http.client
import json
import pathlib
import socket
import ssl
import subprocess
import tempfile
import time

binary = pathlib.Path(__file__).resolve().parent / "test_daemon"
ORIGIN = "https://cad.onshape.com"


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def start_daemon(state_dir, *extra_args):
    port = free_port()
    proc = subprocess.Popen(
        [str(binary), "--host", "127.0.0.1", "--port", str(port),
         "--state-dir", state_dir, *extra_args],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 5
    while True:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except ConnectionRefusedError:
            assert proc.poll() is None, (
                "daemon exited during startup:\n" + proc.stdout.read().decode())
            if time.monotonic() > deadline:
                raise
            time.sleep(0.02)
    return proc, port


def stop_daemon(proc):
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise AssertionError("daemon did not stop promptly")


def request(port, ca_path, method, path, origin=ORIGIN, body=None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(ca_path)
    conn = http.client.HTTPSConnection("127.0.0.1", port, timeout=2, context=ctx)
    headers = {}
    if origin is not None:
        headers["Origin"] = origin
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    try:
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        try:
            parsed = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            parsed = raw.decode()  # e.g. the plain-text 403 "origin not allowed" body
        return resp.status, parsed
    finally:
        conn.close()


def test_default_sensitivity_and_origin_check():
    with tempfile.TemporaryDirectory(prefix="spnav-test-") as state:
        proc, port = start_daemon(state)
        try:
            status, _ = request(port, state + "/ca.crt.pem", "GET",
                                 "/spnav/sensitivity", origin=None)
            assert status == 403

            status, body = request(port, state + "/ca.crt.pem", "GET",
                                    "/spnav/sensitivity")
            assert status == 200
            assert body["sensitivity"] == 0.35  # CONTROLLER_DEFAULT_SENSITIVITY
        finally:
            stop_daemon(proc)


def test_post_applies_live_and_rejects_out_of_range():
    with tempfile.TemporaryDirectory(prefix="spnav-test-") as state:
        proc, port = start_daemon(state)
        try:
            status, body = request(port, state + "/ca.crt.pem", "POST",
                                    "/spnav/sensitivity", body={"sensitivity": 0.6})
            assert status == 200
            assert body["sensitivity"] == 0.6

            status, body = request(port, state + "/ca.crt.pem", "GET",
                                    "/spnav/sensitivity")
            assert status == 200
            assert body["sensitivity"] == 0.6  # visible immediately, no restart

            for bad in (0, 0.005, 10.5, -1, "not a number"):
                status, body = request(port, state + "/ca.crt.pem", "POST",
                                        "/spnav/sensitivity", body={"sensitivity": bad})
                assert status == 400, bad

            status, body = request(port, state + "/ca.crt.pem", "GET",
                                    "/spnav/sensitivity")
            assert body["sensitivity"] == 0.6, "rejected value must not have applied"

            conf = pathlib.Path(state, "sensitivity.conf")
            assert conf.exists()
            assert float(conf.read_text().strip()) == 0.6
        finally:
            stop_daemon(proc)


def test_persisted_value_survives_restart_but_cli_flag_wins():
    with tempfile.TemporaryDirectory(prefix="spnav-test-") as state:
        proc, port = start_daemon(state)
        try:
            status, _ = request(port, state + "/ca.crt.pem", "POST",
                                 "/spnav/sensitivity", body={"sensitivity": 0.6})
            assert status == 200
        finally:
            stop_daemon(proc)

        # Restart with no --sensitivity: should pick the saved 0.6 back up.
        proc, port = start_daemon(state)
        try:
            status, body = request(port, state + "/ca.crt.pem", "GET",
                                    "/spnav/sensitivity")
            assert body["sensitivity"] == 0.6
        finally:
            stop_daemon(proc)

        # Restart with an explicit --sensitivity: CLI wins for this run...
        proc, port = start_daemon(state, "--sensitivity", "0.9")
        try:
            status, body = request(port, state + "/ca.crt.pem", "GET",
                                    "/spnav/sensitivity")
            assert body["sensitivity"] == 0.9
        finally:
            stop_daemon(proc)

        # ...but must not have overwritten the saved value on disk.
        conf = pathlib.Path(state, "sensitivity.conf")
        assert float(conf.read_text().strip()) == 0.6


if __name__ == "__main__":
    test_default_sensitivity_and_origin_check()
    test_post_applies_live_and_rejects_out_of_range()
    test_persisted_value_survives_restart_but_cli_flag_wins()
    print("sensitivity endpoint tests passed")
