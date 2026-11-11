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

cxl-fmd does not currently implement authentication. All endpoints are accessible without credentials. The `SessionService` endpoint exists for schema compliance but sessions are not enforced.

For production deployments, place cxl-fmd behind a reverse proxy (e.g., nginx) that handles authentication.

---

## Table 1 - CXL CCI to Redfish Mapping

Mapping between CXL Component Command Interface (CCI) commands and Redfish resources.

| CXL Command | CXL Field | Redfish Resource | Redfish Property | Implemented |
|-------------|-----------|------------------|------------------|:-----------:|
| Get Log Capabilities | Parameter Flags[3]: Persistent across cold reset | `LogService` | `Persistency` | No |
| Get Event Records | Flags[0]: Overflow | `LogService` | `Overflow` | No |
| Get Event Records | Overflow Error Count | `LogEntry` | `OverflowErrorCount` | No |
| Get Event Records | First Overflow Event Timestamp | `LogEntry` | `FirstOverflowTimestamp` | No |
| Get Event Records | Last Overflow Event Timestamp | `LogEntry` | `LastOverflowTimestamp` | No |
| Get FW Info | FW Slot Info | `PCIeDevice` | `StagedVersion` | No |
| Get Timestamp | Timestamp | `PCIeDevice` | `CXLDevice/Timestamp` | Yes |
| Set Timestamp | Timestamp | `PCIeDevice` | `CXLDevice/Timestamp` | Yes |
| Identify Memory Device | Informational, Warning, Failure, and Fatal Event Log Size | `LogService` | `MaxNumberOfRecords` | No |
| Identify Memory Device | Poison List Maximum Media Error Records | `Memory` | `PoisonListMaxMediaErrorRecords` | No |
| Identify Memory Device | Poison Handling Capabilities | `Memory` | `PoisonHandlingCapabilities` | No |
| Identify Memory Device | QoS Telemetry Capabilities[0]: Egress Port Congestion Supported | `PCIeDevice` | `CXLDevice/EgressPortCongestionSupported` | Yes |
| Identify Memory Device | QoS Telemetry Capabilities[1]: Temporary Throughput Reduction Supported | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionSupported` | Yes |
| Identify Memory Device | LSA Size | `Memory` | `LabelStorageSizeBytes` | No |
| Get SLD QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | No |
| Set SLD QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | No |
| Get Partition Info | Active Volatile Capacity | `Memory` | `VolatileSizeLimitMiB` | Yes |
| Get Partition Info | Next Volatile Capacity | `Memory` | `StagedVolatileSizeMiB` | Yes |
| Get Partition Info | Next Persistent Capacity | `Memory` | `StagedNonVolatileSizeMiB` | Yes |
| Set Partition Info | Next Volatile Capacity | `Memory` | `StagedVolatileSizeMiB` | Yes |
| Set Partition Info | Next Persistent Capacity | `Memory` | `StagedNonVolatileSizeMiB` | Yes |
| Get Health Info | Health Status | Many | `Status` | Yes |
| Get Health Info | Life Used | `MemoryMetrics` | `CapacityUtilizationPercent` | Yes |
| Get Health Info | Corrected Volatile Error Count | `MemoryMetrics` | `CorrectedVolatileErrorCount` | Yes |
| Get Health Info | Corrected Persistent Error Count | `MemoryMetrics` | `CorrectedPersistentErrorCount` | Yes |
| Get Health Info | Additional Status[4]: Corrected Volatile Error Count | `Memory` | `Status/Conditions` | No |
| Get Health Info | Additional Status[5]: Corrected Persistent Error Count | `Memory` | `Status/Conditions` | No |
| Get Health Info | Dirty Shutdown Count | `MemoryMetrics` | `DirtyShutdownCount` | Yes |
| Get Alert Configuration | Valid Alerts and Programmable Alerts | `MemoryMetrics` | `CXL/AlertCapabilities` | No |
| Scan Media | N/A | `Memory` | `ScanMedia` action | Yes |
| Get Security State | Security State | `Memory` | `SecurityStates` | Yes |
| Set Passphrase | All | `Memory` | `SetPassphrase`, `SetMasterPassphrase` actions | Yes |
| Disable Passphrase | All | `Memory` | `DisablePassphrase`, `DisableMasterPassphrase` actions | Yes |
| Freeze Security State | N/A | `Memory` | `FreezeSecurityState` action | Yes |
| Passphrase Secure Erase | All | `Memory` | `SecureEraseUnit` action | Yes |
| Unlock | All | `CXLLogicalDevice` | `Unlock` action | No |

### Comments for Table 1

**Comment 1:** A `LogEntry` resource is used to capture CXL event records. The `CXLEntryType` property shall contain the specific CXL entry type, and the `EntryType` property shall contain `CXL`. Overflow information is captured in properties of the `LogEntry` prior to the overflow condition.

**Comment 2:** A `Memory` resource can only contain a single `LogService`. This `LogService` resource shall contain the informational, warning, failure, and fatal event records as `LogEntry` resources. The `MaxNumberOfRecords` shall contain the sum of the informational, warning, failure, and fatal log sizes.

**Comment 3:** Redfish resources contain a `Status` property to show the health and state of a device. The `Conditions` property within the `Status` property is used to show why the `Health` property contains a value other than `OK`.

**Comment 4:** The `Conditions` property within the `Status` property of the `Memory` resource shall contain messages to indicate either the Corrected Volatile Error Count or Corrected Persistent Error Count flags are set.

---

## Table 2 - CXL FM-API to Redfish Mapping

Mapping between CXL Fabric Management API (FM-API) commands and Redfish resources.

| CXL Command | CXL Field | Redfish Resource | Redfish Property | Implemented |
|-------------|-----------|------------------|------------------|:-----------:|
| Identify Switch Device | All | `Switch` | `SwitchType` | Yes |
| Identify Switch Device | Number of VCSs | `Switch` | `CXL/MaxVCSSupported` | Yes |
| Identify Switch Device | Total Number of vPPBs | `Switch` | `CXL/TotalNumbervPPBs` | Yes |
| Identify Switch Device | Total Number of Bound vPPBs | `Switch` | `CXL/NumberOfBoundvPPBs` | Yes |
| Identify Switch Device | Number of HDM Decoders available per USP | `Switch` | `CXL/HDMDecoders` | Yes |
| Get Physical Port State | Port Information List, PBR Capable | `Switch` | `CXL/PBRCapable` | Yes |
| Get Physical Port State | Port Information List, Current Port Configuration State | `Port` | `CXL/CurrentPortConfigurationState` | Yes |
| Get Physical Port State | Port Information List, Connected Device Mode | `Port` | `CXL/ConnectedDeviceMode` | Yes |
| Get Physical Port State | Port Information List, Connected Device Type | `Port` | `CXL/ConnectedDeviceType` | Yes |
| Get Physical Port State | Port Information List, Supported CXL Modes | `Port` | `CXL/SupportedCXLModes` | Yes |
| Get Physical Port State | Port Information List, Supported LD Count | `Port` | `CXL/MaxLogicalDeviceCount` | Yes |
| Get Physical Port State | Port Information List, Negotiated Link Width | `Port` | `ActiveWidth` | Yes |
| Get Physical Port State | Port Information List, Supported Link Speeds Vector | `Port` | `LinkConfiguration/CapableLinkSpeedGbps` | Yes |
| Get Physical Port State | Port Information List, Max Link Speed | `Port` | `MaxSpeedGbps` | Yes |
| Get Physical Port State | Port Information List, Current Link Speed | `Port` | `CurrentSpeedGbps` | Yes |
| Physical Port Control | Port Opcode 02h: Reset PPB | `Port` | `ResetPPB` action | Yes |
| Physical Port Control | Port Opcode 00h and 01h: Assert and Deassert PERST | `Port` | `Reset` action | No |
| Get Virtual CXL Switch Info | Virtual CXL Switch ID | `VirtualCXLSwitch` | `VCSId` | Yes |
| Get Virtual CXL Switch Info | VCS State | `VirtualCXLSwitch` | `Status` | Yes |
| Get Virtual CXL Switch Info | vPPB Array Index | `VirtualPCI2PCIBridge` | `VPPBId` | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Binding Status | `VirtualPCI2PCIBridge` | `BindingStatus` | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound Port Id | `VirtualPCI2PCIBridge` | `BoundPortId` | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound LD Id | `VirtualPCI2PCIBridge` | `BoundLDId` | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] Bound PID | `VirtualPCI2PCIBridge` | `BoundPBRId` | Yes |
| Get Virtual CXL Switch Info | vPPB[Array Index] PCIe Device Serial Number + Bound LD Id | `VirtualPCI2PCIBridge` | `GCXLID` | Yes |
| Get LD Info | Memory Size | `MemoryDomain` | `MemorySizeMiB` | Yes |
| Get LD Info | LD Count | `PCIeDevice` | `CXLDevice/MaxNumberLogicalDevices` | Yes |
| Get LD Info | QoS Telemetry Capabilities[0]: Egress Port Congestion Supported | `Port` | `CXL/QoSTelemetryCapabilities/EgressPortBackpressureSupported` | Yes |
| Get LD Info | QoS Telemetry Capabilities[1]: Temporary Throughput Reduction Supported | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionSupported` | Yes |
| Get LD Allocations | Memory Granularity | `MemoryDomain` | `MemoryChunkIncrementMiB` | Yes |
| Get LD Allocations | Memory Granularity, LD Allocation List | `CXLLogicalDevice` | `MemorySizeMiB` | Yes |
| Set LD Allocations | LD Allocation List | `MemoryChunks` | POST/DELETE | Yes |
| Get QoS Control | QoS Telemetry Control[0]: Egress Port Congestion Enable | `Port` | `CXL/Congestion/CongestionTelemetryEnabled` | Yes |
| Get QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | Yes |
| Get QoS Control | Egress Moderate Percentage | `Port` | `CXL/Congestion/EgressModeratePercentage` | Yes |
| Get QoS Control | Egress Severe Percentage | `Port` | `CXL/Congestion/EgressSeverePercentage` | Yes |
| Get QoS Control | Backpressure Sample Interval | `Port` | `CXL/Congestion/BackpressureSampleInterval` | Yes |
| Get QoS Control | ReqCmpBasis | `Port` | `CXL/Congestion/MaxSustainedRequestCmpBias` | Yes |
| Get QoS Control | Completion Collection Interval | `Port` | `CXL/Congestion/CompletionCollectionInterval` | Yes |
| Get QoS Status | Backpressure Average Percentage | `PortMetrics` | `CXL/BackpressureAveragePercentage` | Yes |
| Get QoS Allocated BW | QoS Allocation Fraction | `CXLLogicalDevice` | `QoS/AllocatedBandwidth` | Yes |
| Get QoS BW Limit | QoS Limit Fraction | `CXLLogicalDevice` | `QoS/LimitPercent` | Yes |
| Set QoS Control | QoS Telemetry Control[1]: Temporary Throughput Reduction Enable | `PCIeDevice` | `CXLDevice/TemporaryThroughputReductionEnabled` | No |
| Get PBR Link Partner Info | Port ID - ID of requested Port | `Port` | `LinkPartnerTransmit/PortId` | No |
| Get PBR Link Partner Info | Far End Port ID - Port ID of Far End Device | `Port` | `LinkPartnerReceived/PortId` | No |
| Get PBR Link Partner Info | Device UUID | `Port` | `LinkPartnerReceived/LinkPartnerId` | No |
| Bind vPPB | All | `Connection` | POST /Connections | Yes |
| Unbind vPPB | All | `Connection` | DELETE /Connections | Yes |
| MLD Port Event Record | All | `LogEntry` | | No |

