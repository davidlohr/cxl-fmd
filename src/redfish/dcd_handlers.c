/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Dynamic Capacity Redfish Handlers
 *
 * Maps the FM-API DCD Management command set (56h) to Redfish, following
 * docs/DCD-FM-API.md Table 3:
 *
 *   PCIeDevice.CXLDevice/DynamicCapacity                 <- Get DCD Info (5600h)
 *   PCIeDevice/MemoryExtents/{ext}                       <- Get DC Region Extent Lists (5603h)
 *   PCIeDevice.CXLReferenceDCMemory action               -> DC Add/Remove Reference (5606h/5607h)
 *   CXLLogicalDevice/MemoryRegions/{region}              <- Get Host DC Region Config (5601h)
 *   PATCH CXLLogicalDevice/MemoryRegions/{region}        -> Set DC Region Config (5602h)
 *   CXLLogicalDevice.DynamicCapacityAdd action           -> Initiate DC Add (5604h)
 *   CXLLogicalDevice.DynamicCapacityRelease action       -> Initiate DC Release (5605h)
 *   DELETE PCIeDevice/MemoryExtents/{ext}                -> Initiate DC Release, Prescriptive
 */

#include "handlers_internal.h"

/*
 * ============================================================================
 * Helpers
 * ============================================================================
 */

static struct fmd_component *find_dcd_device(struct fmd_ctx *ctx,
					     const char *chassis_id,
					     const char *dev_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);

	if (!comp || !fmd_is_memdev(comp) || strcmp(chassis_id, dev_id) != 0)
		return NULL;
	if (!fmd_memdev_dcd(comp))
		return NULL;
	return comp;
}

/* Redfish Id of an extent: ext_<hex DPA> */
static void extent_rf_id(struct fmd_dc_extent *ext, char *buf, size_t len)
{
	snprintf(buf, len, "ext_%" PRIx64, ext->start_dpa);
}

static struct fmd_dc_extent *find_extent_by_rf_id(struct fmd_component *dev,
						  const char *ext_id)
{
	char *end;
	uint64_t dpa;

	if (!ext_id || strncmp(ext_id, "ext_", 4) != 0 || ext_id[4] == '\0')
		return NULL;

	errno = 0;
	dpa = strtoull(ext_id + 4, &end, 16);
	if (errno || *end != '\0')
		return NULL;

	return fmd_dc_find_extent(dev, dpa);
}

/* Resolve a MemoryExtent @odata.id link to the extent under this device */
static struct fmd_dc_extent *find_extent_by_link(struct fmd_component *dev,
						 const char *odata_id)
{
	const char *p;

	if (!odata_id)
		return NULL;
	p = strstr(odata_id, "/MemoryExtents/");
	if (!p)
		return NULL;
	return find_extent_by_rf_id(dev, p + strlen("/MemoryExtents/"));
}

/* CXLLogicalDevice index that owns a host's regions (for building links) */
static int host_ld_index(struct fmd_component *dev, int host_id)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);

	if (!owner)
		return -1;
	if (owner->type == FMD_COMPONENT_LD)
		return fmd_to_ld(owner)->ld_id;
	return 0;
}

static void region_uri(const char *chassis_id, const char *dev_id, int ld_idx,
		       uint8_t region_id, char *buf, size_t len)
{
	snprintf(buf, len,
		 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d/MemoryRegions/%u",
		 chassis_id, dev_id, ld_idx, region_id);
}

static void extent_uri(const char *chassis_id, const char *dev_id,
		       struct fmd_dc_extent *ext, char *buf, size_t len)
{
	char id[32];

	extent_rf_id(ext, id, sizeof(id));
	snprintf(buf, len, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/%s",
		 chassis_id, dev_id, id);
}

/*
 * Parse an ExtentList action parameter into an array of extents.
 * Returns the count, 0 if the parameter is absent, -1 on a bad link.
 */
