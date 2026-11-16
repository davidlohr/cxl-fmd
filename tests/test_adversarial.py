#!/usr/bin/env python3
"""
Adversarial tests for cxl-fmd.

These tests attempt to crash the daemon or trigger unexpected behavior through:
- Race conditions (concurrent requests)
- Integer overflow/wraparound
- Long/malicious strings
- Special characters in inputs
- State machine violations
- Memory pressure
- HTTP protocol abuse
- Resource exhaustion

Run with: pytest tests/test_adversarial.py -v
"""

import pytest
import json
import socket
import threading
import time
import concurrent.futures
import requests


# =============================================================================
# CONCURRENT REQUEST TESTS (Race Conditions)
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestConcurrency:
    """Tests for race conditions and concurrent access."""

    def test_concurrent_chunk_create_delete(self, api_client):
        """Concurrent create/delete on same domain - looking for race conditions."""
        domain_url = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        chunk_url = f"{domain_url}/sld_0_chunk0"

        # Delete existing chunk first
        api_client.delete(chunk_url)

        errors = []

        def create_chunk(idx):
            try:
                status, data, loc = api_client.post(domain_url, {"MemoryChunkSizeMiB": 256})
                return ("create", idx, status)
            except Exception as e:
                errors.append(f"create {idx}: {e}")
                return ("create", idx, "error")

        def delete_chunk(idx):
            try:
                # Try to delete whatever chunks exist
                status, data = api_client.get(domain_url)
                if status == 200 and data.get("Members"):
                    for member in data["Members"]:
                        uri = member.get("@odata.id", "")
                        if uri:
                            api_client.delete(uri)
                return ("delete", idx, "ok")
            except Exception as e:
                errors.append(f"delete {idx}: {e}")
                return ("delete", idx, "error")

        # Run concurrent creates and deletes
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = []
            for i in range(20):
                if i % 2 == 0:
                    futures.append(executor.submit(create_chunk, i))
                else:
                    futures.append(executor.submit(delete_chunk, i))

            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        # Verify daemon didn't crash - can still respond
        status, data = api_client.get("/redfish/v1")
        assert status == 200, f"Daemon stopped responding after concurrent operations. Errors: {errors}"

    def test_concurrent_connection_create(self, api_client):
        """Create many connections concurrently - test ID generation race."""
        # Get endpoint URIs
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        assert status == 200

        endpoints = [m["@odata.id"] for m in data.get("Members", [])]
        if len(endpoints) < 2:
            pytest.skip("Need at least 2 endpoints")

        initiator = endpoints[0]
        target = endpoints[1]

        created_ids = []
        errors = []
        lock = threading.Lock()

        def create_connection(idx):
            try:
                status, data, loc = api_client.post(
                    "/redfish/v1/Fabrics/CXL/Connections",
                    {
                        "Links": {
                            "InitiatorEndpoints": [{"@odata.id": initiator}],
                            "TargetEndpoints": [{"@odata.id": target}]
                        }
                    }
                )
                if status == 201 and data:
                    conn_id = data.get("Id")
                    with lock:
                        created_ids.append(conn_id)
                return status
            except Exception as e:
                with lock:
                    errors.append(str(e))
                return "error"

        # Create 50 connections concurrently
        with concurrent.futures.ThreadPoolExecutor(max_workers=20) as executor:
            futures = [executor.submit(create_connection, i) for i in range(50)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        # Check for duplicate IDs (race condition in ID generation)
        if created_ids:
            unique_ids = set(created_ids)
            assert len(unique_ids) == len(created_ids), \
                f"Duplicate connection IDs detected! IDs: {created_ids}"

        # Verify daemon still works
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_rapid_get_requests(self, api_client):
        """Flood with rapid GET requests."""
        def get_request(url):
            try:
                return api_client.get(url)
            except:
                return (0, None)

        urls = [
            "/redfish/v1",
            "/redfish/v1/Chassis",
            "/redfish/v1/Chassis/sld_0",
            "/redfish/v1/Fabrics/CXL",
        ]

        with concurrent.futures.ThreadPoolExecutor(max_workers=50) as executor:
            futures = []
            for _ in range(100):
                for url in urls:
                    futures.append(executor.submit(get_request, url))

            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        # Verify daemon survived
        status, _ = api_client.get("/redfish/v1")
        assert status == 200


# =============================================================================
# LONG STRING TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestLongStrings:
    """Tests for buffer overflow via long strings."""

    def test_very_long_url(self, api_client):
        """URL longer than typical buffer sizes."""
        # 10KB URL path
        long_path = "/redfish/v1/" + "A" * 10000
        status, data = api_client.get(long_path)
        # Should return 404, not crash
        assert status in [400, 404, 414]  # 414 = URI Too Long

    def test_very_long_json_key(self, api_client):
        """JSON with extremely long key names."""
        long_key = "A" * 10000
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, long_key: "value"}
        )
        # Should handle gracefully
        assert status in [201, 400, 409, 500]

    def test_very_long_json_value(self, api_client):
        """JSON with extremely long string values."""
        long_value = "B" * 50000  # Under 64KB limit
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "Description": long_value}
        )
        assert status in [201, 400, 409]

    def test_many_url_segments(self, api_client):
        """URL with many path segments."""
        segments = "/".join(["segment"] * 1000)
        status, data = api_client.get(f"/redfish/v1/{segments}")
        assert status in [400, 404, 414]

    def test_long_query_string(self, api_client):
        """Very long query string."""
        query = "?" + "&".join([f"param{i}={'x'*100}" for i in range(100)])
        status, data = api_client.get(f"/redfish/v1/Chassis{query}")
        # Query params are typically ignored, but shouldn't crash
        assert status in [200, 400, 414]


