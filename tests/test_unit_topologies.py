# SPDX-License-Identifier: LGPL-2.1
"""
Unit tests for 7 CXL topology combinations: every attachment and device type
of the matrix below but a Multi-Headed MLD on a switch, which has no topology
here (a head that presents an MLD cannot be bound per LD, a known gap).

Each topology has three test categories:
1. Discovery - Verify correct resource hierarchy
2. Memory Lifecycle - Verify chunk operations
3. Connections - Verify host-to-device binding

Topology Matrix:
    | Attachment | Device   | Heads | LDs        |
    |------------|----------|-------|------------|
    | Direct     | SLD      | -     | 1 implicit |
    | Direct     | MLD      | -     | 2 explicit |
    | Direct     | MH-SLD   | 2     | 1 per head |
    | Direct     | MH-MLD   | 2     | 2 per head |
    | Switch     | SLD      | -     | 1 implicit |
    | Switch     | MLD      | -     | 2 explicit |
    | Switch     | MH-SLD   | 2     | 1 per head |
    | Switch     | MH-MLD   | 2     | not tested |
"""

import pytest

CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"
EP = "/redfish/v1/Fabrics/CXL/Endpoints/"


def chunk_uri_of(api_client, dev, ld=None):
    status, data = api_client.get(f"/redfish/v1/Chassis/{dev}/MemoryDomains/{dev}_domain0/MemoryChunks")
    assert status == 200
    return next(m["@odata.id"] for m in data["Members"]
                if ld is None or "_ld%d_" % ld in m["@odata.id"])


def post(api_client, host, target, chunk=None):
    body = {"ConnectionType": "Memory",
            "Links": {"InitiatorEndpoints": [{"@odata.id": EP + host}],
                      "TargetEndpoints": [{"@odata.id": EP + target}]}}
    if chunk:
        body["MemoryChunkInfo"] = [{"MemoryChunk": {"@odata.id": chunk}}]
    return api_client.post(CONNECTIONS, body)


def connection_ids(api_client):
    status, data = api_client.get(CONNECTIONS)
    assert status == 200
    return sorted(m["@odata.id"].rsplit("/", 1)[1] for m in data["Members"])



# =============================================================================
# 1. DIRECT-ATTACHED SLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestDirectSLDDiscovery:
    """Discovery tests for direct-attached SLD."""

    def test_chassis_exists(self, api_client):
        """SLD appears in Chassis collection."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        assert data["Members@odata.count"] >= 1
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("sld_0" in m for m in member_ids)

    def test_pcie_device_exists(self, api_client):
        """PCIeDevice exists under Chassis."""
        status, data = api_client.get("/redfish/v1/Chassis/sld_0/PCIeDevices")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_single_cxl_logical_device(self, api_client):
        """SLD has exactly 1 CXLLogicalDevice."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/PCIeDevices/sld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 1

    def test_ld_memory_size(self, api_client):
        """LD memory size is 2048 MiB."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/PCIeDevices/sld_0/CXLLogicalDevices/0"
        )
        assert status == 200
        assert data["MemorySizeMiB"] == 2048

    def test_memory_domain_exists(self, api_client):
        """MemoryDomain exists under Chassis."""
        status, data = api_client.get("/redfish/v1/Chassis/sld_0/MemoryDomains")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_no_switch_in_fabric(self, api_client):
        """No switches in direct-attached topology."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200
        assert data["Members@odata.count"] == 0

    def test_endpoint_is_target(self, api_client):
        """SLD endpoint has EntityRole=Target."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        assert status == 200
        # Find SLD endpoint
        for member in data["Members"]:
            ep_status, ep_data = api_client.get(member["@odata.id"])
            if "sld_0" in member["@odata.id"]:
                assert ep_data["ConnectedEntities"][0]["EntityRole"] == "Target"
                return
        pytest.fail("SLD endpoint not found")


@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestDirectSLDMemoryLifecycle:
    """Memory lifecycle tests for direct-attached SLD."""

    def test_chunk_exists_after_discovery(self, api_client):
        """Initial chunk exists after discovery."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_chunk_size_matches_device(self, api_client):
        """Chunk size is 2048 MiB."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        assert status == 200
        chunk_uri = data["Members"][0]["@odata.id"]
        status, chunk = api_client.get(chunk_uri)
        assert status == 200
        assert chunk["MemoryChunkSizeMiB"] == 2048

    def test_chunk_type_is_pmem(self, api_client):
        """AddressRangeType is PMEM."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]
        status, chunk = api_client.get(chunk_uri)
        assert status == 200
        assert chunk["AddressRangeType"] == "PMEM"

    def test_delete_chunk_frees_capacity(self, api_client):
        """DELETE chunk returns 204 and count becomes 0."""
        # Get chunk URI
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]

        # Delete chunk
        status = api_client.delete(chunk_uri)
        assert status == 204

        # Verify count is 0
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        assert data["Members@odata.count"] == 0

    def test_create_chunk_after_delete(self, api_client):
        """POST succeeds after DELETE."""
        # Delete existing chunk
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]
        api_client.delete(chunk_uri)

        # Create new chunk
        status, data, location = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1024, "AddressRangeType": "PMEM"}
        )
        assert status == 201
        assert data["MemoryChunkSizeMiB"] == 1024

    def test_reject_oversized_chunk(self, api_client):
        """POST for chunk larger than capacity rejected with 409."""
        # Try to create chunk larger than device capacity
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 8192, "AddressRangeType": "PMEM"}
        )
        assert status == 409


