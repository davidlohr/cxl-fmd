# SPDX-License-Identifier: LGPL-2.1
"""
Tests for Dynamic Capacity Device (DCD) support.

Exercises the FM-API DCD Management (56h) to Redfish mapping described in
docs/DCD-FM-API.md against mock/dcd-device.json:

- PCIeDevice.CXLDevice/DynamicCapacity          (Get DCD Info 5600h)
- CXLLogicalDevice/MemoryRegions                (Get Host DC Region Config 5601h)
- PATCH MemoryRegion                            (Set DC Region Config 5602h)
- PCIeDevice/MemoryExtents                      (Get DC Region Extent Lists 5603h)
- CXLLogicalDevice.DynamicCapacityAdd           (Initiate DC Add 5604h)
- CXLLogicalDevice.DynamicCapacityRelease       (Initiate DC Release 5605h)
- PCIeDevice.CXLReferenceDCMemory               (DC Add/Remove Reference 5606h/5607h)
"""

import pytest

SLD = "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0"
SLD_LD0 = SLD + "/CXLLogicalDevices/0"
SLD_ADD = SLD_LD0 + "/Actions/CXLLogicalDevice.DynamicCapacityAdd"
SLD_RELEASE = SLD_LD0 + "/Actions/CXLLogicalDevice.DynamicCapacityRelease"
SLD_REFERENCE = SLD + "/Actions/PCIeDevice.CXLReferenceDCMemory"

MLD = "/redfish/v1/Chassis/cxl_dcd_mld0/PCIeDevices/cxl_dcd_mld0"

TAG_SEED = "6f1a2c3e-0000-4000-8000-000000000001"   # extent present at startup
TAG_SHARED = "6f1a2c3e-0000-4000-8000-000000000002"
TAG_PENDING = "6f1a2c3e-0000-4000-8000-000000000003"
TAG_NEW = "6f1a2c3e-0000-4000-8000-0000000000aa"


def extent_ids(api_client, device=SLD):
    status, data = api_client.get(device + "/MemoryExtents")
    assert status == 200
    return sorted(m["@odata.id"].rsplit("/", 1)[1] for m in data["Members"])


def release_body(policy, **kw):
    body = {"Policy": policy, "ForcedRemoval": False, "SanitizeOnRelease": False}
    body.update(kw)
    return body


