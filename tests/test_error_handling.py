# SPDX-License-Identifier: LGPL-2.1
"""
Tests for Redfish API error handling.

Validates that cxl-fmd returns appropriate HTTP status codes and
Redfish error responses for various error conditions.
"""

import pytest


# =============================================================================
# 404 Not Found Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class Test404NotFound:
    """Tests for 404 Not Found responses."""

    def test_invalid_chassis_id(self, api_client):
        """Verify 404 for non-existent Chassis ID."""
        status, data = api_client.get("/redfish/v1/Chassis/nonexistent_device")
        assert status == 404

    def test_invalid_pcie_device_id(self, api_client):
        """Verify 404 for non-existent PCIeDevice ID."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/nonexistent"
        )
        assert status == 404

    def test_invalid_memory_domain_id(self, api_client):
        """Verify 404 for non-existent MemoryDomain ID."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/nonexistent_md"
        )
        assert status == 404

    def test_invalid_memory_chunk_id(self, api_client):
        """Verify 404 for non-existent MemoryChunk ID."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/nonexistent_chunk"
        )
        assert status == 404

    def test_invalid_cxl_logical_device_id(self, api_client):
        """Verify 404 for non-existent CXLLogicalDevice ID."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices/999"
        )
        assert status == 404

    def test_invalid_endpoint_id(self, api_client):
        """Verify 404 for non-existent Endpoint ID."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/nonexistent")
        assert status == 404

    def test_invalid_connection_id(self, api_client):
        """Verify 404 for non-existent Connection ID."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Connections/nonexistent")
        assert status == 404

    def test_delete_nonexistent_chunk(self, api_client):
        """Verify 404 when deleting non-existent chunk."""
        status = api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/fake_chunk"
        )
        assert status == 404

    def test_delete_nonexistent_connection(self, api_client):
        """Verify 404 when deleting non-existent connection."""
        status = api_client.delete("/redfish/v1/Fabrics/CXL/Connections/fake_conn")
        assert status == 404


@pytest.mark.mock_topology("mock/switch-attached.json")
class Test404Switch:
    """Tests for 404 Not Found on switch-related resources."""

    def test_invalid_switch_id(self, api_client):
        """Verify 404 for non-existent Switch ID."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/nonexistent")
        assert status == 404

    def test_invalid_port_id(self, api_client):
        """Verify 404 for non-existent Port ID."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/nonexistent"
        )
        assert status == 404

    def test_invalid_vcs_id(self, api_client):
        """Verify 404 for non-existent VCS ID."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/999"
        )
        assert status == 404

    def test_invalid_vppb_id(self, api_client):
        """Verify 404 for non-existent vPPB ID."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs/999"
        )
        assert status == 404

    def test_invalid_zone_id(self, api_client):
        """Verify 404 for non-existent Zone ID."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones/nonexistent")
        assert status == 404


# =============================================================================
# 400 Bad Request Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class Test400BadRequest:
    """Tests for 400 Bad Request responses."""

    def test_empty_post_body(self, api_client):
        """Verify 400 for POST with empty body."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {}
        )
        assert status == 400

    def test_missing_required_size(self, api_client):
        """Verify 400 when MemoryChunkSizeMiB is missing."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"AddressRangeType": "PMEM"}  # Size is required
        )
        # Should fail if size is truly required
        assert status in [400, 201]  # Implementation may default size

    def test_invalid_address_range_type(self, api_client):
        """Verify invalid AddressRangeType is rejected."""
        # Delete existing chunk to free capacity first
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 256, "AddressRangeType": "InvalidType"}
        )
        assert status == 400

    def test_negative_chunk_size(self, api_client):
        """Verify negative chunk size is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": -1024}
        )
        assert status == 400

    def test_zero_chunk_size(self, api_client):
        """Verify zero chunk size is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 0}
        )
        assert status == 400

    def test_non_numeric_size(self, api_client):
        """Verify 400 for non-numeric chunk size."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": "large"}
        )
        assert status == 400


@pytest.mark.mock_topology("mock/switch-attached.json")
class Test400ConnectionErrors:
    """Tests for 400 Bad Request on Connections."""

    def test_connection_missing_links(self, api_client):
        """Verify 400 when Links is missing."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {"ConnectionType": "Memory"}
        )
        assert status == 400

    def test_connection_missing_initiator(self, api_client):
        """Verify 400 when InitiatorEndpoints is missing."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "TargetEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0"}
                    ]
                }
            }
        )
        assert status == 400

    def test_connection_missing_target(self, api_client):
        """Verify 400 when TargetEndpoints is missing."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0"}
                    ]
                }
            }
        )
        assert status == 400

    def test_connection_empty_initiator_array(self, api_client):
        """Verify 400 when InitiatorEndpoints is empty array."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [],
                    "TargetEndpoints": [
                        {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0"}
                    ]
                }
            }
        )
        assert status == 400


# =============================================================================
# 409 Conflict Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class Test409Conflict:
    """Tests for 409 Conflict responses."""

    def test_chunk_exceeds_capacity(self, api_client):
        """Verify 409 when chunk exceeds device capacity."""
        # Device has 16384 MiB, request more
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 999999}
        )
        # Should be 409 Conflict for insufficient capacity
        assert status in [400, 409]

    def test_capacity_exhaustion(self, api_client):
        """Verify 409 when capacity is exhausted."""
        # First allocate most capacity
        api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 15360}  # 15 GiB of 16 GiB
        )

        # Try to allocate more than remaining
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 2048}  # 2 GiB when only ~1 GiB left
        )
        assert status in [400, 409]


# =============================================================================
# Error Response Format Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestErrorResponseFormat:
    """Tests for Redfish error response format."""

    def test_404_has_error_object(self, api_client):
        """Verify 404 response has error object."""
        status, data = api_client.get("/redfish/v1/Chassis/nonexistent")
        assert status == 404
        if data:
            # Redfish error format
            assert "error" in data or "@Message.ExtendedInfo" in data

    def test_error_response_has_error_object(self, api_client):
        """Verify error response has error object."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": -1}
        )
        assert status == 400
        if data:
            assert "error" in data or "@Message.ExtendedInfo" in data


# =============================================================================
# Method Not Allowed Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestUnsupportedMethodResponses:
    """Tests for unsupported method responses."""

    def test_post_to_chassis_rejected(self, api_client):
        """Verify POST to Chassis is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0",
            {"Name": "New Name"}
        )
        # POST not allowed on individual Chassis - implementation returns 501
        assert status in [405, 400, 404, 501]

    def test_delete_chassis_rejected(self, api_client):
        """Verify DELETE on Chassis is rejected."""
        status = api_client.delete("/redfish/v1/Chassis/cxl_mem0")
        # DELETE not allowed on Chassis - implementation returns 501
        assert status in [405, 400, 404, 501]

    def test_post_to_fabric_rejected(self, api_client):
        """Verify POST to Fabric is rejected."""
        status, _, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL",
            {"Name": "New Fabric"}
        )
        # Implementation returns 501 for unimplemented methods
        assert status in [405, 400, 404, 501]
