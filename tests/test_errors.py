# SPDX-License-Identifier: LGPL-2.1
"""
Error responses carry @Message.ExtendedInfo, with messages of the DMTF Base
registry that say what went wrong (review F18 claim 2).

"The @Message.ExtendedInfo property should be present in all error
responses" (DSP0266 1.25.0 Section 9.5.10). Every error response any test
meets is checked against the registry (redfish_messages.py, from
conftest.py); these are the messages cxl-fmd chooses for each kind of
error.
"""

import re
from pathlib import Path

import pytest
import requests

from redfish_messages import REGISTRY, check_error

CHUNKS = "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"


def error_of(response, status):
    """The keys of the messages of @response, a @status error"""
    assert response.status_code == status, response.text
    return check_error(response.json())


def info_of(response):
    return response.json()["error"]["@Message.ExtendedInfo"]


@pytest.mark.mock_topology("mock/single-sld.json")
class TestMessages:

    def test_unknown_uri(self, api_client):
        uri = "/redfish/v1/Chassis/nonexistent"
        response = requests.get(api_client.base_url + uri, timeout=30)
        assert error_of(response, 404) == ["InvalidURI"]
        assert info_of(response)[0]["MessageArgs"] == [uri]
        assert info_of(response)[0]["Message"] == f"The URI '{uri}' was not found."
        # cxl-fmd's own sentence, as before
        assert response.json()["error"]["message"] == "Resource not found"

    def test_method_not_allowed(self, api_client):
        response = requests.put(api_client.base_url + "/redfish/v1/Chassis",
                                json={}, timeout=30)
        assert error_of(response, 405) == ["OperationNotAllowed"]
        assert response.headers["Allow"] == "GET"

    def test_malformed_json(self, api_client):
        response = requests.post(api_client.base_url + CONNECTIONS,
                                 data="{not json", timeout=30)
        assert error_of(response, 400) == ["MalformedJSON"]

    def test_body_not_as_asked(self, api_client):
        """Valid JSON that is not a Connection: GeneralError, with what to do"""
        response = requests.post(api_client.base_url + CONNECTIONS,
                                 json={"bogus": 1}, timeout=30)
        assert error_of(response, 400) == ["GeneralError"]
        assert info_of(response)[0]["Resolution"] == \
            "Correct the request as the error's message says, and resubmit it."

    def test_property_missing(self, api_client):
        response = requests.post(api_client.base_url + CHUNKS, json={}, timeout=30)
        assert error_of(response, 400) == ["PropertyMissing"]
        m = info_of(response)[0]
        assert m["MessageArgs"] == ["MemoryChunkSizeMiB"]
        assert m["RelatedProperties"] == ["/MemoryChunkSizeMiB"]

    def test_property_value(self, api_client):
        response = requests.post(api_client.base_url + CHUNKS,
                                 json={"MemoryChunkSizeMiB": 256,
                                       "AddressRangeType": "Block"}, timeout=30)
        assert error_of(response, 400) == ["PropertyValueError"]
        m = info_of(response)[0]
        assert m["MessageArgs"] == ["AddressRangeType"]
        assert m["RelatedProperties"] == ["/AddressRangeType"]

    def test_exhausted(self, api_client):
        """A chunk larger than its domain: the domain cannot give it"""
        response = requests.post(api_client.base_url + CHUNKS,
                                 json={"MemoryChunkSizeMiB": 1 << 40}, timeout=30)
        assert error_of(response, 409) == ["ResourceExhaustion"]
        assert info_of(response)[0]["MessageArgs"] == \
            ["/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0"]

    def test_payload_too_large(self, api_client):
        response = requests.post(api_client.base_url + CONNECTIONS,
                                 data="x" * (65 * 1024), timeout=30)
        assert error_of(response, 413) == ["PayloadTooLarge"]

    def test_query_parameter(self, api_client):
        response = requests.get(api_client.base_url + "/redfish/v1/Chassis?$top=1",
                                timeout=30)
        assert error_of(response, 501) == ["QueryParameterUnsupported"]


def test_messages_c_is_the_registry():
    """The table in src/redfish/messages.c is copied from the registry"""
    src = (Path(__file__).parent.parent / "src" / "redfish" / "messages.c").read_text()
    entries = re.findall(r'\[FMD_RF_\w+\] = \{\s*"(\w+)", (\d+), "(\w+)",\s*'
                         r'"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)" \}', src)
    assert len(entries) == src.count("[FMD_RF_") - src.count("[FMD_RF_MSG_COUNT]")
    assert entries
    for key, nargs, severity, message, resolution in entries:
        entry = REGISTRY["Messages"][key]
        assert int(nargs) == entry["NumberOfArgs"], key
        assert severity == entry["MessageSeverity"], key
        assert message == entry["Message"], key
        assert resolution == entry["Resolution"], key
