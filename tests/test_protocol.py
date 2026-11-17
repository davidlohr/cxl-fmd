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

from test_dcd import dc_delete

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


# Conditional requests (review F18 claim 4)
DCD = "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0"
REGION = DCD + "/CXLLogicalDevices/0/MemoryRegions/0"
EXTENT = DCD + "/MemoryExtents/ext_0_400000000"
SLD_CHUNKS = "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
PRECONDITIONS = [{"If-Match": '"stale-etag-from-earlier-read"'},
                 {"If-Match": "*"},
                 {"If-None-Match": "*"},
                 {"If-None-Match": '"x"'}]


def precondition_failed(response):
    """
    412: "Precondition check, such as check of the OData-Version, If-Match,
    or If-None-Match header, failed." (DSP0266 1.25.0 Table 14). cxl-fmd
    returns no ETags, and cannot compare one.
    """
    assert response.status_code == 412, response.text
    info = response.json()["error"]["@Message.ExtendedInfo"]
    assert [m["MessageId"] for m in info] == ["Base.1.24.GeneralError"]
    assert "If-Match" in info[0]["Resolution"]


@pytest.mark.mock_topology("mock/dcd-device.json")
class TestPreconditionsDCD:

    @pytest.mark.parametrize("headers", PRECONDITIONS)
    def test_patch_with_precondition_changes_nothing(self, api_client, headers):
        """It was 200, and Set DC Region Configuration sent, whatever the
        ETag asked for (there are none)."""
        assert dc_delete(api_client, EXTENT) == 204
        response = requests.patch(api_client.base_url + REGION,
                                  json={"BlockSizeMiB": 512},
                                  headers=headers, timeout=30)
        precondition_failed(response)
        status, data = api_client.get(REGION)
        assert status == 200 and data["BlockSizeMiB"] == 256
        # without one, it is carried out
        response = requests.patch(api_client.base_url + REGION,
                                  json={"BlockSizeMiB": 512}, timeout=30)
        assert response.status_code == 200
        assert api_client.get(REGION)[1]["BlockSizeMiB"] == 512

    def test_delete_with_precondition_changes_nothing(self, api_client):
        response = requests.delete(api_client.base_url + EXTENT,
                                   headers={"If-Match": "*"}, timeout=30)
        precondition_failed(response)
        assert api_client.get(EXTENT)[0] == 200

    def test_action_with_precondition(self, api_client):
        response = requests.post(
            api_client.base_url + DCD + "/CXLLogicalDevices/0/Actions/"
            "CXLLogicalDevice.DynamicCapacityRelease",
            json={"Policy": "Free", "ForcedRemoval": False,
                  "SanitizeOnRelease": False},
            headers={"If-Match": '"x"'}, timeout=30)
        precondition_failed(response)
        assert api_client.get(EXTENT)[0] == 200

    def test_get_with_precondition_is_a_get(self, api_client):
        """A conditional GET is "If supported by the service" (Section
        7.2.1): cxl-fmd answers it as one without."""
        response = requests.get(api_client.base_url + REGION,
                                headers={"If-None-Match": '"x"'}, timeout=30)
        assert response.status_code == 200


@pytest.mark.mock_topology("mock/single-sld.json")
class TestPreconditionsSLD:

    def test_post_with_precondition_makes_nothing(self, api_client):
        status, before = api_client.get(SLD_CHUNKS)
        response = requests.post(api_client.base_url + SLD_CHUNKS,
                                 json={"MemoryChunkSizeMiB": 256},
                                 headers={"If-None-Match": "*"}, timeout=30)
        precondition_failed(response)
        assert api_client.get(SLD_CHUNKS)[1]["Members"] == before["Members"]

    def test_not_found_and_not_allowed_win(self, api_client):
        """'A server MUST ignore all received preconditions if its response
        to the same request without those conditions ... would have been a
        status code other than a 2xx (Successful) or 412' (RFC 9110 Section
        13.2.1)"""
        h = {"If-Match": "*"}
        base = api_client.base_url
        assert requests.patch(base + "/redfish/v1/Chassis/nonexistent", json={},
                              headers=h, timeout=30).status_code == 404
        assert requests.delete(base + SLD_CHUNKS + "/nonexistent",
                               headers=h, timeout=30).status_code == 404
        assert requests.put(base + CHASSIS, json={}, headers=h,
                            timeout=30).status_code == 405
        assert requests.post(base + CHASSIS, json={}, headers=h,
                             timeout=30).status_code == 405


# Response headers every response has (review F18.6, the DMTF
# Redfish-Protocol-Validator's RESP_HEADERS_CACHE_CONTROL)
def cache_control(response):
    """
    Cache-Control "Shall be supported and indicates whether a response can
    or cannot be cached" (DSP0266 1.25.0 Table 13, required); no-store is
    cxl-fmd's choice.
    """
    assert response.headers.get("Cache-Control") == "no-store", \
        (response.request.method, response.request.url, response.status_code)
    assert response.headers.get("OData-Version") == "4.0"


