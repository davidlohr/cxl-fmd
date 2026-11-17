# SPDX-License-Identifier: LGPL-2.1
"""
cxl-fmd's Oem properties and Oem action have a schema (review DCD-Q A6,
P5.5).

Every "Oem": {"Cxl": ...} object cxl-fmd serves names its type with
@odata.type, "#Cxl.v1_0_0.<Type>", and that type is in schema/oem/csdl/
Cxl_v1.xml, which is served at /redfish/v1/Schemas/Cxl_v1.xml ("Local copy
of a Redfish schema file", DSP0266 1.25.0 Table 4) and referenced from
$metadata ("The OData metadata document may reference additional schema
documents that describe OEM-specific extensions that the service uses",
Section 8.4.2.2). Each OEM action is "#<OEMSchemaName>.<Action>" (Section
9.8.8): #Cxl.ForceUnbind is the ForceUnbind action of the Cxl namespace.

The tests walk every resource of a few mock topologies and check each Oem
object against the CSDL: its type, each property declared, each enum
value a member, each nested object of its declared type.
"""

import re
import xml.etree.ElementTree as ET
from collections import deque
from pathlib import Path

import pytest
import requests

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_FILE = ROOT / "schema" / "oem" / "csdl" / "Cxl_v1.xml"
EDM = "{http://docs.oasis-open.org/odata/ns/edm}"
EDMX = "{http://docs.oasis-open.org/odata/ns/edmx}"


def load_schema():
    """{qualified name: element} of the complex, enum types and actions"""
    types = {}
    tree = ET.parse(SCHEMA_FILE)
    for schema in tree.iter(EDM + "Schema"):
        ns = schema.get("Namespace")
        for el in schema:
            if el.tag in (EDM + "ComplexType", EDM + "EnumType", EDM + "Action"):
                types[f"{ns}.{el.get('Name')}"] = el
    return types


TYPES = load_schema()
EDM_CHECK = {
    "Edm.String": lambda v: isinstance(v, str),
    "Edm.Boolean": lambda v: isinstance(v, bool),
    "Edm.Int64": lambda v: isinstance(v, int) and not isinstance(v, bool),
    # cxl-fmd serves its times in UTC, "YYYY-MM-DDThh:mm:ssZ"
    "Edm.DateTimeOffset": lambda v: isinstance(v, str) and
        re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", v) is not None,
}


def check_value(where, typename, value):
    if typename.startswith("Collection("):
        assert isinstance(value, list), where
        for i, item in enumerate(value):
            check_value(f"{where}/{i}", typename[11:-1], item)
        return
    if typename in EDM_CHECK:
        assert EDM_CHECK[typename](value), f"{where}: {value!r} is not {typename}"
        return
    if typename.startswith("MemoryExtent."):
        # a link to a MemoryExtent resource
        assert set(value) == {"@odata.id"}, where
        assert "/MemoryExtents/" in value["@odata.id"], where
        return
    el = TYPES.get(typename)
    assert el is not None, f"{where}: type {typename} is not in Cxl_v1.xml"
    if el.tag == EDM + "EnumType":
        assert value in [m.get("Name") for m in el.findall(EDM + "Member")], \
            f"{where}: {value!r} is not a member of {typename}"
        return
    check_object(where, typename, value)


def check_object(where, typename, obj):
    el = TYPES[typename]
    assert isinstance(obj, dict), where
    props = {p.get("Name"): p for p in el
             if p.tag in (EDM + "Property", EDM + "NavigationProperty")}
    for name, value in obj.items():
        if name == "@odata.type":
            continue
        assert name in props, f"{where}/{name} is not declared in {typename}"
        prop = props[name]
        if value is None:
            assert prop.get("Nullable") != "false", f"{where}/{name} is null"
            continue
        check_value(f"{where}/{name}", prop.get("Type"), value)


def check_oem(uri, resource):
    oem = resource.get("Oem")
    found = []
    if oem is not None:
        assert set(oem) == {"Cxl"}, uri
        cxl = oem["Cxl"]
        odata_type = cxl.get("@odata.type", "")
        assert odata_type.startswith("#Cxl.v1_0_0."), f"{uri}: Oem.Cxl @odata.type {odata_type!r}"
        typename = odata_type[1:]
        assert typename in TYPES, f"{uri}: {typename} is not in Cxl_v1.xml"
        assert TYPES[typename].get("BaseType") == "Resource.OemObject", typename
        check_object(f"{uri}/Oem/Cxl", typename, cxl)
        found.append(typename)
    actions = resource.get("Actions", {}).get("Oem", {})
    for name, action in actions.items():
        assert name.startswith("#Cxl."), f"{uri}: {name}"
        el = TYPES.get(name[1:])
        assert el is not None and el.tag == EDM + "Action", f"{uri}: {name} is not defined"
        # the binding parameter: the resource's OemActions
        binding = el.find(EDM + "Parameter")
        rtype = resource["@odata.type"].split(".")[0].lstrip("#")
        assert binding.get("Type") == f"{rtype}.v1_0_0.OemActions", name
        # "<ResourceUri>/Actions/Oem/<OEMSchemaName>.<Action>" (Section 9.8.8)
        assert action["target"] == f"{resource['@odata.id']}/Actions/Oem/{name[1:]}"
        found.append(name)
    return found