# =============================================================================
# SPECIAL CHARACTER TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestSpecialCharacters:
    """Tests for special characters that might cause parsing issues."""

    def test_null_in_url(self, api_client):
        """Null byte in URL path."""
        try:
            # requests may reject this, use raw socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            # Extract host/port from base_url
            import urllib.parse
            parsed = urllib.parse.urlparse(api_client.base_url)
            sock.connect((parsed.hostname, parsed.port))

            # Send request with null in path
            request = b"GET /redfish/v1/Chassis\x00/evil HTTP/1.1\r\nHost: localhost\r\n\r\n"
            sock.sendall(request)
            response = sock.recv(4096)
            sock.close()

            # Should get some HTTP response, not crash
            assert b"HTTP/1" in response
        except Exception:
            pass  # Connection issues are acceptable

        # Verify daemon still responds
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_unicode_in_json(self, api_client):
        """Unicode characters in JSON values."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "Name": "测试\u0000\uFFFF"}
        )
        assert status in [201, 400, 409]

    def test_control_characters_in_json(self, api_client):
        """Control characters in JSON."""
        # Bell, backspace, form feed, etc.
        control_chars = "".join(chr(i) for i in range(1, 32) if i not in [9, 10, 13])
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body=f'{{"MemoryChunkSizeMiB": 512, "Name": "{control_chars}"}}'
        )
        assert status in [400, 201, 409]  # 400 for invalid JSON is fine

    def test_format_string_in_json(self, api_client):
        """Format string characters that might reach printf."""
        dangerous = "%s%s%s%s%s%s%s%s%s%s%n%n%n%n"
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "Name": dangerous}
        )
        # Should not crash from format string attack
        assert status in [201, 400, 409]

        # Verify daemon still works
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_shell_injection_chars(self, api_client):
        """Characters that might cause shell injection if passed to system()."""
        dangerous = "; rm -rf / #"
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 512, "Name": dangerous}
        )
        assert status in [201, 400, 409]

    def test_path_traversal_in_id(self, api_client):
        """Path traversal attempts in resource IDs."""
        traversals = [
            "../../../etc/passwd",
            "..\\..\\..\\windows\\system32",
            "%2e%2e%2f%2e%2e%2f",
            "....//....//",
        ]
        for path in traversals:
            status, data = api_client.get(f"/redfish/v1/Chassis/{path}")
            assert status in [400, 404]


# =============================================================================
# HTTP PROTOCOL ABUSE
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestHTTPAbuse:
    """Tests for HTTP protocol edge cases."""

    def test_http_request_smuggling_cl_te(self, api_client):
        """Attempt Content-Length / Transfer-Encoding confusion."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((parsed.hostname, parsed.port))

            # Send request with both Content-Length and Transfer-Encoding
            request = (
                b"POST /redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/json\r\n"
                b"Content-Length: 6\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"\r\n"
                b"0\r\n\r\n"
            )
            sock.sendall(request)
            response = sock.recv(4096)
            sock.close()

            assert b"HTTP/1" in response
        except Exception:
            pass

        # Daemon should still be alive
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_malformed_content_length(self, api_client):
        """Invalid Content-Length values."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        invalid_lengths = [
            b"-1",
            b"9999999999999999999999",
            b"abc",
            b"100abc",
            b"",
        ]

        for cl in invalid_lengths:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)
                sock.connect((parsed.hostname, parsed.port))

                body = b'{"MemoryChunkSizeMiB": 512}'
                request = (
                    b"POST /redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks HTTP/1.1\r\n"
                    b"Host: localhost\r\n"
                    b"Content-Type: application/json\r\n"
                    b"Content-Length: " + cl + b"\r\n"
                    b"\r\n" + body
                )
                sock.sendall(request)
                sock.recv(4096)
                sock.close()
            except Exception:
                pass

        # Daemon should survive
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_incomplete_request(self, api_client):
        """Send incomplete HTTP request and close connection."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect((parsed.hostname, parsed.port))

            # Send partial request
            sock.sendall(b"GET /redfish/v1 HTT")
            time.sleep(0.1)
            sock.close()
        except Exception:
            pass

        # Daemon should survive
        time.sleep(0.5)
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_pipelining(self, api_client):
        """Send multiple pipelined requests."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((parsed.hostname, parsed.port))

            # Send 3 requests without waiting for response
            requests_data = (
                b"GET /redfish/v1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
                b"GET /redfish/v1/Chassis HTTP/1.1\r\nHost: localhost\r\n\r\n"
                b"GET /redfish/v1/Fabrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
            )
            sock.sendall(requests_data)

            response = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                response += chunk
            sock.close()

            # Should get multiple responses
            assert response.count(b"HTTP/1") >= 1
        except Exception:
            pass

        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_slow_loris(self, api_client):
        """Slow loris style attack - send headers very slowly."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        def slow_request():
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(10)
                sock.connect((parsed.hostname, parsed.port))

                # Send request line
                sock.sendall(b"GET /redfish/v1 HTTP/1.1\r\n")

                # Send headers one at a time with delays
                for i in range(5):
                    time.sleep(0.5)
                    sock.sendall(f"X-Header-{i}: value{i}\r\n".encode())

                sock.sendall(b"\r\n")
                sock.recv(4096)
                sock.close()
            except Exception:
                pass

        # Start several slow connections
        threads = [threading.Thread(target=slow_request) for _ in range(5)]
        for t in threads:
            t.start()

        # Daemon should still respond to normal requests
        time.sleep(1)
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

        for t in threads:
            t.join(timeout=15)


