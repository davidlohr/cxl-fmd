# SPDX-License-Identifier: LGPL-2.1
"""
Status from Get Health Info (4200h), the same in every view of a device
(review F17 claims 1 and 6).

Health Status is a bit field ("Bit[0]: Maintenance Needed ... Bit[1]:
Performance Degraded ... Bit[2]: Hardware Replacement Needed ... Bit[3]:
Memory Capacity Degraded", CXL r4.0 Table 8-315), not a scale, and Media
Status an enumeration. The Chassis compared the OR of the bits and the
Media Status as numbers: 03h, Maintenance Needed and Performance Degraded,
was Critical and UnavailableOffline, and so was 08h, while the Memory of
the same device was Enabled, Warning. Every view now maps them with
fmd_rollup_status(); which Health and State each gets is cxl-fmd's
mapping.

Each topology is mock/single-sld.json with one change, written to a
temporary file.
"""

import copy
import json
import subprocess
import tempfile
from pathlib import Path

import pytest
import requests

from conftest import _find_free_port, _wait_for_service

ROOT = Path(__file__).resolve().parent.parent
SLD = json.loads((ROOT / "mock" / "single-sld.json").read_text())
DEV = "cxl_mem0"
VIEWS = [
    f"/redfish/v1/Chassis/{DEV}",
    f"/redfish/v1/Chassis/{DEV}/PCIeDevices/{DEV}",
    f"/redfish/v1/Chassis/{DEV}/PCIeDevices/{DEV}/CXLLogicalDevices/0",
    f"/redfish/v1/Chassis/{DEV}/Memory/{DEV}",
    f"/redfish/v1/Chassis/{DEV}/MemoryDomains/{DEV}_domain0",
    f"/redfish/v1/Chassis/{DEV}/MemoryDomains/{DEV}_domain0/MemoryChunks/{DEV}_chunk0",
]


@pytest.fixture
def daemon(cxl_fmd_executable, tmp_path):
    """start(topology, *args) runs cxl-fmd on it, with more command line
    arguments @args, and returns its base URL."""
    started = []

    def start(topology, *args):
        path = tmp_path / f"topology{len(started)}.json"
        path.write_text(json.dumps(topology))
        port = _find_free_port()
        log = tempfile.TemporaryFile()
        proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(path),
                                 "--port", str(port), *args],
                                stdout=log, stderr=subprocess.STDOUT)
        started.append((proc, log))
        base = f"http://localhost:{port}"
        if not _wait_for_service(base + "/redfish/v1"):
            log.seek(0)
            pytest.fail("cxl-fmd did not start:\n" + log.read().decode(errors="replace"))
        return base

    yield start

    for proc, log in started:
        proc.terminate()
        proc.wait(timeout=10)
        log.close()


def status(base, uri):
    response = requests.get(base + uri, timeout=30)
    assert response.status_code == 200, uri
    return response.json()["Status"]


@pytest.mark.parametrize("health, media, state, expected", [
    (0x00, 0x00, "Enabled", "OK"),
    (0x01, 0x00, "Enabled", "Warning"),
    (0x02, 0x00, "Enabled", "Warning"),
    (0x03, 0x00, "Enabled", "Warning"),    # was Critical, UnavailableOffline
    (0x04, 0x00, "Enabled", "Critical"),   # was UnavailableOffline
    (0x08, 0x00, "Enabled", "Warning"),    # was Critical, UnavailableOffline
    (0x00, 0x01, "Starting", "OK"),        # was Warning
    (0x00, 0x02, "Degraded", "Warning"),
    (0x00, 0x03, "UnavailableOffline", "Critical"),
    (0x00, 0x04, "Degraded", "Warning"),   # was Critical, UnavailableOffline
    (0x00, 0x09, "Degraded", "Warning"),   # was Critical, UnavailableOffline
    (0x04, 0x02, "Degraded", "Critical"),
])
def test_every_view_maps_health_alike(daemon, health, media, state, expected):
    topology = copy.deepcopy(SLD)
    topology["devices"][0]["health_info"]["health_status"] = health
    topology["devices"][0]["health_info"]["media_status"] = media
    base = daemon(topology)
    for uri in VIEWS:
        got = status(base, uri)
        assert (got["State"], got["Health"]) == (state, expected), uri
    # the device's Chassis: its own health, and that of its views
    assert status(base, VIEWS[0])["HealthRollup"] == expected


@pytest.mark.mock_topology("mock/switch-attached.json")
class TestSwitchChassis:

    def test_switch_rollup_is_mapped(self, api_client):
        """cxl_switch0_degraded has Health Status 02h and an LD of
        cxl_switch0_mld 01h: OR-ed, 03h, which the switch's Chassis
        reported as Critical and UnavailableOffline though no device was
        worse than Warning. The switch reads no health of its own (switches
        are prohibited from the Memory Device Command Set, Table 8-308):
        Enabled, no Health, and HealthRollup Warning."""
        code, data = api_client.get("/redfish/v1/Chassis/cxl_switch_0")
        assert code == 200
        assert data["Status"] == {"State": "Enabled", "HealthRollup": "Warning"}

    def test_device_chassis_agree(self, api_client):
        for dev, health in [("cxl_switch0_degraded", "Warning"),
                            ("cxl_switch0_dev0", "OK")]:
            code, data = api_client.get(f"/redfish/v1/Chassis/{dev}")
            assert code == 200
            assert data["Status"]["Health"] == health
            assert data["Status"]["HealthRollup"] == health
