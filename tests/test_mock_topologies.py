# SPDX-License-Identifier: LGPL-2.1
"""
Mock topologies are valid CXL fabrics, and invalid ones are refused.

`cxl-fmd --mock FILE --check` loads a topology, checks it against the rules
of the CXL specification (src/validate.c) and exits: 0 if it could be a real
fabric, non-zero with the broken rule on stderr if not.

Two things are tested here:

 - Every topology shipped in mock/ passes. A test that runs on a topology
   that could not exist tests nothing.
 - Each rule the checker enforces rejects a topology that breaks it. Each
   case below takes a valid topology, breaks exactly one rule, and expects
   the refusal to name it. Without these, a checker that accepted
   everything would pass.
"""

import copy
import json
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOPOLOGIES = sorted(ROOT.glob("mock/*.json")) + sorted(ROOT.glob("mock/unit/*.json"))


def check(executable, path):
    proc = subprocess.run([str(executable), "--mock", str(path), "--check"],
                          capture_output=True, text=True, timeout=30)
    return proc.returncode, proc.stdout + proc.stderr


@pytest.mark.parametrize("path", TOPOLOGIES, ids=lambda p: p.name)
def test_shipped_topology_is_valid(cxl_fmd_executable, path):
    rc, output = check(cxl_fmd_executable, path)
    assert rc == 0, output


def test_there_are_topologies():
    assert len(TOPOLOGIES) >= 20


# ---------------------------------------------------------------------------
# One broken rule each
# ---------------------------------------------------------------------------

def vppb(vppb_id, status, port=None, ld=None):
    out = {"vppb_id": vppb_id, "binding_status": status}
    if port is not None:
        out["bound_port_id"] = port
    if ld is not None:
        out["bound_ld_id"] = ld
    return out


def port(t, number):
    return next(p for p in t["switches"][0]["ports"] if p["port_number"] == number)


def device(t, dev_id):
    return next(d for d in t["devices"] if d["id"] == dev_id)


# multi-host-vcs.json: VCS 0 has port 2 (dev0), port 3 (dev1) and LDs 0-1 of
# the MLD on port 5; VCS 1 has port 4 (dev2) and LDs 2-3; its vPPB 4 is unbound.
def set_spare_vppb(t, binding):
    t["switches"][0]["vcs"][1]["vppbs"][4] = binding


def port_bound_twice(t):
    set_spare_vppb(t, vppb(4, "bound_physical_port", port=2))


def ld_bound_twice(t):
    set_spare_vppb(t, vppb(4, "bound_ld", port=5, ld=0))


def mld_port_bound_whole(t):
    set_spare_vppb(t, vppb(4, "bound_physical_port", port=5))


def ld_of_a_port_without_mld(t):
    t["switches"][0]["vcs"][0]["vppbs"][1] = vppb(1, "bound_ld", port=2, ld=0)


def bound_to_a_missing_port(t):
    set_spare_vppb(t, vppb(4, "bound_physical_port", port=99))


def bound_to_a_missing_ld(t):
    set_spare_vppb(t, vppb(4, "bound_ld", port=5, ld=9))


def bound_to_another_vcs_usp(t):
    set_spare_vppb(t, vppb(4, "bound_physical_port", port=0))


def usp_is_not_upstream(t):
    t["switches"][0]["vcs"][1]["upstream_port_id"] = 2


def two_vcs_one_usp(t):
    t["switches"][0]["vcs"][1]["upstream_port_id"] = 0


def duplicate_vppb_id(t):
    t["switches"][0]["vcs"][1]["vppbs"][4]["vppb_id"] = 3


def device_on_a_port_that_is_down(t):
    port(t, 2)["link_status"] = "down"


def port_type_is_not_the_device(t):
    port(t, 2)["conn_dev_type"] = "mld"


def duplicate_serial(t):
    device(t, "dev1")["serial_number"] = device(t, "dev0")["serial_number"]


def serial_too_long(t):
    device(t, "dev0")["serial_number"] = "0x4445563030303030ff"


def vendor_is_not_a_number(t):
    device(t, "dev0")["vendor_id"] = "Intel"


def device_on_a_missing_switch(t):
    device(t, "dev0")["parent_switch"] = "no_such_switch"


def duplicate_device_id(t):
    device(t, "dev1")["id"] = "dev0"


def misspelt_binding_status(t):
    # used to load as unbound
    t["switches"][0]["vcs"][0]["vppbs"][1]["binding_status"] = "bound_port"


def unknown_port_device_type(t):
    port(t, 2)["conn_dev_type"] = "ssd"


# single-sld.json
def capacity_not_256mb(t):
    ident = t["devices"][0]["identify_memory_device"]
    ident["total_capacity_mb"] = 1000
    ident["persistent_capacity_mb"] = 1000


