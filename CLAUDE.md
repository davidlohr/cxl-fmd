# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Project Overview

cxl-fmd is a CXL Fabric Manager Daemon that discovers CXL devices and exposes them via Redfish REST APIs.

Supported topologies:
- Direct-attached: SLD, MLD, MH-SLD, MH-MLD
- Switch-attached: SLD, MLD, MH-SLD, MH-MLD

## Build Commands

```bash
# Configure (first time)
meson setup build

# Build
ninja -C build

# Run with mock topology (no hardware needed)
./build/cxl-fmd --mock mock/single-sld.json -v

# Run with real hardware
./build/cxl-fmd -m -v              # MCTP discovery
./build/cxl-fmd -i -v              # ioctl discovery

# Test API
curl http://localhost:8080/redfish/v1 | jq
```

## Testing

```bash
# Run Redfish validator (auto-starts daemon with mock topology)
python scripts/run_redfish_validator.py

# Test MLD topology
python scripts/run_redfish_validator.py --mock-topology mock/mld-device.json

# Run meson tests
meson test -C build
```

## Architecture

```
cxl-fmd/
├── include/
│   ├── cxl-fmd.h                 # Main header with data structures
│   ├── mock.h                    # Mock topology support
│   └── generated/enums/          # Auto-generated enums from DMTF schemas
├── src/
│   ├── main.c                    # Entry point, CLI parsing
│   ├── log.c                     # Logging functions
│   ├── discovery.c               # CXL device discovery via libcxlmi
│   ├── mock.c                    # Mock topology JSON parser
│   ├── topology.c                # In-memory device tree management
│   ├── cci_redfish_map.c         # CXL CCI to Redfish enum mapping
│   ├── dcd.c                     # Dynamic Capacity (FM-API 56h): libcxlmi + mock state
│   └── redfish/
│       ├── server.c              # microhttpd HTTP server, routing
│       ├── handlers.c            # Redfish JSON response generators
│       └── dcd_handlers.c        # MemoryRegions, MemoryExtents, DC actions
├── mock/                         # Mock topology JSON files
│   ├── single-sld.json           # Single SLD (16GB)
│   ├── mld-device.json           # Multiple MLDs with various configs
│   ├── degraded-device.json      # Device with health issues
│   ├── dcd-device.json           # Dynamic Capacity Devices
│   └── unit/                     # Unit test topologies (all 8 combinations)
├── schema/dmtf/
│   ├── csdl/                     # DMTF CSDL (XML) schemas
│   └── json-schema/              # DMTF JSON schemas
├── scripts/
│   ├── update_schemas.py         # Download DMTF schemas
│   ├── generate_enums.py         # Generate C enums from CSDL
│   └── run_redfish_validator.py  # Run DMTF Redfish validator
├── tests/
│   ├── conftest.py               # pytest fixtures
│   └── test_redfish_validator.py # Validator tests
├── subprojects/
│   └── libcxlmi/                 # CXL MI library (CCI commands)
└── docs/
    ├── discovery.md              # Discovery process documentation
    ├── DCD-FM-API.md             # DCD (FM-API 56h) to Redfish mapping
    └── mock-topology.md          # Mock topology language reference
```

## DMTF Schema Workflow (bmcweb model)

Following the bmcweb pattern, schemas are used for **development-time code generation**, not runtime parsing:

```bash
# 1. Download DMTF schemas (one-time or version update)
python3 scripts/update_schemas.py

# 2. Generate C enums from CSDL schemas
python3 scripts/generate_enums.py

# 3. Build with generated enums
ninja -C build
```

**Key principles:**
- Schemas are NOT read at runtime (too slow/memory-intensive for embedded)
- Enums are generated at build time for type-safe Redfish values
- `installed/` symlinks control which schemas are packaged
- Handlers construct JSON manually using generated enums

## Key Data Structures

- `struct fmd_ctx` - Main daemon context
- `struct fmd_topology` - Tree of all discovered components
- `struct fmd_sld` / `fmd_mld` / `fmd_mhd` - Device types
- `struct fmd_switch` / `fmd_port` / `fmd_vcs` - Switch components
- `struct fmd_ld` - Logical device (child of MLD/MHD)
- `struct fmd_head` - MHD head (CXL upstream port)
- `struct fmd_mem_domain` / `fmd_mem_chunk` - Memory resources