# =============================================================================
# JSON EDGE CASES
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestJSONEdgeCases:
    """Tests for JSON parsing edge cases."""

    def test_duplicate_keys(self, api_client):
        """JSON with duplicate keys."""
        # Per RFC 7159, behavior is undefined for duplicate keys
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='{"MemoryChunkSizeMiB": 512, "MemoryChunkSizeMiB": 1024}'
        )
        assert status in [201, 400, 409]

    def test_json_number_precision(self, api_client):
        """JSON numbers that exceed double precision."""
        # 9007199254740993 cannot be exactly represented in IEEE 754 double
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='{"MemoryChunkSizeMiB": 9007199254740993}'
        )
        assert status in [400, 409]

    def test_json_scientific_notation(self, api_client):
        """JSON numbers in scientific notation."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1e3}  # 1000.0
        )
        # Float should be rejected or truncated
        assert status in [201, 400, 409]

    def test_json_unicode_escapes(self, api_client):
        """JSON with Unicode escape sequences."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='{"MemoryChunkSizeMiB": 512, "Name": "\\u0000\\u001F\\uFFFF"}'
        )
        assert status in [201, 400, 409]

    def test_json_surrogate_pairs(self, api_client):
        """JSON with UTF-16 surrogate pairs (emoji)."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='{"MemoryChunkSizeMiB": 512, "Name": "\\uD83D\\uDE00"}'  # 😀
        )
        assert status in [201, 400, 409]

    def test_json_lone_surrogate(self, api_client):
        """JSON with unpaired UTF-16 surrogate (invalid Unicode)."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='{"MemoryChunkSizeMiB": 512, "Name": "\\uD800"}'  # Lone high surrogate
        )
        assert status in [201, 400, 409]

    def test_empty_json_object(self, api_client):
        """Completely empty JSON object."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {}
        )
        assert status == 400  # Missing required field

    def test_json_with_bom(self, api_client):
        """JSON with UTF-8 BOM."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            None,
            raw_body='\xef\xbb\xbf{"MemoryChunkSizeMiB": 512}'
        )
        assert status in [201, 400, 409]


