# SPDX-License-Identifier: LGPL-2.1
"""
The mock switch's bind and unbind outcomes, and binds made outside cxl-fmd.

"bind_result" and "unbind_result" of a mock switch say what it answers Bind
vPPB (5201h) and Unbind vPPB (5202h) with. Busy and Unsupported come at
once; Invalid Input and Internal Error come at the end of a background
operation, as the return code of Background Operation Status, with the vPPB
as it was and a Binding Change in the Warning log: "Failed bind and unbind
operations generate events to the Warning Event Log" (CXL r4.0 Table 7-80).

The top-level "external" list makes one bind or unbind each time the daemon
is sent SIGUSR2, with no task: another FM's, or one through another CCI.
With emit_event the switch records it in its Informational log; without,
it records nothing, and only the vPPB shows it.

Each topology here is mock/background-bind.json with one change, written to
a temporary file.
"""

import copy
import json
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import pytest
import requests

from conftest import _find_free_port, _wait_for_service
from test_orchestration import bindings, bound, chunk_of
from test_tasks import post_connection, wait_for

ROOT = Path(__file__).resolve().parent.parent
BASE = json.loads((ROOT / "mock" / "background-bind.json").read_text())
SWITCH = "switch_0"
CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"
SWITCH_LOG = f"/redfish/v1/Chassis/{SWITCH}/LogServices/EventLog/Entries"


class Client:
    def __init__(self, url):
        self.base_url = url

    def get(self, path):
        r = requests.get(self.base_url + path, timeout=30)
        return r.status_code, (r.json() if r.content else None)


@pytest.fixture
def daemon(cxl_fmd_executable, tmp_path):
    """start(change) runs cxl-fmd on background-bind.json as change() leaves
    it, and returns (process, client)."""
    started = []

    def start(change):
        topology = copy.deepcopy(BASE)
        change(topology)
        path = tmp_path / "topology.json"
        path.write_text(json.dumps(topology))
        port = _find_free_port()
        log = tempfile.TemporaryFile()
        proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(path),
                                 "--port", str(port)],
                                stdout=log, stderr=subprocess.STDOUT)
        started.append((proc, log))
        if not _wait_for_service(f"http://localhost:{port}/redfish/v1"):
            log.seek(0)
            pytest.fail("cxl-fmd did not start:\n" + log.read().decode(errors="replace"))
        return proc, Client(f"http://localhost:{port}")

    yield start

    for proc, log in started:
        died = proc.poll()
        proc.terminate()
        rc = proc.wait(timeout=10)
        log.seek(0)
        text = log.read().decode(errors="replace")
        log.close()
        assert died is None, f"cxl-fmd exited during the test:\n{text[-4000:]}"
        assert rc == 0, f"cxl-fmd did not shut down cleanly:\n{text[-4000:]}"


def switch(t):
    return t["switches"][0]


def results(bind="success", unbind="success", time_ms=200):
    def change(t):
        switch(t)["bind_result"] = bind
        switch(t)["unbind_result"] = unbind
        switch(t)["bind_time_ms"] = time_ms
    return change


# cxl-fmd's Resolution of a bind or unbind the switch answered Invalid Input
REFUSED = ("Read the switch's ports and Virtual CXL Switches, and resubmit "
           "the request if it still applies: the switch refused the command "
           "as Invalid Input.")


def answered_as_at_once(done, status, message, resolution):
    """
    The monitor @done of a task that is over answers as the request would
    have, had the switch answered at once ("as if it had completed
    synchronously", DSP0266 1.25.0 Section 12.2): @status, with the error
    body the POST or DELETE gives (src/redfish/handlers.c
    fmd_redfish_vppb_failure()).
    """
    assert done.status_code == status, done.text
    error = done.json()["error"]
    assert error["message"] == message
    info = error["@Message.ExtendedInfo"]
    assert [m["MessageId"] for m in info] == ["Base.1.24.GeneralError"]
    assert info[0]["Resolution"] == resolution


def warnings(client):
    status, data = client.get(SWITCH_LOG)
    assert status == 200
    return [e for e in data["Members"] if e["CXLEntryType"] == "Warning"]


@pytest.mark.parametrize("time_ms", [0, 200])
def test_bind_that_fails_in_the_background(daemon, time_ms):
    """Internal Error, at the end of the background operation: the task ends
    in Exception with that return code, the vPPB is unbound, the switch has
    a Binding Change in its Warning log, and the bind can be asked again."""
    _, client = daemon(results(bind="internal_error", time_ms=time_ms))
    chunk = chunk_of(client, "sld_0")

    for attempt in (1, 2):
        response = post_connection(client.base_url, "switch_0_host0", "sld_0", chunk)
        assert response.status_code == 202, response.text
        done = wait_for(client.base_url, response.headers["Location"])
        answered_as_at_once(done, 500, "Failed to create connection",
                            "Read the resource to see what the device did "
                            "before the request is resubmitted: the device "
                            "failed the command.")
        status, task = client.get(response.json()["@odata.id"])
        assert task["TaskState"] == "Exception"
        assert "internal device error" in task["Messages"][0]["Message"]
        assert bound(bindings(client, SWITCH)) == {}
        assert len(warnings(client)) == attempt
        assert warnings(client)[-1]["Message"] == "vPPB 1 of VCS 0: binding change"
        # nothing was made: no Connection, and the task created none
        assert "Links" not in task
        status, conns = client.get(CONNECTIONS)
        assert conns["Members"] == []


