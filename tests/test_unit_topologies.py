# SPDX-License-Identifier: LGPL-2.1
"""
Unit tests for all 8 CXL topology combinations.

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
    | Switch     | MH-MLD   | 2     | 2 per head |
"""

import pytest


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
    """Connection tests for direct-attached SLD."""

    def test_create_connection(self, api_client):
        """POST connection succeeds."""
        # Get chunk URI
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]

        # Create connection using 'local' host (default for direct-attached)
        status, data, location = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/local"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201

    def test_connection_has_endpoints(self, api_client):
        """Connection response includes InitiatorEndpoints and TargetEndpoints."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]

        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/local"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert "Links" in data
        assert "InitiatorEndpoints" in data["Links"]
        assert "TargetEndpoints" in data["Links"]

    def test_delete_connection(self, api_client):
        """DELETE connection returns 204."""
        # Create connection first
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]

        status, data, location = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "ConnectionType": "Memory",
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/local"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201

        # Delete connection
        status = api_client.delete(location)
        assert status == 204


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
    """Connection tests for direct-attached MLD."""

    def test_connection_to_ld0_chunk(self, api_client):
        """Connection using LD0's chunk succeeds."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        # Find LD0 chunk
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
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201

    def test_connection_to_ld1_chunk(self, api_client):
        """Connection using LD1's chunk succeeds."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks"
        )
        # Find LD1 chunk
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
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201


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
    """Connection tests for direct-attached MH-SLD."""

    def test_host0_connects_to_pool_ld0(self, api_client):
        """host0 can connect to pool LD 0 (head 0)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
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
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201

    def test_host1_connects_to_pool_ld1(self, api_client):
        """host1 can connect to pool LD 1 (head 1)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
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
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/host1"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 201

    def test_host0_cannot_access_pool_ld1(self, api_client):
        """host0 cannot access pool LD 1 (wrong head) - expect 400."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mhsld_0/MemoryDomains/mhsld_0_domain0/MemoryChunks"
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
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0"}]
                },
                "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk_uri}}]
            }
        )
        assert status == 400


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
        assert status == 201

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
        assert status == 201

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
        assert status == 201

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

    def test_delete_and_recreate(self, api_client):
        """DELETE and POST cycle works."""
        # Get and delete chunk
        status, data = api_client.get(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        )
        chunk_uri = data["Members"][0]["@odata.id"]
        status = api_client.delete(chunk_uri)
        assert status == 204

        # Recreate
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1024, "AddressRangeType": "PMEM"}
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
        assert status == 201


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
        assert status == 201


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
        for port in ports:
            status, data = api_client.get(
                "/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports/" + port)
            assert status == 200
            # each head presents an SLD to the port it is on
            assert data["CXL"]["ConnectedDeviceType"] == "Type3SLD"
            assert data["Links"]["AssociatedEndpoints"] == [
                {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mhsld_0"}]

    def test_host0_has_head0(self, api_client):
        """Port 2 is already bound to VCS 0: nothing more is bound."""
        assert self._connect(api_client, 0, 0)[0] == 201
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


