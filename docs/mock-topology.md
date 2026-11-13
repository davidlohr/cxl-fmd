# CXL Mock Topology Language

This document describes the JSON-based topology language used by cxl-fmd's mock mode to define simulated CXL fabrics for testing and development.

## Overview

The mock topology language allows you to declaratively define CXL device configurations that mirror real hardware behavior. When cxl-fmd runs with `--mock <file.json>`, it bypasses hardware discovery and instead creates a virtual topology from the JSON specification.

Each mock device is configured with the same data fields that would be returned by CXL CCI (Component Command Interface) commands, ensuring that Redfish API responses accurately reflect real-world scenarios.

## Usage

```bash
cxl-fmd --mock topology.json [-p PORT] [-v]
```

## Available Mock Topologies

The project includes pre-built mock topologies for testing:

### Standard Topologies (`mock/`)

| File | Description |
|------|-------------|
| `single-sld.json` | Single 16GB direct-attached SLD |
| `multi-device.json` | Multiple direct-attached devices |
| `mld-device.json` | Multi-Logical Device configurations |
| `mhd-device.json` | Multi-Headed Device configurations |
| `switch-attached.json` | Switch-attached topology with VCS |
| `multi-host-vcs.json` | Multi-host fabric with VCS partitioning |
| `degraded-device.json` | Devices with various health states |
| `dcd-device.json` | Dynamic Capacity Devices (SLD DCD and two-host MLD DCD with a shared region) |

### Unit Test Topologies (`mock/unit/`)

Minimal topologies covering all 8 CXL device type combinations:

| File | Description |
|------|-------------|
| `unit-direct-sld.json` | Direct-attached SLD |
| `unit-direct-mld.json` | Direct-attached MLD (2 LDs) |
| `unit-direct-mhsld.json` | Direct-attached MH-SLD (2 heads) |
| `unit-direct-mhmld.json` | Direct-attached MH-MLD (2 heads, 2 LDs each) |
| `unit-switch-sld.json` | Switch-attached SLD |
| `unit-switch-mld.json` | Switch-attached MLD |
| `unit-switch-mhsld.json` | Switch-attached MH-SLD (2 VCS) |
| `unit-switch-mhmld.json` | Switch-attached MH-MLD (2 VCS) |

## File Structure

A topology file is a JSON object with the following top-level structure:

```json
{
    "version": 1,
    "id": "topology_identifier",
    "description": "Human-readable description",
    "hosts": [ ... ],
    "devices": [ ... ],
    "switches": [ ... ]
}
```

### Top-Level Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `version` | integer | No | Schema version (default: 1) |
| `id` | string | No | Unique identifier for this topology |
| `description` | string | No | Human-readable description |
| `hosts` | array | No | Array of host objects (for MHD topologies) |
| `devices` | array | Yes | Array of CXL device objects (SLD, MLD, or MHD) |
| `switches` | array | No | Array of CXL switch objects |

## Device Configuration

Each device in the `devices` array represents a CXL endpoint (typically a Type 3 memory device).

### Basic Identification

