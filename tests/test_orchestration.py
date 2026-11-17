# SPDX-License-Identifier: LGPL-2.1
"""
Provisioning memory on the larger mock topologies, as an orchestrator does.

What may be given to whom is the CXL specification's (r4.0): an SLD port is
bound to only one VCS (Section 7.2.3.1), each LD of an MLD is bound to one
host or is unbound (Section 7.3.4), and "the Fabric Manager can bind any
unused physical port to the unbound vPPB".
"""

import pytest

CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"


def chunk_of(api_client, dev, ld=None):
    status, data = api_client.get(f"/redfish/v1/Chassis/{dev}/MemoryDomains")
    assert status == 200
    for domain in data["Members"]:
        status, chunks = api_client.get(domain["@odata.id"] + "/MemoryChunks")
        assert status == 200
        for member in chunks["Members"]:
            if ld is None or f"_ld{ld}_" in member["@odata.id"]:
                return member["@odata.id"]
    raise AssertionError(f"no memory chunk on {dev} (LD {ld})")


def connect(api_client, host, dev, ld=None):
    status, data, _ = api_client.post(CONNECTIONS, {
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host}"}],
            "TargetEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{dev}"}]},
        "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_of(api_client, dev, ld)}}]})
    return status, data


def bindings(api_client, switch):
    """{(vcs, vppb): (status, port, ld)} for every vPPB of a switch."""
    out = {}
    base = f"/redfish/v1/Fabrics/CXL/Switches/{switch}/VCSs"
    status, vcss = api_client.get(base)
    assert status == 200
    for vcs in vcss["Members"]:
        status, vppbs = api_client.get(vcs["@odata.id"] + "/VPPBs")
        assert status == 200
        for vppb in vppbs["Members"]:
            status, data = api_client.get(vppb["@odata.id"])
            assert status == 200
            key = (vcs["@odata.id"].rsplit("/", 1)[1], vppb["@odata.id"].rsplit("/", 1)[1])
            out[key] = (data["BindingStatus"], data.get("BoundPortId"), data.get("BoundLDId"))
    return out


