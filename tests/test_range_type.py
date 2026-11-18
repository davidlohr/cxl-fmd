# SPDX-License-Identifier: LGPL-2.1
"""
AddressRangeType of a MemoryChunk, the same in every view (review F17
claim 5, and the POST/GET disagreement found with F15).

A chunk was "PMEM" unless its device had Volatile Only capacity and no
Persistent Only capacity: a device with both, or with partitionable
capacity, or whose capacities were not known, was PMEM. The view under
Chassis read Get Partition Info instead, so one chunk had two types, and
a POST with no AddressRangeType answered PMEM on a device with no
persistent capacity, whose GET then said Volatile.

Now a chunk has the type it is made with: at discovery, that of the
active partition ("Volatile" or "PMEM" when all of the memory is one, null
when it is both or not known); on a POST, the one asked for, if the device
can have it, else the device's. cxl-fmd's rules.

Each topology is mock/single-sld.json with one change.
"""

import copy
import json
from pathlib import Path

import pytest
import requests

from test_health import daemon  # noqa: F401  (the fixture)

ROOT = Path(__file__).resolve().parent.parent
SLD = json.loads((ROOT / "mock" / "single-sld.json").read_text())
DEV = "cxl_mem0"
CHUNKS = f"/redfish/v1/Chassis/{DEV}/MemoryDomains/{DEV}_domain0/MemoryChunks"
CHUNK0 = f"{CHUNKS}/{DEV}_chunk0"
HOST_CHUNK0 = f"/redfish/v1/Systems/local/MemoryDomains/local_direct_{DEV}/MemoryChunks/{DEV}_chunk0"


def sld(volatile_mb, persistent_mb, partition=None):
    topology = copy.deepcopy(SLD)
    dev = topology["devices"][0]
    dev["identify_memory_device"]["volatile_capacity_mb"] = volatile_mb
    dev["identify_memory_device"]["persistent_capacity_mb"] = persistent_mb
    if partition:
        dev["partition_info"] = partition
    return topology


def range_type(base, uri):
    response = requests.get(base + uri, timeout=30)
    assert response.status_code == 200, uri
    return response.json()["AddressRangeType"]


def post(base, body):
    return requests.post(base + CHUNKS, json=body, timeout=30)


def free_chunk0(base):
    """the discovered chunk has the whole domain: make room"""
    assert requests.delete(base + CHUNK0, timeout=30).status_code == 204


def test_volatile_only(daemon):
    base = daemon(sld(16384, 0))
    assert range_type(base, CHUNK0) == "Volatile"
    assert range_type(base, HOST_CHUNK0) == "Volatile"
    free_chunk0(base)
    # no type asked for: the device's, in the answer and on a GET
    response = post(base, {"MemoryChunkSizeMiB": 1024})
    assert response.status_code == 201
    assert response.json()["AddressRangeType"] == "Volatile"
    assert range_type(base, response.headers["Location"]) == "Volatile"
    # it has no persistent memory, and none can be partitioned
    response = post(base, {"MemoryChunkSizeMiB": 1024, "AddressRangeType": "PMEM"})
    assert response.status_code == 400


def test_mixed(daemon):
    """8 GiB of each: the one chunk of all of it is neither"""
    base = daemon(sld(8192, 8192))
    assert range_type(base, CHUNK0) is None
    assert range_type(base, HOST_CHUNK0) is None
    free_chunk0(base)
    response = post(base, {"MemoryChunkSizeMiB": 1024})
    assert response.status_code == 400
    assert "AddressRangeType" in response.json()["error"]["message"]
    response = post(base, {"MemoryChunkSizeMiB": 1024, "AddressRangeType": "PMEM"})
    assert response.status_code == 201
    assert response.json()["AddressRangeType"] == "PMEM"
    assert range_type(base, response.headers["Location"]) == "PMEM"


def test_partitionable_active_volatile(daemon):
    """No Volatile Only or Persistent Only capacity, all of it partitioned
    volatile now: Volatile in both views (it was Volatile under Chassis and
    PMEM under Systems)."""
    base = daemon(sld(0, 0, {"active_volatile_mb": 16384, "active_persistent_mb": 0,
                             "next_volatile_mb": 0, "next_persistent_mb": 0}))
    assert range_type(base, CHUNK0) == "Volatile"
    assert range_type(base, HOST_CHUNK0) == "Volatile"