static int parse_extent_list(struct fmd_component *dev, json_t *root,
			     struct fmd_dc_extent ***out)
{
	json_t *list = json_object_get(root, "ExtentList");
	struct fmd_dc_extent **extents;
	size_t idx, n;
	json_t *item;

	*out = NULL;
	if (!list)
		return 0;
	if (!json_is_array(list))
		return -1;

	n = json_array_size(list);
	if (n == 0)
		return -1;

	extents = calloc(n, sizeof(*extents));
	if (!extents)
		return -1;

	json_array_foreach(list, idx, item) {
		json_t *link = json_is_object(item) ? json_object_get(item, "@odata.id") : NULL;

		extents[idx] = find_extent_by_link(dev, link ? json_string_value(link) : NULL);
		if (!extents[idx]) {
			free(extents);
			return -1;
		}
	}

	*out = extents;
	return (int)n;
}

/*
 * ============================================================================
 * PCIeDevice additions
 * ============================================================================
 */

/*
 * CXLDevice/DynamicCapacity from Get DCD Info (5600h).
 * Returns NULL when the device is not a DCD.
 */
json_t *fmd_redfish_dc_capabilities(struct fmd_ctx *ctx, struct fmd_component *dev,
				    const char *chassis_id, const char *dev_id)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	json_t *result, *add, *release, *blocks, *sanitize;

	(void)ctx;
	if (!dcd)
		return NULL;

	add = json_array();
	for (int v = 0; fmd_dc_policy_name(false, v); v++) {
		if (dcd->add_policies & fmd_dc_policy_cap_bit(false, v))
			json_array_append_new(add, json_string(fmd_dc_policy_name(false, v)));
	}

	release = json_array();
	for (int v = 0; fmd_dc_policy_name(true, v); v++) {
		if (dcd->release_policies & fmd_dc_policy_cap_bit(true, v))
			json_array_append_new(release, json_string(fmd_dc_policy_name(true, v)));
	}

	blocks = json_array();
	sanitize = json_array();
	for (int r = 0; r < dcd->num_regions && r < FMD_MAX_DC_REGIONS; r++) {
		json_t *sizes = json_array();

		/* Bit n of the mask means a 2^n byte block; report MiB and up */
		for (int n = 20; n < 64; n++) {
			if (dcd->blk_sz_mask[r] & (1ULL << n))
				json_array_append_new(sizes,
					json_integer((json_int_t)(1ULL << (n - 20))));
		}

		json_array_append_new(blocks, json_pack("{s:i, s:o}",
			"RegionNumber", r,
			"BlockSizeMiB", sizes));
		json_array_append_new(sanitize, json_pack("{s:i, s:b}",
			"RegionNumber", r,
			"SanitizationOnReleaseSupported",
			(dcd->sanitize_cfg_mask & (1 << r)) ? 1 : 0));
	}

	result = json_pack(
		"{s:i, s:i, s:I, s:o, s:o, s:o, s:o, s:{s:s}}",
		"MaxHosts", (int)dcd->num_hosts,
		"MaxDynamicCapacityRegions", (int)dcd->num_regions,
		"TotalDynamicCapacityMiB", (json_int_t)(dcd->total_dc_capacity / FMD_MIB),
		"AddCapacityPoliciesSupported", add,
		"ReleaseCapacityPoliciesSupported", release,
		"MemoryBlockSizesSupported", blocks,
		"SanitizationOnReleaseSupport", sanitize,
		"MemoryExtents", "@odata.id",
			rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents",
			       chassis_id, dev_id));

	return result;
}

json_t *fmd_redfish_dc_pcie_actions(const char *chassis_id, const char *dev_id)
{
	return json_pack("{s:{s:s, s:s}}",
		"#PCIeDevice.CXLReferenceDCMemory",
		"target", rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/Actions/PCIeDevice.CXLReferenceDCMemory",
				 chassis_id, dev_id),
		"title", "CXLReferenceDCMemory");
}

/*
 * ============================================================================
 * CXLLogicalDevice additions
 * ============================================================================
 */

