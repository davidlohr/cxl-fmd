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
 *   PCIeDevice Oem/Cxl/DynamicCapacityTags               <- Get DC Region Tag List (5608h)
 *   CXLLogicalDevice/MemoryRegions/{region}              <- Get Host DC Region Config (5601h)
 *   PATCH CXLLogicalDevice/MemoryRegions/{region}        -> Set DC Region Config (5602h)
 *   CXLLogicalDevice.DynamicCapacityAdd action           -> Initiate DC Add (5604h)
 *   CXLLogicalDevice.DynamicCapacityRelease action       -> Initiate DC Release (5605h)
 *   DELETE PCIeDevice/MemoryExtents/{ext}                -> Initiate DC Release, from that one host
 *
 * A MemoryExtent is one host's extent: 5603h lists extents per host, in
 * that host's DPA space, so its Id names the CXLLogicalDevice as well as
 * the DPA. That one resource per host and per extent is cxl-fmd's mapping:
 * DSP0288 maps no Dynamic Capacity command.
 */

#include "handlers_internal.h"

/* -EAGAIN: the device answered Retry Required or Busy */
#define DC_RETRY_MSG "The device cannot carry out the request now; try again"

/*
 * FMD_DC_ACCEPTED_STALE: the device answered Success, and the read after
 * it failed. Not a failure, and not to be sent again (cxl-fmd's wording).
 */
#define DC_ACCEPTED_STALE_MSG \
	"The device accepted the request, but its state could not be read back after it: " \
	"what is shown is stale until it is read again, on the next event poll. " \
	"Do not send the request again"

/* -EREMOTEIO: no response, or one that could not be read */
#define DC_UNKNOWN_MSG \
	"Outcome unknown: the device may or may not have carried out the request, and it " \
	"is read again on the next event poll: check its state before sending this again"

/*
 * The message for what the four DC operations return in common, or NULL
 * for the rest, which each handler words itself.
 */
static const char *dc_common_msg(int rc)
{
	switch (rc) {
	case FMD_DC_ACCEPTED_STALE:
		return DC_ACCEPTED_STALE_MSG;
	case -EREMOTEIO:
		return DC_UNKNOWN_MSG;
	case -EAGAIN:
		return DC_RETRY_MSG;
	case -EOPNOTSUPP:
		return "No route to the LD Pool CCI of the Multi-Headed device is known: "
		       "nothing was sent";
	default:
		return NULL;
	}
}

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

/*
 * Redfish Id of an extent: ext_<ld>_<hex DPA>, <ld> the CXLLogicalDevice
 * whose host's extent it is (cxl-fmd's choice of Id). The DPA alone, which
 * was the Id once, names an extent of every host that has that DPA.
 */
static void extent_rf_id(struct fmd_component *dev, struct fmd_dc_extent *ext,
			 char *buf, size_t len)
{
	int ld = host_ld_index(dev, ext->host_id);

	snprintf(buf, len, "ext_%d_%" PRIx64, ld < 0 ? ext->host_id : ld,
		 ext->start_dpa);
}

static struct fmd_dc_extent *find_extent_by_rf_id(struct fmd_component *dev,
						  const char *ext_id)
{
	struct fmd_dc_extent *ext;
	const char *p;
	char *end, id[32];
	unsigned long ld;
	uint64_t dpa;
	int host_id;

	if (!ext_id || strncmp(ext_id, "ext_", 4) != 0)
		return NULL;
	p = ext_id + 4;
	if (*p < '0' || *p > '9')
		return NULL;

	errno = 0;
	ld = strtoul(p, &end, 10);
	if (errno || *end != '_' || ld > 0xFFFF)
		return NULL;
	p = end + 1;
	if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
	      (*p >= 'A' && *p <= 'F')))
		return NULL;
	dpa = strtoull(p, &end, 16);
	if (errno || *end != '\0')
		return NULL;

	host_id = fmd_dc_host_id(dev, (int)ld);
	if (host_id < 0)
		return NULL;
	ext = fmd_dc_find_extent(dev, host_id, dpa);
	if (!ext)
		return NULL;

	/*
	 * Only the Id the extent is listed by names it: strtoul() and
	 * strtoull() also take leading zeros, a "0x" or an upper-case DPA,
	 * which would be aliases of it, each answered with its own "Id".
	 */
	extent_rf_id(dev, ext, id, sizeof(id));
	return strcmp(id, ext_id) ? NULL : ext;
}

/* Resolve a MemoryExtent @odata.id link to the extent under this device */
static struct fmd_dc_extent *find_extent_by_link(struct fmd_component *dev,
						 const char *odata_id)
{
	char prefix[2 * FMD_MAX_ID_LEN + 64];
	size_t n;

	if (!odata_id)
		return NULL;

	/* an extent of this device: a link to another's names none of these */
	n = (size_t)snprintf(prefix, sizeof(prefix),
			     "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/",
			     dev->id, dev->id);
	if (n >= sizeof(prefix) || strncmp(odata_id, prefix, n))
		return NULL;
	return find_extent_by_rf_id(dev, odata_id + n);
}

/* An all-zero Tag is no tag (cxl-fmd's convention) */
static bool tag_is_zero(const uint8_t *tag)
{
	for (int i = 0; i < FMD_DC_TAG_LEN; i++) {
		if (tag[i])
			return false;
	}
	return true;
}

static void region_uri(const char *chassis_id, const char *dev_id, int ld_idx,
		       uint8_t region_id, char *buf, size_t len)
{
	snprintf(buf, len,
		 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d/MemoryRegions/%u",
		 chassis_id, dev_id, ld_idx, region_id);
}

static void extent_uri(const char *chassis_id, const char *dev_id,
		       struct fmd_component *dev, struct fmd_dc_extent *ext,
		       char *buf, size_t len)
{
	char id[32];

	extent_rf_id(dev, ext, id, sizeof(id));
	snprintf(buf, len, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/%s",
		 chassis_id, dev_id, id);
}

