# SPDX-License-Identifier: LGPL-2.1
"""
Status.Conditions from Get Health Info (review F17 claim 2), and the
registry their messages are from.

DSP0288 1.4.0 Section 3: implementations "shall adhere to the numbered
comments referenced in the Comment column". Table 1 maps "Get Health Info,
Health Status, Many, Status" with Comment 3, "The Conditions property
within the Status property is used to show why the Health property
contains a value other than OK", and Additional Status[4] and [5] to
Memory Status/Conditions with Comment 4, "The Conditions property within
the Status property of the Memory resource shall contain messages to
indicate either the Corrected Volatile Error Count or Corrected Persistent
Error Count flags are set". cxl-fmd served no Conditions.

The messages are those of cxl-fmd's registry CxlHealth.1.0.0.json
(schema/oem/registries), served at /redfish/v1/Registries.
"""

import copy
import json
import re
from pathlib import Path

import pytest
import requests

from test_health import SLD, VIEWS, daemon  # noqa: F401 (a fixture)

ROOT = Path(__file__).resolve().parent.parent
REGISTRY_FILE = ROOT / "schema" / "oem" / "registries" / "CxlHealth.1.0.0.json"
REGISTRY = json.loads(REGISTRY_FILE.read_text())
JSON_SCHEMA_DIR = ROOT / "schema" / "dmtf" / "json-schema"
MEMORY = VIEWS[3]

ALL_HEALTH = ["MaintenanceNeeded", "PerformanceDegraded",
              "HardwareReplacementNeeded", "MemoryCapacityDegraded"]
MEDIA = {
    0x00: [], 0x01: [],
    0x02: ["WritePersistencyLost"],
    0x03: ["AllDataLost"],
    0x04: ["WritePersistencyLossOnPowerLoss"],
    0x05: ["WritePersistencyLossOnShutdown"],
    0x06: ["WritePersistencyLossImminent"],
    0x07: ["AllDataLossOnPowerLoss"],
    0x08: ["AllDataLossOnShutdown"],
    0x09: ["AllDataLossImminent"],
    0x0a: ["MediaStatusReserved"],
}


def get(base, uri):
    response = requests.get(base + uri, timeout=30)
    assert response.status_code == 200, uri
    return response.json()


def keys(status):
    return [c["MessageId"].split(".")[-1] for c in status.get("Conditions", [])]


def check_condition(cond):
    """A Condition is a message of the registry, as the registry has it."""
    prefix, major, minor, key = cond["MessageId"].split(".")
    assert (prefix, f"{major}.{minor}") == ("CxlHealth", "1.0")
    msg = REGISTRY["Messages"][key]
    args = cond.get("MessageArgs", [])
    assert len(args) == msg["NumberOfArgs"]
    text = re.sub(r"%(\d)", lambda m: args[int(m.group(1)) - 1], msg["Message"])
    assert cond["Message"] == text
    assert cond["Severity"] == msg["MessageSeverity"]
    assert cond["Resolution"] == msg["Resolution"]
    # Resource_v1 Condition has AdditionalProperties false
    assert set(cond) <= {"MessageId", "Message", "MessageArgs", "Severity", "Resolution"}


def sld(health=0, media=0, additional=0):
    topology = copy.deepcopy(SLD)
    info = topology["devices"][0]["health_info"]
    info.update(health_status=health, media_status=media, additional_status=additional)
    return topology


def test_corrected_error_counts_on_memory(daemon):  # noqa: F811
    """Case C of the review: Additional Status 30h, Health OK."""
    base = daemon(sld(additional=0x30))
    status = get(base, MEMORY)["Status"]
    assert keys(status) == ["CorrectedVolatileErrorCount", "CorrectedPersistentErrorCount"]
    assert (status["State"], status["Health"]) == ("Enabled", "OK")
    for cond in status["Conditions"]:
        check_condition(cond)
    # Comment 4 is the Memory's
    for uri in VIEWS:
        if uri != MEMORY:
            assert "Conditions" not in get(base, uri)["Status"], uri


@pytest.mark.parametrize("health, media", [(0x0f, 0x00)] + [(0, m) for m in MEDIA])
def test_why_health_is_not_ok(daemon, health, media):  # noqa: F811
    base = daemon(sld(health, media))
    expected = ALL_HEALTH * (health == 0x0f) + MEDIA[media]
    for uri in VIEWS:
        status = get(base, uri)["Status"]
        assert keys(status) == expected, uri
        for cond in status.get("Conditions", []):
            check_condition(cond)
        # a Condition is there exactly when Health is not OK
        assert bool(expected) == (status["Health"] != "OK"), uri