void fmd_redfish_dc_logical_device_props(struct fmd_ctx *ctx, struct fmd_component *dev,
					 int ld_idx, const char *chassis_id,
					 const char *dev_id, const char *ld_id,
					 json_t *result)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *owner;
	uint32_t ext_sup, ext_avail, tag_sup, tag_avail;
	int host_id;

	(void)ctx;
	if (!dcd)
		return;

	host_id = fmd_dc_host_id(dev, ld_idx);
	owner = host_id >= 0 ? fmd_dc_host_component(dev, host_id) : NULL;

	/* MemoryRegions collection link (CXLLogicalDevice v1.1) */
	json_object_set_new(result, "MemoryRegions",
		rf_nav("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/MemoryRegions",
		       chassis_id, dev_id, ld_id));

	/* Extent/tag counters (CXLLogicalDevice v1.4) from the 5601h trailer */
	if (owner && owner->type == FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(owner);
		ext_sup = ld->dc_extents_supported;
		ext_avail = ld->dc_extents_available;
		tag_sup = ld->dc_tags_supported;
		tag_avail = ld->dc_tags_available;
	} else {
		ext_sup = dcd->extents_supported;
		ext_avail = dcd->extents_available;
		tag_sup = dcd->tags_supported;
		tag_avail = dcd->tags_available;
	}
	json_object_set_new(result, "TotalSupportedExtents", json_integer(ext_sup));
	json_object_set_new(result, "AvailableExtents", json_integer(ext_avail));
	json_object_set_new(result, "TotalSupportedTags", json_integer(tag_sup));
	json_object_set_new(result, "AvailableTags", json_integer(tag_avail));

	/* Actions (CXLLogicalDevice v1.4) */
	json_t *actions = json_object_get(result, "Actions");
	if (!actions) {
		actions = json_object();
		json_object_set_new(result, "Actions", actions);
	}
	json_object_set_new(actions, "#CXLLogicalDevice.DynamicCapacityAdd",
		json_pack("{s:s, s:s}",
			"target", rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/Actions/CXLLogicalDevice.DynamicCapacityAdd",
					 chassis_id, dev_id, ld_id),
			"title", "DynamicCapacityAdd"));
	json_object_set_new(actions, "#CXLLogicalDevice.DynamicCapacityRelease",
		json_pack("{s:s, s:s}",
			"target", rf_uri(1, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/Actions/CXLLogicalDevice.DynamicCapacityRelease",
					 chassis_id, dev_id, ld_id),
			"title", "DynamicCapacityRelease"));
}

/*
 * ============================================================================
 * MemoryExtents
 * ============================================================================
 */

json_t *fmd_redfish_dc_memory_extents(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_component *child;
	struct rf_collection coll;

	if (!dev)
		return NULL;

	rf_collection_init(&coll,
		rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents",
		       chassis_id, dev_id),
		"#MemoryExtentCollection.MemoryExtentCollection",
		"Memory Extent Collection");

	fmd_topology_rdlock(&ctx->topology);
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		char id[32];

		extent_rf_id(fmd_to_dc_extent(child), id, sizeof(id));
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/%s",
				  chassis_id, dev_id, id);
	}
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

