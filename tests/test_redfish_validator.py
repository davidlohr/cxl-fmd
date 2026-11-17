# SPDX-License-Identifier: LGPL-2.1
"""Redfish Service Validator integration tests."""

import json
import re
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

import pytest

from test_dcd import dcd_daemon  # noqa: F401 (a fixture)

ROOT = Path(__file__).resolve().parent.parent

# What the validator may warn of and still pass: a property DSP8010
# deprecates that cxl-fmd still serves. Anything else it warns of fails,
# Oem extensions included: it reports a bad one only as a warning ("For
# now, downgrade bad OEM extensions to warnings", its validate.py).
ALLOWED_WARNINGS = [
    re.compile(r"^/QoSTelemetryCapabilities/TemporaryThroughputReductionSupported \(.*\): "
               r"Deprecated Property Warning"),
]

# Topologies with a test of their own below (hot-removal.json is run
# after a device is pulled out): test_every_topology_validation runs the
# rest
OWN_TEST = {"single-sld.json", "mld-device.json", "event-logs.json",
            "hot-removal.json", "dcd-device.json", "dcd-shared-perhost.json",
            "degraded-device.json"}
OTHER_TOPOLOGIES = sorted(
    str(p.relative_to(ROOT)) for p in
    list((ROOT / "mock").glob("*.json")) + list((ROOT / "mock" / "unit").glob("*.json"))
    if p.name not in OWN_TEST)


def validator_problems(logdir):
    """Each WARN or FAIL in the validator's debug log in @logdir that is
    not allowed"""
    problems = []
    for log in Path(logdir).rglob("RedfishServiceValidatorDebug_*.log"):
        for line in log.read_text(errors="replace").splitlines():
            m = re.search(r" - (WARN|FAIL) - (.*)$", line)
            if m and not any(a.search(m.group(2)) for a in ALLOWED_WARNINGS):
                problems.append(f"{m.group(1)} {m.group(2)}")
    return problems


class TestRedfishEndpoints:
    """Quick sanity checks for Redfish endpoints."""

    def test_service_root_accessible(self, cxl_fmd_daemon):
        """Verify ServiceRoot is accessible and has required fields."""
        url = f"{cxl_fmd_daemon['url']}/redfish/v1"
        with urllib.request.urlopen(url) as resp:
            data = json.load(resp)
            assert "@odata.id" in data
            assert "@odata.type" in data
            assert data["@odata.type"].startswith("#ServiceRoot")

    def test_chassis_collection(self, cxl_fmd_daemon):
        """Verify Chassis collection is accessible."""
        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis"
        with urllib.request.urlopen(url) as resp:
            data = json.load(resp)
            assert "Members" in data
            assert "Members@odata.count" in data
            assert data["Members@odata.count"] >= 1

    def test_chassis_member(self, cxl_fmd_daemon):
        """Verify individual Chassis resource."""
        # First get the collection
        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis"
        with urllib.request.urlopen(url) as resp:
            data = json.load(resp)

        # Then get first member
        if data["Members"]:
            member_url = cxl_fmd_daemon['url'] + data["Members"][0]["@odata.id"]
            with urllib.request.urlopen(member_url) as resp:
                chassis = json.load(resp)
                assert "@odata.id" in chassis
                assert "@odata.type" in chassis
                assert "Id" in chassis

    def test_pcie_device(self, cxl_fmd_daemon):
        """Verify PCIeDevice resource."""
        # Navigate: Chassis -> PCIeDevices -> first device
        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis"
        with urllib.request.urlopen(url) as resp:
            chassis_coll = json.load(resp)

        if not chassis_coll["Members"]:
            pytest.skip("No chassis found")

        chassis_id = chassis_coll["Members"][0]["@odata.id"].split("/")[-1]
        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis/{chassis_id}/PCIeDevices"
        with urllib.request.urlopen(url) as resp:
            pcie_coll = json.load(resp)

        if not pcie_coll["Members"]:
            pytest.skip("No PCIe devices found")

        pcie_url = cxl_fmd_daemon['url'] + pcie_coll["Members"][0]["@odata.id"]
        with urllib.request.urlopen(pcie_url) as resp:
            pcie_dev = json.load(resp)
            assert "@odata.type" in pcie_dev
            assert "PCIeDevice" in pcie_dev["@odata.type"]

    def test_cxl_logical_device(self, cxl_fmd_daemon):
        """Verify CXLLogicalDevice resource has MemorySizeMiB."""
        # Navigate: Chassis -> PCIeDevices -> CXLLogicalDevices -> first LD
        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis"
        with urllib.request.urlopen(url) as resp:
            chassis_coll = json.load(resp)

        chassis_id = chassis_coll["Members"][0]["@odata.id"].split("/")[-1]

        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis/{chassis_id}/PCIeDevices"
        with urllib.request.urlopen(url) as resp:
            pcie_coll = json.load(resp)

        pcie_id = pcie_coll["Members"][0]["@odata.id"].split("/")[-1]

        url = f"{cxl_fmd_daemon['url']}/redfish/v1/Chassis/{chassis_id}/PCIeDevices/{pcie_id}/CXLLogicalDevices"
        with urllib.request.urlopen(url) as resp:
            ld_coll = json.load(resp)

        ld_url = cxl_fmd_daemon['url'] + ld_coll["Members"][0]["@odata.id"]
        with urllib.request.urlopen(ld_url) as resp:
            ld = json.load(resp)
            assert "MemorySizeMiB" in ld
            assert isinstance(ld["MemorySizeMiB"], int)
            assert ld["MemorySizeMiB"] > 0