@pytest.mark.mock_topology("mock/degraded-device.json")
def test_degraded_device(api_client):
    code, data = api_client.get(
        "/redfish/v1/Chassis/cxl_maintenance_needed/Memory/cxl_maintenance_needed")
    assert code == 200
    assert keys(data["Status"]) == ["MaintenanceNeeded", "CorrectedVolatileErrorCount",
                                    "CorrectedPersistentErrorCount"]
    code, data = api_client.get("/redfish/v1/Chassis/cxl_all_data_lost")
    assert code == 200
    assert keys(data["Status"]) == ["MaintenanceNeeded", "HardwareReplacementNeeded",
                                    "AllDataLost"]


@pytest.mark.mock_topology("mock/mld-device.json")
def test_mld_conditions_are_its_lds(api_client):
    """The Chassis of an MLD adds up its LDs (01h, 02h and Media 02h)."""
    code, data = api_client.get("/redfish/v1/Chassis")
    assert code == 200
    seen = {}
    for member in data["Members"]:
        code, chassis = api_client.get(member["@odata.id"])
        assert code == 200
        if chassis["Status"].get("Health", "OK") != "OK":
            seen[chassis["Id"]] = keys(chassis["Status"])
            assert seen[chassis["Id"]], chassis["Id"]
    assert any(set(k) >= {"MaintenanceNeeded", "PerformanceDegraded"} for k in seen.values()), seen


@pytest.mark.mock_topology("mock/single-sld.json")
class TestRegistry:

    def test_service_root_links_registries(self, api_client):
        code, root = api_client.get("/redfish/v1")
        assert code == 200
        assert root["Registries"] == {"@odata.id": "/redfish/v1/Registries"}
        code, coll = api_client.get("/redfish/v1/Registries")
        assert code == 200
        assert coll["Members"] == [{"@odata.id": "/redfish/v1/Registries/CxlHealth"}]

    def test_registry_file(self, api_client, cxl_fmd_daemon):
        code, entry = api_client.get("/redfish/v1/Registries/CxlHealth")
        assert code == 200
        assert entry["Registry"] == "CxlHealth.1.0"
        # DSP0266 Table 4: /redfish/v1/Registries/<RegistryFile>, and
        # Section 10.1 "<RegistryName>.<MajorVersion>.<MinorVersion>.<Errata>.json"
        assert entry["Location"] == [{"Language": "en",
                                      "Uri": "/redfish/v1/Registries/CxlHealth.1.0.0.json"}]
        response = requests.get(cxl_fmd_daemon["url"] + entry["Location"][0]["Uri"], timeout=30)
        assert response.status_code == 200
        assert response.headers["Content-Type"].startswith("application/json")
        assert response.content == REGISTRY_FILE.read_bytes()

    def test_unknown_registry_is_404(self, api_client):
        assert api_client.get("/redfish/v1/Registries/Base")[0] == 404
        assert api_client.get("/redfish/v1/Registries/Base.1.24.0.json")[0] == 404

    def test_metadata_references_registry_files(self, cxl_fmd_daemon):
        text = requests.get(cxl_fmd_daemon["url"] + "/redfish/v1/$metadata", timeout=30).text
        assert "MessageRegistryFileCollection_v1.xml" in text
        assert 'Namespace="MessageRegistryFile.v1_1_5"' in text


def test_registry_is_a_message_registry():
    """CxlHealth.1.0.0.json against MessageRegistry.v1_9_0.json (DSP8010),
    its $refs resolved in schema/dmtf/json-schema."""
    jsonschema = pytest.importorskip("jsonschema")
    schema = json.loads((JSON_SCHEMA_DIR / "MessageRegistry.v1_9_0.json").read_text())
    store = {}
    for path in JSON_SCHEMA_DIR.glob("*.json"):
        doc = json.loads(path.read_text())
        for prefix in ("http://redfish.dmtf.org/schemas/v1/",
                       "https://redfish.dmtf.org/schemas/v1/"):
            store[prefix + path.name] = doc
    def offline(uri):
        # every $ref must be in schema/dmtf/json-schema: nothing is fetched
        raise AssertionError(f"$ref {uri} is not in {JSON_SCHEMA_DIR}")

    resolver = jsonschema.RefResolver.from_schema(
        schema, store=store, handlers={"http": offline, "https": offline})
    # DSP8010's own metaschema has keywords draft 7 does not: the schema
    # itself is not checked, only the registry against it
    errors = list(jsonschema.Draft7Validator(schema, resolver=resolver).iter_errors(REGISTRY))
    assert not errors, [e.message for e in errors]
    assert REGISTRY["Id"] == "CxlHealth.1.0.0"
    assert (REGISTRY["RegistryPrefix"], REGISTRY["RegistryVersion"]) == ("CxlHealth", "1.0.0")
    for key, msg in REGISTRY["Messages"].items():
        # "The message key shall be Pascal-cased and shall not include
        # spaces, periods, or special characters" (DSP0266 Section 9.5.11.2)
        assert re.fullmatch(r"[A-Z][A-Za-z0-9]*", key)
        assert msg["Message"].count("%") == msg["NumberOfArgs"]
        # Severity is deprecated for MessageSeverity: not given
        assert "Severity" not in msg

