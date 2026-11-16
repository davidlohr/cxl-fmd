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

cxl-fmd does not currently implement authentication. All endpoints are accessible without credentials. Only the `Sessions` collection, `/redfish/v1/SessionService/Sessions`, is served, linked from the service root's `Links/Sessions`; there is no `/redfish/v1/SessionService` resource, and no session is created or required.

For production deployments, place cxl-fmd behind a reverse proxy (e.g., nginx) that handles authentication.

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
| Get Health Info | Health Status | Many | `Status` | See Comment 3. | Partial: `Status/Health` and `Status/State` are served; `Status/Conditions`, which Comment 3 describes, is not |
| Get Health Info | Life Used | `MemoryMetrics` | `CapacityUtilizationPercent` | | No (deviation, see "Life Used" below) |
| Get Health Info | Corrected Volatile Error Count | `MemoryMetrics` | `CorrectedVolatileErrorCount` | | Yes |
| Get Health Info | Corrected Persistent Error Count | `MemoryMetrics` | `CorrectedPersistentErrorCount` | | Yes |
| Get Health Info | Additional Status[4]: Corrected Volatile Error Count | `Memory` | `Status/Conditions` | See Comment 4. | No |
| Get Health Info | Additional Status[5]: Corrected Persistent Error Count | `Memory` | `Status/Conditions` | See Comment 4. | No |
| Get Health Info | Dirty Shutdown Count | `MemoryMetrics` | `DirtyShutdownCount` | | Yes |
| Get Alert Configuration | Valid Alerts and Programmable Alerts | `MemoryMetrics` | `CXL/AlertCapabilities` | | No |
| Scan Media | N/A | `Memory` | `ScanMedia` action | | Yes |
| Get Security State | Security State | `Memory` | `SecurityStates` | | Yes |
| Set Passphrase | All | `CXLLogicalDevice` | `SetPassphrase` action | | No. Served on `Memory`, as DSP0288 1.2.0 mapped it (see "Security actions" below) |
| Disable Passphrase | All | `CXLLogicalDevice` | `DisablePassphrase` action | | No. Served on `Memory`, as DSP0288 1.2.0 mapped it |
| Freeze Security State | N/A | `CXLLogicalDevice` | `FreezeSecurityState` action | | No. Served on `Memory`, as DSP0288 1.2.0 mapped it |
| Passphrase Secure Erase | All | `CXLLogicalDevice` | `PassphraseSecureErase` action | | No. Served on `Memory` as `SecureEraseUnit`, as DSP0288 1.2.0 mapped it |
| Unlock | All | `CXLLogicalDevice` | `Unlock` action | | No |

### Comments for Table 1

**Comment 1:** A `LogEntry` resource is used to capture CXL event records. The `CXLEntryType` property shall contain the specific CXL entry type, and the `EntryType` property shall contain `CXL`. Overflow information is captured in properties of the `LogEntry` prior to the overflow condition.

**Comment 2:** A `Memory` resource can only contain a single `LogService`. This `LogService` resource shall contain the informational, warning, failure, and fatal event records as `LogEntry` resources. The `MaxNumberOfRecords` shall contain the sum of the informational, warning, failure, and fatal log sizes.

**Comment 3:** Redfish resources contain a `Status` property to show the health and state of a device. The `Conditions` property within the `Status` property is used to show why the `Health` property contains a value other than `OK`.

**Comment 4:** The `Conditions` property within the `Status` property of the `Memory` resource shall contain messages to indicate either the Corrected Volatile Error Count or Corrected Persistent Error Count flags are set.

### Security actions

