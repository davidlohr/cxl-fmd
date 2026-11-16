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

import re

import pytest

SLD = "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0"
SLD_LD0 = SLD + "/CXLLogicalDevices/0"
SLD_ADD = SLD_LD0 + "/Actions/CXLLogicalDevice.DynamicCapacityAdd"
SLD_RELEASE = SLD_LD0 + "/Actions/CXLLogicalDevice.DynamicCapacityRelease"
SLD_REFERENCE = SLD + "/Actions/PCIeDevice.CXLReferenceDCMemory"

MLD = "/redfish/v1/Chassis/cxl_dcd_mld0/PCIeDevices/cxl_dcd_mld0"
MLD_REFERENCE = MLD + "/Actions/PCIeDevice.CXLReferenceDCMemory"
MLD_LD1_RELEASE = MLD + "/CXLLogicalDevices/1/Actions/CXLLogicalDevice.DynamicCapacityRelease"

TAG_SEED = "6f1a2c3e-0000-4000-8000-000000000001"   # extent present at startup
TAG_SHARED = "6f1a2c3e-0000-4000-8000-000000000002"
TAG_PENDING = "6f1a2c3e-0000-4000-8000-000000000003"
TAG_UNSHARED = "6f1a2c3e-0000-4000-8000-000000000004"   # MLD region 1, not Sharable
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
        assert data["AvailableTags"] == 15         # and one tag
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
        assert ext["ExtentLink"]["@odata.id"] == SLD + "/MemoryExtents/ext_0_400000000"

    def test_region_flags(self, api_client):
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert status == 200
        assert data["NonVolatileRegion"] is True
        assert data["SanitizeOnRelease"] is False
        # the file does not say: every DC region has it (Section 9.13.3)
        assert data["InterconnectSpecificDCManagement"] is True
        assert data["ExtentsCount"] == 0

    def test_unknown_region_404(self, api_client):
        status, _ = api_client.get(SLD_LD0 + "/MemoryRegions/5")
        assert status == 404

    def test_memory_extent(self, api_client):
        status, data = api_client.get(SLD + "/MemoryExtents/ext_0_400000000")
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
        status, _ = api_client.get(SLD + "/MemoryExtents/ext_0_1")
        assert status == 404

    def test_extent_ids_carry_ld(self, api_client):
        """5603h lists extents per host: an Id is ext_<ld>_<hex DPA>."""
        for device in (SLD, MLD):
            for ext in extent_ids(api_client, device):
                assert re.fullmatch(r"ext_[0-9]+_[0-9a-f]+", ext), ext
        # the DPA alone names no extent: two hosts can both have it
        status, _ = api_client.get(SLD + "/MemoryExtents/ext_400000000")
        assert status == 404
        status, _ = api_client.get(MLD + "/MemoryExtents/ext_200000000")
        assert status == 404

    def test_extent_id_has_one_spelling(self, api_client):
        """An extent has one Id: another spelling of its LD or DPA is no
        alias of it, for GET, DELETE or an ExtentList link."""
        for alias in ("ext_00_200000000", "ext_0_0x200000000", "ext_0_0200000000",
                      "ext_0_200000000A", "ext_+0_200000000", "ext_0_+200000000"):
            status, _ = api_client.get(MLD + "/MemoryExtents/" + alias)
            assert status == 404, alias
            assert api_client.delete(MLD + "/MemoryExtents/" + alias) == 404, alias
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(rel, release_body("Prescriptive", ExtentList=[
            {"@odata.id": MLD + "/MemoryExtents/ext_00_200000000"}]))
        assert status == 400
        status, data = api_client.get(MLD + "/MemoryExtents/ext_0_200000000")
        assert status == 200 and data["Id"] == "ext_0_200000000"

    def test_shared_capacity_is_one_extent_per_host(self, api_client):
        """Shared capacity is an extent in each host's list, with the same Tag."""
        seen = {}
        for ld in (0, 1):
            status, data = api_client.get(MLD + f"/MemoryExtents/ext_{ld}_200000000")
            assert status == 200
            assert data["Tag"] == TAG_SHARED
            assert data["FMReferencedExtent"] is True
            assert data["Links"]["MemoryRegions"] == [
                {"@odata.id": MLD + f"/CXLLogicalDevices/{ld}/MemoryRegions/0"}]
            seen[ld] = (data["SequenceNumber"], data["ExtentSizeMiB"])
        assert seen[0] == seen[1]

    def test_pending_extent_flagged_in_region(self, api_client):
        status, data = api_client.get(MLD + "/CXLLogicalDevices/1/MemoryRegions/0")
        assert status == 200
        pending = {e["ExtentLink"]["@odata.id"]: e["Pending"] for e in data["MemoryExtents"]}
        assert pending[MLD + "/MemoryExtents/ext_1_280000000"] is True
        assert pending[MLD + "/MemoryExtents/ext_1_200000000"] is False
        status, data = api_client.get(MLD + "/MemoryExtents/ext_1_280000000")
        assert data["Status"]["State"] == "Starting"
        status, data = api_client.get(MLD + "/MemoryExtents/ext_1_200000000")
        assert data["Status"]["State"] == "Enabled"

    def test_mld_host_views_are_per_ld(self, api_client):
        """A region lists its own host's extents only."""
        status, data = api_client.get(MLD + "/CXLLogicalDevices/0/MemoryRegions/0")
        assert status == 200
        links = [e["ExtentLink"]["@odata.id"] for e in data["MemoryExtents"]]
        assert links == [MLD + "/MemoryExtents/ext_0_200000000"]


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
        assert "ext_0_500000000" in ids

        status, data = api_client.get(SLD + "/MemoryExtents/ext_0_500000000")
        assert status == 200
        assert data["ExtentSizeMiB"] == 1024
        assert data["Tag"] == TAG_NEW

        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert data["ExtentsCount"] == 2

        status, data = api_client.get(SLD_LD0)
        assert data["AvailableExtents"] == 62
        assert data["AvailableTags"] == 14         # TAG_NEW is a second tag

    def test_add_contiguous_other_region(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Contiguous", "RegionNumber": 1, "Length": 512, "Tag": TAG_NEW})
        assert status == 200
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert data["ExtentsCount"] == 1
        # Region 1 starts at 49152 MiB = 0xC00000000
        assert data["MemoryExtents"][0]["ExtentLink"]["@odata.id"].endswith("/ext_0_c00000000")

    def test_add_free_spans_gaps(self, api_client):
        """Free policy may be satisfied from several gaps; Contiguous may not."""
        # Three 256 MiB extents at the start of region 1, then punch a hole
        for _ in range(3):
            status, _, _ = api_client.post(SLD_ADD, {
                "Policy": "Contiguous", "RegionNumber": 1, "Length": 256, "Tag": TAG_NEW})
            assert status == 200
        assert api_client.delete(SLD + "/MemoryExtents/ext_0_c10000000") == 204

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
        assert "ext_0_c10000000" in ids            # the hole
        assert "ext_0_ff0000000" in ids            # the 256 MiB tail
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/1")
        assert data["ExtentsCount"] == 5

    def test_shared_extent_sequence(self, api_client):
        """Table 8-230: in a shareable region, 0 to n-1, "each value appearing only once"."""
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        other = "6f1a2c3e-0000-4000-8000-0000000000bb"
        for tag in (TAG_NEW, other, TAG_NEW):
            status, _, _ = api_client.post(add, {
                "Policy": "Contiguous", "RegionNumber": 0, "Length": 256, "Tag": tag})
            assert status == 200
        status, _, _ = api_client.post(rel, release_body("TagBased", Tag=other, Length=256))
        assert status == 200
        # Free across the hole and the tail: two more pieces
        status, _, _ = api_client.post(add, {
            "Policy": "Free", "RegionNumber": 0, "Length": 512, "Tag": TAG_NEW})
        assert status == 200

        seqs = []
        for ext in extent_ids(api_client, MLD):
            status, data = api_client.get(MLD + "/MemoryExtents/" + ext)
            if data["Tag"] == TAG_NEW:
                seqs.append(data["SequenceNumber"])
        assert sorted(seqs) == [0, 1, 2, 3]

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
        """EnableSharedAccess gives host 0 an extent of its own with host 1's tag."""
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_PENDING})
        assert status == 200
        status, src = api_client.get(MLD + "/MemoryExtents/ext_1_280000000")
        status, data = api_client.get(MLD + "/MemoryExtents/ext_0_280000000")
        assert status == 200
        assert data["Tag"] == TAG_PENDING
        assert data["SequenceNumber"] == src["SequenceNumber"]
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/0/MemoryRegions/0"}]
        assert src["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/1/MemoryRegions/0"}]

    def test_shared_access_not_advertised(self, api_client):
        """Bit[3] of the add policies "Must be 0" (Table 7-65): never listed, still served."""
        status, data = api_client.get(MLD)
        assert "EnableSharedAccess" not in data["CXLDevice"]["DynamicCapacity"]["AddCapacityPoliciesSupported"]

    def test_shared_access_tag_already_held_400(self, api_client):
        """Invalid Input "When the same tagged capacity is currently accessible by the same LD"."""
        add = MLD + "/CXLLogicalDevices/1/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_PENDING})
        assert status == 400
        status, data = api_client.get(MLD + "/CXLLogicalDevices/1")
        assert data["AvailableExtents"] == 254     # unchanged: two extents held

    def test_shared_access_tag_in_unsharable_region_400(self, api_client):
        """Invalid Input "When the tagged capacity is already mapped to any Host ID via a non-Sharable region"."""
        add = MLD + "/CXLLogicalDevices/1/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_UNSHARED})
        assert status == 400
        status, data = api_client.get(MLD + "/MemoryExtents/ext_0_600000000")
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/0/MemoryRegions/1"}]

    def test_shared_access_empty_tag_400(self, api_client):
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        for tag in ("", "00000000-0000-0000-0000-000000000000"):
            status, _, _ = api_client.post(add, {
                "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": tag})
            assert status == 400

    def test_prescriptive_extent_already_held_400(self, api_client):
        """Invalid Extent List: "any of the DPAs are already accessible to the same LD"."""
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Prescriptive",
            "ExtentList": [{"@odata.id": SLD + "/MemoryExtents/ext_0_400000000"}]})
        assert status == 400

    def test_prescriptive_add_foreign_link_400(self, api_client):
        """An Extent List is in the DPA space of the host it names (Section
        9.13.3): another host's extent is not in host 0's, and is refused
        before anything is sent. Capacity another host holds is shared with
        EnableSharedAccess."""
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        status, data, _ = api_client.post(add, {
            "Policy": "Prescriptive",
            "ExtentList": [{"@odata.id": MLD + "/MemoryExtents/ext_1_280000000"}]})
        assert status == 400
        assert "another logical device" in data["error"]["message"]
        status, data = api_client.get(MLD + "/MemoryExtents/ext_1_280000000")
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/1/MemoryRegions/0"}]
        assert "ext_0_280000000" not in extent_ids(api_client, MLD)