# =============================================================================
# Discovery (read path)
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestDCDDiscovery:
    """Static DCD state from the mock topology."""

    def test_pcie_device_dynamic_capacity(self, api_client):
        status, data = api_client.get(SLD)
        assert status == 200
        assert data["@odata.type"] == "#PCIeDevice.v1_23_0.PCIeDevice"
        assert data["CXLDevice"]["DeviceClass"] == "SLD"
        dc = data["CXLDevice"]["DynamicCapacity"]
        assert dc["MaxHosts"] == 1
        assert dc["MaxDynamicCapacityRegions"] == 2
        assert dc["TotalDynamicCapacityMiB"] == 32768 + 16384
        assert dc["AddCapacityPoliciesSupported"] == ["Free", "Contiguous", "Prescriptive"]
        assert dc["ReleaseCapacityPoliciesSupported"] == ["TagBased", "Prescriptive"]
        assert {"RegionNumber": 0, "BlockSizeMiB": [256, 512, 1024]} in dc["MemoryBlockSizesSupported"]
        assert {"RegionNumber": 1, "SanitizationOnReleaseSupported": False} in dc["SanitizationOnReleaseSupport"]
        assert dc["MemoryExtents"]["@odata.id"] == SLD + "/MemoryExtents"
        assert "#PCIeDevice.CXLReferenceDCMemory" in data["Actions"]

    def test_mld_device_class(self, api_client):
        status, data = api_client.get(MLD)
        assert status == 200
        assert data["CXLDevice"]["DeviceClass"] == "MLD"
        assert "DynamicCapacity" in data["CXLDevice"]

    def test_logical_device_dc_properties(self, api_client):
        status, data = api_client.get(SLD_LD0)
        assert status == 200
        assert data["@odata.type"] == "#CXLLogicalDevice.v1_5_0.CXLLogicalDevice"
        assert data["MemoryRegions"]["@odata.id"] == SLD_LD0 + "/MemoryRegions"
        assert data["TotalSupportedExtents"] == 64
        assert data["AvailableExtents"] == 63      # one extent in the mock
        assert data["TotalSupportedTags"] == 16
        actions = data["Actions"]
        assert actions["#CXLLogicalDevice.DynamicCapacityAdd"]["target"] == SLD_ADD
        assert actions["#CXLLogicalDevice.DynamicCapacityRelease"]["target"] == SLD_RELEASE

    def test_memory_regions_collection(self, api_client):
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions")
        assert status == 200
        assert data["Members@odata.count"] == 2
        assert [m["@odata.id"] for m in data["Members"]] == [
            SLD_LD0 + "/MemoryRegions/0", SLD_LD0 + "/MemoryRegions/1"]

    def test_memory_region_properties(self, api_client):
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert status == 200
        assert data["@odata.type"] == "#MemoryRegion.v1_1_0.MemoryRegion"
        assert data["RegionType"] == "Dynamic"
        assert data["RegionNumber"] == 0
        assert data["RegionBaseOffsetMiB"] == 16384
        assert data["RegionSizeMiB"] == 32768
        assert data["BlockSizeMiB"] == 256
        assert data["SanitizeOnRelease"] is True
        assert data["ShareableRegion"] is False
        assert data["InterconnectSpecificDCManagement"] is True
        assert data["ExtentsCount"] == 1
        ext = data["MemoryExtents"][0]
        assert ext["Pending"] is False
        assert ext["ExtentLink"]["@odata.id"] == SLD + "/MemoryExtents/ext_400000000"

    def test_region_flags(self, api_client):
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert status == 200
        assert data["NonVolatileRegion"] is True
        assert data["SanitizeOnRelease"] is False
        assert data["ExtentsCount"] == 0

    def test_unknown_region_404(self, api_client):
        status, _ = api_client.get(SLD_LD0 + "/MemoryRegions/5")
        assert status == 404

    def test_memory_extent(self, api_client):
        status, data = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert status == 200
        assert data["@odata.type"] == "#MemoryExtent.v1_0_0.MemoryExtent"
        assert data["ExtentOffsetMiB"] == 16384
        assert data["ExtentSizeMiB"] == 4096
        assert data["Tag"] == TAG_SEED
        assert data["SequenceNumber"] == 0
        assert data["FMReferencedExtent"] is False
        assert data["Links"]["MemoryRegions"] == [{"@odata.id": SLD_LD0 + "/MemoryRegions/0"}]

    def test_unknown_extent_404(self, api_client):
        status, _ = api_client.get(SLD + "/MemoryExtents/ext_1")
        assert status == 404
        status, _ = api_client.get(SLD + "/MemoryExtents/bogus")
        assert status == 404

    def test_shared_extent_links_every_host_region(self, api_client):
        status, data = api_client.get(MLD + "/MemoryExtents/ext_200000000")
        assert status == 200
        assert data["Tag"] == TAG_SHARED
        assert data["FMReferencedExtent"] is True
        regions = {r["@odata.id"] for r in data["Links"]["MemoryRegions"]}
        assert regions == {MLD + "/CXLLogicalDevices/0/MemoryRegions/0",
                           MLD + "/CXLLogicalDevices/1/MemoryRegions/0"}

    def test_pending_extent_flagged_in_region(self, api_client):
        status, data = api_client.get(MLD + "/CXLLogicalDevices/1/MemoryRegions/0")
        assert status == 200
        pending = {e["ExtentLink"]["@odata.id"]: e["Pending"] for e in data["MemoryExtents"]}
        assert pending[MLD + "/MemoryExtents/ext_280000000"] is True
        assert pending[MLD + "/MemoryExtents/ext_200000000"] is False

    def test_mld_host_views_are_per_ld(self, api_client):
        """Host 0 does not see the extent that was only offered to host 1."""
        status, data = api_client.get(MLD + "/CXLLogicalDevices/0/MemoryRegions/0")
        assert status == 200
        links = [e["ExtentLink"]["@odata.id"] for e in data["MemoryExtents"]]
        assert MLD + "/MemoryExtents/ext_280000000" not in links