def test_unbind_that_fails_in_the_background(daemon):
    """Invalid Input, at the end of an unbind: the vPPB is bound as before."""
    _, client = daemon(results(unbind="invalid_input"))
    chunk = chunk_of(client, "sld_0")

    response = post_connection(client.base_url, "switch_0_host0", "sld_0", chunk)
    assert response.status_code == 202
    done = wait_for(client.base_url, response.headers["Location"])
    assert done.status_code == 201
    uri = done.json()["@odata.id"]
    before = bound(bindings(client, SWITCH))
    assert list(before.values()) == [("BoundPhysicalPort", 2, None)]

    response = requests.delete(client.base_url + uri, timeout=30)
    assert response.status_code == 202
    done = wait_for(client.base_url, response.headers["Location"])
    # 500 before review F18: a DELETE the switch refused at once is 400
    answered_as_at_once(done, 400, "The switch refused the unbind as Invalid Input",
                        REFUSED)
    status, task = client.get(response.json()["@odata.id"])
    assert "cmd input was invalid" in task["Messages"][0]["Message"]
    assert bound(bindings(client, SWITCH)) == before
    assert client.get(uri)[0] == 200
    assert len(warnings(client)) == 1


def test_bind_refused_in_the_background(daemon):
    """Invalid Input, at the end of a bind: the monitor answers 400, as a
    POST the switch refused at once is (it was 500), and nothing is bound."""
    _, client = daemon(results(bind="invalid_input"))
    chunk = chunk_of(client, "sld_0")
    response = post_connection(client.base_url, "switch_0_host0", "sld_0", chunk)
    assert response.status_code == 202, response.text
    done = wait_for(client.base_url, response.headers["Location"])
    answered_as_at_once(done, 400, "The switch refused the bind as Invalid Input",
                        REFUSED)
    status, task = client.get(response.json()["@odata.id"])
    assert task["TaskState"] == "Exception"
    assert "cmd input was invalid" in task["Messages"][0]["Message"]
    assert bound(bindings(client, SWITCH)) == {}
    status, conns = client.get(CONNECTIONS)
    assert conns["Members"] == []


@pytest.mark.parametrize("result,http", [("busy", 503), ("unsupported", 501)])
def test_bind_refused_at_once(daemon, result, http):
    """Busy and Unsupported come at once, as from a switch: no task, and the
    vPPB is never Busy. Bind vPPB is optional (Table 7-31): a switch that
    does not support it is answered 501 (cxl-fmd's choice)."""
    _, client = daemon(results(bind=result))
    chunk = chunk_of(client, "sld_0")
    response = post_connection(client.base_url, "switch_0_host0", "sld_0", chunk)
    assert response.status_code == http, response.text
    assert all(v[0] == "Unbound" for k, v in bindings(client, SWITCH).items()
               if k[1] != "0")
    assert warnings(client) == []


@pytest.mark.parametrize("result,http", [("busy", 503), ("unsupported", 501)])
def test_unbind_refused_at_once(daemon, result, http):
    """The Connection stays, Enabled, and no task is left."""
    _, client = daemon(results(unbind=result, time_ms=0))
    chunk = chunk_of(client, "sld_0")
    response = post_connection(client.base_url, "switch_0_host0", "sld_0", chunk)
    assert response.status_code == 201
    uri = response.json()["@odata.id"]
    assert requests.delete(client.base_url + uri, timeout=30).status_code == http
    assert list(bound(bindings(client, SWITCH)).values()) == [("BoundPhysicalPort", 2, None)]
    status, conn = client.get(uri)
    assert status == 200 and conn["Status"]["State"] == "Enabled"
    # and it can be asked again
    assert requests.delete(client.base_url + uri, timeout=30).status_code == http


def external(t):
    t["external"] = [
        {"switch": SWITCH, "vcs": 1, "vppb": 2, "action": "bind", "port": 3},
        {"switch": SWITCH, "vcs": 1, "vppb": 3, "action": "bind", "port": 4,
         "ld": 1, "emit_event": False},
        {"switch": SWITCH, "vcs": 1, "vppb": 2, "action": "unbind",
         "emit_event": False},
    ]


def informational(client):
    status, data = client.get(SWITCH_LOG)
    assert status == 200
    return [e for e in data["Members"] if e["CXLEntryType"] == "Informational"]


def send(proc, client, key, want):
    proc.send_signal(signal.SIGUSR2)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if bindings(client, SWITCH)[key] == want:
            return
        time.sleep(0.1)
    raise AssertionError(f"vPPB {key} is {bindings(client, SWITCH)[key]}, not {want}")