# =============================================================================
# Dynamic Capacity Release (5605h) and DELETE MemoryExtent
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestDynamicCapacityRelease:

    def test_release_tag_based_all(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=4096))
        assert status == 200
        assert extent_ids(api_client) == []
        status, data = api_client.get(SLD_LD0)
        assert data["AvailableExtents"] == 64
        assert data["AvailableTags"] == 16

    def test_release_tag_based_partial_trims_extent(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=1024))
        assert status == 200
        status, data = api_client.get(SLD + "/MemoryExtents/ext_0_400000000")
        assert status == 200
        assert data["ExtentSizeMiB"] == 4096 - 1024

    def test_release_prescriptive(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "Prescriptive",
            ExtentList=[{"@odata.id": SLD + "/MemoryExtents/ext_0_400000000"}]))
        assert status == 200
        assert extent_ids(api_client) == []

    def test_release_prescriptive_same_extent_twice_400(self, api_client):
        """Listing an extent twice is refused, and releases nothing."""
        link = {"@odata.id": SLD + "/MemoryExtents/ext_0_400000000"}
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "Prescriptive", ExtentList=[link, link]))
        assert status == 400
        # the daemon is still there, and so is the extent
        assert extent_ids(api_client) == ["ext_0_400000000"]

    def test_delete_extent(self, api_client):
        status = api_client.delete(SLD + "/MemoryExtents/ext_0_400000000")
        assert status == 204
        status, _ = api_client.get(SLD + "/MemoryExtents/ext_0_400000000")
        assert status == 404
        status, data = api_client.get(SLD_LD0 + "/MemoryRegions/0")
        assert data["ExtentsCount"] == 0

    def test_delete_unknown_extent_404(self, api_client):
        status = api_client.delete(SLD + "/MemoryExtents/ext_1")
        assert status == 404

    def test_release_unknown_tag_404(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag="00000000-0000-0000-0000-0000000000ff", Length=256))
        assert status == 404

    def test_release_tag_based_length_zero_400(self, api_client):
        """Length "Shall be > 0" (Table 7-73): 0 does not mean everything."""
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=0))
        assert status == 400
        assert extent_ids(api_client) == ["ext_0_400000000"]

    def test_release_tag_based_more_than_held_409(self, api_client):
        """More than the host holds with the tag is Resources Exhausted."""
        status, _, _ = api_client.post(SLD_RELEASE, release_body(
            "TagBased", Tag=TAG_SEED, Length=4096 + 256))
        assert status == 409
        status, data = api_client.get(SLD + "/MemoryExtents/ext_0_400000000")
        assert data["ExtentSizeMiB"] == 4096

    def test_release_requires_booleans(self, api_client):
        status, _, _ = api_client.post(SLD_RELEASE, {
            "Policy": "TagBased", "Tag": TAG_SEED, "Length": 256})
        assert status == 400
        status, _, _ = api_client.post(SLD_RELEASE, {
            "Policy": "TagBased", "Tag": TAG_SEED, "Length": 256,
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
            ExtentList=[{"@odata.id": MLD + "/MemoryExtents/ext_0_200000000"}]))
        assert status == 200
        status, _ = api_client.get(MLD + "/MemoryExtents/ext_0_200000000")
        assert status == 404
        status, data = api_client.get(MLD + "/MemoryExtents/ext_1_200000000")
        assert status == 200
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": MLD + "/CXLLogicalDevices/1/MemoryRegions/0"}]

    def test_release_foreign_extent_400(self, api_client):
        """Host 0's list is host 0's: host 1's extent at the same DPA is not
        in it, and naming it is refused with 400."""
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, data, _ = api_client.post(rel, release_body(
            "Prescriptive",
            ExtentList=[{"@odata.id": MLD + "/MemoryExtents/ext_1_200000000"}]))
        assert status == 400
        assert "another logical device" in data["error"]["message"]
        assert "ext_1_200000000" in extent_ids(api_client, MLD)
        assert "ext_0_200000000" in extent_ids(api_client, MLD)

    def test_release_foreign_with_own_extent_400(self, api_client):
        """One foreign link refuses the whole list: nothing is released."""
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(rel, release_body(
            "Prescriptive",
            ExtentList=[{"@odata.id": MLD + "/MemoryExtents/ext_0_200000000"},
                        {"@odata.id": MLD + "/MemoryExtents/ext_1_200000000"}]))
        assert status == 400
        ids = extent_ids(api_client, MLD)
        assert "ext_0_200000000" in ids and "ext_1_200000000" in ids

    def test_release_pending_without_forced_400(self, api_client):
        """Invalid Physical Address for "a nonexistent or pending ... DPA range
        and the Forced Removal flag is not set" (Section 7.6.7.6.6)."""
        link = {"@odata.id": MLD + "/MemoryExtents/ext_1_280000000"}
        status, _, _ = api_client.post(MLD_LD1_RELEASE, release_body(
            "Prescriptive", ExtentList=[link]))
        assert status == 400
        assert "ext_1_280000000" in extent_ids(api_client, MLD)
        # with it, the device marks the Extent Group "Dead"
        status, _, _ = api_client.post(MLD_LD1_RELEASE, release_body(
            "Prescriptive", ForcedRemoval=True, ExtentList=[link]))
        assert status == 200
        assert "ext_1_280000000" not in extent_ids(api_client, MLD)

    def test_tagbased_partial_sharable_400(self, api_client):
        """Invalid Input "When the Tag represents sharable capacity, and the
        Extent List covers only a portion of the capacity associated with
        the Tag": the mock takes a Length short of the host's for it."""
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(rel, release_body("TagBased", Tag=TAG_SHARED, Length=1024))
        assert status == 400
        status, data = api_client.get(MLD + "/MemoryExtents/ext_0_200000000")
        assert data["ExtentSizeMiB"] == 2048
        # the whole of it removes host 0's access, and only host 0's
        status, _, _ = api_client.post(rel, release_body("TagBased", Tag=TAG_SHARED, Length=2048))
        assert status == 200
        ids = extent_ids(api_client, MLD)
        assert "ext_0_200000000" not in ids
        assert "ext_1_200000000" in ids

    def test_prescriptive_partial_sharable_400(self, api_client):
        """A Prescriptive list of part of a host's sharable tag is refused too."""
        add = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd"
        rel = MLD + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(add, {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 256, "Tag": TAG_SHARED})
        assert status == 200
        # host 0 now has TAG_SHARED in two extents: 8192 MiB and 11264 MiB
        first = {"@odata.id": MLD + "/MemoryExtents/ext_0_200000000"}
        second = {"@odata.id": MLD + "/MemoryExtents/ext_0_2c0000000"}
        status, data = api_client.get(second["@odata.id"])
        assert status == 200 and data["SequenceNumber"] == 1
        status, _, _ = api_client.post(rel, release_body("Prescriptive", ExtentList=[first]))
        assert status == 400
        assert "ext_0_200000000" in extent_ids(api_client, MLD)
        status, _, _ = api_client.post(rel, release_body("Prescriptive",
                                                         ExtentList=[first, second]))
        assert status == 200
        ids = extent_ids(api_client, MLD)
        assert "ext_0_200000000" not in ids and "ext_0_2c0000000" not in ids
        assert "ext_1_200000000" in ids

    def test_add_then_delete_round_trip(self, api_client):
        status, _, _ = api_client.post(SLD_ADD, {
            "Policy": "Free", "RegionNumber": 0, "Length": 256, "Tag": TAG_NEW})
        assert status == 200
        assert "ext_0_500000000" in extent_ids(api_client)
        assert api_client.delete(SLD + "/MemoryExtents/ext_0_500000000") == 204
        assert extent_ids(api_client) == ["ext_0_400000000"]