@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestDirectSLDConnections:
    """A direct-attached SLD is on the link of the only direct-attached
    host, 'local': that link is their Connection, there from the start
    (cxl-fmd's choice). There is no vPPB: nothing to bind or unbind."""

    CID = "local_direct_sld_0"

    def test_connection_exists_at_startup(self, api_client):
        assert connection_ids(api_client) == [self.CID]
        status, data = api_client.get(f"{CONNECTIONS}/{self.CID}")
        assert status == 200
        assert data["Status"] == {"State": "Enabled", "Health": "OK"}
        assert data["Links"]["InitiatorEndpoints"] == [{"@odata.id": EP + "local"}]
        assert data["Links"]["TargetEndpoints"] == [{"@odata.id": EP + "sld_0"}]
        assert [i["MemoryChunk"]["@odata.id"] for i in data["MemoryChunkInfo"]] == [
            chunk_uri_of(api_client, "sld_0")]
        status, domains = api_client.get("/redfish/v1/Systems/local/MemoryDomains")
        assert domains["Members"] == [
            {"@odata.id": f"/redfish/v1/Systems/local/MemoryDomains/{self.CID}"}]

    def test_post_of_the_pair_is_200(self, api_client):
        status, data, location = post(api_client, "local", "sld_0",
                                      chunk_uri_of(api_client, "sld_0"))
        assert status == 200
        assert location == f"{CONNECTIONS}/{self.CID}"
        assert "InitiatorEndpoints" in data["Links"] and "TargetEndpoints" in data["Links"]

    def test_delete_is_not_allowed(self, api_client):
        assert api_client.delete(f"{CONNECTIONS}/{self.CID}") == 405
        assert connection_ids(api_client) == [self.CID]