def capacity_parts_exceed_total(t):
    ident = t["devices"][0]["identify_memory_device"]
    ident["volatile_capacity_mb"] = ident["total_capacity_mb"]


# mld-device.json
def mld_with_17_lds(t):
    dev = t["devices"][0]
    dev["ld_count"] = 17
    dev["logical_devices"] = [{"ld_id": i, "memory_size_mb": 1024} for i in range(17)]


def mld_ld_not_a_granule(t):
    # granularity 4 x 256 MB = 1 GB
    t["devices"][0]["logical_devices"][0]["memory_size_mb"] = 32768 + 256


def mld_bad_granularity(t):
    t["devices"][0]["memory_granularity"] = 3


def mld_duplicate_ld_id(t):
    t["devices"][0]["logical_devices"][1]["ld_id"] = 0


# mhd-device.json
def mhd(t):
    return device(t, "cxl_mhd_partial")


def head_without_lds(t):
    mhd(t)["heads"][1]["lds"] = []


def mhd_with_one_head(t):
    dev = mhd(t)
    dev["num_heads"] = 1
    del dev["heads"][1]


def head_presents_wrongly(t):
    # one LD mapped: the head presents an SLD
    mhd(t)["heads"][0]["presentation"] = "MLD"


# unit/unit-switch-mhsld.json: an MH-SLD with head 0 on port 2 and head 1 on
# port 3 of a switch. A head is a port of the device.
def whole_mhd_on_one_port(t):
    dev = t["devices"][0]
    dev["parent_switch"], dev["parent_port"] = "switch_0", 2
    for head in dev["heads"]:
        del head["parent_switch"], head["parent_port"]


def two_heads_on_one_port(t):
    t["devices"][0]["heads"][1]["parent_port"] = 2


def head_on_a_missing_port(t):
    t["devices"][0]["heads"][1]["parent_port"] = 9


def mld_head_on_a_switch(t):
    dev = t["devices"][0]
    dev["heads"][0]["presentation"] = "MLD"
    dev["heads"][0]["lds"] = [0, 1]
    dev["heads"][1]["lds"] = []


def head_port_is_not_an_sld_port(t):
    port(t, 3)["conn_dev_type"] = "mld"


def head_on_a_port_that_is_down(t):
    port(t, 3)["link_status"] = "down"


# hot-removal.json
def unplug_twice(t):
    t["unplug"] = ["sld_0", "sld_0"]


def unplug_a_switch(t):
    t["unplug"] = ["switch_0"]


def unplug_nothing_there(t):
    t["unplug"] = ["sld_9"]


# event-logs.json
def records(t, dev):
    return device(t, dev)["event_records"]


def event_in_no_log(t):
    records(t, "dev0")[0]["log"] = "debug"


def event_of_no_kind(t):
    records(t, "dev0")[0]["kind"] = "thermal"


def event_ld_on_an_sld(t):
    records(t, "dev0")[0]["ld_id"] = 0


def event_for_a_missing_ld(t):
    records(t, "mld0")[0]["ld_id"] = 9


def event_data_too_long(t):
    records(t, "dev0")[0]["data"] = "00" * 0x51


# dcd-device.json
def dcd(t):
    return t["devices"][0]["dcd"]


def extent_outside_its_region(t):
    dcd(t)["extents"][0]["start_mb"] = 1024


def extent_not_block_aligned(t):
    dcd(t)["regions"][0]["block_size_mb"] = 1024
    dcd(t)["extents"][0]["len_mb"] = 4096 + 256


def region_base_not_256mb(t):
    dcd(t)["extents"] = []      # so that the base is all that is wrong
    dcd(t)["regions"][0]["base_mb"] = 16384 + 100


def extents_overlap(t):
    extents = dcd(t)["extents"]
    extents.append(dict(extents[0], start_mb=extents[0]["start_mb"] + 256))


# switch-dcd.json: region 0 of the MLD is Sharable, region 1 is not
def shared_extent_in_a_region_that_is_not_sharable(t):
    extent = t["devices"][0]["dcd"]["extents"][1]      # in region 1, host 1's
    extent["hosts"] = [0, 1]