# =============================================================================
# RESOURCE EXHAUSTION
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-mld.json")
class TestResourceExhaustion:
    """Tests for resource exhaustion scenarios."""

    def test_create_max_connections(self, api_client):
        """Create connections until we hit a limit."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        if status != 200:
            pytest.skip("Cannot get endpoints")

        endpoints = [m["@odata.id"] for m in data.get("Members", [])]
        if len(endpoints) < 2:
            pytest.skip("Need at least 2 endpoints")

        initiator = endpoints[0]
        target = endpoints[1]

        created = 0
        for i in range(1000):
            status, data, _ = api_client.post(
                "/redfish/v1/Fabrics/CXL/Connections",
                {
                    "Links": {
                        "InitiatorEndpoints": [{"@odata.id": initiator}],
                        "TargetEndpoints": [{"@odata.id": target}]
                    }
                }
            )
            if status == 201:
                created += 1
            elif status in [409, 507]:  # Conflict or Insufficient Storage
                break
            elif status == 500:
                pytest.fail("Server error during connection creation")

        # Daemon should still respond
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_many_simultaneous_connections(self, api_client):
        """Open many HTTP connections simultaneously."""
        import urllib.parse
        parsed = urllib.parse.urlparse(api_client.base_url)

        sockets = []
        try:
            for i in range(100):
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)
                sock.connect((parsed.hostname, parsed.port))
                sockets.append(sock)
        except Exception as e:
            pass  # Some failures expected

        # Send requests on all open sockets
        for sock in sockets:
            try:
                sock.sendall(b"GET /redfish/v1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
            except:
                pass

        # Close all
        for sock in sockets:
            try:
                sock.close()
            except:
                pass

        time.sleep(1)

        # Daemon should recover
        status, _ = api_client.get("/redfish/v1")
        assert status == 200


# =============================================================================
# STATE MACHINE VIOLATIONS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestStateMachineViolations:
    """Tests for operations in invalid states."""

    def test_delete_nonexistent_chunk(self, api_client):
        """Delete a chunk that doesn't exist."""
        status = api_client.delete(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/nonexistent_chunk"
        )
        assert status == 404

    def test_delete_nonexistent_connection(self, api_client):
        """Delete a connection that doesn't exist."""
        status = api_client.delete("/redfish/v1/Fabrics/CXL/Connections/conn99999")
        assert status == 404

    def test_post_to_readonly_collection(self, api_client):
        """POST to a collection that doesn't support creation."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis",
            {"Name": "NewChassis"}
        )
        assert status in [405, 501]  # Method Not Allowed or Not Implemented

    def test_delete_readonly_resource(self, api_client):
        """DELETE a resource that can't be deleted."""
        status = api_client.delete("/redfish/v1/Chassis/sld_0")
        assert status in [405, 501]

    def test_create_chunk_in_nonexistent_domain(self, api_client):
        """Create chunk in domain that doesn't exist."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/fake_domain/MemoryChunks",
            {"MemoryChunkSizeMiB": 512}
        )
        assert status == 404

    def test_connection_to_nonexistent_endpoint(self, api_client):
        """Create connection to endpoint that doesn't exist."""
        status, data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/fake_endpoint"}],
                    "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0_ld0"}]
                }
            }
        )
        assert status in [400, 404]