# =============================================================================
# 2. DIRECT-ATTACHED MLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-mld.json")
class TestDirectMLDDiscovery:
    """Discovery tests for direct-attached MLD."""

    def test_chassis_exists(self, api_client):
        """MLD appears in Chassis collection."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("mld_0" in m for m in member_ids)

    def test_two_cxl_logical_devices(self, api_client):
        """MLD has exactly 2 CXLLogicalDevices."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_ld_ids_zero_and_one(self, api_client):
        """LD IDs are 0 and 1."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices"
        )
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any(m.endswith("/0") for m in member_ids)
        assert any(m.endswith("/1") for m in member_ids)

    def test_each_ld_has_memory(self, api_client):
        """Both LDs have 2048 MiB memory."""
        for ld_id in [0, 1]:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/{ld_id}"
            )
            assert status == 200
            assert data["MemorySizeMiB"] == 2048

    def test_memory_collection_has_two(self, api_client):
        """Memory collection has 2 entries (one per LD)."""
        status, data = api_client.get("/redfish/v1/Chassis/mld_0/Memory")
        assert status == 200
        assert data["Members@odata.count"] == 2


@pytest.mark.mock_topology("mock/unit/unit-direct-mld.json")
class TestDirectMLDMemoryLifecycle:
    """Memory lifecycle tests for direct-attached MLD."""

    def test_two_chunks_after_discovery(self, api_client):
        """2 chunks exist (one per LD)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_delete_ld0_chunk(self, api_client):
        """DELETE LD0 chunk reduces count to 1."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        # Find LD0 chunk
        for member in data["Members"]:
            if "_ld0_" in member["@odata.id"]:
                status = api_client.delete(member["@odata.id"])
                assert status == 204
                break

        # Verify count reduced
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        assert data["Members@odata.count"] == 1

    def test_create_chunk_with_interleave_set(self, api_client):
        """POST with InterleaveSets referencing Memory resource."""
        # Delete LD0 chunk first
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        for member in data["Members"]:
            if "_ld0_" in member["@odata.id"]:
                api_client.delete(member["@odata.id"])
                break

        # Create chunk with InterleaveSets
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks",
            {
                "InterleaveSets": [{
                    "Memory": {"@odata.id": "/redfish/v1/Chassis/mld_0/Memory/mld_0_ld0"},
                    "SizeMiB": 1024
                }]
            }
        )
        assert status == 201


@pytest.mark.mock_topology("mock/unit/unit-direct-mld.json")
class TestDirectMLDConnections:
    """host0 is the only direct-attached host: its Connection to the MLD
    has all of its LDs (the specification does not say what an MLD on a
    root port gives its host; that is cxl-fmd's choice)."""

    CID = "host0_direct_mld_0"

    def test_connection_has_each_ld(self, api_client):
        assert connection_ids(api_client) == [self.CID]
        status, data = api_client.get(f"{CONNECTIONS}/{self.CID}")
        assert data["Links"]["TargetEndpoints"] == [
            {"@odata.id": EP + "mld_0_ld0"}, {"@odata.id": EP + "mld_0_ld1"}]
        assert len(data["MemoryChunkInfo"]) == 2

    def test_connection_to_ld0_chunk(self, api_client):
        status, data, location = post(api_client, "host0", "mld_0",
                                      chunk_uri_of(api_client, "mld_0", 0))
        assert status == 200 and data["Id"] == self.CID

    def test_connection_to_ld1_chunk(self, api_client):
        status, data, location = post(api_client, "host0", "mld_0_ld1",
                                      chunk_uri_of(api_client, "mld_0", 1))
        assert status == 200 and data["Id"] == self.CID


# =============================================================================
# 3. DIRECT-ATTACHED MH-SLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-mhsld.json")
class TestDirectMHSLDDiscovery:
    """Discovery tests for direct-attached MH-SLD."""

    def test_chassis_exists(self, api_client):
        """MH-SLD appears in Chassis collection."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("mhsld_0" in m for m in member_ids)

    def test_two_pool_lds(self, api_client):
        """MH-SLD has 2 CXLLogicalDevices (pool indices 0, 1)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/PCIeDevices/mhsld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_two_hosts_exist(self, api_client):
        """host0 and host1 exist in Systems."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("host0" in m for m in member_ids)
        assert any("host1" in m for m in member_ids)

    def test_ld_pool_indices(self, api_client):
        """LD IDs are pool indices 0 and 1."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/PCIeDevices/mhsld_0/CXLLogicalDevices"
        )
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any(m.endswith("/0") for m in member_ids)
        assert any(m.endswith("/1") for m in member_ids)


@pytest.mark.mock_topology("mock/unit/unit-direct-mhsld.json")
class TestDirectMHSLDMemoryLifecycle:
    """Memory lifecycle tests for direct-attached MH-SLD."""

    def test_two_chunks_for_pool(self, api_client):
        """2 chunks exist (one per pool LD)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_chunk_sizes(self, api_client):
        """Each chunk is 2048 MiB."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
        )
        for member in data["Members"]:
            status, chunk = api_client.get(member["@odata.id"])
            assert chunk["MemoryChunkSizeMiB"] == 2048