# =============================================================================
# Set DC Region Configuration (5602h)
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestSetDCRegionConfig:

    def test_patch_rejected_while_allocated(self, api_client):
        status, _ = api_client.patch(SLD_LD0 + "/MemoryRegions/0", {"BlockSizeMiB": 512})
        assert status == 409

    def test_patch_after_release(self, api_client):
        assert api_client.delete(SLD + "/MemoryExtents/ext_0_400000000") == 204
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

    def test_patch_rejected_while_fm_referenced(self, api_client):
        """Capacity an FM reference keeps is still allocated (Section 7.6.7.6.7)."""
        # DELETE releases an extent from its own host
        for ext in ("ext_0_200000000", "ext_1_200000000"):
            assert api_client.delete(MLD + "/MemoryExtents/" + ext) == 204
        # host 1 has not answered the add of this one: only Forced Removal
        status, _, _ = api_client.post(MLD_LD1_RELEASE, release_body(
            "Prescriptive", ForcedRemoval=True,
            ExtentList=[{"@odata.id": MLD + "/MemoryExtents/ext_1_280000000"}]))
        assert status == 200
        # no host's list has it, so no extent is left
        assert extent_ids(api_client, MLD) == ["ext_0_600000000"]
        status, _ = api_client.patch(MLD + "/CXLLogicalDevices/0/MemoryRegions/0",
                                     {"BlockSizeMiB": 512})
        assert status == 409

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
        rel = MLD + "/MemoryExtents/ext_1_280000000"
        status, _, _ = api_client.post(MLD_REFERENCE, {"Tag": TAG_PENDING, "FMReference": True})
        assert status == 200
        status, data = api_client.get(rel)
        assert data["FMReferencedExtent"] is True

        # Host releases it (Forced Removal: it is pending): no host's list
        # has it, so it is no extent
        status, _, _ = api_client.post(MLD_LD1_RELEASE, release_body(
            "Prescriptive", ForcedRemoval=True, ExtentList=[{"@odata.id": rel}]))
        assert status == 200
        status, data = api_client.get(rel)
        assert status == 404
        for ext in extent_ids(api_client, MLD):
            assert api_client.get(MLD + "/MemoryExtents/" + ext)[1]["Tag"] != TAG_PENDING
        # but it is still tracked, FM-referenced (Section 9.13.3.3)
        status, data = api_client.get(MLD + "/CXLLogicalDevices/1")
        assert data["AvailableExtents"] == 254
        assert data["AvailableTags"] == 62

        # A second DELETE has nothing to release
        assert api_client.delete(rel) == 404

        # Dropping the FM reference frees it
        status, _, _ = api_client.post(MLD_REFERENCE, {"Tag": TAG_PENDING, "FMReference": False})
        assert status == 200
        assert "ext_1_280000000" not in extent_ids(api_client, MLD)
        status, data = api_client.get(MLD + "/CXLLogicalDevices/1")
        assert data["AvailableExtents"] == 255
        assert data["AvailableTags"] == 63

    def test_reference_unknown_tag_400(self, api_client):
        """5606h: "Invalid Input if the Tag ... does not match an existing sharable tag"."""
        status, _, _ = api_client.post(SLD_REFERENCE, {
            "Tag": "00000000-0000-0000-0000-0000000000ff", "FMReference": True})
        assert status == 400

    def test_reference_unsharable_tag_400(self, api_client):
        """The SLD's tag is in a region that is not Sharable."""
        status, _, _ = api_client.post(SLD_REFERENCE, {"Tag": TAG_SEED, "FMReference": True})
        assert status == 400
        status, data = api_client.get(SLD + "/MemoryExtents/ext_0_400000000")
        assert data["FMReferencedExtent"] is False

    def test_reference_empty_tag_400(self, api_client):
        for tag in ("", "00000000-0000-0000-0000-000000000000"):
            status, _, _ = api_client.post(MLD_REFERENCE, {"Tag": tag, "FMReference": True})
            assert status == 400

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
        assert "Oem" not in data
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


