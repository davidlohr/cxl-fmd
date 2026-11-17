# Dynamic Capacity Device (DCD) FM-API to Redfish Mapping

This document specifies how cxl-fmd exposes the CXL Fabric Management API
(FM-API) **DCD Management command set (56h)** through Redfish. The mapping is
implemented in `src/dcd.c` (FM-API and mock state) and
`src/redfish/dcd_handlers.c` (Redfish). It is exercised by `tests/test_dcd.py`
against `mock/dcd-device.json`, `mock/dcd-shared-perhost.json` and
`mock/dcd-prescriptive-release.json`, and on the FM-API side by the fake
endpoints of `tests/discovery-tests.c`.

Two kinds of statement are kept apart below. What the CXL specification
says is quoted, with its section or table (CXL r4.0). What cxl-fmd chose,
where the specification or the DMTF material is silent, is labelled
*cxl-fmd's choice* (or *the mock's choice* for `--mock` only). The choices
are gathered in [Open Questions](#open-questions).

It fills a gap in the published DMTF material, which was checked on
2026-10-21:

| Source | What it says about DCD |
|--------|------------------------|
| [DSP0288 v1.4.0](https://www.dmtf.org/sites/default/files/standards/documents/DSP0288_1.4.0.pdf) (2026-08-07), *CXL to Redfish Mapping* | Table 2 (FM-API) maps only the Physical Switch (51h), Virtual Switch (52h), MLD Component (54h) and *Get PBR Link Partner Info* commands. No 55h (MHD) and no 56h (DCD) command is mapped. The spec still normatively references CXL r3.0. A request to DMTF to add them is drafted, to be submitted by the maintainer (Open Question 26). |
| [DSP8010 2026.1](https://redfish.dmtf.org/schemas/v1/) | Adds everything needed for the write path (earlier releases had `MemoryRegion v1.0.3` with inline extents and `PCIeDevice` `CXLDevice/DynamicCapacity`, but no way to add or release capacity): a standalone `MemoryExtent` resource, `MemoryRegion v1.1.0`, `CXLLogicalDevice v1.4.0` with `DynamicCapacityAdd` / `DynamicCapacityRelease` actions and extent/tag counters, `PCIeDevice v1.22.0` with the `CXLReferenceDCMemory` action, the `EnableSharedAccess` policy and a `MemoryExtents` collection link, and `MemoryChunks v1.7.0` with `Links/MemoryExtent`. |
| [DSP8010 2026.2](https://redfish.dmtf.org/schemas/v1/) (bundle vendored in `schema/dmtf`) | `PCIeDevice v1.23.0` adds `CXLDevice/DeviceClass` (`SLD`, `MLD`, `MH_SLD`, `MH_MLD`). Not DCD-specific but directly useful to cxl-fmd. |

So the JSON schema has everything the DCD surface needs and the vendored
bundle carries it, but DSP0288 has not caught up with it. The mapping
below is therefore cxl-fmd's profile of those schemas, not a DMTF
mapping: where it puts a 56h field, and in what unit, is cxl-fmd's
choice, and it may differ from what DSP0288 maps when it covers DCD. Everything below
follows the 2026.1 model, with the FM-API field semantics taken from CXL r4.0
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
| Get DC Region Extent Lists (5603h) | Extent Start DPA | `MemoryExtent` | `ExtentOffsetMiB`, in the DPA space of the host whose list has the extent (Comment 2) | Yes |
| Get DC Region Extent Lists | Extent Length | `MemoryExtent` | `ExtentSizeMiB` | Yes |
| Get DC Region Extent Lists | Extent Length, summed over the host's extents | `CXLLogicalDevice` | `MemorySizeMiB`: the LD's static capacity plus this sum, in bytes, divided into MiB once. The schema says it "shall equate to the sum of the dynamic capacity extents and the static capacity assigned to this logical device" (`CXLLogicalDevice_v1.xml`). The static capacity is Total Capacity of Identify Memory Device for an SLD and the LD allocation (5401h) for an MLD or Multi-Headed device; that neither has the dynamic capacity in it is cxl-fmd's reading, as CXL does not say. Every extent 5603h lists counts (Comment 5) | Yes |
| Get DC Region Extent Lists | Extent Tag | `MemoryExtent` | `Tag`; left out when all zero | Yes |
| Get DC Region Extent Lists | Shared Extent Sequence | `MemoryExtent` | `SequenceNumber` | Yes |
| Get DC Region Extent Lists | Host ID of the request | `MemoryExtent` | the `<ld>` of the Id `ext_<ld>_<hex DPA>`, and the one link of `Links/MemoryRegions` | Yes |
| Get DC Region Extent Lists | Extent List ("for the specified host", all regions) | `MemoryRegion` | `ExtentsCount`, `MemoryExtents[].ExtentLink`: the host's extents in that region, counted by cxl-fmd | Yes |
| Get DC Region Extent Lists | Extent List Generation Number | `MemoryExtent` | not shown; a read whose generation keeps changing leaves the host's extents as they were, `Status.Health` `Warning` (Comment 6) | Yes |
| Initiate Dynamic Capacity Add (5604h) | All | `CXLLogicalDevice` | `DynamicCapacityAdd` action | Yes |
| Initiate Dynamic Capacity Release (5605h) | All | `CXLLogicalDevice` | `DynamicCapacityRelease` action | Yes |
| Initiate Dynamic Capacity Release | Tag-based or Prescriptive, from one host | `MemoryExtent` | DELETE (cxl-fmd's choice): of an extent with a tag in a Sharable region, every extent of that host with the tag goes, see [DELETE](#delete-of-a-memoryextent) | Yes |
| Dynamic Capacity Add Reference (5606h) | Tag | `PCIeDevice` | `CXLReferenceDCMemory` action, `FMReference` = `true` | Yes |
| Dynamic Capacity Remove Reference (5607h) | Tag | `PCIeDevice` | `CXLReferenceDCMemory` action, `FMReference` = `false` | Yes |
| Dynamic Capacity List Tags (5608h) | Generation Number | `PCIeDevice` | `Oem/Cxl/DynamicCapacityTags/Generation` (Comment 4) | Yes |
| Dynamic Capacity List Tags | Validity Bitmap | `PCIeDevice` | whether `AcceptedLDs` and `PendingLDs` are given (Comment 4) | Yes |
| Dynamic Capacity List Tags | Tag | `PCIeDevice` | `Oem/Cxl/DynamicCapacityTags/Tags[].Tag` | Yes |
| Dynamic Capacity List Tags | Flags Bit[0]: FM Holds Reference | `MemoryExtent`, `PCIeDevice` | `FMReferencedExtent` on every extent with the tag; `Tags[].FMHoldsReference` | Yes |
| Dynamic Capacity List Tags | Reference Bitmap | `PCIeDevice` | `Tags[].AcceptedLDs` (Comment 4) | Yes, Oem |
| Dynamic Capacity List Tags | Pending Reference Bitmap | `PCIeDevice` | `Tags[].PendingLDs`, per tag; not `MemoryExtents[].Pending`, which is `false` for every extent 5603h lists (Comment 5) | Yes, Oem |
| Dynamic Capacity Event Record (via Get Event Records) | Event Types 04h (Add Capacity Response) and 05h (Capacity Released) | `LogEntry` | `CXLEntryType` = `DynamicCapacity`, in the `DeviceLog` of the host's LD; not for Multi-Headed devices | Yes |
| Dynamic Capacity Event Record, 04h and 05h | Number of Available Extents (78h), Number of Available Tags (7Ch), when Validity Flags Bit[0] is set | `CXLLogicalDevice` | `AvailableExtents`, `AvailableTags` of the host the record names | Yes |

### Comments for Table 3

**Comment 1:** DC regions are per host. The `MemoryRegion` collection hangs
off the `CXLLogicalDevice` that the host is bound to, so the same physical
region on a multi-host DCD appears once under each LD that can see
it, with the host-specific `RegionBaseOffsetMiB` reported by 5601h for that
Host ID. "The DCD controls which DPA range it assigns to each region for
each host" (Section 9.13.3): Region Base and Region Decode Length are kept
for each host as its 5601h reports them, and may differ from host to host.
Region Block Size and Sanitize on Release are what 5602h sets for the
region, with no Host ID, so a `PATCH` on one LD's region changes every
LD's view of it (see Set DC Region Configuration).

**Comment 2:** An extent is one host's. 5603h returns the "Extent list for
the specified host" (Table 7-71), and "The DPA ranges exposed by the device
to each host are independent of one another" (Section 9.13.3). cxl-fmd
keeps each entry of a host's list as one extent, a child of the Type 3
device, keyed by Host ID and Starting DPA: two hosts that list the same
DPA have two extents, each with its own length, tag, sequence and region,
and a refresh of one host adds, updates and drops that host's extents
only. Each `MemoryExtent` is one such extent (cxl-fmd's choice: the schema
does not say whether a `MemoryExtent` is one host's or the capacity all
its hosts share). Its `ExtentOffsetMiB` is in the DPA space of the host
whose extent it is, and its one `Links/MemoryRegions` link is that host's
region.

Capacity several hosts share is one extent of each host, with the same
Tag: "Within sharable regions, all extents targeting the same range this
field is required and all Tag values shall be identical for all hosts
sharing the same extent" (Table 8-230), and the Shared Extent Sequence is
"The relative order that hosts place this extent within the virtual
address space" (Table 8-230). cxl-fmd never takes two hosts' extents for
the same capacity because they have the same DPA; the specification says
nothing of where shared capacity is in each host's DPA space. Which hosts
have a tag is given by the device's tag list, in the `PCIeDevice`'s
`Oem/Cxl/DynamicCapacityTags` (Comment 4).

The extent's region is worked out again on each refresh, from the host's
own regions, bounded by the Region Decode Length: "All extents for this
region shall describe a memory range ≥ the Region Base and < the Region
Base + Region Decode Length" (Table 8-347). An extent in none of the
host's regions is kept, with a warning in the log, no
`Links/MemoryRegions` and `Status.Health` `Warning`.

Capacity an FM reference keeps after every host has released it ("The
tagged capacity will remain allocated, and contents will be preserved
even if all DCD Extents that reference it are removed", Section
7.6.7.6.7) is in no host's Extent List, so it is no `MemoryExtent`: it is
a row of the tag list only, in the mock as on hardware.

A `MemoryRegion` refers to its extents through
`MemoryExtents[].ExtentLink`; the inline
`ExtentOffsetMiB`/`ExtentSizeMiB`/`Tag`/`SequenceNumber` members are
deprecated since `MemoryRegion v1.1.0` and are not emitted.

**Comment 3:** FM-API DC lengths and offsets are bytes; a Length is
"Always a multiple of the configured Region Block Size" (Table 7-72).
Redfish uses MiB throughout, and cxl-fmd multiplies by 2^20. A Free or
Contiguous add whose Length is not a multiple of the region's block size is
refused with `400 Bad Request` before anything is sent, in the mock and on
hardware; nothing is rounded. The DMTF properties are whole MiB, so a
block size below 1 MiB (Table 7-65 allows any power of two from 2^6 bytes)
is not listed in `MemoryBlockSizesSupported`, shows as `BlockSizeMiB` 0,
and cannot be chosen with PATCH (a known limitation), and an extent's or
region's offset or size that is not whole MiB is rounded down. Each of
them is therefore given in bytes too, in cxl-fmd's Oem (its Oem schema,
`Cxl_v1.xml`; review F12). A request to DMTF for a standard way to give
them is drafted, to be submitted by the maintainer (Open Question 25):

| Resource | MiB property | `Oem.Cxl` property, bytes |
|----------|--------------|---------------------------|
| `PCIeDevice` | `CXLDevice/DynamicCapacity/TotalDynamicCapacityMiB` | `DynamicCapacity/TotalDynamicCapacityBytes` |
| `PCIeDevice` | `CXLDevice/DynamicCapacity/MemoryBlockSizesSupported[]/BlockSizeMiB` (1 MiB and up) | `DynamicCapacity/MemoryBlockSizesSupported[]/BlockSizeBytes` (every bit of the mask) |
| `MemoryRegion` | `RegionBaseOffsetMiB`, `RegionSizeMiB`, `BlockSizeMiB` | `RegionBaseOffsetBytes`, `RegionSizeBytes`, `BlockSizeBytes` |
| `MemoryExtent` | `ExtentOffsetMiB`, `ExtentSizeMiB` | `ExtentOffsetBytes`, `ExtentSizeBytes` |

`MemorySizeMiB` of a `CXLLogicalDevice` has none.

Errors are a Redfish error object whose `code` is always
`Base.1.24.GeneralError`, whose `message` says what was wrong, and whose
`@Message.ExtendedInfo` has a message of the DMTF Base registry: for these
requests `MalformedJSON` for a body that is not JSON, `ResourceInUse`
(`409`, `503`), `ResourceExhaustion` (`409` for Resources Exhausted),
`InvalidURI` (`404` for a URI that is not a resource), `ActionNotSupported`
(`501` on an action), or else `GeneralError` with a `Resolution` of
cxl-fmd's (see "Error Response Format" in Redfish-API.md). The HTTP status
says what kind of failure it is. When the device refuses a
command, its return code decides it: Invalid Input, Invalid Extent List and
Invalid Physical Address are `400`, Resources Exhausted is `409`, Retry
Required and Busy are `503` (with no `Retry-After`), Unsupported from 5602h
is `409`; any other return code is `500`. A command libcxlmi could not
carry, or whose response it could not read, is `500` with a message that
starts "Outcome unknown": libcxlmi gives the same result for a request not
sent, one not answered, and a response it could not read, so the device
may have carried it out; the host (every host for 5602h, the tag list for
5606h and 5607h) is read again on the next `--event-poll` tick, and the
client is to check before sending it again.

A command the device answered Success for is not a failure, even when the
read after it fails: 5604h "shall complete when the device initiates the
Add Capacity procedure" (Section 7.6.7.6.5), 5605h "when the device
initiates the Remove Capacity procedure" (Section 7.6.7.6.6). An add, a
release or a `DELETE` of an extent is then `202 Accepted` with its `Task`,
as when the read succeeds, and the task's message says the host could not
be read back after it (see [Tasks of an add or a
release](#tasks-of-an-add-or-a-release)): a Free, Contiguous or Tag-based
request sent again is another add or release. A `PATCH` of a region
(5602h) and `CXLReferenceDCMemory` (5606h, 5607h) are then `200 OK`, with
an error object whose `code` is `Base.1.24.Success`, with
`Base.1.24.Success` in its `@Message.ExtendedInfo`, and whose `message`
says the device accepted the request, that what is shown is stale until it
is read again, and not to send the request again. A 5603h list that keeps
changing as it is read is such a read, not `503`. The host is shown
`Warning` and read again on the next `--event-poll` tick. That the answer
is `200` with this message, and that a PATCH answers it too, where
DSP0266 says it "should" return the resource or `204`, is cxl-fmd's
choice.

**Comment 4:** DMTF has no resource for a tag, and no property for the LDs
that have accepted a tag or been offered it. cxl-fmd shows the device's
5608h list on the `PCIeDevice` of a DCD, as
`Oem/Cxl/DynamicCapacityTags` (next to `DynamicCapacity`, the sizes of
Comment 3 in bytes). The view and its names are cxl-fmd's choice. Its type, `#Cxl.v1_0_0.PCIeDevice`, is in cxl-fmd's Oem schema,
`Cxl_v1.xml` (see "Oem schema" in [Redfish-API.md](Redfish-API.md)).

```json
"Oem": {
  "Cxl": {
    "@odata.type": "#Cxl.v1_0_0.PCIeDevice",
    "DynamicCapacity": {
      "TotalDynamicCapacityBytes": 12884901888,
      "MemoryBlockSizesSupported": [
        { "RegionNumber": 0, "BlockSizeBytes": [268435456] },
        { "RegionNumber": 1, "BlockSizeBytes": [268435456] },
        { "RegionNumber": 2, "BlockSizeBytes": [268435456, 536870912] }
      ]
    },
    "DynamicCapacityTags": {
      "TagsKnown": true,
      "Generation": 7,
      "Tags": [
        {
          "Tag": "6f1a2c3e-0000-4000-8000-0000000000c1",
          "FMHoldsReference": false,
          "AcceptedLDs": [0, 1],
          "PendingLDs": [],
          "Extents": [
            { "@odata.id": "/redfish/v1/Chassis/cxl_perhost_mld0/PCIeDevices/cxl_perhost_mld0/MemoryExtents/ext_0_200000000" },
            { "@odata.id": "/redfish/v1/Chassis/cxl_perhost_mld0/PCIeDevices/cxl_perhost_mld0/MemoryExtents/ext_1_400000000" }
          ]
        }
      ]
    }
  }
}
```

| Property | From |
|----------|------|
| `TagsKnown` | whether the last 5608h read of the device completed. `false` when it failed, or the device answered Unsupported; `Tags` is then left out, not empty, as an empty list would say that no LD has an add outstanding (cxl-fmd's choice) |
| `Generation` | Generation Number of the last complete read, "Generation number of the tags list. This number shall change every time the remainder of the command's payload would change" (Table 7-77) |
| `LastRefreshed` | the time of cxl-fmd (UTC) at which the last complete read ended; served while `TagsKnown` is `false` too, so that a client can tell how old the last list is. Absent until a read completes, and when the device answered Unsupported (cxl-fmd's) |
| `Tags[].Tag` | the Tag, as a UUID string |
| `Tags[].FMHoldsReference` | Flags Bit[0] |
| `Tags[].AcceptedLDs` | the LD-IDs whose bit is set in the Reference Bitmap: "Each 1 indicates an LD that has accepted the capacity associated with this tag" (Table 7-78). Given only when the Validity Bitmap has "Reference Bitmaps Valid" set |
| `Tags[].PendingLDs` | the LD-IDs whose bit is set in the Pending Reference Bitmap: "Each 1 indicates an LD for which the tagged capacity has been added with no host response yet" (Table 7-78). Given only when "Pending Reference Bitmaps Valid" is set |
| `Tags[].Extents` | links to every host's `MemoryExtent` with the tag |

The LD-IDs are the bitmaps' own: "Bit[0] of the first byte represents LD
0, and bit[7] of the last byte represents LD 255" (Table 7-78). What no
`MemoryExtent` can show is here: capacity an FM reference keeps that no
host holds (`AcceptedLDs` empty, no `Extents`), and an add an LD has not
answered that its 5603h does not list (the LD in `PendingLDs`, no
`Extents` of that LD).

**Comment 5:** `MemoryExtents[].Pending` of a `MemoryRegion` is `false`,
and `Status.State` of a `MemoryExtent` `Enabled`, for every extent the
host's 5603h lists (cxl-fmd's choice). An add an LD has not answered is
shown only in the tag view, as `PendingLDs` (Comment 4). Why:

- The Pending Reference Bitmap is per tag: "Each 1 indicates an LD for
  which the tagged capacity has been added with no host response yet"
  (Table 7-78). A host can hold capacity it accepted and an add it has not
  answered under one tag, its LD in both bitmaps; which of its extents, if
  any, is the add, the bitmap does not say. So no extent is made pending
  from it.
- Section 9.13.3 says of the Extent List a host sees that it "does not
  contain extents that are still pending acceptance from the host via the
  Add Dynamic Capacity Response command". 5603h says only that its list is
  the "Extent list for the specified host as defined in Table 8-230"
  (Table 7-71), and does not say that it is the same list. cxl-fmd takes
  an extent 5603h lists not to be pending, as Section 9.13.3 says of the
  host's list; that 5603h is that list is cxl-fmd's reading, not the
  specification's. No extent is made up from the 5604h request or from
  the tag list.
- When the tag list could not be read (a 5608h that failed, or kept
  changing as it was read), whether an add is outstanding is not known:
  `TagsKnown` is `false`, `Tags` is left out, and the `MemoryRegion`s of
  the device and its `MemoryExtent`s with a tag have `Status.Health`
  `Warning`. The listed extents stay `Pending` `false`, which does not
  depend on the tag list. A device that answers Unsupported (5608h is
  "DCO", "Optional for DCDs", in Table 8-397) has no tag list to fail:
  `TagsKnown` `false`, no `Tags`, and no `Warning`; on it an add a host
  has not answered cannot be seen at all.
- 5604h and 5605h complete when the device initiates the procedure; the
  host answers after, which the FM may learn from a DC event record, an
  Add Capacity Response (04h) or a Capacity Released (05h). The
  specification does not say whether the device sends 04h for a host that
  accepts no capacity, nor 05h after a Forced Removal, so the records give
  no dependable completion signal. An add or a release is therefore
  answered `202 Accepted` with a `Task`, which follows the host's 5603h
  and the tag's bitmaps until they show what the host did (see [Tasks of
  an add or a release](#tasks-of-an-add-or-a-release)); a record makes
  cxl-fmd read the host again, and one of no capacity is taken as a host
  that accepted none (cxl-fmd's choices). The mock host accepts added
  capacity at once, so in the mock only a topology file has an add the
  host has not answered.

**Comment 6:** A host's Extent List is read a page at a time, and its
Extent List Generation Number is "A device-generated value that is used to
indicate that the list has changed" (Table 7-71). The pages are read into a
scratch list, read again from the start when the generation changes, up to
three times, and applied only when the list was read to its end. The
regions (5601h) are read a page at a time too, and the region table
changes only once every page has been read: a region still there is
updated in place, one gone is removed, and the host's extents take the
region their DPA is in now. 5601h has no generation number, so a table
that changes between pages cannot be detected (cxl-fmd's choice, as the
rest of this paragraph). A host whose 5601h or 5603h fails keeps the
regions, counters and extents of its last complete read, which are then
shown with `Status.Health` `Warning`, and is read again on the next
`--event-poll` tick. The refresh of the
device goes on with the other hosts and with 5608h, and returns the first
failure (cxl-fmd's choice).

When each of these reads last completed is served, in UTC, as the time of
cxl-fmd at which its last page was answered; a read that fails leaves the
time of the one before, and a time is absent until a read has completed
(cxl-fmd's Oem, review F07 claim 6). No CXL command reports such a time:

| Read | Served as |
|------|-----------|
| a host's regions (5601h) | `Oem/Cxl/LastRefreshed` of its `MemoryRegion`s; `Oem/Cxl/DynamicCapacityRegionsLastRefreshed` of its `CXLLogicalDevice` (whose `TotalSupportedExtents`, `AvailableExtents`, `TotalSupportedTags` and `AvailableTags` are from it) |
| a host's extents (5603h) | `Oem/Cxl/LastRefreshed` of its `MemoryExtent`s; `Oem/Cxl/DynamicCapacityExtentsLastRefreshed` of its `CXLLogicalDevice` |
| the tag list (5608h) | `Oem/Cxl/DynamicCapacityTags/LastRefreshed` of the `PCIeDevice` (Comment 4) |

As 5601h has no generation number, the time of a region table is when its
pages were read, not that of one consistent table. In a mock the times are
when the mock last set the state: at load, and at each change it makes.

---

## Resource Model

```
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}
    CXLDevice/DynamicCapacity                          <- 5600h
    CXLDevice/DynamicCapacity/MemoryExtents            -> collection link
    Actions/PCIeDevice.CXLReferenceDCMemory            <- 5606h / 5607h
    Oem/Cxl/DynamicCapacityTags                        <- 5608h (cxl-fmd's, Comment 4)

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents            <- 5603h, every host's extents
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}      DELETE -> 5605h, from that extent's host
                                                                    (all its extents of a sharable tag)

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}
    TotalSupportedExtents, AvailableExtents,
    TotalSupportedTags, AvailableTags                  <- 5601h trailer
    MemoryRegions                                      -> collection link
    Actions/CXLLogicalDevice.DynamicCapacityAdd        <- 5604h
    Actions/CXLLogicalDevice.DynamicCapacityRelease    <- 5605h

/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions           <- 5601h
/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}  PATCH -> 5602h

```

`Links/MemoryExtent` on a `MemoryChunks` resource (`MemoryChunks v1.7.0`)
is not served.

Extent Ids are `ext_<ld>_<hex DPA>`: `<ld>` is the `CXLLogicalDevice` Id of
the host whose extent it is (0 for an SLD), and the DPA is its Starting
DPA in that host's DPA space, in hex with no `0x` (`ext_0_400000000`). This
Id is cxl-fmd's choice, as DSP0288 maps no Dynamic Capacity command. It is
stable across rescans, where the index into the Extent List would shift
whenever an extent is released, and unique on the device, where the DPA
alone is not (Comment 2). The earlier form `ext_<hex DPA>` named an extent
of every host with that DPA; it is `404 Not Found`, with no alias. So is
any other spelling of an Id, with leading zeros, a `0x` or upper-case hex,
in a URI or an `ExtentList` link. On a
Multi-Headed device `<ld>` is the pool LD-ID, as the Host ID is (see
[Host ID Resolution](#host-id-resolution)). Region Ids are the region
index (`0`..`7`).

---

## Command Details

Field layouts are quoted from libcxlmi's `api-types.h`. Byte offsets are
omitted; the struct order is the wire order. Section and table numbers are
CXL r4.0's.

### Get DCD Info (5600h)

Sent on discovery to every Type 3 device, whatever its CEL says. Its
Possible Command Return Codes are Success, Unsupported, Internal Error and
Retry Required (Section 7.6.7.6.1): a device that answers Unsupported (or
Unsupported Mailbox or CCI) is taken not to be a DCD. One that fails
otherwise, or that cannot be reached for a timeout or an I/O error, is
asked again from the next `--rescan` on, with the backoff and the cap of
discovery (cxl-fmd's policy: see "Devices that are not ready" in
[Redfish-API.md](Redfish-API.md)); until it answers it has no Dynamic
Capacity resources, and once it does it is read as at discovery and its
Dynamic Capacity Event Log notification is asked for. Like every
56xxh command, it is sent on the device's own FM-API endpoint, or, for an
MLD on a switch port, through that port with
`DEFINE_CXLMI_TUNNEL_SWITCH` (see `dcd_tunnel()` in `src/dcd.c` and
[Host ID Resolution](#host-id-resolution)). The FM-owned LD itself is no
tunnel target: "The FM-owned LD (LD=FFFFh) is an invalid target in MLDs."
(Section 7.6.7.3.1). For a Multi-Headed device, the specification names
"the LD Pool CCI in a Multi-Headed device" among the valid tunnel targets,
"targeted using the Target Type field", and says "The LD Pool CCI may be
exposed as an MCTP-based CCI or can be accessed via the Tunnel Management
Command command through a head's Mailbox CCI" (Section 2.5). cxl-fmd keeps
each way it reached the device as a route, with the role of the CCI at its
end (`struct fmd_route`), and sends the 56xxh commands on the route to the
LD Pool CCI (`fmd_mhd_pool_route()`): with no tunnel to an endpoint that
answered Get Multi-Headed Info (5500h) with no tunnel, which cxl-fmd takes
to be the LD Pool CCI (its inference: the command set "is required on the
LD Pool CCI", Section 7.6.7.5, which does not say only there), or with a
tunnel of Target Type 1 through a head's Mailbox CCI. Discovery finds only
the first kind; it sends no tunnelled probe. With no route to the LD Pool
CCI known, nothing is sent: the action or `DELETE` is `501 Not
Implemented`.

```c
int cxlmi_cmd_fmapi_get_dcd_info(struct cxlmi_endpoint *ep,
                                 struct cxlmi_tunnel_info *ti,
                                 struct cxlmi_cmd_fmapi_get_dcd_info_rsp *ret);
```

| Response field | Encoding | Redfish |
|----------------|----------|---------|
| `num_hosts` | count | `MaxHosts` |
| `num_supported_dc_regions` | count, 1..8 | `MaxDynamicCapacityRegions` |
| `capacity_selection_policies` | bitmask: bit 0 Free, bit 1 Contiguous, bit 2 Prescriptive; bit 3 "Must be 0" in CXL r4.0 Table 7-65, so a device cannot advertise Enable Shared Access | `AddCapacityPoliciesSupported[]` with `Free`, `Contiguous`, `Prescriptive`; no shipped mock topology lists `EnableSharedAccess`. An `EnableSharedAccess` request is served whatever the mask says, by the mock and by a device: the device refuses it with Invalid Input if the region is not Sharable (Section 7.6.7.6.5) |
| `capacity_removal_policies` | bitmask: bit 0 Tag-based, bit 1 Prescriptive | `ReleaseCapacityPoliciesSupported[]` with `TagBased`, `Prescriptive` |
| `sanitize_on_release_config_mask` | bit N set = region N's Sanitize on Release is configurable | `SanitizationOnReleaseSupport[]` of `{RegionNumber, SanitizationOnReleaseSupported}` |
| `total_dynamic_capacity` | multiples of 256 MB | `TotalDynamicCapacityMiB` = value * 256 |
| `region_N_supported_blk_sz_mask` | bit n set = block size 2^n bytes supported; valid for N < `num_supported_dc_regions` | `MemoryBlockSizesSupported[]` of `{RegionNumber, BlockSizeMiB[]}` listing every set bit >= 20 as 2^(n-20) MiB; sizes below 1 MiB are not listed (Comment 3) |

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
| `region_configs[i].decode_len` | not exposed; it is the HDM decode window the host programs (multiple of 256 MB). It bounds the extents of the region: an extent is in the region when it starts at or after `base` and before `base` + `decode_len` (Table 8-347) |
| `region_configs[i].region_len` | `RegionSizeMiB` |
| `region_configs[i].block_size` | `BlockSizeMiB` |
| `region_configs[i].flags` | Attributes1 (CXL r4.0 Table 7-68, byte 20h); the bits are the CDAT DSMAS Flags positions: bit 2 NonVolatile -> `NonVolatileRegion`, bit 3 Sharable -> `ShareableRegion`, bit 4 Hardware Managed Coherency -> `HardwareManagedCoherencyRegion`, bit 5 Interconnect specific Dynamic Capacity Management -> `InterconnectSpecificDCManagement`, bit 6 Read-Only -> `ReadOnlyRegion`. Bits 1:0 and 7 are reserved |
| `region_configs[i].sanitize_on_release` | Attributes2 (byte 24h), bit 0 -> `SanitizeOnRelease`; bits 7:1 reserved |
| `num_extents_supported` / `num_extents_available` | `CXLLogicalDevice.TotalSupportedExtents` / `AvailableExtents` |
| `num_tags_supported` / `num_tags_available` | `CXLLogicalDevice.TotalSupportedTags` / `AvailableTags` |

`RegionType` is always `Dynamic`. `InterconnectSpecificDCManagement` is
Bit[5] of Attributes1 as the device reports it. A DCD "shall set the
Dynamic Capacity DSMAS Flag in each structure to indicate a Dynamic
Capacity supported range" (Section 9.13.3), so it is expected to be set for
every DC region; that Bit[5] is that flag is cxl-fmd's reading, not
confirmed against the CDAT specification. A region with `region_len == 0`
is not configured and is omitted from the collection rather than shown
empty.

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
      "ExtentLink": { "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0_400000000" }
    }
  ],
  "Status": { "State": "Enabled", "Health": "OK" }
}
```

`ExtentsCount` and `MemoryExtents[]` are the extents of this LD's host in
the region; `Pending` is `false` for each (Comment 5). `Status.Health` is
`Warning` while the tag list could not be read (Comment 5), and while the
host's last read failed or one is owed (Comment 6; cxl-fmd's choice).

### Set DC Region Configuration (5602h)

`PATCH` on a `MemoryRegion`. Only `BlockSizeMiB` and `SanitizeOnRelease` are
writable; any other property in the body is refused with `400` and the
message "Property is not writable" (Comment 3). OData annotations (`@...`)
are ignored.

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

- No host may have an extent in the region. 5602h "shall be processed
  only when all capacity has been released from the region on all LDs"
  (Section 7.6.7.6.3), and cxl-fmd refuses it with `409` before sending
  anything while any host's 5603h lists an extent in a region of that
  number, pending or not. Capacity with an FM reference "will remain
  allocated ... even if all DCD Extents that reference it are removed"
  (Section 7.6.7.6.7); on hardware no host lists it, and cxl-fmd does not
  know its region, so it is not refused for it and the device decides
  (cxl-fmd's choice). The mock knows where such capacity is, and refuses
  5602h on its region. A device's Unsupported is also `409`.
- The command has no Host ID: the new Region Block Size and Sanitize on
  Release are the region's on every LD. On hardware every host's regions
  and extents are read again after it; the mock changes every host's view
  of the region.
- Omitted properties are re-sent with their current values; 5602h has no
  "leave unchanged" encoding.

```bash
curl -X PATCH .../CXLLogicalDevices/0/MemoryRegions/0 \
  -H 'Content-Type: application/json' \
  -d '{"BlockSizeMiB": 512, "SanitizeOnRelease": false}'
```

### Get DC Region Extent Lists (5603h)

Read once per host. Each entry is one extent of that host (Comment 2);
together they are the `MemoryExtents` collection of the `PCIeDevice`, and
each host's are its regions' `MemoryExtents[]`. Page with
`start_ext_index`/`extent_count` until `extents_returned` accumulates to
`total_extents`, into a scratch list; restart from index 0 if
`list_generation_num` changes between pages, and apply the list only once
it is complete (Comment 6).

```c
int cxlmi_cmd_fmapi_get_dc_region_ext_list(struct cxlmi_endpoint *ep,
                                           struct cxlmi_tunnel_info *ti,
                                           struct cxlmi_cmd_fmapi_get_dc_region_ext_list_req *in,
                                           struct cxlmi_cmd_fmapi_get_dc_region_ext_list_rsp *ret);
```

| Response field | Redfish `MemoryExtent` property |
|----------------|--------------------------------|
| `extents[i].start_dpa` | `ExtentOffsetMiB` (bytes / 2^20), in the DPA space of the request's host: the schema's "device address range" is taken to be that host's (cxl-fmd's reading). It also selects the host's region the extent is in |
| `extents[i].len` | `ExtentSizeMiB` |
| `extents[i].tag` | `Tag`, 16 bytes rendered as a UUID string (`8-4-4-4-12`). An all-zero tag is no tag, cxl-fmd's convention: CXL r4.0 gives a zero Tag no reserved meaning (Open Question 5). `Tag` and `FMReferencedExtent` are left out |
| `extents[i].shared_seq` | `SequenceNumber` |
| `host_id` (request) | the `<ld>` of the extent's Id, and its one `Links/MemoryRegions` link, to that host's region; none when the extent is in no region of the host |

`FMReferencedExtent` is the FM Holds Reference flag of the extent's tag in
the last complete 5608h read; it is left out while that read is not known
(Comment 4). `Status.State` is `Enabled`: an extent 5603h lists is not
pending (Comment 5). `Status.Health` is `Warning` when the
extent is in no region of its host, when its host's last read failed
and it is what the read before listed (Comment 6), or when it has a tag
and the tag list could not be read (Comment 5); `OK` otherwise.

```json
{
  "@odata.type": "#MemoryExtent.v1_0_0.MemoryExtent",
  "@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0_400000000",
  "Id": "ext_0_400000000",
  "Name": "Dynamic Capacity Extent",
  "ExtentOffsetMiB": 16384,
  "ExtentSizeMiB": 4096,
  "Tag": "6f1a2c3e-0000-4000-8000-000000000001",
  "SequenceNumber": 0,
  "FMReferencedExtent": false,
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
| `region_num` | region index; reserved for Prescriptive (Table 7-72) | `RegionNumber` (required for Free, Contiguous, EnableSharedAccess); sent as 0 for Prescriptive |
| `length` | bytes, block-size multiple; reserved for Prescriptive and Enable Shared Access | `Length` in MiB (required for Free, Contiguous; sent as 0 for the other policies); the schema leaves the unit unannotated, cxl-fmd treats it as MiB like every other size property |
| `tag` | 16 bytes; reserved for Prescriptive | `Tag` UUID string (required for Free, Contiguous, EnableSharedAccess; sent as zeros for Prescriptive). An empty or all-zero `Tag` is refused with `400` for EnableSharedAccess: cxl-fmd takes all zeros for no tag, its own convention, as CXL r4.0 gives a zero Tag no reserved meaning (Open Question 5) |
| `ext_count` + `extents[]` | DPA/length/tag/shared_seq per extent | `ExtentList[]` of `MemoryExtent` links (required for Prescriptive); every link is refused, see Prescriptive below |

Policy semantics (CXL r4.0 Section 7.6.7.6.5):

- **Free**: the device picks any blocks in `RegionNumber` totalling `Length`.
- **Contiguous**: same, but a single DPA-contiguous extent.
- **Prescriptive**: the FM names the exact extents. The specification
  does not require them to exist already; it fails the command with
  Invalid Extent List "When the Selection Policy is set to Prescriptive and
  any of the DPAs are already accessible to the same LD" (Section
  7.6.7.6.5). The DPAs of the list are in the DPA space of the host the
  request names, as "The DPA ranges exposed by the device to each host are
  independent of one another" (Section 9.13.3). `ExtentList` takes links
  to `MemoryExtent` resources, and every `MemoryExtent` is some host's
  extent: a link to an extent of this LD's host names DPAs "already
  accessible to the same LD", and one to another LD's extent names DPAs of
  another host's DPA space. Both are refused with `400` before anything is
  sent ("An extent in ExtentList is already accessible to this logical
  device", "An extent in ExtentList belongs to another logical device").
  A Prescriptive add therefore cannot be named through Redfish: there is
  no way to name free capacity, as `POST` on the `MemoryExtents`
  collection is not served (cxl-fmd's limitation). So the action's
  `Policy@Redfish.AllowableValues` ("To specify the list of supported
  values for a parameter, the service may include the
  @Redfish.AllowableValues annotation", DSP0266 1.25.0 Section 9.6.14.1)
  never has `Prescriptive`, even on a device whose
  `AddCapacityPoliciesSupported` lists it (review DCD-A5). It has `Free`
  and `Contiguous` when the device supports them, and `EnableSharedAccess`
  always, as it is always sent (see Get DCD Info).
- **EnableSharedAccess**: give this host access to the capacity tagged
  `Tag`, in `RegionNumber`; `Length` is not sent. The device fails it
  with Invalid Input "When the specified region is not Sharable", "When
  the tagged capacity is already mapped to any Host ID via a non-Sharable
  region", "When the tagged capacity cannot be added to the requested
  region due to device-imposed restrictions" and "When the same tagged
  capacity is currently accessible by the same LD" (Section 7.6.7.6.5).
  The mock does the same, and its device-imposed restriction is that the
  host is given the capacity in the region number its holders have it in
  (`400` for another region number). The host gets extents of its own,
  one for each extent of the holder with the lowest Host ID (or of what an
  FM reference keeps, when no host holds it), with the same length, Tag
  and Shared Extent Sequence, at the same offset into its own view of the
  region.
- **Free** and **Contiguous** in the mock: the new extents are not
  pending, and in a Sharable region their Shared Extent Sequence numbers
  go on from the host's extents with the tag (0 in a region that is not
  Sharable, "For extents that describe non-shareable regions, this field
  shall be 0", Table 8-230). Capacity is taken at an offset into the
  region that no host, and no FM reference, has.

```bash
curl -X POST .../CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityAdd \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Free", "RegionNumber": 0, "Length": 4096,
       "Tag": "6f1a2c3e-0000-4000-8000-000000000001"}'
```

What cxl-fmd checks before sending: `Policy` must be in
`AddCapacityPoliciesSupported`, except EnableSharedAccess, which is always
sent (see Get DCD Info); a Free or Contiguous `Length` must be a multiple of
the region's `BlockSizeMiB`. cxl-fmd does not check the capacity left in
the region, nor `AvailableExtents` or `AvailableTags`: the device does, and
fails with Resources Exhausted "if the Extent List would cause the device
to exceed its extent or tag tracking ability" or when the capacity would
exceed the region's decode length (Section 7.6.7.6.5), which is `409`
(Comment 3). The mock checks the region's free space itself (`409`, or
`400` for more than the region's length). On success cxl-fmd reads 5601h
and 5603h for that host again, then 5608h; whether the new extent is then
listed, or only shown in `PendingLDs` of its tag until the host answers,
depends on the device (Comment 5). The answer is `202 Accepted` with a
`Task` that follows the host until it is seen to have accepted the
capacity, some of it, or none (see [Tasks of an add or a
release](#tasks-of-an-add-or-a-release)).

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
| `flags[5]` | Sanitize on Release | `SanitizeOnRelease` (required). If set, the device "shall sanitize all released capacity", for shared capacity "after the last host has released the capacity" (Table 7-73). A device that does not support it refuses the request: 5605h fails with Invalid Input "When Sanitize on Release is set but is not supported by the device" (Section 7.6.7.6.6), which is `400` |
| `length` | bytes; "Shall be > 0" and reserved for Prescriptive (Table 7-73) | `Length` MiB (required for TagBased, and greater than 0: there is no "release everything"; sent as 0 for Prescriptive). More than the host holds with the tag is Resources Exhausted, `409` |
| `tag` | 16 bytes; reserved for Prescriptive | `Tag` (required for TagBased; sent as zeros for Prescriptive) |
| `ext_count` + `extents[]` | DPA/length/tag/shared_seq per extent, in the host's DPA space | `ExtentList[]` (required for Prescriptive). Each link must be to an extent of this LD's host: a link to another LD's extent is refused with `400` ("An extent in ExtentList belongs to another logical device") before anything is sent. The payload carries that host's own DPA, length, Tag and Shared Extent Sequence of each extent |

```bash
curl -X POST .../CXLLogicalDevices/0/Actions/CXLLogicalDevice.DynamicCapacityRelease \
  -H 'Content-Type: application/json' \
  -d '{"Policy": "Prescriptive", "ForcedRemoval": false, "SanitizeOnRelease": true,
       "ExtentList": [
         {"@odata.id": "/redfish/v1/Chassis/cxl_dcd0/PCIeDevices/cxl_dcd0/MemoryExtents/ext_0_400000000"}
       ]}'
```

The release takes capacity from the host the request names, and from no
other: "To remove a host's access to the shared extent, the FM issues
Initiate Dynamic Capacity Release Request with Selection Policy=Tag-based
with the Host ID associated with that host" (Section 7.6.7.6.6). The
answer is `202 Accepted` with a `Task`, which is `Completed` once what the
release named is gone from the host's 5603h (see [Tasks of an add or a
release](#tasks-of-an-add-or-a-release)); on hardware 5601h and 5603h of
that host are read again after the command, and 5608h.

What the mock refuses, as the specification says a device does:

- A Prescriptive list with an extent the topology file declares pending
  (the mock's choice; Redfish does not show it pending) and
  `ForcedRemoval` `false`:
  "The command shall fail with Invalid Physical Address if an extent in the
  Extent List covers a nonexistent or pending (Pending state as defined in
  Section 9.13.3.3) DPA range and the Forced Removal flag is not set"
  (Section 7.6.7.6.6). `400`, as Invalid Physical Address is on hardware.
  With `ForcedRemoval` `true` the extent is removed.
- A release of part of what the host has of a tag in a Sharable region:
  Invalid Input "When the Tag represents sharable capacity, and the Extent
  List covers only a portion of the capacity associated with the Tag"
  (Section 7.6.7.6.6). `400` for a Prescriptive list that leaves out some
  of the host's extents with the tag, and for a Tag-based `Length` short
  of what the host has of it; that the latter is the "portion" the
  sentence means is the mock's reading, as the sentence names the Extent
  List. A Tag-based release in a region that is not Sharable takes
  `Length` from the tail of the host's extents with the tag.
- A Tag-based `Length` more than the host has of the tag: Resources
  Exhausted, `409`. A tag the host has nothing of is `404` in the mock; a
  device fails it with Invalid Input "When the command is sent with a
  Removal Policy of Tag-based and the input Tag does not correspond to any
  currently allocated capacity" (Section 7.6.7.6.6), which is `400`.

When the last host's extents of a tag go and the FM holds a reference on
the tag, the mock keeps the capacity, as "The tagged capacity will remain
allocated, and contents will be preserved even if all DCD Extents that
reference it are removed" (Section 7.6.7.6.7). It is then no
`MemoryExtent`: it is a row of `Oem/Cxl/DynamicCapacityTags` with
`FMHoldsReference` `true`, empty `AcceptedLDs` and no `Extents`, it counts
against the region, and Enable Shared Access gives it to a host again.

#### DELETE of a MemoryExtent

The contract, in short: **a `DELETE` of one extent with a tag in a
Sharable region releases every extent that host has with that tag, not
only the one the URI names.** The host's other extents with the tag are
released with it; other hosts' extents with the tag stay, and so does the
FM reference. In a mock, whose host accepts at once (Open Question 22),
they are all gone (`404`) when the `DELETE` answers. On hardware the
release is not forced, and the extents, the one the URI names too, stay
and are served until the host accepts the release and its 5603h no
longer lists them (Open Question 23). Anything narrower, or on other
terms, is asked for with `DynamicCapacityRelease` (see below).

`DELETE .../MemoryExtents/ext_<ld>_<dpa>` is sugar, cxl-fmd's own (DMTF
does not say what DELETE of a `MemoryExtent` means), for a release from
the host whose extent it is, and from no other host, with
`ForcedRemoval` `false` and `SanitizeOnRelease` taken from that host's
region (`false` when the extent is in no region of the host):

- An extent with a tag, in a Sharable region: a Tag-based release, with
  the Host ID of the extent's host and a `Length` that is the sum of
  every extent that host has with the tag. Every one of them goes, the
  other hosts' extents with the tag stay, and so does the FM reference.
  Releasing less would be the "portion" the specification fails with
  Invalid Input: "When the Tag represents sharable capacity, and the
  Extent List covers only a portion of the capacity associated with the
  Tag" (Section 7.6.7.6.6). Refusing such a `DELETE` with `409` was
  considered; the maintainer kept the wider release (Open Question 19).
- On a device whose `ReleaseCapacityPoliciesSupported` lacks `TagBased`,
  the same capacity is released Prescriptive instead: a list of every
  extent the host has with the tag (cxl-fmd's choice).
  `mock/dcd-prescriptive-release.json` is such a device.
- Any other extent (no tag, a region that is not Sharable, or no region
  of the host): a Prescriptive release of that one extent. An extent
  whose tag is all zeros is one with no tag (Open Question 5), so in a
  Sharable region it too is released by itself, which a device that
  takes the zero Tag for an ordinary one may refuse as a "portion".

`DynamicCapacityRelease` is the way to release on other terms:
`ForcedRemoval` `true`, a `SanitizeOnRelease` other than the region's, a
Tag-based `Length` of the client's choosing, or a Prescriptive list of
the client's extents. What it names is sent as asked, and the device
decides; a release of only part of what a host has of a sharable tag is
expected to fail with Invalid Input as quoted above (the mock refuses it,
Open Questions 11 and 12).

The answer is `202 Accepted` with a `Task`, whose `Payload` is the
`DELETE`; its monitor answers `204 No Content` once what was released is
gone from the host's 5603h (see [Tasks of an add or a
release](#tasks-of-an-add-or-a-release)). A device that refuses is
answered at once as in Comment 3; a policy the device does not support is
`400`. This mirrors how
`DELETE` on a `MemoryChunk` maps to Set LD Allocations. Capacity an FM
reference keeps that no host holds is no `MemoryExtent`, so it cannot be
deleted; only `CXLReferenceDCMemory` with `FMReference` `false` frees it.

### Tasks of an add or a release

Initiate Dynamic Capacity Add "shall complete when the device initiates
the Add Capacity procedure" (CXL r4.0 Section 7.6.7.6.5), and Initiate
Dynamic Capacity Release "when the device initiates the Remove Capacity
procedure" (Section 7.6.7.6.6). The host answers after: an add "allows
the host to control exactly how much of the added capacity it wishes to
utilize, which may be less than the amount of capacity sent in the add
capacity event, or even 0", and of a release "The host may call Release
Dynamic Capacity multiple times, returning different portions of the
total capacity over time" (Section 9.13.3). A device that accepted the
command has started a change the host may make in part, late, or not at
all.

So `DynamicCapacityAdd`, `DynamicCapacityRelease` and a `DELETE` of a
`MemoryExtent` the device accepts are answered as DSP0266 1.25.0 Section
12.2 has an asynchronous operation answered: "When a client issues a
request that results in a long-running operation, the service returns the
HTTP 202 Accepted status code and a Location header that contains a task
monitor URI", and "If a response body is provided, it shall contain a
representation of the Task resource that represents the state of the
operation". The `Task` is at `/redfish/v1/TaskService/Tasks/{id}`, its
monitor at `/redfish/v1/TaskService/TaskMonitors/{id}`. A request
cxl-fmd or the device refuses is answered at once, with no task, as
before; so is a command whose outcome is not known ("Outcome unknown",
Comment 3).

**Recorded before the command is sent.** The task is made, and the
request recorded in it, before 5604h or 5605h is sent; if it cannot be
made (no memory), nothing is sent (`500`). `Payload` has the request as
it came: `TargetUri`, `HttpOperation` and, for an action, `JsonBody`, the
body. `Oem/Cxl/DynamicCapacity` (type `#Cxl.v1_0_0.Task` of cxl-fmd's Oem
schema) has what was sent: `HostId`, `Policy`, `RegionNumber` and `Tag`
of an add, `LengthBytes` of a Free or Contiguous add or of a Tag-based
release, `ForcedRemoval` and `SanitizeOnRelease` of a release, and the
extents of a Prescriptive release (`RequestedExtents`), with the host's
extents before it (`BaselineExtents`) and their Extent List Generation
Number (`BaselineGeneration`). On hardware a host whose last read failed,
or is owed, or a tag list that could not be read, is read first; if the
host still cannot be read, nothing is sent and the answer is `503`. An
extent a Prescriptive release or a `DELETE` names that the read no
longer lists for the host is not sent either: `404`.

**One task at a time for a host and a tag.** What 5603h and the bitmaps
show of a host cannot be told apart between two requests of one tag, so
a request is refused with `409` and nothing is sent while a task follows
the same host with the same Tag (all zeros is a tag here; a Prescriptive
release has the tags of its extents), and an add of a tag whose
Pending Reference Bitmap already has the host's LD is refused likewise
(`ResourceInUse`, "Wait for the Dynamic Capacity task that follows this
host and Tag to end"). Both are cxl-fmd's choices.

**How a task ends** (cxl-fmd's rules; the specification gives none). A
task is judged on a read of its host that completed, 5603h and then
5608h; a read that fails changes nothing.

| Task | `Running` while | `Completed` when | `Exception` when |
|------|-----------------|------------------|------------------|
| Add | the host's LD is in the tag's Pending Reference Bitmap, "an LD for which the tagged capacity has been added with no host response yet" (Table 7-78), or nothing says the host answered | capacity of the add is in the host's 5603h (the parts of its extents with the add's Tag, in its region, that `BaselineExtents` does not have) and the LD is not pending. With less than a Free or Contiguous `Length`, `TaskStatus` is `Warning` and the message says how much | the LD was seen in the Pending Reference Bitmap after the command and is no longer, with none of the add listed; or an Add Capacity Response record, whose extent "specifies the capacity accepted by the host" (Table 8-229), names the host with a `Length` of 0 (taken to be this add's when its Tag is the add's, or, when it is all zeros, when this is the only add followed on that host) |
| Release | any of what it named is still in the host's 5603h; the message says how much has gone | all of it is gone: the extents of a Prescriptive list, or `Length` bytes of the Tag for a Tag-based one (a `DELETE` is one of these) | never on what the host does: nothing says a host refuses a release |
| Either | | | the device is gone: what its host did is not known (`500` "Outcome unknown") |

The capacity seen is in the task as `AcceptedExtents` or
`ReleasedExtents` (DPA, length and Tag of each part), and an add's
`Links/CreatedResources` links the `MemoryExtent`s it is in that are
still there. There is no `PercentComplete`: what share a host has done is
not known. No time limit ends a task, and nothing in a task sends a
command or frees capacity: a host that never answers leaves its task
`Running`, the capacity where it is, and its tag refused for that host
(by the rule above) until the host answers or the device goes. That a
5603h lists only what the host accepted is cxl-fmd's reading (Comment 5);
on a device whose 5603h also lists capacity the host has not accepted, an
untagged add, or one on a device with no tag list, is `Completed` as soon
as it is listed.

**When it is judged.** On hardware, on every read of the host: after a
command, after a DC event record names it, and on every `--event-poll`
tick while a task follows it (the host is read on the tick for that
reason alone). With `--event-poll 0`, only a record or a request that
reads that host moves its tasks. In the mock, on every turn of the event
loop (100 ms while one runs); the mock's host has answered by then,
unless the topology file's `host_response` makes it answer later, in
part, or not at all (see [docs/mock-topology.md](mock-topology.md)).

**The monitor.** While the task runs, `202` and the `Task`. Once it is
over, "The headers and response body of the initial operation, as if it
had completed synchronously. If the initial operation fails, the response
body shall contain an error response" (DSP0266 1.25.0 Section 12.2): an
action `200 OK` with an error object whose `code` is `Base.1.24.Success`
and whose `message` says what the host accepted or released, a `DELETE`
`204`; a host that accepted none `409` (`GeneralError`); a device that is
gone `500` "Outcome unknown". The statuses are cxl-fmd's choices. Nothing
cancels a task (DSP0266: "If a service supports cancellation of a task,
the Allow header shall contain DELETE for the task monitor"; cxl-fmd does
not), tasks are not kept across a restart, and of the tasks that are over
the newest 64 are kept.

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
`PCIeDevice`. Both "shall return Invalid Input if the Tag in the payload
does not match an existing sharable tag" (Sections 7.6.7.6.7 and
7.6.7.6.8), which is `400`. The mock does the same: a tag that no
extent in a Sharable region has, and that is not capacity an FM reference
keeps, is `400`. Adding a reference the FM already holds succeeds: "This
command has no effect and will return Success if the FM has already added
a reference to the tagged capacity" (Section 7.6.7.6.7). An empty or
all-zero `Tag` is refused with
`400` before anything is sent (cxl-fmd's convention that all zeros is no
tag). After either, 5608h is read again, so `FMReferencedExtent` on every
extent with the tag, and `FMHoldsReference` of its row, are up to date.
In the mock, removing the reference of a tag no host has frees the
capacity: the tag leaves the list, and the region can be reconfigured.

```bash
curl -X POST .../PCIeDevices/cxl_dcd0/Actions/PCIeDevice.CXLReferenceDCMemory \
  -H 'Content-Type: application/json' \
  -d '{"Tag": "6f1a2c3e-0000-4000-8000-000000000001", "FMReference": true}'
```

### Dynamic Capacity List Tags (5608h)

No dedicated Redfish resource: the list is shown as
`Oem/Cxl/DynamicCapacityTags` on the `PCIeDevice` (Comment 4), and gives
the extents `FMReferencedExtent`. Read once per device after
the hosts' 5601h and 5603h: on discovery, after 5602h, 5604h and 5605h,
after 5606h and 5607h (alone), and after the DC event records name a host.

"This command allows an FM to re-establish context for an LD-FAM DCD, by
receiving a list of all existing tags, with bitmaps indicating which LDs
have access, and a flag indicating whether the FM holds a reference"
(Section 7.6.7.6.9). cxl-fmd keeps the list as it was read, each row with
both 32-byte bitmaps, together with the Generation Number and the Validity
Bitmap. The list is read a page at a time, with Starting Index, and read
again from index 0 when
the Generation Number changes on a later page, up to three times. Total
Number of Tags is described as "Maximum number of tags to return in the
response payload" (Table 7-77), the text of Max Tags, so the read stops at
it or at a page with no tags, whichever comes first, and keeps at most 4096
tags (cxl-fmd's limit). Only a complete read replaces the list. A device
that answers Unsupported has no list (`TagsKnown` `false`); on any other
failure the last list is kept, not known to be current (`TagsKnown`
`false`, `Tags` left out, and the device's regions and tagged extents
`Warning`, Comment 5). The extents are kept either way.

```c
int cxlmi_cmd_fmapi_dc_list_tags(struct cxlmi_endpoint *ep,
                                 struct cxlmi_tunnel_info *ti,
                                 struct cxlmi_cmd_fmapi_dc_list_tags_req *in,
                                 struct cxlmi_cmd_fmapi_dc_list_tags_rsp *ret);
```

| Response field | Encoding | Redfish |
|----------------|----------|---------|
| `generation_num` | changes whenever the tag list changes | `Generation`; restart paging on change |
| `validity_bitmap` | bit 0: `ref_bitmap` valid, bit 1: `pending_ref_bitmap` valid | gates `AcceptedLDs` and `PendingLDs` |
| `tags_list[i].tag` | 16 bytes | `Tags[].Tag` |
| `tags_list[i].flags` | bit 0: FM reference held | `Tags[].FMHoldsReference`; `MemoryExtent.FMReferencedExtent` on every extent with this tag |
| `tags_list[i].ref_bitmap` | 256 bits, bit n for LD n | `Tags[].AcceptedLDs` (Comment 4) |
| `tags_list[i].pending_ref_bitmap` | 256 bits, bit n for LD n | `Tags[].PendingLDs`; no extent's `Pending` (Comment 5) |

An extent that 5603h lists for a host, and whose tag's row has the host in
neither bitmap while the Reference Bitmap is valid, is logged at debug
level: the two lists were read at different times. Nothing is changed for
it.

---

## Host ID Resolution

FM-API DCD commands address a host by a 16-bit Host ID that is local to the
DCD. Redfish never exposes it; cxl-fmd derives it from the
`CXLLogicalDevice` the request is made on:

| Device class | Host ID |
|--------------|---------|
| SLD DCD | 0 |
| MLD DCD | the LD-ID (`fmd_ld.ld_id`) |
| MH-SLD / MH-MLD DCD | the pool LD-ID (`fmd_ld.ld_id`). This is cxl-fmd's interpretation: the specification says only that the Host ID is, "For an LD-FAM device, the LD-ID of the host interface" (Tables 7-66 and 8-229), and does not say which LD-ID numbering 56xxh uses on a Multi-Headed DCD. The LD-IDs at a head run "from 0 to n-1 relative to that head" (Section 2.5.1), so they are not unique on the device; the pool index is, and 5500h already names LDs by it (Start LD ID and LD Map). The per-head LD-ID a host sees is not used. An open question for CXL clarification; the `<ld>` of an extent's Id, and the LD-IDs of the tag view, inherit it |

The Host ID is two bytes, but the bitmaps of 5608h stop at LD 255 ("bit[7]
of the last byte represents LD 255", Table 7-78). A host past 255 keeps
its extents, it is in no `AcceptedLDs`
or `PendingLDs`, and a DC event
record that names it is logged as a warning and does not refresh it.

The FM-side DC event records, Add Capacity Response (04h) and Capacity
Released (05h), name the host: "For an LD-FAM device, the LD-ID of the
host interface that triggered the generation of the event" (Table 8-229).
The hosts the records name are gathered in a bitmap of LDs 0 to 255. When
Bit[0] of a record's Validity Flags is set, its Number of Available
Extents (78h), "The remaining number of extents that the device supports",
and Number of Available Tags (7Ch) become that host's `AvailableExtents`
and `AvailableTags`. Each host named is then read again, 5601h and 5603h,
and 5608h once after them; after an overflow of the log, every host is.
The extent a record carries (38h) is not applied: the host's 5603h, read
again, says what it has (cxl-fmd's choice).

The 56xxh set names the host by the Host ID in the payload, never by the
tunnel: it is "used by the FM to manage memory assignment within an LD-FAM
DCD" (Section 7.6.7.6). A switch-attached MLD gets it through the Tunnel
Management Command, which "instructs a switch to tunnel the provided command
to the FM-owned LD of the MLD on the specified port" (Section 7.6.7.3.1). For switch-attached devices
the command is tunnelled with `DEFINE_CXLMI_TUNNEL_SWITCH(ti, port)` exactly
as the existing 54xxh MLD commands are (see `dcd_tunnel()` in `src/dcd.c`).
cxl-fmd does not send them through the two-level
`DEFINE_CXLMI_TUNNEL_SWITCH_MLD(ti, port, ld)` tunnel, which reaches an
individual LD: that is cxl-fmd's choice, as the specification does not say
whether an LD answers them.

The `Systems/{host}` that ultimately receives the capacity is found through
the existing `Connection` for that LD; no new routing is needed.

### Writes on a Multi-Headed DCD

That the Host ID of a Multi-Headed DCD is the pool LD-ID is not verified:
not on hardware, and not by the specification. The Host ID of Initiate
Dynamic Capacity Add is "For an LD-FAM device, the LD-ID of the host
interface to which the capacity is being added" (Table 7-72), and of
Initiate Dynamic Capacity Release "For an LD-FAM device, the LD-ID of the
host interface from which the capacity is being released" (Table 7-73).
A Multi-Headed device has two numberings of LD-IDs: "MLDs exposed via the
heads of an MH-MLD use LD-IDs from 0 to n-1 relative to that head, where n
is the number of LDs mapped to the head. The MH-MLD maps the LD-IDs
received at a head to the device-wide LD index in the MH-MLD's LD pool"
(Section 2.5.1). Which of the two the 56xxh commands take, the tables do
not say.

All the same, cxl-fmd sends the DC writes that name a host, Initiate
Dynamic Capacity Add and Release (the actions, and `DELETE` of a
`MemoryExtent`), to a Multi-Headed DCD with the pool LD-ID as the Host
ID, as to any DCD; Set DC Region Configuration and Add and Remove
Reference name no host (Tables 7-69, 7-74 and 7-75) and are sent as on
any DCD. Nothing gates them, and no option turns them off: this is the
maintainer's decision, recorded here so that it is not taken for
something verified. A device that reads the Host ID as a head-local LD-ID
would take the request for another LD than the one the `CXLLogicalDevice`
is: on an MH-MLD, LD-ID `n` of some head, which may be another host's.
The reads, Get Host DC Region Configuration and Get DC Region Extent
Lists, use the same Host ID, so what is shown of a host has the same
doubt. The commands go on the route to the LD Pool CCI
(`fmd_mhd_pool_route()`, see [Get DCD Info (5600h)](#get-dcd-info-5600h)),
not to a head. `test_mhd_route_head_mailbox` in `tests/discovery-tests.c`
checks that Initiate Dynamic Capacity Add on pool LD 2 is sent with Host
ID 2. See Open Question 17.

---

## Proposed cxl-fmd Changes

### Schema bundle

The DSP8010 zip on dmtf.org sits behind a bot challenge, so the files that
changed for DCD (`MemoryExtent`, `MemoryExtentCollection`, `MemoryRegion
v1.1.0`, `CXLLogicalDevice v1.4.0` for the DCD actions and counters, of
which the v1.5.0 file is installed, `PCIeDevice v1.23.0`, `MemoryChunks
v1.7.0`) were fetched individually from `https://redfish.dmtf.org/schemas/v1/`
and installed with their symlinks. `scripts/update_schemas.py` lists the
MemoryExtent schemas in its defaults so a future bundle update keeps them.
Handlers emit `MemoryRegion.v1_1_0`, `MemoryExtent.v1_0_0`,
`CXLLogicalDevice.v1_5_0` and `PCIeDevice.v1_23_0`.

### Data model (`include/cxl-fmd.h`)

As implemented; the comments are the header's.

```c
#define FMD_MAX_DC_REGIONS     8
#define FMD_DC_TAG_LEN         16
#define FMD_DC_LD_BITMAP_BYTES 32    /* LD 0..255 in the 5608h bitmaps (Table 7-78) */
#define FMD_DC_MAX_HOST_ID     255   /* highest Host ID those bitmaps can name */
#define FMD_DC_REGION_NONE     0xFF  /* extent lies in no region of its host */

enum fmd_dc_sync {
	FMD_DC_SYNC_NEVER = 0,		/* not read yet */
	FMD_DC_SYNC_OK,			/* the last read completed */
	FMD_DC_SYNC_STALE,		/* the last read failed */
	FMD_DC_SYNC_DIRTY,		/* to be read: a record, an overflow or a command */
};

/* One Dynamic Capacity Tag Information entry of 5608h (Table 7-78) */
struct fmd_dc_tag {
	uint8_t tag[FMD_DC_TAG_LEN];
	bool    fm_ref;                             /* Flags Bit[0], FM Holds Reference */
	uint8_t ref[FMD_DC_LD_BITMAP_BYTES];        /* Reference Bitmap: LDs that accepted */
	uint8_t pend[FMD_DC_LD_BITMAP_BYTES];       /* Pending Reference Bitmap */
};

/* The device's tag list, as the last complete 5608h read gave it */
struct fmd_dc_tags {
	struct fmd_dc_tag *v;
	uint32_t n;
	uint32_t gen;                       /* Generation Number (Table 7-77) */
	uint8_t  validity;                  /* Validity Bitmap: Bit[0] ref, Bit[1] pending */
	bool     known;                     /* the last read completed */
	bool     unsupported;               /* the device answered Unsupported */
};

/* Device-wide, from Get DCD Info (5600h); on fmd_sld, fmd_mld or fmd_mhd */
struct fmd_dcd_info {
	uint8_t  num_hosts;
	uint8_t  num_regions;
	uint16_t add_policies;              /* FMD_DC_ADD_* bitmask */
	uint16_t release_policies;          /* FMD_DC_REL_* bitmask */
	uint8_t  sanitize_cfg_mask;
	uint64_t total_dc_capacity;         /* bytes */
	uint64_t blk_sz_mask[FMD_MAX_DC_REGIONS];
	/* SLD host 0's counters and 5603h state (MLD/MHD keep them on fmd_ld) */
	uint32_t extents_supported, extents_available, tags_supported, tags_available;
	uint32_t dc_ext_gen;
	uint8_t  dc_sync;                   /* enum fmd_dc_sync */
	int      dc_sync_err;
	struct fmd_dc_tags tags;            /* 5608h; the mock builds its own */
	/* Mock only */
	struct fmd_dc_mock_hold *mock_orphans;  /* FM-referenced capacity no host holds */
	uint8_t (*mock_refs)[FMD_DC_TAG_LEN];   /* tags the FM holds a reference on */
	uint32_t n_mock_refs;
	struct fmd_dc_tag *mock_tags;           /* the file's "tags" entries */
	uint32_t n_mock_tags;
};

/* One host's view of a DC region, from 5601h. Child of fmd_sld or fmd_ld. */
struct fmd_dc_region {
	struct fmd_component base;
	uint8_t  region_id;
	uint16_t host_id;
	uint64_t base_dpa;                  /* Region Base, this host's */
	uint64_t decode_len;                /* Region Decode Length */
	uint64_t region_len;
	uint64_t block_size;
	uint8_t  flags;                     /* FMD_DC_REGION_* */
	bool     sanitize_on_release;
};

/* One host's extent (Comment 2), keyed by (host_id, start_dpa). Child of the device. */
struct fmd_dc_extent {
	struct fmd_component base;
	uint16_t host_id;                   /* Host ID (LD-ID) whose list has it */
	uint64_t start_dpa;                 /* in this host's DPA space */
	uint64_t len;
	uint8_t  tag[FMD_DC_TAG_LEN];       /* all zero: no tag (cxl-fmd's choice) */
	uint16_t shared_seq;
	uint8_t  region_id;                 /* this host's region, or FMD_DC_REGION_NONE */
	bool     pending;                   /* mock only: the file has the add not answered */
};

/* Mock only: capacity an FM reference keeps that no host holds; no MemoryExtent */
struct fmd_dc_mock_hold {
	uint8_t  tag[FMD_DC_TAG_LEN];
	uint8_t  region_id;
	uint16_t shared_seq;                /* given back by Enable Shared Access */
	uint64_t phys;                      /* offset from the region base */
	uint64_t len;
	struct fmd_dc_mock_hold *next;
};
```

- `fmd_ld` has the host's counters (`dc_extents_supported`,
  `dc_extents_available`, `dc_tags_supported`, `dc_tags_available`) and
  its 5603h state (`dc_ext_gen`, `dc_sync`, `dc_sync_err`).
- An extent's internal component Id is `<device>_h<host>_e<hex DPA>`.
  `fmd_dc_find_extent(dev, host, dpa)` finds it, and
  `fmd_dc_host_extent_count(dev, host, region)` counts a host's extents in
  a region. `fmd_dc_tag_find(dev, tag)` finds the row of a non-zero tag in
  a tag list that is known.

### Discovery and refresh (`src/dcd.c`)

After the existing Type 3 classification:

1. `cxlmi_cmd_fmapi_get_dcd_info()`, to every Type 3 device.
2. For each Host ID the device can see (SLD: 0; MLD and MHD: each LD):
   5601h, every page staged before the region table changes, then 5603h
   into a scratch list, applied when complete (Comment 6). A host that fails keeps its extents and is marked stale; the others
   are still read.
3. 5608h once, device-wide; each extent's tag is checked against it,
   and none is made pending from it (Comment 5).
4. After 5602h: steps 2 and 3 for every host. After 5604h or 5605h: step
   2 for that host, then step 3. After 5606h or 5607h: step 3. After DC
   event records: step 2 for each host they name, then step 3; a host is
   marked DIRTY when its record is read, before the record is cleared.
5. On every `--event-poll` tick, after the DC event logs are drained and
   whether or not they had records: step 2 for each host that is STALE or
   DIRTY, then step 3, and step 3 alone when the tag list could not be
   read (`fmd_dcd_retry_stale()`). A record that has been cleared is not
   read again, so nothing else would read such a host: it is read once a
   tick until a read completes. Devices whose DC event log is the host's are
   read too. With `--event-poll 0` there is no retry. The retry policy is
   cxl-fmd's; the specification has none for an FM.

All of it runs under `ctx->dcd_lock`; the refresh returns the first
failure.

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
`Actions` on `CXLLogicalDevice`. `Links/MemoryExtent` on `MemoryChunks` is
not served.
Tag conversion (16 bytes to UUID string and back) belongs in
`cci_redfish_map.c` next to the existing enum mappers.

### Mock topology (`src/mock.c`, `docs/mock-topology.md`)

A `dcd` object on any Type 3 device, so all of the above can be exercised
and validated without hardware; [mock-topology.md](mock-topology.md) has
the format. The mock host accepts added capacity immediately, so only a
topology file has an add the host has not answered: in the tag's
`PendingLDs`, and, for an extent the file declares `pending`, in the
mock's own Prescriptive release (the mock's choice); no extent shows
`Pending` `true`. The
mock builds the 5608h list a device would send from its extents, its FM
references and the file's `tags` (the mock's choice).

The mock gives a region a base for each host (`host_base_mb`, the mock's
choice: its length, decode length and block size are the region's). Where
capacity is, is its offset into the region, the same on every host that
has it: an extent of host *h* is at *h*'s base plus that offset. Two
hosts, or a host and what an FM reference keeps, have the same capacity
only as shared capacity, with the same non-zero Tag in a Sharable region;
anything else is a load error. `mock/dcd-shared-perhost.json` has capacity
shared at a different DPA on each host, other capacity at the same DPA on
two hosts, capacity only an FM reference keeps, and an add no host has
answered that 5603h does not list.

`tests/test_dcd.py` covers the lifecycle over Redfish (add Free,
Contiguous and across gaps, release TagBased with trimming and
Prescriptive, DELETE by Tag and by extent, region reconfiguration,
reference and unreference of a tag, shared access, the tag view);
`tests/discovery-tests.c` covers the FM-API side against fake endpoints
(two hosts at one DPA, per-host region bases, Decode Length, 5608h paging,
validity and LD 200, a host whose 5603h keeps changing, DC event records,
the Prescriptive payloads).

### Mock topology checks (`src/validate.c`)

Besides the regions (Table 8-347), a topology's extents are checked:

- An extent is in a region of its own host, from Region Base to Region
  Base + Region Decode Length, and aligned to that region's block size.
- One host's extents do not overlap: "Overlapping DPA ranges for multiple
  extents" makes an Extent List malformed (Sections 8.2.10.9.9.3 and
  8.2.10.9.9.4). Extents of different hosts may have the same DPA.
- "For extents that describe non-shareable regions, this field shall be
  0" (Shared Extent Sequence, Table 8-230); in a Sharable region a host's
  extents of a tag have the sequences 0 to n-1 once each. That n is the
  number of that host's extents of the tag is cxl-fmd's reading, as the
  table is an entry of one host's Extent List.
- A tag held by two hosts may not be in a region that is not Sharable on
  any of them: Enable Shared Access fails "When the tagged capacity is
  already mapped to any Host ID via a non-Sharable region" (Section
  7.6.7.6.5).

Logged as doubtful, and still loaded: an extent with no tag in a Sharable
region; two hosts' extents of the same tag and sequence with different
lengths (cxl-fmd's inference from "the same offsets reach the same data
accessed by all hosts", Table 8-230); a tag with no LD in either bitmap and
no FM reference ("the tag no longer exists", Section 9.13.3.2); a host past
LD 255.

---

## Open Questions

What the CXL specification or the DMTF material leaves open, and what
cxl-fmd does about each. Every choice below is cxl-fmd's (or the mock's,
where it says so), not the specification's.

1. **Units of the `Length` action parameter.** In the vendored DSP8010
   2026.2 schema, `Length` of `DynamicCapacityAdd` and
   `DynamicCapacityRelease` is an `Edm.Int64`, "The size of the dynamic
   capacity to add" (and "to release"), with no `Measures.Unit`
   annotation (`CXLLogicalDevice_v1.xml`), where `MemorySizeMiB` of the
   same resource is annotated `MiBy`. That the schema leaves it out does not make it
   MiB. Choice: `Length` is in MiB, as cxl-fmd's profile; it is converted
   to bytes with a check, and a value whose bytes do not fit is `400`.
   An exact byte count cannot be asked for, which matters only for a
   region with a block size below 1 MiB. A request to DMTF to state the
   unit is drafted, to be submitted by the maintainer (review F12.8 and
   DCD-T5b); the missing rows of DSP0288 are Open Question 26.
2. **Does 5603h list extents still pending for the host?** Section 9.13.3
   says the host's Extent List "does not contain extents that are still
   pending acceptance from the host"; 5603h (Table 7-71) does not say
   whether its list is that one. Choice: an extent 5603h lists is not
   pending (`Pending` `false`, `Enabled`), and an add an LD has not
   answered shows only in `Oem/Cxl/DynamicCapacityTags` (`PendingLDs`). No
   extent is made up. `tests/discovery-tests.c` covers both.
3. **Where is shared capacity in each host's DPA space?** "The DPA ranges
   exposed by the device to each host are independent of one another"
   (Section 9.13.3), and the read-only sharing example of Section 9.13.3.2
   has the second host given the tag by "specifying a Region that is
   Sharable and read-only", which need not be the first host's. Choice:
   shared
   capacity is known by an equal non-zero Tag, ordered by Shared Extent
   Sequence; equal DPAs on two hosts mean nothing. The mock gives each
   host its own base, but Enable Shared Access must name the region number
   the holders have the tag in (a device-imposed restriction of the mock's,
   which Section 7.6.7.6.5 allows for).
4. **Can Region Base and Decode Length differ per host?** 5602h sets only
   Block Size and Sanitize on Release, for every LD. Choice: whatever each
   host's 5601h reports is kept for that host; the mock's `host_base_mb`
   varies the base only.
5. **Which Tag is "no tag"?** CXL r4.0 gives a zero Tag no reserved
   meaning: no Tag field says what a Tag of all zeros is, and none sets a
   value aside for "no tag". The Tag field definitions, as written:
   - Dynamic Capacity Extent, Table 8-230, offset 10h: "Tag: Optional
     opaque context field utilized by implementations making use of the
     Dynamic Capacity feature. Within sharable regions, all extents
     targeting the same range this field is required and all Tag values
     shall be identical for all hosts sharing the same extent. See Section
     7.6.7.6.5 for how the Tag field is utilized while setting up
     sharing." (Section 7.6.7.6.5 names the tag only as what Enable
     Shared Access enables access to, "as designated by the specified tag
     value", and points to the examples of Section 9.13.3.2.)
   - Initiate Dynamic Capacity Add, Table 7-72, offset 0Ch: "Tag: Context
     field utilized by implementations that make use of the Dynamic
     Capacity feature. This field is reserved when the Selection Policy is
     set to Prescriptive."
   - Initiate Dynamic Capacity Release, Table 7-73, offset 0Ch: "Tag:
     Optional opaque context field utilized by implementations that make
     use of the Dynamic Capacity feature. This field is reserved when the
     Removal Policy is set to Prescriptive."
   - Dynamic Capacity Add Reference, Table 7-74, offset 00h: "Tag: Tag that
     is associated with the memory capacity to be preserved."
   - Dynamic Capacity Remove Reference, Table 7-75, offset 00h: "Tag: Tag
     that is associated with the memory capacity."
   - Dynamic Capacity Tag Information, Table 7-78, offset 00h: "Tag: Tag
     that is associated with the memory capacity."

   "Optional" says that an implementation need not use the field, not
   which value means it is not used; "required" in a sharable region does
   not say that the value is non-zero. So taking all zeros for no tag is
   **cxl-fmd's convention**, not the specification's; a device may use a
   zero Tag as an ordinary one. Choice (kept by the maintainer): all zeros
   is no tag. What follows from it:
   - An extent whose tag is all zeros has no `Tag` and no
     `FMReferencedExtent`, and is not shared capacity: `DELETE` releases
     it by itself, Prescriptive, even in a Sharable region (see
     [DELETE](#delete-of-a-memoryextent)).
   - Enable Shared Access, 5606h and 5607h with an empty or all-zero `Tag`
     are `400`, with nothing sent. An add with the other policies, and a
     Tag-based release, are not refused for it: the Tag is passed on as
     given, zeros too, and the device decides.
   - A row of the device's tag list (5608h) whose Tag is all zeros is
     served as the device lists it, in `Oem/Cxl/DynamicCapacityTags`, with
     `Tag` `""` and every extent of all-zero tag as its `Extents`, though
     those extents have no `Tag`; it gives no extent `FMReferencedExtent`.
6. **One host with accepted and pending capacity under one tag.** The
   bitmaps are per tag (Table 7-78), and do not say which extent, if any,
   is the add. Choice: no extent is made pending from them; the host's LD
   is in the tag's `AcceptedLDs` and `PendingLDs`, and its listed extents
   are `Pending` `false` and `Enabled` (Comment 5). While the tag list
   cannot be read, the regions and tagged extents are `Warning` and the
   view has no `Tags`, rather than showing nothing outstanding.
7. **Completion of an add or a release.** Whether 04h is sent for an add a
   host accepts none of, or 05h after a Forced Removal or a device's own
   reclaim, is not said; nor whether the FM's 04h records follow the order
   the host answered in. Choice: a record only makes cxl-fmd read the host
   again and take its counts (78h, 7Ch); one with no capacity is taken to
   be a host that accepted none of the add of its Tag, or of the one add
   followed on that host when its Tag is all zeros. Completion is judged
   on 5603h and the Pending Reference Bitmap, by a `Task` (see [Tasks of
   an add or a release](#tasks-of-an-add-or-a-release)).
8. **Which log carries 04h and 05h on an MLD or a Multi-Headed device.**
   Choice: the device's FM-side Dynamic Capacity Event Log, read through
   its FM CCI; a Multi-Headed device's records are not kept per LD.
9. **Counters of capacity only an FM reference keeps, and of shared
   extents.** Number of Available Extents and Tags (Section 9.13.3.3) do
   not say which LD such capacity counts against. Choice: on hardware the
   device's counters are shown as it reports them. The mock counts a
   shared extent once for each host that has it, and capacity only an FM
   reference keeps against every host that sees its region.
10. **Does capacity only an FM reference keeps block 5602h?** 5602h is
    processed "only when all capacity has been released from the region on
    all LDs" (Section 7.6.7.6.3). Choice: on hardware it is not refused
    for it, as no host lists it and its region is unknown; the device
    decides. The mock refuses it, `409`.
11. **Is a Tag-based `Length` short of a host's sharable tag the
    "portion" of Section 7.6.7.6.6?** The sentence names the Extent List.
    Choice: the mock refuses it with `400`. DELETE always sends the host's
    whole capacity of the tag, and does not meet the question.
12. **Does a Prescriptive release of some of a host's extents of a
    sharable tag fail?** Choice: the mock refuses it with `400` as the
    "portion" of Section 7.6.7.6.6. DELETE releases such an extent by Tag
    instead.
13. **Do the per-extent Tags of a Prescriptive 5604h become the extents'
    tags?** The request's own Tag is "reserved when the Selection Policy is
    set to Prescriptive" (Table 7-72). Choice: on hardware what 5603h then
    lists is taken; a Prescriptive add cannot be named through Redfish
    anyway (see Initiate Dynamic Capacity Add).
14. **Total Number of Tags of 5608h** has Max Tags' text (Table 7-77).
    Choice: the read stops at it or at an empty page, at most 4096 tags,
    and starts again on a new Generation Number.
15. **Enable Shared Access has no support bit.** Bit[3] of the add
    policies "Must be 0" (Table 7-65). Choice: it is never listed in
    `AddCapacityPoliciesSupported`, and always sent.
16. **Host IDs past 255.** The Host ID is two bytes, the bitmaps end at LD
    255. Choice: such a host keeps its extents, gets nothing from the tag
    list, and DC event records naming it are logged and left.
17. **Host ID of a Multi-Headed DCD**: the pool LD-ID (see
    [Host ID Resolution](#host-id-resolution)), not verified; the `<ld>`
    of extent Ids inherits it. Choice (the maintainer's): DC writes on a
    Multi-Headed DCD are sent with it all the same, with no gate and no
    option to refuse them (see
    [Writes on a Multi-Headed DCD](#writes-on-a-multi-headed-dcd)). Until
    hardware or a CXL clarification settles it, a write on an MH-MLD may
    reach a head-local LD of the same number, another host's.
18. **One `MemoryExtent` per host, or per shared capacity, and what
    `ExtentOffsetMiB` is when hosts differ.** The schema says "the device
    address range". Choice: one resource per host's extent, its offset in
    that host's DPA space, one region link.
19. **What POST and DELETE on `MemoryExtents` mean, the Tag's format.**
    The schema does not say. Choice: POST is not served; DELETE releases
    from the extent's host only; the Tag is a UUID string. A DELETE of an
    extent with a tag in a Sharable region releases every extent of that
    host with the tag, as Section 7.6.7.6.6 makes a release of a
    "portion" of sharable capacity Invalid Input (see
    [DELETE](#delete-of-a-memoryextent)). The external design review
    (DCD-Q A3) asked for a narrower contract, and refusing such a DELETE
    with `409` was proposed; the maintainer kept the wider release, and
    documented it. A client that wants something else uses
    `DynamicCapacityRelease`.
20. **No DMTF resource for a tag.** Choice: `Oem/Cxl/DynamicCapacityTags`
    on `PCIeDevice`, described by cxl-fmd's Oem schema `Cxl_v1.xml`
    (served at `/redfish/v1/Schemas/Cxl_v1.xml`). A request to DMTF for a
    standard place for the tag list (the holders and pending LDs of each
    tag, and the FM reference of a tag no host holds, which no
    `MemoryExtent` can carry) is drafted, to be submitted by the
    maintainer.
21. **The earlier extent Ids `ext_<hex DPA>`.** Choice: `404`, no alias:
    they are ambiguous across hosts, and Ids are the service's to assign.
    cxl-fmd keeps no release notes; this document is where it is said.
22. **When the mock host accepts.** Choice: at once, so its tasks are
    `Completed` on the next turn of the event loop, unless the device's
    `host_response` in the topology file says it answers after so many
    turns, in part, not at all, or never (see
    [docs/mock-topology.md](mock-topology.md)). Pending state of a file's
    own extents comes from it too (`per_host` `pending`, `tags`
    `pending_hosts`).
23. **An extent being released.** Section 9.13.3 says of the Extent
    List a host sees: "The Extent List does not contain extents that are
    still pending acceptance from the host via the Add Dynamic Capacity
    Response command (see Section 8.2.10.9.9.3). Similarly, the Extent
    List does contain extents that are still pending release acceptance
    from the host via the Release Dynamic Capacity command (see Section
    8.2.10.9.9.4)." So an extent the device has asked the host to release
    stays in the host's list until the host accepts the release with
    Release Dynamic Capacity. 5605h says only that it "shall complete
    when the device initiates the Remove Capacity procedure" (Section
    7.6.7.6.6). Choice: there is no "pending release" state; an extent
    stays, and is served, until its host's 5603h no longer lists it, and
    the release's `Task` runs until then (see [Tasks of an add or a
    release](#tasks-of-an-add-or-a-release)). That
    5603h, the "Extent list for the specified host as defined in Table
    8-230" (Table 7-71), lists what Section 9.13.3 says of the host's list
    is cxl-fmd's reading: the specification does not say they are one
    list. Nor does an extent leaving the list say that the host's
    software has stopped using the memory; it is the protocol state
    between the device and the host, and taking pages or mappings out of
    use is the host's (a host agent's) to coordinate.

    Two other sentences are not relied on. 5605h fails with Invalid
    Input "When the command is sent with an invalid Host ID, or an invalid
    region number" (Section 7.6.7.6.6), but its request (Table 7-73) has
    no Region Number field. The `Length` of 5604h and of 5605h is "Always
    a multiple of the configured Region Block Size returned in Get DCD
    Info" (Tables 7-72 and 7-73), and Get DCD Info returns only a
    "Supported Block Size Mask" for each region (Table 7-65), not a
    configured size. Choice: an add's `Length` is checked against the
    block size the host's 5601h reports for the region; a release's is
    left to the device.
24. **GFD.** The GFAM equivalents (4907h-490Bh) share the same shapes but
    address the device via PBR. Out of scope here and listed separately in
    `TODO`.
25. **DC sizes below 1 MiB.** A region's block size may be any power of
    two from 2^6 bytes ("Bits[5:0] and bits[63:52] shall be 0", Table
    7-65), and its Region Length, extent offsets and extent lengths follow
    the block size (Tables 8-347 and 8-230). The DMTF properties they map
    to are integer MiB (`BlockSizeMiB`, `RegionSizeMiB`,
    `ExtentOffsetMiB`, `ExtentSizeMiB`, `MemorySizeMiB`). Total Dynamic
    Capacity ("in multiples of 256 MB", Table 7-65) and Region Base
    ("aligned to 256 MB", Table 8-347) are always whole MiB. Choice: the
    bytes in `Oem.Cxl` (Comment 3). A request to DMTF for byte-granular
    properties is drafted, to be submitted by the maintainer (review
    F12.8).
26. **No DSP0288 mapping for 55h and 56h.** DSP0288 1.4.0 Table 2 maps no
    command of the Multi-Headed Devices (55h) or DCD Management (56h)
    command sets, and its normative reference is CXL r3.0, of which "only
    the edition cited applies". Choice: Table 3 above for 56h, and the
    mapping of "MHD Memory Model" in [Redfish-API.md](Redfish-API.md) for
    55h, both cxl-fmd's. A request to DMTF to add the rows and update the
    reference is drafted, to be submitted by the maintainer (review
    DCD-T5b).