def links(obj):
    if isinstance(obj, dict):
        for key, value in obj.items():
            if key == "@odata.id" and isinstance(value, str):
                yield value
            else:
                yield from links(value)
    elif isinstance(obj, list):
        for item in obj:
            yield from links(item)


def crawl(base):
    """Every resource reachable from the service root: {uri: payload}"""
    seen, todo, out = set(), deque(["/redfish/v1"]), {}
    while todo:
        uri = todo.popleft().split("#")[0].rstrip("/")
        if uri in seen or not uri.startswith("/redfish/v1"):
            continue
        seen.add(uri)
        response = requests.get(base + uri, timeout=30)
        if response.status_code != 200 or \
           not response.headers.get("Content-Type", "").startswith("application/json"):
            continue
        data = response.json()
        if "@odata.id" not in data:
            continue
        out[uri] = data
        todo.extend(links(data))
    return out


@pytest.mark.parametrize("topology, expected", [
    ("mock/dcd-device.json", {"Cxl.v1_0_0.PCIeDevice", "Cxl.v1_0_0.Connection",
                              "Cxl.v1_0_0.MemoryDomain", "Cxl.v1_0_0.MemoryRegion",
                              "Cxl.v1_0_0.MemoryExtent", "Cxl.v1_0_0.CXLLogicalDevice"}),
    ("mock/event-logs.json", {"Cxl.v1_0_0.Connection", "Cxl.v1_0_0.MemoryDomain",
                              "#Cxl.ForceUnbind"}),
    ("mock/mhd-device.json", {"Cxl.v1_0_0.Connection", "Cxl.v1_0_0.MemoryDomain"}),
])
def test_every_oem_object_has_its_schema(cxl_fmd_executable, topology, expected):
    from conftest import _find_free_port, _wait_for_service
    import subprocess
    import tempfile

    port = _find_free_port()
    log = tempfile.TemporaryFile()
    proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(ROOT / topology),
                             "--port", str(port)], stdout=log, stderr=subprocess.STDOUT)
    try:
        base = f"http://localhost:{port}"
        assert _wait_for_service(base + "/redfish/v1")
        found = set()
        for uri, data in crawl(base).items():
            found.update(check_oem(uri, data))
        assert found >= expected, found
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        log.close()


@pytest.mark.mock_topology("mock/single-sld.json")
class TestServed:

    def test_schema_file(self, cxl_fmd_daemon):
        response = requests.get(cxl_fmd_daemon["url"] + "/redfish/v1/Schemas/Cxl_v1.xml",
                                timeout=30)
        assert response.status_code == 200
        assert response.headers["Content-Type"].startswith("application/xml")
        assert response.content == SCHEMA_FILE.read_bytes()

    def test_unknown_schema_is_404(self, api_client):
        assert api_client.get("/redfish/v1/Schemas/Chassis_v1.xml")[0] == 404
        assert api_client.get("/redfish/v1/Schemas")[0] == 404

    def test_metadata_references_it(self, cxl_fmd_daemon):
        text = requests.get(cxl_fmd_daemon["url"] + "/redfish/v1/$metadata", timeout=30).text
        root = ET.fromstring(text)
        refs = {r.get("Uri"): {i.get("Namespace") for i in r.findall(EDMX + "Include")}
                for r in root.findall(EDMX + "Reference")}
        assert refs["/redfish/v1/Schemas/Cxl_v1.xml"] == {"Cxl", "Cxl.v1_0_0"}


def test_schema_is_well_formed():
    """Every type it uses is its own, Edm's, or one it references."""
    tree = ET.parse(SCHEMA_FILE)
    included = {i.get("Namespace") for i in tree.iter(EDMX + "Include")}
    included |= {i.get("Alias") for i in tree.iter(EDMX + "Include") if i.get("Alias")}
    for el in tree.iter():
        for attr in ("Type", "BaseType"):
            t = el.get(attr)
            if not t:
                continue
            t = t[11:-1] if t.startswith("Collection(") else t
            if t.startswith("Edm.") or t in TYPES:
                continue
            assert t.rsplit(".", 1)[0] in included, f"{t} is not referenced"
    for name, el in TYPES.items():
        if el.tag == EDM + "ComplexType":
            assert el.find(EDM + "Annotation[@Term='OData.Description']") is not None, name