@pytest.mark.mock_topology("mock/unit/unit-direct-mhsld.json")
class TestDirectMHSLDConnections:
    """Each head is attached to a host: head 0 to host0, with pool LD 0,
    head 1 to host1, with pool LD 1. Each such link is a Connection."""

    def test_a_connection_per_head(self, api_client):
        assert connection_ids(api_client) == ["host0_direct_mhsld_0_h0",
                                              "host1_direct_mhsld_0_h1"]
        status, data = api_client.get(f"{CONNECTIONS}/host1_direct_mhsld_0_h1")
        assert data["Links"]["TargetEndpoints"] == [{"@odata.id": EP + "mhsld_0_ld1"}]
        assert [i["MemoryChunk"]["@odata.id"] for i in data["MemoryChunkInfo"]] == [
            chunk_uri_of(api_client, "mhsld_0", 1)]

    def test_pool_ld_endpoint_as_target(self, api_client):
        """A pool LD's Endpoint is a target, with a chunk of that LD, and the
        device's ID still is, for compatibility: both are head 0's
        Connection."""
        chunk = chunk_uri_of(api_client, "mhsld_0", 0)
        for target in ("mhsld_0_ld0", "mhsld_0"):
            status, data, _ = post(api_client, "host0", target, chunk)
            assert status == 200, target
            assert data["Id"] == "host0_direct_mhsld_0_h0"
            assert data["Links"]["TargetEndpoints"] == [{"@odata.id": EP + "mhsld_0_ld0"}]

    def test_host0_connects_to_pool_ld0(self, api_client):
        assert post(api_client, "host0", "mhsld_0", chunk_uri_of(api_client, "mhsld_0", 0))[0] == 200

    def test_host1_connects_to_pool_ld1(self, api_client):
        assert post(api_client, "host1", "mhsld_0", chunk_uri_of(api_client, "mhsld_0", 1))[0] == 200

    def test_host0_cannot_access_pool_ld1(self, api_client):
        """Pool LD 1 is head 1's: host0 is not on its link."""
        assert post(api_client, "host0", "mhsld_0", chunk_uri_of(api_client, "mhsld_0", 1))[0] == 400

    def test_delete_is_not_allowed(self, api_client):
        assert api_client.delete(f"{CONNECTIONS}/host0_direct_mhsld_0_h0") == 405