@pytest.mark.mock_topology("mock/single-sld.json")
class TestCacheControlSLD:

    @pytest.mark.parametrize("uri", [
        "/redfish/v1", "/redfish", "/redfish/v1/odata", "/redfish/v1/$metadata",
        "/redfish/v1/Schemas/Cxl_v1.xml", "/redfish/v1/Registries/CxlHealth.1.0.0.json",
        SLD_CHUNKS, "/redfish/v1/Nonexistent", CHASSIS + "?$top=1"])
    def test_get(self, api_client, uri):
        """200 (JSON, XML, a file compiled in), 404 and 501"""
        cache_control(requests.get(api_client.base_url + uri, timeout=30))

    def test_writes(self, api_client):
        """201, 204, 400, 405 and 412"""
        base = api_client.base_url
        chunk = api_client.get(SLD_CHUNKS)[1]["Members"][0]["@odata.id"]
        deleted = requests.delete(base + chunk, timeout=30)
        assert deleted.status_code == 204
        cache_control(deleted)
        created = requests.post(base + SLD_CHUNKS, json={"MemoryChunkSizeMiB": 256},
                                timeout=30)
        assert created.status_code == 201
        cache_control(created)
        for r in [requests.post(base + SLD_CHUNKS, data="{", timeout=30,
                                headers={"Content-Type": "application/json"}),
                  requests.put(base + CHASSIS, json={}, timeout=30),
                  requests.post(base + SLD_CHUNKS, json={"MemoryChunkSizeMiB": 256},
                                headers={"If-Match": "*"}, timeout=30)]:
            assert r.status_code in (400, 405, 412)
            cache_control(r)


@pytest.mark.mock_topology("mock/dcd-device.json")
def test_cache_control_task(api_client):
    """202, with a Task, and its monitor"""
    from test_dcd import SLD_ADD, TAG_NEW
    r = requests.post(api_client.base_url + SLD_ADD, timeout=30, json={
        "Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW})
    assert r.status_code == 202
    cache_control(r)
    cache_control(requests.get(api_client.base_url + r.headers["Location"], timeout=30))


# Allow on a 200 to a GET (review F18.6, the DMTF Redfish-Protocol-Validator's
# RESP_HEADERS_ALLOW_GET_OR_HEAD)
def crawl_responses(base, extra=()):
    """Every resource reachable from the service root, and @extra: {uri:
    the response to its GET}"""
    from collections import deque
    from test_oem_schema import links

    seen, todo, out = set(), deque(["/redfish/v1", *extra]), {}
    while todo:
        uri = todo.popleft().split("#")[0].rstrip("/") or "/"
        if uri in seen or not uri.startswith("/redfish"):
            continue
        seen.add(uri)
        response = requests.get(base + uri, timeout=30)
        out[uri] = response
        if response.status_code == 200 and \
           response.headers.get("Content-Type", "").startswith("application/json"):
            todo.extend(links(response.json()))
    return out


def methods(value):
    return {m.strip() for m in value.split(",")}


def allow_as_405_has_it(base, uri, response):
    """
    The Allow header "Shall be returned with the HTTP 200 (OK) status code
    with any GET or HEAD operation to indicate the other allowable
    operations for this resource" (DSP0266 1.25.0 Table 13): the same
    methods a 405 for the resource lists (PUT is taken by none).
    """
    assert response.status_code == 200, uri
    assert "Allow" in response.headers, uri
    assert methods(response.headers["Allow"]) == \
        allowed(requests.put(base + uri, json={}, timeout=30)), uri


@pytest.mark.parametrize("topology", ["mock/fm-boots-first.json", "mock/dcd-device.json",
                                      "mock/unit/unit-direct-sld.json"])
def test_get_has_allow(cxl_fmd_executable, topology):
    """Every 200 of a GET has Allow; a 405 had it, a 200 did not"""
    from conftest import _find_free_port, _wait_for_service
    import subprocess
    import tempfile
    from pathlib import Path

    root = Path(__file__).resolve().parent.parent
    port = _find_free_port()
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(root / topology),
                                 "--port", str(port)], stdout=log, stderr=subprocess.STDOUT)
        try:
            base = f"http://localhost:{port}"
            assert _wait_for_service(base + "/redfish/v1")
            if topology == "mock/fm-boots-first.json":
                bound = requests.post(base + CONNECTIONS, timeout=30, json={
                    "ConnectionType": "Memory",
                    "Links": {
                        "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"}],
                        "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]},
                    "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id":
                        CHASSIS + "/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks/sld_0_chunk0"}}]})
                assert bound.status_code == 201
            if topology == "mock/dcd-device.json":
                from test_dcd import SLD_ADD, TAG_NEW
                task = requests.post(base + SLD_ADD, timeout=30, json={
                    "Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW})
                assert task.status_code == 202
            extra = ["/redfish", "/redfish/v1/odata", "/redfish/v1/$metadata",
                     "/redfish/v1/Schemas/Cxl_v1.xml",
                     "/redfish/v1/Registries/CxlHealth.1.0.0.json"]
            if topology == "mock/dcd-device.json":
                import time
                monitor = task.headers["Location"]
                deadline = time.time() + 10
                while requests.get(base + monitor, timeout=30).status_code == 202:
                    assert time.time() < deadline
                    time.sleep(0.05)
                extra.append(monitor)
            responses = crawl_responses(base, extra)
            ok = {u: r for u, r in responses.items() if r.status_code == 200}
            assert set(extra) <= set(ok)
            if topology == "mock/dcd-device.json":
                # the monitor of the add, which the mock host has done
                assert any("/TaskMonitors/" in u for u in ok)
            seen = set()
            for uri, response in ok.items():
                allow_as_405_has_it(base, uri, response)
                seen |= methods(response.headers["Allow"])
            # each method one resource or another takes
            if topology == "mock/fm-boots-first.json":
                assert seen == {"GET", "POST", "PATCH", "DELETE"}
        finally:
            proc.terminate()
            assert proc.wait(timeout=10) == 0
