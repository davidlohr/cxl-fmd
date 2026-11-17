# CXL Fabric Manager Daemon (cxl-fmd)

cxl-fmd provides a DMTF Redfish API for managing CXL memory fabrics. It exposes CXL switches, Type 3 memory devices (SLDs, MLDs, and Multi-Headed Devices), Virtual CXL Switch (VCS) configurations, and host-to-device connections as standard Redfish resources. This allows orchestration systems and management tools to discover, monitor, and configure CXL infrastructure using the same interfaces they use for other datacenter resources.

The daemon discovers CXL topology by issuing CCI commands over MCTP or kernel ioctl interfaces. A mock mode enables development and testing against simulated topologies without requiring CXL hardware.

## Building

### Prerequisites

- Meson >= 0.60.0
- Ninja
- GCC or Clang with C17 support
- libmicrohttpd
- jansson (JSON library)
- libcxlmi

### Build Instructions

```bash
# Configure
meson setup build

# Build
meson compile -C build

# Install (optional)
sudo meson install -C build
```

Installing also installs the manual page, `cxl-fmd(8)`. To read it from the
source tree: `man -l man/cxl-fmd.8`.

### Build Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tests` | feature | auto | Enable test suite |

```bash
# Enable tests explicitly
meson setup build -Dtests=enabled
```

## Usage

### Basic Usage

```bash
# Start with default settings (MCTP discovery, port 8080)
./build/cxl-fmd

# Specify port
./build/cxl-fmd --port 8081

# Enable verbose logging
./build/cxl-fmd -v      # INFO level
./build/cxl-fmd -vv     # DEBUG level

# Use ioctl interface instead of MCTP
./build/cxl-fmd --ioctl
```

### Mock Mode

For development and testing without CXL hardware:

```bash
./build/cxl-fmd --mock mock/single-sld.json
```

See [docs/mock-topology.md](docs/mock-topology.md) for available topologies and the mock JSON schema.

### Command Line Options