# =============================================================================
# Dynamic Capacity Event Log records, as log entries
# =============================================================================

@pytest.mark.mock_topology("mock/dcd-device.json")
class TestDCDEventRecords:
    """The records of a DCD's Dynamic Capacity Event Log that are the FM's,
    Add Capacity Response (04h) and Capacity Released (05h), name their host
    by its LD-ID (Table 8-229 Host ID). Each is a LogEntry, CXLEntryType
    DynamicCapacity, in the DeviceLog of that host's CXLLogicalDevice."""

    BASE = "/redfish/v1/Chassis/cxl_dcd_mld0/PCIeDevices/cxl_dcd_mld0/CXLLogicalDevices"

    def entries(self, api_client, ld):
        status, data = api_client.get(f"{self.BASE}/{ld}/DeviceLog/Entries")
        assert status == 200
        return data["Members"]

    def test_each_in_the_log_of_its_host(self, api_client):
        (ld1,) = self.entries(api_client, 1)
        assert ld1["EntryType"] == "CXL"
        assert ld1["CXLEntryType"] == "DynamicCapacity"
        assert ld1["Severity"] == "OK"
        assert ld1["Message"] == "Host 1: Add Capacity Response"
        assert ld1["EventTimestamp"] == "2025-11-13T02:13:25Z"
        assert ld1["Links"]["OriginOfCondition"]["@odata.id"] == f"{self.BASE}/1"

        (ld0,) = self.entries(api_client, 0)
        assert ld0["CXLEntryType"] == "DynamicCapacity"
        assert ld0["Message"] == "Host 0: Capacity Released"
        assert "EventTimestamp" not in ld0

    def test_the_log_service(self, api_client):
        status, service = api_client.get(f"{self.BASE}/1/DeviceLog")
        assert status == 200
        assert service["LogEntryType"] == "CXL"
        assert service["Overflow"] is False


