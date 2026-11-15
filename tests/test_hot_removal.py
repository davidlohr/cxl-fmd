# SPDX-License-Identifier: LGPL-2.1
"""
Devices pulled out of a switch port, seen through the Redfish API.

mock/hot-removal.json pulls out sld_0, then mld_0, one each time the daemon
is sent SIGUSR1, as a switch reports it (CXL r4.0 Sections 7.6.6.7 and
7.6.6.8): the port links down and reports no device, and cxl-fmd removes
it. Its connections are the switch's to decide. "When a Surprise or Managed
Hot-Removal of an SLD or PCIe device occurs on a bound port, the FM can be
notified but is not involved": the vPPB stays bound, and the connection
stands, Absent, until it is deleted. For an MLD, "the switch will
automatically unbind any existing LD bindings", and its connections go.
"""

import signal
import time

import pytest

from test_orchestration import CONNECTIONS, bindings, bound, connect

pytestmark = pytest.mark.mock_topology("mock/hot-removal.json")


def unplug(daemon, api_client, dev):
    """Pull the next device out, and wait for cxl-fmd to have removed it."""
    daemon["process"].send_signal(signal.SIGUSR1)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if api_client.get(f"/redfish/v1/Chassis/{dev}")[0] == 404:
            return
        time.sleep(0.1)
    raise AssertionError(f"{dev} was not removed")


def gone(api_client, dev):
    """Every resource of a device that was removed answers 404."""
    for uri in (f"/redfish/v1/Chassis/{dev}",
                f"/redfish/v1/Fabrics/CXL/Endpoints/{dev}",
                f"/redfish/v1/Chassis/{dev}/PCIeDevices/{dev}",
                f"/redfish/v1/Chassis/{dev}/Memory/{dev}",
                f"/redfish/v1/Chassis/{dev}/MemoryDomains"):
        status, _ = api_client.get(uri)
        assert status == 404, uri


def port(api_client, number):
    status, data = api_client.get(
        f"/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports/switch_0_dsp{number}")
    assert status == 200
    return data


def test_an_sld_pulled_out_of_a_bound_port(cxl_fmd_daemon, api_client):
    status, conn = connect(api_client, "switch_0_host0", "sld_0")
    assert status == 201
    uri = conn["@odata.id"]

    unplug(cxl_fmd_daemon, api_client, "sld_0")
    gone(api_client, "sld_0")
    assert port(api_client, 2)["LinkStatus"] != "LinkUp"

    # the switch keeps the vPPB bound to the port
    assert ("BoundPhysicalPort", 2, None) in bound(bindings(api_client, "switch_0")).values()

    # and the connection stands, without a device to link to
    status, data = api_client.get(uri)
    assert status == 200
    assert data["Status"]["State"] == "Absent"
    assert "TargetEndpoints" not in data["Links"]
    assert "MemoryChunkInfo" not in data
    status, members = api_client.get(CONNECTIONS)
    assert {"@odata.id": uri} in members["Members"]

    # the host has no memory from it
    status, domains = api_client.get("/redfish/v1/Systems/switch_0_host0/MemoryDomains")
    assert status == 200 and domains["Members"] == []

    # deleting it unbinds the vPPB
    assert api_client.delete(uri) in (200, 204)
    assert api_client.get(uri)[0] == 404
    assert bound(bindings(api_client, "switch_0")) == {}


def test_an_mld_pulled_out(cxl_fmd_daemon, api_client):
    assert connect(api_client, "switch_0_host0", "sld_1")[0] == 201
    status, conn = connect(api_client, "switch_0_host1", "mld_0", ld=1)
    assert status == 201
    uri = conn["@odata.id"]

    unplug(cxl_fmd_daemon, api_client, "sld_0")
    unplug(cxl_fmd_daemon, api_client, "mld_0")
    gone(api_client, "mld_0")
    for ld in range(4):
        status, _ = api_client.get(
            f"/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/{ld}")
        assert status == 404

    # the switch unbound the LD, and the connection is gone with it
    assert api_client.get(uri)[0] == 404
    status, members = api_client.get(CONNECTIONS)
    assert {"@odata.id": uri} not in members["Members"]
    assert all(v[2] is None for v in bound(bindings(api_client, "switch_0")).values())

    # what was not on the ports pulled out is as it was
    assert list(bound(bindings(api_client, "switch_0")).values()) == [
        ("BoundPhysicalPort", 3, None)]
    assert api_client.get("/redfish/v1/Chassis/sld_1")[0] == 200


def test_nothing_left_to_pull_out(cxl_fmd_daemon, api_client):
    unplug(cxl_fmd_daemon, api_client, "sld_0")
    unplug(cxl_fmd_daemon, api_client, "mld_0")
    cxl_fmd_daemon["process"].send_signal(signal.SIGUSR1)
    time.sleep(1.5)
    assert cxl_fmd_daemon["process"].poll() is None
    status, root = api_client.get("/redfish/v1/Chassis")
    assert status == 200
    assert {m["@odata.id"].rsplit("/", 1)[1] for m in root["Members"]} == {
        "switch_0", "sld_1"}