### Comments for Table 2

**Comment 1:** For an MLD, implementations shall represent Egress Port Congestion Supported in the `Port` resource. For an SLD, implementations shall represent Egress Port Congestion Supported in the `PCIeDevice` resource.

**Comment 2:** A `LogEntry` resource is used to capture CXL event records. The `CXLEntryType` property shall contain the specific CXL entry type, and the `EntryType` property shall contain `CXL`. Overflow information is captured in properties of the `LogEntry` prior to the overflow condition.

**Comment 3:** Bind vPPB and Unbind vPPB operations are exposed through the Connections API rather than direct OEM actions. POST to `/redfish/v1/Fabrics/CXL/Connections` automatically performs vPPB binding for switch-attached hosts, and DELETE automatically performs unbinding.

---

## HTTP Method Mapping

Implementations shall map CXL commands to HTTP operations as follows:

| CXL Command Type | HTTP Method | Implemented |
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
- `POST /redfish/v1/Fabrics/CXL/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB` - Reset PPB
- `PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}` - Set DC Region Configuration
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` - Initiate Dynamic Capacity Add
- `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` - Initiate Dynamic Capacity Release
- `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` - Initiate Dynamic Capacity Release (Prescriptive, one extent)
- `POST /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory` - DC Add Reference / DC Remove Reference

---

## Error Responses

All error responses follow the Redfish error format with an `error` object containing a `code` and `message`.

### HTTP Status Codes

| Status | Meaning | When Used |
|--------|---------|-----------|
| 200 OK | Success | GET requests returning data |
| 201 Created | Resource created | POST to collections (MemoryChunks, Connections) |
| 204 No Content | Success, no body | DELETE requests |
| 400 Bad Request | Invalid request | Malformed JSON, missing required fields, invalid values |
| 404 Not Found | Resource not found | Invalid resource ID or path |
| 405 Method Not Allowed | Method not supported | Using unsupported HTTP method on endpoint |
| 409 Conflict | Resource conflict | Capacity exceeded, invalid state transition |
| 413 Payload Too Large | Request body too large | Body exceeds 64KB limit |
| 501 Not Implemented | Feature not implemented | PATCH on read-only resources |

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
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}` | PCIe Device |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices` | CXL Logical Devices |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}` | CXL Logical Device |
| `GET .../CXLLogicalDevices/{ld}/MemoryRegions` | Dynamic capacity regions visible to the LD's host |
| `GET .../CXLLogicalDevices/{ld}/MemoryRegions/{region}` | DC Region |
| `PATCH .../CXLLogicalDevices/{ld}/MemoryRegions/{region}` | Set DC Region Configuration (`BlockSizeMiB`, `SanitizeOnRelease`) |
| `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd` | Initiate Dynamic Capacity Add |
| `POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease` | Initiate Dynamic Capacity Release |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents` | Dynamic capacity extents on the device |
| `GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` | DC Extent |
| `DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}` | Release the extent from every host |
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
| `GET /redfish/v1/Fabrics/CXL/Endpoints` | Endpoint Collection (devices + hosts) |
| `GET /redfish/v1/Fabrics/CXL/Endpoints/{ep}` | Fabric Endpoint (device or host) |
| `GET /redfish/v1/Fabrics/CXL/Connections` | Connection Collection |
| `GET /redfish/v1/Fabrics/CXL/Connections/{conn}` | Connection (host-to-device binding) |
| `POST /redfish/v1/Fabrics/CXL/Connections` | Create Connection (binds memory to host) |
| `DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}` | Delete Connection (unbinds memory) |
| `GET /redfish/v1/Fabrics/CXL/Zones` | Zone Collection |
| `GET /redfish/v1/Fabrics/CXL/Zones/{zone}` | Zone (VCS access domain) |

### Endpoints

Fabric Endpoints represent entities that participate in CXL communication. There are two types:

**Target Endpoints (Memory Devices)**
- Represent SLD and MLD devices that provide CXL memory
- `EntityType`: `MemoryChunk`
- `EntityRole`: `Target` (responds to memory requests)
- `EntityPciId`: Contains VendorId/DeviceId from device
- `Links.ConnectedPorts`: Switch port the device connects to (if switch-attached)
- `Links.Zones`: VCS(s) the device is bound to

**Initiator Endpoints (Hosts)**
- Represent host systems that issue CXL.mem requests
- `EntityType`: `Processor`
- `EntityRole`: `Initiator` (issues memory requests)
- `Links.Zones`: VCS the host connects through (if switch-attached)

Example host endpoint:
```json
{
    "@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0",
    "EndpointProtocol": "CXL",
    "ConnectedEntities": [{
        "EntityType": "Processor",
        "EntityRole": "Initiator"
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
  - Device endpoints (EntityRole=Target) bound to the VCS via vPPBs
- Bidirectional links: Both hosts and devices include `Links.Zones` back-references

### Actions

| Endpoint | Method | Description |
|----------|--------|-------------|
| `.../Ports/{port}/Actions/Port.ResetPPB` | POST | Reset Physical Port PPB |
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
- **MLD**: Issues `Set LD Allocations (0x5402h)` to increase the LD's memory allocation
- **SLD**: Issues `Set Partition Info (0x4101h)` to adjust the volatile/persistent boundary

### Deleting Memory Chunks (DELETE)

```http
DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
```

**Response:** HTTP 204 No Content on success.

**Hardware Behavior:**
- **MLD**: Issues `Set LD Allocations (0x5402h)` to decrease the LD's memory allocation
- **SLD**: Issues `Set Partition Info (0x4101h)` to reduce volatile capacity

### MLD Memory Model

For Multi-Logical Devices (MLDs), the memory hierarchy is:
- **MLD Device** → Chassis with `ChassisType: "RackMount"`
- **Memory Domain** → One domain per MLD, represents the device's total capacity
- **Memory Chunk** → One chunk per Logical Device (LD), 1:1 mapping
- **Memory** → Individual LDs as memory resources

For Phase 1 (direct-attached devices):
- Each LD maps to exactly one chunk
- InterleaveSets has a single entry identifying the target LD
- POST resizes an existing LD's allocation
- DELETE reduces the LD's allocation

Chunk IDs follow the format: `{domain_id}_chunk{N}` (e.g., `cxl_mld_0_domain0_chunk0`)

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
- **SLD Device** → Chassis with `ChassisType: "RackMount"`
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
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/{device_id}"}
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
| `Links.TargetEndpoints` | array | Yes | Device endpoint(s) with `EntityRole: Target` |
| `MemoryChunkInfo` | array | Yes | Memory chunk(s) to bind |
| `MemoryChunkInfo[].MemoryChunk` | object | Yes | Reference to the MemoryChunk |
| `MemoryChunkInfo[].AccessCapabilities` | array | No | `["Read"]`, `["Write"]`, or `["Read", "Write"]` (default) |

**Response:** Returns the created Connection resource with HTTP 201 Created.

**Hardware Behavior (Switch-Attached Hosts):**
- Automatically finds an available vPPB in the host's VCS
- Issues `Bind vPPB (0x5201h)` to bind the device port (and LD for MLD/MHD) to the vPPB
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

**Response:** HTTP 204 No Content on success.

**Hardware Behavior (Switch-Attached Hosts):**
- Issues `Unbind vPPB (0x5202h)` with Managed Hot-Remove option
- Releases the vPPB for reuse

### Connection Lifecycle

The complete memory lifecycle is:

1. **Discovery** - Devices and hosts are discovered, topology built. Existing LD allocations are represented as MemoryChunks; remaining capacity is tracked for future provisioning.
2. **Provisioning** - `POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks` allocates memory from the remaining capacity, creating a new chunk.
3. **Binding** - `POST /redfish/v1/Fabrics/CXL/Connections` binds a chunk to a host (vPPB bind for switch-attached).
4. **Unbinding** - `DELETE /redfish/v1/Fabrics/CXL/Connections/{id}` releases the binding (vPPB unbind).
5. **Deprovisioning** - `DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}` returns capacity to the available pool.

### Direct-Attached vs Switch-Attached

| Topology | vPPB Binding | Connection Effect |
|----------|--------------|-------------------|
| Direct-attached | Not applicable | Creates logical association only |
| Switch-attached | Automatic | Binds vPPB to port/LD via FM-API |

For direct-attached hosts (`host.direct_attached = true`), connections track the host-to-device association but no vPPB binding occurs.

---

## Known Gaps

Properties and links defined in DMTF schemas but not yet implemented.

### MemoryChunks

| Property | Schema | Notes |
|----------|--------|-------|
| `MediaLocation` | MemoryChunks.v1_5_0+ | Location of memory media (e.g., "Local", "Remote"). |
| `RequestedOperationalState` | MemoryChunks.v1_5_0+ | Requested operational state for the chunk. |

### CXLLogicalDevice

| Property | Schema | Notes |
|----------|--------|-------|
| `Links.PCIeFunctions` | CXLLogicalDevice.v1_0_0+ | Link to associated PCIe functions. Requires PCIeFunction resource implementation. |

---

## References

- [DMTF DSP0288 - CXL to Redfish Mapping Specification](https://www.dmtf.org/dsp/DSP0288)
- [DMTF DSP0266 - Redfish Specification](https://www.dmtf.org/dsp/DSP0266)
- [DMTF DSP8010 - Redfish Schema Bundle](https://www.dmtf.org/dsp/DSP8010)
- [CXL Specification](https://www.computeexpresslink.org/download-the-specification/)
