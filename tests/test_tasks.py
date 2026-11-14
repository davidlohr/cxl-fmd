# SPDX-License-Identifier: LGPL-2.1
"""
A bind or unbind that the switch carries out in the background.

"All binding operations are executed as background commands" (CXL r4.0
Section 7.6.7.2.2): the switch answers Background Command Started, and how
the operation ended is in Background Operation Status. The request that
started it is answered with 202 Accepted and a task (DSP0266, asynchronous
operations); the task monitor answers 202 while it runs and then with what
the request would have been answered with.

mock/background-bind.json has a switch that takes 1.5 s over each: long enough that a request sent
while one runs is not late on a loaded machine, as under a sanitizer.
"""

import time

import pytest
import requests

from test_orchestration import bindings, bound, chunk_of

CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"
SWITCH = "switch_0"


def post_connection(base, host, dev, chunk):
    return requests.post(base + CONNECTIONS, timeout=30, json={
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host}"}],
            "TargetEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{dev}"}]},
        "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk}}]})


def made(api_client):
    """The connections that were asked for. The collection also lists one per
    device on a switch, whether or not any host has it."""
    status, data = api_client.get(CONNECTIONS)
    assert status == 200
    return [m["@odata.id"] for m in data["Members"]
            if m["@odata.id"].rsplit("/", 1)[1].startswith("conn")]


def wait_for(base, monitor, timeout=10):
    """Poll a task monitor until it stops answering 202."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = requests.get(base + monitor, timeout=30)
        if response.status_code != 202:
            return response
        assert response.json()["TaskState"] == "Running"
        time.sleep(0.05)
    pytest.fail("the task never finished")


@pytest.mark.mock_topology("mock/background-bind.json")
class TestBackgroundBind:

    def test_bind_is_accepted_then_completes(self, api_client):
        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")

        response = post_connection(base, "switch_0_host0", "sld_0", chunk)
        assert response.status_code == 202
        task = response.json()
        monitor = response.headers["Location"]
        assert monitor == task["TaskMonitor"]
        assert task["TaskState"] == "Running"
        assert task["Payload"] == {"TargetUri": CONNECTIONS, "HttpOperation": "POST"}
        # not over: neither of these is set until it is
        assert "TaskStatus" not in task and "EndTime" not in task

        # the switch is binding: the vPPB is neither unbound nor bound
        assert [v[0] for v in bindings(api_client, SWITCH).values()].count("Busy") == 1

        done = wait_for(base, monitor)
        assert done.status_code == 201
        connection = done.json()
        assert done.headers["Location"] == connection["@odata.id"]
        assert connection["Status"]["State"] == "Enabled"
        assert list(bound(bindings(api_client, SWITCH)).values()) == [
            ("BoundPhysicalPort", 2, None)]

        status, task = api_client.get(task["@odata.id"])
        assert status == 200
        assert task["TaskState"] == "Completed"
        assert task["TaskStatus"] == "OK"
        assert task["PercentComplete"] == 100
        assert task["Links"]["CreatedResources"] == [{"@odata.id": connection["@odata.id"]}]
        assert "EndTime" in task

    def test_connection_is_not_one_until_the_bind_is_over(self, api_client):
        base = api_client.base_url
        response = post_connection(base, "switch_0_host0", "sld_0",
                                   chunk_of(api_client, "sld_0"))
        assert response.status_code == 202

        connections = made(api_client)
        assert len(connections) == 1
        status, connection = api_client.get(connections[0])
        assert status == 200
        assert connection["Status"]["State"] == "Starting"
        # and it cannot be removed while the switch is working on it
        assert api_client.delete(connection["@odata.id"]) == 503

        assert wait_for(base, response.headers["Location"]).status_code == 201

    def test_one_operation_at_a_time(self, api_client):
        """A component runs one background command at a time: a second bind
        is told to try again, and nothing is left behind by it."""
        base = api_client.base_url
        first = post_connection(base, "switch_0_host0", "sld_0",
                                chunk_of(api_client, "sld_0"))
        assert first.status_code == 202

        second = post_connection(base, "switch_0_host1", "sld_1",
                                 chunk_of(api_client, "sld_1"))
        assert second.status_code == 503
        assert len(made(api_client)) == 1

        assert wait_for(base, first.headers["Location"]).status_code == 201
        # and once it is over, the second goes ahead
        second = post_connection(base, "switch_0_host1", "sld_1",
                                 chunk_of(api_client, "sld_1"))
        assert second.status_code == 202
        assert wait_for(base, second.headers["Location"]).status_code == 201
        assert len(bound(bindings(api_client, SWITCH))) == 2

    def test_unbind_is_accepted_then_completes(self, api_client):
        base = api_client.base_url
        response = post_connection(base, "switch_0_host0", "sld_0",
                                   chunk_of(api_client, "sld_0"))
        connection = wait_for(base, response.headers["Location"]).json()["@odata.id"]

        response = requests.delete(base + connection, timeout=30)
        assert response.status_code == 202
        task = response.json()
        assert task["Payload"] == {"TargetUri": connection, "HttpOperation": "DELETE"}
        # the host still has the device, and the connection stands
        status, data = api_client.get(connection)
        assert status == 200 and data["Status"]["State"] == "Updating"

        assert wait_for(base, response.headers["Location"]).status_code == 204
        assert api_client.get(connection)[0] == 404
        assert bound(bindings(api_client, SWITCH)) == {}

    def test_task_service(self, api_client):
        base = api_client.base_url
        status, root = api_client.get("/redfish/v1")
        assert root["Tasks"] == {"@odata.id": "/redfish/v1/TaskService"}
        status, service = api_client.get("/redfish/v1/TaskService")
        assert status == 200 and service["ServiceEnabled"] is True
        status, tasks = api_client.get(service["Tasks"]["@odata.id"])
        assert status == 200 and tasks["Members@odata.count"] == 0

        response = post_connection(base, "switch_0_host0", "sld_0",
                                   chunk_of(api_client, "sld_0"))
        wait_for(base, response.headers["Location"])
        status, tasks = api_client.get(service["Tasks"]["@odata.id"])
        assert tasks["Members"] == [{"@odata.id": response.json()["@odata.id"]}]
        assert api_client.get("/redfish/v1/TaskService/Tasks/999")[0] == 404
        assert api_client.get("/redfish/v1/TaskService/TaskMonitors/999")[0] == 404


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestImmediateBind:

    def test_a_bind_that_is_done_at_once_has_no_task(self, api_client):
        """A switch that finishes before it answers: 201, as before."""
        response = post_connection(api_client.base_url, "switch_0_host0", "sld_0",
                                   chunk_of(api_client, "sld_0"))
        assert response.status_code == 201
        status, tasks = api_client.get("/redfish/v1/TaskService/Tasks")
        assert tasks["Members@odata.count"] == 0