json_t *fmd_redfish_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ext_id)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dcd_info *dcd;
	struct fmd_dc_extent *ext;
	char uri[512];
	char tag_str[40];
	json_t *result, *regions;

	if (!dev)
		return NULL;
	dcd = fmd_memdev_dcd(dev);

	fmd_topology_rdlock(&ctx->topology);
	ext = find_extent_by_rf_id(dev, ext_id);
	if (!ext) {
		fmd_topology_rdunlock(&ctx->topology);
		return NULL;
	}

	extent_uri(chassis_id, dev_id, ext, uri, sizeof(uri));
	fmd_dc_tag_to_str(ext->tag, tag_str, sizeof(tag_str));

	result = json_pack(
		"{s:s, s:s, s:s, s:s, s:I, s:I, s:s, s:I, s:b, s:o}",
		"@odata.id", uri,
		"@odata.type", "#MemoryExtent.v1_0_0.MemoryExtent",
		"Id", ext_id,
		"Name", "Dynamic Capacity Extent",
		"ExtentOffsetMiB", (json_int_t)(ext->start_dpa / FMD_MIB),
		"ExtentSizeMiB", (json_int_t)(ext->len / FMD_MIB),
		"Tag", tag_str,
		"SequenceNumber", (json_int_t)ext->shared_seq,
		"FMReferencedExtent", ext->fm_referenced ? 1 : 0,
		"Status", rf_status(ext->pending_mask ? "Starting" : "Enabled", "OK"));

	/* Links/MemoryRegions: every host's region this extent is exposed through */
	regions = json_array();
	for (int h = 0; h < dcd->num_hosts && h < FMD_MAX_DC_HOSTS; h++) {
		int ld_idx;
		char ruri[512];

		if (!(ext->host_mask & fmd_dc_host_bit(h)))
			continue;
		ld_idx = host_ld_index(dev, h);
		if (ld_idx < 0)
			continue;
		region_uri(chassis_id, dev_id, ld_idx, ext->region_id, ruri, sizeof(ruri));
		json_array_append_new(regions, rf_link("%s", ruri));
	}
	json_object_set_new(result, "Links", json_pack("{s:o}", "MemoryRegions", regions));

	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * DELETE on an extent releases it from every host that holds it
 * (Initiate DC Release, Prescriptive, one extent per host).
 */
int fmd_redfish_delete_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
					const char *dev_id, const char *ext_id,
					const char **msg)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dcd_info *dcd;
	struct fmd_dc_extent *ext;
	uint64_t dpa, mask;
	bool sanitize = false;

	if (!dev) {
		*msg = "Device is not a dynamic capacity device";
		return -ENOENT;
	}
	dcd = fmd_memdev_dcd(dev);

	ext = find_extent_by_rf_id(dev, ext_id);
	if (!ext) {
		*msg = "Memory extent not found";
		return -ENOENT;
	}

	if (ext->host_mask == 0) {
		*msg = "Extent is held only by the Fabric Manager reference; use CXLReferenceDCMemory";
		return -EBUSY;
	}

	dpa = ext->start_dpa;
	mask = ext->host_mask;

	for (int h = 0; h < dcd->num_hosts && h < FMD_MAX_DC_HOSTS; h++) {
		struct fmd_dc_region *region;
		struct fmd_dc_extent *cur;
		int rc;

		if (!(mask & fmd_dc_host_bit(h)))
			continue;

		/* The extent may already be gone if a previous host freed it */
		cur = fmd_dc_find_extent(dev, dpa);
		if (!cur)
			break;

		region = fmd_dc_find_region(dev, h, cur->region_id);
		sanitize = region ? region->sanitize_on_release : false;

		rc = fmd_dc_release(ctx, dev, h, FMD_DC_REL_POLICY_PRESCRIPTIVE,
				    false, sanitize, 0, cur->tag, &cur, 1);
		if (rc) {
			*msg = "Dynamic capacity release failed";
			return rc;
		}
	}

	return 0;
}

/*
 * ============================================================================
 * MemoryRegions
 * ============================================================================
 */

/* Resolve {ld} to the host id; returns -1 if the LD has no DC view */
static int resolve_host(struct fmd_component *dev, const char *ld_id)
{
	int ld_idx = parse_ld_id(ld_id);

	if (ld_idx < 0)
		return -1;
	return fmd_dc_host_id(dev, ld_idx);
}