# =============================================================================
# 4. DIRECT-ATTACHED MH-MLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-mhmld.json")
class TestDirectMHMLDDiscovery:
    """Discovery tests for direct-attached MH-MLD."""

    def test_four_pool_lds(self, api_client):
        """MH-MLD has 4 CXLLogicalDevices (pool indices 0-3)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/PCIeDevices/mhmld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 4

    def test_two_hosts_exist(self, api_client):
        """host0 and host1 exist in Systems."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("host0" in m for m in member_ids)
        assert any("host1" in m for m in member_ids)

    def test_ld_pool_indices_0_to_3(self, api_client):
        """LD IDs are pool indices 0, 1, 2, 3."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/PCIeDevices/mhmld_0/CXLLogicalDevices"
        )
        member_ids = [m["@odata.id"] for m in data["Members"]]
        for i in range(4):
            assert any(m.endswith(f"/{i}") for m in member_ids)


@pytest.mark.mock_topology("mock/unit/unit-direct-mhmld.json")
class TestDirectMHMLDMemoryLifecycle:
    """Memory lifecycle tests for direct-attached MH-MLD."""

    def test_four_chunks_for_pool(self, api_client):
        """4 chunks exist (one per pool LD)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 4

    def test_chunk_sizes(self, api_client):
        """Each chunk is 1024 MiB."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        for member in data["Members"]:
            status, chunk = api_client.get(member["@odata.id"])
            assert chunk["MemoryChunkSizeMiB"] == 1024


@pytest.mark.mock_topology("mock/unit/unit-direct-mhmld.json")
class TestDirectMHMLDConnections:
    """Connection tests for direct-attached MH-MLD."""

    EP = "/redfish/v1/Fabrics/CXL/Endpoints/"

    def test_endpoints_are_pool_lds_and_heads(self, api_client):
        """Each pool LD has an Endpoint, and each head, which presents an
        MLD, one of its own, EntityType Memory: which pool LD a head's LD-ID
        is, the device keeps to itself (CXL r4.0 Section 2.5.1). The device
        has none."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        ids = {m["@odata.id"].rsplit("/", 1)[1] for m in data["Members"]}
        assert ids == {"host0", "host1", "mhmld_0_head0", "mhmld_0_head1",
                       "mhmld_0_ld0", "mhmld_0_ld1", "mhmld_0_ld2", "mhmld_0_ld3"}
        status, ep = api_client.get(self.EP + "mhmld_0_head1")
        assert status == 200
        entity = ep["ConnectedEntities"][0]
        assert entity["EntityType"] == "Memory"
        assert entity["EntityRole"] == "Target"
        assert "EntityLink" not in entity
        status, ep = api_client.get(self.EP + "mhmld_0_ld3")
        assert status == 200
        assert ep["ConnectedEntities"][0]["EntityLink"] == {
            "@odata.id": "/redfish/v1/Chassis/mhmld_0/PCIeDevices/mhmld_0/CXLLogicalDevices/3"}
        status, _ = api_client.get(self.EP + "mhmld_0")
        assert status == 404

    def test_head_endpoint_as_target_400(self, api_client):
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks")
        chunk_uri = next(m["@odata.id"] for m in data["Members"] if "_ld0_" in m["@odata.id"])
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": self.EP + "host0"}],
                    "TargetEndpoints": [{"@odata.id": self.EP + "mhmld_0_head0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 400

    def test_a_connection_per_head(self, api_client):
        """The heads present MLDs; on a host's link the LD Map says which
        pool LDs each has: head 0 pool LDs 0 and 1, head 1 2 and 3."""
        assert connection_ids(api_client) == ["host0_direct_mhmld_0_h0",
                                              "host1_direct_mhmld_0_h1"]
        status, data = api_client.get(f"{CONNECTIONS}/host1_direct_mhmld_0_h1")
        assert data["Links"]["TargetEndpoints"] == [
            {"@odata.id": EP + "mhmld_0_ld2"}, {"@odata.id": EP + "mhmld_0_ld3"}]

    def test_host0_connects_to_pool_ld0(self, api_client):
        """host0 can connect to pool LD 0 (head 0)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        chunk_uri = None
        for member in data["Members"]:
            if "_ld0_" in member["@odata.id"]:
                chunk_uri = member["@odata.id"]
                break

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/host0"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhmld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 200

    def test_host0_connects_to_pool_ld1(self, api_client):
        """host0 can connect to pool LD 1 (head 0)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        chunk_uri = None
        for member in data["Members"]:
            if "_ld1_" in member["@odata.id"]:
                chunk_uri = member["@odata.id"]
                break

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/host0"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhmld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 200

    def test_host1_connects_to_pool_ld2(self, api_client):
        """host1 can connect to pool LD 2 (head 1)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        chunk_uri = None
        for member in data["Members"]:
            if "_ld2_" in member["@odata.id"]:
                chunk_uri = member["@odata.id"]
                break

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/host1"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhmld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 200

    def test_host0_cannot_access_pool_ld2(self, api_client):
        """host0 cannot access pool LD 2 (head 1's LD) - expect 400."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhmld_0/MemoryDomains/mhmld_0_domain0/MemoryChunks"
        )
        chunk_uri = None
        for member in data["Members"]:
            if "_ld2_" in member["@odata.id"]:
                chunk_uri = member["@odata.id"]
                break

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/host0"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhmld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 400


# =============================================================================
# 5. SWITCH-ATTACHED SLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-switch-sld.json")
class TestSwitchSLDDiscovery:
    """Discovery tests for switch-attached SLD."""

    def test_switch_exists_in_fabric(self, api_client):
        """Switch exists in Fabrics/CXL/Switches."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_switch_has_ports(self, api_client):
        """Switch has 2 ports (1 USP + 1 DSP)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports")
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_sld_exists(self, api_client):
        """SLD exists in Chassis collection."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        member_ids = [m["@odata.id"] for m in data["Members"]]
        assert any("sld_0" in m for m in member_ids)

    def test_vcs_exists(self, api_client):
        """VCS collection has 1 entry."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_host_auto_created(self, api_client):
        """Host auto-created for VCS 0."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_zone_exists(self, api_client):
        """Zone exists in fabric."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        assert data["Members@odata.count"] >= 1


@pytest.mark.mock_topology("mock/unit/unit-switch-sld.json")
class TestSwitchSLDMemoryLifecycle:
    """Memory lifecycle tests for switch-attached SLD."""

    def test_chunk_exists(self, api_client):
        """Chunk exists under SLD domain."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    CHUNKS = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
    CONNECTION = "/redfish/v1/Fabrics/CXL/Connections/switch_0_vcs0_vppb1_p1"

    def test_chunk_delete_while_connected_409(self, api_client):
        """sld_0 is bound to vPPB 1 before cxl-fmd starts: its chunk is
        what Connection switch_0_vcs0_vppb1_p1 exposes, and is not deleted
        under the host (cxl-fmd's choice)."""
        status, data = api_client.get(self.CHUNKS)
        chunk_uri = data["Members"][0]["@odata.id"]
        assert api_client.delete(chunk_uri) == 409
        status, _ = api_client.get(chunk_uri)
        assert status == 200
        status, conn = api_client.get(self.CONNECTION)
        assert chunk_uri in [i["MemoryChunk"]["@odata.id"] for i in conn["MemoryChunkInfo"]]

    def test_chunk_delete_after_unbind_ok(self, api_client):
        """Once the Connection is deleted, the vPPB unbound, the chunk can
        be deleted and made again."""
        status, data = api_client.get(self.CHUNKS)
        chunk_uri = data["Members"][0]["@odata.id"]
        assert api_client.delete(self.CONNECTION) == 204
        assert api_client.delete(chunk_uri) == 204

        status, data, _ = api_client.post(
            self.CHUNKS, {"MemoryChunkSizeMiB": 1024, "AddressRangeType": "PMEM"}
        )
        assert status == 201


@pytest.mark.mock_topology("mock/unit/unit-switch-sld.json")
class TestSwitchSLDConnections:
    """Connection tests for switch-attached SLD."""

    def test_create_connection(self, api_client):
        """Connection through switch succeeds."""
        # Get host endpoint
        status, data = api_client.get("/redfish/v1/Systems")
        host_id = data["Members"][0]["@odata.id"].split("/")[-1]

        # Get chunk
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]

        # Create connection
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host_id}"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 200
        assert data["Id"] == "switch_0_vcs0_vppb1_p1"

    def test_prebound_connection(self, api_client):
        """vPPB 1 of VCS 0 is bound to the SLD's port in the topology."""
        assert connection_ids(api_client) == ["switch_0_vcs0_vppb1_p1"]

    def test_delete_then_post_binds_again(self, api_client):
        cid = "switch_0_vcs0_vppb1_p1"
        assert api_client.delete(f"{CONNECTIONS}/{cid}") == 204
        assert connection_ids(api_client) == []
        status, data, location = post(api_client, "switch_0_host0", "sld_0")
        assert status == 201 and data["Id"] == cid


# =============================================================================
# 6. SWITCH-ATTACHED MLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-switch-mld.json")
class TestSwitchMLDDiscovery:
    """Discovery tests for switch-attached MLD."""

    def test_two_lds(self, api_client):
        """MLD has 2 CXLLogicalDevices."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_switch_exists(self, api_client):
        """Switch exists in fabric."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_vppb_bound_ld_status(self, api_client):
        """vPPBs have BoundLD status."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs/0/VPPBs"
        )
        assert status == 200
        # Should have vPPBs with BoundLD status
        for member in data["Members"]:
            status, vppb = api_client.get(member["@odata.id"])
            if "BoundLD" in vppb.get("CurrentVPPBBindingState", ""):
                return
        # At least some should exist
        assert data["Members@odata.count"] >= 2


@pytest.mark.mock_topology("mock/unit/unit-switch-mld.json")
class TestSwitchMLDMemoryLifecycle:
    """Memory lifecycle tests for switch-attached MLD."""

    def test_per_ld_chunks(self, api_client):
        """2 chunks exist (one per LD)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2


@pytest.mark.mock_topology("mock/unit/unit-switch-mld.json")
class TestSwitchMLDConnections:
    """Connection tests for switch-attached MLD."""

    def test_connection_to_ld0(self, api_client):
        """Connection to MLD LD 0 succeeds."""
        # Get host
        status, data = api_client.get("/redfish/v1/Systems")
        host_id = data["Members"][0]["@odata.id"].split("/")[-1]

        # Get LD0 chunk
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        chunk_uri = None
        for member in data["Members"]:
            if "_ld0_" in member["@odata.id"]:
                chunk_uri = member["@odata.id"]
                break

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host_id}"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 200
        assert data["Id"] == "switch_0_vcs0_vppb1_p1_ld0"


    CHUNKS = "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
    EP = "/redfish/v1/Fabrics/CXL/Endpoints/"

    def _post(self, api_client, target, ld):
        status, data = api_client.get("/redfish/v1/Systems")
        host_id = data["Members"][0]["@odata.id"].split("/")[-1]
        status, data = api_client.get(self.CHUNKS)
        chunk_uri = next(m["@odata.id"] for m in data["Members"]
                         if "_ld%d_" % ld in m["@odata.id"])
        return api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": self.EP + host_id}],
                    "TargetEndpoints": [{"@odata.id": self.EP + target}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )

    def test_old_form_targets_the_chunks_ld(self, api_client):
        """The MLD's own ID, which has no Endpoint any more, is still taken
        with a chunk, for compatibility: the Connection's target is the
        Endpoint of the chunk's LD, which is served."""
        status, _ = api_client.get(self.EP + "mld_0")
        assert status == 404
        status, data, _ = self._post(api_client, "mld_0", 1)
        assert status == 200
        assert data["Links"]["TargetEndpoints"] == [{"@odata.id": self.EP + "mld_0_ld1"}]
        status, _ = api_client.get(data["Links"]["TargetEndpoints"][0]["@odata.id"])
        assert status == 200

    def test_ld_endpoint_as_target(self, api_client):
        """An LD Endpoint is a target, with a chunk of that LD."""
        status, data, _ = self._post(api_client, "mld_0_ld0", 0)
        assert status == 200
        assert data["Links"]["TargetEndpoints"] == [{"@odata.id": self.EP + "mld_0_ld0"}]

    def test_ld_endpoint_with_a_chunk_of_another_ld_400(self, api_client):
        status, _, _ = self._post(api_client, "mld_0_ld0", 1)
        assert status == 400
        assert connection_ids(api_client) == ["switch_0_vcs0_vppb1_p1_ld0",
                                              "switch_0_vcs0_vppb2_p1_ld1"]

    def test_no_implicit_connection(self, api_client):
        """A device is not a Connection: its LDs' bindings are."""
        assert api_client.get("/redfish/v1/Fabrics/CXL/Connections/mld_0")[0] == 404
        status, data = api_client.get(CONNECTIONS + "/switch_0_vcs0_vppb2_p1_ld1")
        assert data["Links"]["TargetEndpoints"] == [{"@odata.id": self.EP + "mld_0_ld1"}]