DSP0288 1.2.0 mapped the security commands to actions of `Memory`: Set
Passphrase to `SetMasterPassphrase` (Passphrase Type 00h) and
`SetPassphrase` (01h), Disable Passphrase to `DisableMasterPassphrase`
(00h) and `DisablePassphrase` (01h), Freeze Security State to
`FreezeSecurityState`, and Passphrase Secure Erase to `SecureEraseUnit`.
DSP0288 1.3.0 changed Set Passphrase, Disable Passphrase, Freeze Security
State and Passphrase Secure Erase to the `SetPassphrase`,
`DisablePassphrase`, `FreezeSecurityState` and `PassphraseSecureErase`
actions of `CXLLogicalDevice`, and added Unlock (Annex A). cxl-fmd serves
the 1.2.0 mapping, on `Memory`, and none of these actions on
`CXLLogicalDevice` (see Known Gaps).

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
8.2.10.9.2.2).

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
| Get LD Allocations | Memory Granularity, LD Allocation List | `CXLLogicalDevice` | `MemorySizeMiB` | | Yes |
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
| Set Partition Info (4101h) | `MemoryChunks` | `POST` and `DELETE` of a `"Volatile"` chunk of an SLD (see "Memory Provisioning") |
| Bind vPPB (5201h) | `Connection` | `POST /redfish/v1/Fabrics/CXL/Connections` |
| Unbind vPPB (5202h) | `Connection` | `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}`, with Unbind Option 1h, Simulate Managed Hot-Remove |

Bind vPPB and Unbind vPPB have no actions: a `POST` to
`/redfish/v1/Fabrics/CXL/Connections` for a host on a switch binds a vPPB
of the host's VCS, and a `DELETE` of the connection unbinds it (see
"Memory Binding (Connections)").

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
- `POST /redfish/v1/Fabrics/CXL/Connections` - Create Connection (Bind vPPB for switch-attached)
- `DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}` - Delete Connection (Unbind vPPB for switch-attached)
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}` - Set Timestamp
- `PATCH /redfish/v1/Chassis/{id}/Memory/{mem}` - Set Partition Info
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia` - Scan Media
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetPassphrase` - Set User Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetMasterPassphrase` - Set Master Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisablePassphrase` - Disable User Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisableMasterPassphrase` - Disable Master Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.FreezeSecurityState` - Freeze Security State
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SecureEraseUnit` - Secure Erase Unit

An SLD that a switch reports on one of its ports (`{switch}_port{n}_dev`)
is sent none of these commands. It has no endpoint of its own, and the
switch cannot pass them on: "Valid targets for the tunneled commands
include switch MLD Ports, valid LDs within an MLD, and the LD Pool CCI in
a Multi-Headed device" (CXL r4.0 Section 7.6.7.3.1). Its `PCIeDevice` has
no `CXLDevice/Timestamp`, a `PATCH` of the timestamp returns 400, and the
`Memory` writes and actions fail. An MLD on a switch port is reached
through that port. In mock mode (`--mock`) no command is sent for any
device, and these requests act on the mock's state as for any other.
- `POST /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB` - Reset PPB
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}` - Set DC Region Configuration
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` - Initiate Dynamic Capacity Add
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` - Initiate Dynamic Capacity Release
- `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` - Initiate Dynamic Capacity Release from the extent's host only: Tag-based for an extent with a tag in a Sharable region, Prescriptive for any other (cxl-fmd's meaning of DELETE, see [DCD-FM-API.md](DCD-FM-API.md#delete-of-a-memoryextent))
- `POST /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory` - DC Add Reference / DC Remove Reference

---

## Error Responses

All error responses follow the Redfish error format with an `error` object containing a `code` and `message`.

### HTTP Status Codes

| Status | Meaning | When Used |
|--------|---------|-----------|
| 200 OK | Success | GET requests returning data; actions |
| 201 Created | Resource created | POST to collections (MemoryChunks; Connections that need no bind, or whose bind the switch finished before answering); the task monitor of a bind once it has succeeded |
| 202 Accepted | Started, not finished | POST and DELETE of a Connection when the switch binds or unbinds in the background, with a `Task` and its monitor in `Location`; the task monitor while the task runs (see "Binds and unbinds in the background") |
| 204 No Content | Success, no body | DELETE requests; the task monitor of an unbind once it has succeeded |
| 400 Bad Request | Invalid request | Malformed JSON, missing required fields, invalid values; a Connection whose host and device are not on the same switch, whose host cannot reach the chunk's LD through its head, whose chunk names no LD the MLD's port can bind, whose target is an LD Endpoint and chunk is on another LD, or whose target is the Endpoint of a head that presents an MLD; `ResetPPB` on a port that is not a DSP |
| 404 Not Found | Resource not found | Invalid resource ID or path |
| 405 Method Not Allowed | Method not supported | Using unsupported HTTP method on endpoint |
| 409 Conflict | Resource conflict | Capacity exceeded, invalid state transition; a Connection to a port or LD bound to another host's VCS, or for a host whose VCS has no unbound vPPB |
| 413 Payload Too Large | Request body too large | Body exceeds 64KB limit |
| 500 Internal Server Error | The operation failed | A bind or unbind the switch refused, or another failure; the task monitor of a task that ended in `Exception` |
| 501 Not Implemented | Feature not implemented | PATCH on read-only resources; POST to a resource that takes none |
| 503 Service Unavailable | Try again | A bind or unbind while the switch is Busy, after it answered Retry Required twice, or when the vPPB chosen was bound meanwhile; a DELETE of a Connection whose bind or unbind is under way |

Which status each case gets is cxl-fmd's choice; neither DSP0288 nor the CXL
specification sets it.

### Error Response Format

```json
{
    "error": {
        "@Message.ExtendedInfo": [{
            "Message": "Human-readable error description",
            "MessageId": "Base.1.0.GeneralError",
            "Resolution": "Suggested resolution",
            "Severity": "Critical"
        }],
        "code": "Base.1.0.GeneralError",
        "message": "Brief error summary"
    }
}
```

### Common Errors

**404 Not Found - Invalid Resource ID:**
```bash
curl -s http://localhost:8080/redfish/v1/Chassis/nonexistent | jq
```
```json
{
    "error": {
        "code": "Base.1.0.ResourceNotFound",
        "message": "Resource not found: nonexistent"
    }
}
```

**400 Bad Request - Missing Required Field:**
```bash
curl -s -X POST http://localhost:8080/redfish/v1/Fabrics/CXL/Connections \
  -H "Content-Type: application/json" \
  -d '{}' | jq
