# Redfish API Reference

This document describes the CXL to Redfish mapping implemented by cxl-fmd, based on [DMTF DSP0288 v1.4.0](https://www.dmtf.org/dsp/DSP0288) (CXL to Redfish Mapping Specification).

DSP0288 v1.4.0 does not cover the FM-API DCD Management (56h) or MHD (55h) command sets. The DCD mapping cxl-fmd implements (Table 3) is in [DCD-FM-API.md](DCD-FM-API.md).

## Quick Start

Start cxl-fmd with a mock topology:

```bash
./build/cxl-fmd --mock mock/single-sld.json -v
```

Test basic endpoints:

```bash
# Service root
curl -s http://localhost:8080/redfish/v1 | jq

# List CXL devices
curl -s http://localhost:8080/redfish/v1/Chassis | jq

# Get device details
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_mem0 | jq

# Get logical devices
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices | jq

# Get memory domain
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0 | jq

# List memory chunks
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks | jq
```

For switch topologies:

```bash
./build/cxl-fmd --mock mock/switch-attached.json -v

# List switches
curl -s http://localhost:8080/redfish/v1/Fabrics/CXL/Switches | jq

# Get VCS configuration
curl -s http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/switch_0/VCSs/0 | jq

# List connections
curl -s http://localhost:8080/redfish/v1/Fabrics/CXL/Connections | jq
```

## Authentication

cxl-fmd does not currently implement authentication, nor TLS, and it listens on every interface: the maintainer has deferred all three (Known Gaps, item 13). All endpoints are accessible without credentials, and credentials that are sent are not checked. Only the `Sessions` collection, `/redfish/v1/SessionService/Sessions`, is served, linked from the service root's `Links/Sessions`; there is no `/redfish/v1/SessionService` resource, and no session is created or required.

For production deployments, place cxl-fmd behind a reverse proxy (e.g., nginx) that handles TLS and authentication, and keep its own port out of reach of anything else.

---

## Table 1 - CXL CCI to Redfish Mapping

DSP0288 1.4.0 Section 3, Table 1 — CXL CCI to Redfish mapping. The first
five columns are DSP0288's rows as it gives them, with its property names
even where DSP8010 has none by that name. The last column is cxl-fmd's:
whether it serves the mapping, and where it serves it under another name.

| CXL Command | CXL Field | Redfish Resource | Redfish Property | Comment | Implemented (cxl-fmd) |
|-------------|-----------|------------------|------------------|---------|-----------------------|
| Get Log Capabilities | Parameter Flags[3]: Persistent across cold reset | `LogService` | `Persistency` | See Comment 1. | No |
| Get Event Records | Flags[0]: Overflow | `LogService` | `Overflow` | See Comment 1. | Yes |
| Get Event Records | Overflow Error Count | `LogEntry` | `OverflowErrorCount` | See Comment 1. | Yes |
| Get Event Records | First Overflow Event Timestamp | `LogEntry` | `FirstOverflowTimestamp` | See Comment 1. | Yes |
| Get Event Records | Last Overflow Event Timestamp | `LogEntry` | `LastOverflowTimestamp` | See Comment 1. | Yes |
| Get FW Info | FW Slot Info | `PCIeDevice` | `StagedVersion` | | No |
| Get Timestamp | Timestamp | `PCIeDevice` | `CXLDevice/Timestamp` | | Yes |
| Set Timestamp | Timestamp | `PCIeDevice` | `CXLDevice/Timestamp` | | Yes |
| Identify Memory Device | Informational, Warning, Failure, and Fatal Event Log Size | `LogService` | `MaxNumberOfRecords` | See Comment 2. | Yes, on the `LogService` of a `CXLLogicalDevice` (see "Where cxl-fmd puts CXL event records") |
| Identify Memory Device | Poison List Maximum Media Error Records | `Memory` | `PoisonListMaxMediaErrorRecords` | | No |
| Identify Memory Device | Poison Handling Capabilities | `Memory` | `PoisonHandlingCapabilities` | | No. DSP8010 has no property of this name |
| Identify Memory Device | QoS Telemetry Capabilities[0]: Egress Port Congestion Supported | `PCIeDevice` | `CXLDevice/EgressPortCongestionSupported` | | Yes, as `CXLDevice/EgressPortCongestionSupport`: DSP8010 has no property named as DSP0288 names it |
| Identify Memory Device | QoS Telemetry Capabilities[1]: Temporary Throughput Reduction Supported | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionSupported` | | Yes |
| Identify Memory Device | LSA Size | `Memory` | `LabelStorageSizeBytes` | | No. DSP8010 has it as `CXL/LabelStorageSizeBytes` |
| Get SLD QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | | No |
| Set SLD QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | | No |
| Get Partition Info | Active Volatile Capacity | `Memory` | `VolatileSizeLimitMiB` | | Yes |
| Get Partition Info | Next Volatile Capacity | `Memory` | `StagedVolatileSizeMiB` | | Yes, as `CXL/StagedVolatileSizeMiB`, where DSP8010 has it |
| Get Partition Info | Next Persistent Capacity | `Memory` | `StagedPersistentSizeMiB` | | Yes, as `CXL/StagedNonVolatileSizeMiB`: DSP8010 has no `StagedPersistentSizeMiB` (see "Set Partition Info" below) |
| Set Partition Info | Next Volatile Capacity | `Memory` | `StagedVolatileSizeMiB` | | Yes, as `CXL/StagedVolatileSizeMiB` (see "Set Partition Info" below) |
| Set Partition Info | Next Persistent Capacity | `Memory` | `StagedPersistentSizeMiB` | | Yes, as `CXL/StagedNonVolatileSizeMiB` (see "Set Partition Info" below) |
| Get Health Info | Health Status | Many | `Status` | See Comment 3. | Yes: `Status/Health`, `Status/State`, and `Status/Conditions` for each bit set (see "Status from Get Health Info") |
| Get Health Info | Life Used | `MemoryMetrics` | `CapacityUtilizationPercent` | | No (deviation, see "Life Used" below) |
| Get Health Info | Corrected Volatile Error Count | `MemoryMetrics` | `CorrectedVolatileErrorCount` | | Yes |
| Get Health Info | Corrected Persistent Error Count | `MemoryMetrics` | `CorrectedPersistentErrorCount` | | Yes |
| Get Health Info | Additional Status[4]: Corrected Volatile Error Count | `Memory` | `Status/Conditions` | See Comment 4. | Yes, `CxlHealth.1.0.CorrectedVolatileErrorCount` (see "Status from Get Health Info") |
| Get Health Info | Additional Status[5]: Corrected Persistent Error Count | `Memory` | `Status/Conditions` | See Comment 4. | Yes, `CxlHealth.1.0.CorrectedPersistentErrorCount` (see "Status from Get Health Info") |
| Get Health Info | Dirty Shutdown Count | `MemoryMetrics` | `DirtyShutdownCount` | | Yes |
| Get Alert Configuration | Valid Alerts and Programmable Alerts | `MemoryMetrics` | `CXL/AlertCapabilities` | | No |
| Scan Media | N/A | `Memory` | `ScanMedia` action | | Yes |
| Get Security State | Security State | `Memory` | `SecurityStates` | | Yes |
| Set Passphrase | All | `CXLLogicalDevice` | `SetPassphrase` action | | Yes, when the LD's CEL lists it (see "Security actions" below) |
| Disable Passphrase | All | `CXLLogicalDevice` | `DisablePassphrase` action | | Yes, likewise |
| Freeze Security State | N/A | `CXLLogicalDevice` | `FreezeSecurityState` action | | Yes, likewise |
| Passphrase Secure Erase | All | `CXLLogicalDevice` | `PassphraseSecureErase` action | | Yes, likewise |
| Unlock | All | `CXLLogicalDevice` | `Unlock` action | | Yes, likewise |

### Comments for Table 1

**Comment 1:** A `LogEntry` resource is used to capture CXL event records. The `CXLEntryType` property shall contain the specific CXL entry type, and the `EntryType` property shall contain `CXL`. Overflow information is captured in properties of the `LogEntry` prior to the overflow condition.

**Comment 2:** A `Memory` resource can only contain a single `LogService`. This `LogService` resource shall contain the informational, warning, failure, and fatal event records as `LogEntry` resources. The `MaxNumberOfRecords` shall contain the sum of the informational, warning, failure, and fatal log sizes.

**Comment 3:** Redfish resources contain a `Status` property to show the health and state of a device. The `Conditions` property within the `Status` property is used to show why the `Health` property contains a value other than `OK`.

**Comment 4:** The `Conditions` property within the `Status` property of the `Memory` resource shall contain messages to indicate either the Corrected Volatile Error Count or Corrected Persistent Error Count flags are set.

### Security actions

DSP0288 1.4.0 Table 1 maps Set Passphrase, Disable Passphrase, Freeze
Security State, Passphrase Secure Erase and Unlock (CXL r4.0 Section
8.2.10.9.6) to the `SetPassphrase`, `DisablePassphrase`,
`FreezeSecurityState`, `PassphraseSecureErase` and `Unlock` actions of
`CXLLogicalDevice`; 1.3.0 moved the first four there from `Memory` and
added Unlock (DSP0288 Annex A). Get Security State stays on `Memory`
(`SecurityStates`). cxl-fmd serves them at
`/redfish/v1/Chassis/{id}/PCIeDevices/{id}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.{action}`,
on LD 0 of an SLD and on each LD of an MLD (through the tunnel to the LD),
with the parameters of `CXLLogicalDevice_v1`:

| Action | Parameters | Command and payload |
|--------|------------|---------------------|
| `SetPassphrase` | `PassphraseType`, `Passphrase`, `NewPassphrase` | Set Passphrase (4501h, Table 8-336) |
| `DisablePassphrase` | `PassphraseType`, `Passphrase` | Disable Passphrase (4502h, Table 8-337) |
| `Unlock` | `PassphraseType` (`User` only), `Passphrase` | Unlock (4503h, Table 8-338) |
| `FreezeSecurityState` | none (the body is not read) | Freeze Security State (4504h), no payload |
| `PassphraseSecureErase` | `PassphraseType`, `Passphrase` | Passphrase Secure Erase (4505h, Table 8-339) |

`PassphraseType` `Master` is Passphrase Type 00h and `User` 01h. Unlock
"supplies the user passphrase" (Section 8.2.10.9.6.4) and its payload has
no Passphrase Type, so `Master` is `400` there. A passphrase field is 20h
bytes: a `Passphrase` or `NewPassphrase` of more than 32 bytes (of UTF-8)
is `400`, not cut, and a shorter one is padded with zero bytes (cxl-fmd's
choices: CXL gives the field no encoding).

An action is in the LD's `Actions`, and carried out, only when

- the LD's Command Effects Log lists its command: an SLD's CEL, or the
  LD's own, read through the tunnel. One whose CEL could not be read is
  taken to support none of them (cxl-fmd's choice), and so is a mock
  device whose `cel_commands` does not list them; the `POST` is then
  `501`;
- the command may be sent: these commands are O4 for MCTP (see "Commands
  restricted on MCTP"), so without `--allow-mctp-restricted` they are
  served only on an SLD on the kernel's mailbox, and the `POST` is `409`
  elsewhere.

The pool LDs of a Multi-Headed device have none: cxl-fmd sends no Memory
Device command to one (`501`). What the device answers is mapped by
cxl-fmd's choice: Invalid Input, Incorrect Passphrase and Invalid Payload
Length `400`; Invalid Security State `409`; Unsupported and Unsupported
Mailbox or CCI `501`; Retry Required and Busy `503`; any other, or a
response that cannot be read, `500`. Success is `200` with no body.

The actions DSP0288 1.2.0 mapped to `Memory` (`SetPassphrase` and
`DisablePassphrase` for the user passphrase, `SetMasterPassphrase` and
`DisableMasterPassphrase` for the master one, `FreezeSecurityState`, and
`SecureEraseUnit` for Passphrase Secure Erase) are deprecated aliases of
these, on the `Memory` of the LD. They take the request they took
(`Passphrase` and `NewPassphrase`; `PassphraseType` and `Passphrase` for
`SecureEraseUnit`), are served and answered as the `CXLLogicalDevice`
action is, and will be removed. The deprecation is cxl-fmd's: the
`Memory` schema does not deprecate them. There is no `Memory` alias of
`Unlock`, and `Memory.UnlockUnit` is not served (`404`).

### Life Used

DSP0288 maps Life Used to `MemoryMetrics` `CapacityUtilizationPercent`,
which DSP8010 defines as "the memory capacity utilization as a
percentage", not the life of the media. cxl-fmd does not serve it.
Instead it serves `HealthData/PredictedMediaLifeLeftPercent` ("an
indicator of the percentage, `0` to `100`, of life remaining in the
media") as 100 - Life Used. That is cxl-fmd's choice. Life Used is "a
percentage value (0 to 100)" that "Returns FFh if not implemented" (CXL
r4.0 Table 8-315). When it is not 0 to 100, `HealthData` is left out
rather than reporting 0% life left.

### Set Partition Info

DSP0288 maps Next Persistent Capacity to `StagedPersistentSizeMiB`, which
the `Memory` schema does not have; cxl-fmd uses `CXL/StagedNonVolatileSizeMiB`
("Total device non-volatile memory capacity in MiB staged for next
activation") in its place. That is cxl-fmd's choice. Next Volatile
Capacity is served as `CXL/StagedVolatileSizeMiB`, where the schema has
`StagedVolatileSizeMiB`.

`StagedVolatileSizeMiB` and `StagedNonVolatileSizeMiB` are totals, as Get
Partition Info's Next Volatile and Next Persistent Capacity are. Each
"shall become the Active Volatile Capacity" (or "the Active Persistent
Capacity") "on the next reset", and Active Volatile Capacity "is the sum
of the device's Volatile Only capacity and the capacity that is
partitioned for volatile use", Active Persistent Capacity "the sum of the
device's Persistent Only capacity and the capacity that is partitioned
for persistent use" (CXL r4.0 Table 8-310). Set Partition
Info's Volatile Capacity is not: it is "The amount of partitionable
capacity that shall be allocated to volatile capacity, in multiples of
256 MB aligned to the partition alignment requirement reported in the
Identify Memory Device command" (Table 8-311), where "Partitionable
capacity is equal to (Total Capacity – Volatile Only Capacity –
Persistent Only Capacity)" (Section 8.2.10.9.2.2). A `PATCH` of either
property sends the requested total less the Volatile Only Capacity, and
is refused with 400, with nothing sent, when:

- the value is not a multiple of 256 MiB;
- it is less than the Volatile Only Capacity, or leaves less than the
  Persistent Only Capacity;
- the partitionable part is not a multiple of the Partition Alignment;
- the Partition Alignment is 0: "This command shall fail with an
  Unsupported error if there is no partitionable capacity (i.e., Identify
  Memory Device reports Partition Alignment as zero)".

The change is sent with Immediate cleared, so it "shall become the
"next" configuration, to become active on the next reset" (Table 8-311),
and it can destroy data: "Using this command to change the size of the
persistent capacity shall result in the loss of data stored" (Section
8.2.10.9.2.2). Set Partition Info is not sent as an MCTP message unless
cxl-fmd was started with `--allow-mctp-restricted`: without it, the
`PATCH` is `409`, but on an SLD on the kernel's mailbox (see "Commands
restricted on MCTP").

#### Staged chunks

A `"Volatile"` chunk `POST` on a directly attached SLD with partitionable
capacity sends Set Partition Info for the volatile capacity staged, or
else active, plus the chunk, with Immediate cleared as above. The chunk
is then of the next configuration, not of the active one, and until Get
Partition Info shows it active (cxl-fmd's choices):

- its `Status.State` is `StandbyOffline`, "a resource is temporarily
  unavailable but may become available through an external action"
  (`Resource_v1`), in the `201` and on a `GET`; its `AddressRangeType` is
  `Volatile`;
- it is in no Connection's `MemoryChunkInfo`, nor in a host's
  `MemoryDomains` and their `MemoryChunks`, and a Connection `POST` naming
  it is `409`;
- its range of the domain stays its own: no other chunk is given it;
- a `DELETE` of it sends Set Partition Info for the volatile capacity
  staged less the chunk, as for any `"Volatile"` chunk.

Each `--rescan` reads Get Partition Info of an SLD that has a staged
chunk, and of no other. While Next Volatile or Next Persistent Capacity
is not 0 ("If both this field and the Next Persistent Capacity field are
0, there is no pending change to the partitioning", Table 8-310), it
stays staged. With no change pending, a chunk whose Next Volatile
Capacity the Active Volatile Capacity has reached is `Enabled`, and in
the Connection; one it has not reached is removed, and its range is free.
A read that fails changes nothing. In a mock no Set Partition Info is
sent, nor is anything staged: a `"Volatile"` chunk is `Enabled` at once.
Such a `POST` and `DELETE` are `409`, in a mock too, unless the SLD is on
the kernel's mailbox or cxl-fmd was started with `--allow-mctp-restricted`
(see "Commands restricted on MCTP").

### Commands restricted on MCTP

CXL r4.0 Table 8-308 marks these Memory Device commands "O4" in its MCTP
column: Set Partition Info (4101h), Set LSA (4103h), Clear Poison (4302h),
Scan Media (4304h), Sanitize (4400h), Secure Erase (4401h), Set Passphrase
(4501h), Disable Passphrase (4502h), Unlock (4503h), Freeze Security State
(4504h), Passphrase Secure Erase (4505h), Security Send (4600h) and Set SLD
QoS Control (4701h). Note 4 of the table reads:

> Systems capable of management from Mailbox registers and an MCTP-based
> CCI shall ensure that these commands are not issued as MCTP messages
> while a device's mailboxes are operational.

No CCI command tells an FM whether a device's mailboxes are operational:
Mailbox Interfaces Ready ("When set, the device is ready to accept
commands through the mailbox register interfaces", Table 8-212) is a bit
of the Memory Device Status Register, which the host reads. So cxl-fmd
does not send any of these as an MCTP message unless the operator, who can
know that no host drives the devices, starts it with
`--allow-mctp-restricted` (cxl-fmd's choice, by the maintainer's
decision). Of them, cxl-fmd sends Set Partition Info, Scan Media and the
security commands. Without the option:

- a request that would send one of them is `409`, with nothing sent or
  changed: `PATCH` of a `Memory` (Set Partition Info), its `ScanMedia` and
  security actions, the security actions of a `CXLLogicalDevice` (see
  "Security actions"), a `"Volatile"` chunk `POST` on an SLD with
  partitionable capacity and the `DELETE` of such a chunk (see "Staged
  chunks"). The check comes after the resource is found (`404`, or `400`
  for an action of a `Memory` that is not there), before the body is read.
  A chunk `POST` that a Connection refuses is `409` for that first;
- the `Memory` and the `CXLLogicalDevice`s of such a device have none of
  these actions in `Actions` (no `Actions` at all if none is left).
  `SecurityStates` (Get Security State, 4500h, "O" for MCTP) is served.

Which routes count as MCTP messages is cxl-fmd's reading:

- only an SLD attached directly through the kernel's mailbox (`--ioctl`)
  is sent these commands without the option: they are not MCTP messages
  there;
- an SLD on an MCTP endpoint of its own is not;
- an LD of an MLD is reached with the Tunnel Management Command, and is not
  either, whatever the endpoint. A switch tunnels a command "to the
  FM-owned LD of the MLD on the specified port, using the transport defined
  in Section 7.6.3.1" (Section 7.6.7.3.1), which is the MCTP PCIe VDM
  binding (DSP0238); the specification does not say what a tunnel within an
  MLD is carried on, and cxl-fmd takes every tunnelled command as MCTP;
- a mock topology's devices are taken as on MCTP, cxl-fmd's default
  transport, so that a mock answers as such a device would. An SLD on a
  switch port is sent none of these commands on hardware (see "HTTP Method
  Mapping"), and is refused as before there.

With `--allow-mctp-restricted` the requests are carried out as before, and
cxl-fmd logs a warning when it starts: making sure that no host drives the
devices is the operator's.

### Where cxl-fmd puts CXL event records

Comment 2 puts the records of a memory device in the `LogService` of its
`Memory` resource. The schema (DSP8010 2026.2) allows that `LogService`, at the
URI segment `DeviceLog`, only under `/redfish/v1/Systems/{id}/Memory/{id}`;
cxl-fmd's `Memory` resources are under `Chassis`, where it allows none. It
does allow one under each `CXLLogicalDevice`, and this is cxl-fmd's mapping:

| Records of | `LogService` |
|------------|--------------|
| A memory device | `/redfish/v1/Chassis/{dev}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog`: LD 0 of an SLD; for an MLD, the LD a record names with its LD-ID Valid Flag, or every LD for a record about the whole device. `MaxNumberOfRecords` is the sum of the four log sizes, as Comment 2 says |
| A switch | `/redfish/v1/Chassis/{switch}/LogServices/EventLog`. The `Switch` resource has `LogServices`, but the schema allows no URI for them; the switch's `Chassis` is where it allows one. `MaxNumberOfRecords` is the 256 records cxl-fmd keeps: no command reports a size |

- Records are read with Get Event Records and then cleared, so what cxl-fmd
  keeps is the only copy: each record is kept before it is cleared, whatever
  its kind, with these exceptions, which are cleared and lost:
  - a record of an MLD whose LD-ID Valid Flag names an LD that cxl-fmd does
    not have;
  - a record of a DCD's Dynamic Capacity Event Log other than Add Capacity
    Response (04h) and Capacity Released (05h); of each of the event types
    00h to 03h, CXL r4.0 Table 8-229 says "This event shall only be reported
    to the host";
  - every record of a Multi-Headed device's Dynamic Capacity Event Log, which
    is drained to refresh its hosts' capacity, and whose records are not kept
    (a known gap).

  `LogService.ClearLog` drops cxl-fmd's copies.
- Only logs that are the FM's are read: a component's own MCTP CCI, and the
  FM-owned LD of an MLD behind a switch port, if its LDs could be read: the
  records of one whose LDs could not have nothing to be kept on. The logs of a component reached
  through the kernel's mailbox are shared with the kernel's driver (CXL r4.0
  Section 9.20.1) and are left to it; its `DeviceLog` has `ServiceEnabled`
  `false`. So does that of a pool LD of a Multi-Headed device: cxl-fmd does
  not read a Multi-Headed device's logs (a known gap). A record of an MH-MLD
  with its LD-ID Valid Flag set names an LD-ID "from 0 to n-1 relative to
  that head" (Section 2.5.1), and the specification gives no map from it to
  the LD of the pool; records about the whole device, or about a whole head
  (Table 8-221), could be placed, and are not read either.
- `Severity` is cxl-fmd's: `OK` for the Informational log, `Warning` for the
  Warning log, `Critical` for the Failure and Fatal logs.
- `EventTimestamp` is the record's Timestamp, left out when it is 0;
  `Created` is when cxl-fmd read the record. `Message` decodes the Physical
  Switch, Virtual CXL Switch and MLD Port Event Records, and names the kind of
  the others.
- The records of a DCD's Dynamic Capacity Event Log that are the FM's, Add
  Capacity Response (04h) and Capacity Released (05h), are in the `DeviceLog`
  of the LD whose host they are about: their Host ID is, "for an LD-FAM
  device, the LD-ID of the host interface that triggered the generation of the
  event" (CXL r4.0 Table 8-229). Their `CXLEntryType` is `DynamicCapacity`
  and their `Severity` `OK`: every one is Informational (Section
  8.2.10.2.1.6). They count towards the same `MaxNumberOfRecords`, the sum of
  the four log sizes Comment 2 names; DSP0288 does not add the Dynamic
  Capacity Event Log's. Those of a Multi-Headed device are not kept (see
  above).
- `Overflow` comes from the Overflow flag of Get Event Records (Table 1
  above),
  which the device keeps set only "until the host has consumed one or more
  events and returned the event handles to the device" (Table 8-232), and
  cxl-fmd returns them as it reads. Once a log has reported it, cxl-fmd keeps
  `Overflow` `true` until `LogService.ClearLog`: that is cxl-fmd's choice,
  not DSP0288's.
- The overflow is put on the last record of the first page of Get Event
  Records that said Overflow: cxl-fmd reads Table 8-232 as saying that no
  record is stored while the flag is set (it is set "when errors occur that
  the device cannot log without overwriting an existing log event"), so
  every record of that page was stored before the overflow, as Comment 1
  asks. It is not always the last record before the overflow: more of those
  can follow on later pages. An LD of an MLD that did not keep that record
  has none tagged; its `LogService` still says `Overflow`. The Dynamic
  Capacity Event Log puts it on the newest record kept of each LD.
  `OverflowErrorCount` is left out when the device gives an Overflow Error
  Count of 0, which "indicates the device does not have a count of errors
  that occurred after the overflow was established" (Table 8-232); each
  overflow timestamp is left out when it is 0.
- Each log is read until a Get Event Records comes back empty, which re-arms
  its MCTP Event Notification (Section 8.2.10.2.8), for at most 32 pages (16
  for the Dynamic Capacity Event Log): under a Response Message Limit of 256
  to 1024 bytes (Table 8-218), a page holds from 1 to 7 records of 128
  bytes after its 32-byte header. A log with more than that is
  emptied by the next reads, on the `--event-poll` timer. With
  `--event-poll 0` it is emptied only when that component's logs are read
  for another reason, a notification for another of its logs or a re-scan
  that finds a new device; until then its notification is not re-armed. A switch is asked for notifications of its four logs and of a
  completed background operation (Set MCTP Event Interrupt Policy,
  Bits[3:0] and Bit[15]), and a memory device on an MCTP endpoint of its own
  for the logs that are read (Bits[3:0], and Bit[4] for a DCD), unless its
  logs are the host's. The policy is set even when those bits are set
  already, as "The subsequent enabled events shall be sent to that address"
  (Section 8.2.10.2.7), with the reserved bits cleared. A memory device
  behind a switch is not: no route is defined for its notifications to reach
  the FM, and its logs are read on the `--event-poll` timer.
- A notification says only which logs have records; the MCTP address it came
  from says who sent it. The logs read are those of the switch or memory
  device on that endpoint, and every component's when the address is none of
  cxl-fmd's endpoints.

---

## Table 2 - CXL FM-API to Redfish Mapping

DSP0288 1.4.0 Section 4, Table 2 — CXL FM-API to Redfish mapping, given as
Table 1 is above: DSP0288's rows and names, and a last column that is
cxl-fmd's.

| CXL Command | CXL Field | Redfish Resource | Redfish Property | Comment | Implemented (cxl-fmd) |
|-------------|-----------|------------------|------------------|---------|-----------------------|
| Identify Switch Device | All | `Switch` | `SwitchType` | Shall contain `CXL`. | Yes |
| Identify Switch Device | Number of VCSs | `Switch` | `CXL/MaxVCSSupported` | | Yes, as `CXL/MaxVCSsSupported`: DSP8010 has no property named as DSP0288 names it |
| Identify Switch Device | Total Number of vPPBs | `Switch` | `CXL/TotalNumbervPPBs` | | Yes |
| Identify Switch Device | Total Number of Bound vPPBs | `Switch` | `CXL/NumberOfBoundvPPBs` | | Yes |
| Identify Switch Device | Number of HDM Decoders available per USP | `Switch` | `CXL/HDMDecoders` | | Yes, as `CXL/TotalHDMDecoders` ("The total number of HDM (Host Device Memory) decoders available per upstream port", Switch_v1): DSP8010's `Switch` has no `CXL/HDMDecoders`. Which property to use is cxl-fmd's reading |
| Get Physical Port State | Port Information List, PBR Capable | `Switch` | `CXL/PBRCapable` | | Yes |
| Get Physical Port State | Port Information List, Current Port Configuration State | `Port` | `CXL/CurrentPortConfigurationState` | | Yes |
| Get Physical Port State | Port Information List, Connected Device Mode | `Port` | `CXL/ConnectedDeviceMode` | | Yes |
| Get Physical Port State | Port Information List, Connected Device Type | `Port` | `CXL/ConnectedDeviceType` | | Yes |
| Get Physical Port State | Port Information List, Supported CXL Modes | `Port` | `CXL/SupportedCXLModes` | | Yes |
| Get Physical Port State | Port Information List, Supported LD Count | `Port` | `CXL/MaxLogicalDeviceCount` | | Yes |
| Get Physical Port State | Port Information List, Negotiated Link Width | `Port` | `ActiveWidth` | | Yes |
| Get Physical Port State | Port Information List, Supported Link Speeds Vector | `Port` | `LinkConfiguration/CapableLinkSpeedGbps` | | Yes |
| Get Physical Port State | Port Information List, Max Link Speed | `Port` | `MaxSpeedGbps` | | Yes |
| Get Physical Port State | Port Information List, Current Link Speed | `Port` | `CurrentSpeedGbps` | | Yes |
| Physical Port Control | Port Opcode 02h: Reset PPB | `Port` | `ResetPPB` action | | Yes, on a DSP only (cxl-fmd's choice, see below) |
| Physical Port Control | Port Opcode 00h and 01h: Assert and Deassert PERST | `Port` | `Reset` action | ForceRestart shall assert then deassert PERST. | No |
| Get Virtual CXL Switch Info | Virtual CXL Switch ID | `VirtualCXLSwitch` | `VCSId` | | Yes |
| Get Virtual CXL Switch Info | VCS State | `VirtualCXLSwitch` | `Status` | | Yes |
| Get Virtual CXL Switch Info | vPPB Array Index | `VirtualPCI2PCIBridge` | `VPPBId` | | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Binding Status | `VirtualPCI2PCIBridge` | `BindingStatus` | | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound Port Id | `VirtualPCI2PCIBridge` | `BoundPortId` | | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound LD Id | `VirtualPCI2PCIBridge` | `BoundLDId` | | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound PID | `VirtualPCI2PCIBridge` | `BoundPBRId` | | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] PCIe Device Serial Number + Bound LD Id | `VirtualPCI2PCIBridge` | `GCXLID` | | Yes |
| Get LD Info | Memory Size | `MemoryDomain` | `MemorySizeMiB` | | Yes |
| Get LD Info | LD Count | `PCIeDevice` | `CXLDevice/MaxNumberLogicalDevices` | | Yes |
| Get LD Info | QoS Telemetry Capabilities[0]: Egress Port Congestion Supported | `Port` | `CXL/QoSTelemetryCapabilities/EgressPortBackpressureSupported` | See Comment 1. | Yes, for an MLD on a switch port. An MLD that is not on one has no `Port`: cxl-fmd serves it on its `PCIeDevice` as `CXLDevice/EgressPortCongestionSupport` (cxl-fmd's choice) |
| Get LD Info | QoS Telemetry Capabilities[1]: Temporary Throughput Reduction Supported | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionSupported` | | Yes |
| Get LD Allocations | Memory Granularity | `MemoryDomain` | `MemoryChunkIncrementMiB` | | Yes |
| Get LD Allocations | Memory Granularity, LD Allocation List | `CXLLogicalDevice` | `MemorySizeMiB` | On a DCD, plus the extents the LD's host holds: the schema has it "equate to the sum of the dynamic capacity extents and the static capacity assigned to this logical device" ([DCD-FM-API.md](DCD-FM-API.md), Table 3). | Yes |
| Get QoS Control | QoS Telemetry Control[0]: Egress Port Congestion Enable | `Port` | `CXL/Congestion/CongestionTelemetryEnabled` | | Yes |
| Get QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | | Yes |
| Get QoS Control | Egress Moderate Percentage | `Port` | `CXL/Congestion/EgressModeratePercentage` | | Yes |
| Get QoS Control | Egress Severe Percentage | `Port` | `CXL/Congestion/EgressSeverePercentage` | | Yes |
| Get QoS Control | Backpressure Sample Interval | `Port` | `CXL/Congestion/BackpressureSampleInterval` | | Yes |
| Get QoS Control | ReqCmpBasis | `Port` | `CXL/Congestion/MaxSustainedRequestCmpBias` | | Yes |
| Get QoS Control | Completion Collection Interval | `Port` | `CXL/Congestion/CompletionCollectionInterval` | | Yes |
| Get QoS Status | Backpressure Average Percentage | `PortMetrics` | `CXL/BackpressureAveragePercentage` | | Yes |
| Get QoS Allocated BW | QoS Allocation Fraction | `CXLLogicalDevice` | `QoS/AllocatedBandwidth` | | Yes |
| Get QoS BW Limit | QoS Limit Fraction | `CXLLogicalDevice` | `QoS/LimitPercent` | | Yes |
| Set QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | | No |
| Get PBR Link Partner Info | Port ID - ID of requested Port | `Port` | `LinkPartnerTransmit/PortId` | | No |
| Get PBR Link Partner Info | Far End Port ID - Port ID of Far End Device | `Port` | `LinkPartnerReceived/PortId` | | No |
| Get PBR Link Partner Info | Device UUID | `Port` | `LinkPartnerReceived/LinkPartnerId` | | No |
| Get PBR Link Partner Info | Device UUID | `Port` | `LinkPartnerReceived/LinkPartnerId` | | No (DSP0288 1.4.0 lists this row twice) |
| MLD Port Event Record | All | `LogEntry` | | See Comment 2. | Yes |

### Comments for Table 2

**Comment 1:** For an MLD, implementations shall represent Egress Port Congestion Supported in the `Port` resource. For an SLD, implementations shall represent Egress Port Congestion Supported in the `PCIeDevice` resource.

**Comment 2:** A `LogEntry` resource is used to capture CXL event records. The `CXLEntryType` property shall contain the specific CXL entry type, and the `EntryType` property shall contain `CXL`. Overflow information is captured in properties of the `LogEntry` prior to the overflow condition.

### Reset PPB

Physical Port Control "is used by the FM to control unbound ports and MLD
ports, including issuing resets and controlling sidebands" (CXL r4.0
Section 7.6.7.1.3). cxl-fmd offers `ResetPPB` only on a DSP, and answers
400 for any other port, without sending anything; that is cxl-fmd's
choice. Whether the DSP is unbound or has an MLD is left to the switch.

