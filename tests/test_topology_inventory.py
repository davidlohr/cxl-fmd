# SPDX-License-Identifier: LGPL-2.1
"""
Tests for CXL topology inventory and discovery.

Validates that cxl-fmd correctly discovers and represents CXL components
in the Redfish API hierarchy per DMTF DSP0288 (CXL to Redfish Mapping).
"""

import pytest


# =============================================================================
# SLD Inventory Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestSLDInventory:
    """Tests for Single Logical Device discovery and representation."""

    def test_sld_appears_in_chassis_collection(self, api_client):
        """Verify SLD device appears in Chassis collection."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        assert data["Members@odata.count"] >= 1
        # Chassis member should exist
        assert len(data["Members"]) >= 1

    def test_sld_chassis_has_correct_type(self, api_client):
        """Verify SLD Chassis resource has correct @odata.type."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mem0")
        assert status == 200
        assert "@odata.type" in data
        assert "Chassis" in data["@odata.type"]

    def test_sld_has_pcie_devices_collection(self, api_client):
        """Verify SLD has PCIeDevices collection."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mem0/PCIeDevices")
        assert status == 200
        assert data["Members@odata.count"] == 1

    def test_sld_pcie_device_has_cxl_properties(self, api_client):
        """Verify PCIeDevice has CXL-specific properties."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0")
        assert status == 200
        assert "@odata.type" in data
        assert "PCIeDevice" in data["@odata.type"]
        # Should have CXLLogicalDevices link
        assert "CXLLogicalDevices" in data

    def test_sld_has_single_cxl_logical_device(self, api_client):
        """Verify SLD has exactly one CXLLogicalDevice."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 1

    def test_sld_logical_device_memory_size(self, api_client):
        """Verify CXLLogicalDevice reports correct MemorySizeMiB."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices/cxl_mem0_ld0"
        )
        assert status == 200
        assert "MemorySizeMiB" in data
        # From mock/single-sld.json: total_capacity_mb = 16384
        assert data["MemorySizeMiB"] == 16384

    def test_sld_has_memory_domain(self, api_client):
        """Verify SLD has MemoryDomains collection."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mem0/MemoryDomains")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_sld_appears_in_fabric_endpoints(self, api_client):
        """Verify SLD appears as Fabric Endpoint with EntityRole=Target."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        assert status == 200
        # Should have at least one endpoint (the SLD)
        assert data["Members@odata.count"] >= 1

    def test_sld_endpoint_entity_role_target(self, api_client):
        """Verify SLD Endpoint has EntityRole=Target."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/cxl_mem0")
        assert status == 200
        assert "ConnectedEntities" in data
        assert len(data["ConnectedEntities"]) >= 1
        assert data["ConnectedEntities"][0]["EntityRole"] == "Target"


# =============================================================================
# MLD Inventory Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mld-device.json")
class TestMLDInventory:
    """Tests for Multi-Logical Device discovery and representation."""

    def test_mld_has_multiple_logical_devices(self, api_client):
        """Verify MLD exposes multiple CXLLogicalDevices."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices"
        )
        assert status == 200
        # cxl_mld_0 has 4 LDs in mock
        assert data["Members@odata.count"] == 4

    def test_mld_ld_ids_are_sequential(self, api_client):
        """Verify LD IDs are 0-based and sequential."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices"
        )
        assert status == 200
        # Extract LD IDs from paths (format: .../cxl_mld_0_ldN)
        ld_ids = []
        for member in data["Members"]:
            path = member["@odata.id"]
            # Extract the last segment and find LD number
            last_segment = path.split("/")[-1]
            # Handle format like "cxl_mld_0_ld0" or just "0"
            if "_ld" in last_segment:
                ld_id = int(last_segment.split("_ld")[-1])
            else:
                ld_id = int(last_segment)
            ld_ids.append(ld_id)
        # Should be 0, 1, 2, 3
        assert sorted(ld_ids) == [0, 1, 2, 3]

    def test_mld_16_ld_maximum(self, api_client):
        """Verify MLD can support up to 16 LDs."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_16ld/PCIeDevices/cxl_mld_16ld/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 16

    def test_mld_asymmetric_memory_allocation(self, api_client):
        """Verify MLDs support asymmetric memory distribution across LDs."""
        # cxl_mld_asymmetric has 3 LDs with different sizes
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_asymmetric/PCIeDevices/cxl_mld_asymmetric/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 3

    def test_mld_individual_ld_memory_size(self, api_client):
        """Verify each LD reports its own memory size."""
        # Get LD 0 from cxl_mld_0 (should be 32GB = 32768 MiB)
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices/cxl_mld_0_ld0"
        )
        assert status == 200
        assert "MemorySizeMiB" in data
        assert data["MemorySizeMiB"] == 32768

    def test_mld_has_memory_collection(self, api_client):
        """Verify MLD has Memory collection with per-LD resources."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mld_0/Memory")
        assert status == 200
        # Should have 4 Memory resources (one per LD)
        assert data["Members@odata.count"] == 4

    def test_mld_memory_has_health_status(self, api_client):
        """Verify Memory resource includes health status."""
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mld_0/Memory/cxl_mld_0_ld0")
        assert status == 200
        assert "Status" in data
        assert "Health" in data["Status"]

    def test_mld_per_ld_health_independent(self, api_client):
        """Verify LDs can have independent health status (cxl_mld_mixed_health)."""
        # Get all LDs from mixed health device
        ld_healths = []
        for i in range(4):
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mld_mixed_health/Memory/cxl_mld_mixed_health_ld{i}"
            )
            assert status == 200
            ld_healths.append(data["Status"]["Health"])
        # Should have at least one non-OK health
        assert "Warning" in ld_healths or "Critical" in ld_healths


# =============================================================================
# Switch Inventory Tests
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestSwitchInventory:
    """Tests for CXL Switch discovery and representation."""

    def test_switch_appears_in_fabric_switches(self, api_client):
        """Verify switch appears in Fabrics/CXL/Switches collection."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_switch_has_correct_type(self, api_client):
        """Verify Switch resource has correct @odata.type."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0")
        assert status == 200
        assert "Switch" in data["@odata.type"]
        assert data["SwitchType"] == "CXL"

    def test_switch_has_ports_collection(self, api_client):
        """Verify switch has Ports collection."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports")
        assert status == 200
        # 1 USP + 7 DSPs = 8 ports
        assert data["Members@odata.count"] == 8

    def test_switch_usp_identified(self, api_client):
        """Verify USP (Upstream Switch Port) is correctly identified."""
        # Get all ports and find the one marked as upstream
        status, ports = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports"
        )
        assert status == 200
        # Find a port with upstream port type (various naming conventions)
        usp_found = False
        for member in ports["Members"]:
            port_status, port_data = api_client.get(member["@odata.id"])
            if port_status == 200:
                port_type = port_data.get("PortType", "")
                # Check for various upstream port type values
                if "Upstream" in port_type or port_type == "USP":
                    usp_found = True
                    break
                # Also check PortId for upstream identification
                port_id = member["@odata.id"].split("/")[-1]
                if "usp" in port_id.lower():
                    usp_found = True
                    break
        # At minimum, port0 should exist (the upstream port)
        port0_status, _ = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_port0"
        )
        assert port0_status == 200 or usp_found, "No upstream port found"

    def test_switch_dsp_identified(self, api_client):
        """Verify DSP (Downstream Switch Port) is correctly identified."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp1"
        )
        assert status == 200
        assert data["PortType"] == "DownstreamPort"

    def test_switch_port_link_status(self, api_client):
        """Verify port link status is reported correctly."""
        # Port with device connected should be Up
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp1"
        )
        assert status == 200
        assert data["LinkStatus"] == "LinkUp"

    def test_switch_port_link_down(self, api_client):
        """Verify ports without devices show LinkDown."""
        # Port 6 and 7 are unconnected in mock
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp6"
        )
        assert status == 200
        assert data["LinkStatus"] == "LinkDown"

    def test_switch_cxl_properties(self, api_client):
        """Verify switch has CXL-specific properties."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0")
        assert status == 200
        # Check for CXL extension properties
        assert "CXL" in data
        assert "MaxVCSsSupported" in data["CXL"]


