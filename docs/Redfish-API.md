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
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetPassphrase` - Set User Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetMasterPassphrase` - Set Master Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisablePassphrase` - Disable User Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisableMasterPassphrase` - Disable Master Passphrase
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.FreezeSecurityState` - Freeze Security State
- `POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SecureEraseUnit` - Secure Erase Unit
- `POST /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB` - Reset PPB
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}` - Set DC Region Configuration
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` - Initiate Dynamic Capacity Add
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` - Initiate Dynamic Capacity Release
- `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` - Initiate Dynamic Capacity Release from the extent's host only: Tag-based for an extent with a tag in a Sharable region, Prescriptive for any other (cxl-fmd's meaning of DELETE, see [DCD-FM-API.md](DCD-FM-API.md#delete-of-a-memoryextent))
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
other.

The write operations on a `Connection`, which no DSP0288 rule maps (see
"cxl-fmd mapping (not in DSP0288)" above), are cxl-fmd's:

- `POST /redfish/v1/Fabrics/CXL/Connections` - Bind vPPB for a switch-attached host; for a direct-attached pair, the Connection it has already
- `DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}` - Unbind vPPB, Simulate Managed Hot-Remove (switch-attached only)
- `POST /redfish/v1/Fabrics/CXL/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` - Unbind vPPB, Simulate Surprise Hot-Remove (cxl-fmd's Oem action)

---

## Error Responses

All error responses follow the Redfish error format with an `error` object containing a `code` and `message`.

### HTTP Status Codes

| Status | Meaning | When Used |
|--------|---------|-----------|
| 200 OK | Success | GET requests returning data; actions; a `POST` of a Connection the host has already, switch-attached (bound, `Absent` included) or direct-attached, with the Connection and `Location` |
| 201 Created | Resource created | POST to collections (MemoryChunks; a Connection whose bind the switch finished before answering); the task monitor of a bind once it has succeeded |
| 202 Accepted | Started, not finished | POST and DELETE of a Connection, and `#Cxl.ForceUnbind`, when the switch binds or unbinds in the background, with a `Task` and its monitor in `Location`; a `POST` of a Connection that is being bound (`Starting`), with the task of that bind; the task monitor while the task runs (see "Binds and unbinds in the background") |
| 204 No Content | Success, no body | DELETE requests, and `#Cxl.ForceUnbind`, finished at once; the task monitor of an unbind once it has succeeded |
| 400 Bad Request | Invalid request | Malformed JSON, missing required fields, invalid values; a Connection `POST` with other than one initiator and one target, whose host and device are not on the same switch, whose direct-attached host has no Connection to the device or head, whose chunk is not on the target, whose chunks are on more than one LD, whose MLD target names no LD (no chunk), whose `AccessCapabilities` are not `Read` and `Write`, or whose target is, or is reached through, a head that presents an MLD; a bind or unbind the switch answered Invalid Input; `#Cxl.ForceUnbind` of a direct-attached Connection; `ResetPPB` on a port that is not a DSP |
| 404 Not Found | Resource not found | Invalid resource ID or path; the initiator, target or chunk a Connection `POST` names; a Connection whose vPPB no longer holds what its ID names (a stale ID), on GET, DELETE and `#Cxl.ForceUnbind` |
| 405 Method Not Allowed | Method not supported, with an `Allow` header | `PATCH` of a Connection (`Allow: GET, DELETE`); `DELETE` of a direct-attached Connection (`Allow: GET`) |
| 409 Conflict | Resource conflict | Capacity exceeded, invalid state transition; a Connection to a port or LD bound, or being bound or unbound, for another vPPB (another host's VCS, or the whole port while an LD of it is, or the reverse); for a host whose VCS has no unbound vPPB; a `POST` of a Connection the host has that is being unbound (`Updating`); a chunk `DELETE`, or a chunk `POST` that would change a bound LD or SLD, while a switch-attached Connection exposes it |
| 413 Payload Too Large | Request body too large | Body exceeds 64KB limit |
| 500 Internal Server Error | The operation failed | A bind or unbind the switch failed (Internal Error, or another code), or another failure; the task monitor of a task that ended in `Exception` |
| 501 Not Implemented | Feature not implemented | PATCH on read-only resources; POST to a resource that takes none; a bind or unbind the switch answered Unsupported (Bind vPPB and Unbind vPPB are optional, "O" in CXL r4.0 Table 7-31) |
| 503 Service Unavailable | Try again (no `Retry-After` header is sent) | A bind or unbind while the switch is Busy, after it answered Retry Required twice, or when the vPPB chosen was bound meanwhile; a DELETE or `#Cxl.ForceUnbind` of a Connection whose bind or unbind is under way (see "Forcing an Unbind" for the one exception) |

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
an MLD component" (CXL r4.0 Section 7.2.3.1).

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
| `.../Memory/{mem}/Actions/Memory.SetPassphrase` | POST | Set user passphrase |
| `.../Memory/{mem}/Actions/Memory.SetMasterPassphrase` | POST | Set master passphrase |
| `.../Memory/{mem}/Actions/Memory.DisablePassphrase` | POST | Disable user passphrase |
| `.../Memory/{mem}/Actions/Memory.DisableMasterPassphrase` | POST | Disable master passphrase |
| `.../Memory/{mem}/Actions/Memory.FreezeSecurityState` | POST | Freeze security state |
| `.../Memory/{mem}/Actions/Memory.SecureEraseUnit` | POST | Secure erase |
| `/redfish/v1/Fabrics/CXL/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` | POST | Unbind vPPB with Simulate Surprise Hot-Remove (cxl-fmd's Oem action, switch-attached Connections only) |

**Note:** vPPB Bind and Unbind are done through the Connections API, a
`POST` binding and a `DELETE` unbinding (see "Memory Binding
(Connections)" below). A vPPB has no action, and a `PATCH` of its
`Links.Port` is not served.

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
| `MemoryChunkSizeMiB` | integer | Conditional | Size of the chunk in MiB, greater than 0 (required unless InterleaveSets.SizeMiB provided) |
| `AddressRangeType` | string | No | `"Volatile"` or `"PMEM"` (default: `"PMEM"`); any other value is `400` |
| `AddressRangeOffsetMiB` | integer | No | Offset of the chunk in its domain, in MiB: from 0 to the domain's `MemorySizeMiB` less `MemoryChunkSizeMiB`. If omitted, the lowest offset where the chunk fits (first fit) |
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

Every size and offset in MiB, `InterleaveSets` entries included, is an
integer from 0 to 17592186044415 (2^44 - 1, the most MiB a byte count of
64 bits holds); one that is not, is negative or is past it is `400`, as is
a chunk that would not lie within its domain. `AddressRangeOffsetMiB` is
cxl-fmd's coordinate within the domain: DSP0288 maps no CXL field to it,
and no CXL command places an allocation at an offset, so it is never sent
to a device. Nothing is changed or sent for a request refused with `400`.

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
- **Memory Chunk** → One chunk per pool LD
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
- POST uses LD Pool CCI `Set LD Allocations (0x5402h)` with pool index
- A head with one pool LD on a switch port is bound whole; a head that
  presents an MLD is not bound (see "Memory Binding (Connections)")

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
Connection is cxl-fmd's choice. It derives one, always `Enabled`, for:

- the only direct-attached host (the `local` host of a topology with no
  switch) with each SLD and MLD that is on no switch port; for an MLD, its
  `TargetEndpoints` are all of its LDs. What an MLD on a root port gives
  its host, the specification does not say. With more than one
  direct-attached host, nothing says whose link a device is on, and there
  is none;
- each head of a Multi-Headed device that is on no switch port and whose
  host is known, with the pool LDs that the LD Map (Table 7-61) maps to
  that head. Only a mock topology says which host a head is attached to
  (`attached_host`): no command maps a head to a host.

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
    "Oem": {"Cxl": {"Origin": "Discovered"}},
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
  7-35): `AccessCapabilities` is always `Read` and `Write`, and
  `AccessState` `Optimized` (cxl-fmd's choice). Left out while `Absent`,
  and when the target is not known
- `Oem.Cxl.Origin` (switch-attached only): see "Forcing an Unbind"
- `Actions.Oem["#Cxl.ForceUnbind"]` (switch-attached only): see "Forcing
  an Unbind"

`Status` uses the standard `State` values; what each means here is
cxl-fmd's choice:

| `State` | `Health` | When |
|---------|----------|------|
| `Starting` | `OK` | cxl-fmd's bind of it is under way |
| `Updating` | `OK` | cxl-fmd's unbind of it is under way |
| `Enabled` | `OK` | bound, and what it reaches is known; every direct-attached Connection |
| `Enabled` | `Warning` | bound, and what it reaches is not known: an LD of a head that presents an MLD (see "Endpoints"), or an LD-ID the MLD does not have |
| `Absent` | `OK` | bound, and no device is on the port (see "When a Device Is Removed") |

A `PATCH` of a Connection is answered `405 Method Not Allowed`, `Allow:
GET, DELETE`: everything in it is what the switch says. Connection_v1
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
| `GET /redfish/v1/TaskService/TaskMonitors/{id}` | `202` and the `Task` while it runs; then `201` and the Connection for a bind, `204` for an unbind, or the error | |

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

However a task ends, what was under way is forgotten, and the Connection is
what the vPPB is: completed, failed, its switch gone, five reads of
Background Operation Status that failed, or no task made after the switch
started. After a task is lost the switch is read again if it answers, and
a vPPB that still reads busy (01h) gives no Connection until it reads
otherwise. The specification sets no time limit on a bind or an unbind,
and cxl-fmd does not abort one (see "Forcing an Unbind").

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
  leaving the managed unbind as it was. A bind under way, or an unbind
  already forced, is `503`.
- A direct-attached Connection has no vPPB to unbind: no action is
  offered, and a `POST` to its action URI is `400`.

`Oem.Cxl.Origin` of a switch-attached Connection is `"Service"` when this
run of cxl-fmd made the binding and `"Discovered"` otherwise. It is
advisory, never decides what a `DELETE` does, and is lost on restart:
nothing the switch reports says who made a binding (Tables 7-34 and 7-80),
so after a restart every binding is `"Discovered"`. Both `Oem` properties
are cxl-fmd's and have no schema.

### Connection Lifecycle

The complete memory lifecycle is:

1. **Discovery** - Devices and hosts are discovered, topology built. Existing LD allocations are represented as MemoryChunks; remaining capacity is tracked for future provisioning. Each vPPB already bound is a Connection.
2. **Provisioning** - `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` allocates memory from the remaining capacity, creating a new chunk. Not on an LD or SLD that is bound (409).
3. **Binding** - `POST /redfish/v1/Fabrics/CXL/Connections` binds the SLD or LD to a host (vPPB bind for switch-attached). The host gets a `MemoryDomain` once the Connection is `Enabled`.
4. **Unbinding** - `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}` releases the binding (vPPB unbind), or `#Cxl.ForceUnbind` forces it.
5. **Deprovisioning** - `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` returns capacity to the available pool, once no switch-attached Connection exposes the chunk (409 until then).

### Direct-Attached vs Switch-Attached

| Topology | vPPB Binding | Connections |
|----------|--------------|-------------|
| Direct-attached | None: there is no switch | Derived from the link, always `Enabled`; `POST` of the pair is `200`, `DELETE` is `405` |
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
| May Set LD Allocations, or Set Partition Info, change an LD or SLD that is bound? | Not said | 409 for a chunk `DELETE`, and for a chunk `POST` that would send either command, while a switch-attached Connection that is not `Absent` exposes it. A `"PMEM"` chunk of an SLD sends nothing and is not refused. Direct-attached Connections, and Dynamic Capacity extents, are not gated |
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
3. A Connection is as old as the last read of its switch's VCSs: a `GET`
   sends no command. A binding made by another FM or through another CCI,
   of which the switch records no Binding Change this FM reads, is seen
   only before the next `POST` or `DELETE` on that switch, or on the
   `--rescan` timer (see "Binding").
4. On hardware, a Multi-Headed device has no Connections, and a `POST` to
   it is 400: no command maps a head's Port Number to a switch port or a
   host. A head that presents an MLD cannot be bound per LD, and a binding
   found on one is `Enabled` with `Health` `Warning` and no chunks (see
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
8. A bind or unbind is followed for as long as the switch takes: there is
   no time limit and no automatic abort. `#Cxl.ForceUnbind` is the
   recourse for an unbind (see "Binding").
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
  say where a head is (`parent_switch` and `parent_port`, or `attached_host`).
- `Oem.Cxl.Origin` is lost on restart: after one, every binding is
  `"Discovered"`.
- What a host has mapped of what is bound is not known: the FM has no
  CXL.io access to a bound port or LD (Section 7.2.3.1). A `MemoryDomain`
  of a host is what a binding makes reachable.

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
  `AccessCapabilities` `["Read", "Write"]` and `AccessState` `Optimized`.
  `Status` can be `Enabled` with `Health` `Warning` (target not known).
  `TargetEndpoints` is the LD Endpoint for an LD. New:
  `Oem.Cxl.Origin`, and `Actions.Oem["#Cxl.ForceUnbind"]`.
- **Tasks.** A bind's `Task` lists the Connection in
  `Links.CreatedResources` while it runs (only once `Completed`, before).
  A task that is lost no longer leaves its Connection `Starting` or
  `Updating`.

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
| `DELETE` of a Connection whose task was lost | `503`, for good | taken |
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
| Chunk `POST` after a `DELETE` of a chunk of the domain | the ID of a chunk the domain still had, so that two chunks had one URI, a collection listed it twice, and a Connection's `MemoryChunkInfo` too | an ID that no chunk of the domain has had since cxl-fmd started, or discovered the device |
| Chunk `POST` on a domain with chunks found at discovery (the `{device}_ld{N}_chunk` of an MLD or MHD) | `{domain}_chunk{N}`, N the number of chunks in the domain | `{domain}_chunk0` for the first, then 1, 2, ... |
| Connection `POST` with a link that is not the URI the service serves the resource at, but ends in the ID of one (`/bogus/sld_0`, `/redfish/v1/Chassis/sld_1` as a target, a chunk of one device under another's Chassis, a relative URI) | `201`: the last segment was taken as the ID, whatever came before it | `400`, nothing bound |
| Connection `POST` naming a chunk by its URI in a host's view, `/redfish/v1/Systems/{host}/MemoryDomains/{dom}/MemoryChunks/{chunk}` | taken, by its last segment | `400`: a chunk is named by its Chassis URI |
| Connection `POST` with `InitiatorEndpointGroups` or `TargetEndpointGroups` | ignored: `201` from the Endpoints alone | `400`, nothing bound |
| `DynamicCapacityAdd` or `DynamicCapacityRelease` with a `Length` past 2^44 - 1 MiB, or `PATCH` of a `MemoryRegion`'s `BlockSizeMiB` past it | `200` with the value wrapped (an add of 2^44 + 256 MiB added 256 MiB), or `400` with another message when it wrapped to 0 | `400`, nothing sent |

Mock topologies and discovery:

- Two components with one ID, and two LDs of an MLD with one LD-ID, are
  refused while the file is loaded ("two components with ID ...", "two
  LDs with LD-ID ..."), as they were by the validation after it. On
  hardware, a component discovered with an ID another one has is not
  added, and an error is logged.

---

## References

- [DMTF DSP0288 - CXL to Redfish Mapping Specification](https://www.dmtf.org/dsp/DSP0288)
- [DMTF DSP0266 - Redfish Specification](https://www.dmtf.org/dsp/DSP0266)
- [DMTF DSP8010 - Redfish Schema Bundle](https://www.dmtf.org/dsp/DSP8010)
- [CXL Specification](https://www.computeexpresslink.org/download-the-specification/)