def test_persistent_only(daemon):
    base = daemon(sld(0, 16384))
    assert range_type(base, CHUNK0) == "PMEM"
    assert range_type(base, HOST_CHUNK0) == "PMEM"


@pytest.mark.mock_topology("mock/mld-device.json")
class TestLogicalDevice:
    DOMAIN = "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0"
    LD0 = {"InterleaveSets": [{"Memory": {"@odata.id":
                                          "/redfish/v1/Chassis/cxl_mld_0/Memory/cxl_mld_0_ld0"}}]}

    def test_ld_chunk_is_its_type(self, api_client):
        """The LDs of the file are all persistent: PMEM, and a Volatile
        chunk is refused before anything is sent (the domain is full, so
        any other POST is 409)."""
        assert range_type(api_client.base_url,
                          self.DOMAIN + "/MemoryChunks/cxl_mld_0_ld0_chunk") == "PMEM"
        base = api_client.base_url + self.DOMAIN + "/MemoryChunks"
        response = requests.post(base, json={"MemoryChunkSizeMiB": 1024,
                                             "AddressRangeType": "Volatile", **self.LD0},
                                 timeout=30)
        assert response.status_code == 400
        assert "AddressRangeType" in response.json()["error"]["message"]


BOOTS_FIRST = json.loads((ROOT / "mock" / "fm-boots-first.json").read_text())
SW_CHUNKS = "/redfish/v1/Chassis/sld_0/MemoryDomains/sld_0_domain0/MemoryChunks"


def bind_volatile_chunk(base):
    """sld_0 is unbound: make room, make a 1 GiB Volatile chunk and bind
    sld_0 to switch_0_host0 with it"""
    response = requests.get(base + SW_CHUNKS, timeout=30)
    assert response.status_code == 200
    for member in response.json()["Members"]:
        assert requests.delete(base + member["@odata.id"], timeout=30).status_code == 204
    response = requests.post(base + SW_CHUNKS, json={
        "MemoryChunkSizeMiB": 1024, "AddressRangeType": "Volatile"}, timeout=30)
    assert response.status_code == 201
    response = requests.post(base + "/redfish/v1/Fabrics/CXL/Connections", json={
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/switch_0_host0"}],
            "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/sld_0"}]},
        "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": response.headers["Location"]}}]},
        timeout=30)
    assert response.status_code == 201


def test_bound_volatile_only_sld_takes_a_volatile_chunk(daemon):
    """sld_0 of fm-boots-first.json is all Volatile Only capacity, with no
    Partition Alignment: a Volatile chunk sends no Set Partition Info (which
    "shall fail with an Unsupported error if there is no partitionable
    capacity", CXL r4.0 Section 8.2.10.9.2.2), so a binding does not refuse
    it, as it does not a PMEM one (it was 409)."""
    base = daemon(BOOTS_FIRST)
    bind_volatile_chunk(base)
    response = requests.post(base + SW_CHUNKS, json={
        "MemoryChunkSizeMiB": 1024, "AddressRangeType": "Volatile"}, timeout=30)
    assert response.status_code == 201
    assert response.json()["AddressRangeType"] == "Volatile"


def test_bound_partitionable_sld_refuses_a_volatile_chunk(daemon):
    """The same SLD with all of its capacity partitionable, volatile now: a
    Volatile chunk would change the partition, and is 409 while bound."""
    topology = copy.deepcopy(BOOTS_FIRST)
    dev = topology["devices"][0]
    assert dev["id"] == "sld_0"
    dev["identify_memory_device"].update({"volatile_capacity_mb": 0,
                                          "persistent_capacity_mb": 0,
                                          "partition_align_mb": 256})
    dev["partition_info"] = {"active_volatile_mb": 16384, "active_persistent_mb": 0,
                             "next_volatile_mb": 0, "next_persistent_mb": 0}
    # its Volatile chunks change the partition: Set Partition Info, which
    # is not sent to a mock's devices unless allowed (review F15)
    base = daemon(topology, "--allow-mctp-restricted")
    bind_volatile_chunk(base)
    response = requests.post(base + SW_CHUNKS, json={
        "MemoryChunkSizeMiB": 1024, "AddressRangeType": "Volatile"}, timeout=30)
    assert response.status_code == 409
    assert "A binding exposes the logical device" in response.json()["error"]["message"]