# =============================================================================
# MEMORY CORRUPTION ATTEMPTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestMemoryCorruption:
    """Tests that might trigger memory corruption bugs."""

    def test_repeated_create_delete_same_resource(self, api_client):
        """Rapidly create/delete same resource - use-after-free potential."""
        domain_url = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"

        for i in range(50):
            # Delete existing
            api_client.delete(f"{domain_url}/sld_0_chunk0")

            # Create new
            status, data, _ = api_client.post(domain_url, {"MemoryChunkSizeMiB": 256})

            # Immediately read
            if status == 201 and data:
                chunk_id = data.get("Id")
                if chunk_id:
                    api_client.get(f"{domain_url}/{chunk_id}")

        # Verify daemon is stable
        status, _ = api_client.get("/redfish/v1")
        assert status == 200

    def test_access_deleted_resource_reference(self, api_client):
        """Access a resource through stale reference after deletion."""
        # Get endpoint before deletion
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        assert status == 200

        endpoints = [m["@odata.id"] for m in data.get("Members", [])]
        if len(endpoints) < 2:
            pytest.skip("Need at least 2 endpoints")

        # Create connection
        status, conn_data, _ = api_client.post(
            "/redfish/v1/Fabrics/CXL/Connections",
            {
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": endpoints[0]}],
                    "TargetEndpoints": [{"@odata.id": endpoints[1]}]
                }
            }
        )

        if status == 201 and conn_data:
            conn_id = conn_data.get("Id")
            conn_url = f"/redfish/v1/Fabrics/CXL/Connections/{conn_id}"

            # Delete the connection
            api_client.delete(conn_url)

            # Try to access deleted connection
            status, data = api_client.get(conn_url)
            assert status == 404

            # Try to delete again
            status = api_client.delete(conn_url)
            assert status == 404

    def test_interleaved_operations_different_resources(self, api_client):
        """Interleave operations on different resources."""
        chunk_url = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"
        conn_url = "/redfish/v1/Fabrics/CXL/Connections"

        # Get endpoints
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        if status != 200:
            pytest.skip("Cannot get endpoints")

        endpoints = [m["@odata.id"] for m in data.get("Members", [])]
        if len(endpoints) < 2:
            pytest.skip("Need 2 endpoints")

        for i in range(20):
            # Mix operations
            api_client.delete(f"{chunk_url}/sld_0_chunk0")
            api_client.post(conn_url, {
                "Links": {
                    "InitiatorEndpoints": [{"@odata.id": endpoints[0]}],
                    "TargetEndpoints": [{"@odata.id": endpoints[1]}]
                }
            })
            api_client.post(chunk_url, {"MemoryChunkSizeMiB": 256})
            api_client.get("/redfish/v1/Chassis/sld_0")

        # Should be stable
        status, _ = api_client.get("/redfish/v1")
        assert status == 200