/*
 * Parse an ExtentList action parameter into an array of extents of host
 * @host_id. Returns the count, 0 if the parameter is absent, -EINVAL on a
 * bad link or on an extent that is listed twice, and -EXDEV on an extent
 * of another host: an Extent List is in the DPA space of the host the
 * request names (Section 9.13.3), and another host's extent, even at the
 * same DPA, is not in it.
 */
static int parse_extent_list(struct fmd_component *dev, int host_id, json_t *root,
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
		return -EINVAL;

	n = json_array_size(list);
	if (n == 0)
		return -EINVAL;

	extents = calloc(n, sizeof(*extents));
	if (!extents)
		return -EINVAL;

	json_array_foreach(list, idx, item) {
		json_t *link = json_is_object(item) ? json_object_get(item, "@odata.id") : NULL;

		extents[idx] = find_extent_by_link(dev, link ? json_string_value(link) : NULL);
		if (!extents[idx]) {
			free(extents);
			return -EINVAL;
		}
		if (extents[idx]->host_id != host_id) {
			free(extents);
			return -EXDEV;
		}
		/* each extent once: it may be freed when it is released */
		for (size_t prev = 0; prev < idx; prev++) {
			if (extents[prev] == extents[idx]) {
				free(extents);
				return -EINVAL;
			}
		}
	}

	*out = extents;
	return (int)n;
}

/*
 * A task for an add or release of @host_id of @dev, which {ld} @ld_id
 * names (NULL for a DELETE), recording the request: its body as it came,
 * which the Task shows as its Payload. NULL, with @msg, if out of memory.
 */
static struct fmd_task *dc_task_new(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				    struct fmd_component *dev, int host_id,
				    const char *chassis_id, const char *ld_id,
				    const char *body, size_t body_size,
				    const char **msg)
{
	struct fmd_task *task = fmd_dc_task_new(ctx, kind);

	if (task && body) {
		task->dc->body = strndup(body, body_size);
		if (!task->dc->body) {
			fmd_task_discard(task);
			task = NULL;
		}
	}
	if (!task) {
		*msg = "No memory for the task: nothing was sent";
		return NULL;
	}
	task->dc->host_id = host_id;
	snprintf(task->dc->dev_id, sizeof(task->dc->dev_id), "%s", dev->id);
	snprintf(task->dc->chassis_id, sizeof(task->dc->chassis_id), "%s", chassis_id);
	snprintf(task->dc->ld_id, sizeof(task->dc->ld_id), "%s", ld_id ? ld_id : "");
	return task;
}

/*
 * The release @task records: its terms, and the extents of a Prescriptive
 * one copied, as releasing them can free them. -ENOMEM, or 0.
 */
static int dc_task_release_fill(struct fmd_task *task, uint8_t policy, bool forced,
				bool sanitize, uint64_t length, const uint8_t *tag,
				struct fmd_dc_extent **extents, int n)
{
	struct fmd_dc_task *dc = task->dc;

	dc->policy = policy;
	dc->forced = forced;
	dc->sanitize = sanitize;
	dc->region_id = FMD_DC_REGION_NONE;
	if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE) {
		dc->req = calloc(n ? n : 1, sizeof(*dc->req));
		if (!dc->req)
			return -ENOMEM;
		for (int i = 0; i < n; i++) {
			dc->req[i].dpa = extents[i]->start_dpa;
			dc->req[i].len = extents[i]->len;
			memcpy(dc->req[i].tag, extents[i]->tag, FMD_DC_TAG_LEN);
			dc->req[i].region_id = extents[i]->region_id;
		}
		dc->n_req = n;
		return 0;
	}
	dc->length = length;
	memcpy(dc->tag, tag, FMD_DC_TAG_LEN);
	return 0;
}

/*
 * fmd_dc_task_prepare() reads a stale host again for the baseline, and
 * that frees the extents the device no longer lists: the @n extents of a
 * Prescriptive release, found before it, are found again from the copies
 * in @task. False if one is gone, or is not the extent it was; then
 * nothing is to be sent.
 */
static bool dc_task_extents_again(struct fmd_component *dev,
				  const struct fmd_task *task,
				  struct fmd_dc_extent **extents, int n)
{
	const struct fmd_dc_task *dc = task->dc;

	for (int i = 0; i < n && i < dc->n_req; i++) {
		struct fmd_dc_extent *ext = fmd_dc_find_extent(dev, dc->host_id,
							       dc->req[i].dpa);

		if (!ext || ext->len != dc->req[i].len ||
		    memcmp(ext->tag, dc->req[i].tag, FMD_DC_TAG_LEN))
			return false;
		extents[i] = ext;
	}
	return true;
}

#define EXTENT_GONE_MSG \
	"An extent the request names is no longer the host's, as read before it: " \
	"nothing was sent"

#define EXTENT_LIST_MSG \
	"ExtentList must be a non-empty array of distinct MemoryExtent links on this device"
#define EXTENT_FOREIGN_MSG \
	"An extent in ExtentList belongs to another logical device"

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

/* The LD-IDs of the bits set in a 5608h LD bitmap, in order */
static json_t *ld_bitmap_array(const uint8_t *bm)
{
	json_t *arr = json_array();

	for (unsigned int ld = 0; ld <= FMD_DC_MAX_HOST_ID; ld++) {
		if (fmd_dc_bm_test(bm, ld))
			json_array_append_new(arr, json_integer(ld));
	}
	return arr;
}

/*
 * Oem/Cxl/DynamicCapacity of a PCIeDevice: what CXLDevice/DynamicCapacity
 * gives in MiB, in bytes. Region 0 Supported Block Size Mask: "Each bit
 * indicates a power of 2 supported block size, where bit n being set
 * indicates that block size 2^n is supported. Bits[5:0] and bits[63:52]
 * shall be 0" (CXL r4.0 Table 7-65, and the other regions "As defined in
 * Region 0"): a block can be 64 bytes, which BlockSizeMiB, an integer of
 * MiB, cannot say. BlockSizeBytes lists every size the mask has, in
 * bytes; TotalDynamicCapacityBytes is Total Dynamic Capacity, in bytes.
 * Review F12 (claims 4 and 5); the properties are cxl-fmd's.
 */