json_t *fmd_redfish_dc_memory_regions(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id, const char *ld_id)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_component *owner, *child;
	struct rf_collection coll;
	int host_id;

	if (!dev)
		return NULL;
	host_id = resolve_host(dev, ld_id);
	if (host_id < 0)
		return NULL;

	rf_collection_init(&coll,
		rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/MemoryRegions",
		       chassis_id, dev_id, ld_id),
		"#MemoryRegionCollection.MemoryRegionCollection",
		"Memory Region Collection");

	/* List in region-number order (children are stored newest first) */
	fmd_topology_rdlock(&ctx->topology);
	owner = fmd_dc_host_component(dev, host_id);
	for (int r = 0; owner && r < FMD_MAX_DC_REGIONS; r++) {
		fmd_for_each_child_type(owner, child, FMD_COMPONENT_DC_REGION) {
			if (fmd_to_dc_region(child)->region_id != r)
				continue;
			rf_collection_add(&coll,
				"/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/MemoryRegions/%u",
				chassis_id, dev_id, ld_id, (unsigned)r);
			break;
		}
	}
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

json_t *fmd_redfish_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ld_id,
				     const char *region_id)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_component *child;
	struct fmd_dc_region *region;
	json_t *result, *extents;
	char uri[512];
	char name[32];
	int host_id, rnum;
	int count = 0;

	if (!dev)
		return NULL;
	host_id = resolve_host(dev, ld_id);
	rnum = safe_parse_int(region_id);
	if (host_id < 0 || rnum < 0 || rnum >= FMD_MAX_DC_REGIONS)
		return NULL;

	fmd_topology_rdlock(&ctx->topology);
	region = fmd_dc_find_region(dev, host_id, (uint8_t)rnum);
	if (!region) {
		fmd_topology_rdunlock(&ctx->topology);
		return NULL;
	}

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/MemoryRegions/%s",
		 chassis_id, dev_id, ld_id, region_id);
	snprintf(name, sizeof(name), "DC Region %d", rnum);

	/* Extents visible to this host, as links (inline fields are deprecated) */
	extents = json_array();
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		char euri[512];

		if (ext->region_id != region->region_id ||
		    !(ext->host_mask & fmd_dc_host_bit(host_id)))
			continue;

		extent_uri(chassis_id, dev_id, ext, euri, sizeof(euri));
		json_array_append_new(extents, json_pack("{s:b, s:{s:s}}",
			"Pending", (ext->pending_mask & fmd_dc_host_bit(host_id)) ? 1 : 0,
			"ExtentLink", "@odata.id", euri));
		count++;
	}

	result = json_pack(
		"{s:s, s:s, s:s, s:s,"
		" s:s, s:i, s:I, s:I, s:I,"
		" s:b, s:b, s:b, s:b, s:b, s:b,"
		" s:i, s:o, s:o}",
		"@odata.id", uri,
		"@odata.type", "#MemoryRegion.v1_1_0.MemoryRegion",
		"Id", region_id,
		"Name", name,
		"RegionType", "Dynamic",
		"RegionNumber", rnum,
		"RegionBaseOffsetMiB", (json_int_t)(region->base_dpa / FMD_MIB),
		"RegionSizeMiB", (json_int_t)(region->region_len / FMD_MIB),
		"BlockSizeMiB", (json_int_t)(region->block_size / FMD_MIB),
		"ShareableRegion", (region->flags & FMD_DC_REGION_SHAREABLE) ? 1 : 0,
		"NonVolatileRegion", (region->flags & FMD_DC_REGION_NONVOLATILE) ? 1 : 0,
		"HardwareManagedCoherencyRegion", (region->flags & FMD_DC_REGION_HW_COHERENT) ? 1 : 0,
		"ReadOnlyRegion", (region->flags & FMD_DC_REGION_READ_ONLY) ? 1 : 0,
		"InterconnectSpecificDCManagement",
			(region->flags & FMD_DC_REGION_INTERCONNECT_DCM) ? 1 : 0,
		"SanitizeOnRelease", region->sanitize_on_release ? 1 : 0,
		"ExtentsCount", count,
		"MemoryExtents", extents,
		"Status", rf_status("Enabled", "OK"));

	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * PATCH MemoryRegion -> Set DC Region Configuration (5602h)
 * Writable: BlockSizeMiB, SanitizeOnRelease.
 */