def bound(b):
    """The bindings to devices: not unbound, and not vPPB 0, the upstream port."""
    return {k: v for k, v in b.items() if v[0] != "Unbound" and k[1] != "0"}


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestFMBootsFirst:
    """Section 7.2.1.2: every downstream vPPB starts unbound, and the FM
    gives the hosts their devices."""

    def test_nothing_is_bound_to_begin_with(self, api_client):
        assert bound(bindings(api_client, "switch_0")) == {}

    def test_bind_an_sld_to_a_host(self, api_client):
        status, _ = connect(api_client, "switch_0_host0", "sld_0")
        assert status == 201
        now = bound(bindings(api_client, "switch_0"))
        assert list(now.values()) == [("BoundPhysicalPort", 2, None)]
        assert all(vcs == "0" for vcs, _ in now)

    def test_the_hosts_view_of_its_memory(self, api_client):
        """The host's MemoryDomain and MemoryChunks for a connection have the
        properties their schemas define (MemoryDomain v1.5, MemoryChunks
        v1.6): the domain's size is MemorySizeMiB."""
        status, conn = connect(api_client, "switch_0_host0", "sld_0")
        assert status == 201
        base = "/redfish/v1/Systems/switch_0_host0/MemoryDomains"
        status, domains = api_client.get(base)
        assert status == 200 and len(domains["Members"]) == 1
        status, domain = api_client.get(domains["Members"][0]["@odata.id"])
        assert status == 200
        assert domain["MemorySizeMiB"] == 16384 and "MemorySize" not in domain
        status, chunks = api_client.get(domain["MemoryChunks"]["@odata.id"])
        status, chunk = api_client.get(chunks["Members"][0]["@odata.id"])
        assert status == 200
        assert chunk["MemoryChunkSizeMiB"] == 16384 and "AccessState" not in chunk

    def test_an_sld_goes_to_one_host(self, api_client):
        assert connect(api_client, "switch_0_host0", "sld_0")[0] == 201
        before = bindings(api_client, "switch_0")
        assert connect(api_client, "switch_0_host1", "sld_0")[0] == 409
        assert bindings(api_client, "switch_0") == before

    def test_the_lds_of_an_mld_go_to_different_hosts(self, api_client):
        assert connect(api_client, "switch_0_host0", "mld_0", 0)[0] == 201
        assert connect(api_client, "switch_0_host1", "mld_0", 1)[0] == 201
        now = bound(bindings(api_client, "switch_0"))
        assert sorted((vcs, v) for (vcs, _), v in now.items()) == [
            ("0", ("BoundLD", 4, 0)), ("1", ("BoundLD", 4, 1))]
        # LD 0 is host0's now
        assert connect(api_client, "switch_0_host1", "mld_0", 0)[0] == 409

    def test_no_unbound_vppb_is_a_conflict(self, api_client):
        """VCS 0 has three downstream vPPBs. Once they are bound, a fourth
        device cannot be given to host0: that is a conflict with the state
        of the switch (409, cxl-fmd's choice), not a server error."""
        for dev, ld in (("sld_0", None), ("sld_1", None), ("mld_0", 0)):
            assert connect(api_client, "switch_0_host0", dev, ld)[0] == 201
        before = bindings(api_client, "switch_0")
        assert connect(api_client, "switch_0_host0", "mld_0", 1)[0] == 409
        assert bindings(api_client, "switch_0") == before
        # host1's VCS has vPPBs of its own
        assert connect(api_client, "switch_0_host1", "mld_0", 1)[0] == 201

    def test_an_mld_chunk_must_name_an_ld(self, api_client):
        """An LD of an MLD is bound by its LD-ID (Table 7-35), taken from the
        chunk. A chunk that names no LD of mld_0 is a bad request (400, an
        error of the client's, cxl-fmd's choice), and nothing is bound."""
        before = bindings(api_client, "switch_0")
        status, connections = api_client.get(CONNECTIONS)
        assert status == 200
        status, _, _ = api_client.post(CONNECTIONS, {
            "ConnectionType": "Memory",
            "Links": {
                "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"}],
                "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld_0"}]},
            "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_of(api_client, "sld_0")}}]})
        assert status == 400
        assert bindings(api_client, "switch_0") == before
        assert api_client.get(CONNECTIONS) == (200, connections)

    def test_releasing_makes_it_available_again(self, api_client):
        status, conn = connect(api_client, "switch_0_host0", "sld_0")
        assert status == 201
        assert api_client.delete(conn["@odata.id"]) == 204
        assert bound(bindings(api_client, "switch_0")) == {}
        assert connect(api_client, "switch_0_host1", "sld_0")[0] == 201


@pytest.mark.mock_topology("mock/large-fabric.json")
class TestLargeFabric:

    def test_inventory(self, api_client):
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200 and data["Members@odata.count"] == 2
        a = bindings(api_client, "switch_a")
        b = bindings(api_client, "switch_b")
        assert len({vcs for vcs, _ in a}) == 4 and len({vcs for vcs, _ in b}) == 2
        # switch_a: each host has an SLD and two LDs of the MLD on port 8
        assert len(bound(a)) == 12
        assert len(bound(b)) == 3

    def test_give_a_host_an_unbound_ld(self, api_client):
        """LDs 8 to 15 of a_mld_0 are FM-owned."""
        assert connect(api_client, "switch_a_host2", "a_mld_0", 8)[0] == 201
        assert ("BoundLD", 8, 8) in bound(bindings(api_client, "switch_a")).values()

    def test_an_ld_of_another_host_is_refused(self, api_client):
        """LD 0 of a_mld_0 is bound to host 0."""
        before = bindings(api_client, "switch_a")
        assert connect(api_client, "switch_a_host3", "a_mld_0", 0)[0] == 409
        assert bindings(api_client, "switch_a") == before

    def test_a_host_is_given_only_what_is_on_its_switch(self, api_client):
        """A host reaches devices through the switch its root port is on. A
        VCS of switch_b cannot be bound to a port of switch_a."""
        a, b = bindings(api_client, "switch_a"), bindings(api_client, "switch_b")
        status, _ = connect(api_client, "switch_b_host0", "a_mld_1", 0)
        assert status in (400, 409)
        assert bindings(api_client, "switch_a") == a
        assert bindings(api_client, "switch_b") == b