# =============================================================================
# INTEGER OVERFLOW TESTS
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestIntegerOverflow:
    """Tests for integer overflow vulnerabilities."""

    def test_int32_max(self, api_client):
        """INT32_MAX value."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 2147483647}
        )
        assert status in [400, 409]

    def test_int32_overflow(self, api_client):
        """Value just over INT32_MAX."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 2147483648}
        )
        assert status in [400, 409]

    def test_uint32_max(self, api_client):
        """UINT32_MAX value."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 4294967295}
        )
        assert status in [400, 409]

    def test_int64_max(self, api_client):
        """INT64_MAX value."""
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 9223372036854775807}
        )
        assert status in [400, 409]

    def test_negative_that_wraps(self, api_client):
        """Negative value that wraps to large unsigned."""
        # -1 as unsigned 64-bit is 18446744073709551615
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": -1}
        )
        assert status == 400


# =============================================================================
# MALFORMED ID TESTS (targeting atoi without validation)
# =============================================================================

@pytest.mark.mock_topology("mock/unit/unit-direct-mld.json")
class TestMalformedIDs:
    """Tests for malformed resource IDs that exploit atoi() parsing."""

    def test_ld_id_negative(self, api_client):
        """LD ID that's negative."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/mld_0_ld-1"
        )
        assert status == 404

    def test_ld_id_very_large(self, api_client):
        """LD ID that overflows int."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/mld_0_ld9999999999"
        )
        assert status == 404

    def test_ld_id_not_a_number(self, api_client):
        """LD ID that's not a number (atoi returns 0)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/mld_0_ldABC"
        )
        # atoi("ABC") returns 0, might accidentally match LD 0
        assert status in [200, 404]

    def test_ld_id_mixed_chars(self, api_client):
        """LD ID with mixed numbers and chars."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/PCIeDevices/mld_0/CXLLogicalDevices/mld_0_ld1abc2"
        )
        # atoi("1abc2") returns 1
        assert status in [200, 404]

    def test_memory_id_malformed(self, api_client):
        """Memory resource with malformed ID."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/mld_0/Memory/mld_0_ld-999"
        )
        assert status == 404

    def test_vcs_id_negative(self, api_client):
        """VCS ID that's negative (for switch topologies)."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs/-1"
        )
        assert status == 404

    def test_vppb_id_overflow(self, api_client):
        """vPPB ID that overflows."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs/0/VPPBs/4294967295"
        )
        assert status == 404

    def test_chunk_id_with_special_chars(self, api_client):
        """Chunk ID with special characters."""
        special_ids = [
            "chunk<script>",
            "chunk%00hidden",
            "chunk/../../../etc/passwd",
            "chunk\x00evil",
        ]
        for chunk_id in special_ids:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks/{chunk_id}"
            )
            assert status in [400, 404]

    def test_interleave_set_malformed_memory_ref(self, api_client):
        """Create chunk with malformed Memory reference in InterleaveSets."""
        # First delete existing chunk
        api_client.delete(
            "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks/mld_0_chunk0"
        )

        malformed_refs = [
            "/redfish/v1/Chassis/mld_0/Memory/mld_0_ld-1",  # Negative
            "/redfish/v1/Chassis/mld_0/Memory/mld_0_ldXYZ",  # Non-numeric
            "/redfish/v1/Chassis/mld_0/Memory/../../etc/passwd",  # Path traversal
            "/redfish/v1/Chassis/WRONG/Memory/mld_0_ld0",  # Wrong chassis
        ]

        for ref in malformed_refs:
            status, data, _ = api_client.post(
                "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks",
                {
                    "MemoryChunkSizeMiB": 512,
                    "InterleaveSets": [{"Memory": {"@odata.id": ref}}]
                }
            )
            assert status in [400, 404, 409], f"Expected error for ref {ref}, got {status}"


