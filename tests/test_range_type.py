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
