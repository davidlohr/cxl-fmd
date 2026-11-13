# Dynamic Capacity Device (DCD) FM-API to Redfish Mapping

This document specifies how cxl-fmd exposes the CXL Fabric Management API
(FM-API) **DCD Management command set (56h)** through Redfish. The mapping is
implemented in `src/dcd.c` (FM-API and mock state) and
`src/redfish/dcd_handlers.c` (Redfish), and exercised by `tests/test_dcd.py`
against `mock/dcd-device.json`.

It fills a gap in the published DMTF material, which was checked on
2026-10-21:

| Source | What it says about DCD |
|--------|------------------------|
| [DSP0288 v1.4.0](https://www.dmtf.org/sites/default/files/standards/documents/DSP0288_1.4.0.pdf) (2026-08-07), *CXL to Redfish Mapping* | Table 2 (FM-API) maps only the Physical Switch (51h), Virtual Switch (52h), MLD Component (54h) and *Get PBR Link Partner Info* commands. No 55h (MHD) and no 56h (DCD) command is mapped. The spec still normatively references CXL r3.0. |
| [DSP8010 2026.1](https://redfish.dmtf.org/schemas/v1/) | Adds everything needed for the write path (earlier releases had `MemoryRegion v1.0.3` with inline extents and `PCIeDevice` `CXLDevice/DynamicCapacity`, but no way to add or release capacity): a standalone `MemoryExtent` resource, `MemoryRegion v1.1.0`, `CXLLogicalDevice v1.4.0` with `DynamicCapacityAdd` / `DynamicCapacityRelease` actions and extent/tag counters, `PCIeDevice v1.22.0` with the `CXLReferenceDCMemory` action, the `EnableSharedAccess` policy and a `MemoryExtents` collection link, and `MemoryChunks v1.7.0` with `Links/MemoryExtent`. |
| [DSP8010 2026.2](https://redfish.dmtf.org/schemas/v1/) (bundle vendored in `schema/dmtf`) | `PCIeDevice v1.23.0` adds `CXLDevice/DeviceClass` (`SLD`, `MLD`, `MH_SLD`, `MH_MLD`). Not DCD-specific but directly useful to cxl-fmd. |

So the JSON schema has everything the DCD surface needs and the vendored
bundle carries it, but DSP0288 has not caught up with it. Everything below
follows the 2026.1 model, with the FM-API field semantics taken from CXL r3.2
Section 7.6.7.6 as encoded in libcxlmi (`struct cxlmi_cmd_fmapi_*` in
`src/cxlmi/api-types.h`), which already implements all nine commands.

## Table of Contents

1. [Table 3 - DCD Management FM-API to Redfish Mapping](#table-3---dcd-management-fm-api-to-redfish-mapping)
2. [Resource Model](#resource-model)
3. [Command Details](#command-details)
4. [Host ID Resolution](#host-id-resolution)
5. [Proposed cxl-fmd Changes](#proposed-cxl-fmd-changes)
6. [Open Questions](#open-questions)

---

## Table 3 - DCD Management FM-API to Redfish Mapping

Same format as Tables 1 and 2 in [Redfish-API.md](Redfish-API.md). This
table is cxl-fmd's proposal, not DSP0288 text.

| CXL Command | CXL Field | Redfish Resource | Redfish Property | Implemented |
|-------------|-----------|------------------|------------------|:-----------:|
| Get DCD Info (5600h) | Number of Hosts | `PCIeDevice` | `CXLDevice/DynamicCapacity/MaxHosts` | Yes |
| Get DCD Info | Number of Supported DC Regions | `PCIeDevice` | `CXLDevice/DynamicCapacity/MaxDynamicCapacityRegions` | Yes |
| Get DCD Info | Supported Add Capacity Selection Policies | `PCIeDevice` | `CXLDevice/DynamicCapacity/AddCapacityPoliciesSupported` | Yes |
| Get DCD Info | Supported Release Capacity Removal Policies | `PCIeDevice` | `CXLDevice/DynamicCapacity/ReleaseCapacityPoliciesSupported` | Yes |
| Get DCD Info | Sanitize on Release Configuration Mask | `PCIeDevice` | `CXLDevice/DynamicCapacity/SanitizationOnReleaseSupport[]` | Yes |
| Get DCD Info | Total Dynamic Capacity | `PCIeDevice` | `CXLDevice/DynamicCapacity/TotalDynamicCapacityMiB` | Yes |
| Get DCD Info | Region N Supported Block Size Mask | `PCIeDevice` | `CXLDevice/DynamicCapacity/MemoryBlockSizesSupported[]` | Yes |
| Get Host DC Region Configuration (5601h) | Region Base | `MemoryRegion` | `RegionBaseOffsetMiB` | Yes |
| Get Host DC Region Configuration | Region Length | `MemoryRegion` | `RegionSizeMiB` | Yes |
| Get Host DC Region Configuration | Region Block Size | `MemoryRegion` | `BlockSizeMiB` | Yes |
| Get Host DC Region Configuration | Region index | `MemoryRegion` | `RegionNumber`, `RegionType` = `Dynamic` | Yes |
| Get Host DC Region Configuration | Flags | `MemoryRegion` | `ShareableRegion`, `NonVolatileRegion`, `HardwareManagedCoherencyRegion`, `ReadOnlyRegion` | Yes |
| Get Host DC Region Configuration | Sanitize on Release | `MemoryRegion` | `SanitizeOnRelease` | Yes |
| Get Host DC Region Configuration | Number of Extents Supported / Available | `CXLLogicalDevice` | `TotalSupportedExtents`, `AvailableExtents` | Yes |
| Get Host DC Region Configuration | Number of Tags Supported / Available | `CXLLogicalDevice` | `TotalSupportedTags`, `AvailableTags` | Yes |
| Set DC Region Configuration (5602h) | Region Block Size, Sanitize on Release | `MemoryRegion` | PATCH `BlockSizeMiB`, `SanitizeOnRelease` | Yes |
| Get DC Region Extent Lists (5603h) | Extent Start DPA | `MemoryExtent` | `ExtentOffsetMiB` | Yes |
| Get DC Region Extent Lists | Extent Length | `MemoryExtent` | `ExtentSizeMiB` | Yes |
| Get DC Region Extent Lists | Extent Tag | `MemoryExtent` | `Tag` | Yes |
| Get DC Region Extent Lists | Shared Extent Sequence | `MemoryExtent` | `SequenceNumber` | Yes |
| Get DC Region Extent Lists | Total Extent Count (per region) | `MemoryRegion` | `ExtentsCount`, `MemoryExtents[].ExtentLink` | Yes |
| Initiate Dynamic Capacity Add (5604h) | All | `CXLLogicalDevice` | `DynamicCapacityAdd` action | Yes |
| Initiate Dynamic Capacity Release (5605h) | All | `CXLLogicalDevice` | `DynamicCapacityRelease` action | Yes |
| Initiate Dynamic Capacity Release | Prescriptive, single extent | `MemoryExtent` | DELETE | Yes |
| Dynamic Capacity Add Reference (5606h) | Tag | `PCIeDevice` | `CXLReferenceDCMemory` action, `FMReference` = `true` | Yes |
| Dynamic Capacity Remove Reference (5607h) | Tag | `PCIeDevice` | `CXLReferenceDCMemory` action, `FMReference` = `false` | Yes |
| Dynamic Capacity List Tags (5608h) | Tag Flags: FM reference | `MemoryExtent` | `FMReferencedExtent` | Yes |
| Dynamic Capacity List Tags | Pending Reference Bitmap | `MemoryRegion` | `MemoryExtents[].Pending` | Yes |
| Dynamic Capacity List Tags | Reference Bitmap (per-host) | | No standard property, see Comment 4 | No |
| Dynamic Capacity Event Record (via Get Event Records on the FM-owned LD) | All | `LogEntry` | `CXLEntryType` = `DynamicCapacity` | No |

### Comments for Table 3

**Comment 1:** DC regions are per host. The `MemoryRegion` collection hangs
off the `CXLLogicalDevice` that the host is bound to, so the same physical
region on a multi-host DCD appears once under each LD that can see
it, with the host-specific `RegionBaseOffsetMiB` reported by 5601h for that
Host ID.

**Comment 2:** Extents are device-wide (keyed by DPA and tag), so the
`MemoryExtent` collection hangs off the `PCIeDevice`. A `MemoryRegion`
refers to its extents through `MemoryExtents[].ExtentLink`; the inline
`ExtentOffsetMiB`/`ExtentSizeMiB`/`Tag`/`SequenceNumber` members are
deprecated since `MemoryRegion v1.1.0` and should not be emitted.

**Comment 3:** All FM-API DC lengths and offsets are bytes aligned to the
region block size. Redfish uses MiB throughout. cxl-fmd shall reject a
request whose MiB value does not convert to a block-size multiple with
`400 Bad Request` and a `PropertyValueIncorrect` message rather than
rounding.

**Comment 4:** The per-host reference bitmaps returned by 5608h (which hosts
currently hold a shared tag, and which have an add pending) have no standard
property in 2026.1 other than the aggregate `Pending` flag. If needed, expose
them as `Oem/Cxl/HostReferences[]` on `MemoryExtent`, or push for a
`Links/ReferencingEndpoints` addition upstream.

**Comment 5:** 5604h and 5605h are asynchronous at the device: the FM
request returns once the device has queued the add/release toward the host,
and completion is signalled by a DC event record. The Redfish actions should
therefore return `202 Accepted` with a `Task`, which is the "BG
commands/context" item in `TODO`. Until the task service exists, returning
`204 No Content` after the FM-API command succeeds is acceptable, but the
extent will show `Pending = true` until the host accepts it.

---

## Resource Model

```
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}
    CXLDevice/DynamicCapacity                          <- 5600h
    CXLDevice/DynamicCapacity/MemoryExtents            -> collection link
    Actions/PCIeDevice.CXLReferenceDCMemory            <- 5606h / 5607h

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents            <- 5603h (all hosts)
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}      DELETE -> 5605h prescriptive

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}
    TotalSupportedExtents, AvailableExtents,
    TotalSupportedTags, AvailableTags                  <- 5601h trailer
    MemoryRegions                                      -> collection link
    Actions/CXLLogicalDevice.DynamicCapacityAdd        <- 5604h
    Actions/CXLLogicalDevice.DynamicCapacityRelease    <- 5605h

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions           <- 5601h
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}  PATCH -> 5602h

/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
    Links/MemoryExtent                                 -> the extent this chunk backs
```

Extent IDs should be stable across rescans. Use the DPA of the extent in
hex (`ext_0x40000000`), since the extent list index shifts whenever an
extent is released. Region IDs are the region index (`0`..`7`).

---

## Command Details

Field layouts are quoted from libcxlmi's `api-types.h`, which tracks CXL
r3.2. Byte offsets are omitted; the struct order is the wire order.

### Get DCD Info (5600h)

Read on discovery for every Type 3 device whose CEL includes 5600h, directly
for SLDs and through `CXLMI_TUNNEL_MLD`/`CXLMI_TUNNEL_MHD` for the FM-owned
LD of an MLD or MHD.

```c
int cxlmi_cmd_fmapi_get_dcd_info(struct cxlmi_endpoint *ep,
                                 struct cxlmi_tunnel_info *ti,
                                 struct cxlmi_cmd_fmapi_get_dcd_info_rsp *ret);
```

| Response field | Encoding | Redfish |
|----------------|----------|---------|
| `num_hosts` | count | `MaxHosts` |
| `num_supported_dc_regions` | count, 1..8 | `MaxDynamicCapacityRegions` |
| `capacity_selection_policies` | bitmask: bit 0 Free, bit 1 Contiguous, bit 2 Prescriptive, bit 3 Enable Shared Access | `AddCapacityPoliciesSupported[]` with `Free`, `Contiguous`, `Prescriptive`, `EnableSharedAccess` |
| `capacity_removal_policies` | bitmask: bit 0 Tag-based, bit 1 Prescriptive | `ReleaseCapacityPoliciesSupported[]` with `TagBased`, `Prescriptive` |
| `sanitize_on_release_config_mask` | bit N set = region N's Sanitize on Release is configurable | `SanitizationOnReleaseSupport[]` of `{RegionNumber, SanitizationOnReleaseSupported}` |
| `total_dynamic_capacity` | multiples of 256 MB | `TotalDynamicCapacityMiB` = value * 256 |
| `region_N_supported_blk_sz_mask` | bit n set = block size 2^n bytes supported; valid for N < `num_supported_dc_regions` | `MemoryBlockSizesSupported[]` of `{RegionNumber, BlockSizeMiB[]}` listing every set bit >= 20 as 2^(n-20) MiB |

```json
"CXLDevice": {
  "DeviceType": "Type3",
  "DynamicCapacity": {
    "MaxHosts": 1,
    "MaxDynamicCapacityRegions": 2,
    "TotalDynamicCapacityMiB": 65536,
    "AddCapacityPoliciesSupported": ["Free", "Contiguous", "Prescriptive"],
    "ReleaseCapacityPoliciesSupported": ["TagBased", "Prescriptive"],
    "MemoryBlockSizesSupported": [
      { "RegionNumber": 0, "BlockSizeMiB": [256, 512, 1024] },
      { "RegionNumber": 1, "BlockSizeMiB": [256] }
    ],
    "SanitizationOnReleaseSupport": [
      { "RegionNumber": 0, "SanitizationOnReleaseSupported": true },
      { "RegionNumber": 1, "SanitizationOnReleaseSupported": false }
    ],
    "MemoryExtents": {
      "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents"
    }
  }
}
```

### Get Host DC Region Configuration (5601h)

Read once per host (see [Host ID Resolution](#host-id-resolution)) on
discovery and again after any add/release/config change. libcxlmi caps the
returned array at 8 regions, which matches the CXL maximum.

```c
int cxlmi_cmd_fmapi_get_dc_reg_config(struct cxlmi_endpoint *ep,
                                      struct cxlmi_tunnel_info *ti,
                                      struct cxlmi_cmd_fmapi_get_host_dc_region_config_req *in,
                                      struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp *ret);
```

Request: `host_id`, `region_cnt` (max to return), `start_region_id`.

| Response field | Redfish `MemoryRegion` property |
|----------------|--------------------------------|
| `num_regions` | number of members in the `MemoryRegions` collection |
| `region_configs[i].base` | `RegionBaseOffsetMiB` (bytes / 2^20) |
| `region_configs[i].decode_len` | not exposed; it is the HDM decode window the host programs (multiple of 256 MB) |
| `region_configs[i].region_len` | `RegionSizeMiB` |
| `region_configs[i].block_size` | `BlockSizeMiB` |
| `region_configs[i].flags` | Attributes1 (CXL r3.2 Table 7-68, byte 20h); the bits are the CDAT DSMAS Flags positions: bit 2 NonVolatile -> `NonVolatileRegion`, bit 3 Sharable -> `ShareableRegion`, bit 4 Hardware Managed Coherency -> `HardwareManagedCoherencyRegion`, bit 5 Interconnect specific Dynamic Capacity Management -> `InterconnectSpecificDCManagement`, bit 6 Read-Only -> `ReadOnlyRegion`. Bits 1:0 and 7 are reserved |
| `region_configs[i].sanitize_on_release` | Attributes2 (byte 24h), bit 0 -> `SanitizeOnRelease`; bits 7:1 reserved |
| `num_extents_supported` / `num_extents_available` | `CXLLogicalDevice.TotalSupportedExtents` / `AvailableExtents` |
| `num_tags_supported` / `num_tags_available` | `CXLLogicalDevice.TotalSupportedTags` / `AvailableTags` |

Always set `RegionType` to `Dynamic` and `InterconnectSpecificDCManagement`
to `true` for regions discovered this way. A region with `region_len == 0`
is not configured and should be omitted from the collection rather than
shown empty.

```json
{
  "@odata.type": "#MemoryRegion.v1_1_0.MemoryRegion",
  "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/MemoryRegions/0",
  "Id": "0",
  "Name": "DC Region 0",
  "RegionType": "Dynamic",
  "RegionNumber": 0,
  "RegionBaseOffsetMiB": 16384,
  "RegionSizeMiB": 32768,
  "BlockSizeMiB": 256,
  "ShareableRegion": false,
  "NonVolatileRegion": false,
  "HardwareManagedCoherencyRegion": false,
  "ReadOnlyRegion": false,
  "InterconnectSpecificDCManagement": true,
  "SanitizeOnRelease": true,
  "ExtentsCount": 1,
  "MemoryExtents": [
    {
      "Pending": false,
      "ExtentLink": { "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0x400000000" }
    }
  ],
  "MemoryChunks": [
    {
      "ChunkOffsetMiB": 0,
      "ChunkLink": { "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/MemoryDomains/cxl_dcd0_domain0/MemoryChunks/dc_chunk0" }
    }
  ],
  "Status": { "State": "Enabled", "Health": "OK" }
}
```

### Set DC Region Configuration (5602h)

`PATCH` on a `MemoryRegion`. Only `BlockSizeMiB` and `SanitizeOnRelease` are
writable; anything else in the body gets `PropertyNotWritable`.

```c
int cxlmi_cmd_fmapi_set_dc_region_config(struct cxlmi_endpoint *ep,
                                         struct cxlmi_tunnel_info *ti,
                                         struct cxlmi_cmd_fmapi_set_dc_region_config_req *in);
```

| Request field | From |
|---------------|------|
| `region_id` | URI `{region}` |
| `block_sz` | `BlockSizeMiB` * 2^20; must be a bit set in the region's supported mask (5600h) |
| `sanitize_on_release` | `SanitizeOnRelease`; writable only if the region's bit is set in `sanitize_on_release_config_mask` |

Validation before issuing the command:

- The region must have no extents allocated to any host (`ExtentsCount == 0`
  across all LDs that see it). The device rejects it otherwise, but a
  `409 Conflict` with `ResourceInUse` is a better error than the generic
  CCI failure.
- Omitted properties are re-sent with their current values; 5602h has no
  "leave unchanged" encoding.

```bash
curl -X PATCH .../CXLLogicalDevices/0/MemoryRegions/0 \
  -H 'Content-Type: application/json' \
  -d '{"BlockSizeMiB": 512, "SanitizeOnRelease": false}'
```

### Get DC Region Extent Lists (5603h)

Read once per host; the result populates the device-wide `MemoryExtents`
collection and each region's `MemoryExtents[]` link array. Page with
`start_ext_index`/`extent_count` until `extents_returned` accumulates to
`total_extents`; restart if `list_generation_num` changes between pages.

```c
int cxlmi_cmd_fmapi_get_dc_region_ext_list(struct cxlmi_endpoint *ep,
                                           struct cxlmi_tunnel_info *ti,
                                           struct cxlmi_cmd_fmapi_get_dc_region_ext_list_req *in,
                                           struct cxlmi_cmd_fmapi_get_dc_region_ext_list_rsp *ret);
```

| Response field | Redfish `MemoryExtent` property |
|----------------|--------------------------------|
| `extents[i].start_dpa` | `ExtentOffsetMiB` (device address range, bytes / 2^20); also selects the owning region by range |
| `extents[i].len` | `ExtentSizeMiB` |
| `extents[i].tag` | `Tag`, 16 bytes rendered as a UUID string (`8-4-4-4-12`); an all-zero tag is rendered as the empty string |
| `extents[i].shared_seq` | `SequenceNumber` |
| `host_id` | adds the owning host's `CXLLogicalDevice` region to `Links/MemoryRegions[]` |

```json
{
  "@odata.type": "#MemoryExtent.v1_0_0.MemoryExtent",
  "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0x400000000",
  "Id": "ext_0x400000000",
  "Name": "DC Extent",
  "ExtentOffsetMiB": 16384,
  "ExtentSizeMiB": 4096,
  "Tag": "6f1a2c3e-0000-4000-8000-000000000001",
  "SequenceNumber": 0,
  "FMReferencedExtent": false,
  "MemoryChunks": [
    {
      "ChunkOffsetMiB": 0,
      "ChunkLink": { "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/MemoryDomains/cxl_dcd0_domain0/MemoryChunks/dc_chunk0" }
    }
  ],
  "Links": {
    "MemoryRegions": [
      { "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/CXLLogicalDevices/0/MemoryRegions/0" }
    ]
  },
  "Status": { "State": "Enabled", "Health": "OK" }
}
```

### Initiate Dynamic Capacity Add (5604h)

`POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd`

```c
int cxlmi_cmd_fmapi_initiate_dc_add(struct cxlmi_endpoint *ep,
                                    struct cxlmi_tunnel_info *ti,
                                    struct cxlmi_cmd_fmapi_initiate_dc_add_req *in);
```

| Request field | Encoding | Action parameter |
|---------------|----------|------------------|
| `host_id` | see [Host ID Resolution](#host-id-resolution) | derived from `{ld}`, not a parameter |
| `selection_policy` | 0 Free, 1 Contiguous, 2 Prescriptive, 3 Enable Shared Access | `Policy` (required) |
| `region_num` | region index | `RegionNumber` (required for Free, Contiguous, EnableSharedAccess) |
| `length` | bytes, block-size multiple | `Length` in MiB (required for Free, Contiguous); the schema leaves the unit unannotated, cxl-fmd treats it as MiB like every other size property |
| `tag` | 16 bytes | `Tag` UUID string (required for Free, Contiguous, EnableSharedAccess) |
| `ext_count` + `extents[]` | DPA/length/tag/shared_seq per extent | `ExtentList[]` of `MemoryExtent` links (required for Prescriptive); cxl-fmd resolves each link back to its cached DPA, length and tag |

Policy semantics (CXL r3.2 7.6.7.6.5):

- **Free**: the device picks any blocks in `RegionNumber` totalling `Length`.
- **Contiguous**: same, but a single DPA-contiguous extent.
- **Prescriptive**: the FM names the exact extents. The extents in
  `ExtentList` must already exist (for example created on another host with
  a shared tag) or be representable as device-free space; cxl-fmd should
  only accept links to existing `MemoryExtent` resources.
- **EnableSharedAccess**: give this host access to the extents already
  tagged `Tag` in `RegionNumber`; `Length` is ignored.

```bash
curl -X POST .../CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Free", "RegionNumber": 0, "Length": 4096,
       "Tag": "6f1a2c3e-0000-4000-8000-000000000001"}'
```

Validation: `Policy` must be in `AddCapacityPoliciesSupported`; `Length`
must be a multiple of the region's `BlockSizeMiB` and not exceed
`RegionSizeMiB` minus allocated extents; `AvailableExtents` and
`AvailableTags` must be non-zero. On success rescan 5601h/5603h for that
host so the new extent shows up as `Pending` until the host's Add Capacity
Response arrives.

### Initiate Dynamic Capacity Release (5605h)

`POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease`

```c
int cxlmi_cmd_fmapi_initiate_dc_release(struct cxlmi_endpoint *ep,
                                        struct cxlmi_tunnel_info *ti,
                                        struct cxlmi_cmd_fmapi_initiate_dc_release_req *in);
```

| Request field | Encoding | Action parameter |
|---------------|----------|------------------|
| `host_id` | derived | from `{ld}` |
| `flags[3:0]` | removal policy: 0 Tag-based, 1 Prescriptive | `Policy` (required) |
| `flags[4]` | Forced Removal | `ForcedRemoval` (required). `true` means the device does not wait for the host's Release Dynamic Capacity; the host loses access immediately. |
| `flags[5]` | Sanitize on Release | `SanitizeOnRelease` (required); only honoured where the region allows it |
| `length` | bytes | `Length` MiB (required for TagBased) |
| `tag` | 16 bytes | `Tag` (required for TagBased) |
| `ext_count` + `extents[]` | | `ExtentList[]` (required for Prescriptive) |

```bash
curl -X POST .../CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Prescriptive", "ForcedRemoval": false, "SanitizeOnRelease": true,
       "ExtentList": [
         {"@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0x400000000"}
       ]}'
```

`DELETE .../MemoryExtents/{ext}` is sugar for a Prescriptive release of that
one extent from every host holding it, with `ForcedRemoval = false` and
`SanitizeOnRelease` taken from the owning region. This mirrors how `DELETE`
on a `MemoryChunk` already maps to Set LD Allocations. An extent with
`FMReferencedExtent = true` survives the release of its last host with an
empty `Links/MemoryRegions`; only `CXLReferenceDCMemory` with
`FMReference = false` frees it, and a `DELETE` on such an orphan returns
`409 Conflict`.

### Dynamic Capacity Add Reference (5606h) / Remove Reference (5607h)

`POST .../PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory`

```c
int cxlmi_cmd_fmapi_dc_add_reference(struct cxlmi_endpoint *ep,
                                     struct cxlmi_tunnel_info *ti,
                                     struct cxlmi_cmd_fmapi_dc_add_ref_req *in);
int cxlmi_cmd_fmapi_dc_remove_reference(struct cxlmi_endpoint *ep,
                                        struct cxlmi_tunnel_info *ti,
                                        struct cxlmi_cmd_fmapi_dc_remove_ref_req *in);
```

| Action parameter | Maps to |
|------------------|---------|
| `Tag` | the 16-byte tag in both requests |
| `FMReference` = `true` | 5606h: the FM holds a reference, so tagged shared capacity survives every host releasing it and cannot be sanitized, freed or reallocated |
| `FMReference` = `false` | 5607h: drop the FM reference; if no host references remain the capacity is freed |

Both commands are device-wide (no Host ID), which is why the action lives on
`PCIeDevice`. After either, refresh 5608h so `FMReferencedExtent` on every
extent carrying that tag is updated.

```bash
curl -X POST .../PCIeDevices/cxl_dcd0/Actions/PCIeDevice.CXLReferenceDCMemory \
  -H 'Content-Type: application/json' \
  -d '{"Tag": "6f1a2c3e-0000-4000-8000-000000000001", "FMReference": true}'
```

### Dynamic Capacity List Tags (5608h)

No dedicated Redfish resource. Read on discovery and after any 5604h-5607h
to decorate extents.

```c
int cxlmi_cmd_fmapi_dc_list_tags(struct cxlmi_endpoint *ep,
                                 struct cxlmi_tunnel_info *ti,
                                 struct cxlmi_cmd_fmapi_dc_list_tags_req *in,
                                 struct cxlmi_cmd_fmapi_dc_list_tags_rsp *ret);
```

| Response field | Encoding | Redfish |
|----------------|----------|---------|
| `generation_num` | changes whenever the tag list changes | restart paging on change |
| `validity_bitmap` | bit 0: `ref_bitmap` valid, bit 1: `pending_ref_bitmap` valid | gate the two mappings below |
| `tags_list[i].flags` | bit 0: FM reference held | `MemoryExtent.FMReferencedExtent` on every extent with this tag |
| `tags_list[i].ref_bitmap` | 256-bit, one bit per Host ID that has accepted the tag | no standard property (Comment 4); could feed `Links/MemoryRegions[]` on the extent |
| `tags_list[i].pending_ref_bitmap` | hosts with an add outstanding | `MemoryRegion.MemoryExtents[].Pending` for that host's region |

---

## Host ID Resolution

FM-API DCD commands address a host by a 16-bit Host ID that is local to the
DCD. Redfish never exposes it; cxl-fmd derives it from the
`CXLLogicalDevice` the request is made on:

| Device class | Host ID |
|--------------|---------|
| SLD DCD | 0 |
| MLD DCD | the LD-ID (`fmd_ld.ld_id`) |
| MH-SLD / MH-MLD DCD | the pool LD-ID (`fmd_ld.ld_id`): the FM-API already identifies MH-device LDs by their index in the device-wide pool (Start LD ID and LD Map in 5500h), and 56xxh follows that numbering. The per-head LD-ID a host sees is not used |

The 56xxh set is serviced by the FM-owned LD, so the host is always named by
the Host ID in the payload, never by the tunnel. For switch-attached devices
the command is tunnelled with `DEFINE_CXLMI_TUNNEL_SWITCH(ti, port)` exactly
as the existing 54xxh MLD commands are (see `dcd_tunnel()` in `src/dcd.c`).
The two-level `DEFINE_CXLMI_TUNNEL_SWITCH_MLD(ti, port, ld)` tunnel reaches an
individual LD's mailbox, which does not service these commands.

The `Systems/{host}` that ultimately receives the capacity is found through
the existing `Connection` for that LD; no new routing is needed.

---

## Proposed cxl-fmd Changes

### Schema bundle

The DSP8010 zip on dmtf.org sits behind a bot challenge, so the files that
changed for DCD (`MemoryExtent`, `MemoryExtentCollection`, `MemoryRegion
v1.1.0`, `CXLLogicalDevice v1.5.0`, `PCIeDevice v1.23.0`, `MemoryChunks
v1.7.0`) were fetched individually from `https://redfish.dmtf.org/schemas/v1/`
and installed with their symlinks. `scripts/update_schemas.py` lists the
MemoryExtent schemas in its defaults so a future bundle update keeps them.
Handlers emit `MemoryRegion.v1_1_0`, `MemoryExtent.v1_0_0`,
`CXLLogicalDevice.v1_5_0` and `PCIeDevice.v1_23_0`.

### Data model (`include/cxl-fmd.h`)

```c
#define FMD_MAX_DC_REGIONS 8
#define FMD_DC_TAG_LEN     16

/* Device-wide DCD capabilities, from Get DCD Info (5600h). */
struct fmd_dcd_info {
	uint8_t  num_hosts;
	uint8_t  num_regions;
	uint16_t add_policies;              /* bit 0 Free, 1 Contiguous, 2 Prescriptive, 3 Shared */
	uint16_t release_policies;          /* bit 0 Tag-based, 1 Prescriptive */
	uint8_t  sanitize_cfg_mask;         /* bit N: region N sanitize-on-release configurable */
	uint64_t total_dc_capacity;         /* bytes */
	uint64_t blk_sz_mask[FMD_MAX_DC_REGIONS];
};

/* One host's view of a DC region, from 5601h. Child of fmd_ld (or fmd_sld). */
struct fmd_dc_region {
	struct fmd_component base;
	uint8_t  region_id;
	uint16_t host_id;
	uint64_t base_dpa;
	uint64_t decode_len;
	uint64_t region_len;
	uint64_t block_size;
	uint8_t  flags;
	bool     sanitize_on_release;
	uint32_t extent_count;
};

/* Device-wide extent, from 5603h decorated with 5608h. Child of the Type 3 device. */
struct fmd_dc_extent {
	struct fmd_component base;
	uint64_t start_dpa;
	uint64_t len;
	uint8_t  tag[FMD_DC_TAG_LEN];
	uint16_t shared_seq;
	uint8_t  region_id;
	uint16_t host_id;                   /* host this extent was listed for */
	bool     pending;                   /* host has not accepted yet */
	bool     fm_referenced;
};
```

- Add `struct fmd_dcd_info *dcd` to `fmd_sld`, `fmd_mld` and `fmd_mhd`
  (`NULL` when 5600h is not in the CEL). `fmd_mhd.dynamic_capacity` becomes
  `dcd != NULL`.
- Add `FMD_COMP_DC_REGION` and `FMD_COMP_DC_EXTENT` to
  `enum fmd_component_type`, plus `fmd_topology_find_dc_region()` /
  `fmd_topology_find_dc_extent()` lookups keyed the same way as chunks.
- Per-LD counters from the 5601h trailer live on `fmd_ld` (`extents_supported`,
  `extents_available`, `tags_supported`, `tags_available`).

### Discovery (`src/discovery.c`)

After the existing Type 3 classification:

1. If the CEL has 5600h: `cxlmi_cmd_fmapi_get_dcd_info()`.
2. For each Host ID the device can see (SLD: 0; MLD and MHD: each LD):
   5601h then 5603h, paging until complete.
3. 5608h once, device-wide, to fill `pending` and `fm_referenced`.
4. Re-run steps 2-3 for the affected host after every successful
   5602h/5604h/5605h, and step 3 after 5606h/5607h.

### Redfish (`src/redfish/server.c`, `handlers.c`)

New routes, in the order the existing `match_route()` chain expects:

```
GET    /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents
GET    /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}
DELETE /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}
POST   /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory
GET    /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions
GET    /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}
PATCH  /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}
POST   /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd
POST   /redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease
```

Plus the additive properties on existing handlers: `CXLDevice/DynamicCapacity`
and `Actions` on `PCIeDevice`; `MemoryRegions`, the four counters and
`Actions` on `CXLLogicalDevice`; `Links/MemoryExtent` on `MemoryChunks`.
Tag conversion (16 bytes to UUID string and back) belongs in
`cci_redfish_map.c` next to the existing enum mappers.

### Mock topology (`src/mock.c`, `docs/mock-topology.md`)

A `dcd` object on any Type 3 device, so all of the above can be exercised
and validated without hardware. The mock host accepts added capacity
immediately, so `Pending` is only ever `true` for extents declared pending in
the topology file:

```json
"dcd": {
    "num_hosts": 1,
    "add_policies": ["Free", "Contiguous", "Prescriptive"],
    "release_policies": ["TagBased", "Prescriptive"],
    "regions": [
        {
            "region_id": 0,
            "base_mb": 16384,
            "decode_len_mb": 32768,
            "region_len_mb": 32768,
            "block_size_mb": 256,
            "supported_block_sizes_mb": [256, 512, 1024],
            "sanitize_on_release": true,
            "sanitize_configurable": true,
            "shareable": false,
            "nonvolatile": false
        }
    ],
    "extents": [
        {
            "host_id": 0,
            "start_mb": 16384,
            "len_mb": 4096,
            "tag": "6f1a2c3e-0000-4000-8000-000000000001",
            "shared_seq": 0,
            "pending": false
        }
    ]
}
```

Mock add/release mutate the extent list in memory the way the `MemoryChunks`
POST/DELETE path does; `tests/test_dcd.py` covers the lifecycle (add Free,
Contiguous and across gaps, release TagBased with trimming and Prescriptive,
region reconfiguration, reference/unreference a tag, shared access).

---

## Open Questions

1. **Units of the `Length` action parameter.** The 2026.1 schema does not
   annotate it with `MiBy`. cxl-fmd assumes MiB. This is worth raising with
   the Redfish Forum together with the missing DCD rows in DSP0288.
2. **Task support.** Comment 5 above. The completion signal now exists:
   the DC event drain (item 3) clears the host's bit in the extent's
   `pending_mask` when the host accepts or the add is withdrawn, so a
   `Task` could be moved to `Completed` or `Exception` from that path
   without polling the device. What is missing is the `TaskService`
   plumbing itself. Until it lands the actions return `204 No Content` at
   "device accepted the request", and clients have to watch the extent's
   `Pending` property to learn when the host accepted the capacity.
3. **Events.** Hosts learn about add/release through DC event records on
   their own mailbox. The FM sees the FM-side records (types 04h/05h) by
   reading Get Event Records on the FM-owned LD; cxl-fmd does this on a
   timer (`--event-poll`) and early on MCTP Event Notification, and the
   records drive the `Pending` transition instead of a rescan.
4. **GFD.** The GFAM equivalents (4907h-490Bh) share the same shapes but
   address the device via PBR. Out of scope here and listed separately in
   `TODO`.
