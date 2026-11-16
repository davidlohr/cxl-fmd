# SPDX-License-Identifier: LGPL-2.1
"""
Tests for CXL memory binding via Connections API.

Validates the Connection lifecycle including vPPB binding/unbinding
for switch-attached topologies per DMTF DSP0288 and OCP CMS specifications.

Memory Model:
- Chunks exist after discovery (created for existing LD allocations)
- Tests use existing chunks for connections
- DELETE chunk frees capacity, POST allocates from remaining
"""

import pytest
import requests


# =============================================================================
# Connection Collection Tests
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestConnectionCollection:
    """Tests for Connections collection access."""

    def test_connections_collection_accessible(self, api_client):
        """Verify Connections collection endpoint is accessible."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Connections")
        assert status == 200
        assert "@odata.type" in data
        assert "ConnectionCollection" in data["@odata.type"]

    def test_connections_collection_has_count(self, api_client):
        """Verify collection has Members@odata.count."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Connections")
        assert status == 200
        assert "Members@odata.count" in data
        assert "Members" in data


# =============================================================================
# Connections are the switch's bindings
# =============================================================================

def chunks_of(api_client, dev, ld=None):
    """Every memory chunk of a device (of LD @ld, if given), by URI."""
    status, data = api_client.get(f"/redfish/v1/Chassis/{dev}/MemoryDomains")
    assert status == 200
    out = []
    for domain in data["Members"]:
        status, chunks = api_client.get(domain["@odata.id"] + "/MemoryChunks")
        assert status == 200
        out += [m["@odata.id"] for m in chunks["Members"]
                if ld is None or f"_ld{ld}_" in m["@odata.id"]]
    return sorted(out)


def post(api_client, host, target, chunks=None, access=None):
    body = {
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host}"}],
            "TargetEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{target}"}]}}
    if chunks is not None:
        body["MemoryChunkInfo"] = [{"MemoryChunk": {"@odata.id": c}} for c in chunks]
        if access is not None:
            for entry in body["MemoryChunkInfo"]:
                entry["AccessCapabilities"] = access
    return api_client.post(CONNECTIONS, body)


CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"
HOST0 = "cxl_switch_0_host0"


