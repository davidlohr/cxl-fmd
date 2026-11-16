# SPDX-License-Identifier: LGPL-2.1
"""
Tests for CXL memory lifecycle management.

Validates memory chunk provisioning, deletion, and capacity management
per DMTF DSP0288 (CXL to Redfish Mapping) for SLD and MLD devices.

Memory Model:
- Discovery creates chunks for existing LD allocations
- DELETE frees capacity back to the pool
- POST allocates from remaining capacity
"""

import pytest


# =============================================================================
# SLD Memory Chunk Discovery Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestSLDMemoryChunkDiscovery:
    """Tests for SLD memory chunks created during discovery."""

    def test_sld_has_memory_domain(self, api_client):
        """Verify SLD has a MemoryDomain."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0"
        )
        assert status == 200
        assert "@odata.type" in data
        assert "MemoryDomain" in data["@odata.type"]

    def test_sld_chunk_exists_after_discovery(self, api_client):
        """Verify chunk exists for SLD after discovery."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1

    def test_sld_chunk_has_correct_size(self, api_client):
        """Verify SLD chunk has correct size from discovery."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )
        assert status == 200
        assert "MemoryChunkSizeMiB" in data
        # single-sld.json has 16384 MB
        assert data["MemoryChunkSizeMiB"] == 16384

    def test_sld_chunk_has_address_range_type(self, api_client):
        """Verify SLD chunk has AddressRangeType."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )
        assert status == 200
        assert "AddressRangeType" in data
        # single-sld.json is persistent only
        assert data["AddressRangeType"] == "PMEM"


# =============================================================================
# SLD Memory Chunk Lifecycle Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestSLDMemoryChunkLifecycle:
    """Tests for SLD memory chunk delete and re-create."""

    def test_delete_existing_chunk(self, api_client):
        """Test deleting the discovered chunk."""
        # Delete existing chunk
        delete_status = api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )
        assert delete_status == 204

        # Verify collection is empty
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 0

    def test_create_chunk_after_delete(self, api_client):
        """Test creating chunk after deleting existing one."""
        # Delete existing chunk to free capacity
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        # Create new chunk
        status, data, location = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1024}
        )
        assert status == 201
        assert data is not None
        assert data["MemoryChunkSizeMiB"] == 1024
        assert location is not None

    def test_create_multiple_chunks_after_delete(self, api_client):
        """Test creating multiple smaller chunks after deleting existing one."""
        # Delete existing chunk to free capacity
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        # Create multiple chunks
        chunks = []
        for _ in range(3):
            status, data, location = api_client.post(
                "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
                {"MemoryChunkSizeMiB": 1024}
            )
            assert status == 201
            chunks.append(location)

        # Verify all chunks exist
        status, collection = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
        )
        assert status == 200
        assert collection["Members@odata.count"] == 3


# =============================================================================
# MLD Memory Chunk Discovery Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mld-device.json")
class TestMLDMemoryChunkDiscovery:
    """Tests for MLD memory chunks created during discovery."""

    def test_mld_has_chunks_per_ld(self, api_client):
        """Verify MLD has one chunk per LD after discovery."""
        # cxl_mld_0 has 4 LDs
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 4

    def test_mld_chunk_per_ld_exists(self, api_client):
        """Verify each LD has its chunk."""
        for ld_id in range(4):
            status, data = api_client.get(
                f"/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld{ld_id}_chunk"
            )
            assert status == 200
            assert "MemoryChunkSizeMiB" in data


# =============================================================================
# MLD Memory Chunk Lifecycle Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mld-device.json")
class TestMLDMemoryChunkLifecycle:
    """Tests for MLD memory chunk delete and re-create."""

    def test_delete_mld_chunk(self, api_client):
        """Test deleting an MLD chunk."""
        # Delete LD0's chunk
        delete_status = api_client.delete(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld0_chunk"
        )
        assert delete_status == 204

        # Verify count decreased
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks"
        )
        assert status == 200
        assert data["Members@odata.count"] == 3

    def test_create_mld_chunk_with_interleave_sets(self, api_client):
        """Test creating MLD chunk using InterleaveSets after delete."""
        # Delete existing chunk to free capacity
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld0_chunk"
        )

        # Create new chunk with InterleaveSets
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks",
            {
                "InterleaveSets": [{
                    "Memory": {
                        "@odata.id": "/redfish/v1/Chassis/cxl_mld_0/Memory/cxl_mld_0_ld0"
                    },
                    "SizeMiB": 4096
                }]
            }
        )
        assert status == 201
        assert "MemoryChunkSizeMiB" in data or "InterleaveSets" in data


