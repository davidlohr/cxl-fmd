# SPDX-License-Identifier: LGPL-2.1
"""
Query parameters and methods a resource does not take (review F18 claims
1 and 3).

DSP0266 1.25.0 Section 7.3.1: services return "The HTTP 501 Not
Implemented status code for any unsupported query parameters that start
with $" and "An extended error that indicates the unsupported query
parameters for this resource", and "Shall ignore unknown or unsupported
query parameters that do not begin with $". cxl-fmd supports none.

Section 7.5.3: "If the resource exists but does not support the requested
operation, services shall return the HTTP 405 Method Not Allowed status
code", and a 405 has an Allow header (Table 13).
"""

import pytest
import requests

CHASSIS = "/redfish/v1/Chassis"
CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"


def unsupported(response, *params):
    """
    A 501 whose extended error names each of @params, with the Message,
    Severity and Resolution of QueryParameterUnsupported in the DMTF Base
    registry (Base.1.24.0.json).
    """
    assert response.status_code == 501
    error = response.json()["error"]
    info = error["@Message.ExtendedInfo"]
    assert [m["MessageArgs"] for m in info] == [[p] for p in params]
    for m in info:
        assert m["MessageId"] == "Base.1.24.QueryParameterUnsupported"
        assert m["Message"] == f"Query parameter '{m['MessageArgs'][0]}' is not supported."
        assert m["Severity"] == "Warning"
        assert m["Resolution"] == \
            "Correct or remove the query parameter and resubmit the request."


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestQueryParameters:

    @pytest.mark.parametrize("query, param", [
        ("$filter=Id%20eq%20%27nonexistent%27", "$filter"),
        ("$top=0", "$top"),
        ("$skip=100", "$skip"),
        ("$select=Id", "$select"),
        ("$expand=*($levels=1)", "$expand"),
        ("$bogus=1", "$bogus"),
        ("%24top=1", "$top"),          # decoded before it is looked at
    ])
    def test_dollar_parameter_is_501(self, api_client, query, param):
        """Each was answered 200 with the whole collection."""
        response = requests.get(f"{api_client.base_url}{CHASSIS}?{query}", timeout=30)
        unsupported(response, param)

    def test_each_dollar_parameter_is_named(self, api_client):
        response = requests.get(
            f"{api_client.base_url}{CHASSIS}?$top=1&only&$skip=2", timeout=30)
        unsupported(response, "$top", "$skip")

    @pytest.mark.parametrize("query", ["only", "excerpt", "foo=1", "only="])
    def test_other_parameters_are_ignored(self, api_client, query):
        plain = requests.get(f"{api_client.base_url}{CHASSIS}", timeout=30)
        response = requests.get(f"{api_client.base_url}{CHASSIS}?{query}", timeout=30)
        assert response.status_code == 200
        assert response.json() == plain.json()

    def test_metadata_is_a_path(self, api_client):
        """$metadata is in the path, not a query parameter."""
        response = requests.get(f"{api_client.base_url}/redfish/v1/$metadata", timeout=30)
        assert response.status_code == 200

    def test_no_protocol_features(self, api_client):
        """Nothing is advertised that is not supported."""
        status, root = api_client.get("/redfish/v1")
        assert status == 200
        assert "ProtocolFeaturesSupported" not in root

    def test_post_with_dollar_parameter_does_nothing(self, api_client):
        """A POST was carried out with the parameter ignored."""
        response = requests.post(f"{api_client.base_url}{CONNECTIONS}?$filter=x",
                                 json={}, timeout=30)
        unsupported(response, "$filter")
        status, data = api_client.get(CONNECTIONS)
        assert status == 200 and data["Members@odata.count"] == 0


def allowed(response):
    assert response.status_code == 405
    return {m.strip() for m in response.headers["Allow"].split(",")}


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestMethodNotAllowed:

    @pytest.mark.parametrize("method, uri, allow", [
        ("PUT", CHASSIS, {"GET"}),
        ("DELETE", CHASSIS, {"GET"}),
        ("POST", CHASSIS, {"GET"}),
        ("PATCH", CHASSIS + "/sld_0", {"GET"}),
        ("DELETE", CHASSIS + "/sld_0", {"GET"}),
        ("POST", "/redfish/v1/Fabrics/CXL", {"GET"}),
        ("PUT", CHASSIS + "/sld_0/Memory/sld_0", {"GET", "PATCH"}),
        ("DELETE", CHASSIS + "/sld_0/PCIeDevices/sld_0", {"GET", "PATCH"}),
        ("DELETE", CHASSIS + "/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks",
         {"GET", "POST"}),
        ("PATCH", CHASSIS + "/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0",
         {"GET", "DELETE"}),
        ("DELETE", CONNECTIONS, {"GET", "POST"}),
        ("HEAD", "/redfish/v1", {"GET"}),
        ("OPTIONS", "/redfish/v1", {"GET"}),
    ])
    def test_existing_resource_is_405(self, api_client, method, uri, allow):
        """Each was 501, or a 405 with no Allow header (HEAD, OPTIONS)."""
        response = requests.request(method, api_client.base_url + uri,
                                    json={} if method in ("POST", "PUT", "PATCH") else None,
                                    timeout=30)
        assert allowed(response) == allow

    @pytest.mark.parametrize("method, uri", [
        ("PUT", "/redfish/v1/Nonexistent"),
        ("DELETE", CHASSIS + "/nonexistent"),
        ("PATCH", CHASSIS + "/nonexistent"),
        ("POST", CHASSIS + "/sld_0/Actions/Chassis.Nonexistent"),
        ("PATCH", CONNECTIONS + "/nonexistent"),
    ])
    def test_unknown_resource_is_404(self, api_client, method, uri):
        response = requests.request(method, api_client.base_url + uri, json={},
                                    timeout=30)
        assert response.status_code == 404

    def test_switch_connection_takes_delete(self, api_client):
        response = requests.post(api_client.base_url + CONNECTIONS, timeout=30, json={
            "ConnectionType": "Memory",
            "Links": {
                "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"}],
                "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]},
            "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id":
                CHASSIS + "/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0"}}]})
        assert response.status_code == 201
        conn = response.headers["Location"]
        assert allowed(requests.patch(api_client.base_url + conn, json={},
                                      timeout=30)) == {"GET", "DELETE"}


@pytest.mark.mock_topology("mock/unit/unit-direct-sld.json")
class TestDirectConnection:

    def test_direct_connection_takes_get_only(self, api_client):
        """A DELETE of one is 405 "no vPPB to unbind": its PATCH says so too
        (it said GET, DELETE)."""
        conn = api_client.base_url + CONNECTIONS + "/local_direct_sld_0"
        assert allowed(requests.patch(conn, json={}, timeout=30)) == {"GET"}
        assert allowed(requests.delete(conn, timeout=30)) == {"GET"}
        assert allowed(requests.put(conn, json={}, timeout=30)) == {"GET"}

    def test_host_chunk_takes_get_only(self, api_client):
        uri = "/redfish/v1/Systems/local/MemoryDomains"
        status, domains = api_client.get(uri)
        domain = domains["Members"][0]["@odata.id"]
        status, chunks = api_client.get(domain + "/MemoryChunks")
        chunk = chunks["Members"][0]["@odata.id"]
        assert allowed(requests.delete(api_client.base_url + chunk, timeout=30)) == {"GET"}