## Redfish Endpoints

```
GET /redfish/v1                                              ServiceRoot
GET /redfish/v1/Chassis                                      Chassis Collection
GET /redfish/v1/Chassis/{id}                                 Chassis
GET /redfish/v1/Chassis/{id}/PCIeDevices                     PCIeDevices Collection
GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}               PCIeDevice
GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices
GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}
GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions        DC regions (PATCH)
POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd|Release        DCD actions
GET /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents                                DC extents (DELETE)
POST /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory
GET /redfish/v1/Chassis/{id}/MemoryDomains                   MemoryDomains Collection
GET /redfish/v1/Chassis/{id}/MemoryDomains/{dom}             MemoryDomain
GET /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
GET /redfish/v1/Fabrics                                      Fabrics Collection
GET /redfish/v1/Fabrics/{fab}                                Fabric
GET /redfish/v1/Fabrics/{fab}/Switches                       Switches Collection
GET /redfish/v1/Fabrics/{fab}/Switches/{sw}                  Switch
GET /redfish/v1/Fabrics/{fab}/Switches/{sw}/Ports            Ports Collection
GET /redfish/v1/Fabrics/{fab}/Connections                    Connections Collection
GET /redfish/v1/Systems                                      Systems Collection (hosts)
```

## Discovery Flow

1. `cxlmi_scan_mctp()` - Find MCTP endpoints (requires mctpd)
2. `cxlmi_scan()` - Find ioctl endpoints (/dev/cxl/*)
3. For each endpoint:
   - `cxlmi_cmd_identify()` (0x0001) - Get device type
   - For Type 3 devices:
     - `cxlmi_cmd_fmapi_get_multiheaded_info()` (0x5500) - MHD detection
     - `cxlmi_cmd_fmapi_get_ld_info()` (0x5400) - MLD detection (ld_count > 1)
   - For SLD: `cxlmi_cmd_memdev_identify()` (0x4000), partition/health info
   - For MLD: `cxlmi_cmd_fmapi_get_ld_allocations()` (0x5401), QoS info
   - For MHD: `cxlmi_cmd_fmapi_get_head_info()` (0x5501), LD pool bindings
   - For Switch: `cxlmi_cmd_fmapi_identify_sw_device()` (0x5100), VCS/port info
   - For any Type 3: `cxlmi_cmd_fmapi_get_dcd_info()` (0x5600); on success
     per host 0x5601/0x5603 and device-wide 0x5608 (src/dcd.c)

## CCI Tunneling

CXL CCI commands use tunneling to reach specific components:

```c
/* Tunnel to specific LD in MLD */
DEFINE_CXLMI_TUNNEL_MLD(ti, ld_id);
cxlmi_cmd_memdev_get_health_info(dev->ep, &ti, &health);

/* Tunnel through switch to device on port */
DEFINE_CXLMI_TUNNEL_SWITCH(ti, port);

/* Tunnel through switch to LD in MLD */
DEFINE_CXLMI_TUNNEL_SWITCH_MLD(ti, port, ld);

/* Tunnel to MHD LD Pool CCI */
cxlmi_cmd_fmapi_set_ld_allocations(ep, CXLMI_TUNNEL_MHD, req, rsp);
```

Key tunneling macros from libcxlmi:
- `CXLMI_NO_TUNNEL` - Direct command, no tunneling
- `CXLMI_TUNNEL_MLD(ld)` - Tunnel to LD in direct-attached MLD
- `CXLMI_TUNNEL_SWITCH(port)` - Tunnel through switch to DSP
- `CXLMI_TUNNEL_SWITCH_MLD(port, ld)` - Switch + MLD combined
- `CXLMI_TUNNEL_MHD` - MHD LD Pool CCI access

## Mock Mode

Mock mode allows testing without hardware by parsing JSON topology files:

```bash
./build/cxl-fmd --mock mock/single-sld.json -v
./build/cxl-fmd --mock mock/unit/unit-switch-mhmld.json -v
```

Mock topologies support all 8 device combinations (Direct/Switch × SLD/MLD/MH-SLD/MH-MLD).
See `docs/mock-topology.md` for JSON schema.

## Dependencies

- libcxlmi (subproject) - CXL CCI commands and tunneling
- libmicrohttpd - HTTP server
- jansson - JSON parsing
- pthreads - Thread safety