int fmd_redfish_patch_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				       const char *dev_id, const char *ld_id,
				       const char *region_id, const char *body,
				       size_t body_size, const char **msg)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dc_region *region;
	json_t *root, *val;
	json_error_t error;
	uint64_t block_size;
	bool sanitize;
	const char *key;
	int host_id, rnum, rc;

	if (!dev) {
		*msg = "Device is not a dynamic capacity device";
		return -ENOENT;
	}
	host_id = resolve_host(dev, ld_id);
	rnum = safe_parse_int(region_id);
	if (host_id < 0 || rnum < 0 || rnum >= FMD_MAX_DC_REGIONS) {
		*msg = "Memory region not found";
		return -ENOENT;
	}
	region = fmd_dc_find_region(dev, host_id, (uint8_t)rnum);
	if (!region) {
		*msg = "Memory region not found";
		return -ENOENT;
	}

	root = json_loadb(body, body_size, 0, &error);
	if (!root || !json_is_object(root)) {
		json_decref(root);
		*msg = "Invalid JSON body";
		return -EINVAL;
	}

	block_size = region->block_size;
	sanitize = region->sanitize_on_release;

	json_object_foreach(root, key, val) {
		if (strcmp(key, "BlockSizeMiB") == 0) {
			if (!json_is_integer(val) || json_integer_value(val) <= 0) {
				json_decref(root);
				*msg = "BlockSizeMiB must be a positive integer";
				return -EINVAL;
			}
			block_size = (uint64_t)json_integer_value(val) * FMD_MIB;
		} else if (strcmp(key, "SanitizeOnRelease") == 0) {
			if (!json_is_boolean(val)) {
				json_decref(root);
				*msg = "SanitizeOnRelease must be a boolean";
				return -EINVAL;
			}
			sanitize = json_is_true(val);
		} else if (key[0] == '@') {
			continue;  /* OData annotations are harmless */
		} else {
			json_decref(root);
			*msg = "Property is not writable";
			return -EINVAL;
		}
	}
	json_decref(root);

	rc = fmd_dc_set_region_config(ctx, dev, region, block_size, sanitize);
	switch (rc) {
	case 0:
		return 0;
	case -EINVAL:
		*msg = "Block size or sanitize setting not supported for this region";
		return rc;
	case -EBUSY:
		*msg = "Region has extents allocated; release them before reconfiguring";
		return rc;
	default:
		*msg = "Set DC Region Configuration failed";
		return rc;
	}
}

/*
 * ============================================================================
 * Actions
 * ============================================================================
 */

/*
 * POST CXLLogicalDevice.DynamicCapacityAdd -> Initiate DC Add (5604h)
 */