# =============================================================================
# Hosts that see a region at different DPAs (mock/dcd-shared-perhost.json)
# =============================================================================

PH = "/redfish/v1/Chassis/cxl_perhost_mld0/PCIeDevices/cxl_perhost_mld0"
PH_REFERENCE = PH + "/Actions/PCIeDevice.CXLReferenceDCMemory"
TAG_C1 = "6f1a2c3e-0000-4000-8000-0000000000c1"     # hosts 0 and 1, offset 0 of region 0
TAG_C2 = "6f1a2c3e-0000-4000-8000-0000000000c2"     # hosts 0 and 1, host 1 pending
TAG_C3 = "6f1a2c3e-0000-4000-8000-0000000000c3"     # held by an FM reference only
TAG_C4 = "6f1a2c3e-0000-4000-8000-0000000000c4"     # offered to host 2, not listed


def ph_action(ld, name):
    return PH + f"/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.{name}"


def ph_extent(api_client, ext):
    return api_client.get(PH + "/MemoryExtents/" + ext)


def tag_view(api_client, device=PH):
    """PCIeDevice Oem/Cxl/DynamicCapacityTags, its Tags by tag"""
    status, data = api_client.get(device)
    assert status == 200
    view = data["Oem"]["Cxl"]["DynamicCapacityTags"]
    assert view["TagsKnown"] is True
    return {t["Tag"]: t for t in view["Tags"]}


