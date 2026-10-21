# SPDX-License-Identifier: LGPL-2.1
"""Redfish Service Validator integration tests."""

import json
import subprocess
import sys
import urllib.request

import pytest


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
    """Run DMTF Redfish Service Validator against cxl-fmd."""

    def _run_validator(self, validator_path, url, schema_dir, timeout=300):
        """Run the validator and return result."""
        cmd = [
            str(validator_path),
            "-r", url,
            "--user", "none", "--password", "none", "--authtype", "Basic",
            "--nooemcheck",
        ]

        if schema_dir and schema_dir.exists():
            cmd.extend(["--schema_directory", str(schema_dir)])

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )

        return result

    def test_single_sld_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate single SLD topology against Redfish schemas."""
        result = self._run_validator(
            validator_venv.validator, cxl_fmd_daemon["url"], schema_dir)

        print("=== Validator stdout ===")
        print(result.stdout)
        if result.stderr:
            print("=== Validator stderr ===")
            print(result.stderr)

        assert result.returncode == 0, f"Validator failed with code {result.returncode}"

    @pytest.mark.mock_topology("mock/mld-device.json")
    def test_mld_validation(self, cxl_fmd_daemon, schema_dir, validator_venv):
        """Validate MLD topology against Redfish schemas."""
        result = self._run_validator(
            validator_venv.validator, cxl_fmd_daemon["url"], schema_dir)

        print("=== Validator stdout ===")
        print(result.stdout)
        if result.stderr:
            print("=== Validator stderr ===")
            print(result.stderr)

        assert result.returncode == 0, f"Validator failed with code {result.returncode}"