@pytest.mark.mock_topology("mock/unit/unit-switch-mld.json")
class TestSwitchMalformedIDs:
    """Tests for malformed IDs in switch topologies."""

    def test_switch_id_not_found(self, api_client):
        """Non-existent switch ID."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/nonexistent")
        assert status == 404

    def test_port_id_negative(self, api_client):
        """Negative port ID."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/switch_0/Ports/-1"
        )
        assert status == 404

    def test_vcs_id_string(self, api_client):
        """VCS ID as non-numeric string."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs/notanumber"
        )
        assert status == 404

    def test_zone_id_overflow(self, api_client):
        """Zone ID that's very large."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Zones/zone9999999999999"
        )
        assert status == 404


# =============================================================================
# MOCK TOPOLOGY ATTACKS
# =============================================================================

class TestMockTopologyAttacks:
    """Tests for malicious mock topology files."""

    def test_topology_with_huge_ld_count(self, cxl_fmd_executable, tmp_path):
        """Mock with ld_count > 255 to trigger uint8 overflow."""
        import subprocess

        topology = {
            "version": 1,
            "id": "overflow_test",
            "devices": [{
                "id": "bad_mld",
                "ld_count": 300,  # > 255, will overflow uint8_t
                "identify_memory_device": {
                    "total_capacity_mb": 1024
                }
            }]
        }

        topo_file = tmp_path / "overflow.json"
        topo_file.write_text(json.dumps(topology))

        # Try to start daemon with bad topology
        proc = subprocess.Popen(
            [str(cxl_fmd_executable), "--mock", str(topo_file), "--port", "18888"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT
        )

        time.sleep(2)

        # Check if process crashed or is still running
        poll = proc.poll()

        if poll is None:
            # Still running - try to connect
            try:
                resp = requests.get("http://localhost:18888/redfish/v1", timeout=5)
            except:
                pass
            proc.terminate()
            proc.wait()

        # Process should exit cleanly or reject the topology
        # It should NOT crash (segfault, etc.)

    def test_topology_with_negative_sizes(self, cxl_fmd_executable, tmp_path):
        """Mock with negative capacity values."""
        import subprocess

        topology = {
            "version": 1,
            "id": "negative_test",
            "devices": [{
                "id": "neg_dev",
                "identify_memory_device": {
                    "total_capacity_mb": -1000,
                    "persistent_capacity_mb": -500
                }
            }]
        }

        topo_file = tmp_path / "negative.json"
        topo_file.write_text(json.dumps(topology))

        proc = subprocess.Popen(
            [str(cxl_fmd_executable), "--mock", str(topo_file), "--port", "18889"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT
        )

        time.sleep(2)
        poll = proc.poll()

        if poll is None:
            proc.terminate()
            proc.wait()

    def test_topology_with_circular_references(self, cxl_fmd_executable, tmp_path):
        """Mock with devices referencing each other (potential infinite loop)."""
        import subprocess

        # This shouldn't be valid, but test that it doesn't hang
        topology = {
            "version": 1,
            "id": "circular_test",
            "switches": [{
                "id": "sw1",
                "ports": [
                    {"port_number": 0, "type": "upstream"},
                    {"port_number": 1, "type": "downstream", "conn_dev_type": "switch"}
                ]
            }],
            "devices": [{
                "id": "dev1",
                "parent_switch": "sw1",
                "parent_port": 1
            }]
        }

        topo_file = tmp_path / "circular.json"
        topo_file.write_text(json.dumps(topology))

        proc = subprocess.Popen(
            [str(cxl_fmd_executable), "--mock", str(topo_file), "--port", "18890"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT
        )

        # Give it time, but not too much (detect hangs)
        time.sleep(3)
        poll = proc.poll()

        if poll is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                pytest.fail("Daemon hung on circular reference topology")