@pytest.mark.mock_topology("mock/switch-dcd.json")
class TestSwitchDCD:

    DEV = "/redfish/v1/Chassis/dcd_mld_0/PCIeDevices/dcd_mld_0"

    def test_each_host_sees_its_extents(self, api_client):
        """Host 0 is LD 0 and host 1 is LD 1: the Host ID of an LD-FAM device
        is the LD-ID of the host interface. The extent in the Sharable region
        is held by both, the other by host 1 alone."""
        counts = {}
        for ld in (0, 1):
            status, region = api_client.get(
                f"{self.DEV}/CXLLogicalDevices/{ld}/MemoryRegions/0")
            assert status == 200
            assert region["ShareableRegion"] is True
            counts[ld] = region["ExtentsCount"]
        assert counts == {0: 1, 1: 1}
        status, region = api_client.get(f"{self.DEV}/CXLLogicalDevices/1/MemoryRegions/1")
        assert status == 200 and region["ShareableRegion"] is False
        assert region["ExtentsCount"] == 1
        status, region = api_client.get(f"{self.DEV}/CXLLogicalDevices/0/MemoryRegions/1")
        assert status == 200 and region["ExtentsCount"] == 0

    def test_the_lds_are_bound_to_different_hosts(self, api_client):
        now = bound(bindings(api_client, "switch_0"))
        assert now[("0", "1")] == ("BoundLD", 2, 0)
        assert now[("1", "1")] == ("BoundLD", 2, 1)


def test_restart_ids_stable(cxl_fmd_executable, tmp_path):
    """A Connection's ID is computed from the binding: the switch, VCS,
    vPPB, port and LD. A daemon started again on the switch as the bind left
    it serves the same Connection with the same ID, though it did not make
    it, and the same MemoryDomain of the host. Its Oem.Cxl.Origin is
    "Service" from the daemon that bound it and "Discovered" from the next:
    who made a binding is not kept across a restart."""
    import json
    import subprocess
    import tempfile

    from conftest import PROJECT_ROOT, _find_free_port, _wait_for_service
    from conftest import RedfishClient

    def run(path):
        port = _find_free_port()
        log = tempfile.TemporaryFile()
        proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(path),
                                 "--port", str(port)],
                                stdout=log, stderr=subprocess.STDOUT)
        assert _wait_for_service(f"http://localhost:{port}/redfish/v1")
        return proc, RedfishClient(f"http://localhost:{port}")

    def stop(proc):
        proc.terminate()
        assert proc.wait(timeout=10) == 0

    source = PROJECT_ROOT / "mock" / "fm-boots-first.json"
    proc, client = run(source)
    try:
        status, first = connect(client, "switch_0_host1", "mld_0", 1)
        assert status == 201
        assert first["Oem"] == {"Cxl": {"@odata.type": "#Cxl.v1_0_0.Connection", "Origin": "Service"}}
        table = bindings(client, "switch_0")
        domains = client.get("/redfish/v1/Systems/switch_0_host1/MemoryDomains")[1]["Members"]
    finally:
        stop(proc)

    # the topology with the binding in it, as the switch now has it
    topology = json.loads(source.read_text())
    for vcs in topology["switches"][0]["vcs"]:
        for vppb in vcs["vppbs"]:
            status, port, ld = table[(str(vcs["vcs_id"]), str(vppb["vppb_id"]))]
            if status == "BoundLD":
                vppb.update(binding_status="bound_ld", bound_port_id=port, bound_ld_id=ld)
    path = tmp_path / "after-bind.json"
    path.write_text(json.dumps(topology))

    proc, client = run(path)
    try:
        status, again = client.get(first["@odata.id"])
        assert status == 200
        assert again["Id"] == first["Id"] == "switch_0_vcs1_vppb1_p4_ld1"
        assert again["Links"] == first["Links"]
        assert again["Oem"] == {"Cxl": {"@odata.type": "#Cxl.v1_0_0.Connection", "Origin": "Discovered"}}
        assert client.get(CONNECTIONS)[1]["Members"] == [{"@odata.id": first["@odata.id"]}]
        assert client.get("/redfish/v1/Systems/switch_0_host1/MemoryDomains")[1]["Members"] == domains
    finally:
        stop(proc)