# =============================================================================
# Dynamic Capacity Add (5604h)
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestDynamicCapacityAdd:

    def test_add_free(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW})
        assert status == 200

        ids = extent_ids(api_client)
        assert len(ids) == 2
        # First free block after the seed extent: 16384 + 4096 MiB = 0x500000000
        assert "ext_500000000" in ids

        status, data = api_client.get(SLD + "/MemoryExtents/ext_500000000")
        assert status == 200
        assert data["ExtentSizeMiB"] == 1024
        assert data["Tag"] == TAG_NEW

        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert data["ExtentsCount"] == 2

        status, data = api_client.get(SLD_LD0)
        assert data["AvailableExtents"] == 62

    def test_add_contiguous_other_region(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 1, "Length": 512, "Tag": TAG_NEW})
        assert status == 200
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert data["ExtentsCount"] == 1
        # Region 1 starts at 49152 MiB = 0xC00000000
        assert data["MemoryExtents"][0]["ExtentLink"]["@odata.id"].endswith("/ext_c00000000")

    def test_add_free_spans_gaps(self, api_client):
        """Free policy may be satisfied from several gaps; Contiguous may not."""
        # Three 256 MiB extents at the start of region 1, then punch a hole
        for _ in range(3):
            status, _, _ = api_client.post(SLD_ADD, {
                "Policy": "Contiguous", "RegionNumber": 1, "Length": 256, "Tag": TAG_NEW})
            assert status == 200
        assert api_client.delete(SLD + "/MemoryExtents/ext_c10000000") == 204

        # Fill the rest of the region so only the hole and a 256 MiB tail are free
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 1, "Length": 16384 - 1024, "Tag": TAG_NEW})
        assert status == 200

        # 512 MiB contiguous cannot be satisfied, but Free can span both gaps
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 1, "Length": 512, "Tag": TAG_NEW})
        assert status == 409
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 1, "Length": 512, "Tag": TAG_NEW})
        assert status == 200

        ids = extent_ids(api_client)
        assert "ext_c10000000" in ids            # the hole
        assert "ext_ff0000000" in ids            # the 256 MiB tail
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert data["ExtentsCount"] == 5

    def test_length_not_block_aligned_400(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 100, "Tag": TAG_NEW})
        assert status == 400

    def test_insufficient_capacity_409(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 1, "Length": 16384 + 256, "Tag": TAG_NEW})
        assert status == 400   # larger than the region is a bad request
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 32768, "Tag": TAG_NEW})
        assert status == 409   # region 0 already has 4096 MiB allocated

    def test_unsupported_policy_400(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_NEW})
        assert status == 400

    def test_missing_parameters_400(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {"Policy": "Free", "RegionNumber": 0, "Length": 256})
        assert status == 400
        status, _, _ = api_client.post(SLD_ADD, {"Policy": "Prescriptive"})
        assert status == 400
        status, _, _ = api_client.post(SLD_ADD, {"Policy": "Bogus"})
        assert status == 400

    def test_malformed_json_400(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, None, raw_body="{not json")
        assert status == 400

    def test_bad_tag_400(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": "not-a-uuid"})
        assert status == 400

    def test_unknown_region_404(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 7, "Length": 256, "Tag": TAG_NEW})
        assert status == 404

    def test_non_dcd_ld_404(self, api_client):
        status, _, _ = api_client.post(
            SLD + "/CXLLogicalDevices/3/Actions/CXLLogicalDevice.DynamicCapacityAdd",
            {"Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": TAG_NEW})
        assert status == 404

    def test_shared_access_adds_host(self, api_client):
        """EnableSharedAccess gives host 0 the extents host 1 already has."""
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_PENDING})
        assert status == 200
        status, data = api_client.get(MLD + "/MemoryExtents/ext_280000000")
        regions = {r["@odata.id"] for r in data["Links"]["MemoryRegions"]}
        assert MLD + "/CXLLogicalDevices/0/MemoryRegions/0" in regions

    def test_prescriptive_on_unshareable_region_409(self, api_client):
        """Region 0 of the SLD is not shareable, so a second host can't join."""
        # The SLD has a single host, so prescriptive re-add of an owned extent is EEXIST
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Prescriptive",
            "ExtentList": [{"@odata.id": SLD + "/MemoryExtents/ext_400000000"}]})
        assert status == 409