@pytest.mark.mock_topology("mock/switch-attached.json")
class TestPreboundConnections:
    """switch-attached.json binds every device to vPPBs of VCS 0 before
    cxl-fmd starts. Each binding is a Connection of host0, though cxl-fmd
    did not make it: the FM "issues Get Virtual CXL Switch Info for each
    enabled VCS to check for all bound vPPBs in the system and create a list
    of binding targets" (CXL r4.0 Section 7.6.6.2)."""

    def test_prebound_sld_connection(self, api_client):
        """vPPB 1 is bound to port 1, dev0: Connection
        cxl_switch_0_vcs0_vppb1_p1, with every chunk of dev0, and a
        MemoryDomain of host0 with the same ID."""
        cid = "cxl_switch_0_vcs0_vppb1_p1"
        status, data = api_client.get(CONNECTIONS)
        assert {"@odata.id": f"{CONNECTIONS}/{cid}"} in data["Members"]
        status, conn = api_client.get(f"{CONNECTIONS}/{cid}")
        assert status == 200
        assert conn["Status"] == {"State": "Enabled", "Health": "OK"}
        assert conn["Links"]["InitiatorEndpoints"] == [
            {"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{HOST0}"}]
        assert conn["Links"]["TargetEndpoints"] == [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0"}]
        assert sorted(i["MemoryChunk"]["@odata.id"] for i in conn["MemoryChunkInfo"]) == \
            chunks_of(api_client, "cxl_switch0_dev0")
        assert all(i["AccessCapabilities"] == ["Read", "Write"] for i in conn["MemoryChunkInfo"])
        status, domain = api_client.get(f"/redfish/v1/Systems/{HOST0}/MemoryDomains/{cid}")
        assert status == 200

    def test_one_connection_per_binding(self, api_client):
        """Six bindings: four SLDs bound whole and two LDs of the MLD."""
        status, data = api_client.get(CONNECTIONS)
        ids = sorted(m["@odata.id"].rsplit("/", 1)[1] for m in data["Members"])
        assert ids == sorted([
            "cxl_switch_0_vcs0_vppb1_p1", "cxl_switch_0_vcs0_vppb2_p2",
            "cxl_switch_0_vcs0_vppb3_p3", "cxl_switch_0_vcs0_vppb4_p4_ld0",
            "cxl_switch_0_vcs0_vppb5_p4_ld1", "cxl_switch_0_vcs0_vppb6_p5"])
        status, domains = api_client.get(f"/redfish/v1/Systems/{HOST0}/MemoryDomains")
        assert domains["Members@odata.count"] == 6

    def test_post_of_a_bound_target_is_that_connection(self, api_client):
        """Idempotent: the host has dev0 already, whoever bound it. The
        answer is 200 with that Connection, and nothing is bound."""
        before = api_client.get(CONNECTIONS)[1]["Members"]
        status, data, location = post(api_client, HOST0, "cxl_switch0_dev0",
                                      chunks_of(api_client, "cxl_switch0_dev0"))
        assert status == 200
        assert location.endswith("/cxl_switch_0_vcs0_vppb1_p1")
        assert data["@odata.id"] == location
        assert api_client.get(CONNECTIONS)[1]["Members"] == before

    def test_post_of_a_bound_ld(self, api_client):
        """LD 1 of the MLD, as its Endpoint and in the old form (the MLD's
        ID and a chunk of LD 1): the same Connection."""
        for target, chunks in (("cxl_switch0_mld_ld1", None),
                               ("cxl_switch0_mld", chunks_of(api_client, "cxl_switch0_mld", 1))):
            status, data, location = post(api_client, HOST0, target, chunks)
            assert status == 200, target
            assert data["Id"] == "cxl_switch_0_vcs0_vppb5_p4_ld1"
            assert data["Links"]["TargetEndpoints"] == [
                {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_mld_ld1"}]

    def test_delete_unbinds_a_binding_cxl_fmd_did_not_make(self, api_client):
        cid = "cxl_switch_0_vcs0_vppb3_p3"
        assert api_client.delete(f"{CONNECTIONS}/{cid}") == 204
        assert api_client.get(f"{CONNECTIONS}/{cid}")[0] == 404
        assert api_client.get(f"/redfish/v1/Systems/{HOST0}/MemoryDomains/{cid}")[0] == 404
        status, vppb = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs/3")
        assert vppb["BindingStatus"] == "Unbound"
        # and it can be bound again: a new Connection, on the lowest free vPPB
        status, data, location = post(api_client, HOST0, "cxl_switch0_dev2")
        assert status == 201
        assert data["Id"] == "cxl_switch_0_vcs0_vppb3_p3"


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestPostIdempotent:

    def test_post_idempotent_enabled(self, api_client):
        """A POST of a target the host has is answered with its Connection:
        201 the first time, then 200 with the same Location."""
        status, first, location = post(api_client, "switch_0_host0", "sld_0")
        assert status == 201
        assert first["Status"]["State"] == "Enabled"
        for _ in range(3):
            status, again, where = post(api_client, "switch_0_host0", "sld_0",
                                        chunks_of(api_client, "sld_0"))
            assert status == 200
            assert where == location and again["Id"] == first["Id"]
        status, data = api_client.get(CONNECTIONS)
        assert data["Members"] == [{"@odata.id": location}]

    def test_post_validation_400(self, api_client):
        """A chunk of another device, an MLD's chunks on two LDs, and
        AccessCapabilities other than Read and Write are bad requests, and
        nothing is bound."""
        before = api_client.get(CONNECTIONS)
        cases = [
            ("sld_0", chunks_of(api_client, "sld_1"), None),
            ("mld_0", chunks_of(api_client, "mld_0", 0) + chunks_of(api_client, "mld_0", 1), None),
            ("mld_0_ld0", chunks_of(api_client, "mld_0", 1), None),
            ("sld_0", chunks_of(api_client, "sld_0"), ["Read"]),
            ("sld_0", chunks_of(api_client, "sld_0"), ["Read", "Write", "Execute"]),
        ]
        for target, chunks, access in cases:
            status, data, _ = post(api_client, "switch_0_host0", target, chunks, access)
            assert status == 400, (target, chunks, access)
        assert api_client.get(CONNECTIONS) == before

    def test_two_initiators_400(self, api_client):
        status, _, _ = api_client.post(CONNECTIONS, {
            "Links": {
                "InitiatorEndpoints": [
                    {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"},
                    {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host1"}],
                "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]}})
        assert status == 400

    def test_patch_is_not_allowed(self, api_client):
        """Everything in a Connection is what the switch says (cxl-fmd's
        choice): 405, with Allow."""
        status, data, location = post(api_client, "switch_0_host0", "sld_0")
        assert status == 201
        response = requests.patch(api_client.base_url + location, json={"Name": "x"}, timeout=30)
        assert response.status_code == 405
        assert "GET" in response.headers["Allow"]


# =============================================================================
# Connection Error Handling
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestConnectionErrors:
    """Tests for connection error handling."""

    def test_reject_missing_initiator_endpoint(self, api_client):
        """Test that missing InitiatorEndpoints is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "TargetEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0"}
                    ]
                },
                "MemoryChunkInfo": [{
                    "MemoryChunk": {"@odata.id": "/fake/chunk"}
                }]
            }
        )
        assert status == 400

    def test_reject_missing_target_endpoint(self, api_client):
        """Test that missing TargetEndpoints is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0"}
                    ]
                },
                "MemoryChunkInfo": [{
                    "MemoryChunk": {"@odata.id": "/fake/chunk"}
                }]
            }
        )
        assert status == 400

    def test_reject_invalid_host_endpoint(self, api_client):
        """Test that invalid host endpoint is rejected."""
        # Use existing chunk
        chunk_path = "/redfish/v1/Chassis/cxl_switch0_dev0/MemoryDomains/cxl_switch0_dev0_domain0/MemoryChunks/cxl_switch0_dev0_chunk0"

        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/nonexistent_host"}
                    ],
                    "TargetEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0"}
                    ]
                },
                "MemoryChunkInfo": [{
                    "MemoryChunk": {"@odata.id": chunk_path}
                }]
            }
        )
        assert status in [400, 404]

    def test_reject_invalid_device_endpoint(self, api_client):
        """Test that invalid device endpoint is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0"}
                    ],
                    "TargetEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/nonexistent_device"}
                    ]
                },
                "MemoryChunkInfo": [{
                    "MemoryChunk": {"@odata.id": "/fake/chunk"}
                }]
            }
        )
        assert status in [400, 404]


# =============================================================================
# Direct-Attached Connection Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mld-device.json")
class TestDirectAttachedConnections:
    """mld-device.json has two direct-attached hosts and devices on no
    switch. A device on no switch has one link, and nothing says which of
    the two hosts it is to: neither has a Connection to it (cxl-fmd's
    choice), and one cannot be made, there being no vPPB to bind."""

    def test_no_connection_without_a_known_host(self, api_client):
        status, data = api_client.get(CONNECTIONS)
        assert status == 200 and data["Members"] == []

    def test_direct_attached_connection_400(self, api_client):
        chunk_path = "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld0_chunk"
        status, data, _ = post(api_client, "host0", "cxl_mld_0", [chunk_path])
        assert status == 400