int fmd_redfish_action_dc_add(struct fmd_ctx *ctx, const char *chassis_id,
			      const char *dev_id, const char *ld_id,
			      const char *body, size_t body_size, const char **msg)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dc_extent **extents = NULL;
	json_t *root, *val;
	json_error_t error;
	uint8_t tag[FMD_DC_TAG_LEN] = {0};
	uint64_t length = 0;
	bool have_tag = false;
	int host_id, policy, region = -1, ext_count = 0, rc;

	if (!dev) {
		*msg = "Device is not a dynamic capacity device";
		return -ENOENT;
	}
	host_id = resolve_host(dev, ld_id);
	if (host_id < 0) {
		*msg = "Logical device not found or not bound to a host";
		return -ENOENT;
	}

	root = json_loadb(body, body_size, 0, &error);
	if (!root || !json_is_object(root)) {
		json_decref(root);
		*msg = "Invalid JSON body";
		return -EINVAL;
	}

	policy = fmd_dc_policy_from_str(false, json_string_value(json_object_get(root, "Policy")));
	if (policy < 0) {
		json_decref(root);
		*msg = "Policy must be Free, Contiguous, Prescriptive or EnableSharedAccess";
		return -EINVAL;
	}

	val = json_object_get(root, "RegionNumber");
	if (val) {
		if (!json_is_integer(val) || json_integer_value(val) < 0 ||
		    json_integer_value(val) >= FMD_MAX_DC_REGIONS) {
			json_decref(root);
			*msg = "RegionNumber out of range";
			return -EINVAL;
		}
		region = (int)json_integer_value(val);
	}

	val = json_object_get(root, "Length");
	if (val) {
		if (!json_is_integer(val) || json_integer_value(val) <= 0) {
			json_decref(root);
			*msg = "Length must be a positive integer (MiB)";
			return -EINVAL;
		}
		length = (uint64_t)json_integer_value(val) * FMD_MIB;
	}

	val = json_object_get(root, "Tag");
	if (val) {
		if (!json_is_string(val) || fmd_dc_tag_from_str(json_string_value(val), tag)) {
			json_decref(root);
			*msg = "Tag must be a UUID string";
			return -EINVAL;
		}
		have_tag = true;
	}

	ext_count = parse_extent_list(dev, root, &extents);
	if (ext_count < 0) {
		json_decref(root);
		*msg = "ExtentList must be a non-empty array of MemoryExtent links on this device";
		return -EINVAL;
	}
	json_decref(root);

	/* Conditional parameter requirements per CXLLogicalDevice v1.4 */
	switch (policy) {
	case FMD_DC_POLICY_FREE:
	case FMD_DC_POLICY_CONTIGUOUS:
		if (region < 0 || length == 0 || !have_tag) {
			free(extents);
			*msg = "RegionNumber, Length and Tag are required for this Policy";
			return -EINVAL;
		}
		break;
	case FMD_DC_POLICY_SHARED_ACCESS:
		if (region < 0 || !have_tag) {
			free(extents);
			*msg = "RegionNumber and Tag are required for EnableSharedAccess";
			return -EINVAL;
		}
		break;
	case FMD_DC_POLICY_PRESCRIPTIVE:
		if (ext_count == 0) {
			*msg = "ExtentList is required for Prescriptive";
			return -EINVAL;
		}
		break;
	}

	rc = fmd_dc_add(ctx, dev, host_id, (uint8_t)policy, (uint8_t)(region < 0 ? 0 : region),
			length, tag, extents, ext_count);
	free(extents);

	switch (rc) {
	case 0:
		return 0;
	case -EINVAL:
		*msg = "Policy not supported by the device, or Length is not a block-size multiple";
		return rc;
	case -ENOSPC:
		*msg = "Insufficient free capacity in the region";
		return rc;
	case -ENOENT:
		*msg = "Region or extents not found for this host";
		return rc;
	case -EEXIST:
		*msg = "Host already has access to an extent in ExtentList";
		return rc;
	case -EBUSY:
		*msg = "Extent is owned by another host and the region is not shareable";
		return rc;
	default:
		*msg = "Initiate Dynamic Capacity Add failed";
		return rc;
	}
}

/*
 * POST CXLLogicalDevice.DynamicCapacityRelease -> Initiate DC Release (5605h)
 */