BROKEN = [
    # (base topology, what to break, part of the message that names the rule)
    ("multi-host-vcs.json", port_bound_twice, "is bound to vPPB"),
    ("multi-host-vcs.json", ld_bound_twice, "(same LD) is bound to vPPB"),
    ("multi-host-vcs.json", mld_port_bound_whole, "it is bound by LD"),
    ("multi-host-vcs.json", ld_of_a_port_without_mld, "which has no MLD"),
    ("multi-host-vcs.json", bound_to_a_missing_port, "which the switch does not have"),
    ("multi-host-vcs.json", bound_to_a_missing_ld, "which has no such LD"),
    ("multi-host-vcs.json", bound_to_another_vcs_usp, "the upstream port of another VCS"),
    ("multi-host-vcs.json", usp_is_not_upstream, "is not an upstream port"),
    ("multi-host-vcs.json", two_vcs_one_usp, "have the same upstream port"),
    ("multi-host-vcs.json", duplicate_vppb_id, "has two vPPBs with ID"),
    ("multi-host-vcs.json", device_on_a_port_that_is_down, "its link is not up"),
    ("multi-host-vcs.json", port_type_is_not_the_device, "and Connected Device Type"),
    ("multi-host-vcs.json", duplicate_serial, "have the same serial number"),
    ("multi-host-vcs.json", serial_too_long, "is not a number that fits"),
    ("multi-host-vcs.json", vendor_is_not_a_number, "is not a number that fits"),
    ("multi-host-vcs.json", device_on_a_missing_switch, "which the topology does not have"),
    ("multi-host-vcs.json", duplicate_device_id, "two components with ID"),
    ("multi-host-vcs.json", misspelt_binding_status, "is not unbound, busy"),
    ("multi-host-vcs.json", unknown_port_device_type, "is not none, pcie"),
    ("single-sld.json", capacity_not_256mb, "is not a multiple of 256 MB"),
    ("single-sld.json", capacity_parts_exceed_total, "is more than the total capacity"),
    ("mld-device.json", mld_with_17_lds, "more than 16 LDs in an MLD"),
    ("mld-device.json", mld_ld_not_a_granule, "is not a multiple of the memory granularity"),
    ("mld-device.json", mld_bad_granularity, "is not 256 MB, 512 MB or 1 GB"),
    ("mld-device.json", mld_duplicate_ld_id, "two LDs with LD-ID"),
    ("mhd-device.json", head_without_lds, "a head has 1 to 16"),
    ("mhd-device.json", mhd_with_one_head, "has more than one"),
    ("mhd-device.json", head_presents_wrongly, "does not present as an SLD"),
    ("unit/unit-switch-mhsld.json", whole_mhd_on_one_port, "is not on one switch port"),
    ("unit/unit-switch-mhsld.json", two_heads_on_one_port, "which already has a device"),
    ("unit/unit-switch-mhsld.json", head_on_a_missing_port, "which the topology does not have"),
    ("unit/unit-switch-mhsld.json", mld_head_on_a_switch, "presents an MLD on a switch port"),
    ("unit/unit-switch-mhsld.json", head_port_is_not_an_sld_port, "which presents an SLD"),
    ("unit/unit-switch-mhsld.json", head_on_a_port_that_is_down, "and its link is not up"),
    ("hot-removal.json", unplug_twice, "sld_0 is pulled out twice"),
    ("hot-removal.json", unplug_a_switch, "switch_0 is not an SLD or an MLD"),
    ("hot-removal.json", unplug_nothing_there, "sld_9 is not an SLD or an MLD"),
    ("event-logs.json", event_in_no_log, "log is not informational"),
    ("event-logs.json", event_of_no_kind, "no known kind"),
    ("event-logs.json", event_ld_on_an_sld, "ld_id is for an MLD"),
    ("event-logs.json", event_for_a_missing_ld, "no LD 9"),
    ("event-logs.json", event_data_too_long, "data is not up to"),
    ("dcd-device.json", extent_outside_its_region, "is outside region base"),
    ("dcd-device.json", extent_not_block_aligned, "is not aligned to the block size"),
    ("dcd-device.json", region_base_not_256mb, "is not aligned to 256 MB"),
    ("dcd-device.json", extents_overlap, "overlap"),
    ("switch-dcd.json", shared_extent_in_a_region_that_is_not_sharable,
     "is not Sharable"),
]


@pytest.mark.parametrize("base,breakage,message", BROKEN,
                         ids=[b[1].__name__ for b in BROKEN])
def test_broken_topology_is_refused(cxl_fmd_executable, tmp_path, base, breakage, message):
    topology = json.loads((ROOT / "mock" / base).read_text())
    broken = copy.deepcopy(topology)
    breakage(broken)
    assert broken != topology, "the case changes nothing"

    path = tmp_path / "broken.json"
    path.write_text(json.dumps(broken))

    rc, output = check(cxl_fmd_executable, path)
    assert rc != 0, f"accepted a topology with {breakage.__name__}\n{output}"
    assert message in output, output


def test_broken_topology_does_not_serve(cxl_fmd_executable, tmp_path):
    """Without --check, a topology that is not valid is still refused: the
    daemon exits instead of serving it."""
    broken = json.loads((ROOT / "mock" / "multi-host-vcs.json").read_text())
    port_bound_twice(broken)
    path = tmp_path / "broken.json"
    path.write_text(json.dumps(broken))

    proc = subprocess.run([str(cxl_fmd_executable), "--mock", str(path),
                           "--port", "65010"],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode != 0
    assert "is not a valid CXL topology" in proc.stdout + proc.stderr
