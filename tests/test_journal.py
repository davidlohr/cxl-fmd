# SPDX-License-Identifier: LGPL-2.1
"""
The operation journal (--state-dir, src/journal.c; follow-up review item 6,
F6d). A bind the mock switch carries out in the background is under way
when cxl-fmd is killed; started again on the same state directory, the
run-1 Task URI is the same task, Interrupted, with Oem.Cxl.OperatorDecision,
and its Connection is Starting. A mock topology reloads from its file, so
only the task and its hold on the vPPB carry over, not what the mock
switch did. tests/discovery-tests.c covers each kind of operation against
hardware answers.
"""

import json
import signal
import subprocess
import tempfile

import pytest
import requests

from conftest import PROJECT_ROOT, _find_free_port, _wait_for_service

CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"


@pytest.fixture
def slow_bind(tmp_path):
    """background-bind.json with a switch that takes 10 minutes over a bind"""
    topo = json.loads((PROJECT_ROOT / "mock" / "background-bind.json").read_text())
    topo["switches"][0]["bind_time_ms"] = 600000
    path = tmp_path / "slow-bind.json"
    path.write_text(json.dumps(topo))
    return path


def start(executable, topology, *args):
    port = _find_free_port()
    log = tempfile.TemporaryFile()
    proc = subprocess.Popen([str(executable), "-M", str(topology), "-p", str(port),
                             "-e", "0", "-r", "0", *args],
                            stdout=log, stderr=subprocess.STDOUT)
    base = f"http://localhost:{port}"
    if not _wait_for_service(base + "/redfish/v1"):
        proc.kill()
        proc.wait()
        log.seek(0)
        pytest.fail("daemon did not start: " + log.read().decode())
    return proc, base, log


def stop(proc, sig=signal.SIGTERM):
    proc.send_signal(sig)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def chunk_of(base, dev):
    domains = requests.get(f"{base}/redfish/v1/Chassis/{dev}/MemoryDomains",
                           timeout=30).json()
    for domain in domains["Members"]:
        chunks = requests.get(base + domain["@odata.id"] + "/MemoryChunks",
                              timeout=30).json()
        for member in chunks["Members"]:
            return member["@odata.id"]
    raise AssertionError(f"no memory chunk on {dev}")


def bind(base):
    response = requests.post(base + CONNECTIONS, timeout=30, json={
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [
                {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"}],
            "TargetEndpoints": [
                {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]},
        "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_of(base, "sld_0")}}]})
    assert response.status_code == 202, response.text
    task = response.json()
    assert task["TaskState"] == "Running"
    return task


@pytest.mark.parametrize("sig", [signal.SIGKILL, signal.SIGTERM],
                         ids=["killed", "stopped"])
def test_a_bind_under_way_survives_a_restart(cxl_fmd_executable, slow_bind,
                                             tmp_path, sig):
    state = tmp_path / "state"
    proc, base, _ = start(cxl_fmd_executable, slow_bind, "--state-dir", str(state))
    try:
        task = bind(base)
        connection = task["Links"]["CreatedResources"][0]["@odata.id"]
    finally:
        stop(proc, sig)
    assert (state / "journal.jsonl").exists()

    proc, base, _ = start(cxl_fmd_executable, slow_bind, "--state-dir", str(state))
    try:
        # the run-1 Task URI: the same task, Interrupted, an operator to decide
        response = requests.get(base + task["@odata.id"], timeout=30)
        assert response.status_code == 200, response.text
        again = response.json()
        assert again["Id"] == task["Id"]
        assert again["TaskState"] == "Interrupted"
        assert again["Oem"]["Cxl"]["OperatorDecision"] == "Required"
        assert again["Messages"][0]["Message"].startswith("Outcome unknown")
        assert "TaskStatus" not in again and "EndTime" not in again

        # its monitor answers 202, and takes a DELETE
        monitor = requests.get(base + task["TaskMonitor"], timeout=30)
        assert monitor.status_code == 202
        assert "DELETE" in monitor.headers.get("Allow", "")

        # its hold: the Connection is Starting, the vPPB not given to another
        response = requests.get(base + connection, timeout=30)
        assert response.status_code == 200, response.text
        assert response.json()["Status"]["State"] == "Starting"

        # the operator ends it
        assert requests.delete(base + task["TaskMonitor"],
                               timeout=30).status_code == 204
        assert requests.get(base + task["@odata.id"], timeout=30).status_code == 404
    finally:
        stop(proc)

    # ended: a third start has nothing to make again
    proc, base, _ = start(cxl_fmd_executable, slow_bind, "--state-dir", str(state))
    try:
        assert requests.get(base + task["@odata.id"], timeout=30).status_code == 404
    finally:
        stop(proc)


def test_without_a_state_directory_nothing_survives(cxl_fmd_executable, slow_bind):
    """With --mock and no --state-dir there is no journal (cxl-fmd's choice)."""
    proc, base, _ = start(cxl_fmd_executable, slow_bind)
    try:
        task = bind(base)
    finally:
        stop(proc, signal.SIGKILL)
    proc, base, _ = start(cxl_fmd_executable, slow_bind)
    try:
        assert requests.get(base + task["@odata.id"], timeout=30).status_code == 404
    finally:
        stop(proc)


def test_a_state_directory_is_one_daemons(cxl_fmd_executable, slow_bind, tmp_path):
    state = tmp_path / "state"
    proc, _, _ = start(cxl_fmd_executable, slow_bind, "--state-dir", str(state))
    try:
        second = subprocess.run([str(cxl_fmd_executable), "-M", str(slow_bind), "-p",
                                 str(_find_free_port()), "--state-dir", str(state)],
                                capture_output=True, text=True, timeout=30)
        assert second.returncode == 1, second
        assert "another cxl-fmd uses the state directory" in second.stderr, \
            second.stderr
    finally:
        stop(proc)


def test_a_journal_of_another_version_is_refused(cxl_fmd_executable, slow_bind,
                                                 tmp_path):
    state = tmp_path / "state"
    state.mkdir()
    (state / "journal.jsonl").write_text('{"t":"boot","v":99}\n')
    proc = subprocess.run([str(cxl_fmd_executable), "-M", str(slow_bind), "-p",
                           str(_find_free_port()), "--state-dir", str(state)],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode == 1, proc
    assert "is of version 99, not 1" in proc.stderr, proc.stderr