```json
{
    "id": "cxl_mem0",
    "vendor_id": "0x8086",
    "device_id": "0x0d93",
    "serial_number": "0x0123456789abcdef",
    "component_type": 3,
    "ld_count": 1
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | **Yes** | Unique device identifier (used in Redfish URIs) |
| `vendor_id` | string/int | No | PCI Vendor ID (hex string or integer) |
| `device_id` | string/int | No | PCI Device ID (hex string or integer) |
| `serial_number` | string/int | No | 64-bit device serial number |
| `component_type` | integer | No | CXL component type (default: 3 for Type 3) |
| `ld_count` | integer | No | Number of logical devices (1 for SLD, >1 for MLD) |
| `max_ld_count` | integer | No | Maximum supported LDs (MLD only, default: ld_count) |
| `memory_granularity` | integer | No | Memory allocation granularity (256MB units, default: 1) |
| `qos_telemetry_capability` | integer | No | QoS telemetry capability bitmask |

#### Component Types

| Value | Type | Description |
|-------|------|-------------|
| 0 | Switch | CXL Switch |
| 3 | Type 3 | CXL Type 3 Memory Device |

### Memory Device Identification (CCI 0x4000)

The `identify_memory_device` object contains fields returned by the CXL Identify Memory Device command.

```json
{
    "identify_memory_device": {
        "fw_revision": "1.2.3",
        "total_capacity_mb": 16384,
        "volatile_capacity_mb": 0,
        "persistent_capacity_mb": 16384,
        "partition_align_mb": 256,
        "lsa_size": 65536,
        "poison_list_max": 256,
        "inject_poison_limit": 0,
        "dc_region_count": 0
    }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `fw_revision` | string | "0.0.0" | Firmware revision string (max 16 chars) |
| `total_capacity_mb` | integer | 0 | Total device capacity in MiB |
| `volatile_capacity_mb` | integer | 0 | Volatile memory capacity in MiB |
| `persistent_capacity_mb` | integer | 0 | Persistent memory capacity in MiB |
| `partition_align_mb` | integer | 256 | Partition alignment in MiB |
| `lsa_size` | integer | 0 | Label Storage Area size in bytes |
| `poison_list_max` | integer | 256 | Maximum poison list entries |
| `inject_poison_limit` | integer | 0 | Poison injection limit |
| `dc_region_count` | integer | 0 | Dynamic Capacity region count |

### Partition Information (CCI 0x4100)

The `partition_info` object contains current partition configuration.

```json
{
    "partition_info": {
        "active_volatile_mb": 8192,
        "active_persistent_mb": 8192,
        "next_volatile_mb": 8192,
        "next_persistent_mb": 8192
    }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `active_volatile_mb` | integer | Currently active volatile capacity (MiB) |
| `active_persistent_mb` | integer | Currently active persistent capacity (MiB) |
| `next_volatile_mb` | integer | Volatile capacity after next reset (MiB) |
| `next_persistent_mb` | integer | Persistent capacity after next reset (MiB) |

If omitted, partition info defaults to the values from `identify_memory_device`.

### Health Information (CCI 0x4200)

The `health_info` object defines device health state, which is mapped to Redfish Status.

```json
{
    "health_info": {
        "health_status": 0,
        "media_status": 0,
        "additional_status": 0,
        "life_used": 5,
        "temperature": 42,
        "dirty_shutdown_count": 0,
        "correctable_volatile_errors": 0,
        "correctable_persistent_errors": 0
    }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `health_status` | integer | 0 | Health status bitmask (see below) |
| `media_status` | integer | 0 | Media status code (see below) |
| `additional_status` | integer | 0 | Additional status flags |
| `life_used` | integer | 0 | Percentage of life used (0-100) |
| `temperature` | integer | 25 | Device temperature in Celsius |
| `dirty_shutdown_count` | integer | 0 | Count of dirty shutdowns |
| `correctable_volatile_errors` | integer | 0 | Correctable volatile memory errors |
| `correctable_persistent_errors` | integer | 0 | Correctable persistent memory errors |

#### Health Status Bitmask

The `health_status` field is a bitmask:

| Bit | Value | Meaning | Redfish Health |
|-----|-------|---------|----------------|
| 0 | 1 | Maintenance Needed | Warning |
| 1 | 2 | Performance Degraded | Warning |
| 2 | 4 | Hardware Replacement Needed | Critical |

Multiple bits can be set. The most severe condition determines the Redfish Health value.

**Examples:**
- `health_status: 0` → Health: OK
- `health_status: 1` → Health: Warning (Maintenance Needed)
- `health_status: 3` → Health: Warning (Maintenance + Performance)
- `health_status: 4` → Health: Critical (HW Replacement)
- `health_status: 7` → Health: Critical (All flags set)

#### Media Status Codes

The `media_status` field indicates media condition:

| Value | Meaning | Redfish State |
|-------|---------|---------------|
| 0 | Normal | Enabled |
| 1 | Not Ready | Starting |
| 2 | Write Persistency Lost | Degraded |
| 3 | All Data Lost | UnavailableOffline |
| 4 | Write Persistency Loss Event (in prior power cycle) | Degraded |

### QoS Control (FM-API 0x5403)

For MLDs, the `qos_control` object defines QoS telemetry and congestion control settings. These map to the Redfish `Port.CXL.Congestion` properties.

```json
{
    "qos_control": {
        "enabled": true,
        "temp_throughput_reduction_enabled": false,
        "egress_moderate_pct": 50,
        "egress_severe_pct": 75,
        "backpressure_sample_interval": 0,
        "reqcmp_basis": 0,
        "completion_collection_interval": 0
    }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | boolean | false | Egress port congestion telemetry enabled |
| `temp_throughput_reduction_enabled` | boolean | false | Temporary throughput reduction enabled |
| `egress_moderate_pct` | integer | 10 | Moderate congestion threshold (%) |
| `egress_severe_pct` | integer | 25 | Severe congestion threshold (%) |
| `backpressure_sample_interval` | integer | 0 | Backpressure sample interval |
| `reqcmp_basis` | integer | 0 | ReqCmpBasis value |
| `completion_collection_interval` | integer | 0 | Completion collection interval |

### QoS Status (FM-API 0x5405)

For MLDs, the `qos_status` object provides runtime QoS metrics. The backpressure average percentage maps to the Redfish `PortMetrics.CXL.BackpressureAveragePercentage` property.

```json
{
    "qos_status": {
        "backpressure_avg_pct": 25
    }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `backpressure_avg_pct` | integer | - | Average backpressure percentage (0-100) |

If `qos_status` is present, the `BackpressureAveragePercentage` will be included in the `PortMetrics` resource for the switch port connected to the MLD.

### Command Effects Log (CEL)

The `cel_commands` array specifies which CCI (Component Command Interface) commands the device supports. This is derived from the Command Effects Log in real hardware.

```json
{
    "cel_commands": [
        "identify",
        "get_supported_logs",
        "get_log",
        "memdev_identify",
        "get_partition_info",
        "get_health_info",
        "get_alert_config",
        "get_poison_list"
    ]
}
```

If `cel_commands` is not specified, defaults are applied based on device type:

**Type 3 Memory Device defaults:**
- `identify` (0x0001)
- `get_supported_logs` (0x0400)
- `get_log` (0x0401)
- `memdev_identify` (0x4000)
- `get_partition_info` (0x4100)
- `get_health_info` (0x4200)

#### Available Commands

| Command Name | Opcode | Description |
|--------------|--------|-------------|
| `identify` | 0x0001 | Identify device |
| `bg_op_status` | 0x0002 | Background Operation Status |
| `get_supported_logs` | 0x0400 | Get Supported Logs |
| `get_log` | 0x0401 | Get Log |
| `memdev_identify` | 0x4000 | Identify Memory Device |
| `memdev_capacity` | 0x4001 | Get Memory Device Capacity |
| `get_partition_info` | 0x4100 | Get Partition Info |
| `set_partition_info` | 0x4101 | Set Partition Info |
| `get_health_info` | 0x4200 | Get Health Info |
| `get_alert_config` | 0x4201 | Get Alert Configuration |
| `set_alert_config` | 0x4202 | Set Alert Configuration |
| `get_shutdown_state` | 0x4203 | Get Shutdown State |
| `set_shutdown_state` | 0x4204 | Set Shutdown State |
| `get_poison_list` | 0x4300 | Get Poison List |
| `inject_poison` | 0x4301 | Inject Poison |
| `clear_poison` | 0x4302 | Clear Poison |

### Logical Devices Configuration (MLD Support)

For Multi-Logical Devices (MLDs), use the `logical_devices` array to define per-LD configuration. If omitted, the device is treated as an SLD with a single LD using the entire device capacity.

#### MLD Logical Device Architecture

MLDs have the following architecture:

- **LD-0 through LD-15 (Host-bindable)**: All logical devices (LD-0 through LD-15) are host-bindable. They can be assigned to hosts for memory access via vPPB binding.

- **FMLD (FM-owned LD at LD-ID FFFFh)**: The Fabric Manager-owned LD is a separate virtual LD accessible only via LD-ID FFFFh in the CXL LD-ID TLP Prefix. It is used for device administration and is not represented in the logical_devices array.

The FM accesses the FMLD implicitly via MLD Component commands (opcodes 5400h-5409h). The FMLD is never bound to hosts.

```json
{
    "id": "cxl_mld_0",
    "ld_count": 4,

    "identify_memory_device": {
        "total_capacity_mb": 131072,
        "persistent_capacity_mb": 131072
    },

    "logical_devices": [
        {
            "ld_id": 0,
            "memory_size_mb": 32768,
            "qos_allocated_bw": 100,
            "qos_limit_bw": 100
        },
        {
            "ld_id": 1,
            "memory_size_mb": 32768,
            "qos_allocated_bw": 75,
            "qos_limit_bw": 100,
            "health_info": {
                "health_status": 1,
                "media_status": 0
            }
        }
    ]
}
```

#### Logical Device Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `ld_id` | integer | array index | Logical Device identifier (0 to ld_count-1) |
| `memory_size_mb` | integer | auto | Memory allocated to this LD (MiB) |
| `qos_allocated_bw` | integer | 100 | Allocated bandwidth percentage (0-100) |
| `qos_limit_bw` | integer | 100 | Bandwidth limit percentage (0-100) |
| `health_info` | object | inherited | Per-LD health override (optional) |

#### Per-LD Health Override

Each LD can optionally have its own health status, allowing simulation of scenarios where individual LDs within an MLD have different health states:

```json
{
    "ld_id": 2,
    "memory_size_mb": 16384,
    "health_info": {
        "health_status": 2,
        "media_status": 0
    }
}
```

If `health_info` is not specified for an LD, it inherits the device-level health status.

#### Automatic LD Configuration

If `logical_devices` array is not provided but `ld_count > 1`, LDs are automatically created with:
- Equal memory distribution: `total_capacity / ld_count`
- Default QoS: 100% allocated and limit
- Inherited health status from device level

### Dynamic Capacity (FM-API 0x5600-0x5608)

A `dcd` object on any Type 3 device (SLD, MLD or MHD) makes it a Dynamic
Capacity Device. It mirrors the FM-API DCD Management responses; see
[DCD-FM-API.md](DCD-FM-API.md) for the Redfish mapping.

```json
{
    "dcd": {
        "num_hosts": 1,
        "add_policies": ["Free", "Contiguous", "Prescriptive"],
        "release_policies": ["TagBased", "Prescriptive"],
        "max_extents": 64,
        "max_tags": 16,
        "regions": [
            {
                "region_id": 0,
                "base_mb": 16384,
                "region_len_mb": 32768,
                "block_size_mb": 256,
                "supported_block_sizes_mb": [256, 512, 1024],
                "sanitize_on_release": true,
                "sanitize_configurable": true,
                "shareable": false
            }
        ],
        "extents": [
            {
                "start_mb": 16384,
                "len_mb": 4096,
                "tag": "6f1a2c3e-0000-4000-8000-000000000001",
                "host_id": 0
            }
        ]
    }
}
```

#### DCD Fields (Get DCD Info 0x5600)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `num_hosts` | integer | 1 (SLD), `ld_count` (MLD), `num_heads` (MHD) | Number of hosts the device serves |
| `add_policies` | array | Free, Contiguous, Prescriptive | Supported add policies; `EnableSharedAccess` for shared regions |
| `release_policies` | array | TagBased, Prescriptive | Supported release policies |
| `max_extents` | integer | 256 | Extents supported per host (`TotalSupportedExtents`) |
| `max_tags` | integer | 64 | Tags supported per host (`TotalSupportedTags`) |
| `total_capacity_mb` | integer | sum of region lengths | `TotalDynamicCapacityMiB` |
| `regions` | array | [] | DC region configurations |
| `extents` | array | [] | Extents already allocated |

#### Region Fields (Get Host DC Region Configuration 0x5601)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `region_id` | integer | array index | Region number (0-7) |
| `base_mb` | integer | 0 | Region base in the device address space (DPA) |
| `region_len_mb` | integer | - | Region length; a region of length 0 is skipped |
| `decode_len_mb` | integer | `region_len_mb` | HDM decode length |
| `block_size_mb` | integer | 256 | Current block size |
| `supported_block_sizes_mb` | array | [`block_size_mb`] | Block sizes accepted by PATCH |
| `sanitize_on_release` | boolean | false | Current Sanitize on Release setting |
| `sanitize_configurable` | boolean | true | Whether PATCH may change `SanitizeOnRelease` |
| `shareable` | boolean | false | Region can be shared between hosts |
| `nonvolatile` | boolean | false | Non-volatile region |
| `hw_coherency` | boolean | false | Hardware-managed coherency |
| `interconnect_dcm` | boolean | false | Interconnect-specific dynamic capacity management (CDAT DSMAS flag) |
| `read_only` | boolean | false | Read-only region |
| `hosts` | array | all hosts | Hosts that see this region. Host IDs are the LD-ID (MLD) or head (MHD) |

Each host listed gets its own view of the region under its
`CXLLogicalDevice`, which is how FM-API 0x5601 reports them.

#### Extent Fields (Get DC Region Extent Lists 0x5603, List Tags 0x5608)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `start_mb` | integer | - | Extent start (DPA); must fall inside a region |
| `len_mb` | integer | - | Extent length |
| `tag` | string | all zeros | 16-byte tag as a UUID string |
| `shared_seq` | integer | 0 | Shared extent sequence number |
| `host_id` / `hosts` | integer / array | 0 | Host(s) the extent is allocated to |
| `pending` | boolean | false | Host has not accepted the extent yet |
| `fm_referenced` | boolean | false | Fabric Manager holds a reference on the tag |

Mock add and release operate on this state. The mock host accepts added
capacity immediately, so new extents are never pending.

### Switch Attachment

For switch-attached topologies, specify the parent switch:

```json
{
    "id": "cxl_dev0",
    "parent_switch": "switch_0",
    "parent_port": 1,
    ...
}
```

| Field | Type | Description |
|-------|------|-------------|
| `parent_switch` | string | ID of the parent switch |
| `parent_port` | integer | Downstream port number on the switch |

## Host Configuration

Hosts represent computing systems that can access CXL memory. For direct-attached devices, hosts connect directly to device heads. For switch-attached topologies, hosts connect via VCS upstream ports.

```json
{
    "hosts": [
        {
            "id": "host0",
            "name": "Compute Node 0",
            "direct_attached": true
        },
        {
            "id": "host1",
            "name": "Compute Node 1",
            "vcs_id": 1,
            "usp_port_id": 1,
            "direct_attached": false
        }
    ]
}
```

### Host Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | **Required** | Unique host identifier |
| `name` | string | id | Human-readable host name |
| `direct_attached` | boolean | true | True for direct-attached, false for switch-attached |
| `vcs_id` | integer | 0 | VCS ID for switch-attached hosts |
| `usp_port_id` | integer | 0 | Upstream port ID for switch-attached hosts |

### Automatic Host Creation

- **Direct-attached**: If no hosts are defined and devices exist, a default "local" host is created representing the system running cxl-fmd.
- **Switch-attached**: Hosts are automatically created for each VCS in the switch configuration.

## Switch Configuration

Switches define the fabric topology for switch-attached devices.

```json
{
    "switches": [
        {
            "id": "switch_0",
            "vendor_id": "0x8086",
            "device_id": "0x0e00",
            "serial_number": "0x5357495443483030",
            "upstream_port": 0,
            "max_vcs": 4,

            "ports": [
                {
                    "port_number": 0,
                    "type": "upstream",
                    "link_status": "up",
                    "max_link_width": 16,
                    "negotiated_link_width": 16
                },
                {
                    "port_number": 1,
                    "type": "downstream",
                    "link_status": "up",
                    "conn_dev_type": "sld",
                    "max_link_width": 16,
                    "negotiated_link_width": 16
                }
            ],

            "vcs": [
                {
                    "vcs_id": 0,
                    "state": 1,
                    "upstream_port_id": 0,
                    "vppbs": [
                        { "port_id": 1, "device_ref": "dev0", "binding_status": "bound" }
                    ]
                }
            ]
        }
    ]
}
```

### Switch Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | **Required** | Unique switch identifier |
| `vendor_id` | string/int | 0 | PCI Vendor ID |
| `device_id` | string/int | 0 | PCI Device ID |
| `serial_number` | string/int | 0 | Switch serial number |
| `upstream_port` | integer | 0 | Upstream port number |
| `max_vcs` | integer | 8 | Maximum VCS supported |
| `ports` | array | [] | Port configuration array |
| `vcs` | array | [] | VCS configuration array |

### Port Configuration

Each port in the `ports` array defines a physical switch port.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `port_number` | integer | array index | Physical port number |
| `type` | string | "downstream" | Port type: "upstream" or "downstream" |
| `link_status` | string | "down" | Link status: "up" or "down" |
| `conn_dev_type` | string | "none" | Connected device type (see below) |
| `max_link_width` | integer | 16 | Maximum link width (lanes) |
| `negotiated_link_width` | integer | 0 | Negotiated link width |
| `supported_ld_count` | integer | 16 | Supported LD count for MLD |
| `conn_dev_cxl_ver` | integer | 0 | Connected device CXL version |
| `port_cxl_ver_bitmask` | integer | 0x06 | Port CXL version support bitmask |
| `supported_link_speeds` | integer | 0x1F | Supported link speeds bitmask |

#### Connected Device Types

| Value | Description |
|-------|-------------|
| `"none"` | No device connected |
| `"pcie"` | PCIe device |
| `"switch"` | CXL Switch (CXL.io only) |
| `"sld"` | Single Logical Device |
| `"mld"` | Multi-Logical Device |

### VCS Configuration

Virtual CXL Switches partition switch resources for multi-host configurations.

**Mock Topology VCS Conventions:**
- **Single VCS**: One upstream port, one or more downstream ports
- **Multiple VCS**: Each VCS has its own upstream port
- **vPPB Ordering**: vPPB #0 represents upstream binding, followed by downstream bindings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `vcs_id` | integer | array index | VCS identifier |
| `state` | integer | 1 | VCS state (0=disabled, 1=enabled) |
| `upstream_port_id` | integer | 0 | Upstream port for this VCS |
| `hdm_decoders` | integer | 8 | Number of HDM decoders |
| `vppbs` | array | [] | Virtual PPB bindings |

### vPPB Configuration

Virtual PCI-to-PCI Bridges (vPPBs) bind physical ports/LDs to a VCS.

**vPPB Ordering (mock topology convention):**
- vPPB #0 represents the upstream port binding
- vPPB #1+ represent downstream port/LD bindings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `vppb_id` | integer | array index | vPPB identifier (0 = USP, 1+ = DSP) |
| `binding_status` | string | "bound_physical_port" | Binding status (see below) |
| `bound_port_id` | integer | - | Physical port number being bound |
| `bound_ld_id` | integer | - | Logical device ID for MLD bindings |
| `bound_pbr_id` | integer | - | PBR route ID for PBR bindings |

#### Binding Status Values

| Value | Description |
|-------|-------------|
| `"unbound"` | vPPB not bound (available for dynamic binding) |
| `"bind_in_progress"` | Binding in progress |
| `"unbind_in_progress"` | Unbinding in progress |
| `"bound_physical_port"` | vPPB bound to entire port (SLD, PCIe, or USP) |
| `"bound_ld"` | vPPB bound to specific LD within MLD |
| `"bound_pbr"` | vPPB bound to PBR route (future PBR fabric support) |
| `"busy"` | vPPB binding/unbinding operation in progress |

## Complete Examples

### Single Direct-Attached SLD

A minimal configuration for a single 16GB persistent memory device:

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
            "serial_number": "0x0123456789abcdef",
            "component_type": 3,

            "identify_memory_device": {
                "fw_revision": "1.0.0",
                "total_capacity_mb": 16384,
                "persistent_capacity_mb": 16384
            },

            "health_info": {
                "health_status": 0,
                "media_status": 0,
                "life_used": 5,
                "temperature": 40
            }
        }
    ]
}
```

### Multi-Vendor Configuration

Testing interoperability with devices from different vendors:

```json
{
    "version": 1,
    "id": "multi_vendor",
    "description": "Mixed vendor CXL devices",

    "devices": [
        {
            "id": "samsung_cxl_0",
            "vendor_id": "0x144d",
            "device_id": "0xa80a",
            "serial_number": "0x53414d53554e4730",

            "identify_memory_device": {
                "fw_revision": "2.1.0",
                "total_capacity_mb": 32768,
                "persistent_capacity_mb": 32768
            }
        },
        {
            "id": "micron_cxl_0",
            "vendor_id": "0x1344",
            "device_id": "0xc301",
            "serial_number": "0x4d4943524f4e3030",

            "identify_memory_device": {
                "fw_revision": "3.0.1",
                "total_capacity_mb": 65536,
                "volatile_capacity_mb": 8192,
                "persistent_capacity_mb": 57344
            }
        }
    ]
}
```

### Degraded Device Scenarios

Testing health status to Redfish mapping:

```json
{
    "version": 1,
    "id": "health_test",
    "description": "Devices with various health states",

    "devices": [
        {
            "id": "healthy_device",
            "identify_memory_device": {
                "total_capacity_mb": 16384,
                "persistent_capacity_mb": 16384
            },
            "health_info": {
                "health_status": 0,
                "media_status": 0
            }
        },
        {
            "id": "maintenance_needed",
            "identify_memory_device": {
                "total_capacity_mb": 16384,
                "persistent_capacity_mb": 16384
            },
            "health_info": {
                "health_status": 1,
                "media_status": 0,
                "life_used": 80
            }
        },
        {
            "id": "critical_device",
            "identify_memory_device": {
                "total_capacity_mb": 16384,
                "persistent_capacity_mb": 16384
            },
            "health_info": {
                "health_status": 4,
                "media_status": 3,
                "life_used": 100,
                "dirty_shutdown_count": 50
            }
        }
    ]
}
```

### Switch-Attached Topology (Multi-VCS)

A CXL switch with multiple VCSs for multi-host access.

```json
{
    "version": 1,
    "id": "switched_fabric",
    "description": "CXL switch with VCS partitioning for two hosts",

    "switches": [
        {
            "id": "switch_0",
            "vendor_id": "0x8086",
            "device_id": "0x0e00",
            "max_vcs": 4,

            "ports": [
                { "port_number": 0, "type": "upstream", "link_status": "up", "_comment": "USP for VCS0/Host A" },
                { "port_number": 1, "type": "upstream", "link_status": "up", "_comment": "USP for VCS1/Host B" },
                { "port_number": 2, "type": "downstream", "link_status": "up", "conn_dev_type": "sld" },
                { "port_number": 3, "type": "downstream", "link_status": "up", "conn_dev_type": "sld" },
                { "port_number": 4, "type": "downstream", "link_status": "up", "conn_dev_type": "mld" }
            ],

            "vcs": [
                {
                    "_comment": "VCS0 for Host A - vPPB #0 is always USP",
                    "vcs_id": 0,
                    "state": 1,
                    "upstream_port_id": 0,
                    "vppbs": [
                        { "vppb_id": 0, "binding_status": "bound_physical_port", "bound_port_id": 0, "_comment": "USP" },
                        { "vppb_id": 1, "binding_status": "bound_physical_port", "bound_port_id": 2 },
                        { "vppb_id": 2, "binding_status": "bound_ld", "bound_port_id": 4, "bound_ld_id": 0 },
                        { "vppb_id": 3, "binding_status": "bound_ld", "bound_port_id": 4, "bound_ld_id": 1 }
                    ]
                },
                {
                    "_comment": "VCS1 for Host B - separate USP (port 1)",
                    "vcs_id": 1,
                    "state": 1,
                    "upstream_port_id": 1,
                    "vppbs": [
                        { "vppb_id": 0, "binding_status": "bound_physical_port", "bound_port_id": 1, "_comment": "USP" },
                        { "vppb_id": 1, "binding_status": "bound_physical_port", "bound_port_id": 3 },
                        { "vppb_id": 2, "binding_status": "bound_ld", "bound_port_id": 4, "bound_ld_id": 2 },
                        { "vppb_id": 3, "binding_status": "bound_ld", "bound_port_id": 4, "bound_ld_id": 3 }
                    ]
                }
            ]
        }
    ],

    "devices": [
        {
            "id": "dev_port2",
            "parent_switch": "switch_0",
            "parent_port": 2,
            "identify_memory_device": {
                "total_capacity_mb": 32768,
                "persistent_capacity_mb": 32768
            }
        },
        {
            "id": "dev_port3",
            "parent_switch": "switch_0",
            "parent_port": 3,
            "identify_memory_device": {
                "total_capacity_mb": 32768,
                "persistent_capacity_mb": 32768
            }
        },
        {
            "id": "mld_port4",
            "parent_switch": "switch_0",
            "parent_port": 4,
            "ld_count": 4,
            "identify_memory_device": {
                "total_capacity_mb": 131072,
                "persistent_capacity_mb": 131072
            }
        }
    ]
}
```

This example demonstrates:
- **Multiple USPs**: Port 0 for VCS0 (Host A), Port 1 for VCS1 (Host B)
- **vPPB Ordering**: vPPB #0 is always the USP in each VCS
- **VCS 0**: SLD on port 2 + MLD LD0/LD1 on port 4
- **VCS 1**: SLD on port 3 + MLD LD2/LD3 on port 4 (sharing physical port)
- **MLD Sharing**: Both hosts access same physical MLD via different LDs

### Multi-Headed Device (MHD)

A Multi-Headed Device (MHD) has multiple CXL upstream ports (heads), allowing multiple hosts to share the device's memory. Each head can present as SLD (single LD) or MLD (multiple LDs).

```json
{
    "version": 1,
    "id": "mhd_topology",
    "description": "Multi-Headed Device examples",

    "hosts": [
        { "id": "host0", "name": "Compute Node 0", "direct_attached": true },
        { "id": "host1", "name": "Compute Node 1", "direct_attached": true }
    ],

    "devices": [
        {
            "_comment": "MH-SLD: Each head presents as single-LD device",
            "id": "cxl_mhsld_0",
            "type": "MHD",
            "vendor_id": "0x8086",
            "device_id": "0x0d95",

            "num_heads": 2,
            "max_heads": 4,

            "heads": [
                {
                    "head_id": 0,
                    "presentation": "SLD",
                    "lds": [0],
                    "max_link_width": 16,
                    "attached_host": "host0"
                },
                {
                    "head_id": 1,
                    "presentation": "SLD",
                    "lds": [1],
                    "attached_host": "host1"
                }
            ],

            "ld_pool": [
                { "pool_index": 0, "memory_size_mb": 16384, "qos_allocated_bw": 100 },
                { "pool_index": 1, "memory_size_mb": 16384, "qos_allocated_bw": 100 }
            ]
        },

        {
            "_comment": "MH-MLD: Each head presents as MLD with multiple LDs",
            "id": "cxl_mhmld_0",
            "type": "MHD",
            "vendor_id": "0x8086",
            "device_id": "0x0d96",

            "num_heads": 2,
            "max_heads": 2,

            "heads": [
                {
                    "head_id": 0,
                    "presentation": "MLD",
                    "lds": [0, 1, 2, 3],
                    "attached_host": "host0",
                    "_comment": "Host 0 sees 4 LDs (LD-IDs 0-3)"
                },
                {
                    "head_id": 1,
                    "presentation": "MLD",
                    "lds": [4, 5],
                    "attached_host": "host1",
                    "_comment": "Host 1 sees 2 LDs (LD-IDs 0-1, mapped from pool 4-5)"
                }
            ],

            "ld_pool": [
                { "pool_index": 0, "memory_size_mb": 8192, "qos_allocated_bw": 100 },
                { "pool_index": 1, "memory_size_mb": 8192, "qos_allocated_bw": 75 },
                { "pool_index": 2, "memory_size_mb": 4096, "qos_allocated_bw": 50 },
                { "pool_index": 3, "memory_size_mb": 4096, "qos_allocated_bw": 50 },
                { "pool_index": 4, "memory_size_mb": 16384, "qos_allocated_bw": 100 },
                { "pool_index": 5, "memory_size_mb": 8192, "qos_allocated_bw": 75 }
            ]
        }
    ]
}
```

#### MHD Device Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | - | Must be `"MHD"` for multi-headed devices |
| `vendor_id` | string/int | 0 | PCI Vendor ID (hex string or integer) |
| `device_id` | string/int | 0 | PCI Device ID (hex string or integer) |
| `serial_number` | string/int | 0 | 64-bit device serial number |
| `num_heads` | integer | 2 | Number of active heads |
| `max_heads` | integer | num_heads | Maximum supported heads |
| `heads` | array | [] | Head configuration array |
| `ld_pool` | array | [] | LD pool configuration |
| `dynamic_capacity` | boolean | false | Supports dynamic capacity |
| `memory_granularity` | integer | 1 | Memory granularity (256MB units) |
| `qos_telemetry_capability` | integer | 0 | QoS telemetry capability bitmask |
| `qos_control` | object | - | QoS control settings (see QoS Control section) |
| `qos_status` | object | - | QoS status metrics (see QoS Status section) |
| `cel_commands` | array | defaults | Supported CCI commands (see CEL section) |
| `health_info` | object | healthy | Device-level health (default for pool LDs) |

#### Head Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `head_id` | integer | array index | Head identifier (0 to num_heads-1) |
| `presentation` | string | "SLD" | How head appears to host: "SLD" or "MLD" |
| `lds` | array | [] | Pool indices of LDs mapped to this head |
| `max_link_width` | integer | 16 | Maximum link width (lanes) |
| `negotiated_link_width` | integer | 16 | Negotiated link width |
| `attached_host` | string | - | Host ID connected to this head (required for connection validation) |

**Note:** The `attached_host` field is critical for MHD memory provisioning. When creating a Connection, the server validates that the host's attached head can access the chunk's pool LD. Without this field set, Connection creation for MHD will fail.

#### LD Pool Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `pool_index` | integer | array index | Pool index (0 to total_lds-1) |
| `memory_size_mb` | integer | 0 | Total memory size in MiB |
| `volatile_capacity_mb` | integer | 0 | Volatile memory capacity in MiB |
| `persistent_capacity_mb` | integer | memory_size_mb | Persistent memory capacity in MiB |
| `qos_allocated_bw` | integer | 100 | Allocated bandwidth percentage (0-100) |
| `qos_limit_bw` | integer | 100 | Bandwidth limit percentage (0-100) |
| `health_info` | object | inherited | Per-LD health override (optional) |

**Note:** If neither `volatile_capacity_mb` nor `persistent_capacity_mb` is specified, all memory defaults to persistent.

#### MHD Presentation Types

- **MH-SLD (Multi-Head SLD)**: Each head presents a single LD to its attached host. There is a 1:1 mapping between heads and LDs.

- **MH-MLD (Multi-Head MLD)**: Each head presents as an MLD with multiple LDs. Each LD in the pool can be mapped to at most one head. The `visible_ld_id` (what the host sees) is derived from the order in the head's `lds` array.

- **Mixed**: A device can have some heads presenting as SLD and others as MLD.

#### Switch-Attached MHD

MHD devices can be attached behind a CXL switch, with each head mapped to a different VCS:

```json
{
    "switches": [{
        "id": "switch_0",
        "ports": [
            { "port_number": 0, "type": "upstream", "link_status": "up" },
            { "port_number": 1, "type": "upstream", "link_status": "up" },
            { "port_number": 2, "type": "downstream", "link_status": "up", "conn_dev_type": "mld" }
        ],
        "vcs": [
            {
                "vcs_id": 0,
                "upstream_port_id": 0,
                "vppbs": [
                    { "vppb_id": 0, "binding_status": "bound_physical_port", "bound_port_id": 0 },
                    { "vppb_id": 1, "binding_status": "bound_ld", "bound_port_id": 2, "bound_ld_id": 0 }
                ]
            },
            {
                "vcs_id": 1,
                "upstream_port_id": 1,
                "vppbs": [
                    { "vppb_id": 0, "binding_status": "bound_physical_port", "bound_port_id": 1 },
                    { "vppb_id": 1, "binding_status": "bound_ld", "bound_port_id": 2, "bound_ld_id": 1 }
                ]
            }
        ]
    }],
    "devices": [{
        "id": "mhd_0",
        "type": "MHD",
        "parent_switch": "switch_0",
        "parent_port": 2,
        "num_heads": 2,
        "heads": [
            { "head_id": 0, "presentation": "SLD", "lds": [0], "attached_host": "switch_0_host0" },
            { "head_id": 1, "presentation": "SLD", "lds": [1], "attached_host": "switch_0_host1" }
        ],
        "ld_pool": [
            { "pool_index": 0, "memory_size_mb": 8192 },
            { "pool_index": 1, "memory_size_mb": 8192 }
        ]
    }]
}
```

Key points for switch-attached MHD:
- Each VCS represents a host connection via its upstream port
- vPPB bindings use `bound_ld` status to bind specific pool LDs to VCS
- `attached_host` references auto-created VCS hosts (format: `{switch_id}_host{vcs_id}`)
- Pool LDs are bound to VCS via vPPB, mapping heads to hosts through the switch

### Multi-Logical Device (MLD)

A device exposing multiple logical devices with per-LD memory allocation, QoS settings, and health status:

```json
{
    "version": 1,
    "id": "mld_config",
    "description": "Multi-Logical Device with 4 LDs",

    "devices": [
        {
            "id": "cxl_mld_0",
            "vendor_id": "0x8086",
            "device_id": "0x0d94",

            "identify_memory_device": {
                "fw_revision": "2.0.0",
                "total_capacity_mb": 131072,
                "persistent_capacity_mb": 131072
            },

            "health_info": {
                "health_status": 0,
                "media_status": 0
            },

            "logical_devices": [
                {
                    "ld_id": 0,
                    "memory_size_mb": 65536,
                    "qos_allocated_bw": 100,
                    "qos_limit_bw": 100
                },
                {
                    "ld_id": 1,
                    "memory_size_mb": 32768,
                    "qos_allocated_bw": 75,
                    "qos_limit_bw": 100
                },
                {
                    "ld_id": 2,
                    "memory_size_mb": 16384,
                    "qos_allocated_bw": 50,
                    "qos_limit_bw": 75,
                    "health_info": {
                        "health_status": 1,
                        "media_status": 0
                    }
                },
                {
                    "ld_id": 3,
                    "memory_size_mb": 16384,
                    "qos_allocated_bw": 25,
                    "qos_limit_bw": 50,
                    "health_info": {
                        "health_status": 0,
                        "media_status": 2
                    }
                }
            ]
        }
    ]
}
```

This example demonstrates:
- **Host-bindable LDs**: All LDs (LD-0 through LD-3) can be bound to hosts via vPPB
- **Asymmetric memory allocation**: LDs have different sizes (64GB, 32GB, 16GB, 16GB)
- **QoS tiering**: Different bandwidth allocations per LD
- **Per-LD health**: LD2 has Warning health, LD3 has Degraded state

## Redfish Mapping Reference

### CXL Health Status → Redfish Health

| CCI health_status | Redfish Health |
|-------------------|----------------|
| 0 (no flags) | OK |
| Bit 0 or 1 set | Warning |
| Bit 2 set | Critical |

### CXL Media Status → Redfish State

| CCI media_status | Redfish State |
|------------------|---------------|
| 0 (Normal) | Enabled |
| 1 (Not Ready) | Starting |
| 2 (Write Persistency Lost) | Degraded |
| 3 (All Data Lost) | UnavailableOffline |
| 4 (Prior Loss Event) | Degraded |

### Device Type → Semantics Supported

| Condition | CXL Semantics |
|-----------|---------------|
| Type 3 Device | CXLio (always) |
| total_capacity > 0 | CXLmem |

## Validation

The mock loader performs the following validation:

1. **Required fields**: Each device must have an `id` field
2. **Unique IDs**: Device and switch IDs must be unique within the topology
3. **Capacity consistency**: `total_capacity_mb` should equal `volatile_capacity_mb + persistent_capacity_mb`
4. **Parent references**: `parent_switch` must reference a defined switch ID

Validation errors are logged and may cause the topology to fail loading.

## Future Extensions

The following features are planned for future releases:

- **Dynamic Capacity events**: Simulated host acceptance of pending extents
- **Event Injection**: Simulate device events and errors
- **Port-Based Routing (PBR)**: Full PBR fabric topologies (vPPB binding status exists)

## References

- CXL Specification
- DMTF Redfish CXLLogicalDevice Schema
- DMTF Redfish VirtualCXLSwitch Schema
- DMTF Redfish Resource.State and Resource.Health Enums