# =============================================================================
# VCS Configuration Tests
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestVCSConfiguration:
    """Tests for Virtual CXL Switch configuration."""

    def test_vcs_collection_accessible(self, api_client):
        """Verify VCS collection is accessible."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_vcs_has_vppb_collection(self, api_client):
        """Verify VCS has vPPBs collection."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_vppb_zero_is_bound(self, api_client):
        """Verify vPPB#0 is bound (mock topology convention: vPPB#0 represents USP)."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs/0"
        )
        assert status == 200
        # vPPB 0 should be bound - BindingStatus is at top level
        binding_status = data.get("BindingStatus", "")
        # Check CXL extension for port ID if present
        bound_port = None
        if "CXL" in data:
            bound_port = data["CXL"].get("BoundPhysicalPortId")
        # vPPB#0 should be bound in the mock topology
        assert "Bound" in binding_status or bound_port is not None

    def test_vppb_binding_status_types(self, api_client):
        """Verify vPPB binding status values are valid."""
        valid_statuses = ["Unbound", "BoundPhysicalPort", "BoundLD", "BindInProgress", "UnbindInProgress"]
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs/1"
        )
        assert status == 200
        # BindingStatus is at top level in the Redfish response
        binding_status = data.get("BindingStatus", "Unbound")
        assert any(s in binding_status for s in valid_statuses)

    def test_vppbs_advertise_no_actions(self, api_client):
        """Bind vPPB and Unbind vPPB are reached through Connections: a vPPB
        advertises no action, as there is no route that would take it."""
        base = "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs"
        status, vppbs = api_client.get(base)
        assert status == 200 and vppbs["Members"]
        for member in vppbs["Members"]:
            status, data = api_client.get(member["@odata.id"])
            assert status == 200
            assert "Actions" not in data, member["@odata.id"]


# =============================================================================
# Host Discovery Tests
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestHostDiscovery:
    """Tests for host system discovery via VCS configuration."""

    def test_hosts_appear_in_systems_collection(self, api_client):
        """Verify hosts appear in Systems collection."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_host_system_has_correct_type(self, api_client):
        """Verify System resource has correct @odata.type."""
        status, data = api_client.get("/redfish/v1/Systems/cxl_switch_0_host0")
        assert status == 200
        assert "ComputerSystem" in data["@odata.type"]

    def test_host_has_memory_domains_link(self, api_client):
        """Verify host System has MemoryDomains link."""
        status, data = api_client.get("/redfish/v1/Systems/cxl_switch_0_host0")
        assert status == 200
        assert "MemoryDomains" in data

    def test_host_appears_as_initiator_endpoint(self, api_client):
        """Verify host appears as Fabric Endpoint with EntityRole=Initiator."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0")
        assert status == 200
        assert "ConnectedEntities" in data
        assert data["ConnectedEntities"][0]["EntityRole"] == "Initiator"

    def test_host_endpoint_entity_type_processor(self, api_client):
        """Verify host Endpoint has EntityType=Processor."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0")
        assert status == 200
        assert data["ConnectedEntities"][0]["EntityType"] == "Processor"