# =============================================================================
# Dynamic Capacity Release (5605h) and DELETE MemoryExtent
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestDynamicCapacityRelease:

    def test_release_tag_based_all(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=0))
        assert status == 200
        assert extent_ids(api_client) == []
        status, data = api_client.get(SLD_LD0)
        assert data["AvailableExtents"] == 64

    def test_release_tag_based_partial_trims_extent(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=1024))
        assert status == 200
        status, data = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert status == 200
        assert data["ExtentSizeMiB"] == 4096 - 1024

    def test_release_prescriptive(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "Prescriptive",
            ExtentList=[{"@odata.id": SLD + "/MemoryExtents/ext_400000000"}]))
        assert status == 200
        assert extent_ids(api_client) == []

    def test_delete_extent(self, api_client):
        status = api_client.delete(SLD + "/MemoryExtents/ext_400000000")
        assert status == 204
        status, _ = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert status == 404
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert data["ExtentsCount"] == 0

    def test_delete_unknown_extent_404(self, api_client):
        status = api_client.delete(SLD + "/MemoryExtents/ext_1")
        assert status == 404

    def test_release_unknown_tag_404(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag="00000000-0000-0000-0000-0000000000ff", Length=0))
        assert status == 404

    def test_release_requires_booleans(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, {
            "Policy": "TagBased", "Tag": TAG_SEED, "Length": 0})
        assert status == 400
        status, _, _ = api_client.post(SLD_RELEASE, {
            "Policy": "TagBased", "Tag": TAG_SEED, "Length": 0,
            "ForcedRemoval": "no", "SanitizeOnRelease": False})
        assert status == 400

    def test_release_tag_based_requires_tag_and_length(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body("TagBased", Tag=TAG_SEED))
        assert status == 400
        status, _, _ = api_client.post(SLD_RELEASE, release_body("Prescriptive"))
        assert status == 400

    def test_shared_extent_release_keeps_other_host(self, api_client):
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(rel, release_body(
            "Prescriptive",
            ExtentList=[{"@odata.id": MLD + "/MemoryExtents/ext_200000000"}]))
        assert status == 200
        status, data = api_client.get(MLD + "/MemoryExtents/ext_200000000")
        assert status == 200
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/1/MemoryRegions/0"}]

    def test_add_then_delete_round_trip(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": TAG_NEW})
        assert status == 200
        assert "ext_500000000" in extent_ids(api_client)
        assert api_client.delete(SLD + "/MemoryExtents/ext_500000000") == 204
        assert extent_ids(api_client) == ["ext_400000000"]


# =============================================================================
# Set DC Region Configuration (5602h)
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestSetDCRegionConfig:

    def test_patch_rejected_while_allocated(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/0", {"BlockSizeMiB": 512})
        assert status == 409

    def test_patch_after_release(self, api_client):
        assert api_client.delete(SLD + "/MemoryExtents/ext_400000000") == 204
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/0",
                                     {"BlockSizeMiB": 512, "SanitizeOnRelease": False})
        assert status == 200
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert data["BlockSizeMiB"] == 512
        assert data["SanitizeOnRelease"] is False

        # Adds now have to honour the new block size
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": TAG_NEW})
        assert status == 400
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 512, "Tag": TAG_NEW})
        assert status == 200

    def test_patch_unsupported_block_size_400(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/1", {"BlockSizeMiB": 128})
        assert status == 400

    def test_patch_sanitize_not_configurable_400(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/1", {"SanitizeOnRelease": True})
        assert status == 400

    def test_patch_readonly_property_400(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/1", {"RegionSizeMiB": 1})
        assert status == 400

    def test_patch_unknown_region_404(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/6", {"BlockSizeMiB": 256})
        assert status == 404


# =============================================================================
# DC Add/Remove Reference (5606h/5607h)
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestCXLReferenceDCMemory:

    def test_reference_keeps_released_capacity(self, api_client):
        status, _, _ = api_client.post(SLD_REFERENCE, {"Tag": TAG_SEED, "FMReference": True})
        assert status == 200
        status, data = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert data["FMReferencedExtent"] is True

        # Host releases it: extent survives with no region links
        assert api_client.delete(SLD + "/MemoryExtents/ext_400000000") == 204
        status, data = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert status == 200
        assert data["Links"]["MemoryRegions"] == []

        # A second DELETE has nothing to release from a host
        assert api_client.delete(SLD + "/MemoryExtents/ext_400000000") == 409

        # Dropping the FM reference frees it
        status, _, _ = api_client.post(SLD_REFERENCE, {"Tag": TAG_SEED, "FMReference": False})
        assert status == 200
        assert extent_ids(api_client) == []

    def test_reference_unknown_tag_404(self, api_client):
        status, _, _ = api_client.post(SLD_REFERENCE, {
            "Tag": "00000000-0000-0000-0000-0000000000ff", "FMReference": True})
        assert status == 404

    def test_reference_requires_parameters(self, api_client):
        status, _, _ = api_client.post(SLD_REFERENCE, {"Tag": TAG_SEED})
        assert status == 400
        status, _, _ = api_client.post(SLD_REFERENCE, {"FMReference": True})
        assert status == 400

    def test_reference_on_non_dcd_404(self, api_client):
        status, _, _ = api_client.post(
            "/redfish/v1/Chassis/nope/PCIeDevices/nope/Actions/PCIeDevice.CXLReferenceDCMemory",
            {"Tag": TAG_SEED, "FMReference": True})
        assert status == 404


# =============================================================================
# Devices without DCD support are unaffected
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestNonDCDDevice:

    def test_no_dynamic_capacity_properties(self, api_client):
        status, data = api_client.get("/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0")
        assert status == 200
        assert "DynamicCapacity" not in data["CXLDevice"]
        assert "Actions" not in data
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices/0")
        assert "MemoryRegions" not in data
        assert "TotalSupportedExtents" not in data

    def test_dcd_endpoints_404(self, api_client):
        base = "/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0"
        assert api_client.get(base + "/MemoryExtents")[0] == 404
        assert api_client.get(base + "/CXLLogicalDevices/0/MemoryRegions")[0] == 404
        status, _, _ = api_client.post(
            base + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd",
            {"Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": TAG_NEW})
        assert status == 404
