"""Black-box tests for argument parsing and --doctor in src/main.c.

Uses tests/test_daemon rather than the real binary: it's built from the
exact same main.o (see Makefile.in's test_daemon rule, which only swaps in
tests/stub_spnav.c for src/spnav_bridge.c), so CLI parsing behaves
identically, but doctor_check_spacenavd()'s sb_open() call always succeeds
against the stub - making that check's outcome deterministic regardless of
whether a real spacenavd happens to be running on the machine executing
this test (e.g. in CI, where it normally isn't).
"""
import os
import pathlib
import subprocess
import tempfile

binary = pathlib.Path(__file__).resolve().parent / "test_daemon"


def run(*args, env=None):
    return subprocess.run([str(binary), *args], capture_output=True, text=True,
                           timeout=5, env=env)


def test_help():
    r = run("--help")
    assert r.returncode == 0
    assert "usage:" in r.stdout
    assert "--sensitivity" in r.stdout
    assert "--doctor" in r.stdout


def test_unknown_argument_is_rejected():
    r = run("--frobnicate")
    assert r.returncode == 1
    assert "unknown argument" in r.stderr


def test_port_validation():
    for bad in ("0", "65536", "-1", "abc", "8181x"):
        r = run("--port", bad)
        assert r.returncode == 1, bad
        assert "--port must be a number" in r.stderr, bad
    for good in ("1", "8181", "65535"):
        # Valid port alone still tries to reach spacenavd and bind, which
        # exits differently - only argument *parsing* is under test here,
        # so just confirm it gets past the parse-error path.
        r = run("--port", good, "--help")
        assert r.returncode == 0, good


def test_sensitivity_validation():
    for bad in ("0", "0.001", "10.1", "20", "abc", ""):
        r = run("--sensitivity", bad)
        assert r.returncode == 1, bad
        assert "--sensitivity must be a number" in r.stderr, bad
    for good in ("0.01", "0.35", "1", "10"):
        r = run("--sensitivity", good, "--help")
        assert r.returncode == 0, good


def test_doctor_runs_to_completion_and_reports_spacenavd_ok():
    with tempfile.TemporaryDirectory(prefix="spnav-test-") as state, \
         tempfile.TemporaryDirectory(prefix="spnav-test-home-") as home:
        # doctor_check_cert() checks $XDG_STATE_HOME/$HOME before --state-dir
        # (see src/main.c) - isolate all three from whatever this machine's
        # real user happens to have installed, so the "no certificate yet"
        # case is deterministic instead of depending on the test runner.
        env = dict(os.environ)
        env["HOME"] = home
        env.pop("XDG_STATE_HOME", None)
        env.pop("XDG_DATA_HOME", None)

        r = run("--state-dir", state, "--doctor", env=env)
        assert r.returncode in (0, 1), r.stderr  # never crashes/signals
        assert "spnav-onshape-bridge --doctor" in r.stdout
        # sb_open() against the stub always succeeds, unlike a real
        # spacenavd that may or may not be running on the test machine.
        assert "spacenavd connection" in r.stdout
        assert "OK" in r.stdout
        assert "certificate file" in r.stdout
        assert "MISSING" in r.stdout  # nothing has generated one in `state`/`home` yet


if __name__ == "__main__":
    test_help()
    test_unknown_argument_is_rejected()
    test_port_validation()
    test_sensitivity_validation()
    test_doctor_runs_to_completion_and_reports_spacenavd_ok()
    print("CLI tests passed")
