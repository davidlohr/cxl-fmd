# SPDX-License-Identifier: LGPL-2.1
"""
CXL event records, as Redfish log entries.

mock/event-logs.json has records in the logs of a switch and of two devices,
as Get Event Records would have returned them. "A LogEntry resource is used
to capture CXL event records. The CXLEntryType property shall contain the
specific CXL entry type, and the EntryType property shall contain CXL."
(DSP0288 Table 1, Comment 1.) The records of a device are in the DeviceLog
of the CXL logical device they are about; those of a switch, in a
LogService of its Chassis (see src/event_log.c for why there).
"""

import pytest

pytestmark = pytest.mark.mock_topology("mock/event-logs.json")

SWITCH_LOG = "/redfish/v1/Chassis/cxl_switch_0/LogServices/EventLog"


def ld_log(dev, ld):
    return f"/redfish/v1/Chassis/{dev}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog"


def entries(api_client, service):
    status, data = api_client.get(service + "/Entries")
    assert status == 200
    # LogEntryCollection Members are auto-expanded: whole entries
    assert all("EntryType" in m for m in data["Members"])
    return data["Members"]


def test_switch_log_is_found_from_its_chassis(api_client):
    status, chassis = api_client.get("/redfish/v1/Chassis/cxl_switch_0")
    assert status == 200
    status, services = api_client.get(chassis["LogServices"]["@odata.id"])
    assert status == 200
    assert services["Members"] == [{"@odata.id": SWITCH_LOG}]
    status, service = api_client.get(SWITCH_LOG)
    assert status == 200
    assert service["LogEntryType"] == "CXL"
    assert service["ServiceEnabled"] is True
    assert service["OverWritePolicy"] == "WrapsWhenFull"


def test_switch_records(api_client):
    """Tables 7-79, 7-80 and 7-81, and a kind of record that is not decoded,
    in order, each in the log it was read from."""
    got = [(e["CXLEntryType"], e["Severity"], e["Message"],
            e.get("Links", {}).get("OriginOfCondition", {}).get("@odata.id"))
           for e in entries(api_client, SWITCH_LOG)]
    assert got == [
        ("Informational", "OK", "Physical port 4: link state change",
         "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp4"),
        ("Warning", "Warning", "vPPB 4 of VCS 1: binding change",
         "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/1/VPPBs/4"),
        ("Warning", "Warning", "MLD port 5: correctable error message received",
         "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp5"),
        ("Failure", "Critical", "Event record 12345678-9abc-def0-1234-56789abcdef0",
         None),
    ]
    for e in entries(api_client, SWITCH_LOG):
        assert e["EntryType"] == "CXL"


def test_event_timestamp_is_the_records(api_client):
    first = entries(api_client, SWITCH_LOG)[0]
    # 1763000000000000000 ns since 1970
    assert first["EventTimestamp"] == "2025-11-13T02:13:20Z"
    # "If the device does not have a valid timestamp, return 0": none given
    assert "EventTimestamp" not in entries(api_client, SWITCH_LOG)[2]


def test_sld_records_are_on_its_ld0(api_client):
    status, ld = api_client.get(
        "/redfish/v1/Chassis/dev0/PCIeDevices/dev0/CXLLogicalDevices/0")
    assert status == 200
    assert ld["Log"] == {"@odata.id": ld_log("dev0", 0)}
    status, service = api_client.get(ld_log("dev0", 0))
    assert status == 200
    # DSP0288: the sum of the sizes of the four logs, 16 + 8 + 4 + 4
    assert service["MaxNumberOfRecords"] == 32
    assert [e["CXLEntryType"] for e in entries(api_client, ld_log("dev0", 0))] == [
        "Informational", "Fatal"]


def test_mld_records_go_to_the_ld_they_are_about(api_client):
    """A record with the LD-ID Valid Flag set is about that LD; one without
    is "applicable to the entire device", so to every LD."""
    ld2 = [e["Message"] for e in entries(api_client, ld_log("mld0", 2))]
    ld0 = [e["Message"] for e in entries(api_client, ld_log("mld0", 0))]
    assert ld2 == ["General Media Event Record", "Memory Module Event Record"]
    assert ld0 == ["Memory Module Event Record"]


def test_overflow(api_client):
    """The Failure log of mld0 overflowed: the service says so, and the
    last record read from that log carries the overflow."""
    status, service = api_client.get(ld_log("mld0", 0))
    assert service["Overflow"] is True
    last = entries(api_client, ld_log("mld0", 0))[-1]
    assert last["OverflowErrorCount"] == 3
    assert last["FirstOverflowTimestamp"] == "2025-11-13T02:13:23Z"
    assert last["LastOverflowTimestamp"] == "2025-11-13T02:13:24Z"
    status, service = api_client.get(ld_log("dev0", 0))
    assert service["Overflow"] is False


def test_entry_by_id(api_client):
    for service in (SWITCH_LOG, ld_log("mld0", 2)):
        for e in entries(api_client, service):
            status, data = api_client.get(e["@odata.id"])
            assert status == 200
            assert data == e
    assert api_client.get(SWITCH_LOG + "/Entries/999")[0] == 404
    assert api_client.get(ld_log("mld0", 9))[0] == 404


def test_clear_log(api_client):
    """ClearLog drops what cxl-fmd keeps; the records were cleared from the
    component when they were read."""
    status, _, _ = api_client.post(SWITCH_LOG + "/Actions/LogService.ClearLog", {})
    assert status == 204
    assert entries(api_client, SWITCH_LOG) == []
    status, service = api_client.get(SWITCH_LOG)
    assert service["Overflow"] is False
    # the devices' logs are their own
    assert len(entries(api_client, ld_log("dev0", 0))) == 2
    status, _, _ = api_client.post(ld_log("mld0", 0) + "/Actions/LogService.ClearLog", {})
    assert status == 204
    assert entries(api_client, ld_log("mld0", 0)) == []
    assert len(entries(api_client, ld_log("mld0", 2))) == 2