## cxl-fmd mapping (not in DSP0288)

DSP0288 maps none of these commands. The mapping is cxl-fmd's own:

| CXL Command | Redfish Resource | Redfish Operation |
|-------------|------------------|-------------------|
| Set LD Allocations (5402h) | `MemoryChunks` | `POST` and `DELETE` of a chunk of an MLD, or of a pool LD of a Multi-Headed device (see "Memory Provisioning") |
| Set Partition Info (4101h) | `MemoryChunks` | `POST` and `DELETE` of a `"Volatile"` chunk of an SLD with partitionable capacity (see "Memory Provisioning") |
| Bind vPPB (5201h) | `Connection` | `POST /redfish/v1/Fabrics/CXL/Connections` |
| Unbind vPPB (5202h) | `Connection` | `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}`, with Unbind Option 1h, Simulate Managed Hot-Remove |
| Unbind vPPB (5202h) | `Connection` | `POST /redfish/v1/Fabrics/CXL/Connections/{id}/Actions/Oem/Cxl.ForceUnbind`, with Unbind Option 2h, Simulate Surprise Hot-Remove (cxl-fmd's Oem action) |

DSP0288 1.4.0 is silent on Bind vPPB and Unbind vPPB: it names neither
command, nor a `Connection`, and of Get Virtual CXL Switch Info it maps
only the Binding Status, to `VirtualPCI2PCIBridge.BindingStatus`. That a
`Connection` is a vPPB binding, made by a `POST` and removed by a `DELETE`,
follows the OCP white paper "CMS: Memory Fabric Orchestration
Architecture", Version 1.0, Section 4.2.5, "Manage Memory Binding for
VHS", which is informative, not a standard: "To assign the composed memory
chunk to a host is by POST a connection to the endpoints between the
memory chunk and the target host endpoint in the fabric", and "To remove
the existing binding, the user issues DELETE method to the connection to
be unbinded." How cxl-fmd does it is its own choice throughout.

Bind vPPB has no action, and Unbind vPPB only the Oem `#Cxl.ForceUnbind`:
a `POST` to `/redfish/v1/Fabrics/CXL/Connections` for a host on a switch
binds a vPPB of the host's VCS, and a `DELETE` of the connection unbinds it
(see "Memory Binding (Connections)").

---

## HTTP Method Mapping

DSP0288 Sections 3 and 4: "Implementations shall map the following to HTTP
operations:"

| CXL Command Type | HTTP Method | Implemented (cxl-fmd) |
|------------------|-------------|:-----------:|
| CXL "get" commands | `GET` | Yes |
| CXL "set" commands (not actions) | `PATCH` or `PUT` | Partial |
| CXL "set" commands (actions) | `POST` | Partial |

Currently implemented write operations:
- `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` - Create Memory Chunk (Set LD Allocations / Set Partition Info)
- `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` - Delete Memory Chunk (Set LD Allocations / Set Partition Info)
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}` - Set Timestamp
- `PATCH /redfish/v1/Chassis/{id}/Memory/{mem}` - Set Partition Info
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia` - Scan Media
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.SetPassphrase` - Set Passphrase
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DisablePassphrase` - Disable Passphrase
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.Unlock` - Unlock
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.FreezeSecurityState` - Freeze Security State
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.PassphraseSecureErase` - Passphrase Secure Erase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetPassphrase` - Set Passphrase, user (deprecated alias)
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetMasterPassphrase` - Set Passphrase, master (deprecated alias)
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisablePassphrase` - Disable Passphrase, user (deprecated alias)
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisableMasterPassphrase` - Disable Passphrase, master (deprecated alias)
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.FreezeSecurityState` - Freeze Security State (deprecated alias)
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SecureEraseUnit` - Passphrase Secure Erase (deprecated alias)
- `POST /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB` - Reset PPB
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}` - Set DC Region Configuration
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` - Initiate Dynamic Capacity Add
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` - Initiate Dynamic Capacity Release
- `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` - Initiate Dynamic Capacity Release from the extent's host only, answered `202` with a `Task` as the two actions above are: for an extent with a tag in a Sharable region, Tag-based, which releases every extent of that host with the tag, not only this one; Prescriptive of this one extent for any other (cxl-fmd's meaning of DELETE, see [DCD-FM-API.md](DCD-FM-API.md#delete-of-a-memoryextent); `DynamicCapacityRelease` for any other release)
- `POST /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory` - DC Add Reference / DC Remove Reference

An SLD that a switch reports on one of its ports (`{switch}_port{n}_dev`)
is sent none of the memory device commands above. It has no MCTP endpoint
of its own, and the switch cannot pass them on: "Valid targets for the
tunneled commands include switch MLD Ports, valid LDs within an MLD, and
the LD Pool CCI in a Multi-Headed device" (CXL r4.0 Section 7.6.7.3.1).
Its `PCIeDevice` has no `CXLDevice/Timestamp`, a `PATCH` of the timestamp
returns 400, and the `Memory` writes and actions fail. An MLD on a switch
port is reached through that port. In mock mode (`--mock`) no command is
sent for any device, and these requests act on the mock's state as for any
other. The `Memory` writes and actions, and the chunk requests that send
Set Partition Info, are `409` unless cxl-fmd was started with
`--allow-mctp-restricted`, on hardware for every device but an SLD on the
kernel's mailbox, and in a mock for every device (see "Commands restricted
on MCTP").

The write operations on a `Connection`, which no DSP0288 rule maps (see
"cxl-fmd mapping (not in DSP0288)" above), are cxl-fmd's:

- `POST /redfish/v1/Fabrics/CXL/Connections` - Bind vPPB for a switch-attached host; for a direct-attached pair, the Connection it has already
- `DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}` - Unbind vPPB, Simulate Managed Hot-Remove (switch-attached only)
- `POST /redfish/v1/Fabrics/CXL/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` - Unbind vPPB, Simulate Surprise Hot-Remove (cxl-fmd's Oem action)

---

## Status from Get Health Info

The `Status` of a memory device's `Chassis`, `PCIeDevice`,
`CXLLogicalDevice`, `Memory`, `MemoryDomain` and `MemoryChunk` comes from
Get Health Info (4200h, CXL r4.0 Table 8-315): an SLD's, or that of each
LD of an MLD or a Multi-Headed device added up. Health Status is a bit
field and Media Status an enumeration; every view maps them the same way
(`fmd_rollup_status()`), and the mapping is cxl-fmd's (DSP0288 maps Health
Status to `Status`, Comment 3, and Media Status to nothing):

- `State` is that of the worst Media Status (see "Media Status" in the
  README): only 03h, All Data Lost, is `UnavailableOffline`. The Health
  Status bits do not change it.
- `Health` is the worse of the Health Status bits, OR-ed then mapped
  (Hardware Replacement Needed is `Critical`; Maintenance Needed,
  Performance Degraded and Memory Capacity Degraded are `Warning`), and of
  the Media Status: 03h is `Critical`; 00h and 01h (Not Ready) add
  nothing; any other is `Warning`.
- Get Health Info is read when a device is discovered, and again
  (cxl-fmd's choice of when): on every `--rescan`, for each device found
  before it, and after a drain of a device's event logs that found a
  record in its Warning, Failure or Fatal Event Log, or an overflow of
  one. A change of Health Status or Media Status adds such a record:
  "When transitioning from 0 to 1, the device shall add an event to the
  Warning or Failure Event Log" (Maintenance Needed; the other bits name
  their logs), and of Media Status "When transitioning from any state to
  any other state, the device shall add an event to the Failure or Fatal
  Event Log" (Table 8-315). A bit going back to 0 adds none, which the
  `--rescan` timer covers. Which record it was is not looked at. A
  Multi-Headed device's logs are not read, nor those of an SLD on a switch
  port, which has no Get Health Info at all: a Multi-Headed device's pool
  LDs are read on the `--rescan` timer only. With `--rescan 0`, only the
  records do it.
- When Get Health Info was not answered Success, `Health` is left out,
  not reported `OK`: nothing is known of it. That is so of a read after
  discovery too: what an earlier read said is not served. Of several LDs, one that was
  not read leaves `Health` out unless another is known to be `Warning` or
  `Critical`, which it is at least. `State` is then that of the LDs that
  were read, `Enabled` if none was.
- A device's `Chassis` has `HealthRollup` too, the same as its `Health`:
  its `PCIeDevice` and `Memory` are views of the same health.
- A switch's `Chassis` is `Enabled` with no `Health`: a switch reads none
  ("It is prohibited for switches to support any commands from the Memory
  Device Command Set", Table 8-308, note 1). Its `HealthRollup` is that of
  every device on its ports, added up as above (Resource_v1: Health "shall
  represent the health state of the resource without considering its
  dependent resources", HealthRollup "the resource and its dependent
  resources").
- `Conditions` say why. DSP0288 1.4.0 Section 3 says implementations
  "shall adhere to the numbered comments referenced in the Comment
  column", and Table 1 maps Health Status to `Status` with Comment 3:
  "The Conditions property within the Status property is used to show why
  the Health property contains a value other than OK". So every view
  above has one Condition for each Health Status bit set, and one for each
  Media Status read that makes `Health` other than `OK` (02h to 09h, and
  a reserved value, with the value as its argument); 00h and 01h have
  none. A `Memory` also has one for each of Additional Status Bit[4],
  Corrected Volatile Error Count, and Bit[5], Corrected Persistent Error
  Count: "The Conditions property within the Status property of the
  Memory resource shall contain messages to indicate either the Corrected
  Volatile Error Count or Corrected Persistent Error Count flags are set"
  (Comment 4). These two do not change `Health` (cxl-fmd's choice:
  DSP0288 maps only Health Status to it, and Resource_v1 says the
  conditions "may affect the `Health`"), so a `Memory` can be `OK` with
  them. Of several LDs (an MLD's `Chassis`, a switch's), the `Conditions`
  are those of every LD read, with no `OriginOfCondition`. A Get Health
  Info that was not read gives none. There is no `Timestamp`: cxl-fmd does
  not know when a condition began.
- The messages are from cxl-fmd's registry `CxlHealth.1.0.0.json`
  (MessageIds `CxlHealth.1.0.<Key>`), served at
  `/redfish/v1/Registries/CxlHealth.1.0.0.json` and listed in
  `/redfish/v1/Registries` ("Local copy of a Redfish registry file", DSP0266
  1.25.0 Table 4). Of the DMTF message registries published at
  https://redfish.dmtf.org/registries/ (the latest of each of the 25,
  Base 1.24.0, ResourceEvent 1.5.0, Platform 1.5.0 and Device 1.0.0
  among them, as of 2026-11-17), none has a message for a CXL Health
  Status bit or Media Status. For Additional Status, ResourceEvent 1.5.0's
  `ResourceWarningThresholdExceeded` takes the threshold's value as an
  argument, which cxl-fmd does not read, and `ResourceErrorsDetected`
  says that errors were detected, not that a count reached its threshold.
  Each message is
  the description Table 8-315 gives (Health Status bits, Media Status);
  those of Additional Status are its "The device's count of total
  corrected errors has risen to or above the user-programmable ...
  threshold" with "volatile" or "persistent" put in. The name is cxl-fmd's
  Oem identifier `Cxl` followed by `Health` ("the defining organization's
  unique OEM identifier ... should be prepended to the registry name",
  Section 9.8.5); see "Oem identifier" under Known Gaps for what that
  identifier is.

| Get Health Info | `Status/Conditions` MessageId (`CxlHealth.1.0.`) | Severity | Views |
|-----------------|-------------------------------------------------|----------|-------|
| Health Status Bit[0] | `MaintenanceNeeded` | Warning | all |
| Health Status Bit[1] | `PerformanceDegraded` | Warning | all |
| Health Status Bit[2] | `HardwareReplacementNeeded` | Critical | all |
| Health Status Bit[3] | `MemoryCapacityDegraded` | Warning | all |
| Media Status 02h | `WritePersistencyLost` | Warning | all |
| Media Status 03h | `AllDataLost` | Critical | all |
| Media Status 04h | `WritePersistencyLossOnPowerLoss` | Warning | all |
| Media Status 05h | `WritePersistencyLossOnShutdown` | Warning | all |
| Media Status 06h | `WritePersistencyLossImminent` | Warning | all |
| Media Status 07h | `AllDataLossOnPowerLoss` | Warning | all |
| Media Status 08h | `AllDataLossOnShutdown` | Warning | all |
| Media Status 09h | `AllDataLossImminent` | Warning | all |
| Media Status 0Ah to FFh | `MediaStatusReserved` (the value, `0Ah`) | Warning | all |
| Additional Status Bit[4] | `CorrectedVolatileErrorCount` | Warning | `Memory` |
| Additional Status Bit[5] | `CorrectedPersistentErrorCount` | Warning | `Memory` |

## Oem schema

cxl-fmd's Oem properties and its one Oem action are described by its own
CSDL schema, `Cxl_v1.xml` (namespaces `Cxl` and `Cxl.v1_0_0`, in
`schema/oem/csdl`). It is served at `/redfish/v1/Schemas/Cxl_v1.xml`
("Local copy of a Redfish schema file", DSP0266 1.25.0 Table 4) and
`$metadata` references it there ("The OData metadata document may
reference additional schema documents that describe OEM-specific
extensions that the service uses", Section 8.4.2.2; a reference may be
"A relative URI to a local copy of the Redfish schema", Section
8.4.2.1). Every `Oem.Cxl` object has `@odata.type` naming its type, each
derived from `Resource.OemObject`:

| Resource | `Oem.Cxl` `@odata.type` | Properties |
|----------|-------------------------|------------|
| `MemoryDomain` of a host (`/redfish/v1/Systems/{id}/MemoryDomains/{dom}`) | `#Cxl.v1_0_0.MemoryDomain` | `HostState` |
| `Connection` | `#Cxl.v1_0_0.Connection` | `Origin` |
| `PCIeDevice` of a DCD | `#Cxl.v1_0_0.PCIeDevice` | `DynamicCapacity` (sizes in bytes) and `DynamicCapacityTags` (with `LastRefreshed`; see [DCD-FM-API.md](DCD-FM-API.md)) |
| `CXLLogicalDevice` of a DCD, once its host's regions or extents have been read | `#Cxl.v1_0_0.CXLLogicalDevice` | `DynamicCapacityRegionsLastRefreshed`, `DynamicCapacityExtentsLastRefreshed` |
| `MemoryRegion` of a DCD | `#Cxl.v1_0_0.MemoryRegion` | `RegionBaseOffsetBytes`, `RegionSizeBytes`, `BlockSizeBytes`, `LastRefreshed` |
| `MemoryExtent` | `#Cxl.v1_0_0.MemoryExtent` | `ExtentOffsetBytes`, `ExtentSizeBytes`, `LastRefreshed` |

The action `#Cxl.ForceUnbind` of a switch-attached `Connection` is the
`ForceUnbind` action of the `Cxl` namespace, bound to
`Connection.v1_0_0.OemActions`: "#<OEMSchemaName>.<Action>" (Section
9.8.8). The schema, its names and every definition in it are cxl-fmd's;
see "Oem identifier" under Known Gaps for `Cxl`. There is no JSON Schema
of it, and no `/redfish/v1/JsonSchemas`.

## Error Responses

All error responses follow the Redfish error format: an `error` object with a `code`, a `message` and `@Message.ExtendedInfo` (see "Error Response Format").

### HTTP Status Codes

| Status | Meaning | When Used |
|--------|---------|-----------|
| 200 OK | Success | GET requests returning data; actions; a `POST` of a Connection the host has already, switch-attached (bound, `Absent` included) or direct-attached, with the Connection and `Location`; `CXLReferenceDCMemory` or a `MemoryRegion` `PATCH` the device accepted whose state could not be read back after it, with an error object whose `code` is `Base.1.24.Success`, with `Base.1.24.Success` in its `@Message.ExtendedInfo`, and whose message says not to send it again (see [DCD-FM-API.md](DCD-FM-API.md), Comment 3); the task monitor of a `DynamicCapacityAdd` or `DynamicCapacityRelease` that is `Completed`, with such an object whose message says what the host accepted or released |
| 201 Created | Resource created | POST to collections (MemoryChunks; a Connection whose bind the switch finished before answering); the task monitor of a bind once it has succeeded |
| 202 Accepted | Started, not finished | POST and DELETE of a Connection, and `#Cxl.ForceUnbind`, when the switch binds or unbinds in the background, with a `Task` and its monitor in `Location`; a `POST` of a Connection that is being bound (`Starting`), with the task of that bind; the task monitor while the task runs or is `Interrupted` (see "Binds and unbinds in the background"); `DynamicCapacityAdd`, `DynamicCapacityRelease` and a `MemoryExtent` `DELETE` the device accepted, with a `Task` that follows the host (see [Tasks of an add or a release](DCD-FM-API.md#tasks-of-an-add-or-a-release)) |
| 204 No Content | Success, no body | DELETE requests, and `#Cxl.ForceUnbind`, finished at once; the task monitor of an unbind, or of a `MemoryExtent` `DELETE`, once it has succeeded |
| 400 Bad Request | Invalid request | Malformed JSON, missing required fields, invalid values; a Connection `POST` with other than one initiator and one target, whose host and device are not on the same switch, whose direct-attached host has no Connection to the device or head, whose chunk is not on the target, whose chunks are on more than one LD, whose MLD target names no LD (no chunk), whose `AccessCapabilities` are not `Read` and `Write`, whose `AccessState` is not `Optimized`, that has `VolumeInfo`, or whose target is, or is reached through, a head that presents an MLD; a chunk `POST` that does not name exactly one LD of its device by its URI, or asks for what an allocation cannot be (see "Creating Memory Chunks"); a bind or unbind the switch answered Invalid Input, at once or at the end of the background operation (the task monitor); `#Cxl.ForceUnbind` of a direct-attached Connection; `ResetPPB` on a port that is not a DSP |
| 404 Not Found | Resource not found | Invalid resource ID or path; the initiator, target or chunk a Connection `POST` names; a Connection whose vPPB no longer holds what its ID names (a stale ID), on GET, DELETE and `#Cxl.ForceUnbind` |
| 405 Method Not Allowed | Method not supported, with an `Allow` header | A method a resource does not take (`PUT` of any, `PATCH` of a Chassis, `POST` to a collection that takes none, `DELETE` of a Chassis, `HEAD`, `OPTIONS`), "If the resource exists but does not support the requested operation" (DSP0266 Section 7.5.3); `Allow` lists the methods it takes: `GET`, and `POST` for the `MemoryChunks` of a Chassis and the Connections, `PATCH` for a `PCIeDevice`, a `Memory` and a DCD `MemoryRegion`, `DELETE` for a Chassis chunk, a DCD `MemoryExtent` and a switch-attached Connection. `PATCH` of a Connection (`Allow: GET, DELETE`, `GET` for a direct-attached one); `DELETE` of a direct-attached Connection (`Allow: GET`). A URI that is not a resource is `404` whatever the method |
| 409 Conflict | Resource conflict | Capacity exceeded, invalid state transition; a chunk `DELETE` or `POST` on a pool LD whose head has a Connection; a chunk `DELETE` of a chunk larger than its LD's allocation; a Connection to a port or LD bound, or being bound or unbound, for another vPPB (another host's VCS, or the whole port while an LD of it is, or the reverse); for a host whose VCS has no unbound vPPB; a `POST` of a Connection the host has that is being unbound (`Updating`); a chunk `DELETE`, or a chunk `POST` that would change a bound LD or SLD, while a switch-attached Connection exposes it; a Connection `POST` naming a staged chunk; without `--allow-mctp-restricted`, a request that would send a command Table 8-308 marks O4 for MCTP as an MCTP message (see "Commands restricted on MCTP"); a security action the device answered Invalid Security State; a Dynamic Capacity add, release or `MemoryExtent` `DELETE` for a host and Tag a `Task` already follows, or an add of a Tag whose `PendingLDs` has the host (`ResourceInUse`, nothing sent); the task monitor of an add the host was seen to accept none of |
| 412 Precondition Failed | A precondition cxl-fmd cannot check | A `POST`, `PATCH` or `DELETE` with `If-Match` or `If-None-Match`, whatever its value, that would otherwise be carried out: nothing is done (see "Conditional Requests") |
| 413 Payload Too Large | Request body too large | Body exceeds 64KB limit |
| 500 Internal Server Error | The operation failed | A bind or unbind the switch failed (Internal Error, or another code), or another failure; the task monitor of a task that ended in `Exception` with a return code other than Invalid Input, Unsupported, Busy or Retry Required, with none, or whose switch is gone ("Outcome unknown"), or of a Dynamic Capacity task whose device is gone ("Outcome unknown"); "Outcome unknown" when a command's response could not be read, so whether the device carried it out is not known (check before sending it again) |
| 501 Not Implemented | Feature not implemented | A query parameter that starts with `$` (`$filter`, `$top`, `$skip`, `$select`, `$expand`, ...), on any method, with an `@Message.ExtendedInfo` entry `Base.1.24.QueryParameterUnsupported` naming each (DSP0266 Section 7.3.1; cxl-fmd supports none, and the service root has no `ProtocolFeaturesSupported`; other query parameters, such as `only` and `excerpt`, are ignored); a chunk `DELETE` whose allocation cxl-fmd cannot give back; a chunk `POST` on a Multi-Headed device whose LD Pool CCI is not known; a bind or unbind the switch answered Unsupported, at once or as the return code of the background operation (the task monitor) (Bind vPPB and Unbind vPPB are optional, "O" in CXL r4.0 Table 7-31); a security action whose command the LD's Command Effects Log does not list, that the device answered Unsupported, or on a pool LD of a Multi-Headed device (see "Security actions") |
| 503 Service Unavailable | Try again (no `Retry-After` header is sent) | A bind or unbind while the switch is Busy (at once, or as the return code of the background operation: the task monitor), after it answered Retry Required twice, or when the vPPB chosen was bound meanwhile; a DELETE or `#Cxl.ForceUnbind` of a Connection whose bind or unbind is under way (see "Forcing an Unbind" for the one exception); a Dynamic Capacity add, release or `MemoryExtent` `DELETE` on hardware whose host could not be read for the task's baseline (nothing sent) |

Which status each case gets is cxl-fmd's choice; neither DSP0288 nor the CXL
specification sets it.

### Conditional Requests

cxl-fmd returns no `ETag` header and no `@odata.etag`, so it cannot compare
an entity tag a client sends with a resource's. A `POST`, `PATCH` or
`DELETE` with an `If-Match` or `If-None-Match` header, whatever its value
(`*` included), is answered `412 Precondition Failed`, and nothing is done
or sent: "412 Precondition Failed: Precondition check, such as check of the
OData-Version, If-Match, or If-None-Match header, failed." (DSP0266 1.25.0
Table 14). Before, the header was ignored, and the request carried out as
if it had none: a `PATCH` of a `MemoryRegion` with a stale `If-Match` sent
Set DC Region Configuration. DSP0266 asks for `If-Match` only of a service
that returns ETags ("PUT and PATCH requests for resources for which a
service returns ETags shall support If-Match", Table 6), so refusing every
precondition, so that a client that asks for one never gets a write it did
not condition, is cxl-fmd's choice. The error's
`@Message.ExtendedInfo` is `GeneralError` with the Resolution "Resubmit
the request without If-Match or If-None-Match: cxl-fmd returns no ETags.":
Base `PreconditionFailed` is for deep operations only ("The use of this
message shall only be used in responses for deep operations, since HTTP
status code 412 is typically used for this", Base.1.24.0.json).

A request answered `404` or `405` without the header is answered so with
it: "A server MUST ignore all received preconditions if its response to
the same request without those conditions, prior to processing the request
content, would have been a status code other than a 2xx (Successful) or
412 (Precondition Failed)" (RFC 9110 Section 13.2.1). An action is judged
by its resource only: a `POST` to an action that a resource that is there
does not have is `412` with the header, and `404` without (cxl-fmd's
simplification). A `GET` is answered
as one without the header: a conditional GET is "If supported by the
service" (DSP0266 Section 7.2.1), and cxl-fmd does not support it.

### Error Response Format

Every error response is an `error` object with `code`, `message` and
`@Message.ExtendedInfo` ("The @Message.ExtendedInfo property should be
present in all error responses", DSP0266 1.25.0 Section 8.6):

- `code` is always `Base.1.24.GeneralError`: "If no better message exists
  or the ExtendedInfo array contains multiple messages, use GeneralError
  from the Base Message Registry only in the code property of the error
  object" (Section 9.5.11.2). Keeping one code is cxl-fmd's choice.
- `message` is cxl-fmd's own sentence on what went wrong, the same text as
  before `@Message.ExtendedInfo` was added. Table 16 has `message`
  "Displays a human-readable error message that corresponds to the message
  in the message registry": it is not GeneralError's registry text, but
  the sentence existing clients read, which the registry message in
  `@Message.ExtendedInfo` stands beside (cxl-fmd's choice).
- `@Message.ExtendedInfo` holds messages of the DMTF Base registry 1.24.0
  (`Base.1.24.0.json`, a copy of which is in `schema/dmtf/registries`):
  `MessageId` `Base.1.24.<key>` (the format is
  "<MessageRegistryPrefix>.<MajorVersion>.<MinorVersion>.<MessageKey>",
  Section 9.5.11.2), `Message` the registry's with `MessageArgs` put in,
  `MessageSeverity` (and the deprecated `Severity`, the same) the
  registry's, `RelatedProperties` where cxl-fmd knows the property, and
  `Resolution` the registry's unless cxl-fmd has a better one. Where no
  message of the registry fits, it is `GeneralError` with a `Resolution`
  of cxl-fmd's ("When an implementation uses GeneralError from the Base
  Message Registry in ExtendedInfo, the implementation should include a
  service-defined value for the Resolution property", Section 9.5.11.2).

The messages cxl-fmd uses, and when (cxl-fmd's choices):

| MessageId (`Base.1.24.`) | When |
|--------------------------|------|
| `InvalidURI` | `404` for a URI that is not a resource, with the URI; `400` for a chunk `DELETE` whose chunk is not in the MemoryDomain of its URI |
| `OperationNotAllowed` | `405` |
| `MalformedJSON` | `400` for a request body that is not JSON |
| `PropertyMissing` | `400` for a chunk `POST` with no `MemoryChunkSizeMiB`, or no `AddressRangeType` where one has to be named |
| `PropertyValueError` | `400` (and `404` for a Connection `POST` whose initiator, target or chunk is not found) where cxl-fmd knows which property is wrong |
| `ResourceInUse` | `409` for a resource bound, being bound or unbound, or exposed by a Connection, and for a command that may not go as an MCTP message; `503` for a device or switch that is busy or asked for a retry |
| `ResourceExhaustion` | `409` for a chunk its MemoryDomain cannot give, with the domain's URI; a Dynamic Capacity request the device has no room for |
| `ActionNotSupported` | `501` for an action the device does not support; `400` for `ResetPPB` on a port that is not a DSP and `#Cxl.ForceUnbind` of a direct-attached Connection |
| `ResourceCannotBeDeleted` | `501` for a chunk whose allocation cxl-fmd cannot give back, and a Connection the switch cannot unbind (Unsupported) |
| `PayloadTooLarge` | `413` |
| `InternalError` | `500` when cxl-fmd runs out of memory |
| `QueryParameterUnsupported` | `501`, one for each query parameter that starts with `$` |
| `GeneralError` | everything else, with a `Resolution` of cxl-fmd's; a `500` "Outcome unknown" has "Read the resource to learn whether the request was carried out before it is resubmitted: cxl-fmd does not know." |

### Common Errors

**404 Not Found - Invalid Resource ID:**
```bash
curl -s http://localhost:8080/redfish/v1/Chassis/nonexistent | jq
```
```json
{
    "error": {
        "code": "Base.1.24.GeneralError",
        "message": "Resource not found",
        "@Message.ExtendedInfo": [
            {
                "MessageId": "Base.1.24.InvalidURI",
                "Message": "The URI '/redfish/v1/Chassis/nonexistent' was not found.",
                "MessageSeverity": "Critical",
                "Severity": "Critical",
                "Resolution": "Provide a valid URI and resubmit the request.",
                "MessageArgs": ["/redfish/v1/Chassis/nonexistent"]
            }
        ]
    }
}
```

**400 Bad Request - Missing Required Property:**
```bash
curl -s -X POST http://localhost:8080/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks \
  -H "Content-Type: application/json" \
  -d '{}' | jq
```
```json
{
    "error": {
        "code": "Base.1.24.GeneralError",
        "message": "Invalid request body - MemoryChunkSizeMiB required",
        "@Message.ExtendedInfo": [
            {
                "MessageId": "Base.1.24.PropertyMissing",
                "Message": "The property 'MemoryChunkSizeMiB' is a required property and must be included in the request.",
                "MessageSeverity": "Warning",
                "Severity": "Warning",
                "Resolution": "Ensure that the property is in the request body and has a valid value and resubmit the request if the operation failed.",
                "MessageArgs": ["MemoryChunkSizeMiB"],
                "RelatedProperties": ["/MemoryChunkSizeMiB"]
            }
        ]
    }
}
```

**409 Conflict - Capacity Exceeded:**
```bash
curl -s -X POST http://localhost:8080/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0/MemoryChunks \
  -H "Content-Type: application/json" \
  -d '{"MemoryChunkSizeMiB": 999999999}' | jq
```
```json
{
    "error": {
        "code": "Base.1.24.GeneralError",
        "message": "Insufficient capacity in memory domain",
        "@Message.ExtendedInfo": [
            {
                "MessageId": "Base.1.24.ResourceExhaustion",
                "Message": "The resource '/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0' was unable to satisfy the request due to unavailability of resources.",
                "MessageSeverity": "Critical",
                "Severity": "Critical",
                "Resolution": "Ensure that the resources are available and resubmit the request.",
                "MessageArgs": ["/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0"]
            }
        ]
    }
}
```

---

## Endpoint Reference

### Service Root

| Endpoint | Description |
|----------|-------------|
| `GET /redfish/v1` | Service Root |
| `GET /redfish/v1/odata` | OData Service Document |
| `GET /redfish/v1/$metadata` | OData Metadata Document |
| `GET /redfish/v1/Registries` | cxl-fmd's registries (`MessageRegistryFileCollection`) |
| `GET /redfish/v1/Registries/CxlHealth` | The `MessageRegistryFile` of `CxlHealth.1.0` |
| `GET /redfish/v1/Registries/CxlHealth.1.0.0.json` | The registry of the `Status/Conditions` messages (see "Status from Get Health Info") |
| `GET /redfish/v1/Schemas/Cxl_v1.xml` | The CSDL of cxl-fmd's Oem properties and Oem action (see "Oem schema") |

### Systems Model

Host systems that can access CXL memory.

| Endpoint | Description |
|----------|-------------|
| `GET /redfish/v1/Systems` | Systems Collection (hosts) |
| `GET /redfish/v1/Systems/{sys}` | Individual System (host) |
| `GET /redfish/v1/Systems/{sys}/MemoryDomains` | Host-visible Memory Domains (from Connections) |
| `GET /redfish/v1/Systems/{sys}/MemoryDomains/{dom}` | Host Memory Domain |
| `GET /redfish/v1/Systems/{sys}/MemoryDomains/{dom}/MemoryChunks` | Host Memory Chunks |
| `GET /redfish/v1/Systems/{sys}/MemoryDomains/{dom}/MemoryChunks/{chunk}` | Host Memory Chunk |

**Host Discovery:**

| Topology | Host Creation | System ID Format |
|----------|---------------|------------------|
| Switch-attached | One host per VCS (via USP) | `{switch_id}_host{N}` (e.g., `cxl_switch_0_host0`) |
| Direct-attached | Default "local" host | `local` |

For switch-attached topologies, each Virtual CXL Switch (VCS) represents a host connection via its Upstream Switch Port (USP). The FM discovers hosts by enumerating active VCSes.

For direct-attached topologies (no switch), a default "local" host is automatically created representing the system running cxl-fmd. This ensures the Systems collection always contains at least one system when devices are present.

For switch-attached topologies, real discovery makes a host only for a VCS
whose VCS State is Enabled: its USP ID is "Valid only when the VCS is
enabled" (CXL r4.0 Table 7-34). The mock does the same. A host made while
its VCS was enabled is not removed if the VCS is later read as disabled.

**MemoryDomains from Connections:** A host's MemoryDomains are computed,
when served, from its Connections (see "Memory Binding (Connections)"),
whoever made the binding: one per Connection of the host whose
`Status.State` is `Enabled`. A `Starting`, `Updating` or `Absent`
Connection, or one whose target is not known (`Enabled` with `Health`
`Warning`), gives none: no memory is reachable through it, or none can be
named. Which Connections give one is cxl-fmd's choice.

- `Id`: the Connection's ID (e.g. `cxl_switch_0_vcs0_vppb1_p2`)
- `MemorySizeMiB`: the sum of its chunks; for an LD of an MLD or a
  Multi-Headed device that has none, the LD's size (cxl-fmd's choice)
- `Links.CXLLogicalDevices`: the CXLLogicalDevice(s) the Connection reaches
- `MemoryChunks`: one member per chunk of the target, each with its
  `MemoryChunkSizeMiB` and `AddressRangeType`, `MediaLocation` `Remote`
  ("created using remote media accessible through a fabric",
  MemoryChunks_v1), and `Links.Endpoints` the Connection's targets

This is what a binding makes reachable, not what the host has mapped: the
FM does not see that. "The switch prohibits any CXL.io access from the FM
to a bound SLD port and any CXL.io access from the FM to a bound LD within
an MLD component" (CXL r4.0 Section 7.2.3.1). Each host `MemoryDomain`
says so with `"Oem": {"Cxl": {"@odata.type": "#Cxl.v1_0_0.MemoryDomain",
"HostState": "NotObserved"}}` (cxl-fmd's Oem): what the host has accepted, mapped or put to use is not known to
cxl-fmd (see "What cxl-fmd can see of memory a host is given"). A domain
from a direct-attached Connection that is `Inferred` rests on no evidence
of a link either (see "What a Connection is").

### Chassis Model

CXL devices are modeled in the Chassis hierarchy.

| Endpoint | Description |
|----------|-------------|
| `GET /redfish/v1/Chassis` | Chassis Collection |
| `GET /redfish/v1/Chassis/{id}` | Individual Chassis (device or switch) |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices` | PCIe Devices in Chassis |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}` | PCIe Device; a dynamic capacity device has its 5608h tag list in `Oem/Cxl/DynamicCapacityTags` |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices` | CXL Logical Devices |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}` | CXL Logical Device |
| `GET .../CXLLogicalDevices/{ld}/MemoryRegions` | Dynamic capacity regions visible to the LD's host |
| `GET .../CXLLogicalDevices/{ld}/MemoryRegions/{region}` | DC Region |
| `PATCH .../CXLLogicalDevices/{ld}/MemoryRegions/{region}` | Set DC Region Configuration (`BlockSizeMiB`, `SanitizeOnRelease`) |
| `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` | Initiate Dynamic Capacity Add |
| `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` | Initiate Dynamic Capacity Release |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents` | Dynamic capacity extents on the device, one per host's extent (`ext_<ld>_<hex DPA>`) |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` | DC Extent |
| `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` | Release the extent from its host only; with a tag in a Sharable region, every extent of that host with the tag goes |
| `POST /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory` | DC Add/Remove Reference |
| `GET /redfish/v1/Chassis/{id}/MemoryDomains` | Memory Domains |
| `GET /redfish/v1/Chassis/{id}/MemoryDomains/{dom}` | Memory Domain |
| `GET /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` | Memory Chunks Collection |
| `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` | Create Memory Chunk |
| `GET /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` | Memory Chunk |
| `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` | Delete Memory Chunk |
| `GET /redfish/v1/Chassis/{id}/Memory` | Memory Collection |
| `GET /redfish/v1/Chassis/{id}/Memory/{mem}` | Memory Resource |
| `GET /redfish/v1/Chassis/{id}/Memory/{mem}/MemoryMetrics` | Memory Metrics |

### Fabric Model

CXL fabric topology (switches, ports, connections, zones).

| Endpoint | Description |
|----------|-------------|
| `GET /redfish/v1/Fabrics` | Fabric Collection |
| `GET /redfish/v1/Fabrics/CXL` | CXL Fabric |
| `GET /redfish/v1/Fabrics/CXL/Switches` | Switch Collection |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}` | CXL Switch |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports` | Port Collection |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}` | Switch Port |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}/Metrics` | Port Metrics |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/VCSs` | Virtual CXL Switch Collection |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/VCSs/{vcs}` | Virtual CXL Switch |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/VCSs/{vcs}/VPPBs` | vPPB Collection |
| `GET /redfish/v1/Fabrics/CXL/Switches/{sw}/VCSs/{vcs}/VPPBs/{vppb}` | Virtual PCI-to-PCI Bridge |
| `GET /redfish/v1/Fabrics/CXL/Endpoints` | Endpoint Collection (SLDs, LDs, MH-MLD heads, hosts) |
| `GET /redfish/v1/Fabrics/CXL/Endpoints/{ep}` | Fabric Endpoint (SLD, LD, MH-MLD head or host) |
| `GET /redfish/v1/Fabrics/CXL/Connections` | Connection Collection |
| `GET /redfish/v1/Fabrics/CXL/Connections/{conn}` | Connection (host-to-device binding) |
| `POST /redfish/v1/Fabrics/CXL/Connections` | Create Connection (binds memory to host) |
| `DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}` | Delete Connection (unbinds memory) |
| `POST /redfish/v1/Fabrics/CXL/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` | Force the unbind (Simulate Surprise Hot-Remove) |
| `GET /redfish/v1/Fabrics/CXL/Zones` | Zone Collection |
| `GET /redfish/v1/Fabrics/CXL/Zones/{zone}` | Zone (VCS access domain) |

### Endpoints

Fabric Endpoints represent entities that participate in CXL communication. There are two types:

**Target Endpoints (Memory Devices)**

A host is bound to an SLD whole and to an MLD one LD at a time ("If the
bind target is a physical port connected to an MLD, then a corresponding
LD-ID must also be specified", CXL r4.0 Section 7.6.7.2.2). A target
Endpoint is what a binding reaches, so there is one Endpoint per logical
device. That is cxl-fmd's choice: Endpoint_v1 allows it and does not
require it.

- An SLD: Id the device's ID (unchanged)
- An LD of an MLD, or a pool LD of a Multi-Headed device: Id the LD's,
  `{dev}_ld{N}`, `N` its LD-ID or pool index. An MLD or a Multi-Headed
  device has no Endpoint of its own
- A head of a Multi-Headed device that presents an MLD: Id
  `{mhd}_head{H}`, `EntityType` `Memory`, no `EntityLink`. Its LDs are
  bound by an LD-ID "from 0 to n-1 relative to that head", which "The
  MH-MLD maps ... to the device-wide LD index in the MH-MLD's LD pool"
  (Section 2.5.1) by no rule the FM is told: a binding of one of them is
  shown to reach the head, not a pool LD
- `EntityType`: `CXLDevice` for an SLD or an LD, "the entity this
  endpoint represents is a CXL logical device" (Endpoint_v1), with
  `EntityLink` to its `CXLLogicalDevices/{N}` (`0` for an SLD), the type
  Endpoint_v1 says it should link to. DMTF names no EntityType for a CXL
  Type 3 target: this is cxl-fmd's choice
- `EntityRole`: `Target` (responds to memory requests)
- `EntityPciId`: Contains VendorId/DeviceId from device
- `Links.ConnectedPorts`: Switch port the device connects to (if
  switch-attached); for a pool LD, the port of the head it is mapped to
- `Links.Zones`: each VCS a vPPB of which is bound to this Endpoint
- `Links.Connections`: each Connection whose `TargetEndpoints` has this
  Endpoint

What a vPPB is bound to: an SLD for Binding Status 02h on its port, the LD
with the Bound LD ID for 03h on an MLD's port, the pool LD mapped to the
head for 02h on the port of a head that presents an SLD, and the head for
03h on the port of a head that presents an MLD. The vPPB's
`Links.Endpoints` is that Endpoint; a Port's `Links.AssociatedEndpoints`
is each target Endpoint at the other end of its link; a
`CXLLogicalDevice`'s `Links.Endpoints` is its own Endpoint; a
`VirtualCXLSwitch`'s `Links.Endpoints` is its Zone's.

`POST /redfish/v1/Fabrics/CXL/Connections` takes an LD Endpoint as the
target, with or without chunks of that LD (400 for a chunk on another LD),
and a head Endpoint is refused with 400. For compatibility, the ID of an
MLD or of a Multi-Headed device, which have no Endpoint any more, is still
accepted as the target, with `MemoryChunkInfo` naming chunks of one LD,
which is then the target.

**Initiator Endpoints (Hosts)**
- Represent host systems that issue CXL.mem requests
- `EntityType`: `RootComplex`, with `EntityLink` to the host's
  `/redfish/v1/Systems/{host}`: Endpoint_v1 says the EntityLink of a
  RootComplex "should be of type `ComputerSystem`". What the FM knows of a
  host is the CXL root port its Virtual Hierarchy starts at, "Everything
  from the CXL RP down" (CXL r4.0 Table 1-1, VH). DMTF names no EntityType
  for a CXL host: this is cxl-fmd's choice. Not `Processor`: cxl-fmd serves
  no Processor resource
- `EntityRole`: `Initiator` (issues memory requests)
- `Links.Zones`: VCS the host connects through (if switch-attached)
- `Links.Connections`: each Connection of the host

Example host endpoint:
```json
{
    "@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0",
    "EndpointProtocol": "CXL",
    "ConnectedEntities": [{
        "EntityType": "RootComplex",
        "EntityRole": "Initiator",
        "EntityLink": {"@odata.id": "/redfish/v1/Systems/cxl_switch_0_host0"}
    }],
    "Links": {
        "Zones": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0"}]
    }
}
```

### Zones

Zones represent Virtual CXL Switches (VCS) as access domains. Each Zone contains all endpoints that participate in that VCS - both the host (initiator) and the devices (targets) bound via vPPBs.

- Zone ID format: `{switch_id}_vcs{N}` (e.g., `cxl_switch_0_vcs0`)
- `Links.Endpoints`: Array of all endpoints in this VCS:
  - Host endpoints (EntityRole=Initiator) connected via the VCS USP
  - Target endpoints (EntityRole=Target) bound to the VCS via vPPBs, each
    once: an MLD whose LD 0 is bound to VCS 0 and LD 1 to VCS 1 is its
    `{mld}_ld0` Endpoint in the one Zone and `{mld}_ld1` in the other
- Bidirectional links: Both hosts and devices include `Links.Zones` back-references

### Actions

| Endpoint | Method | Description |
|----------|--------|-------------|
| `.../Ports/{port}/Actions/Port.ResetPPB` | POST | Reset Physical Port PPB (offered on a DSP only) |
| `.../Memory/{mem}/Actions/Memory.ScanMedia` | POST | Scan media for errors |
| `.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.SetPassphrase` | POST | Set the user or master passphrase (see "Security actions") |
| `.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DisablePassphrase` | POST | Disable the user or master passphrase |
| `.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.Unlock` | POST | Unlock with the user passphrase |
| `.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.FreezeSecurityState` | POST | Freeze the security state |
| `.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.PassphraseSecureErase` | POST | Passphrase secure erase |
| `.../Memory/{mem}/Actions/Memory.SetPassphrase` | POST | Set user passphrase (deprecated alias) |
| `.../Memory/{mem}/Actions/Memory.SetMasterPassphrase` | POST | Set master passphrase (deprecated alias) |
| `.../Memory/{mem}/Actions/Memory.DisablePassphrase` | POST | Disable user passphrase (deprecated alias) |
| `.../Memory/{mem}/Actions/Memory.DisableMasterPassphrase` | POST | Disable master passphrase (deprecated alias) |
| `.../Memory/{mem}/Actions/Memory.FreezeSecurityState` | POST | Freeze security state (deprecated alias) |
| `.../Memory/{mem}/Actions/Memory.SecureEraseUnit` | POST | Passphrase secure erase (deprecated alias) |
| `/redfish/v1/Fabrics/CXL/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` | POST | Unbind vPPB with Simulate Surprise Hot-Remove (cxl-fmd's Oem action, switch-attached Connections only) |

**Note:** vPPB Bind and Unbind are done through the Connections API, a
`POST` binding and a `DELETE` unbinding (see "Memory Binding
(Connections)" below). A vPPB has no action, and a `PATCH` of its
`Links.Port` is not served.

---

## Memory Provisioning

Memory provisioning for CXL devices is performed through the MemoryChunks collection.

**Provisioning Model:** During discovery, cxl-fmd queries existing LD allocations from the device and creates MemoryChunks for already-allocated memory. The remaining (unallocated) capacity is tracked in the MemoryDomain but not represented as chunks until explicitly provisioned via POST. DELETE returns capacity back to the available pool.

**Domain size of an MLD or Multi-Headed device:** the domain's `MemorySizeMiB` is the Memory Size of Get LD Info (5400h), "Total device memory capacity" (CXL r4.0 Table 7-46), and what is left for a `POST` is that less the allocations of the LDs (Get LD Allocations, 5401h). Table 7-46 gives the field no unit: cxl-fmd reads it as bytes. A Multi-Headed device is sent Get LD Info on its LD Pool CCI, which need not support it (only the Multi-Headed command set "is required on the LD Pool CCI", Section 7.6.7.5); if it does not answer, its domain is the sum of the allocations of its pool LDs, with nothing left (cxl-fmd's choice). A device that reports more allocated than its Memory Size has a domain as large as its allocations, with nothing left, and a warning is logged (cxl-fmd's choice). A device with nothing allocated has a domain, with no chunks; an LD with nothing allocated has no chunk. There is no property for what is left: a `POST` larger than it is `409`.

**Allocations made elsewhere:** CXL has no notification of a change to an LD allocation, so cxl-fmd reads them again (cxl-fmd's choice of when): on the `--rescan` timer, Get LD Info and Get LD Allocations for every MLD and Multi-Headed device found before (a Memory Size that cannot be read is kept, and allocations that cannot be read change nothing); and before a chunk `POST` or `DELETE`, Get LD Allocations for the chunk's LD, whose Range 2 and the Memory Granularity the Set LD Allocations is then built from. Other LDs, and the Memory Size, are read on the timer only: a `POST` the domain has room for that the device has not is refused by the device, as Set LD Allocations "will fail if the device fails to allocate any of the allocations defined in the request" (CXL r4.0 Section 7.6.7.4.3), and is `500`. The chunks of each LD are then made to add up to its allocation (cxl-fmd's choice): what an LD gained is a chunk of its own, `{dev}_ld{N}_chunk` if no chunk has that ID or else `{domain}_chunk{M}`, at the lowest offset where it fits (in pieces if no free range is that large), and what it lost comes off its chunks, the newest first, a chunk left with nothing going. A `DELETE` of a chunk that went that way is `404`. A domain whose Memory Size is not known keeps the capacity it had, raised if the allocations are larger.

### Creating Memory Chunks (POST)

**Basic request:**
```http
POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
Content-Type: application/json

{
    "MemoryChunkSizeMiB": 1024,
    "AddressRangeType": "Volatile"
}
```

**On an MLD or a Multi-Headed device**, the chunk names its LD:
```http
POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
Content-Type: application/json

{
    "InterleaveSets": [{
        "Memory": {"@odata.id": "/redfish/v1/Chassis/{id}/Memory/{id}_ld{N}"},
        "SizeMiB": 1024
    }]
}
```
or
```http
{
    "MemoryChunkSizeMiB": 1024,
    "Links": {"CXLLogicalDevices": [
        {"@odata.id": "/redfish/v1/Chassis/{id}/PCIeDevices/{id}/CXLLogicalDevices/{N}"}
    ]}
}
```

**Request Body:**
| Property | Type | Required | Description |
|----------|------|:--------:|-------------|
| `MemoryChunkSizeMiB` | integer | Conditional | Size of the chunk in MiB, greater than 0 (required unless InterleaveSets[0].SizeMiB provided; if both are, they are equal) |
| `AddressRangeType` | string | No | `"Volatile"` or `"PMEM"`; any other value is `400`. The device or LD has to have memory of that type, or partitionable capacity (Total Capacity less Volatile Only and Persistent Only, with a Partition Alignment) that could be, else `400`; an LD that did not answer Identify Memory Device takes any. Left out: the type all of its memory is now (see below); on an SLD whose memory is of both types, or of none known, `400`; on an LD, the chunk has none (`null`) |
| `AddressRangeOffsetMiB` | integer | No | Offset of the chunk in its domain, in MiB: from 0 to the domain's `MemorySizeMiB` less `MemoryChunkSizeMiB`. If omitted, the lowest offset where the chunk fits (first fit) |
| `InterleaveSets` | array | No | One entry at most (see below) |
| `Links.CXLLogicalDevices` | array | No | One entry at most: the LD, by the URI of its `CXLLogicalDevice` |

**InterleaveSets Entry:**
| Property | Type | Description |
|----------|------|-------------|
| `Memory` | object | The LD, by the URI of its `Memory` (`{"@odata.id": "/redfish/v1/Chassis/{id}/Memory/{id}_ld{N}"}`, or `.../Memory/{id}` for an SLD) |
| `SizeMiB` | integer | The chunk's size in MiB: `MemoryChunkSizeMiB`, if that is given too |
| `MemoryLevel` | integer | 1, or absent |
| `OffsetMiB` | integer | 0, or absent |
| `RegionId` | string | Absent (or empty) |

A chunk of an MLD, or of a Multi-Headed device's pool, grows the
allocation of one LD (a pool LD for an MHD), and the request names it:
`InterleaveSets[0].Memory` as the URI of its `Memory`,
`Links.CXLLogicalDevices[0]` as the URI of its `CXLLogicalDevice`, or both
if they name the same LD; exactly as cxl-fmd serves them. A request that
names none, an LD the device does not have, another device's, or several,
or that names one in another form (an LD's component ID, a URI with a
trailing `/`, a path that ends in the LD's ID), is `400`. Set LD
Allocations (5402h) gives an LD a "Range 1 Allocation Multiplier" and a
"Range 2 Allocation Multiplier" (CXL r4.0 Table 7-49) and nothing else, so
a request with more than one `InterleaveSets` entry, an `OffsetMiB` other
than 0, a `MemoryLevel` other than 1, a `RegionId`, or a `SizeMiB` other
than `MemoryChunkSizeMiB` is `400` too: an allocation cannot be any of
these. An SLD's chunk is of its one LD; it may name it, and nothing else.
The new size of the LD is a multiple of the domain's
`MemoryChunkIncrementMiB` (the Memory Granularity, Table 7-48), or the
request is `400`: checked before anything is made where cxl-fmd has the
granularity (an MLD; a mock MHD), and on a hardware MHD, whose
granularity discovery does not record, on Get LD Allocations before Set
LD Allocations is sent.

**Response:** Returns the created MemoryChunk resource with HTTP 201 Created.

Every size and offset in MiB, `InterleaveSets` entries included, is an
integer from 0 to 17592186044415 (2^44 - 1, the most MiB a byte count of
64 bits holds); one that is not, is negative or is past it is `400`, as is
a chunk that would not lie within its domain. `AddressRangeOffsetMiB` is
cxl-fmd's coordinate within the domain: DSP0288 maps no CXL field to it,
and no CXL command places an allocation at an offset, so it is never sent
to a device. Nothing is changed or sent for a request refused with `400`.

For a chunk of an MLD or MHD, the response has `Links.CXLLogicalDevices`,
the LD it grows, and one `InterleaveSets` entry, that LD's `Memory` with
the chunk's `SizeMiB`; `GET` of the chunk has the same link, and the LD's
`Links.MemoryChunks` lists the chunk.

**Hardware Behavior:**
- **MLD**: Issues `Set LD Allocations (0x5402h)` to increase the LD's memory allocation.
  The LD's Range 2 is kept as `Get LD Allocations (0x5401h)` reads it just
  before, and Range 1 takes the rest. The new size must be a multiple of
  `MemoryChunkIncrementMiB` (the Memory Granularity, Table 7-48); one that
  is not is refused with 400, as is one smaller than Range 2. The same
  holds for an MHD's pool LDs.
- **SLD**: A `"Volatile"` chunk of an SLD with partitionable capacity
  issues `Set Partition Info (0x4101h)` for the volatile capacity staged
  (or else active) plus the chunk, worked out and checked as in "Set
  Partition Info" above. Immediate is cleared: the device takes the new
  partitioning at its next reset, and the chunk is `StandbyOffline` until
  it does (see "Staged chunks"). Persistent data is lost if the persistent
  capacity changes. A `"PMEM"` chunk sends no command: it only divides
  cxl-fmd's own record of the domain. Nor does a `"Volatile"` chunk of an
  SLD with no partitionable capacity (Partition Alignment 0, or no
  capacity beyond Volatile Only and Persistent Only, Table 8-309), on
  which Set Partition Info "shall fail with an Unsupported error"
  (Section 8.2.10.9.2.2): the chunk is of the Volatile Only capacity,
  and taking them as a division of the domain is cxl-fmd's choice.

### Deleting Memory Chunks (DELETE)

```http
DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
```

**Response:** HTTP 204 No Content on success.

**Hardware Behavior:**
- **MLD**: Issues `Set LD Allocations (0x5402h)` to decrease the allocation
  of the chunk's LD by the chunk's `MemoryChunkSizeMiB`, what its `POST`
  added. The domain gets back what the LD gave back, as the response
  gives the LD's allocation ("The allocations provided in the response
  reflect the state of the LD allocations after the command is
  processed", CXL r4.0 Section 7.6.7.4.3): if that is less than the
  chunk, the chunk stays, with what was not given back, and the request
  is `500`. An LD with less allocated than its chunk is `409`, with
  nothing sent.
- **MHD**: the same, for a pool LD, on the LD Pool CCI (see "MHD Memory
  Model"); `501`, with nothing sent, when no LD Pool CCI is known.

**An unclear Set LD Allocations** (chunk `POST` and `DELETE` of an MLD or
MHD). Invalid Input, Unsupported, Retry Required, Invalid Payload Length
and Unsupported Mailbox or CCI are taken to mean the command was not
carried out: `500`, and the chunk is as it was. Any other outcome (another
return code such as Internal Error, a response libcxlmi cannot read, which
it reports as it reports a message not sent or not answered, or a Success
for another LD) does not say what the LD has, and Get LD Allocations is
sent to read it back (cxl-fmd's choice: the specification says only that a
response's allocations "reflect the state of the LD allocations after the
command is processed", Section 7.6.7.4.3). What it reads decides: the size
asked for is `201` or `204`; the size the LD had is `500` with the chunk as
it was; another size is `500` with the chunks of the LD made to add up to
it. If the read back fails as well the outcome is not known: `500` with a
message that says so, the chunk is kept (a `POST`'s counted as allocated),
and the LD is fenced: the next chunk `POST` or `DELETE` of the device reads
that LD first, and is `500` with nothing sent while it cannot be read. The
`--rescan` timer reads it too. Fencing is kept in memory only.

In mock mode (`--mock`) nothing is sent, and a chunk `POST` or `DELETE` of
an MLD or MHD changes the mock LD's `MemorySizeMiB` as Set LD Allocations
would a device's; a `"Volatile"` chunk of an SLD does not change the
mock's partition.
- **SLD**: A `"Volatile"` chunk of an SLD with partitionable capacity
  issues `Set Partition Info (0x4101h)` for the volatile capacity staged
  (or else active) less the chunk, staged for the next reset as on
  creation; a partition the device cannot be given is `400`, and the
  chunk stays. A `"PMEM"` chunk, and a `"Volatile"` one of an SLD with
  no partitionable capacity, send no command.

**While bound (409 Conflict):** a chunk that a switch-attached Connection
exposes (one that is not Absent: Starting, Updating and Enabled ones
included) is not deleted, and no chunk is created that would change the
allocation of a bound LD (Set LD Allocations) or, `"Volatile"`, the
partition of a bound SLD with partitionable capacity (Set Partition
Info). Delete the Connection
first. The CXL specification does not say whether an LD or SLD that is
bound may be changed; refusing it is cxl-fmd's choice. Direct-attached
Connections, which have no binding to undo, do not hold their chunks.

The exception is a pool LD of a Multi-Headed device: no chunk of it is
deleted or created while the head the LD Map maps it to (Table 7-61) has
any Connection that is not Absent: a direct-attached host, or a vPPB
binding, `Unresolved` ones included. Which LD-ID of a head that presents
an MLD a pool LD is, is not known ("The MH-MLD maps the LD-IDs received
at a head to the device-wide LD index in the MH-MLD's LD pool", Section
2.5.1, by no rule the FM is told), so every pool LD mapped to a head that
has a host or a binding is taken to be in use (cxl-fmd's choice). A pool
LD mapped to no head can be changed.

### MLD Memory Model

For Multi-Logical Devices (MLDs), the memory hierarchy is:
- **MLD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → One domain per MLD, represents the device's total capacity
- **Memory Chunk** → Part of the allocation of one LD: discovery makes one
  per LD, its allocation, and each `POST` another on the LD it names
- **Memory** → Individual LDs as memory resources

What a chunk is, is cxl-fmd's choice: DSP0288 maps Get LD Allocations to
`MemoryChunkIncrementMiB` and an LD's `MemorySizeMiB` (Table 2), and no
CXL command to a chunk `POST`. In cxl-fmd:
- a chunk `POST` grows the allocation of the one LD it names (Set LD
  Allocations), and a `DELETE` shrinks it by the chunk's size; an LD can
  have several chunks;
- a chunk is not a unit of access: a Connection binds a whole LD, and
  exposes every chunk of it (its `MemoryChunkInfo`);
- the chunk of an SLD is cxl-fmd's own division of the device: a `"PMEM"`
  one sends nothing, a reservation in cxl-fmd's record only.

Chunk IDs:

- the chunk of an LD, found at discovery: `{device}_ld{N}_chunk` (e.g.,
  `cxl_mld_0_ld0_chunk`), for an MLD and for a pool LD of a Multi-Headed
  device;
- the chunk of an SLD, found at discovery: `{device}_chunk0`;
- a chunk made with `POST`: `{domain_id}_chunk{N}`, N from a counter of
  the domain that starts at 0 and only goes up: an ID a deleted chunk had
  is not given to another while cxl-fmd runs, and a POST never takes an
  ID some component has. It starts again at 0 when cxl-fmd starts, or
  when the device is discovered again (cxl-fmd's choice: chunk IDs are
  not kept);
- in the `{switch}_pool` domain of a switch's Chassis: `{device}_chunk` for
  an SLD on one of its ports, `{device}_ld{N}_chunk` for an LD of an MLD.

### MHD Memory Model

For Multi-Headed Devices (MHDs), the memory hierarchy is:
- **MHD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → One domain covering the entire LD Pool
- **Memory Chunk** → Part of the allocation of one pool LD, as for an MLD:
  discovery makes one per pool LD, and each `POST` another on the pool LD
  it names
- **Memory** → Individual pool LDs as memory resources
- **CXLLogicalDevices** → Pool LDs indexed by pool index (0 to total_lds-1)

Key differences from MLD:
- LD Pool can have **more than 16 LDs** (unlike MLD's 16 max)
- Each head sees **head-local LD-IDs** (0 to n-1), which map to pool indices
- FM uses **pool indices** in Redfish (not head-local IDs)
- A head's Connection reaches only the pool LDs mapped to it (the LD Map,
  Table 7-61): a `POST` naming a pool LD that is not mapped to the host's
  head is 400

**LD-ID Translation:**
```
Host 0 connects to Head 0: sees LDs 0,1,2 → maps to pool LDs 2,3,4
Host 1 connects to Head 1: sees LDs 0,1   → maps to pool LDs 0,5
FM Redfish view: all pool LDs 0-5 visible as CXLLogicalDevices/0 through /5
```

**Hardware Behavior:**
- POST uses LD Pool CCI `Set LD Allocations (0x5402h)` with pool index,
  preceded by `Get LD Allocations (0x5401h)`. The LD Pool CCI "may be
  exposed as an MCTP-based CCI or can be accessed via the Tunnel
  Management Command command through a head's Mailbox CCI" (CXL r4.0
  Section 2.5). cxl-fmd takes the endpoint that answered `Get
  Multi-Headed Info (0x5500h)` with no tunnel, where it discovered the
  device, to be the LD Pool CCI (its inference: the command set "is
  required on the LD Pool CCI", Section 7.6.7.5, which does not say only
  there), and sends both there with no tunnel. A device for which no LD
  Pool CCI is known is sent nothing, and the request is `501`.
- A head with one pool LD on a switch port is bound whole; a head that
  presents an MLD is not bound (see "Memory Binding (Connections)")

**One device on several CCIs:** a device can answer on more than one CCI,
and the serial number of Identify is the "unique identifier for this
device" (Table 8-216). A Multi-Headed device's "LD Pool CCI may be exposed
as an MCTP-based CCI or can be accessed via the Tunnel Management Command
command through a head's Mailbox CCI", and "The FMLD within each head of an
MH-MLD shall expose and manage only the LDs that are mapped to that head"
(Sections 2.5 and 2.5.1). cxl-fmd keeps a Multi-Headed device as one
device, with each CCI it was found on as a route of a role (cxl-fmd's
inferences, as no command says what a CCI is):

- an endpoint that answers Get Multi-Headed Info with no tunnel, and more
  than one head, is the LD Pool CCI; the device is read there, and the
  commands for its pool go there;
- another endpoint with the serial number that answers Get LD Info, or an
  MLD on a switch port whose Identify through the switch gives it, is the
  FM-owned LD of a head that presents an MLD; one that answers neither is
  a CCI of the device whose role is not known. Neither is sent commands
  for the pool. A head's FM-owned LD is taken to be of the device's only
  head that presents an MLD, has its link up and is placed nowhere else;
  with two or more such heads, it is of none. On a switch port, that head
  is placed on the port, until the port is no longer a DSP or reports
  another kind of device, or none: the head is then on no port, and the
  route through the port is dropped.

A device found first on a head (as an SLD or MLD, directly attached or on
a switch port; switches are discovered first) is replaced by the
Multi-Headed device when its LD Pool CCI is found, with new IDs (see
"Changes for Clients"). An endpoint whose answer is a temporary error is
asked again later, as in discovery. A device found on two CCIs that is not
Multi-Headed stays as it was first found.

### SLD Memory Model

For Single Logical Devices (SLDs):
- **SLD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → Represents the device's total capacity
- **Memory Chunk** → Volatile and/or persistent partitions
- **Memory** → Single memory resource for the SLD

---

## Memory Binding (Connections)

A host gets memory on a switch when a vPPB of its VCS is bound to the
memory device's port, or to an LD of it. cxl-fmd serves each such binding
as a `Connection`, and binds and unbinds vPPBs when a client creates and
deletes Connections. DSP0288 maps neither (see "cxl-fmd mapping (not in
DSP0288)"); what follows is cxl-fmd's design, with the CXL text it rests
on quoted.

### What a Connection is

A switch-attached Connection is a vPPB binding, as last read with Get
Virtual CXL Switch Info (5200h): the FM "issues Get Virtual CXL Switch Info
for each enabled VCS to check for all bound vPPBs in the system and create
a list of binding targets" (CXL r4.0 Section 7.6.6.2). No Connection is
stored. The list is computed whenever it is served, from the switch's VCS
tables and from the binds and unbinds cxl-fmd has under way, so the
Connections and the bindings cannot disagree, except by being as old as
the last read (see "When the switch is read").

Nothing the switch reports says who made a binding: no field of Get
Virtual CXL Switch Info (Table 7-34) or of a Binding Change (Table 7-80)
records it. A binding made by cxl-fmd, by the switch's initial
configuration, by another Fabric Manager, or through another CCI of the
switch, is a Connection all the same, and a `DELETE` unbinds it.

A vPPB gives a Connection when all of these hold (cxl-fmd's choice):

- its VCS's VCS State is Enabled: the host is known only by the USP ID,
  "Valid only when the VCS is enabled" (Table 7-34);
- it is not vPPB 0: "vPPBs within a VCS are ordered in the following
  sequence: the USP vPPB, then the DSP vPPBs" (Section 7.1.4);
- its Binding Status is 02h (Bound Physical Port) or 03h (Bound LD), or a
  bind cxl-fmd started names it;
- the bound port has a memory device (an SLD, an MLD, or a head of a
  Multi-Headed device), or no device, or one cxl-fmd never discovered.

A port with a PCIe, Type 1 or Type 2 device, or a downstream switch, gives
none, and neither does a vPPB with Binding Status 04h (Bound PID). Those
are seen only on the vPPB, in `BindingStatus`.

A direct-attached Connection has no vPPB: there is no VCS, and no bind,
on a link from a host's root port to a device. That the link is the
Connection is cxl-fmd's choice. It derives one, always `Enabled`, with
`Oem.Cxl.Origin` saying where it comes from, for:

- the only direct-attached host (the `local` host of a topology with no
  switch) with each SLD and MLD that is on no switch port; for an MLD, its
  `TargetEndpoints` are all of its LDs. What an MLD on a root port gives
  its host, the specification does not say. With more than one
  direct-attached host, nothing says whose link a device is on, and there
  is none. Such a Connection is `Inferred`, with `Health` `Warning`: that
  a device answers on a management interface such as MCTP says nothing of
  which root port, if any, its CXL.mem link is on, and no CXL command
  reports that. It is still `Enabled`, so that the host has its
  `MemoryDomain`. A device reached through the mailbox of the kernel
  cxl-fmd runs on (`-i`) is on that host's hierarchy, but cxl-fmd does
  not tell it apart, and marks it `Inferred` too (cxl-fmd's choice);
- each head of a Multi-Headed device that is on no switch port and whose
  host is known, with the pool LDs that the LD Map (Table 7-61) maps to
  that head. Only a mock topology says which host a head is attached to
  (`attached_host`): no command maps a head to a host. Such a Connection
  is `Configured`, `OK`: the topology names the host.

#### Connection IDs

The ID is cxl-fmd's choice, and is the same after a restart while the
binding is:

| Connection | ID |
|------------|----|
| A vPPB bound to a port (02h) | `{switch}_vcs{V}_vppb{P}_p{N}` |
| A vPPB bound to an LD (03h) | `{switch}_vcs{V}_vppb{P}_p{N}_ld{L}` |
| A direct-attached SLD or MLD | `{host}_direct_{device}` |
| A direct-attached head of a Multi-Headed device | `{host}_direct_{mhd}_h{H}` |

`V`, `P`, `N` and `L` are the VCS ID, vPPB ID, Physical Port ID and LD-ID
in decimal. The vPPB is in the ID, so that a `DELETE` of an ID whose vPPB
now holds something else finds nothing (404) instead of unbinding the later
binding, and so that one LD shown on two vPPBs, which the specification
does not forbid, gives two Connections. A rebind of the same target to
another vPPB is another ID; a `POST` of that target finds it.

#### Properties

```json
{
    "@odata.id": "/redfish/v1/Fabrics/CXL/Connections/cxl_switch_0_vcs0_vppb1_p2_ld0",
    "@odata.type": "#Connection.v1_4_0.Connection",
    "Id": "cxl_switch_0_vcs0_vppb1_p2_ld0",
    "Name": "CXL vPPB Binding",
    "ConnectionType": "Memory",
    "Status": {"State": "Enabled", "Health": "OK"},
    "Links": {
        "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0"}],
        "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_mld_0_ld0"}]
    },
    "MemoryChunkInfo": [{
        "MemoryChunk": {"@odata.id": "/redfish/v1/Chassis/cxl_mld_0/MemoryDomains/cxl_mld_0_domain0/MemoryChunks/cxl_mld_0_ld0_chunk"},
        "AccessCapabilities": ["Read", "Write"],
        "AccessState": "Optimized"
    }],
    "Oem": {"Cxl": {"@odata.type": "#Cxl.v1_0_0.Connection", "Origin": "Discovered"}},
    "Actions": {
        "Oem": {
            "#Cxl.ForceUnbind": {
                "target": "/redfish/v1/Fabrics/CXL/Connections/cxl_switch_0_vcs0_vppb1_p2_ld0/Actions/Oem/Cxl.ForceUnbind"
            }
        }
    }
}
```

- `Name`: `CXL vPPB Binding`, or `CXL Direct-Attached Connection`
- `Links.InitiatorEndpoints`: the host's Endpoint
- `Links.TargetEndpoints`: what the binding reaches: the SLD, the LD of an
  MLD, the pool LD mapped to a head that presents an SLD, or the head
  Endpoint (`{mhd}_head{H}`) for an LD of a head that presents an MLD (see
  "Endpoints"). Left out while `Absent`
- `MemoryChunkInfo`: one entry per chunk of the target: every chunk of an
  SLD, each chunk on the LD of an LD. A binding exposes the whole of what
  it reaches, and Bind vPPB has no field for how a host may use it (Table
  7-35): `AccessCapabilities` is always `Read` and `Write`. `AccessState`
  is `Optimized` once it is bound (`Enabled`), and `Transitioning` while a
  bind or unbind of it is under way (`Starting`, `Updating`): cxl-fmd's
  choice of Connection_v1's values. Left out while `Absent`, and when the
  target is not known
- `Oem.Cxl.Origin`: of a switch-attached one, `Service` or `Discovered`
  (see "Forcing an Unbind"); of a direct-attached one, `Inferred` or
  `Configured` (see above). cxl-fmd's Oem
- `Actions.Oem["#Cxl.ForceUnbind"]` (switch-attached only): see "Forcing
  an Unbind"

`Status` uses the standard `State` values; what each means here is
cxl-fmd's choice:

| `State` | `Health` | When |
|---------|----------|------|
| `Starting` | `OK` | cxl-fmd's bind of it is under way |
| `Updating` | `OK` | cxl-fmd's unbind of it is under way |
| `Enabled` | `OK` | bound, and what it reaches is known; a direct-attached Connection that is `Configured` |
| `Enabled` | `Warning` | bound, and what it reaches is not known: an LD of a head that presents an MLD (see "Endpoints"), or an LD-ID the MLD does not have; a direct-attached Connection that is `Inferred` |
| `Absent` | `OK` | bound, and no device is on the port (see "When a Device Is Removed") |

A `PATCH` of a Connection is answered `405 Method Not Allowed`, `Allow:
GET, DELETE` (`Allow: GET` for a direct-attached one): everything in it
is what the switch says. Connection_v1
marks the resource updatable; not taking a `PATCH` is cxl-fmd's choice.

#### When the switch is read

A `GET` sends no command: a Connection is as old as the last read of the
switch's VCSs (cxl-fmd's choice). They are read:

- when the switch is discovered;
- every VCS of the switch, before each `POST` and `DELETE` of a
  Connection and each `#Cxl.ForceUnbind`, as another FM, or a request
  through another CCI, may have changed one;