| Option | Short | Description |
|--------|-------|-------------|
| `--port PORT` | `-p` | HTTP port (default: 8080) |
| `--mock FILE` | `-M` | Use mock topology from JSON file |
| `--check` | `-c` | Check that the topology is a valid CXL fabric, report, and exit |
| `--mctp` | `-m` | Enable MCTP device scanning |
| `--ioctl` | `-i` | Enable ioctl device scanning |
| `--event-poll SECS` | `-e` | Read the event logs every SECS seconds (default: 10, 0 disables) |
| `--rescan SECS` | `-r` | Look for new devices every SECS seconds (default: 30, 0 disables) |
| `--threads N` | `-N` | Serve requests on N threads, 1 to 256 (default: 8). Requests that only read run together; a change runs alone |
| `--allow-mctp-restricted` | | Send Set Partition Info, Scan Media and the security commands as MCTP messages, tunnelled ones too. Without it they are refused (409): CXL r4.0 Table 8-308 note 4. Only when no host drives the devices (see [docs/Redfish-API.md](docs/Redfish-API.md#commands-restricted-on-mctp)) |
| `--verbose` | `-v` | Increase verbosity (repeatable) |
| `--help` | `-h` | Show help message |
| `--version` | `-V` | Show version |

## Redfish API

cxl-fmd implements (currently a subset of) the DMTF Redfish API focused on CXL memory device management.
See [docs/Redfish-API.md](docs/Redfish-API.md) for the complete API reference.

### Service Root

```
GET /redfish/v1/
```

Returns the Redfish service root with links to available resource collections.

### Supported Resources

| Resource | URI Pattern | Description |
|----------|-------------|-------------|
| Service Root | `/redfish/v1` | API entry point |
| OData Service Document | `/redfish/v1/odata` | OData resource listing |
| Metadata | `/redfish/v1/$metadata` | EDMX schema references |
| **Chassis** | | |
| Chassis Collection | `/redfish/v1/Chassis` | CXL devices as chassis |
| Chassis | `/redfish/v1/Chassis/{id}` | Individual device |
| PCIeDevices | `/redfish/v1/Chassis/{id}/PCIeDevices` | PCIe device collection |
| PCIeDevice | `/redfish/v1/Chassis/{id}/PCIeDevices/{dev}` | PCIe device details |
| CXLLogicalDevices | `.../PCIeDevices/{dev}/CXLLogicalDevices` | Logical device collection |
| CXLLogicalDevice | `.../CXLLogicalDevices/{ld}` | Logical device with memory info and DCD actions |
| MemoryRegions | `.../CXLLogicalDevices/{ld}/MemoryRegions` | Dynamic capacity regions seen by the LD's host (PATCH) |
| MemoryRegion | `.../MemoryRegions/{region}` | DC region configuration and extent links |
| MemoryExtents | `.../PCIeDevices/{dev}/MemoryExtents` | Dynamic capacity extents of every host of the device (DELETE releases from that extent's host; for an extent of a sharable tag, all of that host's extents with the tag) |
| MemoryExtent | `.../MemoryExtents/{ext}` | Extent offset, size, tag, FM reference |
| MemoryDomains | `/redfish/v1/Chassis/{id}/MemoryDomains` | Memory domain collection |
| MemoryDomain | `.../MemoryDomains/{dom}` | Memory domain details |
| MemoryChunks | `.../MemoryDomains/{dom}/MemoryChunks` | Memory chunk collection (POST/DELETE) |
| MemoryChunk | `.../MemoryChunks/{chunk}` | Individual memory chunk |
| **Fabrics** | | |
| Fabric Collection | `/redfish/v1/Fabrics` | CXL fabric collection |
| Fabric | `/redfish/v1/Fabrics/CXL` | CXL fabric resource |
| Switch Collection | `/redfish/v1/Fabrics/CXL/Switches` | CXL switches |
| Switch | `.../Switches/{sw}` | Individual switch with CXL properties |
| Port Collection | `.../Switches/{sw}/Ports` | Switch ports (USP/DSP) |
| Port | `.../Ports/{port}` | Port details with link status |
| PortMetrics | `.../Ports/{port}/Metrics` | Port metrics with QoS status |
| VCS Collection | `.../Switches/{sw}/VCSs` | Virtual CXL Switches |
| VCS | `.../VCSs/{vcs}` | VCS with state and vPPB link |
| vPPB Collection | `.../VCSs/{vcs}/VPPBs` | Virtual PPBs in VCS |
| vPPB | `.../VPPBs/{vppb}` | vPPB binding status and target |
| Endpoint Collection | `/redfish/v1/Fabrics/CXL/Endpoints` | CXL endpoints (hosts and devices) |
| Endpoint | `.../Endpoints/{ep}` | Endpoint with connected entity |
| Connection Collection | `/redfish/v1/Fabrics/CXL/Connections` | One per vPPB binding of a host's VCS to a memory device, whoever made it, and per direct-attached link; POST binds a vPPB |
| Connection | `.../Connections/{conn}` | Connection with initiator/target endpoints; DELETE unbinds, `Actions/Oem/Cxl.ForceUnbind` forces the unbind |
| Zone Collection | `/redfish/v1/Fabrics/CXL/Zones` | CXL zones (per VCS) |
| Zone | `.../Zones/{zone}` | Zone with endpoint membership |
| **Systems** | | |
| Systems Collection | `/redfish/v1/Systems` | Host systems |
| System | `/redfish/v1/Systems/{host}` | Host system with MemoryDomains link |
| System MemoryDomains | `.../Systems/{host}/MemoryDomains` | Host-visible memory domains |
| **Other** | | |
| Managers | `/redfish/v1/Managers` | Empty collection (TODO) |
| Sessions | `/redfish/v1/SessionService/Sessions` | Empty session collection |

### Example Requests

```bash
# Get service root
curl http://localhost:8080/redfish/v1/

# List all chassis (CXL devices)
curl http://localhost:8080/redfish/v1/Chassis

# Get specific device
curl http://localhost:8080/redfish/v1/Chassis/cxl_mem0

# Get CXL logical device with memory size
curl http://localhost:8080/redfish/v1/Chassis/cxl_mem0/PCIeDevices/cxl_mem0/CXLLogicalDevices/0

# Get memory domain
curl http://localhost:8080/redfish/v1/Chassis/cxl_mem0/MemoryDomains/cxl_mem0_domain0

# List CXL switches (requires switch topology)
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches

# Get switch with VCS info
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0

# List switch ports
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports

# Get Virtual CXL Switches
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs

# Get vPPB bindings for a VCS
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs
```

### Response Headers

All JSON responses include:
- `Content-Type: application/json; charset=utf-8`
- `OData-Version: 4.0`

XML metadata responses include:
- `Content-Type: application/xml; charset=utf-8`
- `OData-Version: 4.0`

### CXL to Redfish Mapping

The Health Status and Media Status fields are those of Get Health Info (CXL
r4.0 Table 8-315). Which Redfish value each is given is cxl-fmd's mapping,
the same in every view of a device; a Media Status other than 00h and 01h
raises Health too (03h `Critical`, any other `Warning`), and a device whose
Get Health Info failed has no Health. It is read again on the `--rescan`
timer and after a record in the device's Warning, Failure or Fatal Event
Log. See "Status from Get Health Info" in docs/Redfish-API.md.

#### Health Status

| CXL CCI health_status | Redfish Health |
|-----------------------|----------------|
| 0 (no flags) | OK |
| Bit 0 (Maintenance Needed), 1 (Performance Degraded) or 3 (Memory Capacity Degraded) set | Warning |
| Bit 2 set (Hardware Replacement Needed) | Critical |

#### Media Status

| CXL CCI media_status | Redfish State |
|----------------------|---------------|
| 0 (Normal) | Enabled |
| 1 (Not Ready) | Starting |
| 2 (Write Persistency Lost) | Degraded |
| 3 (All Data Lost) | UnavailableOffline |
| 4 (Write Persistency Loss in the Event of Power Loss) | Degraded |
| 5 to 9 (write persistency or all data may be lost) | Degraded |
| Reserved (0Ah and up) | Degraded (cxl-fmd's choice) |

## Mock Topology

The mock system allows testing without CXL hardware by defining device configurations in JSON files. Mock devices respond with the same data fields as real CXL CCI commands.

See [docs/mock-topology.md](docs/mock-topology.md) for the complete mock topology language reference.

### Quick Example

```json
{
    "version": 1,
    "id": "single_sld",
    "description": "Single 16GB CXL SLD",
    "devices": [
        {
            "id": "cxl_mem0",
            "vendor_id": "0x8086",
            "device_id": "0x0d93",
            "identify_memory_device": {
                "fw_revision": "1.0.0",
                "total_capacity_mb": 16384,
                "persistent_capacity_mb": 16384
            },
            "health_info": {
                "health_status": 0,
                "media_status": 0
            }
        }
    ]
}
```

### MLD Configuration

Multi-Logical Devices support per-LD memory allocation and health status:

```json
{
    "id": "cxl_mld_0",
    "ld_count": 4,
    "identify_memory_device": {
        "total_capacity_mb": 131072,
        "persistent_capacity_mb": 131072
    },
    "logical_devices": [
        { "ld_id": 0, "memory_size_mb": 32768, "qos_allocated_bw": 100 },
        { "ld_id": 1, "memory_size_mb": 32768, "qos_allocated_bw": 75 },
        { "ld_id": 2, "memory_size_mb": 32768, "qos_allocated_bw": 50 },
        { "ld_id": 3, "memory_size_mb": 32768, "qos_allocated_bw": 25 }
    ]
}
```

## Testing

See [docs/testing.md](docs/testing.md) for comprehensive testing documentation including QEMU-based hardware emulation tests.

### Quick Start

```bash
# Run all tests (builds if needed)
meson test -C build

# Run with verbose output
meson test -C build -v

# Run specific test suite
meson test -C build --suite redfish    # Quick endpoint tests
meson test -C build --suite api        # Full API tests
meson test -C build --suite security   # Fuzzing and adversarial tests
meson test -C build --suite unit       # discovery-tests, the mock topologies, the man page, the docs, and 7 attachment/device combinations
meson test -C build --suite validator  # DMTF schema validation
```

### Standalone Validator

The [DMTF Redfish Service Validator](https://github.com/DMTF/Redfish-Service-Validator) is an official conformance tool that checks Redfish implementations against DMTF schemas. It crawls all resources exposed by the service and validates that JSON payloads conform to schema definitions, required properties are present, and navigation links resolve correctly.

```bash
# Run validator manually with verbose daemon output
python scripts/run_redfish_validator.py --verbose

# Test specific topology
python scripts/run_redfish_validator.py --mock-topology mock/mld-device.json

# Save validator logs
python scripts/run_redfish_validator.py --output-dir ./logs
```

## Dynamic Capacity

Type 3 devices that implement the FM-API DCD Management command set (CXL r4.0 Section 7.6.7.6) are exposed through the Redfish model DSP8010 2026.1 introduced. DSP0288 does not cover these commands yet; the mapping cxl-fmd implements is specified in [docs/DCD-FM-API.md](docs/DCD-FM-API.md).

```bash
./build/cxl-fmd --mock mock/dcd-device.json

# Device capabilities (Get DCD Info)
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0 | jq .CXLDevice.DynamicCapacity

# Regions a host sees, and the extents on the device
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/MemoryRegions/0
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents

# Add 1 GiB to region 0 (Initiate Dynamic Capacity Add): 202 and a Task,
# which is Completed once the host is seen to have the capacity; GET the
# Location the answer gives (its task monitor) to learn what the host took
curl -s -i -X POST http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": "6f1a2c3e-0000-4000-8000-0000000000aa"}'

# Release it again (Initiate Dynamic Capacity Release): a Task too
curl -s -i -X POST http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "TagBased", "Tag": "6f1a2c3e-0000-4000-8000-0000000000aa", "Length": 1024, "ForcedRemoval": false, "SanitizeOnRelease": true}'
```

## Limitations

The following CXL features are not yet supported:
- **Port-Based Routing (PBR)**: PBR fabric discovery and Global Fabric-Attached Memory Devices
- **Removal of devices**: a switch records a change of a port's link or slot status in a Physical Switch Event Record (CXL r4.0 Table 7-79). cxl-fmd reads a switch's event logs every `--event-poll` seconds (default 10) and when the switch sends an MCTP Event Notification; the ports that Physical Switch Event Records name are read again, and the records cleared. The whole fabric is looked at again every `--rescan` seconds (default 30), which is also what finds new endpoints. New ports, devices that have appeared on a port, new VCSs and new endpoints are added. A device on a switch port is removed when the port reports no device, or one of another kind, in its place, with its LDs, memory domains and chunks: that rule is cxl-fmd's, as the specification does not say what a port reports after a removal. A connection to it goes once its vPPB is read as unbound, as for the LDs of an MLD removed by surprise: "The switch will automatically unbind any existing LD bindings" (Section 7.6.6.8). One whose vPPB stays bound stands as `Absent` until it is deleted; that the vPPB of an SLD's port stays bound is cxl-fmd's assumption, as of such a removal Section 7.6.6.7 says only that "the FM can be notified but is not involved" (see [docs/Redfish-API.md](docs/Redfish-API.md#when-a-device-is-removed)). The managed hot-removal of an MLD, in which "The FM unbinds each assigned LD with the Unbind vPPB command as it receives notifications from each host" (Section 7.6.6.7), is not carried out, and removal is not decided from the port's presence detect (see Known Gaps there). A port whose link is down and that still reports its device keeps it. A directly attached device, and a Multi-Headed device, are not removed: they stay until cxl-fmd is restarted. On hardware, which host or switch port each head of a Multi-Headed device is plugged into is not discovered
- **Dynamic Capacity events**: the FM-side DC event log is drained every `--event-poll` seconds (default 10) and early on MCTP Event Notification, so a host's extents and the tag list can lag the host's response by up to one interval on devices or transports without notifications. A host whose read failed, after a record that named it was cleared or after a command, is read again on every tick until a read completes, and so is a tag list that could not be read, on devices whose DC event log is the host's too (cxl-fmd's choice); with `--event-poll 0` they are not read again until a record or a command reads them. No extent is shown pending: the Pending Reference Bitmap (5608h) is per tag, and an add a host has not answered shows only in the `PCIeDevice`'s `Oem/Cxl/DynamicCapacityTags` (`PendingLDs`). An add or a release is a Redfish `Task` that ends on what the host's extents and the tag list show: while one runs, its host is read on every tick, and with `--event-poll 0` it moves only when something else reads that host. Each host's extents are its own, at its own DPAs, and capacity hosts share is one extent of each with the same Tag; see Comments 2, 4 and 5 of [docs/DCD-FM-API.md](docs/DCD-FM-API.md)

## References

- [CXL Specification 4.0](https://www.computeexpresslink.org/spec-landing) (section and table numbers in this repository are r4.0's)
- [DMTF Redfish Specification](https://www.dmtf.org/standards/redfish)
- [DMTF Redfish CXL Schemas](https://redfish.dmtf.org/schemas/)
- [DMTF DSP0288 CXL to Redfish Mapping](https://www.dmtf.org/dsp/DSP0288)
- [libcxlmi Documentation](https://github.com/computexpresslink/libcxlmi)