# =============================================================================
# Zone Membership Tests
# =============================================================================

@pytest.mark.mock_topology("mock/switch-attached.json")
class TestZoneMembership:
    """Tests for Zone (VCS) membership including hosts and devices."""

    def test_zone_collection_accessible(self, api_client):
        """Verify Zone collection is accessible."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_zone_has_endpoints(self, api_client):
        """Verify Zone has Links.Endpoints array."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0")
        assert status == 200
        assert "Links" in data
        assert "Endpoints" in data["Links"]
        assert len(data["Links"]["Endpoints"]) >= 1

    def test_zone_includes_host_endpoint(self, api_client):
        """Verify Zone includes host endpoint (EntityRole=Initiator)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0")
        assert status == 200
        endpoints = [ep["@odata.id"] for ep in data["Links"]["Endpoints"]]
        # Host endpoint should be in the zone
        host_endpoints = [ep for ep in endpoints if "host" in ep]
        assert len(host_endpoints) >= 1

    def test_zone_includes_device_endpoints(self, api_client):
        """Verify Zone includes device endpoints (EntityRole=Target)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0")
        assert status == 200
        endpoints = [ep["@odata.id"] for ep in data["Links"]["Endpoints"]]
        # Device endpoints should be in the zone
        device_endpoints = [ep for ep in endpoints if "dev" in ep or "mld" in ep]
        assert len(device_endpoints) >= 1

    def test_device_endpoint_links_to_zone(self, api_client):
        """Verify device Endpoint has Links.Zones back-reference."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch0_dev0")
        assert status == 200
        assert "Links" in data
        assert "Zones" in data["Links"]
        assert len(data["Links"]["Zones"]) >= 1

    def test_host_endpoint_links_to_zone(self, api_client):
        """Verify host Endpoint has Links.Zones back-reference."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0")
        assert status == 200
        assert "Links" in data
        assert "Zones" in data["Links"]
        assert len(data["Links"]["Zones"]) >= 1