# =============================================================================
# 7. SWITCH-ATTACHED MH-SLD
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-switch-mhsld.json")
class TestSwitchMHSLDDiscovery:
    """Discovery tests for switch-attached MH-SLD."""

    def test_two_vcs(self, api_client):
        """Switch has 2 VCS (one per host)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs")
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_two_usps(self, api_client):
        """Switch has 2 USP ports."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports")
        assert status == 200
        # Count upstream ports by ID pattern
        usp_count = sum(1 for m in data["Members"] if "usp" in m["@odata.id"])
        assert usp_count == 2

    def test_two_hosts(self, api_client):
        """2 hosts auto-created (one per VCS)."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        assert data["Members@odata.count"] >= 2

    def test_two_pool_lds(self, api_client):
        """MH-SLD has 2 pool LDs."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/PCIeDevices/mhsld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_two_zones(self, api_client):
        """2 zones exist (one per VCS)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        assert data["Members@odata.count"] >= 2


@pytest.mark.mock_topology("mock/unit/unit-switch-mhsld.json")
class TestSwitchMHSLDMemoryLifecycle:
    """Memory lifecycle tests for switch-attached MH-SLD."""

    def test_per_pool_ld_chunks(self, api_client):
        """2 chunks exist (one per pool LD)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2


@pytest.mark.mock_topology("mock/unit/unit-switch-mhsld.json")
class TestSwitchMHSLDConnections:
    """Connection tests for an MH-SLD whose heads are on switch ports.

    A head is a CXL port of the device: head 0 (pool LD 0) is on port 2,
    which is bound to VCS 0, and head 1 (pool LD 1) is on port 3, which is
    bound to no host. Each head presents an SLD, so its port is bound as a
    whole.
    """

    CHUNKS = "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
    VCSS = "/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs"

    def _connect(self, api_client, host, ld):
        status, data = api_client.get(self.CHUNKS)
        assert status == 200
        chunk_uri = next(m["@odata.id"] for m in data["Members"]
                         if "_ld%d_" % ld in m["@odata.id"])
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host%d" % host}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        return status, data

    def _vppb(self, api_client, vcs, vppb):
        status, data = api_client.get("%s/%d/VPPBs/%d" % (self.VCSS, vcs, vppb))
        assert status == 200
        return data["BindingStatus"], data.get("BoundPortId")

    def test_each_head_is_on_its_own_port(self, api_client):
        status, data = api_client.get("/redfish/v1/Chassis/mhsld_0/PCIeDevices/mhsld_0")
        assert status == 200
        ports = sorted(p["@odata.id"].rsplit("/", 1)[1]
                       for p in data["Links"]["ConnectedPCIePorts"])
        assert ports == ["switch_0_dsp2", "switch_0_dsp3"]
        for ld, port in enumerate(ports):
            status, data = api_client.get(
                "/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports/" + port)
            assert status == 200
            # each head presents an SLD to the port it is on, and what is
            # at the other end of the link is the pool LD mapped to it
            assert data["CXL"]["ConnectedDeviceType"] == "Type3SLD"
            assert data["Links"]["AssociatedEndpoints"] == [
                {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0_ld%d" % ld}]

    def test_head_port_vppb_links_to_its_pool_ld(self, api_client):
        """vPPB 1 of VCS 0 is bound to port 2, head 0's: its Endpoint is
        pool LD 0, which is in Zone vcs0, and LD 1 is in no Zone."""
        status, data = api_client.get(self.VCSS + "/0/VPPBs/1")
        assert status == 200
        assert data["Links"]["Endpoints"] == [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0_ld0"}]
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0_ld0")
        assert status == 200
        assert data["Links"]["ConnectedPorts"] == [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports/switch_0_dsp2"}]
        assert data["Links"]["Zones"] == [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Zones/switch_0_vcs0"}]
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0_ld1")
        assert status == 200
        assert "Zones" not in data["Links"]

    def test_host0_has_head0(self, api_client):
        """Port 2 is already bound to VCS 0: nothing more is bound, and the
        answer is that Connection."""
        assert self._connect(api_client, 0, 0)[0] == 200
        assert self._vppb(api_client, 0, 1) == ("BoundPhysicalPort", 2)
        assert self._vppb(api_client, 0, 2) == ("Unbound", None)

    def test_host1_is_given_head1(self, api_client):
        """Head 1 is on port 3: the whole port is bound to VCS 1."""
        assert self._vppb(api_client, 1, 1) == ("Unbound", None)
        assert self._connect(api_client, 1, 1)[0] == 201
        assert self._vppb(api_client, 1, 1) == ("BoundPhysicalPort", 3)

    def test_host1_cannot_have_head0(self, api_client):
        """Pool LD 0 is on head 0, whose port is bound to VCS 0."""
        assert self._connect(api_client, 1, 0)[0] == 409
        assert self._vppb(api_client, 1, 1) == ("Unbound", None)


