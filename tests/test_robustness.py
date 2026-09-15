"""Exercises server behavior that test_transport.py doesn't reach: fanning
spacenavd events out to multiple simultaneous clients, rejecting a
connection once MAX_CLIENTS slots are full, and closing a connection whose
single WebSocket frame exceeds the message-size limit outright (as opposed
to test_transport.py's near-limit fragmented-message case)."""
import os
import socket
import tempfile
import time

import wstest

MAX_CLIENTS = 8  # must match src/main.c


def test_spnav_event_fans_out_to_all_subscribed_clients():
    with tempfile.TemporaryDirectory(prefix="spnav-test-fifo-") as tmp:
        fifo_path = tmp + "/events.fifo"
        os.mkfifo(fifo_path)
        with wstest.daemon(extra_env={"SPNAV_TEST_EVENTS": fifo_path}) as (port, state, ctx):
            a = wstest.WsClient(port, ctx)
            b = wstest.WsClient(port, ctx)
            try:
                a.handshake_as_onshape()
                b.handshake_as_onshape()

                wstest.inject_event(fifo_path, x=100, y=0, z=0)

                for name, client in (("a", a), ("b", b)):
                    ev = client.receive_json()
                    assert ev[0] == 8, (name, ev)
                    assert ev[1] == "wss://127.51.68.120/3dconnexion3dcontroller/controller0"
                    call = ev[2]
                    assert call[2:5] == ["self:read", "", "model.extents"], (name, call)
            finally:
                a.close()
                b.close()
    print("fan-out test passed")


def test_max_clients_is_enforced_and_slots_are_reused():
    # listen()'s own backlog (src/main.c) happens to equal MAX_CLIENTS, so
    # opening MAX_CLIENTS+1 raw sockets *simultaneously* mostly tests the
    # kernel's backlog, not find_free_slot(). Instead, accept each of the
    # first MAX_CLIENTS one at a time, confirmed via a completed TLS+WS
    # handshake (proof the daemon actually accept()ed and slotted it) -
    # this leaves the kernel's backlog empty, so the next raw connection
    # reaches accept4() cleanly and exercises the application-level
    # "too many concurrent connections" rejection deterministically.
    with wstest.daemon() as (port, state, ctx):
        held = [wstest.WsClient(port, ctx) for _ in range(MAX_CLIENTS)]
        try:
            overflow = socket.create_connection(("127.0.0.1", port), timeout=2)
            overflow.settimeout(2)
            assert overflow.recv(1) == b"", "9th connection should be rejected immediately"
            overflow.close()

            held.pop().close()
            time.sleep(0.2)  # let close_client() free the slot
            reused = wstest.WsClient(port, ctx)  # raises if the freed slot isn't reused
            reused.close()
        finally:
            for c in held:
                c.close()
    print("MAX_CLIENTS test passed")


def test_oversized_single_frame_is_rejected_without_crashing():
    with wstest.daemon() as (port, state, ctx):
        ws = wstest.connect(port, ctx)
        try:
            ws.sendall(wstest.UPGRADE)
            assert b"101 Switching Protocols" in ws.recv(4096)

            # A frame header declaring a payload far larger than the
            # MSG_BUF_SIZE/INPUT_BUF_SIZE limit (src/main.c), followed by
            # enough filler bytes to actually fill the input buffer without
            # ever completing the frame.
            header = bytes([0x81, 0xFF]) + (2_000_000).to_bytes(8, "big") + b"\0\0\0\0"
            filler = b"A" * (1024 * 1024 + 200)
            ws.settimeout(5)
            ws.sendall(header + filler)
            assert ws.recv(4096) == b"", "daemon should close, not hang or crash"
        finally:
            ws.close()

        # The daemon itself must still be alive and serving other clients.
        reply = b""
        probe = wstest.connect(port, ctx)
        try:
            probe.sendall(wstest.DISCOVERY)
            while True:
                chunk = probe.recv(4096)
                if not chunk:
                    break
                reply += chunk
        finally:
            probe.close()
        assert b"200 OK" in reply, "oversized frame from one client killed the daemon"
    print("oversized-frame test passed")


if __name__ == "__main__":
    test_spnav_event_fans_out_to_all_subscribed_clients()
    test_max_clients_is_enforced_and_slots_are_reused()
    test_oversized_single_frame_is_rejected_without_crashing()
    print("robustness tests passed")