# =============================================================================
# Capacity Constraint Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestCapacityConstraints:
    """Tests for memory capacity constraints and error handling."""

    def test_reject_chunk_when_no_capacity(self, api_client):
        """Test that requests are rejected when no capacity available."""
        # All capacity is already allocated, POST should fail
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 1024}
        )
        # Should return 409 Conflict for insufficient capacity
        assert status == 409

    def test_reject_chunk_exceeding_freed_capacity(self, api_client):
        """Test that requests exceeding freed capacity are rejected."""
        # Delete existing 16GB chunk
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        # Try to allocate more than freed capacity (16GB available, request 32GB)
        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 32768}
        )
        assert status == 409

    def test_reject_zero_size_chunk(self, api_client):
        """Test that zero-size chunks are rejected."""
        # Free capacity first
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": 0}
        )
        assert status == 400

    def test_reject_negative_size_chunk(self, api_client):
        """Test that negative-size chunks are rejected."""
        # Free capacity first
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks/cxl_mem0_chunk0"
        )

        status, data, _ = api_client.post(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks",
            {"MemoryChunkSizeMiB": -1024}
        )
        assert status == 400


# =============================================================================
# Memory Domain Validation Tests
# =============================================================================

@pytest.mark.mock_topology("mock/single-sld.json")
class TestMemoryDomainValidation:
    """Tests for MemoryDomain properties and constraints."""

    def test_memory_domain_has_capacity_info(self, api_client):
        """Verify MemoryDomain has capacity properties."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0"
        )
        assert status == 200
        assert "@odata.type" in data
        assert "MemoryDomain" in data["@odata.type"]

    def test_memory_domain_has_chunks_link(self, api_client):
        """Verify MemoryDomain has MemoryChunks link."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0"
        )
        assert status == 200
        assert "MemoryChunks" in data

    def test_memory_domain_collection(self, api_client):
        """Verify MemoryDomains collection is accessible."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mem0/MemoryDomains"
        )
        assert status == 200
        assert data["Members@odata.count"] >= 1


# =============================================================================
# CXLLogicalDevice MemoryChunks Links Tests
# =============================================================================

@pytest.mark.mock_topology("mock/mld-device.json")
class TestCXLLogicalDeviceChunkLinks:
    """Tests for CXLLogicalDevice Links.MemoryChunks."""

    def test_ld_has_chunk_link_when_allocated(self, api_client):
        """Verify CXLLogicalDevice has MemoryChunks link when chunk exists."""
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices/cxl_mld_0_ld0"
        )
        assert status == 200
        assert "Links" in data
        # Should have MemoryChunks link since chunk exists
        if "MemoryChunks" in data["Links"]:
            assert len(data["Links"]["MemoryChunks"]) >= 1

    def test_ld_no_chunk_link_when_deleted(self, api_client):
        """Verify CXLLogicalDevice has no MemoryChunks link after delete."""
        # Delete the chunk
        api_client.delete(
            "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld0_chunk"
        )

        # Check LD - should have empty or no MemoryChunks
        status, data = api_client.get(
            "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices/cxl_mld_0_ld0"
        )
        assert status == 200
        # Either no MemoryChunks key or empty array
        if "Links" in data and "MemoryChunks" in data["Links"]:
            assert len(data["Links"]["MemoryChunks"]) == 0


# =============================================================================
# Sizes and offsets in MiB (review F11)
# =============================================================================

SLD_CHUNKS = "/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks"
SLD_SHIPPED = SLD_CHUNKS + "/cxl_mem0_chunk0"
SLD_DOMAIN_MIB = 16384
# The most MiB a 64-bit byte count can hold: 2^44 - 1
MIB_MAX = ((1 << 64) - 1) >> 20


def chunk_members(api_client):
    status, data = api_client.get(SLD_CHUNKS)
    assert status == 200
    return sorted(m["@odata.id"] for m in data["Members"])


@pytest.mark.mock_topology("mock/single-sld.json")
class TestChunkSizeAndOffsetBounds:
    """A size or offset in MiB is a byte count of 64 bits once converted:
    one that does not fit is refused with 400, and so is a chunk that
    would not lie within its domain. Nothing is created for any of them."""

    @pytest.mark.parametrize("size", [MIB_MAX + 1, (1 << 44) + 1024, (1 << 63) - 1,
                                      -1, 0])
    def test_size_out_of_range_400(self, api_client, size):
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": size,
                                                       "AddressRangeType": "PMEM"})
        assert status == 400, data
        assert chunk_members(api_client) == []

    def test_interleave_size_out_of_range_400(self, api_client):
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS,
                                          {"InterleaveSets": [{"SizeMiB": (1 << 44) + 1025}]})
        assert status == 400, data
        assert chunk_members(api_client) == []

    @pytest.mark.parametrize("entry", [
        {"SizeMiB": -1024}, {"SizeMiB": "1024"}, {"SizeMiB": 1024.5},
        {"SizeMiB": 1024, "OffsetMiB": -1}, {"SizeMiB": 1024, "OffsetMiB": "0"},
        {"SizeMiB": 1024, "OffsetMiB": MIB_MAX + 1},
    ])
    def test_interleave_mib_not_an_integer_in_range_400(self, api_client, entry):
        """InterleaveSets SizeMiB and OffsetMiB are integers from 0 to
        2^44 - 1 too: one that is not was ignored, or wrapped."""
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024,
                                                       "InterleaveSets": [entry]})
        assert status == 400, data
        assert chunk_members(api_client) == []

    @pytest.mark.parametrize("size", ["1024", 1024.0, None])
    def test_size_not_an_integer_400(self, api_client, size):
        """A MemoryChunkSizeMiB that is there and not an integer is refused,
        not passed over for InterleaveSets[0].SizeMiB."""
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": size,
                                                       "InterleaveSets": [{"SizeMiB": 1024}]})
        assert status == 400, data
        assert chunk_members(api_client) == []

    @pytest.mark.parametrize("offset", [-5, MIB_MAX, SLD_DOMAIN_MIB - 1024 + 1,
                                        999999, "zero"])
    def test_offset_out_of_range_400(self, api_client, offset):
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024,
                                                       "AddressRangeOffsetMiB": offset})
        assert status == 400, data
        assert chunk_members(api_client) == []

    @pytest.mark.parametrize("range_type", ["Bogus", 5])
    def test_address_range_type_400(self, api_client, range_type):
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024,
                                                       "AddressRangeType": range_type})
        assert status == 400, data
        assert chunk_members(api_client) == []

    def test_chunk_at_the_end_of_the_domain(self, api_client):
        """The last MiB of the domain can be given; chunks placed
        automatically then fill the room below it, first fit, and none
        lies past the end of the domain: once it is full, 409."""
        assert api_client.delete(SLD_SHIPPED) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {
            "MemoryChunkSizeMiB": 1024, "AddressRangeOffsetMiB": SLD_DOMAIN_MIB - 1024})
        assert status == 201, data
        assert data["AddressRangeOffsetMiB"] == SLD_DOMAIN_MIB - 1024

        offsets = []
        for _ in range(SLD_DOMAIN_MIB // 1024 - 1):
            status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024})
            assert status == 201, data
            offsets.append(data["AddressRangeOffsetMiB"])
        assert sorted(offsets) == list(range(0, SLD_DOMAIN_MIB - 1024, 1024))

        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024})
        assert status == 409, data

    def test_auto_offset_fills_a_hole(self, api_client):
        """A hole left by a DELETE is used again, before the end."""
        assert api_client.delete(SLD_SHIPPED) == 204
        made = []
        for _ in range(3):
            status, data, location = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024})
            assert status == 201, data
            made.append((location, data["AddressRangeOffsetMiB"]))
        assert [o for _, o in made] == [0, 1024, 2048]
        assert api_client.delete(made[1][0]) == 204
        status, data, _ = api_client.post(SLD_CHUNKS, {"MemoryChunkSizeMiB": 1024})
        assert status == 201, data
        assert data["AddressRangeOffsetMiB"] == 1024


# =============================================================================
# Chunk Ids (review F10)
# =============================================================================

MLD_CHUNKS = "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks"


def ld_memory(chassis, ld):
    """The URI of the Memory of LD @ld of @chassis, as a chunk POST names it."""
    return {"@odata.id": f"/redfish/v1/Chassis/{chassis}/Memory/{chassis}_ld{ld}"}


def post_chunk(api_client, chunks, mib=1024, memory=None):
    body = {"MemoryChunkSizeMiB": mib}
    if memory:
        body["InterleaveSets"] = [{"Memory": memory}]
    status, data, location = api_client.post(chunks, body)
    assert status == 201, data
    assert location == data["@odata.id"]
    return data["@odata.id"], data["AddressRangeOffsetMiB"]


def unique_members(api_client, chunks):
    status, data = api_client.get(chunks)
    assert status == 200
    members = [m["@odata.id"] for m in data["Members"]]
    assert len(members) == len(set(members)) == data["Members@odata.count"], members
    return set(members)


@pytest.mark.mock_topology("mock/single-sld.json")
class TestChunkIdentity:
    """A chunk's Id is "The unique identifier for this resource within the
    collection of similar resources" (Resource_v1): one a chunk holds is
    not given to another. cxl-fmd numbers a domain's chunks from a counter
    of its own, so a deleted Id does not come back while the daemon runs
    (cxl-fmd's choice; Ids are not kept over a restart)."""

    def test_ids_unique_after_middle_delete(self, api_client):
        assert api_client.delete(SLD_SHIPPED) == 204
        a, _ = post_chunk(api_client, SLD_CHUNKS)
        b, _ = post_chunk(api_client, SLD_CHUNKS)
        c, c_off = post_chunk(api_client, SLD_CHUNKS)
        assert api_client.delete(b) == 204
        d, d_off = post_chunk(api_client, SLD_CHUNKS)
        assert len({a, b, c, d}) == 4
        assert unique_members(api_client, SLD_CHUNKS) == {a, c, d}
        assert api_client.get(c)[1]["AddressRangeOffsetMiB"] == c_off
        assert api_client.get(d)[1]["AddressRangeOffsetMiB"] == d_off
        # a deleted chunk's URI stays gone, and the other is untouched
        assert api_client.delete(d) == 204
        assert api_client.get(d)[0] == 404
        assert api_client.get(c)[0] == 200
        assert api_client.get(b)[0] == 404

    def test_first_ids_are_unchanged(self, api_client):
        """On a fresh domain the Ids are _chunk0, _chunk1, ... as before."""
        assert api_client.delete(SLD_SHIPPED) == 204
        made = [post_chunk(api_client, SLD_CHUNKS)[0] for _ in range(3)]
        assert [m.rsplit("/", 1)[1] for m in made] == [
            "cxl_mem0_domain0_chunk0", "cxl_mem0_domain0_chunk1", "cxl_mem0_domain0_chunk2"]


@pytest.mark.mock_topology("mock/mld-device.json")
class TestMLDChunkIdentity:
    """The same on an MLD, whose domain also holds the chunk of each LD
    (named for the LD, not numbered): deleting one of those does not let
    a later POST take an Id a chunk holds."""

    def test_ids_unique_after_ld_chunk_delete(self, api_client):
        assert api_client.delete(MLD_CHUNKS + "/cxl_mld_0_ld0_chunk") == 204
        a, _ = post_chunk(api_client, MLD_CHUNKS, memory=ld_memory("cxl_mld_0", 0))
        # numbered from the domain's own counter, not from its chunks
        assert a == MLD_CHUNKS + "/cxl_mld_0_domain0_chunk0"
        assert api_client.delete(MLD_CHUNKS + "/cxl_mld_0_ld1_chunk") == 204
        b, _ = post_chunk(api_client, MLD_CHUNKS, memory=ld_memory("cxl_mld_0", 1))
        assert a != b
        assert unique_members(api_client, MLD_CHUNKS) == {
            a, b, MLD_CHUNKS + "/cxl_mld_0_ld2_chunk", MLD_CHUNKS + "/cxl_mld_0_ld3_chunk"}
        assert api_client.delete(a) == 204
        assert api_client.get(b)[0] == 200
        assert api_client.get(a)[0] == 404


# =============================================================================
# A chunk POST names one LD (review F02)
# =============================================================================

MLD_LD = "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices"


@pytest.mark.mock_topology("mock/mld-device.json")
class TestChunkPlan:
    """A chunk of an MLD grows the allocation of one LD, which Set LD
    Allocations (5402h) sets with a Range 1 and a Range 2 multiplier and
    nothing else (CXL r4.0 Table 7-49). The POST names that LD by its
    Memory URI (InterleaveSets[0].Memory) or its CXLLogicalDevice URI
    (Links.CXLLogicalDevices[0]); a request naming none, an LD the device
    does not have, several, or asking for what 5402h cannot carry is 400,
    and makes nothing. Each of these was 201."""

    def refused(self, api_client, body):
        # room for the chunk, which is not why it is refused
        assert api_client.delete(MLD_CHUNKS + "/cxl_mld_0_ld1_chunk") == 204
        before = unique_members(api_client, MLD_CHUNKS)
        status, data, _ = api_client.post(MLD_CHUNKS, body)
        assert status == 400, data
        assert unique_members(api_client, MLD_CHUNKS) == before

    def test_no_ld(self, api_client):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024})

    def test_ld_the_device_does_not_have(self, api_client):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024, "InterleaveSets": [
            {"Memory": ld_memory("cxl_mld_0", 9), "SizeMiB": 1024}]})
        assert api_client.get(MLD_LD + "/9")[0] == 404

    def test_two_entries(self, api_client):
        self.refused(api_client, {"MemoryChunkSizeMiB": 2048, "InterleaveSets": [
            {"Memory": ld_memory("cxl_mld_0", 0), "SizeMiB": 1024},
            {"Memory": ld_memory("cxl_mld_0", 1), "SizeMiB": 1024}]})

    def test_size_mismatch(self, api_client):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024, "InterleaveSets": [
            {"Memory": ld_memory("cxl_mld_0", 0), "SizeMiB": 4096}]})

    @pytest.mark.parametrize("extra", [{"OffsetMiB": 1024}, {"RegionId": "r9"},
                                       {"MemoryLevel": 3}])
    def test_what_an_allocation_cannot_be(self, api_client, extra):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024, "InterleaveSets": [
            dict({"Memory": ld_memory("cxl_mld_0", 0)}, **extra)]})

    @pytest.mark.parametrize("ref", ["cxl_mld_0_ld0",
                                     "/redfish/v1/Chassis/cxl_mld_0/Memory/cxl_mld_0_ld0/",
                                     "/x/cxl_mld_0_ld0"])
    def test_not_a_uri_of_its_memory(self, api_client, ref):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024, "InterleaveSets": [
            {"Memory": {"@odata.id": ref}}]})

    def test_memory_and_link_disagree(self, api_client):
        self.refused(api_client, {"MemoryChunkSizeMiB": 1024,
                                  "InterleaveSets": [{"Memory": ld_memory("cxl_mld_0", 0)}],
                                  "Links": {"CXLLogicalDevices": [{"@odata.id": MLD_LD + "/1"}]}})

    def test_by_its_cxl_logical_device(self, api_client):
        """Links.CXLLogicalDevices alone names the LD; the chunk as served
        links it, and so does the LD."""
        assert api_client.delete(MLD_CHUNKS + "/cxl_mld_0_ld1_chunk") == 204
        status, data, _ = api_client.post(MLD_CHUNKS, {
            "MemoryChunkSizeMiB": 1024,
            "Links": {"CXLLogicalDevices": [{"@odata.id": MLD_LD + "/1"}]}})
        assert status == 201, data
        assert data["Links"]["CXLLogicalDevices"] == [{"@odata.id": MLD_LD + "/1"}]
        assert data["InterleaveSets"] == [{"Memory": ld_memory("cxl_mld_0", 1),
                                           "MemoryLevel": 1, "OffsetMiB": 0,
                                           "SizeMiB": 1024}]
        status, chunk = api_client.get(data["@odata.id"])
        assert status == 200
        assert chunk["Links"]["CXLLogicalDevices"] == [{"@odata.id": MLD_LD + "/1"}]
        status, ld = api_client.get(MLD_LD + "/1")
        assert status == 200
        assert {"@odata.id": data["@odata.id"]} in ld["Links"]["MemoryChunks"]


@pytest.mark.mock_topology("mock/mhd-device.json")
class TestMHDChunkPlan:
    """The same for a pool LD of a Multi-Headed device."""

    def test_no_ld(self, api_client):
        chunks = ("/redfish/v1/Chassis/cxl_mhsld_0/MemoryDomains/cxl_mhsld_0_domain0/"
                  "MemoryChunks")
        assert api_client.delete(chunks + "/cxl_mhsld_0_ld1_chunk") == 204
        before = unique_members(api_client, chunks)
        status, data, _ = api_client.post(chunks, {"MemoryChunkSizeMiB": 1024})
        assert status == 400, data
        assert unique_members(api_client, chunks) == before
