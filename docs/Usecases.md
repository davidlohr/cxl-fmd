# CXL Fabric Manager Daemon: Use Cases and Integration Opportunities

This document outlines potential users, deployment scenarios, and integration opportunities for cxl-fmd as middleware between CXL hardware and orchestration systems.

**What is implemented and what is not.** The Redfish requests in "Memory
Lifecycle and Provisioning" and in Scenarios 1, 2 and 6 are ones cxl-fmd
serves, with resource IDs named as cxl-fmd names them (device and host
names are placeholders, except where an example says which mock topology
it comes from); `docs/Redfish-API.md` is the reference. The "cxl-fmd Value"
lists under "Target Users" are aims, not features: no scheduler,
orchestrator or DCIM integration ships with cxl-fmd. Everything else is a
design sketch, marked **Not implemented** where it appears: the QoS writes of
Scenario 3, the events and migration of Scenario 4, the tenancy features of
Scenario 5, all of "Orchestration Integration" and "Deployment
Architectures", and the parts of "Middleware Value Proposition" marked so. None of the Kubernetes,
OpenStack, vSphere, container runtime or Prometheus components named there
exists in this repository. cxl-fmd itself has no EventService, no Prometheus
metrics endpoint (it serves Redfish `PortMetrics` and `MemoryMetrics`), no authentication or audit log, no `PATCH` of a `Connection` (405) or a
`CXLLogicalDevice`, and no `$filter` query (a query string is ignored). The
known gaps of what it does implement are listed under "Known Gaps" in
`docs/Redfish-API.md`.

## Table of Contents