- after a Binding Change (Virtual CXL Switch Event Record, Event Type 00h)
  that no task of cxl-fmd follows, once per poll of the switch's logs, all
  of its VCSs. The record's own binding fields are not taken as they are
  (cxl-fmd's choice);
- after the switch's event log overflowed;
- on the `--rescan` timer, which stays the backstop: whether a switch
  records binds made by another FM or through another CCI in logs this FM
  reads, the specification does not say;
- in mock mode, when a background bind or unbind is over.

### Creating Connections (POST)

```http
POST /redfish/v1/Fabrics/CXL/Connections
Content-Type: application/json

{
    "ConnectionType": "Memory",
    "Links": {
        "InitiatorEndpoints": [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/{host_id}"}
        ],
        "TargetEndpoints": [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/{endpoint_id}"}
        ]
    },
    "MemoryChunkInfo": [{
        "MemoryChunk": {
            "@odata.id": "/redfish/v1/Chassis/{dev}/MemoryDomains/{dom}/MemoryChunks/{chunk}"
        },
        "AccessCapabilities": ["Read", "Write"]
    }]
}
```

**Request Body:**
| Property | Type | Required | Description |
|----------|------|:--------:|-------------|
| `ConnectionType` | string | No | Not read; the Connection is `"Memory"` |
| `Links.InitiatorEndpoints` | array | Yes | Exactly one: the host's Endpoint |
| `Links.TargetEndpoints` | array | Yes | Exactly one: an SLD's Endpoint, or an LD's (`{dev}_ld{N}`). Accepted for compatibility: `/redfish/v1/Fabrics/CXL/Endpoints/{id}` with the ID of an MLD or a Multi-Headed device, which have no Endpoint, with `MemoryChunkInfo` naming the LD |
| `Links.InitiatorEndpointGroups`, `Links.TargetEndpointGroups` | array | No | Not supported: a body with either is 400 |
| `MemoryChunkInfo` | array | With an MLD or Multi-Headed device's ID | Chunks of the target; with an SLD or LD Endpoint it can be left out. It does not choose what is bound: a binding gives the whole SLD or LD |
| `MemoryChunkInfo[].MemoryChunk` | object | Yes, in each entry | Reference to the MemoryChunk |
| `MemoryChunkInfo[].AccessCapabilities` | array | No | `["Read", "Write"]` only: anything else is 400 |
| `MemoryChunkInfo[].AccessState` | string | No | `"Optimized"` only: anything else is 400 (see below) |
| `VolumeInfo` | array | No | Not supported: a body with it, even `[]`, is 400 (see below) |

`AccessState` is ReadWrite in Connection_v1, one of `Optimized` ("The
resource is in an active and optimized state"), `NonOptimized`,
`Standby`, `Unavailable` and `Transitioning` ("The resource is
transitioning to a new state"). A Connection cxl-fmd makes is `Optimized`
once it is bound and `Enabled`, which is where a `POST` that succeeds
ends (`201`; `202`, then `Enabled`; `200` of an `Enabled` one; an `Absent`
one lists no chunk), and Bind vPPB has no field for any other (Table 7-35); `Transitioning` is what the service serves
while a bind runs, not something to ask for. So `Optimized` is taken and
any other value is `400` (cxl-fmd's choice, rather than ignoring it).

`VolumeInfo` has no `OData.Permissions` annotation in Connection_v1 (where
`ConnectionType`'s is `Read`), so it is not one of the "service-controlled
properties" a create "may ignore" (DSP0266 Section 7.9). Of each entry's
`Volume`, "The endpoints referenced by the `InitiatorEndpoints` or
`InitiatorEndpointGroups` properties shall be given access to this volume"
(Connection_v1). cxl-fmd serves no `Volume`, and a binding gives memory, so
a body with `VolumeInfo` is `400`, even an empty one (cxl-fmd's choice).
Both are checked before anything is read from the switch: a `POST` of a
target the host has already, or of a direct-attached pair, is `400` too,
not `200`.

Every `@odata.id` is the URI the service serves the resource at: an
Endpoint is `/redfish/v1/Fabrics/CXL/Endpoints/{id}`, and a chunk
`/redfish/v1/Chassis/{device}/MemoryDomains/{domain}/MemoryChunks/{chunk}`,
its own device and domain. Any other form (another path ending in the same
ID, a relative URI, a trailing `/`) is 400, and so is the URI of the same
chunk in a host's view, `/redfish/v1/Systems/{host}/MemoryDomains/{dom}/MemoryChunks/{chunk}`
(cxl-fmd's choice: the Chassis URI is the chunk's own). An Endpoint or
chunk URI of that form whose ID nothing has is 404. `InitiatorEndpoints` "shall not be present if
`InitiatorEndpointGroups` is present", and `TargetEndpoints` likewise
(Connection_v1); cxl-fmd serves no `EndpointGroup`, so a body with either
`EndpointGroups` property is 400 (cxl-fmd's choice, rather than ignoring
it).

Each chunk named must be the target's, and the chunks of an MLD or a
Multi-Headed device must all be on one LD (400 otherwise). The target is:

| Target | Bound |
|--------|-------|
| An SLD on a switch port | The port, LD ID FFFFh ("Must be FFFFh for other EP types", Table 7-35) |
| An LD of an MLD on a switch port, or the MLD's ID with chunks of that LD | The port and the LD's LD-ID |
| A pool LD of a Multi-Headed device, or the device's ID with chunks of that LD, when the LD is mapped to a head on a switch port that presents an SLD | That head's port, LD ID FFFFh: "A head with one LD mapped shall present itself as an SLD" (CXL r4.0 Section 2.5) |
| A head that presents an MLD, its Endpoint, or a pool LD mapped to it | Nothing: 400. "The MH-MLD maps the LD-IDs received at a head to the device-wide LD index in the MH-MLD's LD pool" (Section 2.5.1) by no rule the FM is told |
| A device on no switch port | Nothing: see "Direct-attached" below |

A host on a switch reaches only the devices of that switch (400 for a
device elsewhere, or for a direct-attached host and a switch-attached
device).

**What a POST of a switch-attached target does.** With `fabric_lock` held
for the whole request, as for every write, in one step:

1. Every VCS of the switch is read (5200h).
2. If the host's VCS has a Connection to the target already, whoever made
   it, the answer is that Connection: `200 OK` with it and `Location`
   when it is `Enabled` or `Absent`; `202 Accepted` with the task of the
   bind when it is `Starting`; `409 Conflict` when it is `Updating`.
   Nothing is sent. That a second bind of a target is not sent is
   cxl-fmd's choice: the specification does not say whether it is an
   error, and lists the return codes of Bind vPPB without conditions.
3. If the target is bound, or being bound or unbound, for another vPPB,
   the answer is `409`. This covers the whole port while an LD of it is
   bound, and an LD while the port is bound whole. An SLD port "is bound
   to only one VCS" (Section 7.2.3.1); that an LD, too, is bound to one
   vPPB only is cxl-fmd's reading.
4. The lowest vPPB above 0 of the host's VCS that reads unbound (00h) and
   has no bind or unbind of cxl-fmd's under way is chosen; if there is
   none, `409`.
5. The bind is recorded as under way, then Bind vPPB (5201h) is sent.

So eight `POST`s of one target send one Bind vPPB, and `POST`s of two
targets never choose the same vPPB.

**Response:**

| Switch's answer | HTTP |
|-----------------|------|
| Success | `201 Created`, the Connection, `Location` |
| Background Command Started | `202 Accepted`, a `Task`, `Location` the task monitor; the Connection is `Starting` meanwhile (see "Binds and unbinds in the background") |
| Busy, or Retry Required twice ("An optional single retry may resolve the issue", Table 8-208), or the vPPB chosen was bound meanwhile | `503` |
| Invalid Input | `400` |
| Unsupported | `501` (Bind vPPB is "O", optional, in Table 7-31) |
| Internal Error, or another code | `500` |

The status codes are cxl-fmd's choice; the errors are listed in "HTTP
Status Codes".

**Direct-attached.** A direct-attached host and device have their
Connection already (see "What a Connection is"): a `POST` of the pair is
answered `200 OK` with it, and sends nothing. A pair that has none, such as
a host and a chunk of a pool LD not mapped to the host's head, is `400`.

### Binds and unbinds in the background

"All binding operations are executed as background commands" (CXL r4.0
Section 7.6.7.2.2). A switch answers Bind vPPB or Unbind vPPB with
Background Command Started and finishes later, one such command at a time.
cxl-fmd does not wait:

| Request | Answer | Then |
|---------|--------|------|
| `POST .../Connections` | `202 Accepted`, a `Task`, `Location:` the task monitor | The Connection is listed, `Starting`, and is in the task's `CreatedResources` while it runs; it is `Enabled` when the bind is over, and gone if the bind failed |
| `DELETE .../Connections/{id}` | `202 Accepted`, a `Task`, `Location:` the task monitor | The Connection stands, `Updating`, until the vPPB reads unbound; then a `GET` of it is `404` |
| `POST` of a target being bound for the host | `202 Accepted` and the same `Task` | |
| Another bind or unbind while the switch is busy | `503 Service Unavailable` | Try again |
| `GET /redfish/v1/TaskService/TaskMonitors/{id}` | `202` and the `Task` while it runs or is `Interrupted`; then `201` and the Connection for a bind, `204` for an unbind, or the error the request would have had (see below) | |

The task is at `/redfish/v1/TaskService/Tasks/{id}`. Its `PercentComplete` is
the Percentage Complete of Background Operation Status (0002h), which the
event loop reads until the operation is over, or a Binding Change for the
vPPB ends it. A bind is `Completed` only when the vPPB, read again, is
bound to the port and LD asked for; an unbind only when it reads unbound.
Otherwise the task ends in `Exception`, with the return code of Background
Operation Status when the switch logged the operation as failed: "Failed
bind and unbind operations generate events to the Warning Event Log"
(Table 7-80). A switch that finishes a bind before it answers is answered
for with `201`, and no task.

Once the task is over, its monitor answers as the request that started it
would have, had the switch answered at once: "the task monitor shall
return: • The appropriate HTTP status code ... • The headers and response
body of the initial operation, as if it had completed synchronously. If
the initial operation fails, the response body shall contain an error
response" (DSP0266 1.25.0 Section 12.2). A task that ended with the
switch's return code is answered as a `POST` (a bind) or `DELETE` (an
unbind) the switch refused with that code at once: Invalid Input `400`,
Unsupported `501`, Busy or Retry Required `503`, any other `500`, with
the same error body. An unbind is answered as its `DELETE`, which the
task's `Payload` names, even when `#Cxl.ForceUnbind` sent it. A task that
failed with no return code (the switch logged the operation failed, or
the vPPB is not what was asked) is `500` with what was found. A task whose
switch is gone is `500` with a message that starts "Outcome unknown":
whether the switch bound or unbound the vPPB before it went is not known.
So is one whose vPPB, read with no word from the switch, shows what was
asked as it did before the command (see below). Which status each of these
gets is cxl-fmd's choice.

The outcome is judged only on a read of the vPPB made after the switch
said the operation was over (cxl-fmd's choice). A vPPB that still reads
01h, "Bind or unbind in progress" (Table 7-34), is still being bound or
unbound, and a VCS that cannot be read (Get Virtual CXL Switch Info, 5200h,
fails) says nothing either way: the task stays `Running` and the switch is
asked again. Success is never taken without a read that shows it; a failure
the switch reported is taken without one. After five such answers in a row
the task is lost (see below).

A Binding Change record is taken as the end of a task only if its vPPB
Binding Status, Port ID and LD ID fit it. Of a Binding Change, 33h "contains
the updated binding state of a vPPB following the binding change"
(Table 7-80), and 34h and 35h likewise, but nothing in a record says which
command it is of: one read while a bind is under way can be that of an
earlier unbind of the same vPPB. So the fields only turn a record away
(cxl-fmd's choice): an Informational one fits only if it shows what the
task asked for (unbound, for an unbind), a Warning one only if it does not,
and neither if it shows 01h. An Informational one that fits is turned
away too when the vPPB read the same before the command, or was not read
then (see below): it can be of an earlier bind or unbind. One turned away
has the VCSs read again, as a record no task follows does, and the task
goes on until Background Operation Status says it is over.

What the vPPB read before the command is recorded with the bind or unbind
when it is recorded as under way, before Bind vPPB or Unbind vPPB is sent:
its Binding Status, and its Bound Port ID and Bound LD ID when they are
valid (Table 7-34), from the read of the VCSs the request starts with
(step 1 of "What a POST of a switch-attached target does"; a `DELETE`
reads them too), or "not read" if that VCS could
not be read. The task carries it. A vPPB that already read what the command
asks for (bound to that port and LD, or unbound) reads the same afterwards
whether or not the switch carried the command out. So where the outcome
rests on the vPPB alone (a Binding Change record, or a lost task, below),
a vPPB that reads what was asked shows it done only if it read otherwise
before (cxl-fmd's reading). Background Operation Status that says the
operation is over with Success is the switch's word, and is taken with a
vPPB that agrees, whatever it read before. A forced unbind sent while an
unbind runs keeps the vPPB as read before the first: it is the same
unbind.

The task of a bind or unbind is made before Bind vPPB or Unbind vPPB is
sent. If it cannot be (no memory), nothing is sent, and the request is
`500`: nothing is left that the switch carries out with no task following
it.

A task is lost when its switch is gone, or does not answer Background
Operation Status five times in a row (or answers five times that the
operation is over with no read of the vPPB to show it). The switch's VCSs
are then read:

- The switch is gone: the task ends in `Exception`, what was under way is
  forgotten, and the Connection is what the vPPB is (none).
- The vPPB reads anything but 01h: the task ends on what it reads, as
  above, but with no return code; its message says it was read from the
  vPPB. If it reads what was asked, and read that before the command too,
  or was not read before it, the task ends in `Exception` instead: whether
  the switch carried the command out is not known, its monitor is `500`
  "Outcome unknown", and the Connection is what the vPPB is.
- The vPPB still reads 01h, or cannot be read: the task is `Interrupted`
  (Task_v1: "the task has been interrupted but is expected to restart and
  is therefore not complete"), with no `EndTime` or `TaskStatus` and a
  message saying why; its monitor still answers `202`. What was under way
  is kept: the Connection stays `Starting` or `Updating`, and a `DELETE`
  of it is `503`. The switch is no longer asked how it went (Return Code
  "Only valid when Percentage Complete = 100", Table 8-217, and the
  switch may run another command since). Each later read of that switch's
  VCSs (after a Binding Change record, on the `--rescan` timer, and before
  each `POST`, `DELETE` and `#Cxl.ForceUnbind`) ends the task on what the
  vPPB then reads, once it no longer reads 01h; a `DELETE` of the
  Connection is that read too. With `--rescan 0`, on a switch whose
  event logs are the host's, only a request makes that read. If the
  switch goes, the task ends in `Exception`.

Which tasks become `Interrupted`, and that a read of the vPPB ends them,
are cxl-fmd's choices. The specification sets no time limit on a bind or
an unbind, and cxl-fmd does not abort one (see "Forcing an Unbind").

### Deleting Connections (DELETE)

```http
DELETE /redfish/v1/Fabrics/CXL/Connections/{conn_id}
```

Every VCS of the switch is read first. Then:

| Case | Answer |
|------|--------|
| The ID is not a Connection now: unknown, or its vPPB holds something else, or nothing | `404`; nothing is sent |
| A bind or unbind of it is under way | `503` |
| A direct-attached Connection | `405`, `Allow: GET`: there is no vPPB to unbind (cxl-fmd's choice) |
| Otherwise | Unbind vPPB (5202h) with Unbind Option 1h, Simulate Managed Hot-Remove (Table 7-36), whoever made the binding, an `Absent` Connection included |

The switch's answer: Success, `204 No Content`; Background Command
Started, `202 Accepted` with a `Task`; Busy or Retry Required, `503`;
Invalid Input, `400`; Unsupported, `501`; another code, `500`. A vPPB found
unbound by the time it was asked is `404`.

Option 0h, "Wait for port Link Down before unbinding", is never sent: the
specification does not say how it applies to an LD, whose link "cannot be
impacted by binding and unbinding of an LD within an MLD component"
(Section 7.2.3.2).

The unbind task follows the switch for as long as it takes: "The timeout
value for the host handshakes to complete is implementation specific"
(Section 7.6.6.7), and cxl-fmd sets none. The switch, which runs one
background command at a time, answers every other bind and unbind Busy
(503) until the host has released the device; `#Cxl.ForceUnbind` is the
way out.

### Forcing an Unbind (Oem action)

```http
POST /redfish/v1/Fabrics/CXL/Connections/{conn_id}/Actions/Oem/Cxl.ForceUnbind
```

A switch-attached Connection offers it in `Actions.Oem["#Cxl.ForceUnbind"]`.
It sends `Unbind vPPB (0x5202h)` with Unbind Option 2h, Simulate Surprise
Hot-Remove (CXL r4.0 Table 7-36). "There is no requirement for the FM to
force the unbind operation, but it can do so using the 'Simulate Surprise
Hot-Remove' unbinding option in the Unbind vPPB command" (Section 7.6.6.7);
the specification gives the host's handshake no timeout, so when to force
is the client's decision. The action, its name and its target are
cxl-fmd's (Connection_v1's `Actions` has an `Oem` container); it has no
parameters, and a body, if any, is ignored.

- `204 No Content` once unbound, `202 Accepted` with a `Task` when the
  switch unbinds in the background, `404` for an ID whose vPPB no longer
  holds it, `501` if the switch answers Unsupported.
- While a `DELETE`'s managed unbind (option 1h) is under way, the action
  still sends option 2h. If the switch takes it, the answer is `202` with
  the task of that unbind, which ends when the vPPB is unbound; a switch
  still running the first command answers Busy, and the action `503`,
  leaving the managed unbind as it was. The same holds for an unbind
  whose task is `Interrupted`: if the switch takes option 2h, the task is
  followed again from there. A bind under way, or an unbind
  already forced, is `503`.
- A direct-attached Connection has no vPPB to unbind: no action is
  offered, and a `POST` to its action URI is `400`.

`Oem.Cxl.Origin` of a switch-attached Connection is `"Service"` when this
run of cxl-fmd made the binding and `"Discovered"` otherwise. It is
advisory, never decides what a `DELETE` does, and is lost on restart:
nothing the switch reports says who made a binding (Tables 7-34 and 7-80),
so after a restart every binding is `"Discovered"`. Both `Oem` properties
are cxl-fmd's, described by its Oem schema (see "Oem schema").

### Connection Lifecycle

The complete memory lifecycle is:

1. **Discovery** - Devices and hosts are discovered, topology built. Existing LD allocations are represented as MemoryChunks; remaining capacity is tracked for future provisioning. Each vPPB already bound is a Connection.
2. **Provisioning** - `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` allocates memory from the remaining capacity, creating a new chunk. Not on an LD or SLD that is bound (409).
3. **Binding** - `POST /redfish/v1/Fabrics/CXL/Connections` binds the SLD or LD to a host (vPPB bind for switch-attached). The host gets a `MemoryDomain` once the Connection is `Enabled`.
4. **Unbinding** - `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}` releases the binding (vPPB unbind), or `#Cxl.ForceUnbind` forces it.
5. **Deprovisioning** - `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` returns capacity to the available pool, once no switch-attached Connection exposes the chunk (409 until then).

### What cxl-fmd can see of memory a host is given

Memory reaches a workload in five steps. The split is the design review's
(F13); the names, and which of them cxl-fmd serves, are cxl-fmd's. The FM
serves the first two, and the third only for Dynamic Capacity. The rest
belong to the host and whatever coordinates it: cxl-fmd takes no input
from a host, and nothing it serves says that a host has mapped or is
using memory.

| Step | What it means | What cxl-fmd serves |
|------|---------------|---------------------|
| Fabric-associated | the switch has bound the port or LD to the host's VCS, or the device is on the host's link | a Connection: from Get Virtual CXL Switch Info (5200h), or derived for a direct-attached device (`Oem.Cxl.Origin` `Inferred` or `Configured`) |
| Device-allocated | the device has given the LD or host the capacity | chunks: LD allocations (Get LD Allocations, 5401h), an SLD's partition (Get Partition Info, 4100h; a chunk staged for a reset is `StandbyOffline`), Dynamic Capacity extents (Get Host DC Region Configuration and Get DC Region Extent Lists, 5601h and 5603h) |
| Host-accepted | the host has taken the capacity | for Dynamic Capacity only, per tag: an LD in `PendingLDs` "for which the tagged capacity has been added with no host response yet" (Pending Reference Bitmap, CXL r4.0 Table 7-78), in `Oem/Cxl/DynamicCapacityTags` (see [DCD-FM-API.md](DCD-FM-API.md)); "The Extent List does not contain extents that are still pending acceptance from the host via the Add Dynamic Capacity Response command" (Section 9.13.3). Not for a binding or an allocation: the FM has no CXL.io access to a bound port or LD (Section 7.2.3.1) |
| Host-mapped | the host has decoded the capacity into its address space and onlined it | nothing: host `MemoryDomains` are `HostState` `NotObserved` |
| Workload-ready | applications use it, and can let it go | nothing |

The host decides how much of an add it takes: it "responds back to the
device with the updated extent for the exact capacity it added ... This
allows the host to control exactly how much of the added capacity it
wishes to utilize, which may be less than the amount of capacity sent in
the add capacity event, or even 0" (Section 9.13.3). The OCP Composable
Memory Systems white paper (informative) keeps "the separation between
fabric-level coordination and host-level memory management" (Section
5.5). So a `MemoryDomain` under `/redfish/v1/Systems/{id}` is what the
fabric makes reachable for that host. A `DELETE` of a Connection, or of a
chunk, is not a sign that the host stopped using the memory: a host-side
coordinator has to say that before a client asks.

### Direct-Attached vs Switch-Attached

| Topology | vPPB Binding | Connections |
|----------|--------------|-------------|
| Direct-attached | None: there is no switch | Derived from the link, always `Enabled` (`Inferred` with `Warning`, or `Configured`); `POST` of the pair is `200`, `DELETE` is `405` |
| Switch-attached | Bind vPPB and Unbind vPPB | One per vPPB binding of a host's VCS to a memory device's port, whoever made it; `POST` binds, `DELETE` unbinds |

### Open questions, and the choice made for each

Where neither the CXL specification nor DMTF settles how a binding is to be
served, cxl-fmd chose. Each choice is its own, not a reading of either
standard:

| Question | What the standards say | cxl-fmd's choice |
|----------|------------------------|------------------|
| Is binding a port or LD that is already bound an error, and with which return code? | Bind vPPB lists its return codes with no conditions (CXL r4.0 Section 7.6.7.2.2) | cxl-fmd refuses it itself: a target bound in another VCS is 409; the same target in the host's VCS is answered with the Connection it has (200), and nothing is sent |
| May one LD be bound to two vPPBs? | Not said. "For a DSP above an MLD, there is a vPPB for each LD" (Table 7-93) is about routing | Refused on `POST` (409). The Connection ID has the vPPB in it, so a switch that shows one LD on two vPPBs gives two Connections, and nothing collides |
| Are Binding Change records made, and seen by this FM, for binds made by another FM, through another CCI, or at boot? | Not said | The switch's VCSs are read again after every Binding Change no task follows, the whole switch before each `POST`, `DELETE` and `#Cxl.ForceUnbind`; `--rescan` stays the backstop |
| Who made a binding? | No field of Table 7-34 or Table 7-80 says | A `DELETE` unbinds whoever made it. `Oem.Cxl.Origin` is advisory and lost on restart. There is no option to protect bindings cxl-fmd did not make: with the record lost on restart, it would protect cxl-fmd's own bindings after one |
| May Set LD Allocations, or Set Partition Info, change an LD or SLD that is bound? | Not said | 409 for a chunk `DELETE`, and for a chunk `POST` that would send either command, while a switch-attached Connection that is not `Absent` exposes it. A `POST` of a `"PMEM"` chunk of an SLD, or of a `"Volatile"` one of an SLD with no partitionable capacity, sends nothing and is not refused. Direct-attached Connections, and Dynamic Capacity extents, are not gated |
| Which chunks does a binding expose? | A binding names a port and an LD-ID only (Table 7-35); what LD Allocation Ranges 1 and 2 (Table 7-49) mean to a host is not said | The chunks cxl-fmd records for the SLD, or for the LD |
| Which pool LD is LD-ID `n` of a head that presents an MLD? | "The MH-MLD maps the LD-IDs received at a head to the device-wide LD index in the MH-MLD's LD pool" (Section 2.5.1), by no rule given | `POST` to such a head is 400. A binding found there is a Connection to the head's Endpoint, `Enabled` with `Health` `Warning`, and no chunks |
| Which switch port, or host, is a head of a Multi-Headed device on? | No command maps a head's Port Number (Table 7-64) to either | Only a mock topology says. On hardware a Multi-Headed device has no Connections, and a `POST` to it is 400 |
| What does a switch that does not do Bind vPPB or Unbind vPPB answer? | Both are "O" in Table 7-31 | Unsupported is answered 501 |
| How long may a managed unbind take? | "The timeout value for the host handshakes to complete is implementation specific" (Section 7.6.6.7) | No automatic abort or escalation. A client can force the unbind with `#Cxl.ForceUnbind` (option 2h), also while a `DELETE`'s unbind is under way |
| How does Unbind Option 0h apply to an LD? | Not said | Option 0h is never sent |
| Do bindings survive a reset of the switch? | Not said | The VCSs are read again when the switch is rediscovered. The IDs are derived from the bindings, so they come back the same if the bindings do |
| Are MLDs supported on PBR switches, and what is a Bound PID? | The specification disagrees with itself: "MLDs are not supported by PBR switches" (Section 14.7.4.1), and "Edge DSPs support PCIe devices, SLDs, MLDs, GFDs, PCIe switches, and CXL HBR switches" (Section 7.7.6.1). A Bound PID is made by the Configure PID Binding command, not by Bind vPPB: "The Bind vPPB command is used to bind a direct attached device or LD to a switch's VCS" (Section 7.7.12.3) | A vPPB with Binding Status 04h is shown on the vPPB, `BoundPBRId` decoded from Bits[11:0], and gives no Connection |
| What identifies a host? | Only the USP ID of an enabled VCS, "Valid only when the VCS is enabled" (Table 7-34) | One `ComputerSystem` per enabled VCS. A host recabled to another port is another host |
| What does a direct-attached MLD give its host? | Not said | One Connection, whose targets are all of its LDs |
| Is one Connection per binding what DMTF means, and may a service make Connection members itself? | DSP0288 is silent. The OCP white paper, which is informative, says: "The user can also check the memory device and host binding information through the resources under Endpoints and Connection collection." (Section 4.2.2) | Yes to both: a Connection is a binding, and every binding found is one |
| Which EntityType is a CXL host, and a Type 3 target? | DMTF names none | `RootComplex` with `EntityLink` to its `ComputerSystem`, and `CXLDevice` with `EntityLink` to its `CXLLogicalDevice`, as Endpoint_v1 describes those values |
| Does a `PATCH` of a vPPB's `Links.Port` mean bind? | Not said | No: it is not served. A `PATCH` of a Connection is 405, though Connection_v1 marks it updatable |
| Which Connections give a host a `MemoryDomain`? | Left to the service | `Enabled` ones only |
| Which free vPPB is bound? | Not said | The lowest one above 0 that reads unbound and has nothing of cxl-fmd's under way |

### When a Device Is Removed

A device on a switch port is removed when the port, read again, reports no
device, or one of another kind. Deciding it from Connected Device Type is
cxl-fmd's; the specification does not say what a port reports after a
removal. A port whose link is down and that still reports its device keeps
it (a surprise Link Down). Its `Endpoint`, `PCIeDevice`,
`CXLLogicalDevice`s, `Memory`, `MemoryDomains` and `MemoryChunks` go, and
the event records kept for it with them.

The port is read again when a Physical Switch Event Record names it, after a
log overflowed, or on the `--rescan` timer. Which record the switch makes
depends on the removal (CXL r4.0): from an unbound port, "The switch
notifies the FM by generating Physical Switch Event Records" (Section
7.6.6.7); for a surprise removal, "The switch notifies the FM of a surprise
removal by generating Virtual CXL Switch Event Records for the change in
link status and Presence Detect" (Section 7.6.6.8), which cxl-fmd only
logs: of those records, only a Binding Change is acted on (see Known Gaps);
for an SLD or PCIe device on a bound port, "the FM can be notified but is not involved" (Section
7.6.6.7), and the removal may be seen only on the `--rescan` timer.

Its connections are the switch's to decide, and can change without a request:

| The device was | The switch | The connection |
|----------------|------------|------------|
| An MLD, removed by surprise | "The switch will automatically unbind any existing LD bindings" (Section 7.6.6.8) | Goes, once the vPPBs are read as unbound (after a Binding Change, if the switch records one, or on the `--rescan` timer). Nothing deletes it |
| An MLD, removed by a managed hot-removal | Nothing: "The FM unbinds each assigned LD with the Unbind vPPB command as it receives notifications from each host" (Section 7.6.6.7), and cxl-fmd does not (see Known Gaps) | Stands for as long as its vPPB is bound |
| An SLD on a bound port | The specification does not say what becomes of the binding: "the FM can be notified but is not involved" (Section 7.6.6.7). A vPPB bound to an SLD port "behaves the same as a PPB in a PCIe switch" (Section 7.2.3.1), so cxl-fmd assumes it stays bound to the port | Stands, with the same ID, `Status.State` `Absent`, no `TargetEndpoints` or `MemoryChunkInfo`, and no `MemoryDomain` of the host. `DELETE` unbinds the vPPB as usual. A device plugged back into the port is the host's again, and the connection has its device again |
| Being bound or unbound | finishes the operation | Its task decides, as for any other |

A device attached directly to an MCTP endpoint of its own, and a Multi-Headed
device, are not removed.

---

## Known Gaps

This section lists what cxl-fmd does not do. Each item is a gap in
cxl-fmd, not a statement of what CXL, DSP0288 or the DMTF schemas
require. Nothing elsewhere in these documents is meant to imply that
any of them is done.

In summary:

1. Dynamic capacity: no extent is shown pending. The Pending Reference
   Bitmap of 5608h is per tag, so an add a host has not answered shows
   only in `Oem/Cxl/DynamicCapacityTags` (`PendingLDs`) of the
   `PCIeDevice`, a property of cxl-fmd's Oem schema, not DMTF's. An add
   or a release is a `Task` that ends only on what is seen of the host:
   nothing cancels one, no time limit ends one, and while one runs its
   host and Tag take no other request; on a device with no tag list an
   add the host refuses ends only on an Add Capacity Response record of
   no capacity (see "Tasks of an add or a release" in DCD-FM-API.md). A
   Prescriptive add cannot be named through Redfish, as `POST` on
   `MemoryExtents` is not served. A host past LD 255 gets nothing from the
   tag list. The Host ID of a Multi-Headed DCD is taken to be the pool
   LD-ID, unverified, and Dynamic Capacity adds and releases are sent to
   such a device with it all the same (the maintainer's decision; see
   "Writes on a Multi-Headed DCD" in DCD-FM-API.md). The
   `DynamicCapacityAdd` action leaves `Prescriptive` out of its
   `Policy@Redfish.AllowableValues` for that reason, though the device may
   list it in `AddCapacityPoliciesSupported`. A `DELETE` of an extent with a tag in a Sharable region
   releases every extent of that host with the tag, and an all-zero Tag
   is taken for no tag (cxl-fmd's convention). See Comments 4 and 5, "DELETE
   of a MemoryExtent" and the Open Questions of
   [DCD-FM-API.md](DCD-FM-API.md#comments-for-table-3).
2. The managed hot-removal of an MLD (CXL r4.0 Section 7.6.6.7) is not
   carried out (see "Event logs and switch events" below).
3. A Connection is as old as the last read of its switch's VCSs: a `GET`
   sends no command. A binding made by another FM or through another CCI,
   of which the switch records no Binding Change this FM reads, is seen
   only before the next `POST` or `DELETE` on that switch, or on the
   `--rescan` timer (see "Binding").
4. On hardware, a Multi-Headed device has no Connections, and a `POST` to
   it is 400: no command maps a head's Port Number to a switch port or a
   host (a head that presents an MLD is placed on a switch port only when
   its FM-owned LD is found there, see "One device on several CCIs"). A
   head that presents an MLD cannot be bound per LD, and a binding found
   on one is `Enabled` with `Health` `Warning` and no chunks (see
   "Binding").
5. The security actions are not served on the pool LDs of a
   Multi-Headed device, nor on a device whose CEL could not be read (see
   "Security actions").
6. The event logs of a Multi-Headed device are not read (see "Event logs
   and switch events").
7. A device on a switch port is taken to be removed when Get Physical Port
   State reports a different Connected Device Type, or none. The port's
   Port Presence Pin State (PRSNT#, Bit[2] of Link State Flags, Table 7-21)
   and its Supported CXL Modes are not used to decide it (see "When a
   Device Is Removed").
8. A bind or unbind is followed for as long as the switch takes: there is
   no time limit and no automatic abort. `#Cxl.ForceUnbind` is the
   recourse for an unbind (see "Binding").
9. Get Health Info is read at
   discovery, on the `--rescan` timer and after a record in a device's
   Warning, Failure or Fatal Event Log, so a `Status` is as old as the
   last of these, and a bit that goes back to 0 is seen only on the
   timer. When a read fails, the device's views have no `Health` (see
   "Status from Get Health Info").
10. A chunk's `AddressRangeType` is the one it was made with, the same in
    every view (`Chassis`, `Systems`, the `POST` answer). A chunk found at
    discovery is `Volatile` if all of its device's or LD's memory is
    volatile in the active partition (Get Partition Info, Active Volatile
    and Active Persistent Capacity, CXL r4.0 Table 8-310; Identify Memory
    Device's Volatile Only and Persistent Only for a device with no
    Partition Alignment that did not answer it), `PMEM` if all is
    persistent, and `null` ("not known") if it has both or neither is
    known: one chunk of mixed memory is not split (cxl-fmd's rules).
    The chunks of a switch's pool domain are typed the same way.
11. Whether a host is using a device's mailboxes is not known. Set
    Partition Info, Scan Media and the security commands are not sent as
    MCTP messages unless the operator allows it with
    `--allow-mctp-restricted`, and then the operator is trusted (see
    "Commands restricted on MCTP").
12. libcxlmi fails a Get Event Records response that reports more than 20
    records (`CXLMI_MAX_SUPPORTED_EVENT_RECORDS`). A component "shall limit
    the output payload size of commands with variably sized outputs such
    that the Message Payload does not exceed" its Response Message Limit
    (Section 8.2.10.1.3), at most 1024 bytes (Table 8-218), so a page from a
    component that keeps to it holds at most 7 records; cxl-fmd does not
    check that one does. A page read through the kernel's mailbox (ioctl)
    could hold more, but cxl-fmd reads no event log that way.
13. No TLS, no authentication, and no choice of listen address: deferred
    by the maintainer (2026-11-16, review F16), not overlooked. cxl-fmd
    serves plain HTTP on every interface (`0.0.0.0`, the `--port` only),
    reads no `Authorization` header (wrong Basic credentials are ignored,
    not refused), and identifies and authorizes no one: whatever it does
    for one client it does for anyone who can reach the port, every
    `POST`, `PATCH` and `DELETE` included (binds and unbinds,
    `#Cxl.ForceUnbind`, chunk changes, Dynamic Capacity, `ResetPPB`, and
    with `--allow-mctp-restricted` the security actions and Set Partition
    Info, which without it are `409` for every client but on an SLD on
    the kernel's mailbox, item 11). Its service root links an empty
    `Sessions` collection, and there is no `SessionService` or
    `AccountService` (see "Authentication"). Nothing keeps a second
    cxl-fmd, on another port, from driving the same devices. DSP0266
    1.25.0 requires what is missing: "Implementations shall support the
    Transport Layer Security (TLS) protocol v1.2 with RFC7525 recommendations or later" (Section
    13.1.1); services "Shall support both HTTP Basic authentication and
    Redfish session login authentication" (Section 13.3.1); "Services
    shall authenticate all write requests to Redfish resources", and
    "Redfish resources shall not be available as unauthenticated", but for
    the service root, the metadata and OData service documents, the
    OpenAPI document and `/redfish` (Section 13.3.2.1). Until then, run
    cxl-fmd only where nothing untrusted can reach its port, or behind a
    proxy that provides TLS and authentication (see `man cxl-fmd`,
    SECURITY).
14. A device that is not ready at discovery (see "Devices that are not
    ready" under "Changes for Clients") is not served at all, `404`,
    until it is discovered whole: there is no resource for it in a
    `Status.State` such as `Starting`. A device whose Get DCD Info is
    read late, on a `--rescan`, has its Dynamic Capacity event log read not
    when it is found but on the next `--event-poll` tick, or on an Event
    Notification from it (with `--event-poll 0`, only then).

### Event logs and switch events

These are known gaps in what cxl-fmd does, not properties of a schema:

- The event logs of a Multi-Headed device are not read, and the records of
  its Dynamic Capacity Event Log are not kept (see "Where cxl-fmd puts CXL
  event records").
- Of a Virtual CXL Switch Event Record (Table 7-80), only a Binding Change
  is acted on. It ends the task of a bind or unbind cxl-fmd started; one
  cxl-fmd did not ask for, such as the switch's own unbind after an MLD's
  surprise removal, has the switch's VCSs read again (all of them, not only
  the record's; the record's binding fields are not taken as they are).
  Whether a switch records binds made by another FM or through another CCI
  in logs this FM reads, the specification does not say: those are seen
  when the switch is next read, on the `--rescan` timer, or after a log
  overflowed.
- The managed hot-removal of an MLD (Section 7.6.6.7) is not carried out:
  cxl-fmd does not act on a host's Attention Indicator, unbind the LDs, or
  set the MLD port's Attention LED.

### Binding

These are known gaps in what cxl-fmd does:

- A bind or unbind is followed for as long as the switch takes: there is no
  time limit, and no automatic abort. "The timeout value for the host
  handshakes to complete is implementation specific" (Section 7.6.6.7). A
  host that does not release the device keeps the task running, and the
  switch, busy with that background command, refuses every other bind and
  unbind (503) until a client sends `#Cxl.ForceUnbind`.
- A `GET` sends no command. Connections, and the host's `MemoryDomains`,
  are as the switch's VCSs were last read: at discovery, before each
  `POST`, `DELETE` and `#Cxl.ForceUnbind` on that switch, after a Binding
  Change no task follows, after a log overflowed, and on the `--rescan`
  timer.
- A head of a Multi-Headed device that presents an MLD cannot be bound per
  LD, and a `POST` to it is 400: nothing maps a pool LD to the LD-ID the
  head uses for it (Section 2.5.1). A vPPB found bound to one of its LDs is
  a Connection to the head's Endpoint, `Enabled` with `Health` `Warning`,
  with no chunks and no `MemoryDomain`.
- On hardware, no command maps a head's Port Number (Table 7-64) to a
  switch port or a host, so cxl-fmd knows neither: a Multi-Headed device
  has no Connections, and a `POST` to it is 400. Only a mock topology can
  say where a head is (`parent_switch` and `parent_port`, or
  `attached_host`). The one exception is a head that presents an MLD on
  a switch port whose FM-owned LD, Identified through the switch, has the
  device's serial number, when it is the only head of the device that can
  be (see "One device on several CCIs"); such a head cannot be bound per
  LD anyway. A head that presents an SLD cannot be Identified through a
  switch (Section 7.6.7.3.1), so it is never placed.
- `Oem.Cxl.Origin` is lost on restart: after one, every binding is
  `"Discovered"`.
- What a host has mapped of what is bound is not known: the FM has no
  CXL.io access to a bound port or LD (Section 7.2.3.1). A `MemoryDomain`
  of a host is what a binding makes reachable (`HostState`
  `NotObserved`).
- A direct-attached SLD or MLD is given to the only direct-attached host
  with no evidence of the link (`Inferred`, `Warning`). Proving it would
  take a host-side source, such as a host agent matching the device's
  serial number, or the operator's configuration; cxl-fmd has neither.

### DSP0288 mappings

These are known gaps against DSP0288 1.4.0 (see Tables 1 and 2):

- The security actions DSP0288 1.2.0 mapped to `Memory` are still served
  there, as deprecated aliases of the `CXLLogicalDevice` actions (see
  "Security actions").

### Memory device writes

These are known gaps in what cxl-fmd does:

- Set Partition Info, Scan Media, Set and Disable Passphrase, Freeze
  Security State and Passphrase Secure Erase are marked O with note 4 for
  MCTP in Table 8-308, which reads: "Systems capable of management from
  Mailbox registers and an MCTP-based CCI shall ensure that these commands
  are not issued as MCTP messages while a device's mailboxes are
  operational." cxl-fmd cannot tell whether they are: it refuses these
  operations (409) on every device but an SLD on the kernel's mailbox,
  unless started with `--allow-mctp-restricted`. With that option it sends
  them whenever a client asks; use it only when no host drives the devices
  (see "Commands restricted on MCTP").
- Which reset makes a staged partition active (the command effects of Set
  Partition Info, in the CEL) is not read, and nothing tells cxl-fmd that
  the device was reset: a staged chunk (see "Staged chunks") is looked at
  again only on a `--rescan`, so with none it stays `StandbyOffline`.

### Conditions

`Status.Conditions` are given only for what Get Health Info reports (see
"Status from Get Health Info"), as DSP0288 maps it. A `Health` of
`Warning` that comes from cxl-fmd's own state has none: a Connection that
is `Inferred` or whose target is not known, a DCD's `MemoryRegion` or
`MemoryExtent` whose host or tag list could not be read, an extent in no
region. Why is said in this document, not in the resource.

### Oem identifier

cxl-fmd's Oem properties are under `Oem.Cxl`, its Oem actions are
`#Cxl.<Action>`, and its registry is `CxlHealth`. "The identifier shall be
either an ICANN-recognized domain name (including the top-level domain
suffix), with all dot ( . ) separators replaced with underscores ( _ ), or
an IANA-assigned Enterprise Number prefixed with "EID_."" (DSP0266 1.25.0
Section 9.8.3). `Cxl` is neither: it is not a domain cxl-fmd's project
holds, nor an Enterprise Number. It is kept, as clients use it; moving to
a conforming identifier would rename every Oem property, the action and
the registry.

### Properties not served

Properties and links defined in DMTF schemas but not yet implemented:

#### MemoryChunks

| Property | Schema | Notes |
|----------|--------|-------|
| `MediaLocation` | MemoryChunks.v1_5_0+ | Not served on a device's chunks under `/redfish/v1/Chassis`. A host's chunks under `/redfish/v1/Systems/{host}/MemoryDomains` have `Remote`. |
| `RequestedOperationalState` | MemoryChunks.v1_5_0+ | Requested operational state for the chunk. |

#### CXLLogicalDevice

| Property | Schema | Notes |
|----------|--------|-------|
| `Links.PCIeFunctions` | CXLLogicalDevice.v1_0_0+ | Link to associated PCIe functions. Requires PCIeFunction resource implementation. |

---

## Changes for Clients

cxl-fmd has no release notes. These are the changes a client can see from
the work that made Connections the switch's bindings (the commits from
"discovery, mock: Bound LD ID is FFh unless Bound LD, and decode Bound
PID" on). None of them is shimmed: a client that used the old IDs or
types has to change.

### IDs and URIs

- **Connection IDs.** `conn0`, `conn1`, ... (numbered from 0 on each
  start of the daemon) are gone. A switch-attached Connection is
  `{switch}_vcs{V}_vppb{P}_p{N}`, with `_ld{L}` for an LD; a
  direct-attached one is `{host}_direct_{device}` or
  `{host}_direct_{mhd}_h{H}`. They are the same after a restart.
- **Implicit Connections.** The Connection each switch-attached device had
  under its own device ID, bound or not, with no `InitiatorEndpoints`, is
  gone: `GET /redfish/v1/Fabrics/CXL/Connections/{device_id}` is 404. A
  device with no binding has no Connection.
- **Host MemoryDomain IDs** follow the Connection IDs:
  `/redfish/v1/Systems/{host}/MemoryDomains/{connection_id}`.
- **The `/redfish/v1/Fabrics/CXL/MemoryChunks/{chunk}` URI** that a
  Connection's `MemoryChunkInfo` used when it could not find a chunk's
  domain is gone; every entry is the chunk's URI under `Chassis`.
- **Endpoints of MLDs and Multi-Headed devices.** `Endpoints/{mld}` and
  `Endpoints/{mhd}` are gone (404). Each LD of an MLD, and each pool LD of
  a Multi-Headed device, is `Endpoints/{dev}_ld{N}`. A head that presents
  an MLD is `Endpoints/{mhd}_head{H}`. An SLD's Endpoint keeps its ID.
- **Hosts on hardware.** Discovery makes a host (`ComputerSystem` and
  `Endpoint`) only for a VCS whose VCS State is Enabled; it made one for
  every VCS in the Active VCS Bitmask.

### EntityTypes and links

- A host Endpoint's `EntityType` is `RootComplex`, with `EntityLink` to
  `/redfish/v1/Systems/{host}`; it was `Processor`, with none.
- An SLD Endpoint's `EntityType` is `CXLDevice`, with `EntityLink` to its
  `CXLLogicalDevices/0`; it was `MemoryChunk`. An LD Endpoint is
  `CXLDevice` with `EntityLink` to its `CXLLogicalDevices/{N}`; a head
  Endpoint is `Memory`, with none.
- New links: `Links.Connections` on an Endpoint that is an end of a Connection; `Links.Endpoints` on a
  `VirtualPCI2PCIBridge` (what it is bound to) and on a `VirtualCXLSwitch`
  (its Zone's Endpoints); a `CXLLogicalDevice`'s `Links.Endpoints` is its
  own LD Endpoint, not the device's; a Port's `Links.AssociatedEndpoints`
  are the LD Endpoints at the other end of its link; a pool LD Endpoint's
  `Links.ConnectedPorts` is the port of its head.
- A Zone lists each LD Endpoint bound to its VCS, not the device: an MLD
  with LD 0 in VCS 0 and LD 1 in VCS 1 is `{mld}_ld0` in one Zone and
  `{mld}_ld1` in the other, where it was the MLD in both.
- A `VirtualPCI2PCIBridge` with `BindingStatus` `BoundPID` has the PID in
  `BoundPBRId` (it was 0 from discovery).

### Connections

- **What is listed.** Every vPPB of an enabled VCS (but vPPB 0) bound to a
  memory device's port, whoever made it: bindings made before cxl-fmd
  started, by another FM or through another CCI are Connections, with
  `InitiatorEndpoints`, and give the host a `MemoryDomain`. Direct-attached
  Connections exist from startup, with no `POST`.
- **Body.** `Name` is `CXL vPPB Binding` or `CXL Direct-Attached
  Connection` (was `CXL Memory Connection`, or `CXL SLD Connection` / `CXL
  MLD Connection` for an implicit one). `MemoryChunkInfo` has one entry per
  chunk of the target, not the one chunk the `POST` named, each with
  `AccessCapabilities` `["Read", "Write"]` and `AccessState` `Optimized`
  (`Transitioning` while it is `Starting` or `Updating`: see "Requests
  after the design review"). `Status` can be `Enabled` with `Health` `Warning` (target not known).
  `TargetEndpoints` is the LD Endpoint for an LD. New:
  `Oem.Cxl.Origin`, and `Actions.Oem["#Cxl.ForceUnbind"]`.
- **Tasks.** A bind's `Task` lists the Connection in
  `Links.CreatedResources` while it runs (only once `Completed`, before).
  A task that is lost no longer leaves its Connection `Starting` or
  `Updating` (since the design review, one whose vPPB still reads 01h is
  `Interrupted` and does: see "Requests after the design review").

### Requests and status codes

| Request | Was | Now |
|---------|-----|-----|
| `POST` of a target the host's VCS has bound already | `201` and a second Connection that did not unbind on `DELETE` | `200` and the existing Connection, `Location` its URI; nothing sent |
| `POST` of a target being bound for the host | another Bind vPPB sent, for another vPPB (a vPPB being bound reads 01h, which was not taken as holding the target), which a switch busy with the first answers Busy: `503` | `202` and the same `Task`; nothing sent |
| `POST` of a target being unbound for the host | `201` and a Connection that adopted the binding while the vPPB still read bound; once it read 01h, another Bind vPPB, answered as above | `409` |
| `POST` with more than one initiator or target | the first of each taken, the rest ignored | `400` |
| `POST` from a switch-attached host to a device on no switch, or from a direct-attached host to a device on a switch | `201` and a Connection, with nothing bound | `400` |
| `POST` of a direct-attached pair | `201` and a new Connection | `200` and the Connection it has |
| `POST` with `AccessCapabilities` other than `Read` and `Write` | taken, and served as asked | `400` |
| `POST` naming a chunk that is not the target's | taken | `400` |
| `POST` with chunks on two LDs of one device | taken (only the first entry was read) | `400` |
| `POST` with an SLD or LD Endpoint and no `MemoryChunkInfo` | `400` | taken |
| `POST` or `DELETE` the switch answers Unsupported | `500` | `501` |
| `POST` or `DELETE` the switch answers Invalid Input | `500` | `400` |
| `DELETE` of a Connection whose binding cxl-fmd did not make, or adopted | `204` and nothing unbound | Unbind vPPB, option 1h: `204` or `202` |
| `DELETE` of a direct-attached Connection | `204` | `405`, `Allow: GET` |
| `DELETE` of a stale ID (the vPPB now holds something else) | (IDs were not tied to a vPPB) | `404`, nothing unbound |
| `DELETE` of a Connection whose task was lost | `503`, for good | taken (since the design review, `503` while the vPPB of an `Interrupted` task reads 01h) |
| `PATCH` of a Connection | `501` | `405`, `Allow: GET, DELETE` |
| `POST .../Actions/Oem/Cxl.ForceUnbind` | (not served) | Unbind vPPB, option 2h |
| Chunk `DELETE`, or a chunk `POST` that sends Set LD Allocations or Set Partition Info, on an LD or SLD a switch-attached Connection binds | sent | `409`, nothing sent |
| `GET` of an MLD's or Multi-Headed device's Endpoint | `200` | `404` |

The text of several error messages changed; for example, a `POST` from a
host to a pool LD not mapped to its head was "Cross-head access denied:
host cannot access this memory", and is "The host is not attached to that
device or head: it cannot reach this memory".

### Systems

- A host's `MemoryDomains` are those of its `Enabled` Connections, made by
  anyone. A domain's `MemorySizeMiB` is that of all of the target's
  chunks, and it has `Links.CXLLogicalDevices`. Its `MemoryChunks` has one
  member per chunk (it had one), each with `MediaLocation` `Remote` and
  `Links.Endpoints`.

### Mock topologies and `--check`

- A switch takes `bind_result` and `unbind_result`, and a topology an
  `external` list played by `SIGUSR2` (see
  [mock-topology.md](mock-topology.md)).
- A `bound_ld` vPPB with no `bound_ld_id` is LD 255, which `--check`
  refuses; it was LD 0. A `bound_ld_id` other than 255 on a vPPB that is
  not `bound_ld` is warned of and loaded as 255.
- `--check` refuses vPPB 0 of an enabled VCS bound to anything but that
  VCS's USP, and warns of bindings that give no Connection, or an
  unresolved one.

### Requests after the design review

Changes from the fixes of the external design review (branch
`review-fixes`):

| Request | Was | Now |
|---------|-----|-----|
| Chunk `POST` with a size in MiB past 2^44 - 1 (`MemoryChunkSizeMiB` or `InterleaveSets[].SizeMiB`) | `201`, and a chunk of the size wrapped to 64 bits of bytes, which Set LD Allocations or Set Partition Info was sent for | `400`, nothing sent |
| Chunk `POST` with `MemoryChunkSizeMiB` 0 or negative | `409` | `400` |
| Chunk `POST` with an `InterleaveSets[]` `SizeMiB` or `OffsetMiB` that is negative, not an integer, or past 2^44 - 1 | ignored (0) when not an integer; a negative one wrapped | `400` |
| Chunk `POST` with a `MemoryChunkSizeMiB` that is not an integer, next to an `InterleaveSets[0].SizeMiB` | the `SizeMiB` taken instead | `400` |
| Chunk `POST` with an `AddressRangeOffsetMiB` that is negative, not an integer, or puts any of the chunk past the domain's `MemorySizeMiB` | `201` (a negative or wrapped offset, or an offset past the end; one that was not an integer was ignored) | `400` |
| Chunk `POST` with no `AddressRangeOffsetMiB` | placed after the chunk that ends last, even past the end of the domain | placed at the lowest offset where it fits, within the domain, or `409` |
| Chunk `POST` with an `AddressRangeType` other than `PMEM` or `Volatile`, or not a string | `201`, as `PMEM` | `400` |
| Chunk `GET` of a device or LD with both volatile and persistent capacity, or of unknown capacities (Identify Memory Device not answered) | `AddressRangeType` `PMEM` | `null` |
| Chunk `GET` under `/redfish/v1/Systems/...` of a device with no Volatile Only or Persistent Only capacity, partitioned all volatile | `PMEM` (and `Volatile` under `Chassis`) | `Volatile` in both: every view serves the type the chunk was made with, from the active partition |
| Chunk `POST` with no `AddressRangeType` | `PMEM` in the answer, whatever the device (a volatile-only SLD's then read `Volatile` on a `GET`) | the type all of the device's or LD's memory is; on an SLD with both, or neither known, `400`; on such an LD, `null` |
| Chunk `POST` with an `AddressRangeType` the device or LD has no memory of and cannot be partitioned to (`PMEM` on a volatile-only SLD, `Volatile` on an LD all persistent) | `201` | `400`, nothing made or sent |
| Chunk `POST` on an MLD or MHD that names no LD, an LD the device does not have, or another device's (no `InterleaveSets`, `.../Memory/{dev}_ld9`, `/redfish/v1/Chassis/other/Memory/other_ld0`) | `201`, and nothing sent: no LD's allocation grew | `400`, nothing made or sent |
| Chunk `POST` naming its LD in another form than the URI of its `Memory` (the LD's component ID `{dev}_ld0`, `/not/a/redfish/uri/{dev}_ld1`) | `201`: the number after the first `_ld` taken, if the string had the Chassis ID in it | `400` |
| Chunk `POST` naming its LD by the URI of its `Memory`, on a device whose ID is longer than the mock's (`cxl_{serial}` on hardware) | `201`, and nothing sent: the URI was cut to 63 characters, which lost the `_ld{N}` | Set LD Allocations for that LD |
| Chunk `POST` with more than one `InterleaveSets` entry, or one with an `OffsetMiB` other than 0, a `RegionId`, a `MemoryLevel` other than 1, or a `SizeMiB` other than `MemoryChunkSizeMiB` | `201`: the first entry's LD grew by `MemoryChunkSizeMiB`, the rest of the request was echoed and not sent | `400` |
| Chunk `POST` with `Links.CXLLogicalDevices` naming the LD | ignored | taken, as `InterleaveSets[0].Memory` is |
| Chunk `POST` on an MLD or MHD whose new LD size is not a multiple of `MemoryChunkIncrementMiB` | `400` from hardware after Get LD Allocations, `201` in a mock | `400`, nothing sent, in a mock too; on a hardware MHD, whose Memory Granularity discovery does not record, still `400` after Get LD Allocations, before Set LD Allocations |
| Chunk `POST` on an SLD with an `InterleaveSets[0].Memory` other than the SLD's own `Memory` URI, or a `Links.CXLLogicalDevices` other than its `CXLLogicalDevices/0` | ignored (`InterleaveSets` echoed in the `201`) | `400` |
| Chunk `POST` with an `InterleaveSets` that is not an array, an entry that is not an object, a `Memory` that is not a link object (a string), or a `Links.CXLLogicalDevices` that is not an array of links | ignored | `400` |
| Chunk `POST` response on an MLD or MHD | `InterleaveSets` as the request gave it, `Links.CXLLogicalDevices` from the number after the first `_ld` of its `Memory` | one `InterleaveSets` entry, the LD's `Memory` URI with `MemoryLevel` 1, `OffsetMiB` 0 and the chunk's `SizeMiB`, and the LD's link; none for an SLD |
| Chunk `DELETE` of an MLD chunk made with `InterleaveSets[0].SizeMiB` | the LD shrunk by that `SizeMiB` (not the size the `POST` added) | shrunk by the chunk's `MemoryChunkSizeMiB` |
| Chunk `DELETE` on hardware of an MLD's chunk that names no LD | `204`, nothing sent, and its size counted free in the domain | `501`, the chunk kept |
| Chunk `DELETE` on hardware of a Multi-Headed device's chunk | `204`, nothing sent, and its size counted free in the domain (so a later `POST` grew the LD past the device) | Set LD Allocations for the pool LD, less the chunk: `204`; `500` if it fails, with the chunk kept |
| Chunk `DELETE` or `POST` on a pool LD whose head has a Connection (a direct-attached host, or a vPPB binding, `Unresolved` too), in a mock too | `204` / `201` | `409`, nothing sent |
| Chunk `DELETE` of an MLD's or MHD's chunk that Set LD Allocations answers Success for, with the LD shrunk by less than the chunk | `204`, the whole chunk counted free | `500`; the chunk stays, as large as what the LD did not give back, and the domain gets back what it did |
| Chunk `DELETE` of a chunk larger than its LD's allocation | Set LD Allocations for an allocation of 0 | `409`, nothing sent |
| Chunk `POST` or `DELETE` of an MLD or MHD in a mock | the LD's `MemorySizeMiB` unchanged | grown or shrunk by the chunk, as on a device |
| `GET` of the `MemoryDomain` of an MLD, or of a Multi-Headed device whose LD Pool CCI answers Get LD Info | `MemorySizeMiB` the sum of the LDs' allocations, with nothing left, so a chunk `POST` was `409` until a chunk was deleted | the device's Memory Size (Get LD Info); a `POST` of up to what the LDs have not been allocated is made. In a mock, an MLD's is `identify_memory_device.total_capacity_mb` (every shipped one is the sum of its LDs) and a Multi-Headed device's the new `pool_capacity_mb` |
| `GET` of the `MemoryDomains` of an MLD or Multi-Headed device none of whose LDs has memory allocated | no domain (an empty collection; `404` for the domain and a chunk `POST`) | its domain, with no chunks |
| `GET` of the chunk of an LD with no memory allocated (`{dev}_ld{N}_chunk`) | `200`, `MemoryChunkSizeMiB` 0 | `404`: there is no such chunk |
| `GET` of an MLD's or Multi-Headed device's `MemoryDomain`, chunks, or `CXLLogicalDevice` after another FM changed an LD allocation | what discovery found, until a restart | what Get LD Allocations says, from the next `--rescan` (a chunk's LD before its `POST` or `DELETE`): chunks are made, shrunk or removed so that each LD's add up to its allocation |
| Chunk `POST` on hardware on an LD whose allocation another FM changed | Set LD Allocations for the size discovery found plus the chunk, which took from the LD what the other FM gave it | Get LD Allocations first: the LD's allocation now plus the chunk; `500`, nothing set, if it cannot be read |
| Chunk `DELETE` on hardware of a chunk whose LD another FM took the memory of | Set LD Allocations for the size discovery found less the chunk | Get LD Allocations first: `404` if the chunk went with the memory, nothing set; `500` if it cannot be read |
| Chunk `POST` or `DELETE` on hardware of an MLD or MHD whose Set LD Allocations answers Internal Error (or another code that is not Invalid Input, Unsupported, Retry Required, Invalid Payload Length or Unsupported Mailbox or CCI), a response that cannot be read, or a Success for another LD | `500`, the new chunk removed (`POST`) or kept (`DELETE`) with nothing read | Get LD Allocations reads the LD back: `201` or `204` if it has what was asked, `500` with the chunk as it was if it has what it had, `500` with its chunks made what it has otherwise. If that read fails too: `500` "Outcome unknown", the chunk kept, and no chunk of the device changed until the LD is read again |
| Chunk `POST` on a bound LD, or Volatile chunk `POST` on a bound SLD, when the domain has no room | `409` "Insufficient capacity" | `409` "A Connection binds the logical device": the binding is checked first, before anything is sent |
| Chunk `POST` on hardware on a pool LD of a Multi-Headed device | Get and Set LD Allocations sent to the endpoint that answered Get Multi-Headed Info with no tunnel, through a tunnel to the LD Pool CCI (Target Type 1) | sent to that endpoint with no tunnel; `501`, with nothing sent, if no LD Pool CCI is known |
| `GET` of a chunk made with `POST` on an MLD | `Links.CXLLogicalDevices` LD 0, and LD 0's `Status`, whatever its LD | its LD's |
| `GET` of a `CXLLogicalDevice` of an MLD or MHD | no `Links.MemoryChunks` | `Links.MemoryChunks`, its chunks |
| Chunk `POST` after a `DELETE` of a chunk of the domain | the ID of a chunk the domain still had, so that two chunks had one URI, a collection listed it twice, and a Connection's `MemoryChunkInfo` too | an ID that no chunk of the domain has had since cxl-fmd started, or discovered the device |
| Chunk `POST` on a domain with chunks found at discovery (the `{device}_ld{N}_chunk` of an MLD or MHD) | `{domain}_chunk{N}`, N the number of chunks in the domain | `{domain}_chunk0` for the first, then 1, 2, ... |
| Connection `POST` with a link that is not the URI the service serves the resource at, but ends in the ID of one (`/bogus/sld_0`, `/redfish/v1/Chassis/sld_1` as a target, a chunk of one device under another's Chassis, a relative URI) | `201`: the last segment was taken as the ID, whatever came before it | `400`, nothing bound |
| Connection `POST` naming a chunk by its URI in a host's view, `/redfish/v1/Systems/{host}/MemoryDomains/{dom}/MemoryChunks/{chunk}` | taken, by its last segment | `400`: a chunk is named by its Chassis URI |
| Connection `POST` with `InitiatorEndpointGroups` or `TargetEndpointGroups` | ignored: `201` from the Endpoints alone | `400`, nothing bound |
| Connection `POST` with `VolumeInfo` (`[]` too) | ignored: `201`, or `200` for a target the host has | `400`, nothing bound |
| Connection `POST` with a `MemoryChunkInfo[].AccessState` other than `Optimized` (`NonOptimized`, `Standby`, `Unavailable`, `Transitioning`, or not a string) | ignored: `201`, or `200`, and the Connection served `Optimized` | `400`, nothing bound |
| `DynamicCapacityAdd` or `DynamicCapacityRelease` with a `Length` past 2^44 - 1 MiB, or `PATCH` of a `MemoryRegion`'s `BlockSizeMiB` past it | `200` with the value wrapped (an add of 2^44 + 256 MiB added 256 MiB), or `400` with another message when it wrapped to 0 | `400`, nothing sent |
| `GET` of a DCD's `MemoryRegion` or `MemoryExtent` when the host's LD is in the Pending Reference Bitmap of a tag (5608h), and of an extent a mock topology file declares `pending` | `Pending` `true` in the region's `MemoryExtents[]` and `Status.State` `Starting` for every extent of that host and tag, accepted ones too | `Pending` `false` and `Enabled` for every extent 5603h lists; the add is in the tag's `PendingLDs` of `Oem/Cxl/DynamicCapacityTags` only |
| `GET` of a `CXLLogicalDevice` of a DCD | `MemorySizeMiB` the static capacity only (Total Capacity, or the LD allocation) | the static capacity plus every extent the LD's host holds, so it changes with an add or a release; a non-DCD device is unchanged |
| `GET` of a device's `Chassis` whose Health Status bits OR to 3 or more, or whose Media Status is not 00h (the Chassis of an MLD or Multi-Headed device with LDs at 01h and 02h, an SLD at 08h or 04h, Media Status 04h to 09h) | `Critical` and `UnavailableOffline` (`Warning` for Media Status 01h), whatever its `PCIeDevice` and `Memory` said | as its other views: see "Status from Get Health Info". Every one of them now has `Health` `Warning` for Media Status 02h and 04h to 09h (was `OK` but on the Chassis), `Critical` for 03h, `OK` for 01h; and a `HealthRollup` on the Chassis |
| `GET` of a switch's `Chassis` | `Status.Health` the OR of its devices' Health Status compared as a number (`mock/switch-attached.json`: `Critical`, `UnavailableOffline`, with no device worse than `Warning`) | `State` `Enabled`, no `Health`, and `HealthRollup` of its devices as each maps (`Warning` there) |
| `GET` of any view of a device or LD whose Get Health Info failed at discovery | `Health` `OK`, `State` `Enabled`, from the zeroed response | no `Health` (or `HealthRollup`); `State` `Enabled` |
| `GET` of any view of a device on hardware whose Health Status or Media Status changed after discovery | `Status` as discovery read it, until a restart | as Get Health Info reads after the next `--rescan`, or after a drain of the device's logs that found a Warning, Failure or Fatal record (or an overflow); a read that fails then leaves `Health` out. See "Status from Get Health Info" |
| A request with a query parameter that starts with `$` (`GET /redfish/v1/Chassis?$filter=...`, `$top`, `$skip`, `$select`, `$expand`; a `POST` too) | `200` with the parameter ignored (the whole collection), or the request carried out | `501`, with an `@Message.ExtendedInfo` `QueryParameterUnsupported` entry for each `$` parameter; other parameters are still ignored |
| The same, when cxl-fmd runs out of memory while it answers | the request carried out as if it had no `$` parameter (no memory to list them), or a `501` whose `@Message.ExtendedInfo` lacked entries, or whose body was `{}` | `501`, with the whole extended error, or else one with a single `GeneralError` message (and if even that cannot be made, a fixed body whose message is `InternalError`) |
| `PUT`, or a `POST`, `PATCH` or `DELETE` a resource does not take (`PUT /redfish/v1/Chassis`, `PATCH` or `DELETE` of a Chassis, `POST /redfish/v1/Chassis`) | `501` | `405` with an `Allow` header of the methods the resource takes; `404` for a URI that is not a resource |
| `HEAD` or `OPTIONS` (or another method cxl-fmd does not serve) | `405` with no `Allow` header | `405` with `Allow`; `404` for a URI that is not a resource |
| `PATCH` of a direct-attached Connection | `405`, `Allow: GET, DELETE` | `405`, `Allow: GET` |
| `GET` of a Connection while the switch binds or unbinds it (`Status.State` `Starting` or `Updating`) | `MemoryChunkInfo[].AccessState` `Optimized` | `Transitioning`; `Optimized` once it is `Enabled` |
| Chunk `DELETE` and `POST` on hardware of a `"Volatile"` chunk of an SLD with no partitionable capacity (Partition Alignment 0, such as one that is all Volatile Only) | `400`: Set Partition Info was taken to be needed, and is not sent to such a device (nor to any SLD behind a switch), so the chunk found at discovery could not be deleted and no `"Volatile"` chunk made | `204` / `201`, nothing sent: the chunk divides the Volatile Only capacity, as in a mock |
| `"Volatile"` chunk `POST` on a bound SLD with no partitionable capacity (`sld_0` of `mock/fm-boots-first.json` once bound) | `409` "A Connection binds the logical device" | `201`, nothing sent, as for a `"PMEM"` chunk; one of an SLD with partitionable capacity is still `409` |
| `"Volatile"` chunk `POST` on hardware on a directly attached SLD with partitionable capacity (Set Partition Info sent, not Immediate) | `201` and an `Enabled` chunk, in the direct-attached Connection's `MemoryChunkInfo` and the host's `MemoryDomains`, though its partition is active only from the device's next reset | `201` with `Status.State` `StandbyOffline`; in no Connection or host view, and a Connection `POST` naming it `409`, until a `--rescan` reads Get Partition Info with the change active (then `Enabled`) or gone (then the chunk is removed). See "Staged chunks" |
| `GET` of a direct-attached Connection of an SLD or MLD (given to the only direct-attached host, `local` on hardware) | `Status` `Enabled`, `OK`, no `Oem` | `Enabled`, `Warning`, with `Oem.Cxl.Origin` `Inferred`; still the host's `MemoryDomain` |
| `GET` of a direct-attached Connection of a head of a Multi-Headed device (mock topologies only) | no `Oem` | `Oem.Cxl.Origin` `Configured`; `Status` unchanged |
| `GET` of a host's `MemoryDomain` (`/redfish/v1/Systems/{id}/MemoryDomains/{dom}`) | no `Oem` | `Oem.Cxl.HostState` `NotObserved`: what the host has mapped is not known |
| `GET` of a DCD's `PCIeDevice`, `MemoryRegion`s and `MemoryExtent`s after a Dynamic Capacity List Tags (5608h) read failed | `TagsKnown` `false` with `"Tags": []`, and every extent `Pending` `false`, `OK` | `TagsKnown` `false` and no `Tags`; the device's `MemoryRegion`s and its `MemoryExtent`s with a tag `Status.Health` `Warning`. A device that answers Unsupported has no `Tags` and no `Warning` |
| `GET` of a DCD's `MemoryExtent`s after a Dynamic Capacity Event Record of their host was read and cleared and the read of the host that followed failed (or after an overflow, or a tag list read that failed) | `Status.Health` `Warning`, as it was, until another record or command made cxl-fmd read the host again: later empty logs read nothing else | the same until the next `--event-poll` tick (default 10 s), which reads every such host, and the tag list, again until a read completes; then what the device lists, `OK`. Devices whose DC event log is the host's are read too. With `--event-poll 0`, as before |
| `GET` of a DCD's `MemoryRegion` while, or after, a read of its host's regions (5601h) fails on a page after the first | `404` for the regions past the first page (they were removed when page 1 was read), the host's extents still linking to them, and `Status.Health` `OK` on the rest | the regions, and the `CXLLogicalDevice`'s extent and tag counters, as the last complete read had them; `Status.Health` `Warning` until the host is read again |
| `GET` of a DCD's `MemoryRegion` whose host's last read failed, or is owed after an event record named it | `Status.Health` `OK` (unless the tag list could not be read) | `Warning`, as the host's `MemoryExtent`s |
| `GET` of a DCD's `CXLLogicalDevice`, `MemoryRegion`, `MemoryExtent` and `PCIeDevice` | nothing said of when the host's regions (5601h), its extents (5603h) or the tag list (5608h) were last read | the time (UTC) the last read of each that completed ended: `Oem.Cxl.DynamicCapacityRegionsLastRefreshed` and `DynamicCapacityExtentsLastRefreshed` of the `CXLLogicalDevice` (a new `#Cxl.v1_0_0.CXLLogicalDevice`), `Oem.Cxl.LastRefreshed` of the `MemoryRegion` and the `MemoryExtent`, `Oem.Cxl.DynamicCapacityTags.LastRefreshed`; each absent until a read completes. A read that fails leaves the time of the one before (review F07 claim 6; see DCD-FM-API.md, Comment 6) |
| `GET` of a `MemoryExtent` whose region is gone from its host's 5601h, when the 5603h after it fails | `Links/MemoryRegions` to the region that is gone (`404`) | no `Links/MemoryRegions`, `Status.Health` `Warning` |
| `DynamicCapacityAdd`, `DynamicCapacityRelease`, `CXLReferenceDCMemory`, a `MemoryExtent` `DELETE` or a `MemoryRegion` `PATCH` on hardware that the device answered Success for, when the read after it (5601h, 5603h or 5608h) fails, is answered Retry Required, or finds the extent list changing each time it is read | `500` "Initiate Dynamic Capacity Add failed" (and the like); a client that sent it again got a second add or release | `CXLReferenceDCMemory` and a `PATCH`: `200` with `{"error": {"code": "Base.1.24.Success", "message": "The device accepted the request, but its state could not be read back after it: ... Do not send the request again"}}`; an add, a release and a `DELETE`: `202` with a `Running` `Task` whose message says the host could not be read back after it. The host's resources `Warning` until the next `--event-poll` tick reads it |
| The same requests when libcxlmi cannot carry the command or read its response (a 5604h answered Success with a payload) | `500` with the request's "failed" message, logged as "was not carried out" | `500` "Outcome unknown: the device may or may not have carried out the request ...: check its state before sending this again"; the host (the tag list for `CXLReferenceDCMemory`) is read again on the next tick |
| A bind or unbind task whose Background Operation Status (0002h) says Success while the switch's VCSs cannot be read (5200h fails) | `Exception`, "... the vPPB is not bound to what was asked" (or "is still bound"), and the Connection of a bind gone, though the switch bound it | `Running`; it ends when the vPPB is read, or is lost after five such answers |
| A bind or unbind task whose 0002h says it is over while the vPPB still reads 01h | `Exception`, its Connection gone (the vPPB at 01h gave none) and the switch not asked again | `Running`, the Connection `Starting` or `Updating`, and the switch asked again (after five such answers, see the next rows) |
| A Binding Change record (Table 7-80) of a vPPB a task follows whose 33h-35h are not what the task asked for (an earlier unbind's record read during a bind), or that shows 01h | taken for the end of the task: `Exception` while the vPPB read 01h, and the real end never read | turned away: the VCSs are read again, and the task goes on |
| A lost or `Interrupted` bind or unbind task whose vPPB, read at last, shows what was asked, when it showed that before the command too, or its VCS could not be read before it | `Completed`, "The vPPB is bound" (or "unbound"), "read from the vPPB" | `Exception`, "... whether it carried out the bind is not known"; its monitor `500` "Outcome unknown"; the Connection is what the vPPB is |
| An Informational Binding Change record that shows what the task asked for, when its vPPB showed that before the command too, or was not read before it | taken for the end of the task: `Completed` | turned away: the VCSs are read again, and the task goes on until Background Operation Status says it is over |
| A bind or unbind task whose switch does not answer Background Operation Status five times, or answers five times that it is over with the vPPB at 01h or not read, while the vPPB reads 01h or cannot be read | `Exception`, "The switch does not answer Background Operation Status"; its Connection gone (none for a vPPB at 01h) and a `DELETE` of it no longer `503`; the task never corrected | `TaskState` `Interrupted`, no `EndTime` or `TaskStatus`, its monitor `202`; the Connection `Starting` or `Updating`, a `DELETE` `503`; the next read of the switch's VCSs (a Binding Change record, `--rescan`, a request) that sees the vPPB settled ends the task `Completed` or `Exception` on what it reads |
| The same when the vPPB, read then, has settled | `Exception` whatever it read | `Completed` if it reads what was asked (bound to the port and LD, or unbound), else `Exception`; the message says it was read from the vPPB |
| `#Cxl.ForceUnbind` of a Connection whose managed unbind was lost with the vPPB at 01h | `404`: the task had ended and the vPPB at 01h gave no Connection | option 2h sent; if the switch takes it, `202` with that task, `Running` again |
| `POST` or `DELETE` of a Connection when the task cannot be allocated | Bind vPPB or Unbind vPPB sent, then `500`, the switch carrying the operation out with no task | `500`, nothing sent |
| `GET` of a device on hardware whose Get Multi-Headed Info (5500h) or Get LD Info (5400h) failed at discovery with a code other than Unsupported or Unsupported Mailbox or CCI (Retry Required, Busy, Internal Error, Invalid Input), or because the command did not reach the device or its response did not come back (a timeout, a route that is down) | an SLD for good (its endpoint was not asked again), of capacity 0 if Identify Memory Device was not answered either | `404` until it is discovered whole: see "Devices that are not ready" below |
| The same for an MLD whose Get LD Allocations (5401h) failed that way or gave a reserved Memory Granularity, or whose second Get LD Info did, directly attached or on a switch port (and a switch-port MLD whose tunnelled Identify did) | an MLD for good, with LDs of 0 MiB and no `MemoryDomain` (or with no LDs) | likewise `404`, then the MLD whole |
| The same for an SLD whose Identify Memory Device (4000h) failed that way | an SLD of capacity 0 for good | likewise |
| `GET` of the `PCIeDevice` (`CXLDevice.DynamicCapacity`), `MemoryRegion`s and `MemoryExtent`s of a DCD on hardware whose Get DCD Info (5600h) failed at discovery other than with Unsupported (Retry Required, Internal Error) | no Dynamic Capacity, for good | none until a `--rescan` reads Get DCD Info (with the backoff and cap of "Devices that are not ready"); then the DCD as at discovery, its DC event notification asked for |
| `PATCH` of a `Memory`; `POST` of its `ScanMedia`, `SetPassphrase`, `SetMasterPassphrase`, `DisablePassphrase`, `DisableMasterPassphrase`, `FreezeSecurityState` or `SecureEraseUnit`; a `"Volatile"` chunk `POST` on an SLD with partitionable capacity, and the `DELETE` of such a chunk: on hardware on any device but an SLD on the kernel's mailbox (`--ioctl`), and on any device of a mock | carried out: the command sent as an MCTP message (tunnelled for an LD of an MLD) whenever a client asked | `409` with nothing sent or changed, unless cxl-fmd is started with the new `--allow-mctp-restricted` (CXL r4.0 Table 8-308 note 4; see "Commands restricted on MCTP") |
| `GET` of such a device's `Memory` | `Actions` with `ScanMedia` and the six security actions | no `Actions`, unless `--allow-mctp-restricted` |
| `POST` of an action of a `Memory` that is not there | `200` in a mock, `400` on hardware | `404` |
| `GET` of a `CXLLogicalDevice` of an SLD or MLD | no security actions | `Actions` with `#CXLLogicalDevice.SetPassphrase`, `DisablePassphrase`, `Unlock`, `FreezeSecurityState` and `PassphraseSecureErase`, each whose command the LD's CEL lists, when it may be sent (see "Security actions"); a DCD's DC actions beside them |
| `POST` of those actions | `404` | carried out, `200`; `400` for a missing or wrong parameter, a `PassphraseType` other than `User` or `Master` (`Master` on `Unlock`), or a passphrase of more than 32 bytes; `501` if the CEL does not list the command; `409` without `--allow-mctp-restricted` but on an SLD on the kernel's mailbox |
| `POST` of a security action of a pool LD of a Multi-Headed device (`.../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.Unlock` and the others) | `404` | `501`: cxl-fmd sends no Memory Device command to a pool LD, and its `CXLLogicalDevice` has none of these actions |
| `GET` of a `Memory`: its security actions | all six, on every device | as the `CXLLogicalDevice`'s: only those whose command the CEL lists. A device whose CEL could not be read, and a mock device whose `cel_commands` does not list them (the defaults), have none. The six are deprecated aliases |
| `POST` of a `Memory` security action whose command the CEL does not list | sent (`200`, or `400` if the device refused it) | `501`, nothing sent |
| `POST` of a `Memory` security action with a passphrase of more than 32 bytes | the first 32 bytes sent | `400`, nothing sent |
| `POST` of a `Memory` or `CXLLogicalDevice` security action the device answers with an error | `400` "Action failed" | Invalid Input, Incorrect Passphrase or Invalid Payload Length `400`; Invalid Security State `409`; Unsupported `501`; Retry Required or Busy `503`; any other `500`; the message names the return code |
| A mock topology's `cel_commands` | `get_security_state` to `passphrase_secure_erase` were not known | known (see [mock-topology.md](mock-topology.md)); an MLD's LDs have the MLD's `cel_commands`; `cxl_mld_0` of `mock/mld-device.json` lists all six |
| On hardware, a Dynamic Capacity action, a `MemoryRegion` `PATCH` or a `MemoryExtent` `DELETE` of a Multi-Headed device | the DCD Management command sent to the device's endpoint with no tunnel, whatever cxl-fmd knew of the route to its LD Pool CCI | sent on the route to the LD Pool CCI, the one Set LD Allocations goes on; `501`, with nothing sent, if no such route is known |
| On hardware, a Multi-Headed device whose head presents an MLD, found on that head's FM-owned LD first (on a switch port, or on an endpoint of its own) and on its LD Pool CCI | an MLD (`DeviceClass` and the IDs of an MLD, on the switch port when it was found there), its LD Pool CCI never asked | a Multi-Headed device: its heads and pool LDs, at the root, the head on the switch port when it can be told which it is; the MLD's `Chassis`, `PCIeDevice`, LDs and chunks are gone |
| On hardware, the same, found on its LD Pool CCI first | the Multi-Headed device, the other CCI dropped | the same, and a head placed on the switch port as above; that head's binding found by a vPPB is a Connection to its Endpoint, `Health` `Warning` |
| Any error response (`4xx`, `5xx`) | `{"error": {"code": "Base.1.0.GeneralError", "message": ...}}`, with no `@Message.ExtendedInfo` | `code` `Base.1.24.GeneralError`, the same `message`, and `@Message.ExtendedInfo` with a message of the DMTF Base registry 1.24.0 (`InvalidURI`, `PropertyMissing`, `MalformedJSON`, `ResourceInUse`, ...; see "Error Response Format") |
| A request with a query parameter that starts with `$` | `code` `Base.1.15.0.QueryParameterUnsupported`, and `MessageId` `Base.1.15.0.QueryParameterUnsupported` in `@Message.ExtendedInfo` | `code` `Base.1.24.GeneralError`, `MessageId` `Base.1.24.QueryParameterUnsupported`: a MessageId has one minor version, "<MessageRegistryPrefix>.<MajorVersion>.<MinorVersion>.<MessageKey>" (DSP0266 Section 9.5.11.2) |
| A Dynamic Capacity request the device accepted whose state could not be read back (`200`) | `code` `Base.1.0.Success` | `code` `Base.1.24.Success`, and `Base.1.24.Success` in `@Message.ExtendedInfo` |
| `Messages[].MessageId` of a `Task` | `Base.1.0.Success`, `Base.1.0.GeneralError` | `Base.1.24.Success`, `Base.1.24.GeneralError` |
| The task monitor of a bind or unbind the switch ended with Invalid Input (`bind_result` or `unbind_result` `invalid_input` in a mock) | `500`, `message` the task's ("Bind vPPB failed: cmd input was invalid") | `400`, the body a `POST` or `DELETE` the switch refused at once gets ("The switch refused the bind as Invalid Input"); the task's `Messages` keep the return code |
| The same, ended with Unsupported, or Busy or Retry Required | `500` | `501`, or `503`, as at once |
| The same, ended with another return code (Internal Error) | `500`, `message` the task's | `500`, `message` "Failed to create connection" or "Failed to delete connection", as at once |
| The task monitor of a task whose switch is gone | `500`, "The switch is gone" | `500`, "Outcome unknown: The switch is gone, and whether it bound (unbound) the vPPB before it went is not known" |
| A `POST`, `PATCH` or `DELETE` with `If-Match` or `If-None-Match` (a `PATCH` of a `MemoryRegion` with a stale `If-Match`) | the header ignored, the request carried out (Set DC Region Configuration sent) | `412`, nothing done; a `404` or `405` stays so (but for an action a resource that is there does not have, which is `412`); a `GET` is unchanged |
| `GET` of a `Chassis`, `PCIeDevice`, `CXLLogicalDevice`, `Memory`, `MemoryDomain` or `MemoryChunk` of a device whose Get Health Info has a Health Status bit set, or a Media Status other than 00h or 01h | no `Status.Conditions` | `Status.Conditions`, one per bit and per Media Status, with MessageIds `CxlHealth.1.0.<Key>` (see "Status from Get Health Info"); a switch's `Chassis` those of its devices |
| `GET` of the `Memory` of a device or LD whose Additional Status has Bit[4] or Bit[5] set | no `Status.Conditions`; the value was read and not used | `CorrectedVolatileErrorCount`, `CorrectedPersistentErrorCount` Conditions; `Health` unchanged |
| `GET /redfish/v1`, `/redfish/v1/odata`, `/redfish/v1/$metadata` | no `Registries` | `Registries`, `/redfish/v1/Registries`, with cxl-fmd's `CxlHealth` registry at `/redfish/v1/Registries/CxlHealth.1.0.0.json` |
| A mock topology's `health_info.additional_status` | ignored, though mock-topology.md listed it | read, for an SLD and for an LD in its own `health_info` |
| `GET` of any resource with `Oem.Cxl` (a host's `MemoryDomain`, a `Connection`, a DCD's `PCIeDevice`) | `Oem.Cxl` with no `@odata.type`, and no schema for it | `@odata.type` `#Cxl.v1_0_0.MemoryDomain`, `#Cxl.v1_0_0.Connection` or `#Cxl.v1_0_0.PCIeDevice`, the types of cxl-fmd's Oem schema (see "Oem schema") |
| `GET /redfish/v1/$metadata`; `GET /redfish/v1/Schemas/Cxl_v1.xml` | no reference to an Oem schema; `404` | a reference to `/redfish/v1/Schemas/Cxl_v1.xml`, which is served |
| `GET` of a DCD's `PCIeDevice`, `MemoryRegion` or `MemoryExtent` | sizes and offsets only in whole MiB, rounded down: a 64 KiB block size `BlockSizeMiB` 0 and not in `MemoryBlockSizesSupported`, two 64 KiB extents both `ExtentOffsetMiB` 1024, `ExtentSizeMiB` 0 | the same, and each in bytes in `Oem.Cxl` (`TotalDynamicCapacityBytes`, `BlockSizeBytes`, `RegionBaseOffsetBytes`, `RegionSizeBytes`, `ExtentOffsetBytes`, `ExtentSizeBytes`; see DCD-FM-API.md, Comment 3) |
| `GET` of a DCD's `CXLLogicalDevice` | `#CXLLogicalDevice.DynamicCapacityAdd` with no `Policy@Redfish.AllowableValues`, so nothing said that a `Prescriptive` add, which `AddCapacityPoliciesSupported` may list, is always `400` | `Policy@Redfish.AllowableValues`: `Free` and `Contiguous` when the device supports them, and `EnableSharedAccess`; never `Prescriptive`, which is still `400`. `AddCapacityPoliciesSupported` still lists what the device supports (review DCD-A5) |
| `DynamicCapacityAdd`, `DynamicCapacityRelease`, a `MemoryExtent` `DELETE` the device accepted | `200` (`204` for the `DELETE`) once the device had initiated it, with nothing to say whether the host took it | `202 Accepted`, a `Task` and its monitor in `Location`. The task records the request (`Payload`, `Oem/Cxl/DynamicCapacity`) and the host's extents before it, and is `Completed` once the host's 5603h shows the capacity accepted (`TaskStatus` `Warning` for less than asked) or released, `Exception` once the host is seen to accept none of an add or the device is gone, and `Running` otherwise. The monitor then answers `200` with a Success message (`204` for the `DELETE`), `409` for an add the host refused, `500` "Outcome unknown" for a device that is gone (review F06.6c, F13.C4, DCD-A1a, DCD-A1c) |
| A Dynamic Capacity add, release or `MemoryExtent` `DELETE` for a host and Tag a `Task` follows, or an add of a Tag whose `PendingLDs` has the host | sent | `409` `ResourceInUse`, nothing sent |
| On hardware, a Dynamic Capacity add, release or `MemoryExtent` `DELETE` whose host's last read failed or is owed | sent, on what was last read | the host read first; `503`, nothing sent, if it cannot be |
| `GET /redfish/v1/TaskService/Tasks` | binds and unbinds only | with the Dynamic Capacity tasks; one that is `Running` on hardware has its host read on every `--event-poll` tick |

Mock topologies and discovery:

- A DCD's `host_response` (`after_ticks`, `add`, `release`) makes the
  mock's hosts answer an add or a release late, in part, not at all, or
  never, so that a Dynamic Capacity `Task` can be seen `Running`,
  `Completed` with `TaskStatus` `Warning`, and in `Exception`; without it
  they answer at once, as before. `part` of an add on a device with a
  Sharable region, and a value that is none of these, are refused by
  `--check` (see mock-topology.md).

- Two components with one ID, and two LDs of an MLD with one LD-ID, are
  refused while the file is loaded ("two components with ID ...", "two
  LDs with LD-ID ..."), as they were by the validation after it. On
  hardware, a component discovered with an ID another one has is not
  added, and an error is logged.
- Devices that are not ready. What kind of device is on an endpoint, and
  what LDs it has of what size, is decided only on what it says: a
  command it answers, or one it does not support (Unsupported, "The
  command is not supported", or Unsupported Mailbox or CCI, CXL r4.0
  Table 8-208). A device that fails one of these commands otherwise
  (Retry Required is "a temporary error"), or that the command does not
  reach or whose response does not come back (a timeout, a route that is
  down), is not added, and is discovered again: on
  the next `--rescan` (for a device on a switch port, the next read of
  the switch's ports), then with 1, 3, 7, 15, 15, ... of them let pass
  between tries, and after 8 failed tries not again until cxl-fmd is
  restarted (for a switch port, until it reports another kind of device,
  or none); each step is logged. With `--rescan 0` a directly attached
  device that is not ready at start-up is not asked again. The
  specification says only "An optional single retry may resolve the
  issue": the backoff, the cap, taking every other code as one of the
  moment, and taking as absence only a transport that will never carry
  the command (a kernel mailbox that answers EPERM, ENOTTY, EOPNOTSUPP or
  ENOSYS) and a response libcxlmi will not use (a list longer than was
  asked for), which is the device's own answer, are cxl-fmd's choices. The commands that decide this are Get Multi-Headed Info
  (5500h), Get LD Info (5400h), Get LD Allocations (5401h), Identify
  Memory Device (4000h) of an SLD, and the tunnelled Identify of an MLD
  on a switch port; the others (Get Head Info, QoS, Get Partition Info,
  Get Health Info, the CEL, per-LD commands) do not keep a device from
  being added. Get DCD Info (5600h) is read the same way, but the device
  is added without it: whether it is a DCD is asked again on the same
  terms, on the `--rescan` timer only (with `--rescan 0`, not at all).

---

## References

- [DMTF DSP0288 - CXL to Redfish Mapping Specification](https://www.dmtf.org/dsp/DSP0288)
- [DMTF DSP0266 - Redfish Specification](https://www.dmtf.org/dsp/DSP0266)
- [DMTF DSP8010 - Redfish Schema Bundle](https://www.dmtf.org/dsp/DSP8010)
- [CXL Specification](https://www.computeexpresslink.org/download-the-specification/)
