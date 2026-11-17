# SPDX-License-Identifier: LGPL-2.1
"""
One cxl-fmd per host drives the devices (review F13.C6b): each holds an
exclusive flock on its lock file, and a second one with the same lock file
exits with an error before it sends anything. A mock topology takes a lock
only when --lock-file names one, so test suites can run many at once.
"""

import fcntl
import os
import subprocess
import tempfile

import pytest

from conftest import PROJECT_ROOT, _find_free_port, _wait_for_service

MOCK = str(PROJECT_ROOT / "mock" / "single-sld.json")


def start(executable, *args):
    """A daemon serving the mock topology, and its port."""
    port = _find_free_port()
    log = tempfile.TemporaryFile()
    proc = subprocess.Popen([str(executable), "-M", MOCK, "-p", str(port),
                             *args], stdout=log, stderr=subprocess.STDOUT)
    if not _wait_for_service(f"http://localhost:{port}/redfish/v1"):
        proc.kill()
        proc.wait()
        log.seek(0)
        pytest.fail("daemon did not start: " + log.read().decode())
    return proc, port


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def run(executable, *args, env=None):
    return subprocess.run([str(executable), *args], capture_output=True,
                          text=True, timeout=30, env=env)


def test_second_daemon_with_the_same_lock_file_exits(cxl_fmd_executable,
                                                     tmp_path):
    lock = tmp_path / "fmd.lock"
    first, _ = start(cxl_fmd_executable, "--lock-file", str(lock))
    try:
        second = run(cxl_fmd_executable, "-M", MOCK, "-p",
                     str(_find_free_port()), "--lock-file", str(lock))
        assert second.returncode == 1, second
        assert (f"another cxl-fmd (PID {first.pid}) holds the lock file "
                f"{lock}") in second.stderr, second.stderr
        assert first.poll() is None, "the first one stopped"
        # the file stays, and names who holds it
        assert lock.read_text() == f"{first.pid}\n"
    finally:
        stop(first)
    assert lock.exists(), "the lock file was removed"

    # the lock goes with the process that held it
    again, _ = start(cxl_fmd_executable, "--lock-file", str(lock))
    stop(again)


def test_daemons_with_different_lock_files_both_start(cxl_fmd_executable,
                                                      tmp_path):
    a, port_a = start(cxl_fmd_executable, "--lock-file",
                      str(tmp_path / "a.lock"))
    try:
        b, port_b = start(cxl_fmd_executable, "--lock-file",
                          str(tmp_path / "b.lock"))
        try:
            for port in (port_a, port_b):
                assert _wait_for_service(f"http://localhost:{port}/redfish/v1",
                                         timeout=5)
        finally:
            stop(b)
    finally:
        stop(a)


def test_mock_takes_no_lock_unless_named(cxl_fmd_executable, tmp_path):
    """With RUNTIME_DIRECTORY's lock held, a mock topology still starts."""
    with open(tmp_path / "cxl-fmd.lock", "w") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        env = dict(os.environ, RUNTIME_DIRECTORY=str(tmp_path))
        proc = run(cxl_fmd_executable, "-M", MOCK, "-c", env=env)
        assert proc.returncode == 0, proc


def held_default(tmp_path, runtime_directory):
    held = open(tmp_path / "cxl-fmd.lock", "w")
    fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
    return held, dict(os.environ, RUNTIME_DIRECTORY=runtime_directory)


def test_hardware_takes_the_default_lock_before_discovery(cxl_fmd_executable,
                                                          tmp_path):
    """Without --mock or --lock-file the lock is cxl-fmd.lock in the first
    directory of $RUNTIME_DIRECTORY, and it is taken before discovery: with
    it held, nothing is discovered (-c on the kernel's mailboxes)."""
    held, env = held_default(tmp_path, f"{tmp_path}:/elsewhere")
    with held:
        proc = run(cxl_fmd_executable, "-i", "-c", env=env)
    assert proc.returncode == 1, proc
    assert (f"holds the lock file {tmp_path}/cxl-fmd.lock"
            in proc.stderr), proc.stderr
    assert "Discovering" not in proc.stderr + proc.stdout, proc.stderr

    # and once it is let go, the same command takes it
    proc = run(cxl_fmd_executable, "-i", "-c", env=env)
    assert f"holding the lock file {tmp_path}/cxl-fmd.lock" in proc.stderr, \
        proc.stderr


def test_lock_file_option_overrides_the_default(cxl_fmd_executable, tmp_path):
    held, env = held_default(tmp_path, str(tmp_path))
    with held:
        mine = tmp_path / "mine.lock"
        proc = run(cxl_fmd_executable, "-i", "-c", "--lock-file", str(mine),
                   env=env)
    assert f"holding the lock file {mine}" in proc.stderr, proc.stderr
    assert "holds the lock file" not in proc.stderr, proc.stderr


def test_a_lock_file_that_cannot_be_opened_is_an_error(cxl_fmd_executable,
                                                      tmp_path):
    lock = tmp_path / "no" / "such" / "x.lock"
    proc = run(cxl_fmd_executable, "-M", MOCK, "-c", "--lock-file", str(lock))
    assert proc.returncode == 1, proc
    assert (f"cannot open the lock file {lock}" in proc.stderr
            and "give --lock-file PATH" in proc.stderr), proc.stderr


def test_an_empty_lock_file_is_an_error(cxl_fmd_executable):
    proc = run(cxl_fmd_executable, "-M", MOCK, "-c", "--lock-file", "")
    assert proc.returncode == 1, proc
    assert "Invalid lock file: an empty path" in proc.stderr, proc.stderr


def test_a_directory_that_cannot_be_made_is_an_error(cxl_fmd_executable,
                                                     tmp_path):
    missing = tmp_path / "no" / "such"
    env = dict(os.environ, RUNTIME_DIRECTORY=str(missing))
    proc = run(cxl_fmd_executable, "-i", "-c", env=env)
    assert proc.returncode == 1, proc
    assert (f"cannot create {missing} for the lock file" in proc.stderr
            and "give --lock-file PATH" in proc.stderr), proc.stderr


@pytest.mark.skipif(os.access("/run", os.W_OK) or
                    os.path.exists("/run/cxl-fmd"),
                    reason="/run/cxl-fmd can be made or is there")
def test_run_cxl_fmd_is_the_default(cxl_fmd_executable):
    env = {k: v for k, v in os.environ.items() if k != "RUNTIME_DIRECTORY"}
    proc = run(cxl_fmd_executable, "-i", "-c", env=env)
    assert proc.returncode == 1, proc
    assert "cannot create /run/cxl-fmd for the lock file" in proc.stderr, \
        proc.stderr
