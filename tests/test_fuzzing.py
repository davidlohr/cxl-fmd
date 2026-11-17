#!/usr/bin/env python3
"""
Fuzzing and edge-case tests for cxl-fmd.

These tests check for crashes, bugs, and unexpected behavior when the daemon
receives malformed or edge-case inputs.

Bugs found during fuzzing:
1. Body size >= 65536 bytes closes connection (Status 000) instead of returning error
2. Negative capacity in mock JSON causes integer underflow (becomes huge value)
3. Empty device ID in mock JSON is accepted, creating unnamed devices

Tests marked xfail are known issues that need to be fixed.
"""

import pytest
import json


# =============================================================================
# MALFORMED JSON TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestMalformedJSON:
    """Tests for malformed JSON handling."""

    def test_invalid_json_returns_400(self, api_client):
        """Invalid JSON syntax returns 400, not 500."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            None,  # Will send raw body
            raw_body="{invalid json}"
        )
        assert status == 400

    def test_empty_body_returns_400(self, api_client):
        """Empty request body returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            None,
            raw_body=""
        )
        assert status == 400

    def test_null_byte_in_json(self, api_client):
        """Null bytes in JSON are rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            None,
            raw_body='{"test": "foo\x00bar"}'
        )
        assert status == 400

    def test_deeply_nested_json(self, api_client):
        """Deeply nested JSON is handled safely."""
        nested = '{"a":' * 100 + '1' + '}' * 100
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            None,
            raw_body=nested
        )
        assert status in [400, 500]  # Either error is acceptable, no crash

    def test_many_keys_in_json(self, api_client):
        """JSON with many keys is handled safely."""
        obj = {"MemoryChunkSizeMiB": 512}
        for i in range(1000):
            obj[f"key{i}"] = f"value{i}"
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            obj
        )
        # Should either succeed or return error, not crash
        assert status in [201, 400, 409, 500]


# =============================================================================
# BOUNDARY VALUE TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestBoundaryValues:
    """Tests for boundary values and integer handling."""

    def test_negative_size_rejected(self, api_client):
        """Negative MemoryChunkSizeMiB is rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": -1}
        )
        # Should reject as invalid, not create huge chunk
        assert status == 400

    def test_huge_size_rejected(self, api_client):
        """Extremely large MemoryChunkSizeMiB is rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 9223372036854775807}  # INT64_MAX
        )
        assert status == 400

    def test_zero_size_rejected(self, api_client):
        """Zero MemoryChunkSizeMiB is rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 0}
        )
        assert status == 400

    def test_float_size_rejected(self, api_client):
        """Floating point MemoryChunkSizeMiB is rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1024.5}
        )
        assert status == 400

    def test_string_size_rejected(self, api_client):
        """String MemoryChunkSizeMiB is rejected."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": "large"}
        )
        assert status == 400


# =============================================================================
# BODY SIZE LIMIT TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestBodySizeLimit:
    """Tests for request body size limits."""

    def test_60kb_body_accepted(self, api_client):
        """60KB body is accepted."""
        large_value = "A" * 60000
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "test": large_value}
        )
        # Should process the request (may fail for other reasons)
        assert status in [201, 400, 409]

    def test_body_over_64kb_returns_413(self, api_client):
        """Body over 64KB returns 413 Payload Too Large."""
        # 70000 bytes ensures we exceed 64KB even with JSON overhead
        large_value = "A" * 70000
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "test": large_value}
        )
        # Should return 413 (Payload Too Large)
        assert status == 413


# =============================================================================
# TYPE VALIDATION TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestTypeValidation:
    """Tests for type checking in request bodies."""

    def test_array_instead_of_object(self, api_client):
        """Array where object expected returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            None,
            raw_body='{"Links": [1, 2, 3]}'
        )
        assert status == 400

    def test_int_for_string_field(self, api_client):
        """Integer for string field is handled gracefully."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "AddressRangeType": 12345}
        )
        assert status == 400