```
```json
{
    "error": {
        "code": "Base.1.0.PropertyMissing",
        "message": "Required property Links is missing"
    }
}
```

**409 Conflict - Capacity Exceeded:**
```bash
curl -s -X POST http://localhost:8080/redfish/v1/Chassis/dev/MemoryDomains/dom/MemoryChunks \
  -H "Content-Type: application/json" \
  -d '{"MemoryChunkSizeMiB": 999999999}' | jq
```
```json
{
    "error": {
        "code": "Base.1.0.ResourceExhaustion",
        "message": "Requested size exceeds available capacity"
    }
}
```

**413 Payload Too Large:**
```json
{
    "error": {
        "code": "Base.1.0.PayloadTooLarge",
        "message": "Request body exceeds maximum size of 65536 bytes"
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

**MemoryDomains from Connections:** A host's MemoryDomains are dynamically derived from active Connections where that host is the initiator. Each Connection creates a MemoryDomain exposing the connected device's memory chunk to the host. The MemoryDomain ID matches the Connection ID (e.g., `conn0`).

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
| `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` | Release the extent from its host only |
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

What a vPPB is bound to: an SLD for Binding Status 02h on its port, the LD
with the Bound LD ID for 03h on an MLD's port, the pool LD mapped to the
head for 02h on the port of a head that presents an SLD, and the head for
03h on the port of a head that presents an MLD. The vPPB's
`Links.Endpoints` is that Endpoint; a Port's `Links.AssociatedEndpoints`
is each target Endpoint at the other end of its link; a
`CXLLogicalDevice`'s `Links.Endpoints` is its own Endpoint; a
`VirtualCXLSwitch`'s `Links.Endpoints` is its Zone's.

`POST /redfish/v1/Fabrics/CXL/Connections` takes an LD Endpoint as the
target, with a chunk of that LD (400 if the chunk is on another LD), and a
head Endpoint is refused with 400. For compatibility, the ID of an MLD or
of a Multi-Headed device, which have no Endpoint any more, is still taken
as the target, with the LD the chunk is on.

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
| `.../Memory/{mem}/Actions/Memory.SetPassphrase` | POST | Set user passphrase |
| `.../Memory/{mem}/Actions/Memory.SetMasterPassphrase` | POST | Set master passphrase |
| `.../Memory/{mem}/Actions/Memory.DisablePassphrase` | POST | Disable user passphrase |
| `.../Memory/{mem}/Actions/Memory.DisableMasterPassphrase` | POST | Disable master passphrase |
| `.../Memory/{mem}/Actions/Memory.FreezeSecurityState` | POST | Freeze security state |
| `.../Memory/{mem}/Actions/Memory.SecureEraseUnit` | POST | Secure erase |

**Note:** vPPB Bind/Unbind operations are performed automatically through the Connections API (see Memory Binding section below).

---

## Memory Provisioning

Memory provisioning for CXL devices is performed through the MemoryChunks collection.

**Provisioning Model:** During discovery, cxl-fmd queries existing LD allocations from the device and creates MemoryChunks for already-allocated memory. The remaining (unallocated) capacity is tracked in the MemoryDomain but not represented as chunks until explicitly provisioned via POST. DELETE returns capacity back to the available pool.

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

**With InterleaveSets (preferred for MLD):**
```http
POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
Content-Type: application/json

{
    "InterleaveSets": [{
        "Memory": {"@odata.id": "/redfish/v1/Chassis/{id}/Memory/{ld}"},
        "SizeMiB": 1024
    }]
}
```

**Request Body:**
| Property | Type | Required | Description |
|----------|------|:--------:|-------------|
| `MemoryChunkSizeMiB` | integer | Conditional | Size of the chunk in MiB (required unless InterleaveSets.SizeMiB provided) |
| `AddressRangeType` | string | No | `"Volatile"` or `"PMEM"` (default: `"PMEM"`) |
| `AddressRangeOffsetMiB` | integer | No | Base offset in MiB (auto-assigned if omitted) |
| `InterleaveSets` | array | No | Array of interleave set entries (see below) |

**InterleaveSets Entry:**
| Property | Type | Description |
|----------|------|-------------|
| `Memory` | object | Reference to Memory resource (`{"@odata.id": "..."}`) |
| `SizeMiB` | integer | Size contributed by this entry in MiB |
| `MemoryLevel` | integer | Interleave level (default: 1) |
| `OffsetMiB` | integer | Offset within the interleave set (default: 0) |
| `RegionId` | string | Region identifier |

**Response:** Returns the created MemoryChunk resource with HTTP 201 Created.

For MLD chunks, the response includes `Links.CXLLogicalDevices` referencing the target LD.

**Hardware Behavior:**
- **MLD**: Issues `Set LD Allocations (0x5402h)` to increase the LD's memory allocation.
  The LD's Range 2 is kept as `Get LD Allocations (0x5401h)` reads it just
  before, and Range 1 takes the rest. The new size must be a multiple of
  `MemoryChunkIncrementMiB` (the Memory Granularity, Table 7-48); one that
  is not is refused with 400, as is one smaller than Range 2. The same
  holds for an MHD's pool LDs.
- **SLD**: A `"Volatile"` chunk issues `Set Partition Info (0x4101h)` for
  the volatile capacity staged (or else active) plus the chunk, worked out
  and checked as in "Set Partition Info" above. Immediate is cleared: the
  device takes the new partitioning at its next reset, yet the chunk is
  in the collection at once (see Known Gaps). Persistent data is lost if
  the persistent capacity changes. A `"PMEM"` chunk sends no command: it
  only divides cxl-fmd's own record of the domain.

### Deleting Memory Chunks (DELETE)

```http
DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
```

**Response:** HTTP 204 No Content on success.

**Hardware Behavior:**
- **MLD**: Issues `Set LD Allocations (0x5402h)` to decrease the LD's memory allocation
- **SLD**: A `"Volatile"` chunk issues `Set Partition Info (0x4101h)` for
  the volatile capacity staged (or else active) less the chunk, staged for
  the next reset as on creation. A `"PMEM"` chunk sends no command.

**While bound (409 Conflict):** a chunk that a switch-attached Connection
exposes (one that is not Absent: Starting, Updating and Enabled ones
included) is not deleted, and no chunk is created that would change the
allocation of a bound LD (Set LD Allocations) or, `"Volatile"`, the
partition of a bound SLD (Set Partition Info). Delete the Connection
first. The CXL specification does not say whether an LD or SLD that is
bound may be changed; refusing it is cxl-fmd's choice. Direct-attached
Connections, which have no binding to undo, do not hold their chunks.

### MLD Memory Model

For Multi-Logical Devices (MLDs), the memory hierarchy is:
- **MLD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → One domain per MLD, represents the device's total capacity
- **Memory Chunk** → One chunk per Logical Device (LD), 1:1 mapping
- **Memory** → Individual LDs as memory resources

For Phase 1 (direct-attached devices):
- Each LD maps to exactly one chunk
- InterleaveSets has a single entry identifying the target LD
- POST resizes an existing LD's allocation
- DELETE reduces the LD's allocation

Chunk IDs:

- the chunk of an LD, found at discovery: `{device}_ld{N}_chunk` (e.g.,
  `cxl_mld_0_ld0_chunk`), for an MLD and for a pool LD of a Multi-Headed
  device;
- the chunk of an SLD, found at discovery: `{device}_chunk0`;
- a chunk made with `POST`: `{domain_id}_chunk{N}`, N the number of chunks
  the domain had before it;
- in the `{switch}_pool` domain of a switch's Chassis: `{device}_chunk` for
  an SLD on one of its ports, `{device}_ld{N}_chunk` for an LD of an MLD.

### MHD Memory Model

For Multi-Headed Devices (MHDs), the memory hierarchy is:
- **MHD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → One domain covering the entire LD Pool
- **Memory Chunk** → One chunk per pool LD
- **Memory** → Individual pool LDs as memory resources
- **CXLLogicalDevices** → Pool LDs indexed by pool index (0 to total_lds-1)

Key differences from MLD:
- LD Pool can have **more than 16 LDs** (unlike MLD's 16 max)
- Each head sees **head-local LD-IDs** (0 to n-1), which map to pool indices
- FM uses **pool indices** in Redfish (not head-local IDs)
- Connections validate that the chunk's pool LD is accessible from the host's head

**LD-ID Translation:**
```
Host 0 connects to Head 0: sees LDs 0,1,2 → maps to pool LDs 2,3,4
Host 1 connects to Head 1: sees LDs 0,1   → maps to pool LDs 0,5
FM Redfish view: all pool LDs 0-5 visible as CXLLogicalDevices/0 through /5
```

**Hardware Behavior:**
- POST uses LD Pool CCI `Set LD Allocations (0x5402h)` with pool index
- Connection creation validates host's head can access the chunk's pool LD

### SLD Memory Model

For Single Logical Devices (SLDs):
- **SLD Device** → Chassis with `ChassisType: "Component"`
- **Memory Domain** → Represents the device's total capacity
- **Memory Chunk** → Volatile and/or persistent partitions
- **Memory** → Single memory resource for the SLD

---

## Memory Binding (Connections)

Memory binding assigns provisioned memory chunks to host systems, making them visible to the host's operating system. This is performed through the Connections API.

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
| `ConnectionType` | string | No | Connection type, should be `"Memory"` |
| `Links.InitiatorEndpoints` | array | Yes | Host endpoint(s) with `EntityRole: Initiator` |
| `Links.TargetEndpoints` | array | Yes | Target endpoint: an SLD or an LD (`{dev}_ld{N}`, with a chunk of that LD); the ID of an MLD or a Multi-Headed device is still taken, for compatibility |
| `MemoryChunkInfo` | array | Yes | Memory chunk(s) to bind |
| `MemoryChunkInfo[].MemoryChunk` | object | Yes | Reference to the MemoryChunk |
| `MemoryChunkInfo[].AccessCapabilities` | array | No | `["Read"]`, `["Write"]`, or `["Read", "Write"]` (default) |

**Response:** `201 Created` and the Connection when no bind is needed (a
direct-attached host, or a port or LD already bound to the host's VCS) or
the switch finished the bind before answering; `202 Accepted` with a
`Task`, and the task monitor in `Location`, when the switch binds in the
background (see "Binds and unbinds in the background" below). The errors
are in "HTTP Status Codes".

**Hardware Behavior (Switch-Attached Hosts):**
- Automatically finds an unbound vPPB in the host's VCS (409 if there is none)
- Issues `Bind vPPB (0x5201h)` to bind the device's port to the vPPB: for an
  MLD, its LD, by the LD-ID of the chunk (400 if the chunk names none); for
  an SLD, the whole port, with LD ID FFFFh. The port of a head of a
  Multi-Headed device is bound as a whole, with LD ID FFFFh: that is
  cxl-fmd's assumption, that the head presents an SLD. A head that
  presents an MLD binds LD-IDs "from 0 to n-1 relative to that head" (CXL
  r4.0 Section 2.5.1); cxl-fmd has no map from a pool LD to them, and such a
  bind is refused (a known gap)
- A Retry Required answer is sent once more ("An optional single retry may
  resolve the issue", Table 8-208); a second one, or Busy, is answered with
  503

**Binds and unbinds in the background:**
"All binding operations are executed as background commands" (CXL r4.0 Section
7.6.7.2.2). A switch answers Bind vPPB or Unbind vPPB with Background Command
Started and finishes later, one such command at a time. cxl-fmd does not wait:

| Request | Answer | Then |
|---------|--------|------|
| `POST .../Connections` | `202 Accepted`, a `Task`, `Location:` the task monitor | The connection exists with `Status.State` `Starting`; it is made, or removed, when the bind is over |
| `DELETE .../Connections/{id}` | `202 Accepted`, a `Task`, `Location:` the task monitor | The connection stands, with `Status.State` `Updating`, until the vPPB is unbound |
| Either, while the switch has an operation under way | `503 Service Unavailable` | Try again |
| `GET /redfish/v1/TaskService/TaskMonitors/{id}` | `202` and the `Task` while it runs; then `201` and the connection for a bind, `204` for an unbind, or the error | |

The task is at `/redfish/v1/TaskService/Tasks/{id}`. Its `PercentComplete` is
the Percentage Complete of Background Operation Status (0002h), which the event
loop reads until the operation is over; the return code there, and what the
vPPB is afterwards, decide between `Completed` and `Exception`. A switch that
finishes a bind before it answers is answered for with `201`, and no task.
A task whose switch is gone, or does not answer Background Operation Status
five times running, ends in `Exception`, and its connection is no longer
`Starting` or `Updating`: it is what the vPPB is when read again (or as last
read). After a lost bind, a vPPB that is bound or still busy keeps the
connection, which a DELETE unbinds; an unbound one removes it. After a lost
unbind, an unbound vPPB removes the connection, and a bound one leaves it
standing.
- Records binding info (`vcs_id`, `vppb_id`) in the connection

**MHD Direct-Attached Validation:**
For MHD connections, the server validates that the host can access the chunk's LD:
1. Finds which head the host is attached to (via `head.attached_host_id`)
2. Extracts the pool LD index from the chunk's InterleaveSets
3. Verifies `ld.binding_point_id == head.head_id` (LD is mapped to the head)
4. Returns HTTP 400 if the LD is not accessible from the host's head

Example error: Host 1 trying to access an LD mapped to Head 0 will be rejected.

**Host Visibility:**
After creating a connection, the bound memory appears in the host's Systems view:
- `GET /redfish/v1/Systems/{host}/MemoryDomains` lists connected memory
- Each connection creates a virtual MemoryDomain under the host

### Deleting Connections (DELETE)

```http
DELETE /redfish/v1/Fabrics/CXL/Connections/{conn_id}
```

**Response:** `204 No Content` when no unbind is needed or the switch
finished it before answering; `202 Accepted` with a `Task` when the switch
unbinds in the background (see "Binds and unbinds in the background").

**Hardware Behavior (Switch-Attached Hosts):**
- Issues `Unbind vPPB (0x5202h)` with Unbind Option 1h, Simulate Managed
  Hot-Remove, if the connection bound the vPPB
- Releases the vPPB for reuse
- The task follows the unbind for as long as the switch takes: no time
  limit is set, and the switch, which runs one background command at a
  time, answers every other bind and unbind Busy (503) until the host has
  released the device (a known gap)

### Connection Lifecycle

The complete memory lifecycle is:

1. **Discovery** - Devices and hosts are discovered, topology built. Existing LD allocations are represented as MemoryChunks; remaining capacity is tracked for future provisioning.
2. **Provisioning** - `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` allocates memory from the remaining capacity, creating a new chunk.
3. **Binding** - `POST /redfish/v1/Fabrics/CXL/Connections` binds a chunk to a host (vPPB bind for switch-attached).
4. **Unbinding** - `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}` releases the binding (vPPB unbind).
5. **Deprovisioning** - `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` returns capacity to the available pool, once no switch-attached Connection exposes the chunk (409 until then).

### Direct-Attached vs Switch-Attached

| Topology | vPPB Binding | Connection Effect |
|----------|--------------|-------------------|
| Direct-attached | Not applicable | Creates logical association only |
| Switch-attached | Automatic | Binds vPPB to port/LD via FM-API |

For direct-attached hosts (`host.direct_attached = true`), connections track the host-to-device association but no vPPB binding occurs.

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
removal by generating Virtual CXL Switch Event Records" (Section 7.6.6.8),
which cxl-fmd does not act on (see Known Gaps); for an SLD or PCIe device on
a bound port, "the FM can be notified but is not involved" (Section
7.6.6.7), and the removal may be seen only on the `--rescan` timer.

Its connections are the switch's to decide, and can change without a request:

| The device was | The switch | The connection |
|----------------|------------|------------|
| An MLD, removed by surprise | "The switch will automatically unbind any existing LD bindings" (Section 7.6.6.8) | Goes, once the vPPBs are read as unbound. Nothing deletes it |
| An MLD, removed by a managed hot-removal | Nothing: "The FM unbinds each assigned LD with the Unbind vPPB command as it receives notifications from each host" (Section 7.6.6.7), and cxl-fmd does not (see Known Gaps) | Stands for as long as its vPPB is bound |
| An SLD on a bound port | The specification does not say what becomes of the binding: "the FM can be notified but is not involved" (Section 7.6.6.7). A vPPB bound to an SLD port "behaves the same as a PPB in a PCIe switch" (Section 7.2.3.1), so cxl-fmd assumes it stays bound to the port | Stands, with `Status.State` `Absent` and no `TargetEndpoints` or `MemoryChunkInfo`. `DELETE` unbinds the vPPB as usual. A device plugged back into the port is the host's again, and the connection has its device again |
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

1. Dynamic capacity: an extent's `Pending` comes from its host's bit of
   the Pending Reference Bitmap of its tag (5608h), so it is per tag, not
   per extent, and only for extents that the host's 5603h lists. An add a
   device does not list shows only in `Oem/Cxl/DynamicCapacityTags` of the
   `PCIeDevice`, which has no schema (the Redfish Service Validator is run
   with `--nooemcheck`). Add and release answer `200 OK` with no `Task`. A
   Prescriptive add cannot be named through Redfish, as `POST` on
   `MemoryExtents` is not served. A host past LD 255 gets nothing from the
   tag list. The Host ID of a Multi-Headed DCD is taken to be the pool
   LD-ID, unverified. See Comments 4 and 5 and the Open Questions of
   [DCD-FM-API.md](DCD-FM-API.md#comments-for-table-3).
2. The managed hot-removal of an MLD (CXL r4.0 Section 7.6.6.7) is not
   carried out (see "Event logs and switch events" below).
3. Connections are not counted. A second Connection to a port or LD
   already bound to the host's VCS adopts the binding, and only the
   Connection that made the binding unbinds it when deleted, even while
   another stands. Deleting an adopted Connection, or one that names a
   binding made before cxl-fmd started, does not unbind (see "Binding").
4. No Connection is made for a vPPB bound by someone other than cxl-fmd.
   The Connections cxl-fmd serves for each device have no
   `InitiatorEndpoints`, whatever the device's vPPBs are bound to (see
   "Binding").
5. The security actions are served on `Memory`, not on
   `CXLLogicalDevice`, where DSP0288 1.3.0 and 1.4.0 put them (see
   "DSP0288 mappings").
6. The event logs of a Multi-Headed device are not read (see "Event logs
   and switch events").
7. A device on a switch port is taken to be removed when Get Physical Port
   State reports a different Connected Device Type, or none. The port's
   Port Presence Pin State (PRSNT#, Bit[2] of Link State Flags, Table 7-21)
   and its Supported CXL Modes are not used to decide it (see "When a
   Device Is Removed").
8. An unbind is followed for as long as the switch takes, with no time
   limit and no abort (see "Binding").
9. `Status/Conditions` is not served. The `Status` of the Chassis of an MLD,
   a Multi-Headed device or a switch is worked out from the Health Status
   and Media Status of all its LDs or devices together. The Health Status
   bits are OR'd and then compared as a number: 3 or more is `Critical`
   with `State` `UnavailableOffline`. So Maintenance Needed with
   Performance Degraded, or one LD's Media Status, makes the whole Chassis
   `Critical` and `UnavailableOffline`, and the Chassis can disagree with
   the device's `PCIeDevice`.
10. `AddressRangeType` is `Volatile` only for a device or LD with volatile
    and no persistent capacity. Every other one is `PMEM`, a device with
    both kinds of capacity or with unknown capacities included. A `POST` of
    a MemoryChunk without it makes a `PMEM` chunk.
11. Set Partition Info, Scan Media and the security commands are sent
    over MCTP without knowing whether a host is using the device's
    mailboxes (see "Memory device writes").
12. libcxlmi fails a Get Event Records response that reports more than 20
    records (`CXLMI_MAX_SUPPORTED_EVENT_RECORDS`). A component "shall limit
    the output payload size of commands with variably sized outputs such
    that the Message Payload does not exceed" its Response Message Limit
    (Section 8.2.10.1.3), at most 1024 bytes (Table 8-218), so a page from a
    component that keeps to it holds at most 7 records; cxl-fmd does not
    check that one does. A page read through the kernel's mailbox (ioctl)
    could hold more, but cxl-fmd reads no event log that way.

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

- An unbind is followed for as long as the switch takes: there is no time
  limit, and no abort. "The timeout value for the host handshakes to
  complete is implementation specific" (Section 7.6.6.7). A host that does
  not release the device keeps the task running, and the switch, busy with
  that background command, refuses every other bind and unbind (503).
- The port of a Multi-Headed device's head is always bound whole, with LD
  ID FFFFh. A head that presents an MLD cannot be bound per LD: nothing maps
  a pool LD to the LD-ID the head uses for it.
- A host's binding is seen only on a Connection cxl-fmd made, or on the
  vPPB: a vPPB bound before cxl-fmd started, or by another FM, gets no
  Connection with `InitiatorEndpoints`, and the host's `MemoryDomains` do
  not list it. An MLD is one `Endpoint`, whichever of its LDs a host has.

### DSP0288 mappings

These are known gaps against DSP0288 1.4.0 (see Tables 1 and 2):

- The security actions are served on `Memory`, as DSP0288 1.2.0 mapped
  them, not as actions of `CXLLogicalDevice`, where 1.3.0 and 1.4.0 map
  them; `Unlock` is not served at all.
- `Status/Conditions` is not served: Comments 3 and 4 of Table 1 are not
  met.

### Memory device writes

These are known gaps in what cxl-fmd does:

- Set Partition Info, Scan Media, Set and Disable Passphrase, Freeze
  Security State and Passphrase Secure Erase are marked O with note 4 for
  MCTP in Table 8-308, which reads: "Systems capable of management from
  Mailbox registers and an MCTP-based CCI shall ensure that these commands
  are not issued as MCTP messages while a device's mailboxes are
  operational." cxl-fmd sends them over MCTP whenever a client asks, and
  does not know whether a host is using the device's mailboxes. Use these
  operations only on a device no host is driving.
- A `"Volatile"` chunk of an SLD is in the collection as soon as it is
  created, though the partition change it asked for is staged and takes
  effect only at the device's next reset; the chunk has no state that says
  so.

### Properties not served

Properties and links defined in DMTF schemas but not yet implemented:

#### MemoryChunks

| Property | Schema | Notes |
|----------|--------|-------|
| `MediaLocation` | MemoryChunks.v1_5_0+ | Location of memory media (e.g., "Local", "Remote"). |
| `RequestedOperationalState` | MemoryChunks.v1_5_0+ | Requested operational state for the chunk. |

#### CXLLogicalDevice

| Property | Schema | Notes |
|----------|--------|-------|
| `Links.PCIeFunctions` | CXLLogicalDevice.v1_0_0+ | Link to associated PCIe functions. Requires PCIeFunction resource implementation. |

---

## References

- [DMTF DSP0288 - CXL to Redfish Mapping Specification](https://www.dmtf.org/dsp/DSP0288)
- [DMTF DSP0266 - Redfish Specification](https://www.dmtf.org/dsp/DSP0266)
- [DMTF DSP8010 - Redfish Schema Bundle](https://www.dmtf.org/dsp/DSP8010)
- [CXL Specification](https://www.computeexpresslink.org/download-the-specification/)