int fmd_redfish_action_dc_release(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *dev_id, const char *ld_id,
				  const char *body, size_t body_size, const char **msg)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dc_extent **extents = NULL;
	json_t *root, *val;
	json_error_t error;
	uint8_t tag[FMD_DC_TAG_LEN] = {0};
	uint64_t length = 0;
	bool forced, sanitize, have_tag = false, have_length = false;
	int host_id, policy, ext_count = 0, rc;

	if (!dev) {
		*msg = "Device is not a dynamic capacity device";
		return -ENOENT;
	}
	host_id = resolve_host(dev, ld_id);
	if (host_id < 0) {
		*msg = "Logical device not found or not bound to a host";
		return -ENOENT;
	}

	root = json_loadb(body, body_size, 0, &error);
	if (!root || !json_is_object(root)) {
		json_decref(root);
		*msg = "Invalid JSON body";
		return -EINVAL;
	}

	policy = fmd_dc_policy_from_str(true, json_string_value(json_object_get(root, "Policy")));
	if (policy < 0) {
		json_decref(root);
		*msg = "Policy must be TagBased or Prescriptive";
		return -EINVAL;
	}

	val = json_object_get(root, "ForcedRemoval");
	if (!val || !json_is_boolean(val)) {
		json_decref(root);
		*msg = "ForcedRemoval (boolean) is required";
		return -EINVAL;
	}
	forced = json_is_true(val);

	val = json_object_get(root, "SanitizeOnRelease");
	if (!val || !json_is_boolean(val)) {
		json_decref(root);
		*msg = "SanitizeOnRelease (boolean) is required";
		return -EINVAL;
	}
	sanitize = json_is_true(val);

	val = json_object_get(root, "Length");
	if (val) {
		if (!json_is_integer(val) || json_integer_value(val) < 0) {
			json_decref(root);
			*msg = "Length must be a non-negative integer (MiB, 0 = all)";
			return -EINVAL;
		}
		length = (uint64_t)json_integer_value(val) * FMD_MIB;
		have_length = true;
	}

	val = json_object_get(root, "Tag");
	if (val) {
		if (!json_is_string(val) || fmd_dc_tag_from_str(json_string_value(val), tag)) {
			json_decref(root);
			*msg = "Tag must be a UUID string";
			return -EINVAL;
		}
		have_tag = true;
	}

	ext_count = parse_extent_list(dev, root, &extents);
	if (ext_count < 0) {
		json_decref(root);
		*msg = "ExtentList must be a non-empty array of MemoryExtent links on this device";
		return -EINVAL;
	}
	json_decref(root);

	if (policy == FMD_DC_REL_POLICY_TAG_BASED && (!have_tag || !have_length)) {
		free(extents);
		*msg = "Tag and Length are required for TagBased";
		return -EINVAL;
	}
	if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE && ext_count == 0) {
		*msg = "ExtentList is required for Prescriptive";
		return -EINVAL;
	}

	rc = fmd_dc_release(ctx, dev, host_id, (uint8_t)policy, forced, sanitize,
			    length, tag, extents, ext_count);
	free(extents);

	switch (rc) {
	case 0:
		return 0;
	case -EINVAL:
		*msg = "Policy not supported by the device";
		return rc;
	case -ENOENT:
		*msg = "No matching extents allocated to this host";
		return rc;
	default:
		*msg = "Initiate Dynamic Capacity Release failed";
		return rc;
	}
}

/*
 * POST PCIeDevice.CXLReferenceDCMemory -> DC Add Reference (5606h) when
 * FMReference is true, DC Remove Reference (5607h) when false.
 */
int fmd_redfish_action_dc_reference(struct fmd_ctx *ctx, const char *chassis_id,
				    const char *dev_id, const char *body,
				    size_t body_size, const char **msg)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	json_t *root, *val;
	json_error_t error;
	uint8_t tag[FMD_DC_TAG_LEN];
	bool add;
	int rc;

	if (!dev) {
		*msg = "Device is not a dynamic capacity device";
		return -ENOENT;
	}

	root = json_loadb(body, body_size, 0, &error);
	if (!root || !json_is_object(root)) {
		json_decref(root);
		*msg = "Invalid JSON body";
		return -EINVAL;
	}

	val = json_object_get(root, "Tag");
	if (!val || !json_is_string(val) || fmd_dc_tag_from_str(json_string_value(val), tag)) {
		json_decref(root);
		*msg = "Tag (UUID string) is required";
		return -EINVAL;
	}

	val = json_object_get(root, "FMReference");
	if (!val || !json_is_boolean(val)) {
		json_decref(root);
		*msg = "FMReference (boolean) is required";
		return -EINVAL;
	}
	add = json_is_true(val);
	json_decref(root);

	rc = fmd_dc_reference(ctx, dev, tag, add);
	if (rc == -ENOENT)
		*msg = "No extents carry the given Tag";
	else if (rc)
		*msg = "Dynamic Capacity reference update failed";
	return rc;
}
