# SPDX-License-Identifier: LGPL-2.1
"""
Commands that are not sent as MCTP messages unless the operator allows it
(review F15).

CXL r4.0 Table 8-308 marks Set Partition Info, Scan Media and the security
commands "O4" for MCTP. Note 4: "Systems capable of management from
Mailbox registers and an MCTP-based CCI shall ensure that these commands
are not issued as MCTP messages while a device's mailboxes are
operational." cxl-fmd sent them whenever a client asked. Now a request that
would send one is 409 with nothing changed, and the action is not
advertised, unless cxl-fmd was started with --allow-mctp-restricted. A
mock's devices are taken as on MCTP (cxl-fmd's choice).

Each topology is mock/single-sld.json with all of its capacity
partitionable.
"""

import copy
import json
from pathlib import Path

import pytest
import requests

from test_health import daemon  # noqa: F401  (the fixture)

ROOT = Path(__file__).resolve().parent.parent
DEV = "cxl_mem0"
MEMORY = f"/redfish/v1/Chassis/{DEV}/Memory/{DEV}"
CHUNKS = f"/redfish/v1/Chassis/{DEV}/MemoryDomains/{DEV}_domain0/MemoryChunks"
FLAG = "--allow-mctp-restricted"

ACTIONS = {
    "ScanMedia": {"PhysicalAddress": "0x1000", "Length": 4096},
    "SetPassphrase": {"Passphrase": "a", "NewPassphrase": "b"},
    "SetMasterPassphrase": {"Passphrase": "a", "NewPassphrase": "b"},
    "DisablePassphrase": {"Passphrase": "a"},
    "DisableMasterPassphrase": {"Passphrase": "a"},
    "FreezeSecurityState": {},
    "SecureEraseUnit": {"PassphraseType": "User", "Passphrase": "a"},
}


def partitionable(volatile=True):
    """all of the capacity partitionable, all volatile now (or persistent)"""
    topology = json.loads((ROOT / "mock" / "single-sld.json").read_text())
    dev = topology["devices"][0]
    assert dev["id"] == DEV
    dev["identify_memory_device"].update({"volatile_capacity_mb": 0,
                                          "persistent_capacity_mb": 0,
                                          "partition_align_mb": 256})
    active = 16384 if volatile else 0
    dev["partition_info"] = {"active_volatile_mb": active,
                             "active_persistent_mb": 16384 - active,
                             "next_volatile_mb": 0, "next_persistent_mb": 0}
    return copy.deepcopy(topology)


def get(base, uri):
    response = requests.get(base + uri, timeout=30)
    assert response.status_code == 200, uri
    return response.json()


def refused(response):
    assert response.status_code == 409, response.text
    message = response.json()["error"]["message"]
    assert "Table 8-308" in message and FLAG in message, message


def chunk_ids(base):
    return sorted(m["@odata.id"] for m in get(base, CHUNKS)["Members"])


def free_chunks(base):
    for uri in chunk_ids(base):
        assert requests.delete(base + uri, timeout=30).status_code == 204


def test_refused_without_the_flag(daemon):
    base = daemon(partitionable(volatile=False))
    memory = get(base, MEMORY)
    assert "Actions" not in memory
    assert "SecurityStates" in memory
    for name, body in ACTIONS.items():
        refused(requests.post(f"{base}{MEMORY}/Actions/Memory.{name}", json=body,
                              timeout=30))
    refused(requests.patch(base + MEMORY, json={"CXL": {"StagedVolatileSizeMiB": 8192}},
                           timeout=30))
    assert get(base, MEMORY) == memory

    # a Volatile chunk changes the partition; a PMEM one does not
    free_chunks(base)  # the one found at discovery, PMEM
    refused(requests.post(base + CHUNKS, json={"MemoryChunkSizeMiB": 1024,
                                               "AddressRangeType": "Volatile"},
                          timeout=30))
    assert chunk_ids(base) == []
    response = requests.post(base + CHUNKS, json={"MemoryChunkSizeMiB": 1024,
                                                  "AddressRangeType": "PMEM"},
                             timeout=30)
    assert response.status_code == 201, response.text


def test_volatile_chunk_delete_refused_without_the_flag(daemon):
    """The chunk found at discovery is Volatile: its DELETE would change
    the partition."""
    base = daemon(partitionable())
    before = chunk_ids(base)
    assert len(before) == 1
    assert get(base, before[0])["AddressRangeType"] == "Volatile"
    refused(requests.delete(base + before[0], timeout=30))
    assert chunk_ids(base) == before


def test_carried_out_with_the_flag(daemon):
    base = daemon(partitionable(), FLAG)
    actions = get(base, MEMORY)["Actions"]
    assert set(actions) == {f"#Memory.{name}" for name in ACTIONS}
    for name, body in ACTIONS.items():
        target = actions[f"#Memory.{name}"]["target"]
        assert target == f"{MEMORY}/Actions/Memory.{name}"
        response = requests.post(base + target, json=body, timeout=30)
        assert response.status_code == 200, (name, response.text)
    response = requests.patch(base + MEMORY, json={"CXL": {"StagedVolatileSizeMiB": 8192}},
                              timeout=30)
    assert response.status_code == 200, response.text
    free_chunks(base)  # Volatile: allowed too
    response = requests.post(base + CHUNKS, json={"MemoryChunkSizeMiB": 1024,
                                                  "AddressRangeType": "Volatile"},
                             timeout=30)
    assert response.status_code == 201, response.text


@pytest.mark.parametrize("args", [(), (FLAG,)])
def test_action_on_no_memory(daemon, args):
    """An action of a Memory that is not there is refused, in a mock too
    (it was 200 there)."""
    base = daemon(partitionable(), *args)
    response = requests.post(f"{base}/redfish/v1/Chassis/{DEV}/Memory/nonesuch"
                             "/Actions/Memory.FreezeSecurityState", json={}, timeout=30)
    assert response.status_code == 400