def tag_extents(row):
    return sorted(e["@odata.id"].rsplit("/", 1)[1] for e in row["Extents"])


@pytest.mark.mock_topology("mock/dcd-shared-perhost.json")
class TestPerHostDPA:
    """"The DPA ranges exposed by the device to each host are independent of
    one another" (Section 9.13.3): the mock gives each host its own bases."""

    def test_region_base_per_host(self, api_client):
        bases = {}
        for ld in (0, 1, 2):
            status, data = api_client.get(PH + f"/CXLLogicalDevices/{ld}/MemoryRegions/0")
            assert status == 200
            bases[ld] = data["RegionBaseOffsetMiB"]
        assert bases == {0: 8192, 1: 16384, 2: 0}

    def test_shared_tag_different_offsets(self, api_client):
        """One shared capacity, at a different DPA on each host."""
        status, h0 = ph_extent(api_client, "ext_0_200000000")
        assert status == 200
        status, h1 = ph_extent(api_client, "ext_1_400000000")
        assert status == 200
        assert h0["Tag"] == h1["Tag"] == TAG_C1
        assert h0["SequenceNumber"] == h1["SequenceNumber"] == 0
        assert h0["ExtentOffsetMiB"] == 8192
        assert h1["ExtentOffsetMiB"] == 16384
        assert h1["Links"]["MemoryRegions"] == [
            {"@odata.id": PH + "/CXLLogicalDevices/1/MemoryRegions/0"}]
        # host 1 has nothing at host 0's DPA
        status, _ = ph_extent(api_client, "ext_1_200000000")
        assert status == 404

    def test_same_dpa_other_capacity(self, api_client):
        """Host 2's extent at host 0's DPA is its own, untagged, in its region 1."""
        status, data = ph_extent(api_client, "ext_2_200000000")
        assert status == 200
        assert data["ExtentOffsetMiB"] == 8192
        assert data["ExtentSizeMiB"] == 512
        assert "Tag" not in data                    # no tag: none said
        assert "FMReferencedExtent" not in data
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": PH + "/CXLLogicalDevices/2/MemoryRegions/1"}]
        status, data = ph_extent(api_client, "ext_0_200000000")
        assert data["Tag"] == TAG_C1
        assert data["ExtentSizeMiB"] == 1024

    def test_pending_is_per_host(self, api_client):
        status, data = api_client.get(PH + "/CXLLogicalDevices/1/MemoryRegions/0")
        pending = {e["ExtentLink"]["@odata.id"]: e["Pending"] for e in data["MemoryExtents"]}
        assert pending[PH + "/MemoryExtents/ext_1_440000000"] is True
        assert pending[PH + "/MemoryExtents/ext_1_400000000"] is False
        status, data = ph_extent(api_client, "ext_1_440000000")
        assert data["Status"]["State"] == "Starting"
        status, data = ph_extent(api_client, "ext_0_240000000")
        assert data["Tag"] == TAG_C2
        assert data["Status"]["State"] == "Enabled"

    def test_tags_entry_fm_reference(self, api_client):
        """A "tags" entry of the file gives the 5608h FM Holds Reference, and
        keeps giving it after the mock's list is built again."""
        for ext in ("ext_0_240000000", "ext_1_440000000"):
            assert ph_extent(api_client, ext)[1]["FMReferencedExtent"] is True
        assert ph_extent(api_client, "ext_0_200000000")[1]["FMReferencedExtent"] is False
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityAdd"), {
            "Policy": "Free", "RegionNumber": 2, "Length": 256, "Tag": TAG_NEW})
        assert status == 200
        assert ph_extent(api_client, "ext_0_240000000")[1]["FMReferencedExtent"] is True

    def test_enable_shared_access_creates_host_extent(self, api_client):
        """Host 2 gets the capacity at its own base plus the offset."""
        add = ph_action(2, "DynamicCapacityAdd")
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_C1})
        assert status == 200
        status, data = ph_extent(api_client, "ext_2_0")
        assert status == 200
        assert data["Tag"] == TAG_C1
        assert data["SequenceNumber"] == 0
        assert data["ExtentSizeMiB"] == 1024
        assert data["ExtentOffsetMiB"] == 0
        assert data["Links"]["MemoryRegions"] == [
            {"@odata.id": PH + "/CXLLogicalDevices/2/MemoryRegions/0"}]
        # "currently accessible by the same LD"
        status, _, _ = api_client.post(add, {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_C1})
        assert status == 400

    def test_enable_shared_access_other_region_400(self, api_client):
        """The holders have it in region 0: region 2, Sharable too, is a
        restriction of the mock's device ("device-imposed restrictions",
        Section 7.6.7.6.5); region 1 is not Sharable."""
        add = ph_action(2, "DynamicCapacityAdd")
        for region in (2, 1):
            status, _, _ = api_client.post(add, {
                "Policy": "EnableSharedAccess", "RegionNumber": region, "Tag": TAG_C1})
            assert status == 400, region
        assert not [e for e in extent_ids(api_client, PH) if e.startswith("ext_2_")
                    and e != "ext_2_200000000"]

    def test_enable_shared_access_unknown_tag_404(self, api_client):
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityAdd"), {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_NEW})
        assert status == 404

    def test_enable_shared_access_of_fm_held_capacity(self, api_client):
        """Capacity only an FM reference keeps is given to a host again, and
        kept again when that host releases it."""
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityAdd"), {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_C3})
        assert status == 200
        status, data = ph_extent(api_client, "ext_2_80000000")    # 0 + 2048 MiB
        assert status == 200
        assert data["Tag"] == TAG_C3
        assert data["FMReferencedExtent"] is True
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityRelease"),
                                       release_body("TagBased", Tag=TAG_C3, Length=512))
        assert status == 200
        assert "ext_2_80000000" not in extent_ids(api_client, PH)
        # the reference still has it: a host can be given it once more
        status, _, _ = api_client.post(ph_action(0, "DynamicCapacityAdd"), {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_C3})
        assert status == 200
        assert ph_extent(api_client, "ext_0_280000000")[0] == 200   # 8192 + 2048 MiB

    def test_allocation_skips_every_hosts_capacity(self, api_client):
        """Free capacity is free on every host: host 2's add goes past what
        hosts 0 and 1 and the FM reference have (offsets 0-1536 and 2048-2560)."""
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityAdd"), {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 512, "Tag": TAG_NEW})
        assert status == 200
        assert "ext_2_60000000" in extent_ids(api_client, PH)       # 0 + 1536 MiB
        status, _, _ = api_client.post(ph_action(0, "DynamicCapacityAdd"), {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW})
        assert status == 200
        assert "ext_0_2a0000000" in extent_ids(api_client, PH)      # 8192 + 2560 MiB

    def test_fm_referenced_orphan(self, api_client):
        """No host's list has the capacity of TAG_C3: it is no MemoryExtent,
        and it is allocated until the reference goes."""
        for ext in extent_ids(api_client, PH):
            assert ph_extent(api_client, ext)[1].get("Tag") != TAG_C3
        # the device's tag list has it, held by the FM and by no LD
        row = tag_view(api_client)[TAG_C3]
        assert row["FMHoldsReference"] is True
        assert row["AcceptedLDs"] == [] and row["PendingLDs"] == []
        assert row["Extents"] == []
        status, _ = api_client.patch(PH + "/CXLLogicalDevices/2/MemoryRegions/0",
                                     {"BlockSizeMiB": 256})
        assert status == 409
        status, _, _ = api_client.post(PH_REFERENCE, {"Tag": TAG_C3, "FMReference": False})
        assert status == 200
        # the capacity is free again, and the tag is gone
        status, _, _ = api_client.post(ph_action(0, "DynamicCapacityAdd"), {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 1024, "Tag": TAG_NEW})
        assert status == 200
        assert "ext_0_260000000" in extent_ids(api_client, PH)      # 8192 + 1536 MiB
        assert TAG_C3 not in tag_view(api_client)
        status, _, _ = api_client.post(PH_REFERENCE, {"Tag": TAG_C3, "FMReference": True})
        assert status == 400

    def test_release_from_one_host(self, api_client):
        status, _, _ = api_client.post(ph_action(1, "DynamicCapacityRelease"),
                                       release_body("TagBased", Tag=TAG_C1, Length=1024))
        assert status == 200
        ids = extent_ids(api_client, PH)
        assert "ext_1_400000000" not in ids
        assert "ext_0_200000000" in ids
        assert "ext_2_200000000" in ids

    def test_tag_view(self, api_client):
        """The 5608h list: who has accepted each tag, who has not answered,
        whether the FM holds it, and every host's extents with it."""
        status, data = api_client.get(PH)
        view = data["Oem"]["Cxl"]["DynamicCapacityTags"]
        assert "@odata.type" not in view and "@odata.type" not in data["Oem"]
        assert isinstance(view["Generation"], int)
        tags = tag_view(api_client)
        assert sorted(tags) == [TAG_C1, TAG_C2, TAG_C3, TAG_C4]
        assert tags[TAG_C1]["AcceptedLDs"] == [0, 1]
        assert tags[TAG_C1]["PendingLDs"] == []
        assert tags[TAG_C1]["FMHoldsReference"] is False
        assert tag_extents(tags[TAG_C1]) == ["ext_0_200000000", "ext_1_400000000"]
        assert tags[TAG_C2]["AcceptedLDs"] == [0]
        assert tags[TAG_C2]["PendingLDs"] == [1]
        assert tags[TAG_C2]["FMHoldsReference"] is True
        assert tag_extents(tags[TAG_C2]) == ["ext_0_240000000", "ext_1_440000000"]

    def test_tag_view_pending_hosts(self, api_client):
        """An add LD 2 has not answered, which its 5603h does not list: no
        MemoryExtent has it, the tag list does."""
        row = tag_view(api_client)[TAG_C4]
        assert row["PendingLDs"] == [2]
        assert row["AcceptedLDs"] == []
        assert row["FMHoldsReference"] is False
        assert row["Extents"] == []
        for ext in extent_ids(api_client, PH):
            assert ph_extent(api_client, ext)[1].get("Tag") != TAG_C4

    def test_tag_view_follows_shared_access(self, api_client):
        status, _, _ = api_client.post(ph_action(2, "DynamicCapacityAdd"), {
            "Policy": "EnableSharedAccess", "RegionNumber": 0, "Tag": TAG_C1})
        assert status == 200
        row = tag_view(api_client)[TAG_C1]
        assert row["AcceptedLDs"] == [0, 1, 2]
        assert tag_extents(row) == ["ext_0_200000000", "ext_1_400000000", "ext_2_0"]

    def test_delete_shared_releases_one_host(self, api_client):
        """DELETE of an extent of a sharable tag removes its host's access to
        the whole of the tag, Tag-based (Section 7.6.7.6.6): every extent of
        that host with the tag goes, and no other host's."""
        status, _, _ = api_client.post(ph_action(1, "DynamicCapacityAdd"), {
            "Policy": "Contiguous", "RegionNumber": 0, "Length": 256, "Tag": TAG_C1})
        assert status == 200
        # host 1 has TAG_C1 in two extents now: 16384 MiB and 16384 + 1536 MiB
        status, data = ph_extent(api_client, "ext_1_460000000")
        assert status == 200 and data["SequenceNumber"] == 1
        fm_ref = ph_extent(api_client, "ext_0_200000000")[1]["FMReferencedExtent"]

        assert api_client.delete(PH + "/MemoryExtents/ext_1_400000000") == 204
        ids = extent_ids(api_client, PH)
        assert "ext_1_400000000" not in ids
        assert "ext_1_460000000" not in ids
        status, data = ph_extent(api_client, "ext_0_200000000")
        assert status == 200
        assert data["Tag"] == TAG_C1 and data["ExtentSizeMiB"] == 1024
        assert data["FMReferencedExtent"] == fm_ref
        # host 1's other tag is untouched
        assert "ext_1_440000000" in ids
        # the tag is still the device's, held by LD 0 only
        row = tag_view(api_client)[TAG_C1]
        assert row["FMHoldsReference"] == fm_ref
        assert row["AcceptedLDs"] == [0] and row["PendingLDs"] == []
        assert tag_extents(row) == ["ext_0_200000000"]

    def test_delete_nonshared_prescriptive(self, api_client):
        """Any other extent is released by itself, Prescriptive: of a tag in
        a region that is not Sharable, the host's other extents stay."""
        add = ph_action(2, "DynamicCapacityAdd")
        for _ in range(2):
            status, _, _ = api_client.post(add, {
                "Policy": "Contiguous", "RegionNumber": 1, "Length": 256, "Tag": TAG_NEW})
            assert status == 200
        ids = extent_ids(api_client, PH)
        assert "ext_2_220000000" in ids and "ext_2_230000000" in ids

        assert api_client.delete(PH + "/MemoryExtents/ext_2_220000000") == 204
        ids = extent_ids(api_client, PH)
        assert "ext_2_220000000" not in ids
        assert "ext_2_230000000" in ids
        # an extent with no tag
        assert api_client.delete(PH + "/MemoryExtents/ext_2_200000000") == 204
        ids = extent_ids(api_client, PH)
        assert "ext_2_200000000" not in ids
        assert "ext_2_230000000" in ids
        assert "ext_0_200000000" in ids

    def test_patch_region_is_device_wide(self, api_client):
        """Set DC Region Configuration names a region, not a host: its Block
        Size is "the region's" for every host (Table 7-69)."""
        # only host 2 has an extent in region 1, and it is allocated for all
        status, _ = api_client.patch(PH + "/CXLLogicalDevices/0/MemoryRegions/1",
                                     {"BlockSizeMiB": 256})
        assert status == 409
        status, _ = api_client.patch(PH + "/CXLLogicalDevices/0/MemoryRegions/2",
                                     {"BlockSizeMiB": 512})
        assert status == 200
        for ld in (0, 1, 2):
            status, data = api_client.get(PH + f"/CXLLogicalDevices/{ld}/MemoryRegions/2")
            assert data["BlockSizeMiB"] == 512, ld