def test_external_binds_and_unbinds(daemon):
    proc, client = daemon(external)
    assert bound(bindings(client, SWITCH)) == {}

    # a bind with an event: the vPPB, and the switch's record of it
    send(proc, client, ("1", "2"), ("BoundPhysicalPort", 3, None))
    records = informational(client)
    assert [e["Message"] for e in records] == ["vPPB 2 of VCS 1: binding change"]

    # an LD bound with no event: only the vPPB shows it
    send(proc, client, ("1", "3"), ("BoundLD", 4, 1))
    assert informational(client) == records

    # and unbound the same way
    send(proc, client, ("1", "2"), ("Unbound", None, None))
    assert informational(client) == records
    assert list(bound(bindings(client, SWITCH)).values()) == [("BoundLD", 4, 1)]


def connections(client):
    status, data = client.get(CONNECTIONS)
    assert status == 200
    return sorted(m["@odata.id"].rsplit("/", 1)[1] for m in data["Members"])


def test_external_binds_are_connections(daemon):
    """A binding made outside cxl-fmd is a Connection of the VCS's host, and
    goes when it is unbound, with or without an event. The mock switch's
    table is what cxl-fmd reads, so one made with no event is seen at once
    too: the case where only --rescan would see it is not one the mock
    can make."""
    proc, client = daemon(external)
    assert connections(client) == []

    send(proc, client, ("1", "2"), ("BoundPhysicalPort", 3, None))
    assert connections(client) == ["switch_0_vcs1_vppb2_p3"]
    status, conn = client.get(CONNECTIONS + "/switch_0_vcs1_vppb2_p3")
    assert conn["Links"]["InitiatorEndpoints"] == [
        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host1"}]
    assert conn["Links"]["TargetEndpoints"] == [
        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_1"}]
    status, domains = client.get("/redfish/v1/Systems/switch_0_host1/MemoryDomains")
    assert domains["Members"] == [
        {"@odata.id": "/redfish/v1/Systems/switch_0_host1/MemoryDomains/switch_0_vcs1_vppb2_p3"}]

    send(proc, client, ("1", "3"), ("BoundLD", 4, 1))
    assert connections(client) == ["switch_0_vcs1_vppb2_p3", "switch_0_vcs1_vppb3_p4_ld1"]

    send(proc, client, ("1", "2"), ("Unbound", None, None))
    assert connections(client) == ["switch_0_vcs1_vppb3_p4_ld1"]
    assert client.get(CONNECTIONS + "/switch_0_vcs1_vppb2_p3")[0] == 404


def rebind(t):
    t["external"] = [
        {"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "bind", "port": 2},
        {"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "unbind"},
        {"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "bind", "port": 3},
    ]


def test_stale_delete_404(daemon):
    """A Connection's ID names its vPPB and what the vPPB holds. Once the
    vPPB holds something else, the ID is not found, and a DELETE of it
    unbinds nothing."""
    proc, client = daemon(rebind)
    send(proc, client, ("0", "1"), ("BoundPhysicalPort", 2, None))
    old = CONNECTIONS + "/switch_0_vcs0_vppb1_p2"
    assert client.get(old)[0] == 200

    send(proc, client, ("0", "1"), ("Unbound", None, None))
    send(proc, client, ("0", "1"), ("BoundPhysicalPort", 3, None))
    assert client.get(old)[0] == 404
    assert requests.delete(client.base_url + old, timeout=30).status_code == 404
    assert bindings(client, SWITCH)[("0", "1")] == ("BoundPhysicalPort", 3, None)
    assert connections(client) == ["switch_0_vcs0_vppb1_p3"]


@pytest.mark.parametrize("entry,message", [
    ({"switch": "nope", "vcs": 0, "vppb": 1, "action": "bind", "port": 2},
     "external 0: no switch nope"),
    ({"switch": SWITCH, "vcs": 0, "vppb": 0, "action": "unbind"},
     "external 0: vPPB 0 is the upstream port's"),
    ({"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "bind", "port": 4},
     "external 0: port 4 has an MLD"),
    ({"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "bind", "port": 2, "ld": 0},
     "external 0: port 2 has no MLD"),
    ({"switch": SWITCH, "vcs": 0, "vppb": 1, "action": "move"},
     "external 0: action is not bind or unbind"),
])
def test_external_entry_is_checked(cxl_fmd_executable, tmp_path, entry, message):
    t = copy.deepcopy(BASE)
    t["external"] = [entry]
    path = tmp_path / "topology.json"
    path.write_text(json.dumps(t))
    proc = subprocess.run([str(cxl_fmd_executable), "--mock", str(path), "--check"],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode != 0
    assert message in proc.stdout + proc.stderr


def test_bind_result_is_checked(cxl_fmd_executable, tmp_path):
    t = copy.deepcopy(BASE)
    switch(t)["bind_result"] = "maybe"
    path = tmp_path / "topology.json"
    path.write_text(json.dumps(t))
    proc = subprocess.run([str(cxl_fmd_executable), "--mock", str(path), "--check"],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode != 0
    assert 'bind_result "maybe" is not success' in proc.stdout + proc.stderr