static json_t *dc_bytes_oem(const struct fmd_dcd_info *dcd)
{
	json_t *blocks = json_array();

	for (int r = 0; r < dcd->num_regions && r < FMD_MAX_DC_REGIONS; r++) {
		json_t *sizes = json_array();

		for (int n = 0; n < 63; n++) {
			if (dcd->blk_sz_mask[r] & (1ULL << n))
				json_array_append_new(sizes, json_integer((json_int_t)(1ULL << n)));
		}
		json_array_append_new(blocks, json_pack("{s:i, s:o}",
			"RegionNumber", r,
			"BlockSizeBytes", sizes));
	}
	return json_pack("{s:I, s:o}",
		"TotalDynamicCapacityBytes", (json_int_t)dcd->total_dc_capacity,
		"MemoryBlockSizesSupported", blocks);
}

static json_t *dc_tags_oem_locked(struct fmd_ctx *ctx, struct fmd_component *dev,
				  const char *chassis_id, const char *dev_id)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	json_t *tags = json_array(), *view;

	fmd_topology_rdlock(&ctx->topology);
	for (uint32_t i = 0; dcd->tags.known && i < dcd->tags.n; i++) {
		const struct fmd_dc_tag *t = &dcd->tags.v[i];
		struct fmd_component *child;
		json_t *row, *links = json_array();
		char tag_str[40];

		fmd_dc_tag_to_str(t->tag, tag_str, sizeof(tag_str));
		row = json_pack("{s:s, s:b}", "Tag", tag_str,
				"FMHoldsReference", t->fm_ref ? 1 : 0);
		/* Validity Bitmap: Bit[0] the Reference Bitmap, Bit[1] the Pending one */
		if (dcd->tags.validity & 0x01)
			json_object_set_new(row, "AcceptedLDs", ld_bitmap_array(t->ref));
		if (dcd->tags.validity & 0x02)
			json_object_set_new(row, "PendingLDs", ld_bitmap_array(t->pend));

		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
			char euri[512];

			if (memcmp(ext->tag, t->tag, FMD_DC_TAG_LEN))
				continue;
			extent_uri(chassis_id, dev_id, dev, ext, euri, sizeof(euri));
			json_array_append_new(links, rf_link("%s", euri));
		}
		json_object_set_new(row, "Extents", links);
		json_array_append_new(tags, row);
	}
	fmd_topology_rdunlock(&ctx->topology);

	/*
	 * Tags only from a list that is known: an empty one would say that
	 * no LD has an add outstanding and the FM holds no reference.
	 */
	view = json_pack("{s:b, s:I}",
			 "TagsKnown", dcd->tags.known ? 1 : 0,
			 "Generation", (json_int_t)dcd->tags.gen);
	/* when the last list read to its end was, also while TagsKnown is false */
	rf_set_date_time(view, "LastRefreshed", dcd->tags.read_at);
	if (dcd->tags.known)
		json_object_set_new(view, "Tags", tags);
	else
		json_decref(tags);
	return json_pack("{s:{s:s, s:o, s:o}}", "Cxl",
			 "@odata.type", "#Cxl.v1_0_0.PCIeDevice",
			 "DynamicCapacity", dc_bytes_oem(dcd),
			 "DynamicCapacityTags", view);
}

/*
 * PCIeDevice Oem/Cxl: DynamicCapacity, sizes in bytes (dc_bytes_oem()),
 * and DynamicCapacityTags, the device's tag list, from Dynamic Capacity
 * List Tags (5608h, Table 7-78). DMTF has no resource for a tag, nor for
 * the LDs that have accepted or been offered its capacity, and DSP0288
 * maps no Dynamic Capacity command: this view and its names are cxl-fmd's,
 * described by its Oem schema (schema/oem/csdl/Cxl_v1.xml).
 *
 * A tag is listed as the device lists it, which a MemoryExtent cannot show
 * of capacity no host's Extent List has: capacity only an FM reference
 * keeps, and an add an LD has not answered that its 5603h does not list.
 * AcceptedLDs and PendingLDs are the LD-IDs of the Reference and Pending
 * Reference Bitmaps, each said only when the Validity Bitmap says it is
 * valid; Extents are every host's MemoryExtents with the tag.
 *
 * Returns NULL when the device is not a DCD.
 */
json_t *fmd_redfish_dc_tags_oem(struct fmd_ctx *ctx, struct fmd_component *dev,
				const char *chassis_id, const char *dev_id)
{
	json_t *res = NULL;

	pthread_mutex_lock(&ctx->dcd_lock);
	if (fmd_memdev_dcd(dev))
		res = dc_tags_oem_locked(ctx, dev, chassis_id, dev_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return res;
}

/*
 * ============================================================================
 * CXLLogicalDevice additions
 * ============================================================================
 */

/*
 * The Policy values DynamicCapacityAdd takes: "To specify the list of
 * supported values for a parameter, the service may include the
 * @Redfish.AllowableValues annotation" (DSP0266 1.25.0 Section 9.6.14.1).
 * Free and Contiguous when the device supports them (Supported Add Capacity
 * Selection Policies, Table 7-65), and EnableSharedAccess always, as it is
 * always sent (fmd_dc_add()). Prescriptive is left out even when the device
 * supports it: its ExtentList names capacity by MemoryExtent links, and
 * every MemoryExtent is capacity some host has, so a Prescriptive add is
 * refused with 400 before anything is sent (action_dc_add_locked()).
 * AddCapacityPoliciesSupported still says what the device supports
 * (cxl-fmd's choices). Review DCD-A5.
 */
static json_t *dc_add_allowable(const struct fmd_dcd_info *dcd)
{
	static const int offered[] = {
		FMD_DC_POLICY_FREE, FMD_DC_POLICY_CONTIGUOUS,
		FMD_DC_POLICY_SHARED_ACCESS,
	};
	json_t *values = json_array();

	for (size_t i = 0; i < sizeof(offered) / sizeof(offered[0]); i++) {
		int v = offered[i];

		if (v == FMD_DC_POLICY_SHARED_ACCESS ||
		    (dcd->add_policies & fmd_dc_policy_cap_bit(false, v)))
			json_array_append_new(values,
				json_string(fmd_dc_policy_name(false, v)));
	}
	return values;
}

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

	/*
	 * When the host's region table (5601h, the counters above among it)
	 * and its extent list (5603h, in MemorySizeMiB) were last read to
	 * their end, each absent until one has been (cxl-fmd's Oem). Review
	 * F07 claim 6.
	 */
	if (host_id >= 0) {
		time_t regions, extents;

		fmd_dc_host_read_times(dev, host_id, &regions, &extents);
		if (regions || extents) {
			json_t *oem = json_pack("{s:s}", "@odata.type",
						"#Cxl.v1_0_0.CXLLogicalDevice");

			rf_set_date_time(oem, "DynamicCapacityRegionsLastRefreshed", regions);
			rf_set_date_time(oem, "DynamicCapacityExtentsLastRefreshed", extents);
			json_object_set_new(result, "Oem", json_pack("{s:o}", "Cxl", oem));
		}
	}

	/* Actions (CXLLogicalDevice v1.4) */
	json_t *actions = json_object_get(result, "Actions");
	if (!actions) {
		actions = json_object();
		json_object_set_new(result, "Actions", actions);
	}
	json_object_set_new(actions, "#CXLLogicalDevice.DynamicCapacityAdd",
		json_pack("{s:s, s:s, s:o}",
			"target", rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/Actions/CXLLogicalDevice.DynamicCapacityAdd",
					 chassis_id, dev_id, ld_id),
			"title", "DynamicCapacityAdd",
			"Policy@Redfish.AllowableValues", dc_add_allowable(dcd)));
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