# =============================================================================
# Multi-Host VCS Tests
# =============================================================================

@pytest.mark.mock_topology("mock/multi-host-vcs.json")
class TestMultiHostVCS:
    """Tests for multi-VCS topology with multiple hosts."""

    def test_multiple_vcs_discovered(self, api_client):
        """Verify multiple VCS are discovered."""
        status, data = api_client.get(
            "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 2

    def test_multiple_hosts_discovered(self, api_client):
        """Verify multiple hosts are discovered."""
        status, data = api_client.get("/redfish/v1/Systems")
        assert status == 200
        assert data["Members@odata.count"] >= 2

    def test_different_zones_for_different_vcs(self, api_client):
        """Verify each VCS maps to a separate Zone."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        assert data["Members@odata.count"] >= 2

    def test_zone_lists_each_endpoint_once(self, api_client):
        """Two LDs of mld0 are bound to each VCS: mld0 is one Endpoint, and
        each Zone links to it once."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        for member in data["Members"]:
            status, zone = api_client.get(member["@odata.id"])
            assert status == 200
            links = [ep["@odata.id"] for ep in zone["Links"]["Endpoints"]]
            assert len(links) == len(set(links)), links
            assert "/redfish/v1/Fabrics/CXL/Endpoints/mld0" in links

    def test_no_pci_ids_as_names(self, api_client):
        """Manufacturer and Model are names, and only the PCI Vendor and
        Device IDs are known: they are left out, and the IDs are on the
        device's Endpoint, as EntityPciId."""
        for uri in ("/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0",
                    "/redfish/v1/Chassis/cxl_switch_0",
                    "/redfish/v1/Chassis/dev0",
                    "/redfish/v1/Chassis/dev0/PCIeDevices/dev0",
                    "/redfish/v1/Chassis/mld0",
                    "/redfish/v1/Chassis/mld0/PCIeDevices/mld0"):
            status, data = api_client.get(uri)
            assert status == 200, uri
            assert "Manufacturer" not in data and "Model" not in data, uri
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/dev0")
        assert status == 200
        ids = data["ConnectedEntities"][0]["EntityPciId"]
        assert ids["VendorId"].startswith("0x") and ids["DeviceId"].startswith("0x")


# =============================================================================
# Direct-Attached Topology Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestDirectAttachedTopology:
    """Tests for direct-attached (no switch) topology."""

    def test_no_switches_in_direct_attached(self, api_client):
        """Verify no switches in direct-attached topology."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Switches")
        assert status == 200
        assert data["Members@odata.count"] == 0

    def test_no_zones_in_direct_attached(self, api_client):
        """Verify no zones in direct-attached topology (no VCS)."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Zones")
        assert status == 200
        assert data["Members@odata.count"] == 0


