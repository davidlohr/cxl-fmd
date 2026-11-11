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
| `--mctp` | `-m` | Enable MCTP device scanning |
| `--ioctl` | `-i` | Enable ioctl device scanning |
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
| MemoryExtents | `.../PCIeDevices/{dev}/MemoryExtents` | Dynamic capacity extents on the device (DELETE) |
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
| Connection Collection | `/redfish/v1/Fabrics/CXL/Connections` | Host-to-device connections (POST/DELETE) |
| Connection | `.../Connections/{conn}` | Connection with initiator/target endpoints |
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
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCS

# Get vPPB bindings for a VCS
curl http://localhost:8080/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCS/0/VPPBs
```

### Response Headers

All JSON responses include:
- `Content-Type: application/json; charset=utf-8`
- `OData-Version: 4.0`

XML metadata responses include:
- `Content-Type: application/xml; charset=utf-8`
- `OData-Version: 4.0`

### CXL to Redfish Mapping

#### Health Status

| CXL CCI health_status | Redfish Health |
|-----------------------|----------------|
| 0 (no flags) | OK |
| Bit 0 or 1 set | Warning |
| Bit 2 set (HW replacement) | Critical |

#### Media Status

| CXL CCI media_status | Redfish State |
|----------------------|---------------|
| 0 (Normal) | Enabled |
| 1 (Not Ready) | Starting |
| 2 (Write Persistency Lost) | Degraded |
| 3 (All Data Lost) | UnavailableOffline |
| 4 (Prior Loss Event) | Degraded |

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
meson test -C build --suite unit       # All 8 CXL topology combinations
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

Type 3 devices that implement the FM-API DCD Management command set (CXL r3.2 Section 7.6.7.6) are exposed through the 2026.1 Redfish model. DSP0288 does not cover these commands yet; the mapping cxl-fmd implements is specified in [docs/DCD-FM-API.md](docs/DCD-FM-API.md).

```bash
./build/cxl-fmd --mock mock/dcd-device.json

# Device capabilities (Get DCD Info)
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0 | jq .CXLDevice.DynamicCapacity

# Regions a host sees, and the extents on the device
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/MemoryRegions/0
curl -s http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents

# Add 1 GiB to region 0 (Initiate Dynamic Capacity Add)
curl -s -X POST http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Free", "RegionNumber": 0, "Length": 1024, "Tag": "6f1a2c3e-0000-4000-8000-0000000000aa"}'

# Release it again (Initiate Dynamic Capacity Release)
curl -s -X POST http://localhost:8080/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "TagBased", "Tag": "6f1a2c3e-0000-4000-8000-0000000000aa", "Length": 0, "ForcedRemoval": false, "SanitizeOnRelease": true}'
```

## Limitations

The following CXL features are not yet supported:
- **Port-Based Routing (PBR)**: PBR fabric discovery and Global Fabric-Attached Memory Devices
- **Dynamic Capacity events**: DC event records (host and FM side) are not polled yet, so an extent's `Pending` state only changes on rescan

## References

- [CXL Specification 3.2](https://www.computeexpresslink.org/spec-landing)
- [DMTF Redfish Specification](https://www.dmtf.org/standards/redfish)
- [DMTF Redfish CXL Schemas](https://redfish.dmtf.org/schemas/)
- [DMTF DSP0288 CXL to Redfish Mapping](https://www.dmtf.org/dsp/DSP0288)
- [libcxlmi Documentation](https://github.com/computexpresslink/libcxlmi)