static json_t *dc_memory_extents_locked(struct fmd_ctx *ctx, const char *chassis_id,
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

		extent_rf_id(dev, fmd_to_dc_extent(child), id, sizeof(id));
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/%s",
				  chassis_id, dev_id, id);
	}
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

static json_t *dc_memory_extent_locked(struct fmd_ctx *ctx, const char *chassis_id,
				       const char *dev_id, const char *ext_id)
{
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dc_extent *ext;
	char uri[512];
	char tag_str[40];
	json_t *result, *regions;
	int ld_idx;

	if (!dev)
		return NULL;

	fmd_topology_rdlock(&ctx->topology);
	ext = find_extent_by_rf_id(dev, ext_id);
	if (!ext) {
		fmd_topology_rdunlock(&ctx->topology);
		return NULL;
	}

	extent_uri(chassis_id, dev_id, dev, ext, uri, sizeof(uri));
	fmd_dc_tag_to_str(ext->tag, tag_str, sizeof(tag_str));

	result = json_pack(
		"{s:s, s:s, s:s, s:s, s:I, s:I, s:I, s:o}",
		"@odata.id", uri,
		"@odata.type", "#MemoryExtent.v1_0_0.MemoryExtent",
		"Id", ext_id,
		"Name", "Dynamic Capacity Extent",
		"ExtentOffsetMiB", (json_int_t)(ext->start_dpa / FMD_MIB),
		"ExtentSizeMiB", (json_int_t)(ext->len / FMD_MIB),
		"SequenceNumber", (json_int_t)ext->shared_seq,
		/*
		 * Enabled: an extent 5603h lists is not pending (cxl-fmd's
		 * choice, see dc_tags_check()). Warning: the extent is in no
		 * region of its host, or is what the host's last complete
		 * 5603h listed, the read after it having failed or not having
		 * been made yet, or it has a tag and the tag list could not be
		 * read, so whether the FM holds a reference on it is not known.
		 */
		"Status", rf_status("Enabled",
				    ext->region_id == FMD_DC_REGION_NONE ||
				    fmd_dc_sync_behind(fmd_dc_host_sync(dev, ext->host_id)) ||
				    (tag_str[0] && fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags)) ?
				    "Warning" : "OK"));

	/* An extent with no tag (all zero, cxl-fmd's convention) has no Tag */
	if (tag_str[0])
		json_object_set_new(result, "Tag", json_string(tag_str));

	/*
	 * Starting DPA and Length in bytes (cxl-fmd's Oem): the Length is
	 * "Always a multiple of the configured Region Block Size" and the
	 * Starting DPA "Always aligned to" it (CXL r4.0 Table 8-230), and a
	 * block can be less than 1 MiB (Table 7-65), so ExtentOffsetMiB and
	 * ExtentSizeMiB, integers of MiB, round down.
	 * Review F12 claim 5.
	 */
	{
		json_t *oem = json_pack("{s:s, s:I, s:I}",
			"@odata.type", "#Cxl.v1_0_0.MemoryExtent",
			"ExtentOffsetBytes", (json_int_t)ext->start_dpa,
			"ExtentSizeBytes", (json_int_t)ext->len);
		time_t regions, extents;

		/* when its host's extent list was last read to its end */
		fmd_dc_host_read_times(dev, ext->host_id, &regions, &extents);
		rf_set_date_time(oem, "LastRefreshed", extents);
		json_object_set_new(result, "Oem", json_pack("{s:o}", "Cxl", oem));
	}

	/*
	 * FMReferencedExtent: Flags Bit[0] "FM Holds Reference" of the tag's
	 * 5608h entry (Table 7-78). Not said when the tag list is not known,
	 * or the extent has no tag.
	 */
	{
		struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
		const struct fmd_dc_tag *t = fmd_dc_tag_find(dev, ext->tag);

		if (t)
			json_object_set_new(result, "FMReferencedExtent", json_boolean(t->fm_ref));
		else if (dcd->tags.known && tag_str[0])
			json_object_set_new(result, "FMReferencedExtent", json_false());
	}

	/*
	 * Links/MemoryRegions: the region of the host whose extent it is,
	 * none when the extent is in no region the host reported.
	 */
	regions = json_array();
	ld_idx = host_ld_index(dev, ext->host_id);
	if (ld_idx >= 0 && ext->region_id != FMD_DC_REGION_NONE) {
		char ruri[512];

		region_uri(chassis_id, dev_id, ld_idx, ext->region_id, ruri, sizeof(ruri));
		json_array_append_new(regions, rf_link("%s", ruri));
	}
	json_object_set_new(result, "Links", json_pack("{s:o}", "MemoryRegions", regions));

	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * What @host_id of @dev has of @tag: the sum of its extents with it in
 * *@length, and when @prescriptive, those extents in *@extents (*@n of
 * them). -ENOMEM, or 0.
 */