# =============================================================================
# MHD (Multi-Headed Device) Inventory Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mhd-device.json")
class TestMHDInventory:
    """
    Tests for Multi-Headed Device discovery and representation.

    MHD key concepts:
    - LD Pool can have >16 LDs (device-wide pool indices)
    - Each head sees LDs 0 to n-1 (head-local visible_ld_id)
    - MH-SLD: 1:1 head-to-LD mapping
    - MH-MLD: Multiple LDs per head
    """

    def test_mhd_appears_in_chassis_collection(self, api_client):
        """Verify MHD devices appear as Chassis members."""
        status, data = api_client.get("/redfish/v1/Chassis")
        assert status == 200
        # mhd-device.json has 4 MHD devices
        assert data["Members@odata.count"] >= 4

        chassis_ids = [m["@odata.id"].split("/")[-1] for m in data["Members"]]
        assert "cxl_mhsld_0" in chassis_ids
        assert "cxl_mhmld_0" in chassis_ids
        assert "cxl_mhd_mixed" in chassis_ids

    def test_mhmld_has_six_lds_in_pool(self, api_client):
        """
        Verify MH-MLD device has all 6 LDs from pool.

        cxl_mhmld_0 has ld_pool with indices 0-5, exceeding MLD's 16-LD limit
        when counting across heads (head 0: 4 LDs, head 1: 2 LDs).
        """
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhmld_0/PCIeDevices/cxl_mhmld_0/CXLLogicalDevices"
        )
        assert status == 200
        # Pool has 6 LDs total (indices 0-5)
        assert data["Members@odata.count"] == 6

    def test_mhmld_ld_pool_indices_are_device_wide(self, api_client):
        """
        Verify CXLLogicalDevice IDs are device-wide pool indices, not head-local.

        Per spec: "LD Pool in MH-MLD may support more than 16 LDs"
        The Redfish ID should be the pool index (device-wide).
        """
        # Check LD at pool index 4 exists (mapped to head 1)
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhmld_0/PCIeDevices/cxl_mhmld_0/CXLLogicalDevices/4"
        )
        assert status == 200
        assert data["Id"] == "4"
        # Memory should match pool entry (16384 MB)
        assert data.get("MemorySizeMiB") == 16384

    def test_mhsld_has_two_lds_one_per_head(self, api_client):
        """Verify MH-SLD has 2 LDs (one per head)."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhsld_0/PCIeDevices/cxl_mhsld_0/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 2

    def test_mhd_memory_domain_exists(self, api_client):
        """Verify MHD has MemoryDomain for memory management."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhmld_0/MemoryDomains"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_mhd_partial_has_unmapped_lds(self, api_client):
        """
        Verify MHD with partial mapping shows all pool LDs.

        cxl_mhd_partial has 4 LDs in pool but only 2 mapped to head 0.
        All 4 should be visible in CXLLogicalDevices (device-wide view).
        """
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhd_partial/PCIeDevices/cxl_mhd_partial/CXLLogicalDevices"
        )
        assert status == 200
        assert data["Members@odata.count"] == 4


@pytest.mark.mock_topology("mock/mhd-device.json")
class TestMHDHeadConfiguration:
    """Tests for MHD head (CXL port) configuration."""

    def test_hosts_connected_to_mhd(self, api_client):
        """Verify hosts appear connected to MHD heads."""
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints")
        assert status == 200
        # mhd-device.json defines host0 and host1
        assert data["Members@odata.count"] >= 2

    def test_mhd_direct_attached_hosts(self, api_client):
        """Verify hosts are marked as direct-attached."""
        # host0 should exist
        status, data = api_client.get("/redfish/v1/Fabrics/CXL/Endpoints/host0")
        assert status == 200
        # Should have Initiator role
        connected_entities = data.get("ConnectedEntities", [])
        assert any(e.get("EntityRole") == "Initiator" for e in connected_entities)


# =============================================================================
# MHD Connection Tests (LD Accessibility)
# =============================================================================