# =============================================================================
# MISSING FIELD TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestMissingFields:
    """Tests for missing required fields."""

    def test_connection_missing_links(self, api_client):
        """Connection without Links returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {"ConnectionType": "Memory"}
        )
        assert status == 400

    def test_connection_empty_links(self, api_client):
        """Connection with empty Links returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {"ConnectionType": "Memory", "Links": {}}
        )
        assert status == 400

    def test_connection_empty_arrays(self, api_client):
        """Connection with empty endpoint arrays returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {"Links": {"InitiatorEndpoints": [], "TargetEndpoints": []}}
        )
        assert status == 400

    def test_chunk_missing_size(self, api_client):
        """MemoryChunk without size returns 400."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"AddressRangeType": "PMEM"}
        )
        assert status == 400


# =============================================================================
# RESOURCE EXHAUSTION TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestResourceExhaustion:
    """Tests for resource exhaustion handling."""

    def test_create_chunks_until_exhaustion(self, api_client):
        """Creating chunks until capacity exhausted returns 409."""
        # Delete existing chunk
        api_client.delete(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0"
        )

        created = 0
        for i in range(20):
            status, data, _ = api_client.post(
                "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
                {"MemoryChunkSizeMiB": 256}
            )
            if status == 201:
                created += 1
            elif status == 409:
                break
            else:
                pytest.fail(f"Unexpected status {status}")

        # Should have created some chunks then failed with 409
        assert created > 0
        assert status == 409

    def test_oversized_chunk_rejected(self, api_client):
        """Chunk larger than device capacity returns 409."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1000000}  # 1TB
        )
        assert status == 409


# =============================================================================
# PATH/URL TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestURLHandling:
    """Tests for URL handling and path traversal."""

    def test_nonexistent_resource_returns_404(self, api_client):
        """Non-existent resource returns 404."""
        status, data = api_client.get("/redfish/v1/Chassis/NONEXISTENT")
        assert status == 404

    def test_path_traversal_rejected(self, api_client):
        """Path traversal attempts return 404."""
        status, data = api_client.get("/redfish/v1/../../../etc/passwd")
        assert status == 404

    def test_double_slash_handled(self, api_client):
        """Double slashes in path are handled."""
        status, data = api_client.get("/redfish/v1//Chassis")
        # May return 200 or 404, but not crash
        assert status in [200, 404]


# =============================================================================
# HTTP METHOD TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestHTTPMethods:
    """Tests for HTTP method handling."""

    def test_patch_returns_405(self, api_client):
        """PATCH of a Chassis, which takes none, is 405 with an Allow
        header (DSP0266 Section 7.5.3); it was 501."""
        import requests
        resp = requests.patch(
            f"{api_client.base_url}/redfish/v1/Chassis/sld_0",
            json={"Name": "test"}
        )
        assert resp.status_code == 405
        assert resp.headers["Allow"] == "GET"

    def test_options_returns_405(self, api_client):
        """OPTIONS returns 405."""
        import requests
        resp = requests.options(f"{api_client.base_url}/redfish/v1")
        assert resp.status_code == 405
        assert resp.headers["Allow"] == "GET"

    def test_trace_returns_405(self, api_client):
        """TRACE returns 405."""
        import requests
        resp = requests.request(
            "TRACE",
            f"{api_client.base_url}/redfish/v1"
        )
        assert resp.status_code == 405


# =============================================================================
# DOUBLE OPERATION TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestDoubleOperations:
    """Tests for repeated operations."""

    def test_double_delete_chunk(self, api_client):
        """Double delete of same chunk - second returns 404."""
        chunk_url = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0"

        # First delete should succeed
        status1 = api_client.delete(chunk_url)
        assert status1 == 204

        # Second delete should return 404
        status2 = api_client.delete(chunk_url)
        assert status2 == 404

    def test_get_after_delete(self, api_client):
        """GET after DELETE returns 404."""
        chunk_url = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0"

        api_client.delete(chunk_url)
        status, data = api_client.get(chunk_url)
        assert status == 404