static int whole_tag_members(struct fmd_component *dev, int host_id, const uint8_t *tag,
			     bool prescriptive, struct fmd_dc_extent ***extents, int *n,
			     uint64_t *length)
{
	struct fmd_component *child;
	int k = 0;

	*extents = NULL;
	*n = 0;
	*length = 0;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *e = fmd_to_dc_extent(child);

		if (e->host_id == host_id && !memcmp(e->tag, tag, FMD_DC_TAG_LEN)) {
			*length += e->len;
			k++;
		}
	}
	if (!prescriptive)
		return 0;
	*length = 0;
	*extents = calloc(k ? k : 1, sizeof(**extents));
	if (!*extents)
		return -ENOMEM;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *e = fmd_to_dc_extent(child);

		if (e->host_id == host_id && !memcmp(e->tag, tag, FMD_DC_TAG_LEN))
			(*extents)[(*n)++] = e;
	}
	return 0;
}

/*
 * DELETE on an extent releases it from the host whose extent it is, and
 * from no other host (cxl-fmd's meaning of DELETE: DMTF does not say).
 *
 * "To remove a host's access to the shared extent, the FM issues Initiate
 * Dynamic Capacity Release Request with Selection Policy=Tag-based with the
 * Host ID associated with that host" (Section 7.6.7.6.6), and a release
 * whose Extent List "covers only a portion of the capacity associated with
 * the Tag" of sharable capacity is Invalid Input. So an extent with a tag
 * in a Sharable region is released with the whole of what its host has of
 * the tag, Tag-based: every extent of that host with the tag goes. Any
 * other extent is released by itself, Prescriptive.
 *
 * A device that does not support Tag-based release is sent a Prescriptive
 * list of every extent of the host with the tag, which covers the same
 * capacity (cxl-fmd's choice).
 */