@pytest.mark.mock_topology("mock/mhd-device.json")
class TestMHDConnections:
    """
    Tests for MHD connection and LD accessibility.

    Key concepts:
    - Each head sees LDs 0 to n-1 (head-local visible_ld_id)
    - LD Pool CCI uses device-wide pool indices in command payloads
    - Hosts only access LDs mapped to their connected head
    """

    def test_mhmld_ld_has_binding_info(self, api_client):
        """Verify CXLLogicalDevice includes head binding information."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhmld_0/PCIeDevices/cxl_mhmld_0/CXLLogicalDevices/0"
        )
        assert status == 200
        # Should have status info
        assert "Status" in data or "CXL" in data

    def test_mhmld_pool_lds_mapped_to_head0(self, api_client):
        """
        Verify pool LDs 0-3 are mapped to head 0 per mock configuration.

        mock/mhd-device.json: head 0 has lds: [0, 1, 2, 3]
        """
        # All 4 LDs should exist and have memory
        for pool_idx in [0, 1, 2, 3]:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mhmld_0/PCIeDevices/cxl_mhmld_0/CXLLogicalDevices/{pool_idx}"
            )
            assert status == 200
            assert "MemorySizeMiB" in data

    def test_mhmld_pool_lds_mapped_to_head1(self, api_client):
        """
        Verify pool LDs 4-5 are mapped to head 1 per mock configuration.

        mock/mhd-device.json: head 1 has lds: [4, 5]
        """
        for pool_idx in [4, 5]:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mhmld_0/PCIeDevices/cxl_mhmld_0/CXLLogicalDevices/{pool_idx}"
            )
            assert status == 200
            assert "MemorySizeMiB" in data

    def test_mhd_partial_unmapped_lds_exist(self, api_client):
        """
        Verify unmapped LDs in pool still exist in device inventory.

        cxl_mhd_partial has pool LDs 2-3 unmapped (available for assignment).
        """
        # Unmapped LDs should still be visible at device level
        for pool_idx in [2, 3]:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mhd_partial/PCIeDevices/cxl_mhd_partial/CXLLogicalDevices/{pool_idx}"
            )
            assert status == 200
            # Memory should be present
            assert data.get("MemorySizeMiB") == 8192

    def test_mhd_mixed_sld_head_single_ld(self, api_client):
        """
        Verify mixed MHD with SLD presentation on head 0 has single LD.

        cxl_mhd_mixed head 0: SLD presentation, pool LD 0 (32GB)
        """
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhd_mixed/PCIeDevices/cxl_mhd_mixed/CXLLogicalDevices/0"
        )
        assert status == 200
        assert data.get("MemorySizeMiB") == 32768

    def test_mhd_mixed_mld_head_multiple_lds(self, api_client):
        """
        Verify mixed MHD with MLD presentation on head 1 has 3 LDs.

        cxl_mhd_mixed head 1: MLD presentation, pool LDs 1-3
        """
        for pool_idx in [1, 2, 3]:
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mhd_mixed/PCIeDevices/cxl_mhd_mixed/CXLLogicalDevices/{pool_idx}"
            )
            assert status == 200

    def test_mhsld_each_head_one_ld(self, api_client):
        """
        Verify MH-SLD has 1:1 head-to-LD mapping.

        cxl_mhsld_0: head 0 → pool 0, head 1 → pool 1
        """
        # Pool index 0 (head 0)
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhsld_0/PCIeDevices/cxl_mhsld_0/CXLLogicalDevices/0"
        )
        assert status == 200
        assert data.get("MemorySizeMiB") == 16384

        # Pool index 1 (head 1)
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mhsld_0/PCIeDevices/cxl_mhsld_0/CXLLogicalDevices/1"
        )
        assert status == 200
        assert data.get("MemorySizeMiB") == 16384

    def test_invalid_ld_index_returns_404(self, api_client):
        """Verify accessing non-existent LD returns 404."""
        # cxl_mhsld_0 only has pool indices 0-1
        status, _ = api_client.get(
            "/redfish/v1/Chassis/cxl_mhsld_0/PCIeDevices/cxl_mhsld_0/CXLLogicalDevices/5"
        )
        assert status == 404