PR = "/redfish/v1/Chassis/cxl_prel_mld0/PCIeDevices/cxl_prel_mld0"
TAG_D1 = "6f1a2c3e-0000-4000-8000-0000000000d1"


@pytest.mark.mock_topology("mock/dcd-prescriptive-release.json")
class TestDeleteWithoutTagBased:
    """A device that does no Tag-based release is sent a Prescriptive list
    of every extent its host has of the tag (cxl-fmd's choice): the same
    capacity, and no portion of it."""

    def test_tag_based_not_supported(self, api_client):
        status, data = api_client.get(PR)
        assert data["CXLDevice"]["DynamicCapacity"]["ReleaseCapacityPoliciesSupported"] == [
            "Prescriptive"]
        rel = PR + "/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease"
        status, _, _ = api_client.post(rel, release_body("TagBased", Tag=TAG_D1, Length=1536))
        assert status == 400

    def test_delete_shared_releases_one_host(self, api_client):
        assert extent_ids(api_client, PR) == [
            "ext_0_200000000", "ext_0_240000000", "ext_1_200000000"]
        assert api_client.delete(PR + "/MemoryExtents/ext_0_240000000") == 204
        assert extent_ids(api_client, PR) == ["ext_1_200000000"]
        status, data = api_client.get(PR + "/MemoryExtents/ext_1_200000000")
        assert data["Tag"] == TAG_D1 and data["ExtentSizeMiB"] == 1024