static int delete_dc_memory_extent_locked(struct fmd_ctx *ctx, const char *chassis_id,
					  const char *dev_id, const char *ext_id,
					  const char **msg, int *task_id)
{
	struct fmd_task *task = NULL;
	uint8_t policy;
	struct fmd_component *dev = find_dcd_device(ctx, chassis_id, dev_id);
	struct fmd_dcd_info *dcd;
	struct fmd_dc_region *region;
	struct fmd_dc_extent *ext, **extents = NULL;
	uint8_t tag[FMD_DC_TAG_LEN];
	uint64_t length = 0, dpa, len;
	bool sanitize, sharable;
	int host_id, n = 0, rc;
	uint8_t region_id;

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

	/* the read for the baseline, or the release, can free @ext: it is copied */
	host_id = ext->host_id;
	dpa = ext->start_dpa;
	len = ext->len;
	region_id = ext->region_id;
	memcpy(tag, ext->tag, FMD_DC_TAG_LEN);
	region = fmd_dc_find_region(dev, host_id, region_id);
	sanitize = region ? region->sanitize_on_release : false;
	sharable = region && (region->flags & FMD_DC_REGION_SHAREABLE) &&
		   !tag_is_zero(tag);

	if (!sharable) {
		extents = calloc(1, sizeof(*extents));
		if (!extents)
			goto nomem;
		extents[n++] = ext;
		policy = FMD_DC_REL_POLICY_PRESCRIPTIVE;
	} else {
		policy = (dcd->release_policies &
			  fmd_dc_policy_cap_bit(true, FMD_DC_REL_POLICY_TAG_BASED)) ?
			 FMD_DC_REL_POLICY_TAG_BASED : FMD_DC_REL_POLICY_PRESCRIPTIVE;
		if (whole_tag_members(dev, host_id, tag,
				      policy == FMD_DC_REL_POLICY_PRESCRIPTIVE,
				      &extents, &n, &length))
			goto nomem;
	}

	task = dc_task_new(ctx, FMD_TASK_DC_RELEASE, dev, host_id, chassis_id, NULL,
			   NULL, 0, msg);
	if (!task || dc_task_release_fill(task, policy, false, sanitize, length, tag,
					  extents, n))
		goto nomem;
	snprintf(task->dc->ext_id, sizeof(task->dc->ext_id), "%s", ext_id);
	rc = fmd_dc_task_prepare(ctx, dev, task, msg);
	if (rc)
		goto out;
	if (!sharable) {
		if (!dc_task_extents_again(dev, task, extents, n)) {
			*msg = EXTENT_GONE_MSG;
			rc = -ENOENT;
			goto out;
		}
	} else {
		/*
		 * The whole tag is what the host has of it as read for the
		 * task's baseline, which may have been read again just now:
		 * the extent the URI names must still be there, the same
		 * extent of the same tag in a Sharable region, and the
		 * release is of every extent the host now has with the tag.
		 */
		ext = fmd_dc_find_extent(dev, host_id, dpa);
		region = ext ? fmd_dc_find_region(dev, host_id, region_id) : NULL;
		if (!ext || ext->len != len || ext->region_id != region_id ||
		    memcmp(ext->tag, tag, FMD_DC_TAG_LEN) || !region ||
		    !(region->flags & FMD_DC_REGION_SHAREABLE)) {
			*msg = EXTENT_GONE_MSG;
			rc = -ENOENT;
			goto out;
		}
		free(extents);
		if (whole_tag_members(dev, host_id, tag,
				      policy == FMD_DC_REL_POLICY_PRESCRIPTIVE,
				      &extents, &n, &length))
			goto nomem;
		free(task->dc->req);
		task->dc->req = NULL;
		task->dc->n_req = 0;
		if (dc_task_release_fill(task, policy, false, sanitize, length, tag,
					 extents, n))
			goto nomem;
	}

	rc = fmd_dc_release(ctx, dev, host_id, policy, false, sanitize,
			    policy == FMD_DC_REL_POLICY_TAG_BASED ? length : 0, tag,
			    policy == FMD_DC_REL_POLICY_TAG_BASED ? NULL : extents,
			    policy == FMD_DC_REL_POLICY_TAG_BASED ? 0 : n);
	if (rc == 0 || rc == FMD_DC_ACCEPTED_STALE) {
		fmd_dc_task_start(ctx, dev, task, rc == FMD_DC_ACCEPTED_STALE);
		*task_id = task->id;
		free(extents);
		return FMD_OP_STARTED;
	}
	*msg = dc_common_msg(rc);
	if (!*msg)
		*msg = "Dynamic capacity release failed";
out:
	fmd_task_discard(task);
	free(extents);
	return rc;
nomem:
	fmd_task_discard(task);
	free(extents);
	*msg = "No memory for the task: nothing was sent";
	return -ENOMEM;
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

static json_t *dc_memory_regions_locked(struct fmd_ctx *ctx, const char *chassis_id,
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

static json_t *dc_memory_region_locked(struct fmd_ctx *ctx, const char *chassis_id,
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

	/* This host's extents in the region, as links (inline fields are deprecated) */
	extents = json_array();
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		char euri[512];

		if (ext->region_id != region->region_id || ext->host_id != host_id)
			continue;

		extent_uri(chassis_id, dev_id, dev, ext, euri, sizeof(euri));
		/* listed by 5603h, so not pending (cxl-fmd's choice, dc_tags_check()) */
		json_array_append_new(extents, json_pack("{s:b, s:{s:s}}",
			"Pending", 0,
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
		/*
		 * Warning: the tag list could not be read, so whether an add
		 * this host has not answered is outstanding is not known; or
		 * the host's last read failed, or one is owed, so the region
		 * and its extents are those of the last read that completed
		 * (cxl-fmd's choice).
		 */
		"Status", rf_status("Enabled",
				    fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags) ||
				    fmd_dc_sync_behind(fmd_dc_host_sync(dev, host_id)) ?
				    "Warning" : "OK"));

	/*
	 * Region Base, Length and Block Size in bytes (cxl-fmd's Oem): a
	 * block size can be 2^6 bytes (Table 7-65), so the MiB properties,
	 * integers, round down, a block size under 1 MiB to 0. Review F12
	 * claims 4 and 5.
	 */
	if (result) {
		json_t *oem = json_pack("{s:s, s:I, s:I, s:I}",
			"@odata.type", "#Cxl.v1_0_0.MemoryRegion",
			"RegionBaseOffsetBytes", (json_int_t)region->base_dpa,
			"RegionSizeBytes", (json_int_t)region->region_len,
			"BlockSizeBytes", (json_int_t)region->block_size);
		time_t regions, ext;

		/* when the host's region table was last read to its end */
		fmd_dc_host_read_times(dev, host_id, &regions, &ext);
		rf_set_date_time(oem, "LastRefreshed", regions);
		json_object_set_new(result, "Oem", json_pack("{s:o}", "Cxl", oem));
	}

	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * PATCH MemoryRegion -> Set DC Region Configuration (5602h)
 * Writable: BlockSizeMiB, SanitizeOnRelease.
 */
static int patch_dc_memory_region_locked(struct fmd_ctx *ctx, const char *chassis_id,
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
			if (rf_mib_to_bytes(val, false, &block_size)) {
				json_decref(root);
				*msg = "BlockSizeMiB must be an integer from 1 to 17592186044415";
				return -EINVAL;
			}
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
	if (dc_common_msg(rc)) {
		*msg = dc_common_msg(rc);
		return rc;
	}
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
static int action_dc_add_locked(struct fmd_ctx *ctx, const char *chassis_id,
				const char *dev_id, const char *ld_id,
				const char *body, size_t body_size, const char **msg,
				int *task_id)
{
	struct fmd_task *task;
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
		if (rf_mib_to_bytes(val, false, &length)) {
			json_decref(root);
			*msg = "Length must be an integer from 1 to 17592186044415 (MiB)";
			return -EINVAL;
		}
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

	ext_count = parse_extent_list(dev, host_id, root, &extents);
	if (ext_count < 0) {
		json_decref(root);
		*msg = ext_count == -EXDEV ? EXTENT_FOREIGN_MSG : EXTENT_LIST_MSG;
		return -EINVAL;
	}
	json_decref(root);

	/*
	 * A Prescriptive add names DPAs of the host's own DPA space: those
	 * of an extent the host has are Invalid Extent List, "When the
	 * Selection Policy is set to Prescriptive and any of the DPAs are
	 * already accessible to the same LD" (Section 7.6.7.6.5), and those
	 * of another host's extent are not of its DPA space. A MemoryExtent
	 * is an extent some host has, so no ExtentList of links can name
	 * capacity to add: refused here rather than sent (cxl-fmd's choice).
	 */
	if (policy == FMD_DC_POLICY_PRESCRIPTIVE && ext_count > 0) {
		free(extents);
		*msg = "An extent in ExtentList is already accessible to this logical device";
		return -EINVAL;
	}

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

	/* the task, with the request and the baseline, before anything is sent */
	task = dc_task_new(ctx, FMD_TASK_DC_ADD, dev, host_id, chassis_id, ld_id,
			   body, body_size, msg);
	if (!task) {
		free(extents);
		return -ENOMEM;
	}
	task->dc->policy = (uint8_t)policy;
	task->dc->region_id = (uint8_t)(region < 0 ? 0 : region);
	memcpy(task->dc->tag, tag, FMD_DC_TAG_LEN);
	task->dc->length = length;
	rc = fmd_dc_task_prepare(ctx, dev, task, msg);
	if (rc) {
		fmd_task_discard(task);
		free(extents);
		return rc;
	}

	rc = fmd_dc_add(ctx, dev, host_id, (uint8_t)policy, (uint8_t)(region < 0 ? 0 : region),
			length, tag, extents, ext_count);
	free(extents);
	if (rc == 0 || rc == FMD_DC_ACCEPTED_STALE) {
		fmd_dc_task_start(ctx, dev, task, rc == FMD_DC_ACCEPTED_STALE);
		*task_id = task->id;
		return FMD_OP_STARTED;
	}
	fmd_task_discard(task);

	if (dc_common_msg(rc)) {
		*msg = dc_common_msg(rc);
		return rc;
	}
	switch (rc) {
	case -EINVAL:
		*msg = "Invalid request: a Policy the device does not support, a Length that is not "
		       "a block-size multiple, or a request the device refused as invalid";
		return rc;
	case -ENOSPC:
		*msg = "Insufficient free capacity in the region, or no room left to track the extents or tag";
		return rc;
	case -ENOENT:
		*msg = "Region or extents not found for this host";
		return rc;
	case -EEXIST:
		*msg = "Host already has access to an extent in ExtentList";
		return rc;
	case -EBUSY:
		*msg = "An extent in ExtentList is held by another host; share it with EnableSharedAccess";
		return rc;
	default:
		*msg = "Initiate Dynamic Capacity Add failed";
		return rc;
	}
}

/*
 * POST CXLLogicalDevice.DynamicCapacityRelease -> Initiate DC Release (5605h)
 */
static int action_dc_release_locked(struct fmd_ctx *ctx, const char *chassis_id,
				    const char *dev_id, const char *ld_id,
				    const char *body, size_t body_size, const char **msg,
				    int *task_id)
{
	struct fmd_task *task;
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
		/* 0 is refused below for TagBased, with its own message */
		if (rf_mib_to_bytes(val, true, &length)) {
			json_decref(root);
			*msg = "Length must be an integer from 1 to 17592186044415 (MiB)";
			return -EINVAL;
		}
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

	ext_count = parse_extent_list(dev, host_id, root, &extents);
	if (ext_count < 0) {
		json_decref(root);
		*msg = ext_count == -EXDEV ? EXTENT_FOREIGN_MSG : EXTENT_LIST_MSG;
		return -EINVAL;
	}
	json_decref(root);

	if (policy == FMD_DC_REL_POLICY_TAG_BASED && (!have_tag || !have_length)) {
		free(extents);
		*msg = "Tag and Length are required for TagBased";
		return -EINVAL;
	}
	/* Length "Shall be > 0" (Table 7-73): there is no "release everything" */
	if (policy == FMD_DC_REL_POLICY_TAG_BASED && length == 0) {
		free(extents);
		*msg = "Length must be a positive integer (MiB) for TagBased";
		return -EINVAL;
	}
	if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE && ext_count == 0) {
		*msg = "ExtentList is required for Prescriptive";
		return -EINVAL;
	}

	task = dc_task_new(ctx, FMD_TASK_DC_RELEASE, dev, host_id, chassis_id, ld_id,
			   body, body_size, msg);
	if (!task || dc_task_release_fill(task, (uint8_t)policy, forced, sanitize,
					  length, tag, extents, ext_count)) {
		fmd_task_discard(task);
		free(extents);
		*msg = "No memory for the task: nothing was sent";
		return -ENOMEM;
	}
	rc = fmd_dc_task_prepare(ctx, dev, task, msg);
	if (rc) {
		fmd_task_discard(task);
		free(extents);
		return rc;
	}
	if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE &&
	    !dc_task_extents_again(dev, task, extents, ext_count)) {
		fmd_task_discard(task);
		free(extents);
		*msg = EXTENT_GONE_MSG;
		return -ENOENT;
	}

	rc = fmd_dc_release(ctx, dev, host_id, (uint8_t)policy, forced, sanitize,
			    length, tag, extents, ext_count);
	free(extents);
	if (rc == 0 || rc == FMD_DC_ACCEPTED_STALE) {
		fmd_dc_task_start(ctx, dev, task, rc == FMD_DC_ACCEPTED_STALE);
		*task_id = task->id;
		return FMD_OP_STARTED;
	}
	fmd_task_discard(task);

	if (dc_common_msg(rc)) {
		*msg = dc_common_msg(rc);
		return rc;
	}
	switch (rc) {
	case -EINVAL:
		*msg = "Invalid request: a Policy the device does not support, or a request the "
		       "device refused as invalid";
		return rc;
	case -ENOENT:
		*msg = "No matching extents allocated to this host";
		return rc;
	case -ENOSPC:
		*msg = "Length exceeds the capacity assigned with the Tag";
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
static int action_dc_reference_locked(struct fmd_ctx *ctx, const char *chassis_id,
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
	if (dc_common_msg(rc))
		*msg = dc_common_msg(rc);
	else if (rc == -ENOENT)
		*msg = "No extents carry the given Tag";
	else if (rc == -EINVAL)
		*msg = "The Tag names no sharable capacity (an empty or all-zero Tag is none)";
	else if (rc)
		*msg = "Dynamic Capacity reference update failed";
	return rc;
}

/*
 * ============================================================================
 * Entry points
 * ============================================================================
 *
 * Regions and extents are freed and rebuilt under ctx->dcd_lock when the
 * event thread refreshes a device. Everything above that looks one up does
 * so with the lock held by its entry point here, from the lookup to the last
 * use.
 */

json_t *fmd_redfish_dc_memory_extents(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id)
{
	json_t *res;

	pthread_mutex_lock(&ctx->dcd_lock);
	res = dc_memory_extents_locked(ctx, chassis_id, dev_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return res;
}

json_t *fmd_redfish_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ext_id)
{
	json_t *res;

	pthread_mutex_lock(&ctx->dcd_lock);
	res = dc_memory_extent_locked(ctx, chassis_id, dev_id, ext_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return res;
}

/* The Task of a request that started one, made once dcd_lock is let go */
static int dc_started(struct fmd_ctx *ctx, int rc, int task_id, json_t **task)
{
	*task = NULL;
	if (rc != FMD_OP_STARTED)
		return rc;
	*task = fmd_redfish_task(ctx, task_id);
	return rc;
}

int fmd_redfish_delete_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
					const char *dev_id, const char *ext_id,
					const char **msg, json_t **task)
{
	int rc, id = 0;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = delete_dc_memory_extent_locked(ctx, chassis_id, dev_id, ext_id, msg, &id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return dc_started(ctx, rc, id, task);
}

json_t *fmd_redfish_dc_memory_regions(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id, const char *ld_id)
{
	json_t *res;

	pthread_mutex_lock(&ctx->dcd_lock);
	res = dc_memory_regions_locked(ctx, chassis_id, dev_id, ld_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return res;
}

json_t *fmd_redfish_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ld_id,
				     const char *region_id)
{
	json_t *res;

	pthread_mutex_lock(&ctx->dcd_lock);
	res = dc_memory_region_locked(ctx, chassis_id, dev_id, ld_id, region_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return res;
}

int fmd_redfish_patch_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				       const char *dev_id, const char *ld_id,
				       const char *region_id, const char *body,
				       size_t body_size, const char **msg)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = patch_dc_memory_region_locked(ctx, chassis_id, dev_id, ld_id,
			region_id, body, body_size, msg);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

int fmd_redfish_action_dc_add(struct fmd_ctx *ctx, const char *chassis_id,
			      const char *dev_id, const char *ld_id,
			      const char *body, size_t body_size, const char **msg,
			      json_t **task)
{
	int rc, id = 0;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = action_dc_add_locked(ctx, chassis_id, dev_id,
			ld_id, body, body_size, msg, &id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return dc_started(ctx, rc, id, task);
}

int fmd_redfish_action_dc_release(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *dev_id, const char *ld_id,
				  const char *body, size_t body_size, const char **msg,
				  json_t **task)
{
	int rc, id = 0;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = action_dc_release_locked(ctx, chassis_id, dev_id,
			ld_id, body, body_size, msg, &id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return dc_started(ctx, rc, id, task);
}

int fmd_redfish_action_dc_reference(struct fmd_ctx *ctx, const char *chassis_id,
				    const char *dev_id, const char *body,
				    size_t body_size, const char **msg)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = action_dc_reference_locked(ctx, chassis_id, dev_id,
			body, body_size, msg);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

/*
 * ============================================================================
 * Tasks
 * ============================================================================
 */

static json_t *dc_ranges_json(const struct fmd_dc_range *v, int n)
{
	json_t *arr = json_array();

	for (int i = 0; i < n; i++) {
		char tag[40];

		fmd_dc_tag_to_str(v[i].tag, tag, sizeof(tag));
		json_array_append_new(arr, json_pack("{s:I, s:I, s:s}",
			"ExtentOffsetBytes", (json_int_t)v[i].dpa,
			"ExtentSizeBytes", (json_int_t)v[i].len,
			"Tag", tag));
	}
	return arr;
}

/*
 * Oem/Cxl of a DC Task: the request as it was recorded before it was sent,
 * the host's extents before it, and what has been seen added or released.
 */
json_t *fmd_redfish_dc_task_oem(struct fmd_ctx *ctx, const struct fmd_task *task)
{
	const struct fmd_dc_task *dc = task->dc;
	bool add = task->kind == FMD_TASK_DC_ADD;
	bool prescriptive = !add && dc->policy == FMD_DC_REL_POLICY_PRESCRIPTIVE;
	json_t *o;
	char tag[40];

	(void)ctx;
	fmd_dc_tag_to_str(dc->tag, tag, sizeof(tag));
	o = json_pack("{s:i, s:s, s:I, s:o}",
		"HostId", dc->host_id,
		"Policy", fmd_dc_policy_name(!add, dc->policy),
		"BaselineGeneration", (json_int_t)dc->base_gen,
		"BaselineExtents", dc_ranges_json(dc->base, dc->n_base));
	if (add) {
		json_object_set_new(o, "RegionNumber", json_integer(dc->region_id));
		json_object_set_new(o, "Tag", json_string(tag));
		if (dc->policy == FMD_DC_POLICY_FREE || dc->policy == FMD_DC_POLICY_CONTIGUOUS)
			json_object_set_new(o, "LengthBytes", json_integer((json_int_t)dc->length));
		json_object_set_new(o, "AcceptedExtents", dc_ranges_json(dc->done, dc->n_done));
	} else {
		json_object_set_new(o, "ForcedRemoval", json_boolean(dc->forced));
		json_object_set_new(o, "SanitizeOnRelease", json_boolean(dc->sanitize));
		if (prescriptive) {
			json_object_set_new(o, "RequestedExtents",
					    dc_ranges_json(dc->req, dc->n_req));
		} else {
			json_object_set_new(o, "Tag", json_string(tag));
			json_object_set_new(o, "LengthBytes",
					    json_integer((json_int_t)dc->length));
		}
		json_object_set_new(o, "ReleasedExtents", dc_ranges_json(dc->done, dc->n_done));
	}
	return json_pack("{s:{s:s, s:o}}", "Cxl",
			 "@odata.type", "#Cxl.v1_0_0.Task",
			 "DynamicCapacity", o);
}

/*
 * Links/CreatedResources of an add: the MemoryExtents the capacity seen
 * added is in, those that are still there ("Services shall set this
 * property prior to the task entering its final state", Task_v1).
 */
json_t *fmd_redfish_dc_task_created(struct fmd_ctx *ctx, const struct fmd_task *task)
{
	const struct fmd_dc_task *dc = task->dc;
	struct fmd_component *dev, *child;
	json_t *arr = json_array();

	if (task->kind != FMD_TASK_DC_ADD)
		return arr;
	pthread_mutex_lock(&ctx->dcd_lock);
	dev = find_dcd_device(ctx, dc->dev_id, dc->dev_id);
	if (dev) {
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
			bool in = false;
			char uri[512];

			if (ext->host_id != dc->host_id)
				continue;
			for (int i = 0; i < dc->n_done && !in; i++)
				in = dc->done[i].dpa >= ext->start_dpa &&
				     dc->done[i].dpa - ext->start_dpa < ext->len;
			if (!in)
				continue;
			extent_uri(dc->chassis_id, dc->dev_id, dev, ext, uri, sizeof(uri));
			json_array_append_new(arr, json_pack("{s:s}", "@odata.id", uri));
		}
		fmd_topology_rdunlock(&ctx->topology);
	}
	pthread_mutex_unlock(&ctx->dcd_lock);
	return arr;
}