1. [Target Users](#target-users)
2. [Memory Lifecycle and Provisioning](#memory-lifecycle-and-provisioning)
3. [CXL Fabric Management Scenarios](#cxl-fabric-management-scenarios)
4. [Orchestration Integration](#orchestration-integration)
5. [Middleware Value Proposition](#middleware-value-proposition)
6. [Deployment Architectures](#deployment-architectures)

---

## Target Users

### Cloud Service Providers (Hyperscalers)

**Profile**: Large-scale data center operators (AWS, Azure, GCP, OCI, Alibaba Cloud)

**Pain Points**:
- Memory stranding: VMs sized to largest memory requirement waste capacity
- DRAM cost: Memory accounts for 40-60% of server TCO
- Capacity planning: Lead time for memory upgrades is 6-12 months

**cxl-fmd Value**:
- Dynamic memory pooling across compute nodes
- Memory-as-a-Service with per-tenant isolation
- Redfish API integration with existing BMC infrastructure
- Real-time capacity visibility and health monitoring

### Enterprise Data Centers

**Profile**: Fortune 500 companies, financial institutions, healthcare organizations

**Pain Points**:
- Workload variability: Peak vs. steady-state memory utilization gaps
- Legacy application support: Applications with large memory footprints
- Compliance: Audit trails for resource allocation decisions

**cxl-fmd Value**:
- Memory expansion without server replacement
- Centralized fabric management (no audit logging: not implemented)
- Integration with existing DCIM/ITSM platforms via Redfish

### High-Performance Computing (HPC) Facilities

**Profile**: National labs, research institutions, universities

**Pain Points**:
- Memory-bound workloads: Scientific applications limited by local DRAM
- Job scheduling complexity: Memory requirements vary widely
- Specialized hardware: Custom memory configurations per application

**cxl-fmd Value**:
- Fabric-attached memory pools for burst workloads
- Integration with HPC schedulers (Slurm, PBS, LSF)
- Support for memory tiering (HBM → DDR → CXL)

### AI/ML Infrastructure Operators

**Profile**: AI training clusters, inference serving platforms

**Pain Points**:
- Model size growth: LLMs exceed single-node memory capacity
- GPU memory limitations: Model sharding across devices
- Training efficiency: Memory bandwidth bottlenecks

**cxl-fmd Value**:
- CXL-attached memory expansion for large model training
- Memory pooling for inference serving elasticity
- Integration with ML orchestrators (Ray, Kubeflow)

### Storage and Memory Vendors

**Profile**: Samsung, SK Hynix, Micron, Intel, AMD

**Pain Points**:
- Product differentiation: Need management software for CXL products
- Customer enablement: Reduce barrier to CXL adoption
- Ecosystem development: Attract ISV and CSP partnerships

**cxl-fmd Value**:
- Reference implementation for CXL device management
- Validation platform for CXL compliance testing
- Foundation for vendor-specific management extensions

### Server OEMs and ODMs

**Profile**: Dell, HPE, Lenovo, Supermicro, Quanta, Wistron

**Pain Points**:
- Platform management: Integrate CXL into existing BMC infrastructure
- Customer support: Provide unified management across product lines
- Competitive differentiation: CXL-ready platforms

**cxl-fmd Value**:
- BMC-agnostic fabric management via Redfish
- Platform validation and interoperability testing
- White-label management solution opportunity

---

## Memory Lifecycle and Provisioning

cxl-fmd provides a memory lifecycle API via Redfish: MemoryChunks are created and deleted, and Connections bind them to hosts.

### Memory Resource Model

```
Device (SLD/MLD/MHD)
└── MemoryDomain
    ├── MemorySizeMiB: capacity of the device
    ├── MemoryChunkIncrementMiB: granularity of a chunk
    │   (the free capacity is kept by cxl-fmd, not served: MemoryDomain
    │    has no property for it; a POST larger than it is refused with 409)
    │
    ├── MemoryChunk (allocation 1)
    │   ├── MemoryChunkSizeMiB
    │   ├── AddressRangeType: "PMEM" or "Volatile"
    │   └── Links.CXLLogicalDevices: the LD (MLD, MHD)
    │
    └── MemoryChunk (allocation 2)
        └── ...
```

### Chunk Management Operations

**Create a Memory Chunk:**
```bash
POST /redfish/v1/Chassis/{device}/MemoryDomains/{dom}/MemoryChunks
Content-Type: application/json

{
    "MemoryChunkSizeMiB": 8192,
    "AddressRangeType": "PMEM",
    "InterleaveSets": [{
        "Memory": {
            "@odata.id": "/redfish/v1/Chassis/{device}/Memory/{device}_ld0"
        },
        "SizeMiB": 8192
    }]
}
```

Response (201 Created; a chunk made with `POST` is named
`{domain}_chunk{N}`, N from a counter of the domain that starts at 0 and
is not reused while cxl-fmd runs):
```json
{
    "@odata.id": "/redfish/v1/Chassis/{device}/MemoryDomains/{dom}/MemoryChunks/{dom}_chunk0",
    "Id": "{dom}_chunk0",
    "MemoryChunkSizeMiB": 8192,
    "AddressRangeType": "PMEM",
    ...
}
```

`AddressRangeType` defaults to `"PMEM"` (a known gap). For an MLD the
chunk grows the LD with Set LD Allocations; see "Memory Provisioning" in
`docs/Redfish-API.md` for what each device type sends.

**Delete a Memory Chunk:**
```bash
DELETE /redfish/v1/Chassis/{device}/MemoryDomains/{dom}/MemoryChunks/{chunk}
```
Returns 204 No Content on success.

### Connection Management Operations

A Connection is a host's binding to an SLD or an LD: for switch-attached devices, a vPPB of the host's VCS bound to the device's port or LD, whoever made it. cxl-fmd binds and unbinds vPPBs when Connections are created and deleted (see "Memory Binding (Connections)" in `docs/Redfish-API.md`).

**Create a Connection:**
```bash
POST /redfish/v1/Fabrics/CXL/Connections
Content-Type: application/json

{
    "Links": {
        "InitiatorEndpoints": [{
            "@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/{host}"
        }],
        "TargetEndpoints": [{
            "@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/{sld or ld}"
        }]
    },
    "MemoryChunkInfo": [{
        "MemoryChunk": {
            "@odata.id": "/redfish/v1/Chassis/{device}/MemoryDomains/{dom}/MemoryChunks/{chunk}"
        },
        "AccessCapabilities": ["Read", "Write"]
    }]
}
```

The target is an SLD's Endpoint (`{device}`) or an LD's (`{device}_ld{N}`);
`MemoryChunkInfo` can be left out, as the whole SLD or LD is bound. The
answer is `201 Created`, `202 Accepted` with a `Task` when the switch binds
in the background (see "Binds and unbinds in the background" in
`docs/Redfish-API.md`), or `200 OK` with the Connection the host has
already.

**Delete a Connection:**
```bash
DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}
```
For switch-attached devices, this unbinds the vPPB, whoever bound it, with
Simulate Managed Hot-Remove. A direct-attached Connection cannot be deleted
(405). `POST .../Connections/{conn}/Actions/Oem/Cxl.ForceUnbind` forces the
unbind (Simulate Surprise Hot-Remove) for a host that does not release the
device.

### MHD Access Control

For Multi-Headed Devices (MHD), cxl-fmd enforces cross-head access validation:

- Each host attaches to exactly one head
- Hosts can only access pool LDs assigned to their head
- Connection creation validates this mapping automatically

```
Host A (Head 0) ─── Pool LDs 0,1 ─┐
                                  │ MHD
Host B (Head 1) ─── Pool LDs 2,3 ─┘

# Valid: Host A accessing LD 0
POST /Connections → OK

# Invalid: Host A accessing LD 2 (assigned to Head 1)
POST /Connections → 400 Bad Request
```

### Complete Provisioning Workflow

```
1. Discovery
   GET /redfish/v1/Chassis → List devices
   GET /redfish/v1/Systems → List hosts

2. Check Capacity
   GET /Chassis/{dev}/MemoryDomains/{dom}
   → "MemorySizeMiB", and the chunks already in it
     (the free capacity is not served)

3. Create Memory Chunk
   POST /Chassis/{dev}/MemoryDomains/{dom}/MemoryChunks
   → Returns new chunk URI

4. Create Connection (bind to host)
   POST /Fabrics/CXL/Connections
   → 201, or 202 and a Task while the switch binds (200 if the host
     has it already)

5. Use Memory
   (outside cxl-fmd: the host's kernel and applications)

6. Release Memory
   DELETE /Fabrics/CXL/Connections/{conn}
   DELETE /Chassis/{dev}/MemoryDomains/{dom}/MemoryChunks/{chunk}
```

---

## CXL Fabric Management Scenarios

### Scenario 1: Memory Pooling and Disaggregation

**Description**: Multiple compute nodes share a pool of CXL-attached memory devices through a CXL switch fabric. cxl-fmd provides complete visibility and control over the pooled memory.

```
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Compute 1  │  │  Compute 2  │  │  Compute 3  │
│   (Host)    │  │   (Host)    │  │   (Host)    │
│   VCS 0     │  │   VCS 1     │  │   VCS 2     │
└──────┬──────┘  └──────┬──────┘  └──────┬──────┘
       │ USP0           │ USP1           │ USP2
       └────────┬───────┴────────┬───────┘
                │                │
         ┌──────┴────────────────┴──────┐
         │         CXL Switch           │
         │  VCS0: vPPB→DSP1/LD0         │
         │  VCS1: vPPB→DSP1/LD1,DSP2    │
         │  VCS2: vPPB→DSP3             │
         └──────┬────────┬───────┬──────┘
                │ DSP1   │ DSP2  │ DSP3
         ┌──────┴──────┐ │  ┌────┴────┐
         │    MLD 1    │ │  │  SLD 1  │
         │  ┌────┬────┐│ │  │  512GB  │
         │  │LD0 │LD1 ││ │  └─────────┘
         │  │256G│256G││ │
         │  └────┴────┘│ │
         └─────────────┘ │
                  ┌──────┴──────┐
                  │    MLD 2    │
                  │    1TB      │
                  └─────────────┘
```

#### VCS/Zone Model for Host Isolation

cxl-fmd exposes each VCS (Virtual CXL Switch) as a Redfish **Zone**. Zones define which devices are accessible from each host:

The examples below are what cxl-fmd serves for `mock/multi-host-vcs.json`
(two VCSs, three SLDs and an MLD with four LDs), not for the figure above.

```bash
# List all zones (VCS partitions)
GET /redfish/v1/Fabrics/CXL/Zones
{
    "Members": [
        {"@odata.id": "/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0"},
        {"@odata.id": "/redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs1"}
    ]
}

# Get zone details - shows bound endpoints
GET /redfish/v1/Fabrics/CXL/Zones/cxl_switch_0_vcs0
{
    "Id": "cxl_switch_0_vcs0",
    "ZoneType": "ZoneOfEndpoints",
    "Links": {
        "Endpoints": [
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/cxl_switch_0_host0"},
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/dev0"},
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/dev1"},
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld0_ld0"},
            {"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld0_ld1"}
        ]
    }
}
```

#### vPPB Binding for Dynamic Pool Management

Each Zone has vPPBs (virtual PCI-to-PCI Bridges) that bind devices/LDs to hosts:

```bash
# View vPPBs of VCS 0
GET /redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs
{
    "Members": [
        {"@odata.id": ".../VPPBs/0"},
        {"@odata.id": ".../VPPBs/1"},
        {"@odata.id": ".../VPPBs/2"},
        {"@odata.id": ".../VPPBs/3"},
        {"@odata.id": ".../VPPBs/4"}
    ]
}

# Get specific vPPB - shows binding details
GET /redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/VCSs/0/VPPBs/3
{
    "Id": "3",
    "VPPBId": "3",
    "BindingStatus": "BoundLD",
    "BoundPortId": 5,
    "BoundLDId": 0,
    "Links": {
        "Port": {"@odata.id": "/redfish/v1/Fabrics/CXL/Switches/cxl_switch_0/Ports/cxl_switch_0_dsp5"}
    }
}
```

#### Pool Capacity Visibility

cxl-fmd serves the capacity of each device and of what is allocated from it.
The free capacity of a domain is not served (MemoryDomain_v1 has no property
for it); a client works it out from the chunks:

```bash
# All devices, as Chassis (switches too)
GET /redfish/v1/Chassis

# Per-device capacity
GET /redfish/v1/Chassis/mld0/MemoryDomains/mld0_domain0
{
    "Id": "mld0_domain0",
    "MemorySizeMiB": 131072,
    "MemoryChunkIncrementMiB": 256,
    "MemoryChunks": {"@odata.id": ".../MemoryChunks"}
}

# Per-LD capacity and QoS (read only)
GET /redfish/v1/Chassis/mld0/PCIeDevices/mld0/CXLLogicalDevices/0
{
    "MemorySizeMiB": 32768,
    "QoS": {
        "AllocatedBandwidth": 100,
        "LimitPercent": 100
    }
}
```

#### Dynamic Pool Rebalancing Workflow

When workload demands change, orchestrators can dynamically rebalance memory across hosts:

```bash
# Step 1: Release memory from underutilized Host 1
DELETE /redfish/v1/Fabrics/CXL/Connections/{conn}
→ the vPPB is unbound (204, or 202 and a Task)

DELETE /redfish/v1/Chassis/mld_2/MemoryDomains/.../MemoryChunks/{chunk}
→ Capacity returned to pool (409 until the unbind is over)

# Step 2: Verify the chunk is gone
GET /redfish/v1/Chassis/mld_2/MemoryDomains/mld_2_domain0/MemoryChunks

# Step 3: Allocate to memory-hungry Host 2
POST /redfish/v1/Chassis/mld_2/MemoryDomains/.../MemoryChunks
{
    "MemoryChunkSizeMiB": 524288,
    "AddressRangeType": "PMEM"
}

POST /redfish/v1/Fabrics/CXL/Connections
{
    "Links": {
        "InitiatorEndpoints": [{"@odata.id": ".../Endpoints/host2"}],
        "TargetEndpoints": [{"@odata.id": ".../Endpoints/mld_2_ld0"}]
    },
    "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": ".../chunk_new"}}]
}
→ vPPB automatically bound to Host 2's VCS
```

#### MLD LD-Level Pooling

For MLDs, cxl-fmd enables per-LD allocation, allowing multiple hosts to share a single physical device:

```
MLD with 4 LDs (256GB each = 1TB total):
┌────────────────────────────────────┐
│              MLD                   │
│  ┌──────┐┌──────┐┌──────┐┌──────┐  │
│  │ LD0  ││ LD1  ││ LD2  ││ LD3  │  │
│  │256GB ││256GB ││256GB ││256GB │  │
│  │Host1 ││Host1 ││Host2 ││(free)│  │
│  └──────┘└──────┘└──────┘└──────┘  │
└────────────────────────────────────┘
```

```bash
# Host 1 has LD0 and LD1 (512GB): list the Connections and look at their
# Links.InitiatorEndpoints ($filter is not supported: it is ignored)
GET /redfish/v1/Fabrics/CXL/Connections

# Host 2 has LD2 (256GB)
# LD3 remains in pool for future allocation

# Allocate LD3 to Host 3
POST /Chassis/mld_0/MemoryDomains/.../MemoryChunks
{
    "MemoryChunkSizeMiB": 262144,
    "InterleaveSets": [{
        "Memory": {"@odata.id": "/redfish/v1/Chassis/mld_0/Memory/mld_0_ld3"}
    }]
}
POST /Fabrics/CXL/Connections  # Bind LD 3 (Endpoints/mld_0_ld3) to host3
```

**cxl-fmd Role**:
- Discover and inventory all memory devices, switches, and VCS configurations
- Expose VCS partitions as Zones for host isolation visibility
- Serve every vPPB binding of a host's VCS as a Connection, whoever made it, and bind and unbind vPPBs as Connections are created and deleted
- Serve the capacity of each device and domain, and its chunks
- Support per-LD allocation for fine-grained MLD pooling
- Serve health (Get Health Info, read when a device is discovered) and
  switch port metrics; performance is not monitored

### Scenario 2: Dynamic Memory Provisioning

**Description**: Orchestration system requests memory capacity changes at runtime based on workload demands.

**Workflow**:
```
1. Monitoring detects memory pressure on Host A
   → Prometheus alert: cxl_memory_pressure{host="hostA"} > 0.9

2. Orchestrator queries cxl-fmd for capacity
   GET /redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0
   → "MemorySizeMiB", and the chunks already in it

3. Create memory chunk from available capacity
   POST /redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks
   {
       "MemoryChunkSizeMiB": 16384,
       "InterleaveSets": [{
           "Memory": {"@odata.id": "/redfish/v1/Chassis/mld_0/Memory/mld_0_ld1"},
           "SizeMiB": 16384
       }]
   }

4. Bind memory to Host A via Connection
   POST /redfish/v1/Fabrics/CXL/Connections
   {
       "Links": {
           "InitiatorEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/hostA"}],
           "TargetEndpoints": [{"@odata.id": "/redfish/v1/Fabrics/CXL/Endpoints/mld_0_ld1"}]
       },
       "MemoryChunkInfo": [{
           "MemoryChunk": {"@odata.id": "/redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks/{chunk}"}
       }]
   }
   → cxl-fmd performs vPPB binding for switch-attached devices
     (201, or 202 and a Task while the switch binds)

5. Host A kernel detects new memory via hotplug
   → Linux CXL driver adds new NUMA node

6. Workload continues with expanded memory

7. (Later) Release memory when no longer needed
   DELETE /redfish/v1/Fabrics/CXL/Connections/{conn_id}
   DELETE /redfish/v1/Chassis/mld_0/MemoryDomains/mld_0_domain0/MemoryChunks/{chunk}
```

Steps 1, 5 and 6 are outside cxl-fmd.

**cxl-fmd Role**:
- Serve each domain's MemorySizeMiB and chunks (the free capacity is not served)
- Create/delete MemoryChunks for allocation management
- Create/delete Connections to bind memory to hosts
- For switch-attached: automatic vPPB bind/unbind via FM-API
- For MHD: enforce head-to-host access validation
- Emit events for allocation changes: **not implemented** (there is no EventService)

**Latency targets** (goals, not measured):
- Discovery: < 5 seconds
- Chunk creation: < 100ms
- Connection creation (includes vPPB bind): < 1 second
- Memory online: < 5 seconds (kernel dependent)

### Scenario 3: Memory Tiering with QoS

**Description**: Workloads access memory tiers with different performance characteristics, managed by cxl-fmd QoS policies.

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

cxl-fmd serves each LD's QoS (`QoS.AllocatedBandwidth`, `QoS.LimitPercent`)
read only: Set QoS Allocated BW and Set QoS BW Limit are never sent, Set QoS
Control is marked "No" in Table 2 of `docs/Redfish-API.md`, and no latency
or bandwidth of a tier is served.

**Memory Hierarchy**:
```
Tier 0: Local DDR5 (fastest, limited capacity)
Tier 1: CXL-attached DDR5 (medium latency, expandable)
Tier 2: CXL-attached CXL Memory (higher latency, large capacity)
Tier 3: CXL-attached Persistent Memory (highest latency, persistent)
```

**cxl-fmd Role**:
- Expose tier characteristics via Redfish (latency, bandwidth)
- Configure QoS bandwidth allocation per LD
- Monitor tier utilization for placement decisions
- Support memory migration hints to kernel

**QoS Configuration** (not implemented: this `PATCH` is answered 501; the
schema's properties are `AllocatedBandwidth` and `LimitPercent`):
```
PATCH /redfish/v1/Chassis/{dev}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}
{
  "QoS": {
    "AllocatedBandwidth": 25,  // percentage
    "LimitPercent": 50         // percentage
  }
}
```

### Scenario 4: Failure Isolation and RAS

**Description**: Detect and isolate failing memory devices to prevent data corruption and system instability.

**Failure Modes**:
- Correctable ECC errors (threshold exceeded)
- Uncorrectable ECC errors
- Media failures (poison)
- Device communication failures
- Switch port failures

**cxl-fmd Role** (what is done today):
- Read Get Health Info (4200h) when a device is discovered, and serve it
  as `Status` and `MemoryMetrics`; it is not polled
- Read and clear the event logs of switches and memory devices, and serve
  the records as `LogEntry` resources

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

- Monitor poison lists via Get Poison List (4300h): not sent
- Generate Redfish events for threshold violations: there is no EventService
- Support graceful LD migration before failure
- Coordinate with kernel for memory offlining

**Event Flow** (not implemented):
```
1. cxl-fmd detects health threshold crossed
2. POST /redfish/v1/EventService/Subscriptions/{id}
   { "MessageId": "CXL.1.0.MemoryHealthWarning", ... }
3. Orchestrator receives event, initiates migration
4. cxl-fmd unbinds LD from affected device
5. cxl-fmd binds replacement LD from healthy device
```

### Scenario 5: Multi-Tenancy with Memory Isolation

**Description**: Multiple tenants share CXL fabric with guaranteed isolation and resource allocation.

**Requirements**:
- Tenant A cannot access Tenant B's memory
- Each tenant has guaranteed minimum capacity
- Burst capacity available when others are idle
- Per-tenant billing based on allocation

**cxl-fmd Role**:
- Enforce access control via vPPB binding and MHD head mapping (implemented)

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

- Map tenants to LD allocations
- Track per-tenant utilization for billing
- Support overcommit policies with fair sharing

**Multi-Tenancy Model**:
```
Tenant A: LD 1-4 on MLD 1 (guaranteed 2TB)
Tenant B: LD 5-8 on MLD 2 (guaranteed 2TB)
Tenant C: LD 9-12 on MLD 3 (guaranteed 2TB)
Shared Pool: LD 13-20 on MLD 4-5 (burst capacity)
```

### Scenario 6: Multi-Headed Device Sharing

**Description**: Multiple hosts share a single MHD device, with each host accessing only its assigned memory partition via dedicated heads.

**Physical Configuration**:
```
┌─────────┐                      ┌─────────────────────────┐
│ Host A  │◄──── Head 0 ────────►│                         │
│ (VCS 0) │      (LD 0,1)        │      MHD Device         │
└─────────┘                      │   ┌────────────────┐    │
                                 │   │   LD Pool      │    │
┌─────────┐                      │   │ LD0 LD1 LD2 LD3│    │
│ Host B  │◄──── Head 1 ────────►│   └────────────────┘    │
│ (VCS 1) │      (LD 2,3)        │                         │
└─────────┘                      └─────────────────────────┘
```

**cxl-fmd Enforced Access Control**:
```bash
# Host A provisions memory from its assigned LDs
POST /Chassis/mhd_0/MemoryDomains/mhd_0_domain0/MemoryChunks
{
    "MemoryChunkSizeMiB": 4096,
    "InterleaveSets": [{
        "Memory": {"@odata.id": "/redfish/v1/Chassis/mhd_0/Memory/mhd_0_ld0"}
    }]
}

# Host A creates connection - ALLOWED (LD0 assigned to Head 0)
POST /Fabrics/CXL/Connections
{
    "Links": {
        "InitiatorEndpoints": [{"@odata.id": "/Fabrics/CXL/Endpoints/hostA"}],
        "TargetEndpoints": [{"@odata.id": "/Fabrics/CXL/Endpoints/mhd_0"}]
    },
    "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": ".../MemoryChunks/chunk_0"}}]
}
→ 200 OK: a directly attached head has its Connection already
  ("mhd_0" with a chunk of one LD is accepted for compatibility; the
  Endpoint of the LD is "mhd_0_ld0")

# Host A attempts to access Host B's memory - DENIED
POST /Fabrics/CXL/Connections
{
    "Links": {
        "InitiatorEndpoints": [{"@odata.id": "/Fabrics/CXL/Endpoints/hostA"}],
        "TargetEndpoints": [{"@odata.id": "/Fabrics/CXL/Endpoints/mhd_0"}]
    },
    "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": ".../MemoryChunks/chunk_from_ld2"}}]
}
→ 400 Bad Request: "The host is not attached to that device or head: it cannot reach this memory"
```

**cxl-fmd Role**:
- Take the head-to-host mapping from the mock topology (`attached_host`):
  no CXL command maps a head to a host or a switch port, so on hardware a
  Multi-Headed device has no Connections (a known gap)
- Give each directly attached head a Connection to the pool LDs mapped to
  it, and refuse a Connection to any other (400)
- Support both direct-attached and switch-attached MHD configurations; a
  head on a switch port that presents an SLD is bound whole, with LD ID
  FFFFh, and a head that presents an MLD cannot be bound per LD (400, a
  known gap)

---

## Orchestration Integration

### Kubernetes Integration

This section describes how cxl-fmd integrates with Kubernetes for cluster-wide CXL memory management, following the OCP (Open Compute Project) Memory Fabric Manager (MFM) architecture using Dynamic Resource Allocation (DRA).

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

None of the MFM Controller, MFM Node Agent, exporters or controllers in
this section exist. The sketches read `CapacityMiB` and
`RemainingCapacityMiB` from a MemoryDomain, which has neither: cxl-fmd serves
`MemorySizeMiB` and the domain's chunks. They read health from
`/Memory/LD0`; cxl-fmd names a Memory resource `{device}_ld{N}` and has no
`Oem` health properties. cxl-fmd serves no QoS writes.

#### OCP MFM Architecture Overview

Kubernetes treats memory as a static, node-local resource, which limits its ability to manage shared or disaggregated memory. The OCP MFM architecture addresses this by introducing three components that work together to provide dynamic, fabric-aware memory allocation:

```
┌─────────────────────────────────────────────────────────────────────┐
│                       K8s Control Plane                              │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                      MFM Controller                            │  │
│  │  - Cluster-level DRA controller                                │  │
│  │  - Allocates memory slices from CXL pool                       │  │
│  │  - Exposes topology hints (AvailableOnNodes) to scheduler      │  │
│  │  - Implements Allocate/Deallocate interfaces                   │  │
│  └───────────────────────────┬───────────────────────────────────┘  │
└──────────────────────────────┼──────────────────────────────────────┘
                               │ Redfish API
┌──────────────────────────────┼──────────────────────────────────────┐
│  ┌───────────────────────────┴───────────────────────────────────┐  │
│  │              MFM (Memory Fabric Manager) = cxl-fmd             │  │
│  │  - Topology, zone, and capacity information                    │  │
│  │  - MemoryChunk allocation/deallocation                         │  │
│  │  - Connection binding (host-to-device)                         │  │
│  │  - Health monitoring and QoS configuration                     │  │
│  │  - Transport-agnostic (MCTP/ioctl)                             │  │
│  └───────────────────────────┬───────────────────────────────────┘  │
└──────────────────────────────┼──────────────────────────────────────┘
                               │ CCI Commands
┌──────────────────────────────┼──────────────────────────────────────┐
│  K8s Worker Nodes            │                                       │
│  ┌───────────────────────────┴───────────────────────────────────┐  │
│  │                     MFM Node Agent                             │  │
│  │  - DaemonSet running on each CXL-capable node                  │  │
│  │  - NodePrepareResources: map memory into pod namespace         │  │
│  │  - NodeUnprepareResources: unmap and release                   │  │
│  │  - Exposes memory via DAX devices or mem slabs                 │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  CXL Devices (SLD, MLD, MHD) - Direct or Switch-Attached    │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

**cxl-fmd's Role**: cxl-fmd serves as the **MFM** component - the external service maintaining topology, zone, and capacity information for all memory pools and connected nodes. It provides the Redfish API that the MFM Controller and MFM Node Agent consume.

#### DRA Resource Model

DRA (Dynamic Resource Allocation) is the recommended Kubernetes mechanism for CXL memory integration. It provides:
- Structured parameters for fine-grained allocation requests
- Deferred allocation until pod scheduling
- Topology-aware scheduling via `AvailableOnNodes` hints
- Automated cleanup on pod termination

**Key DRA Resources**:

| Resource | Purpose | Created By |
|----------|---------|------------|
| `DeviceClass` | Defines CXL memory device type for scheduling | Admin (once) |
| `ResourceSlice` | Publishes available CXL capacity to K8s | MFM Node Agent |
| `ResourceClaimTemplate` | Template for user memory requests | User/Admin |
| `ResourceClaim` | Actual allocation request | User or from template |

#### DeviceClass Definition

Admin defines a DeviceClass during cluster setup to make CXL memory schedulable:

```yaml
apiVersion: resource.k8s.io/v1
kind: DeviceClass
metadata:
  name: mfm-pooled-memory
spec:
  selectors:
    - cel:
        expression: device.driver == "memory.cxl.io"
```

For MH-MLD (Multi-Headed Multi-Logical Device) configurations:

```yaml
apiVersion: resource.k8s.io/v1
kind: DeviceClass
metadata:
  name: mfm-mhmld-memory
spec:
  selectors:
    - cel:
        expression: 'device.driver == "memory.cxl.io" && device.attributes["type"].string == "mfm-MHMLD-memory"'
```

#### ResourceSlice (Published by MFM Node Agent)

The MFM Node Agent queries cxl-fmd and publishes ResourceSlices to advertise CXL capacity:

```yaml
apiVersion: resource.k8s.io/v1
kind: ResourceSlice
metadata:
  name: fabric0
spec:
  driver: memory.cxl.io
  nodeName: worker-node-1
  devices:
    - name: pooled-memory
      attributes:
        type:
          string: mfm-MHMLD-memory
      capacity:
        memory:
          value: 64Gi
        size:
          value: 64Gi
          requestPolicy:
            default: 2Gi
            validRange:
              min: 2Gi
              step: 2Gi
    - name: shared-memory
      attributes:
        type:
          string: mfm-MHMLD-memory
      capacity:
        memory:
          value: 64Gi
```

**MFM Node Agent ResourceSlice Generation** (queries cxl-fmd):
```go
func (a *MFMNodeAgent) PublishResourceSlices() error {
    // Query cxl-fmd for available memory domains
    resp, _ := a.fmd.Get("/redfish/v1/Chassis")

    var devices []resourcev1.Device
    for _, chassis := range resp.Members {
        domains, _ := a.fmd.Get(chassis.OdataID + "/MemoryDomains")
        for _, domain := range domains.Members {
            domainData, _ := a.fmd.Get(domain.OdataID)

            devices = append(devices, resourcev1.Device{
                Name: domainData.Id,
                Attributes: map[string]resourcev1.DeviceAttribute{
                    "type":     {StringValue: ptr("mfm-pooled-memory")},
                    "device":   {StringValue: ptr(chassis.Id)},
                    "domain":   {StringValue: ptr(domainData.Id)},
                },
                Capacity: map[string]resource.Quantity{
                    "memory": resource.MustParse(fmt.Sprintf("%dMi", domainData.RemainingCapacityMiB)),
                },
            })
        }
    }

    return a.k8s.CreateResourceSlice(a.nodeName, devices)
}
```

#### ResourceClaimTemplate (User Request)

Users request CXL memory via ResourceClaimTemplate:

```yaml
apiVersion: resource.k8s.io/v1
kind: ResourceClaimTemplate
metadata:
  name: cxl-memory-16gi
spec:
  spec:
    devices:
      requests:
        - name: req-0
          exactly:
            deviceClassName: mfm-pooled-memory
          capacity:
            requests:
              size: 16Gi
```

**Pod Using DRA**:
```yaml
apiVersion: v1
kind: Pod
metadata:
  name: ml-training
spec:
  containers:
    - name: trainer
      image: ml-trainer:latest
      resources:
        claims:
          - name: cxl-mem
  resourceClaims:
    - name: cxl-mem
      resourceClaimTemplateName: cxl-memory-16gi
```

#### MFM Controller Allocation Workflow

When a ResourceClaim is submitted, the MFM Controller handles allocation by calling cxl-fmd:

```go
// MFM Controller implements k8s.io/kubelet/pkg/apis/dra/v1 interfaces
type MFMController struct {
    fmd *RedfishClient  // cxl-fmd connection
}

func (c *MFMController) Allocate(ctx context.Context, claim *resourcev1.ResourceClaim) (*resourcev1.AllocationResult, error) {
    params := getClaimParameters(claim)

    // Step 1: Find device with sufficient capacity
    device, domain := c.findAvailableCapacity(params.SizeMiB)
    if device == "" {
        return nil, fmt.Errorf("insufficient CXL capacity for %d MiB", params.SizeMiB)
    }

    // Step 2: Create MemoryChunk via cxl-fmd
    chunk, err := c.fmd.Post(fmt.Sprintf("/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks", device, domain), map[string]interface{}{
        "MemoryChunkSizeMiB": params.SizeMiB,
        "AddressRangeType":   "PMEM",
    })
    if err != nil {
        return nil, fmt.Errorf("failed to create chunk: %w", err)
    }

    // Step 3: Identify which nodes can access this memory
    availableNodes := c.getNodesWithAccessToDevice(device)

    // Step 4: Return allocation result with topology hints
    return &resourcev1.AllocationResult{
        Devices: resourcev1.DeviceAllocationResult{
            Results: []resourcev1.DeviceRequestAllocationResult{{
                Request: "req-0",
                Device:  domain,
            }},
        },
        // Topology hints guide scheduler to nodes that can access this memory
        AvailableOnNodes: &v1.NodeSelector{
            NodeSelectorTerms: []v1.NodeSelectorTerm{{
                MatchExpressions: []v1.NodeSelectorRequirement{{
                    Key:      "cxl.io/fabric-access",
                    Operator: v1.NodeSelectorOpIn,
                    Values:   availableNodes,
                }},
            }},
        },
        // Store allocation metadata for NodePrepareResources
        ResourceHandles: []resourcev1.ResourceHandle{{
            Data: fmt.Sprintf(`{"device":"%s","domain":"%s","chunk":"%s"}`, device, domain, chunk.Id),
        }},
    }, nil
}

func (c *MFMController) Deallocate(ctx context.Context, claim *resourcev1.ResourceClaim) error {
    handle := claim.Status.Allocation.ResourceHandles[0]
    var alloc struct {
        Device string `json:"device"`
        Domain string `json:"domain"`
        Chunk  string `json:"chunk"`
    }
    json.Unmarshal([]byte(handle.Data), &alloc)

    // Delete MemoryChunk via cxl-fmd (Connection deleted by NodeUnprepareResources)
    return c.fmd.Delete(fmt.Sprintf("/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
        alloc.Device, alloc.Domain, alloc.Chunk))
}
```

#### MFM Node Agent: NodePrepareResources

After the scheduler places the pod, the MFM Node Agent on that node executes NodePrepareResources to map the CXL memory into the pod's namespace:

```go
func (a *MFMNodeAgent) NodePrepareResources(ctx context.Context, req *drav1.NodePrepareResourcesRequest) (*drav1.NodePrepareResourcesResponse, error) {
    var results []*drav1.NodePrepareResourceResponse

    for _, claim := range req.Claims {
        var alloc struct {
            Device string `json:"device"`
            Domain string `json:"domain"`
            Chunk  string `json:"chunk"`
        }
        json.Unmarshal([]byte(claim.ResourceHandle), &alloc)

        // Step 1: Create Connection via cxl-fmd to bind memory to this node
        conn, err := a.fmd.Post("/redfish/v1/Fabrics/CXL/Connections", map[string]interface{}{
            "Links": map[string]interface{}{
                "InitiatorEndpoints": []map[string]string{
                    {"@odata.id": fmt.Sprintf("/redfish/v1/Fabrics/CXL/Endpoints/%s", a.hostID)},
                },
                "TargetEndpoints": []map[string]string{
                    {"@odata.id": fmt.Sprintf("/redfish/v1/Fabrics/CXL/Endpoints/%s", alloc.Device)},
                },
            },
            "MemoryChunkInfo": []map[string]interface{}{{
                "MemoryChunk": map[string]string{
                    "@odata.id": fmt.Sprintf("/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
                        alloc.Device, alloc.Domain, alloc.Chunk),
                },
                "AccessCapabilities": []string{"Read", "Write"},
            }},
        })
        if err != nil {
            return nil, fmt.Errorf("failed to create connection: %w", err)
        }

        // Step 2: Wait for kernel to detect memory via hotplug
        daxDevice := a.waitForDAXDevice(alloc.Device, alloc.Chunk)

        // Step 3: Return CDI device spec for container runtime
        results = append(results, &drav1.NodePrepareResourceResponse{
            CDIDevices: []string{fmt.Sprintf("cxl.io/memory=%s", daxDevice)},
        })

        // Store connection ID for cleanup
        a.storeConnectionMapping(claim.Uid, conn.Id)
    }

    return &drav1.NodePrepareResourcesResponse{Claims: results}, nil
}

func (a *MFMNodeAgent) NodeUnprepareResources(ctx context.Context, req *drav1.NodeUnprepareResourcesRequest) (*drav1.NodeUnprepareResourcesResponse, error) {
    for _, claim := range req.Claims {
        connID := a.getConnectionMapping(claim.Uid)

        // Delete Connection via cxl-fmd (triggers vPPB unbind for switch-attached)
        if err := a.fmd.Delete(fmt.Sprintf("/redfish/v1/Fabrics/CXL/Connections/%s", connID)); err != nil {
            return nil, fmt.Errorf("failed to delete connection: %w", err)
        }

        a.removeConnectionMapping(claim.Uid)
    }

    return &drav1.NodeUnprepareResourcesResponse{}, nil
}
```

#### Complete DRA Workflow

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│    User     │    │  MFM Ctrl   │    │   cxl-fmd   │    │  Node Agent │
└──────┬──────┘    └──────┬──────┘    └──────┬──────┘    └──────┬──────┘
       │                  │                  │                  │
       │ kubectl apply    │                  │                  │
       │ (ResourceClaim)  │                  │                  │
       │─────────────────►│                  │                  │
       │                  │                  │                  │
       │                  │ GET /MemoryDomains                  │
       │                  │─────────────────►│                  │
       │                  │◄─────────────────│                  │
       │                  │                  │                  │
       │                  │ POST /MemoryChunks                  │
       │                  │─────────────────►│                  │
       │                  │◄─────────────────│ chunk_id         │
       │                  │                  │                  │
       │                  │ AllocationResult │                  │
       │                  │ (AvailableOnNodes)                  │
       │                  │                  │                  │
       │                  │      [Scheduler places pod]         │
       │                  │                  │                  │
       │                  │                  │ NodePrepareResources
       │                  │                  │◄─────────────────│
       │                  │                  │                  │
       │                  │                  │ POST /Connections│
       │                  │                  │◄─────────────────│
       │                  │                  │─────────────────►│
       │                  │                  │                  │
       │                  │                  │ [Kernel hotplug] │
       │                  │                  │                  │
       │ Pod Running with CXL memory         │                  │
       │◄────────────────────────────────────┼──────────────────│
       │                  │                  │                  │
       │ [Pod terminates] │                  │                  │
       │                  │                  │                  │
       │                  │                  │ NodeUnprepareResources
       │                  │                  │◄─────────────────│
       │                  │                  │                  │
       │                  │                  │ DELETE /Connections
       │                  │                  │◄─────────────────│
       │                  │                  │                  │
       │                  │ Deallocate       │                  │
       │                  │─────────────────►│                  │
       │                  │ DELETE /MemoryChunks                │
       │                  │                  │                  │
```

#### Checking Allocation Status

Users can inspect allocations via kubectl:

```bash
kubectl get resourceclaim cxl-mem-abc123 -o yaml
```

```yaml
apiVersion: resource.k8s.io/v1
kind: ResourceClaim
metadata:
  name: cxl-mem-abc123
status:
  allocation:
    devices:
      results:
        - consumedCapacity:
            size: 16Gi
          device: pooled-memory
          shareID: a1b2c3d4-...
    availableOnNodes:
      nodeSelectorTerms:
        - matchExpressions:
            - key: cxl.io/fabric-access
              operator: In
              values: ["worker-node-1", "worker-node-2"]
```

#### Topology-Aware Scheduling (NUMA)

CXL memory appears as NUMA nodes to the Linux kernel. The MFM Node Agent uses Kubernetes Topology Manager and NodeResourceTopology to enable NUMA-aware scheduling.

**NodeResourceTopology CRD** (updated by MFM Node Agent):
```yaml
apiVersion: topology.node.k8s.io/v1alpha2
kind: NodeResourceTopology
metadata:
  name: worker-node-1
topologyPolicies: ["SingleNUMANodePodLevel"]
zones:
  - name: node-0           # Local DRAM
    type: Node
    resources:
      - name: memory
        capacity: 128Gi
        allocatable: 120Gi
  - name: node-1           # CXL Memory (lower tier)
    type: Node
    resources:
      - name: memory
        capacity: 512Gi
        allocatable: 500Gi
    attributes:
      - name: memory-tier
        value: cxl
      - name: cxl-device
        value: mld_0
```

**Pod with NUMA Affinity**:
```yaml
apiVersion: v1
kind: Pod
metadata:
  name: numa-aware-app
spec:
  containers:
  - name: app
    resources:
      requests:
        memory: 256Gi
      limits:
        memory: 256Gi
  # Topology Manager ensures NUMA alignment
  # Pod scheduled only if 256Gi available on single NUMA node
```

**MFM Node Agent NodeResourceTopology Update** (queries cxl-fmd):
```go
func (a *MFMNodeAgent) UpdateNodeTopology() {
    // Query cxl-fmd for CXL memory on this node
    chassis, _ := a.fmd.Get("/redfish/v1/Chassis")

    for _, dev := range chassis.Members {
        domains, _ := a.fmd.Get(dev.OdataID + "/MemoryDomains")
        for _, domain := range domains.Members {
            domainData, _ := a.fmd.Get(domain.OdataID)

            zone := topology.Zone{
                Name: fmt.Sprintf("cxl-%s", domainData.Id),
                Type: "Node",
                Resources: []topology.Resource{{
                    Name:        "memory",
                    Capacity:    resource.MustParse(fmt.Sprintf("%dMi", domainData.CapacityMiB)),
                    Allocatable: resource.MustParse(fmt.Sprintf("%dMi", domainData.RemainingCapacityMiB)),
                }},
                Attributes: []topology.Attribute{
                    {Name: "memory-tier", Value: "cxl"},
                    {Name: "cxl-device", Value: dev.Id},
                },
            }
            a.nrt.Zones = append(a.nrt.Zones, zone)
        }
    }
    a.updateNRT()
}
```

#### Health-Based Operations

A Kubernetes controller can poll cxl-fmd health endpoints and trigger actions based on device health status:

**Polling Device Health**:
```bash
# Get device health via Memory resource
GET /redfish/v1/Chassis/{dev}/Memory/{dev}_ld0
{
    "Status": {
        "Health": "Warning",    # OK, Warning, or Critical
        "State": "Enabled"
    }
}

# Get memory metrics (HealthData.PredictedMediaLifeLeftPercent, error counts)
GET /redfish/v1/Chassis/{dev}/Memory/{dev}_ld0/MemoryMetrics
```

**Health-Based Pod Eviction Controller**:
```go
func (c *CXLHealthController) PollAndEvict() {
    for _, device := range c.devices {
        health := c.fmd.GetMemoryHealth(device.ID)

        if health.Status != "Healthy" {
            // Find pods using this device
            pods := c.findPodsUsingDevice(device.ID)

            // Gracefully evict pods to allow rescheduling
            for _, pod := range pods {
                c.evictPod(pod, fmt.Sprintf("CXL device %s health: %s", device.ID, health.Status))
            }

            if health.Status == "Critical" {
                c.cordonNode(c.nodeName)
            }
        }
    }
}
```

**Capacity Monitoring for Autoscaling**:
```go
// Poll cxl-fmd for capacity metrics
func (c *CapacityController) GetUtilization() float64 {
    var totalCapacity, usedCapacity uint64

    domains := c.fmd.GetMemoryDomains()
    for _, domain := range domains {
        totalCapacity += domain.CapacityMiB
        usedCapacity += domain.CapacityMiB - domain.RemainingCapacityMiB
    }

    return float64(usedCapacity) / float64(totalCapacity) * 100
}
```

#### Resource Quotas and Limits

Enforce CXL memory quotas per namespace:

```yaml
apiVersion: v1
kind: ResourceQuota
metadata:
  name: cxl-quota
  namespace: ml-training
spec:
  hard:
    requests.cxl.io/memory: 2Ti
    limits.cxl.io/memory: 4Ti
---
apiVersion: v1
kind: LimitRange
metadata:
  name: cxl-limits
  namespace: ml-training
spec:
  limits:
  - type: Container
    default:
      cxl.io/memory: 256Gi
    defaultRequest:
      cxl.io/memory: 128Gi
    max:
      cxl.io/memory: 1Ti
    min:
      cxl.io/memory: 64Gi
```

#### Graceful Memory Migration

During node maintenance or CXL device issues, gracefully migrate workloads:

```bash
# Step 1: Cordon node to prevent new pods
kubectl cordon worker-node-1

# Step 2: Drain with CXL-aware eviction
kubectl drain worker-node-1 \
  --ignore-daemonsets \
  --delete-emptydir-data \
  --grace-period=300  # Allow time for memory migration

# Behind the scenes, CXL controller:
# 1. Signals pods to checkpoint/flush CXL memory
# 2. Waits for graceful termination
# 3. Releases CXL allocations:
#    DELETE /Fabrics/CXL/Connections/{conn}
#    DELETE /Chassis/{dev}/MemoryDomains/{dom}/MemoryChunks/{chunk}
# 4. Pods reschedule to other nodes with available CXL capacity
```

**PodDisruptionBudget for CXL Workloads**:
```yaml
apiVersion: policy/v1
kind: PodDisruptionBudget
metadata:
  name: cxl-workload-pdb
spec:
  minAvailable: 2
  selector:
    matchLabels:
      app: memory-intensive
      uses-cxl: "true"
```

#### Monitoring Integration

A sidecar exporter can scrape cxl-fmd's Redfish endpoints and expose Prometheus metrics:

**Redfish-to-Prometheus Exporter**:
```go
// Exporter scrapes cxl-fmd Redfish API and exposes Prometheus metrics
type CXLExporter struct {
    fmdEndpoint string

    // Metrics
    memoryCapacity    *prometheus.GaugeVec
    memoryRemaining   *prometheus.GaugeVec
    deviceHealth      *prometheus.GaugeVec
    connectionCount   prometheus.Gauge
}

func (e *CXLExporter) Collect(ch chan<- prometheus.Metric) {
    // Scrape MemoryDomains for capacity
    for _, chassis := range e.getChassisIDs() {
        domains := e.fmd.Get(fmt.Sprintf("/Chassis/%s/MemoryDomains", chassis))
        for _, domain := range domains.Members {
            domainData := e.fmd.Get(domain.OdataID)
            e.memoryCapacity.WithLabelValues(chassis, domain.ID).Set(float64(domainData.CapacityMiB))
            e.memoryRemaining.WithLabelValues(chassis, domain.ID).Set(float64(domainData.RemainingCapacityMiB))
        }

        // Scrape Memory health
        memory := e.fmd.Get(fmt.Sprintf("/Chassis/%s/Memory", chassis))
        for _, mem := range memory.Members {
            memData := e.fmd.Get(mem.OdataID)
            healthValue := 1.0 // Healthy
            if memData.Status.Health == "Warning" {
                healthValue = 0.5
            } else if memData.Status.Health == "Critical" {
                healthValue = 0.0
            }
            e.deviceHealth.WithLabelValues(chassis, mem.ID).Set(healthValue)
        }
    }

    // Count active connections
    conns := e.fmd.Get("/Fabrics/CXL/Connections")
    e.connectionCount.Set(float64(conns.MembersCount))
}
```

**Deployment with Exporter Sidecar**:
```yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: cxl-fmd
spec:
  template:
    spec:
      containers:
      - name: cxl-fmd
        image: cxl-fmd:latest
        ports:
        - containerPort: 8080
          name: redfish

      - name: metrics-exporter
        image: cxl-prometheus-exporter:latest
        env:
        - name: CXL_FMD_ENDPOINT
          value: "http://localhost:8080"
        ports:
        - containerPort: 9090
          name: metrics
```

**Alerting Rules** (using exported metrics):
```yaml
apiVersion: monitoring.coreos.com/v1
kind: PrometheusRule
metadata:
  name: cxl-alerts
spec:
  groups:
  - name: cxl.rules
    rules:
    - alert: CXLMemoryCapacityLow
      expr: cxl_memory_remaining_mib / cxl_memory_capacity_mib < 0.1
      for: 5m
      labels:
        severity: warning
      annotations:
        summary: "CXL memory pool {{ $labels.device }} below 10% capacity"

    - alert: CXLDeviceHealthDegraded
      expr: cxl_device_health < 1
      for: 1m
      labels:
        severity: critical
      annotations:
        summary: "CXL device {{ $labels.device }} health degraded"
```

#### OCP Future Enhancements

The OCP CMS (Composable Memory Systems) working group has identified several future enhancements for composable memory in Kubernetes. These represent areas where cxl-fmd's capabilities can be extended:

**NUMA-Aware Co-Scheduling**

Scheduling decisions will consider NUMA topology, aligning CPU cores with memory slices that are physically closest on the CXL fabric. cxl-fmd's topology discovery enables this by exposing device-to-host relationships.

```go
// MFM Controller can use cxl-fmd's topology to compute NUMA distances
func (c *MFMController) GetNUMADistance(nodeID, deviceID string) int {
    // Query cxl-fmd for fabric topology
    zones := c.fmd.Get("/redfish/v1/Fabrics/CXL/Zones")
    // Calculate hop count from node's VCS to device
    return c.computeHops(nodeID, deviceID, zones)
}
```

**Dynamic Resizing of Memory Allocations**

Pods can request additional CXL memory at runtime. cxl-fmd's MemoryChunk API supports this:

```bash
# Resize existing chunk (create additional chunk in same domain)
POST /redfish/v1/Chassis/{dev}/MemoryDomains/{dom}/MemoryChunks
{
    "MemoryChunkSizeMiB": 8192,
    "InterleaveSets": [...]
}

# Add to existing connection (not implemented: cxl-fmd answers a PATCH of a
# Connection with 405; a Connection lists every chunk of the SLD or LD it
# binds, so a chunk made on that LD would be in it, but no chunk is made on
# a bound LD: 409)
PATCH /redfish/v1/Fabrics/CXL/Connections/{conn}
{
    "MemoryChunkInfo": [
        {"MemoryChunk": {"@odata.id": ".../MemoryChunks/chunk_0"}},
        {"MemoryChunk": {"@odata.id": ".../MemoryChunks/chunk_1"}}  # New
    ]
}
```

**Fabric-Aware Metrics and Telemetry**

Integrating Kubernetes resource metrics with fabric-level telemetry enables smarter scheduling. cxl-fmd exposes:

| Metric | cxl-fmd API | Use Case |
|--------|------------|----------|
| Memory utilization | `GET /MemoryDomains/{dom}` (MemorySizeMiB) and its MemoryChunks | Capacity planning |
| Device health | `GET /Memory/{ld}` (Status.Health) | Predictive migration |
| QoS bandwidth | `GET /CXLLogicalDevices/{ld}` (QoS, read only) | Tier selection |
| Switch port status | `GET /Switches/{sw}/Ports` | Fabric health |

**QoS and Bandwidth-Aware Scheduling**

Different memory slices may offer varying performance characteristics. cxl-fmd serves each LD's QoS read only; configuring it is not implemented:

```yaml
# ResourceClaimTemplate with QoS requirements
apiVersion: resource.k8s.io/v1
kind: ResourceClaimTemplate
metadata:
  name: high-bandwidth-cxl
spec:
  spec:
    devices:
      requests:
        - name: req-0
          exactly:
            deviceClassName: mfm-pooled-memory
          # MFM Controller uses cxl-fmd QoS APIs to match
          allocationMode: ExactCount
          count: 1
```

```go
// MFM Controller selects LD based on QoS requirements
func (c *MFMController) SelectLDForQoS(minBandwidthPercent int) string {
    devices := c.fmd.Get("/redfish/v1/Chassis")
    for _, dev := range devices.Members {
        lds := c.fmd.Get(dev.OdataID + "/PCIeDevices/.../CXLLogicalDevices")
        for _, ld := range lds.Members {
            ldData := c.fmd.Get(ld.OdataID)
            if ldData.QoS.AllocatedBandwidth >= minBandwidthPercent {
                return ld.Id
            }
        }
    }
    return ""
}
```

**CXL Security Domain Integration**

Multi-tenant isolation can leverage CXL security domains. cxl-fmd's MHD head-to-host validation provides the foundation:

```
Security Levels (per OCP):
- Fully-Trusted: No isolation between tenants
- Secure: Hardware-enforced isolation via CXL security domains
- Default: Software-enforced isolation via vPPB binding

cxl-fmd provides:
- MHD head-to-host access control (400 on cross-head access)
- vPPB binding enforcement for switch-attached devices
- Zone-based isolation via VCS partitioning
```

**Expanded Allocation Parameters**

Future DRA requests will include additional factors that cxl-fmd can support:

| Parameter | cxl-fmd Support | API |
|-----------|-----------------|-----|
| Distance metrics | Fabric topology | `/Fabrics/CXL/Zones`, `/Switches` |
| Interleaving | MemoryChunk InterleaveSets | `POST /MemoryChunks` |
| RAS profiles | Device health/media status | `GET /Memory/{ld}` |
| Security config | MHD access control | Connection validation |

### OpenStack Integration

#### Nova CXL Memory Backend

OpenStack Nova can use cxl-fmd to provide CXL-backed memory to VM instances.

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

The driver below reads `RemainingCapacityMiB`, which cxl-fmd does not serve.

**Flavor Definition**:
```
openstack flavor create cxl-large \
  --ram 65536 \
  --vcpus 16 \
  --property hw:cxl_memory=262144  # 256GB CXL memory
```

**Nova Compute Driver Integration**:
```python
class CXLMemoryDriver:
    def __init__(self, fmd_endpoint):
        self.fmd = RedfishClient(fmd_endpoint)

    def allocate_memory(self, instance, size_mb):
        """Allocate CXL memory for instance."""
        # Find device with sufficient capacity
        for chassis in self.fmd.get("/Chassis")["Members"]:
            domain = self.fmd.get(f"{chassis['@odata.id']}/MemoryDomains").get("Members", [{}])[0]
            domain_data = self.fmd.get(domain["@odata.id"])

            if domain_data.get("RemainingCapacityMiB", 0) >= size_mb:
                # Step 1: Create memory chunk
                chunk = self.fmd.post(f"{domain['@odata.id']}/MemoryChunks", {
                    "MemoryChunkSizeMiB": size_mb,
                    "AddressRangeType": "PMEM"
                })

                # Step 2: Create connection to bind chunk to host
                connection = self.fmd.post("/Fabrics/CXL/Connections", {
                    "Links": {
                        "InitiatorEndpoints": [{"@odata.id": f"/Fabrics/CXL/Endpoints/{self.host_id}"}],
                        "TargetEndpoints": [{"@odata.id": chassis["@odata.id"]}]
                    },
                    "MemoryChunkInfo": [{
                        "MemoryChunk": {"@odata.id": chunk["@odata.id"]},
                        "AccessCapabilities": ["Read", "Write"]
                    }]
                })
                return {"connection": connection["Id"], "chunk": chunk["Id"]}

        raise InsufficientMemoryError()

    def deallocate_memory(self, allocation):
        """Release CXL memory allocation."""
        self.fmd.delete(f"/Fabrics/CXL/Connections/{allocation['connection']}")
        # Also delete the chunk to free capacity
        self.fmd.delete(f"/Chassis/{self.device}/MemoryDomains/{self.domain}/MemoryChunks/{allocation['chunk']}")
```

#### Placement Service Integration

```python
# Custom resource provider for CXL memory
RESOURCE_CLASS = "CUSTOM_CXL_MEMORY_MB"

class CXLResourceProvider:
    def update_inventory(self):
        """Sync CXL capacity with Placement service."""
        total = 0
        reserved = 0

        for device in self.fmd.get_devices():
            total += device.total_capacity_mb
            reserved += device.allocated_capacity_mb

        self.placement.set_inventory(
            self.host_uuid,
            RESOURCE_CLASS,
            total=total,
            reserved=reserved,
            allocation_ratio=1.0
        )
```

### VMware vSphere Integration

#### vSphere CXL Memory Resource Pool

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

cxl-fmd could integrate with vSphere via the CIM/WBEM or REST API to expose CXL resources.

**Resource Pool Configuration**:
```
Cluster: Production
└── Resource Pool: CXL-Enabled
    ├── CXL Memory Pool: 16TB
    │   ├── Reservation: 8TB (guaranteed)
    │   └── Limit: 16TB (burst)
    └── VMs:
        ├── DB-Server (4TB CXL)
        ├── Analytics (8TB CXL)
        └── Cache-Tier (2TB CXL)
```

**vSphere Integration Points**:
- Hardware Status Provider: Report CXL device health
- DRS Integration: Consider CXL locality in VM placement
- Storage Policy: Define CXL memory tiers as storage policies
- Alarms: Alert on CXL health threshold violations

### Container Runtime Integration

#### containerd CXL Plugin

Direct integration with containerd for memory-centric workloads.

```go
// CXL memory plugin for containerd
type CXLPlugin struct {
    fmd *FMDClient
}

func (p *CXLPlugin) Allocate(ctx context.Context, req *AllocateRequest) (*AllocateResponse, error) {
    // Allocate CXL memory for container
    ld, err := p.fmd.AllocateLD(req.Size)
    if err != nil {
        return nil, err
    }

    // Return memory region for cgroup configuration
    return &AllocateResponse{
        NumaNode: ld.NumaNode,
        Size:     ld.Size,
        DevPath:  fmt.Sprintf("/dev/dax%d.%d", ld.Region, ld.LD),
    }, nil
}
```

#### Podman/CRI-O Integration

```yaml
# Pod with CXL memory annotation
apiVersion: v1
kind: Pod
metadata:
  annotations:
    io.kubernetes.cri-o.cxl-memory: "256Gi"
spec:
  containers:
  - name: app
    # Container sees CXL memory as additional NUMA node
```

---

## Middleware Value Proposition

> **Partly implemented.** Parts of this section describe features cxl-fmd does not have; each is marked below.

### Abstraction Layer Benefits

cxl-fmd provides an abstraction between hardware-specific CXL implementations and orchestration systems:

```
┌─────────────────────────────────────────────────────────┐
│           Orchestration Layer                           │
│  Kubernetes │ OpenStack │ VMware │ Custom               │
└──────────────────────┬──────────────────────────────────┘
                       │ Redfish API (standardized)
┌──────────────────────┴──────────────────────────────────┐
│                     cxl-fmd                             │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐       │
│  │  Discovery  │ │  Inventory  │ │   Health    │       │
│  │   Engine    │ │   Manager   │ │  Monitor    │       │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘       │
│         └───────────────┼───────────────┘               │
│                         │ libcxlmi (CCI commands)       │
└─────────────────────────┼───────────────────────────────┘
                          │
┌─────────────────────────┴───────────────────────────────┐
│              CXL Hardware Layer                         │
│  Samsung │ SK Hynix │ Micron │ Intel │ AMD │ ...       │
└─────────────────────────────────────────────────────────┘
```

**Benefits**:

1. **Vendor Independence**: Orchestrators don't need vendor-specific drivers
2. **Protocol Abstraction**: Hide MCTP/PCIe transport complexity
3. **Schema Compliance**: responses are checked with the DMTF Redfish Service Validator over mock topologies (`meson test --suite validator`)
4. **Version Compatibility**: not implemented; section and table numbers follow CXL r4.0
5. **Security Boundary**: not implemented; cxl-fmd does not authenticate clients or keep an audit log (see `man cxl-fmd`, SECURITY)

### Event-Driven Architecture

cxl-fmd can serve as an event source for reactive orchestration:

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

cxl-fmd has no EventService. CXL event records are served as `LogEntry`
resources (see "Where cxl-fmd puts CXL event records" in
`docs/Redfish-API.md`), which a client has to poll.

```
┌─────────────────────────────────────────────────────────┐
│                    Event Consumers                      │
├─────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │Prometheus│  │ Grafana  │  │  PagerDuty│  │ Custom  │ │
│  │ Alertmgr │  │ Loki     │  │  OpsGenie │  │ Webhook │ │
│  └────┬─────┘  └────┬─────┘  └────┬──────┘  └────┬────┘ │
│       └─────────────┼─────────────┼──────────────┘      │
│                     │ Redfish EventService              │
│  ┌──────────────────┴───────────────────────────────┐   │
│  │                    cxl-fmd                        │   │
│  │  Events:                                          │   │
│  │  - DeviceDiscovered                               │   │
│  │  - MemoryHealthWarning                            │   │
│  │  - AllocationChanged                              │   │
│  │  - LinkStateChanged                               │   │
│  │  - QoSThresholdExceeded                           │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Metrics and Observability

cxl-fmd can expose Prometheus-compatible metrics for observability platforms:

> **Not implemented.** This describes what cxl-fmd could do; the code does not do it today.

cxl-fmd serves no metrics endpoint; the names below are a proposal.

```prometheus
# Memory capacity metrics
cxl_memory_total_bytes{device="mld-1",tier="tier1"} 1099511627776
cxl_memory_allocated_bytes{device="mld-1",tier="tier1"} 549755813888
cxl_memory_available_bytes{device="mld-1",tier="tier1"} 549755813888

# Health metrics
cxl_device_health_status{device="mld-1"} 1  # 1=healthy, 0=degraded
cxl_correctable_errors_total{device="mld-1"} 42
cxl_media_errors_total{device="mld-1"} 0

# Performance metrics
cxl_bandwidth_read_bytes_per_second{device="mld-1"} 25000000000
cxl_bandwidth_write_bytes_per_second{device="mld-1"} 20000000000
cxl_latency_read_nanoseconds{device="mld-1",quantile="0.99"} 180
cxl_latency_write_nanoseconds{device="mld-1",quantile="0.99"} 200

# Fabric metrics
cxl_switch_port_link_status{switch="sw-1",port="0"} 1
cxl_switch_port_bandwidth_gbps{switch="sw-1",port="0"} 64
cxl_fabric_total_devices 12
cxl_fabric_healthy_devices 11
```

---

## Deployment Architectures

> **Not implemented.** These are design sketches: no deployment manifest, image or BMC integration ships with cxl-fmd.

### Single-Node Deployment

For direct-attached CXL memory devices without a fabric.

```
┌─────────────────────────────────────────┐
│              Compute Node               │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐    │
│  │         Applications            │    │
│  └──────────────┬──────────────────┘    │
│                 │                        │
│  ┌──────────────┴──────────────────┐    │
│  │        Linux Kernel             │    │
│  │   (CXL driver, DAX, NUMA)       │    │
│  └──────────────┬──────────────────┘    │
│                 │                        │
│  ┌──────────────┴──────────────────┐    │
│  │          cxl-fmd                │    │ ← Localhost Redfish API
│  └──────────────┬──────────────────┘    │
│                 │ ioctl                  │
│  ┌──────────────┴──────────────────┐    │
│  │   /dev/cxl/mem0   /dev/cxl/mem1 │    │
│  └──────────────┬──────────────────┘    │
│                 │ PCIe                   │
│  ┌──────────────┴──────────────────┐    │
│  │     CXL Type 3 SLD/MLD          │    │
│  └─────────────────────────────────┘    │
└─────────────────────────────────────────┘
```

**Use Cases**:
- Development and testing
- Single-server memory expansion
- Edge computing with local CXL memory

### BMC-Hosted Deployment

cxl-fmd runs on the BMC for out-of-band fabric management.

```
┌─────────────────────────────────────────────────────────┐
│                    Server Platform                       │
├──────────────────────────┬──────────────────────────────┤
│      Host System         │         BMC                   │
│  ┌────────────────┐      │  ┌─────────────────────┐     │
│  │  Applications  │      │  │     cxl-fmd         │     │
│  └───────┬────────┘      │  └──────────┬──────────┘     │
│          │               │             │ MCTP/I2C       │
│  ┌───────┴────────┐      │             │                │
│  │  Linux Kernel  │      │             │                │
│  └───────┬────────┘      │             │                │
│          │ PCIe          │             │                │
│          │               │             │                │
│  ┌───────┴───────────────┴─────────────┴──────────┐     │
│  │              CXL Switch / Memory Devices        │     │
│  │    (accessible via both PCIe and MCTP)          │     │
│  └─────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────┘
                              │
                              │ Network (Redfish)
                              ▼
                    ┌─────────────────┐
                    │   Management    │
                    │   Controller    │
                    └─────────────────┘
```

**Use Cases**:
- Data center management integration
- Out-of-band health monitoring
- Pre-boot fabric configuration
- Secure management plane isolation

### Fabric Manager Appliance

Dedicated management appliance for large CXL fabrics.

```
┌─────────────────────────────────────────────────────────┐
│               CXL Fabric Manager Appliance              │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────┐   │
│  │                  cxl-fmd                         │   │
│  │  ┌───────────┐ ┌───────────┐ ┌───────────┐     │   │
│  │  │ Discovery │ │ Allocator │ │  Health   │     │   │
│  │  │  Engine   │ │  Engine   │ │  Monitor  │     │   │
│  │  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘     │   │
│  │        └─────────────┼─────────────┘           │   │
│  └──────────────────────┼──────────────────────────┘   │
│                         │ MCTP over Ethernet/PCIe      │
└─────────────────────────┼───────────────────────────────┘
                          │
    ┌─────────────────────┼─────────────────────┐
    │                     │                     │
    ▼                     ▼                     ▼
┌────────┐          ┌────────┐          ┌────────┐
│ Rack 1 │          │ Rack 2 │          │ Rack 3 │
│ Switch │──────────│ Switch │──────────│ Switch │
│   │    │          │   │    │          │   │    │
│ MLDs   │          │ MLDs   │          │ MLDs   │
└────────┘          └────────┘          └────────┘
```

**Use Cases**:
- Large-scale disaggregated memory deployments
- Multi-rack CXL fabrics
- Centralized policy enforcement
- High-availability fabric management

### Kubernetes MFM Deployment (OCP Architecture)

In the OCP MFM architecture, cxl-fmd runs as a DaemonSet on each CXL-capable node, serving as the MFM component that the MFM Node Agent communicates with.

```yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: cxl-fmd
  namespace: kube-system
  labels:
    app.kubernetes.io/name: cxl-fmd
    app.kubernetes.io/component: mfm
spec:
  selector:
    matchLabels:
      app: cxl-fmd
  template:
    metadata:
      labels:
        app: cxl-fmd
    spec:
      hostNetwork: true
      hostPID: true
      nodeSelector:
        cxl.io/capable: "true"
      containers:
      # cxl-fmd: The MFM component providing Redfish API
      - name: cxl-fmd
        image: ghcr.io/cxl-fmd/cxl-fmd:latest
        securityContext:
          privileged: true
        volumeMounts:
        - name: dev
          mountPath: /dev
        - name: sys
          mountPath: /sys
        ports:
        - containerPort: 8080
          hostPort: 8080
          name: redfish

      # MFM Node Agent: Handles NodePrepareResources/NodeUnprepareResources
      - name: mfm-node-agent
        image: ghcr.io/cxl-fmd/mfm-node-agent:latest
        env:
        - name: MFM_ENDPOINT
          value: "http://localhost:8080"
        - name: NODE_NAME
          valueFrom:
            fieldRef:
              fieldPath: spec.nodeName
        volumeMounts:
        - name: cdi
          mountPath: /var/run/cdi
        - name: kubelet-plugins
          mountPath: /var/lib/kubelet/plugins

      volumes:
      - name: dev
        hostPath:
          path: /dev
      - name: sys
        hostPath:
          path: /sys
      - name: cdi
        hostPath:
          path: /var/run/cdi
      - name: kubelet-plugins
        hostPath:
          path: /var/lib/kubelet/plugins
```

**Use Cases**:
- OCP MFM Kubernetes integration
- DRA-based CXL memory allocation
- Per-node CXL resource management

---

## Summary

cxl-fmd is meant to serve as the **MFM (Memory Fabric Manager)** component in the OCP CMS (Composable Memory Systems) architecture, providing the external service that maintains topology, zone, and capacity information for all memory pools and connected nodes. The MFM Controller and Node Agent below are not part of this repository.

### Role in OCP Architecture

```
┌─────────────────────────────────────┐
│  MFM Controller (K8s DRA)           │  ← Cluster-level, calls cxl-fmd
├─────────────────────────────────────┤
│  cxl-fmd (MFM)                      │  ← THIS PROJECT
├─────────────────────────────────────┤
│  MFM Node Agent (K8s DaemonSet)     │  ← Per-node, calls cxl-fmd
├─────────────────────────────────────┤
│  CXL Hardware (SLD/MLD/MHD/Switch)  │
└─────────────────────────────────────┘
```

### Core Capabilities

1. **Standardized Management Interface**: Redfish API consumed by MFM Controller and Node Agent
2. **Hardware Abstraction**: Vendor-agnostic, transport-agnostic (MCTP/ioctl) fabric management
3. **Memory Lifecycle Management**: Full CRUD operations for MemoryChunks and Connections
4. **Access Control**: MHD head-to-host validation, vPPB binding automation
5. **Topology Information**: Zones (VCS), switches, ports, endpoints for scheduler hints
6. **Capacity Tracking**: Per-domain MemorySizeMiB and MemoryChunks (the free capacity is not served)
7. **Health Monitoring**: Device health status, read when a device is discovered

### Key API Capabilities

| API | Purpose | Called By |
|-----|---------|-----------|
| `GET /Chassis/{id}/MemoryDomains/{dom}` | Query capacity | MFM Controller, Node Agent |
| `POST /MemoryDomains/{dom}/MemoryChunks` | Allocate memory slice | MFM Controller |
| `DELETE /MemoryDomains/{dom}/MemoryChunks/{chunk}` | Deallocate memory | MFM Controller |
| `POST /Fabrics/CXL/Connections` | Bind memory to node | MFM Node Agent |
| `DELETE /Fabrics/CXL/Connections/{conn}` | Unbind memory | MFM Node Agent |
| `GET /Fabrics/CXL/Zones` | Topology for AvailableOnNodes hints | MFM Controller |
| `GET /Memory/{ld}` | Health status for eviction decisions | MFM Node Agent |

### OCP Kubernetes Integration

cxl-fmd enables the OCP DRA-based Kubernetes integration:

1. **MFM Controller** queries cxl-fmd for capacity, creates MemoryChunks, returns topology hints
2. **Kubernetes Scheduler** uses `AvailableOnNodes` to place pods on CXL-capable nodes
3. **MFM Node Agent** creates Connections via cxl-fmd, maps memory into pod namespace
4. **Pod termination** triggers NodeUnprepareResources, deleting Connections and releasing capacity

### Future OCP Enhancements Supported

| Enhancement | cxl-fmd Support |
|-------------|-----------------|
| NUMA-Aware Co-Scheduling | Fabric topology via `/Fabrics/CXL/Zones` |
| Dynamic Memory Resizing | Additional MemoryChunks (no Connection updates: not implemented) |
| QoS-Aware Scheduling | Per-LD QoS, read only (configuration not implemented) |
| Security Domains | MHD access control, vPPB isolation |

The combination of cxl-fmd's Redfish API and the OCP MFM architecture enables composable memory in Kubernetes without modifying Kubernetes core, maintaining a standard and maintainable integration.