@pytest.mark.slow
class TestRedfishValidator:
    """Run DMTF Redfish Service Validator against cxl-fmd, its Oem
    extensions checked against cxl-fmd's Oem schema (Cxl_v1.xml, served
    at /redfish/v1/Schemas and named in $metadata)."""

    def _run_validator(self, validator_path, url, schema_dir, timeout=300):
        """Run the validator; returns its result, and the WARNs and FAILs
        of its log that are not allowed."""
        with tempfile.TemporaryDirectory() as logdir:
            cmd = [
                str(validator_path),
                "-r", url,
                "--user", "none", "--password", "none", "--authtype", "Basic",
                "--logdir", logdir,
            ]

            if schema_dir and schema_dir.exists():
                cmd.extend(["--schema_directory", str(schema_dir)])

            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                )
            finally:
                # The validator saves each schema $metadata references by a
                # relative URI in the schema directory, DMTF's: cxl-fmd's Oem
                # schema (/redfish/v1/Schemas/Cxl_v1.xml) is not left there
                if schema_dir:
                    (schema_dir / "Cxl_v1.xml").unlink(missing_ok=True)

            return result, validator_problems(logdir)

    def _validate(self, url, schema_dir, validator_venv):
        result, problems = self._run_validator(validator_venv.validator, url, schema_dir)

        print("=== Validator stdout ===")
        print(result.stdout)
        if result.stderr:
            print("=== Validator stderr ===")
            print(result.stderr)

        assert result.returncode == 0, f"Validator failed with code {result.returncode}"
        assert not problems, "\n".join(problems)

    def test_single_sld_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate single SLD topology against Redfish schemas."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)
        # the Oem schema it fetched from cxl-fmd is not left in the tree
        assert not (schema_dir / "Cxl_v1.xml").exists()

    @pytest.mark.mock_topology("mock/event-logs.json")
    def test_event_logs_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate the log services of the switch and of the memory devices,
        and their entries, against the Redfish schemas."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/hot-removal.json")
    def test_hot_removal_validation(self, cxl_fmd_daemon, api_client, schema_dir,
                                    validator_venv):
        """Validate after a device has been pulled out of a bound switch
        port: its resources are gone, and its connection stands, Absent."""
        from test_hot_removal import unplug
        from test_orchestration import connect

        assert connect(api_client, "switch_0_host0", "sld_0")[0] == 201
        assert connect(api_client, "switch_0_host1", "mld_0", ld=1)[0] == 201
        unplug(cxl_fmd_daemon, api_client, "sld_0")

        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/dcd-device.json")
    def test_dcd_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate the DCDs: their regions and extents, and the Dynamic
        Capacity records in their logical devices' logs."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/dcd-device.json")
    def test_dcd_tasks_validation(self, cxl_fmd_daemon, api_client, schema_dir,
                                  validator_venv):
        """Validate with Dynamic Capacity tasks: an add, a release and a
        DELETE, each Completed, with their Payload, Oem and links."""
        from test_dcd import (SLD, SLD_ADD, SLD_RELEASE, TAG_NEW, dc_delete,
                              dc_post, release_body)

        assert dc_post(api_client, SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 1024,
            "Tag": TAG_NEW})[0] == 200
        assert dc_post(api_client, SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_NEW, Length=512))[0] == 200
        assert dc_delete(api_client, SLD + "/MemoryExtents/ext_0_400000000") == 204
        status, tasks = api_client.get("/redfish/v1/TaskService/Tasks")
        assert tasks["Members@odata.count"] == 3
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/dcd-shared-perhost.json")
    def test_dcd_perhost_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate a DCD whose hosts see its regions at different DPAs: an
        extent with no tag, and capacity only an FM reference keeps."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/degraded-device.json")
    def test_degraded_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate devices whose Get Health Info is not healthy: their
        Status.Conditions, and the registry they are from."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.mock_topology("mock/mld-device.json")
    def test_mld_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate MLD topology against Redfish schemas."""
        self._validate(cxl_fmd_daemon["url"], schema_dir, validator_venv)

    @pytest.mark.parametrize("topology", OTHER_TOPOLOGIES)
    def test_every_topology_validation(self, cxl_fmd_executable, schema_dir,
                                       validator_venv, topology):
        """Every other topology in mock/ and mock/unit/."""
        from conftest import _find_free_port, _wait_for_service

        port = _find_free_port()
        with tempfile.TemporaryFile() as log:
            proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(ROOT / topology),
                                     "--port", str(port)],
                                    stdout=log, stderr=subprocess.STDOUT)
            try:
                url = f"http://localhost:{port}"
                assert _wait_for_service(url + "/redfish/v1"), topology
                self._validate(url, schema_dir, validator_venv)
            finally:
                proc.terminate()
                proc.wait(timeout=10)


def test_dcd_task_states_validation(dcd_daemon, schema_dir, validator_venv):
    """
    Validate with a Dynamic Capacity task in each state: Completed with
    TaskStatus Warning (a host that accepts part), Exception (one that
    accepts none) and Running (one that never gives a release back), the
    mock's host_response.
    """
    from test_dcd import (MLD, SLD_ADD, SLD_RELEASE, TAG_NEW, TAG_SEED,
                          release_body, started, wait_task)

    def change(t):
        for dev in t["devices"]:
            dev["dcd"]["host_response"] = (
                {"add": "part", "release": "never"} if dev["id"] == "cxl_dcd0"
                else {"add": "none"})
    client = dcd_daemon(change)
    part = started(*client.post(SLD_ADD, {
        "Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW}))
    none = started(*client.post(
        MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd",
        {"Policy": "Free", "RegionNumber": 1, "Length": 256, "Tag": TAG_NEW}))
    assert wait_task(client, part)["TaskStatus"] == "Warning"
    assert wait_task(client, none)["TaskState"] == "Exception"
    running = started(*client.post(SLD_RELEASE, release_body(
        "TagBased", Tag=TAG_SEED, Length=4096)))
    assert client.get(running["@odata.id"])[1]["TaskState"] == "Running"
    TestRedfishValidator()._validate(client.base_url, schema_dir, validator_venv)


def test_validator_problems(tmp_path):
    """What fails a run: a warning of an Oem extension (as the validator
    reports one), and not the deprecated property cxl-fmd still serves."""
    (tmp_path / "RedfishServiceValidatorDebug_1.log").write_text(
        "x - redfish - WARNING - WARN - /QoSTelemetryCapabilities/"
        "TemporaryThroughputReductionSupported (False): Deprecated Property Warning: "
        "The property is deprecated.\n"
        "x - redfish - WARNING - WARN - /Oem/Cxl/@odata.type ([Not Present]): Required "
        "Property Error: The property '@odata.type' is mandatory, but not present in the "
        "payload.\n"
        "x - redfish - INFO - PASS - /Status/Health (Warning): Pass: The property is valid.\n")
    assert validator_problems(tmp_path) == [
        "WARN /Oem/Cxl/@odata.type ([Not Present]): Required Property Error: The property "
        "'@odata.type' is mandatory, but not present in the payload."]


def test_no_oem_check_is_not_used(monkeypatch):
    """The Oem extensions are validated: neither the tests nor
    scripts/run_redfish_validator.py pass --nooemcheck."""
    import importlib.util
    import types

    calls = []

    def run(cmd, **kw):
        calls.append(cmd)
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(subprocess, "run", run)
    TestRedfishValidator()._run_validator("rf_service_validator", "http://x", None)
    spec = importlib.util.spec_from_file_location(
        "run_redfish_validator", ROOT / "scripts" / "run_redfish_validator.py")
    script = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(script)
    script.run_validator(types.SimpleNamespace(validator="rf_service_validator"), 1)
    assert len(calls) == 2
    for cmd in calls:
        assert "--nooemcheck" not in cmd, cmd
