/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Dynamic Capacity Device (DCD) management
 *
 * Implements the FM-API DCD Management command set (CXL r3.2 Section
 * 7.6.7.6, opcodes 5600h-5608h) on top of libcxlmi, plus a mock
 * implementation that keeps the same state in memory so the Redfish
 * surface can be exercised without hardware.
 *
 * State model (see docs/DCD-FM-API.md):
 *   - struct fmd_dcd_info  : device-wide capabilities (5600h)
 *   - struct fmd_dc_region : one host's view of a region (5601h), child of
 *                            the SLD (host 0) or of the LD the host sees
 *   - struct fmd_dc_extent : device-wide extent keyed by DPA (5603h),
 *                            child of the Type 3 device, with a bit per host
 *                            that has been offered it and a bit per host
 *                            whose acceptance is still pending (5608h)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "cxl-fmd.h"

/* Page sizes for the FM-API list commands */
#define DC_EXT_LIST_PAGE  64
#define DC_TAG_LIST_PAGE  32
#define DC_LIST_RETRIES   3

static inline bool dev_is_hw(struct fmd_component *dev)
{
	return dev && dev->fabric && dev->fabric->ep;
}

/*
 * ============================================================================
 * Policies
 * ============================================================================
 *
 * Indexed by the wire value written to 5604h Selection Policy / 5605h
 * Removal Policy; each entry carries the 5600h capability bit and the
 * Redfish name via the generated PCIeDevice.CXLDynamicCapacityPolicies enum.
 */

struct dc_policy {
	enum rf_cxl_dynamic_capacity_policies rf;
	uint16_t cap_bit;
};

static const struct dc_policy add_policies[] = {
	[FMD_DC_POLICY_FREE] = { CXL_DYNAMIC_CAPACITY_POLICIES_FREE, FMD_DC_ADD_FREE },
	[FMD_DC_POLICY_CONTIGUOUS] = { CXL_DYNAMIC_CAPACITY_POLICIES_CONTIGUOUS, FMD_DC_ADD_CONTIGUOUS },
	[FMD_DC_POLICY_PRESCRIPTIVE] = { CXL_DYNAMIC_CAPACITY_POLICIES_PRESCRIPTIVE, FMD_DC_ADD_PRESCRIPTIVE },
	[FMD_DC_POLICY_SHARED_ACCESS] = { CXL_DYNAMIC_CAPACITY_POLICIES_ENABLE_SHARED_ACCESS, FMD_DC_ADD_SHARED_ACCESS },
};

static const struct dc_policy rel_policies[] = {
	[FMD_DC_REL_POLICY_TAG_BASED] = { CXL_DYNAMIC_CAPACITY_POLICIES_TAG_BASED, FMD_DC_REL_TAG_BASED },
	[FMD_DC_REL_POLICY_PRESCRIPTIVE] = { CXL_DYNAMIC_CAPACITY_POLICIES_PRESCRIPTIVE, FMD_DC_REL_PRESCRIPTIVE },
};

static const struct dc_policy *dc_policy(bool release, int value)
{
	const struct dc_policy *tbl = release ? rel_policies : add_policies;
	size_t n = release ? sizeof(rel_policies) / sizeof(rel_policies[0])
			   : sizeof(add_policies) / sizeof(add_policies[0]);

	if (value < 0 || (size_t)value >= n)
		return NULL;
	return &tbl[value];
}

const char *fmd_dc_policy_name(bool release, int value)
{
	const struct dc_policy *p = dc_policy(release, value);

	return p ? rf_cxl_dynamic_capacity_policies_to_str(p->rf) : NULL;
}

uint16_t fmd_dc_policy_cap_bit(bool release, int value)
{
	const struct dc_policy *p = dc_policy(release, value);

	return p ? p->cap_bit : 0;
}

int fmd_dc_policy_from_str(bool release, const char *name)
{
	const char *s;

	if (!name)
		return -1;
	for (int v = 0; (s = fmd_dc_policy_name(release, v)); v++) {
		if (strcmp(name, s) == 0)
			return v;
	}
	return -1;
}

/*
 * ============================================================================
 * Accessors
 * ============================================================================
 */

struct fmd_dcd_info *fmd_memdev_dcd(struct fmd_component *dev)
{
	if (!dev)
		return NULL;

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		return fmd_to_sld(dev)->dcd;
	case FMD_COMPONENT_MLD:
		return fmd_to_mld(dev)->dcd;
	case FMD_COMPONENT_MHD:
		return fmd_to_mhd(dev)->dcd;
	default:
		return NULL;
	}
}

static struct fmd_ld *find_ld(struct fmd_component *dev, int ld_idx)
{
	struct fmd_component *child;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		if (ld->ld_id == ld_idx)
			return ld;
	}
	return NULL;
}

int fmd_dc_host_id(struct fmd_component *dev, int ld_idx)
{
	struct fmd_ld *ld;

	if (!dev)
		return -1;

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		return ld_idx == 0 ? 0 : -1;
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
		/* MHD memory is managed per pool LD (Section 2.5), as in an MLD */
		ld = find_ld(dev, ld_idx);
		return ld ? ld->ld_id : -1;
	default:
		return -1;
	}
}

struct fmd_component *fmd_dc_host_component(struct fmd_component *dev, int host_id)
{
	struct fmd_component *child;

	if (!dev || host_id < 0)
		return NULL;

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		return host_id == 0 ? dev : NULL;
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			if (ld->ld_id == host_id)
				return child;
		}
		return NULL;
	default:
		return NULL;
	}
}

struct fmd_dc_region *fmd_dc_find_region(struct fmd_component *dev, int host_id,
					 uint8_t region_id)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_component *child;

	if (!owner)
		return NULL;

	fmd_for_each_child_type(owner, child, FMD_COMPONENT_DC_REGION) {
		struct fmd_dc_region *r = fmd_to_dc_region(child);
		if (r->region_id == region_id)
			return r;
	}
	return NULL;
}

struct fmd_dc_extent *fmd_dc_find_extent(struct fmd_component *dev, uint64_t dpa)
{
	struct fmd_component *child;

	if (!dev)
		return NULL;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		if (ext->start_dpa == dpa)
			return ext;
	}
	return NULL;
}

int fmd_dc_region_extent_count(struct fmd_component *dev, struct fmd_dc_region *region)
{
	struct fmd_component *child;
	uint64_t bit = fmd_dc_host_bit(region->host_id);
	int count = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		if (ext->region_id == region->region_id && (ext->host_mask & bit))
			count++;
	}
	return count;
}

/*
 * ============================================================================
 * Tag helpers
 * ============================================================================
 */

void fmd_dc_tag_to_str(const uint8_t tag[FMD_DC_TAG_LEN], char *buf, size_t len)
{
	static const uint8_t zero[FMD_DC_TAG_LEN];

	if (memcmp(tag, zero, FMD_DC_TAG_LEN) == 0) {
		if (len)
			buf[0] = '\0';
		return;
	}

	snprintf(buf, len,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 tag[0], tag[1], tag[2], tag[3], tag[4], tag[5], tag[6], tag[7],
		 tag[8], tag[9], tag[10], tag[11], tag[12], tag[13], tag[14], tag[15]);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Accepts "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", 32 bare hex digits, or the
 * empty string (all-zero tag). Returns 0 on success, -1 on parse error.
 */
int fmd_dc_tag_from_str(const char *str, uint8_t tag[FMD_DC_TAG_LEN])
{
	int n = 0;

	memset(tag, 0, FMD_DC_TAG_LEN);
	if (!str)
		return -1;

	for (const char *p = str; *p; p++) {
		int hi, lo;

		if (*p == '-')
			continue;
		if (n >= FMD_DC_TAG_LEN)
			return -1;
		hi = hexval(p[0]);
		lo = p[1] ? hexval(p[1]) : -1;
		if (hi < 0 || lo < 0)
			return -1;
		tag[n++] = (uint8_t)((hi << 4) | lo);
		p++;
	}

	return (n == 0 || n == FMD_DC_TAG_LEN) ? 0 : -1;
}

/*
 * ============================================================================
 * Shared state helpers
 * ============================================================================
 */

static void extent_id(struct fmd_component *dev, uint64_t dpa, char *buf, size_t len)
{
	snprintf(buf, len, "%.40s_ext_%" PRIx64, dev->id, dpa);
}

static void region_id_str(struct fmd_component *owner, uint8_t region_id,
			  char *buf, size_t len)
{
	snprintf(buf, len, "%.50s_dcr%u", owner->id, region_id);
}

/* Region containing a DPA, from the host's point of view */
static struct fmd_dc_region *region_for_dpa(struct fmd_component *dev, int host_id,
					    uint64_t dpa)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_component *child;

	if (!owner)
		return NULL;

	fmd_for_each_child_type(owner, child, FMD_COMPONENT_DC_REGION) {
		struct fmd_dc_region *r = fmd_to_dc_region(child);
		if (dpa >= r->base_dpa && dpa < r->base_dpa + r->region_len)
			return r;
	}
	return NULL;
}

struct fmd_dc_region *fmd_dc_region_add(struct fmd_ctx *ctx, struct fmd_component *owner,
					uint8_t region_id, uint16_t host_id)
{
	struct fmd_dc_region *r;
	char id[FMD_MAX_ID_LEN];

	region_id_str(owner, region_id, id, sizeof(id));
	r = fmd_dc_region_create(id, region_id, host_id);
	if (!r)
		return NULL;

	if (fmd_topology_add(&ctx->topology, &r->base, owner) < 0) {
		fmd_component_free(&r->base);
		return NULL;
	}
	return r;
}

struct fmd_dc_extent *fmd_dc_extent_add(struct fmd_ctx *ctx, struct fmd_component *dev,
					uint64_t dpa, uint64_t len,
					const uint8_t tag[FMD_DC_TAG_LEN],
					uint16_t shared_seq, uint8_t region_id)
{
	struct fmd_dc_extent *ext;
	char id[FMD_MAX_ID_LEN];

	extent_id(dev, dpa, id, sizeof(id));
	ext = fmd_dc_extent_create(id);
	if (!ext)
		return NULL;

	ext->start_dpa = dpa;
	ext->len = len;
	if (tag)
		memcpy(ext->tag, tag, FMD_DC_TAG_LEN);
	ext->shared_seq = shared_seq;
	ext->region_id = region_id;

	if (fmd_topology_add(&ctx->topology, &ext->base, dev) < 0) {
		fmd_component_free(&ext->base);
		return NULL;
	}
	return ext;
}

/* Drop a host from an extent; free the extent when nobody holds it */
static void extent_drop_host(struct fmd_ctx *ctx, struct fmd_dc_extent *ext, int host_id)
{
	ext->host_mask &= ~fmd_dc_host_bit(host_id);
	ext->pending_mask &= ~fmd_dc_host_bit(host_id);

	if (ext->host_mask == 0 && !ext->fm_referenced)
		fmd_topology_remove(&ctx->topology, &ext->base);
}

void fmd_dc_adjust_available_extents(struct fmd_component *dev, int host_id, int delta)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	uint32_t *avail;
	uint32_t max;

	if (!dcd || !owner)
		return;

	if (owner->type == FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(owner);
		avail = &ld->dc_extents_available;
		max = ld->dc_extents_supported;
	} else {
		avail = &dcd->extents_available;
		max = dcd->extents_supported;
	}

	if (delta < 0 && (uint32_t)(-delta) > *avail)
		*avail = 0;
	else if (delta > 0 && *avail + delta > max)
		*avail = max;
	else
		*avail += delta;
}

/*
 * ============================================================================
 * Mock implementation
 * ============================================================================
 */

struct dpa_range {
	uint64_t start;
	uint64_t end;
};

static int cmp_range(const void *a, const void *b)
{
	const struct dpa_range *ra = a, *rb = b;

	if (ra->start < rb->start)
		return -1;
	return ra->start > rb->start;
}

/* Sorted list of DPA ranges occupied by extents of a region (any host) */
static int collect_region_ranges(struct fmd_component *dev, uint8_t region_id,
				 struct dpa_range **out)
{
	struct fmd_component *child;
	struct dpa_range *ranges;
	int count = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		if (fmd_to_dc_extent(child)->region_id == region_id)
			count++;
	}

	ranges = calloc(count ? count : 1, sizeof(*ranges));
	if (!ranges)
		return -1;

	count = 0;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		if (ext->region_id == region_id) {
			ranges[count].start = ext->start_dpa;
			ranges[count].end = ext->start_dpa + ext->len;
			count++;
		}
	}

	qsort(ranges, count, sizeof(*ranges), cmp_range);
	*out = ranges;
	return count;
}

/*
 * Carve @length bytes out of the free space of @region for @host_id.
 * Contiguous policy needs a single gap; Free policy may span gaps.
 */
static int mock_allocate(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			 struct fmd_dc_region *region, uint64_t length, bool contiguous,
			 const uint8_t tag[FMD_DC_TAG_LEN])
{
	struct dpa_range *used = NULL, *pieces = NULL;
	uint64_t bs = region->block_size ? region->block_size : CXL_CAPACITY_UNIT;
	uint64_t region_end = region->base_dpa + region->region_len;
	uint64_t cursor = region->base_dpa;
	uint64_t remaining = length;
	int n_used, n_pieces = 0;
	int rc = 0;

	n_used = collect_region_ranges(dev, region->region_id, &used);
	if (n_used < 0)
		return -ENOMEM;

	pieces = calloc(n_used + 1, sizeof(*pieces));
	if (!pieces) {
		free(used);
		return -ENOMEM;
	}

	/* Walk gaps between occupied ranges, then the tail gap */
	for (int i = 0; i <= n_used && remaining; i++) {
		uint64_t gap_end = (i < n_used) ? used[i].start : region_end;
		uint64_t gap = (gap_end > cursor) ? gap_end - cursor : 0;

		gap -= gap % bs;

		if (contiguous) {
			if (gap >= length) {
				pieces[n_pieces].start = cursor;
				pieces[n_pieces].end = cursor + length;
				n_pieces++;
				remaining = 0;
			}
		} else if (gap) {
			uint64_t take = gap < remaining ? gap : remaining;
			pieces[n_pieces].start = cursor;
			pieces[n_pieces].end = cursor + take;
			n_pieces++;
			remaining -= take;
		}

		if (i < n_used && used[i].end > cursor)
			cursor = used[i].end;
	}

	if (remaining) {
		rc = -ENOSPC;
		goto out;
	}

	for (int i = 0; i < n_pieces; i++) {
		struct fmd_dc_extent *ext = fmd_dc_extent_add(ctx, dev, pieces[i].start,
							      pieces[i].end - pieces[i].start,
							      tag, 0, region->region_id);
		if (!ext) {
			rc = -ENOMEM;
			goto out;
		}
		/* The mock host accepts immediately, so nothing is left pending */
		ext->host_mask = fmd_dc_host_bit(host_id);
		fmd_dc_adjust_available_extents(dev, host_id, -1);

		fmd_info(ctx, "%s: mock DC add host %d region %u dpa 0x%" PRIx64
			 " len %" PRIu64 " MB", dev->id, host_id, region->region_id,
			 ext->start_dpa, (uint64_t)(ext->len / FMD_MIB));
	}

out:
	free(used);
	free(pieces);
	return rc;
}

static int mock_dc_add(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		       uint8_t policy, uint8_t region_id, uint64_t length,
		       const uint8_t tag[FMD_DC_TAG_LEN],
		       struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dc_region *region;
	struct fmd_component *child;
	uint64_t bit = fmd_dc_host_bit(host_id);
	int matched = 0;

	switch (policy) {
	case FMD_DC_POLICY_FREE:
	case FMD_DC_POLICY_CONTIGUOUS:
		region = fmd_dc_find_region(dev, host_id, region_id);
		if (!region)
			return -ENOENT;
		if (length == 0 || length > region->region_len ||
		    (region->block_size && length % region->block_size))
			return -EINVAL;
		return mock_allocate(ctx, dev, host_id, region, length,
				     policy == FMD_DC_POLICY_CONTIGUOUS, tag);

	case FMD_DC_POLICY_PRESCRIPTIVE:
		if (ext_count <= 0 || !extents)
			return -EINVAL;
		for (int i = 0; i < ext_count; i++) {
			struct fmd_dc_extent *ext = extents[i];

			if (!ext)
				return -ENOENT;
			if (ext->host_mask & bit)
				return -EEXIST;
			region = fmd_dc_find_region(dev, host_id, ext->region_id);
			if (!region)
				return -ENOENT;
			if (ext->host_mask && !(region->flags & FMD_DC_REGION_SHAREABLE))
				return -EBUSY;
		}
		for (int i = 0; i < ext_count; i++) {
			extents[i]->host_mask |= bit;
			fmd_dc_adjust_available_extents(dev, host_id, -1);
		}
		return 0;

	case FMD_DC_POLICY_SHARED_ACCESS:
		region = fmd_dc_find_region(dev, host_id, region_id);
		if (!region)
			return -ENOENT;
		if (!(region->flags & FMD_DC_REGION_SHAREABLE))
			return -EINVAL;
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
			if (ext->region_id != region_id ||
			    memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
				continue;
			if (!(ext->host_mask & bit)) {
				ext->host_mask |= bit;
				fmd_dc_adjust_available_extents(dev, host_id, -1);
			}
			matched++;
		}
		return matched ? 0 : -ENOENT;

	default:
		return -EINVAL;
	}
}

static int mock_dc_release(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			   uint8_t policy, uint64_t length,
			   const uint8_t tag[FMD_DC_TAG_LEN],
			   struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_component *child, *next;
	uint64_t bit = fmd_dc_host_bit(host_id);
	uint64_t remaining = length;
	int matched = 0;

	switch (policy) {
	case FMD_DC_REL_POLICY_TAG_BASED:
		/*
		 * Release Length bytes (0 = everything) carrying Tag. An extent
		 * only this host holds can be trimmed from the tail; a shared
		 * extent is released whole.
		 */
		for (child = dev->first_child; child; child = next) {
			struct fmd_dc_extent *ext;

			next = child->next_sibling;
			if (child->type != FMD_COMPONENT_DC_EXTENT)
				continue;
			ext = fmd_to_dc_extent(child);
			if (!(ext->host_mask & bit) ||
			    memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
				continue;
			if (length && remaining == 0)
				break;

			matched++;
			if (length && remaining < ext->len && ext->host_mask == bit) {
				ext->len -= remaining;
				remaining = 0;
				continue;
			}
			if (length)
				remaining = (ext->len >= remaining) ? 0 : remaining - ext->len;

			fmd_dc_adjust_available_extents(dev, host_id, 1);
			extent_drop_host(ctx, ext, host_id);
		}
		return matched ? 0 : -ENOENT;

	case FMD_DC_REL_POLICY_PRESCRIPTIVE:
		if (ext_count <= 0 || !extents)
			return -EINVAL;
		for (int i = 0; i < ext_count; i++) {
			if (!extents[i] || !(extents[i]->host_mask & bit))
				return -ENOENT;
		}
		for (int i = 0; i < ext_count; i++) {
			fmd_dc_adjust_available_extents(dev, host_id, 1);
			extent_drop_host(ctx, extents[i], host_id);
		}
		return 0;

	default:
		return -EINVAL;
	}
}

static int mock_dc_reference(struct fmd_ctx *ctx, struct fmd_component *dev,
			     const uint8_t tag[FMD_DC_TAG_LEN], bool add)
{
	struct fmd_component *child, *next;
	int matched = 0;

	for (child = dev->first_child; child; child = next) {
		struct fmd_dc_extent *ext;

		next = child->next_sibling;
		if (child->type != FMD_COMPONENT_DC_EXTENT)
			continue;
		ext = fmd_to_dc_extent(child);
		if (memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
			continue;

		matched++;
		ext->fm_referenced = add;
		if (!add && ext->host_mask == 0)
			fmd_topology_remove(&ctx->topology, &ext->base);
	}

	return matched ? 0 : -ENOENT;
}

/* Apply a region configuration change to every host's view of the region */
static void apply_region_config(struct fmd_component *dev, uint8_t region_id,
				uint64_t block_size, bool sanitize)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	for (int h = 0; h < dcd->num_hosts; h++) {
		struct fmd_dc_region *r = fmd_dc_find_region(dev, h, region_id);
		if (r) {
			r->block_size = block_size;
			r->sanitize_on_release = sanitize;
		}
	}
}

/*
 * ============================================================================
 * Hardware implementation (libcxlmi)
 * ============================================================================
 */

/*
 * DCD commands are serviced by the FM-owned LD, so the only tunnel ever
 * needed is through a switch for a switch-attached MLD.
 */
static struct cxlmi_tunnel_info *dcd_tunnel(struct fmd_component *dev,
					    struct cxlmi_tunnel_info *buf)
{
	if (dev->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(dev);
		if (mld->switch_port >= 0) {
			DEFINE_CXLMI_TUNNEL_SWITCH(t, mld->switch_port);
			*buf = t;
			return buf;
		}
	}
	return CXLMI_NO_TUNNEL;
}

/* Host IDs a device exposes: SLD 0, MLD and MHD each LD */
static int dc_hosts(struct fmd_component *dev, int *hosts, int max)
{
	struct fmd_component *child;
	int n = 0;

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		if (max > 0)
			hosts[n++] = 0;
		break;
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
			if (n < max)
				hosts[n++] = fmd_to_ld(child)->ld_id;
		}
		break;
	default:
		break;
	}
	return n;
}

static void clear_host_regions(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_component *child, *next;

	if (!owner)
		return;

	for (child = owner->first_child; child; child = next) {
		next = child->next_sibling;
		if (child->type == FMD_COMPONENT_DC_REGION)
			fmd_topology_remove(&ctx->topology, child);
	}
}

static int hw_refresh_regions(struct fmd_ctx *ctx, struct fmd_component *dev,
			      struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti,
			      int host_id)
{
	struct cxlmi_cmd_fmapi_get_host_dc_region_config_req req = {0};
	struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp rsp = {0};
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	int rc;

	if (!owner)
		return -ENOENT;

	req.host_id = host_id;
	req.region_cnt = FMD_MAX_DC_REGIONS;
	req.start_region_id = 0;

	rc = cxlmi_cmd_fmapi_get_dc_reg_config(ep, ti, &req, &rsp);
	if (rc) {
		fmd_warn(ctx, "%s: get host %d DC region config failed: %d",
			 dev->id, host_id, rc);
		return -EIO;
	}

	clear_host_regions(ctx, dev, host_id);

	for (int i = 0; i < rsp.regions_returned && i < FMD_MAX_DC_REGIONS; i++) {
		struct fmd_dc_region *r;

		if (rsp.region_configs[i].region_len == 0)
			continue;

		r = fmd_dc_region_add(ctx, owner, i, host_id);
		if (!r)
			return -ENOMEM;

		r->base_dpa = rsp.region_configs[i].base;
		r->decode_len = rsp.region_configs[i].decode_len;
		r->region_len = rsp.region_configs[i].region_len;
		r->block_size = rsp.region_configs[i].block_size;
		r->flags = rsp.region_configs[i].flags;
		r->sanitize_on_release = rsp.region_configs[i].sanitize_on_release & 0x01;
	}

	if (owner->type == FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(owner);
		ld->dc_extents_supported = rsp.num_extents_supported;
		ld->dc_extents_available = rsp.num_extents_available;
		ld->dc_tags_supported = rsp.num_tags_supported;
		ld->dc_tags_available = rsp.num_tags_available;
	} else if (dcd) {
		dcd->extents_supported = rsp.num_extents_supported;
		dcd->extents_available = rsp.num_extents_available;
		dcd->tags_supported = rsp.num_tags_supported;
		dcd->tags_available = rsp.num_tags_available;
	}

	return 0;
}

/*
 * Bring the host's extents in line with 5603h: new DPAs are added, extents
 * the device no longer lists for the host are dropped, and the rest are
 * updated in place so their identity, pending state and FM reference
 * survive the refresh.
 */
static int hw_refresh_extents(struct fmd_ctx *ctx, struct fmd_component *dev,
			      struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti,
			      int host_id)
{
	struct cxlmi_cmd_fmapi_get_dc_region_ext_list_req req;
	struct cxlmi_cmd_fmapi_get_dc_region_ext_list_rsp *rsp;
	struct fmd_component *child, *next;
	uint64_t bit = fmd_dc_host_bit(host_id);
	uint64_t *seen = NULL;
	uint32_t n_seen = 0, cap_seen = 0;
	uint32_t start = 0, generation = 0;
	int retries = 0;
	int rc = 0;

	rsp = calloc(1, sizeof(*rsp) + DC_EXT_LIST_PAGE * sizeof(rsp->extents[0]));
	if (!rsp)
		return -ENOMEM;

	for (;;) {
		memset(&req, 0, sizeof(req));
		req.host_id = host_id;
		req.extent_count = DC_EXT_LIST_PAGE;
		req.start_ext_index = start;

		rc = cxlmi_cmd_fmapi_get_dc_region_ext_list(ep, ti, &req, rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get host %d DC extent list failed: %d",
				 dev->id, host_id, rc);
			rc = -EIO;
			goto out;
		}

		if (start == 0) {
			generation = rsp->list_generation_num;
		} else if (rsp->list_generation_num != generation) {
			/* List changed underneath us: start over */
			if (++retries > DC_LIST_RETRIES) {
				rc = -EAGAIN;
				goto out;
			}
			n_seen = 0;
			start = 0;
			continue;
		}

		if (n_seen + rsp->extents_returned > cap_seen) {
			uint32_t ncap = 2 * (n_seen + rsp->extents_returned);
			uint64_t *tmp = realloc(seen, ncap * sizeof(*seen));

			if (!tmp) {
				rc = -ENOMEM;
				goto out;
			}
			seen = tmp;
			cap_seen = ncap;
		}

		for (uint32_t i = 0; i < rsp->extents_returned; i++) {
			uint64_t dpa = rsp->extents[i].start_dpa;
			struct fmd_dc_extent *ext = fmd_dc_find_extent(dev, dpa);

			if (!ext) {
				struct fmd_dc_region *region = region_for_dpa(dev, host_id, dpa);

				ext = fmd_dc_extent_add(ctx, dev, dpa, rsp->extents[i].len,
							rsp->extents[i].tag,
							rsp->extents[i].shared_seq,
							region ? region->region_id : 0);
				if (!ext) {
					rc = -ENOMEM;
					goto out;
				}
			} else {
				ext->len = rsp->extents[i].len;
				memcpy(ext->tag, rsp->extents[i].tag, FMD_DC_TAG_LEN);
				ext->shared_seq = rsp->extents[i].shared_seq;
			}
			ext->host_mask |= bit;
			seen[n_seen++] = dpa;
		}

		start += rsp->extents_returned;
		if (rsp->extents_returned == 0 || start >= rsp->total_extents)
			break;
	}

	/* Drop the host from extents the device no longer lists for it */
	for (child = dev->first_child; child; child = next) {
		struct fmd_dc_extent *ext;
		uint32_t i;

		next = child->next_sibling;
		if (child->type != FMD_COMPONENT_DC_EXTENT)
			continue;
		ext = fmd_to_dc_extent(child);
		if (!(ext->host_mask & bit))
			continue;
		for (i = 0; i < n_seen && seen[i] != ext->start_dpa; i++)
			;
		if (i == n_seen)
			extent_drop_host(ctx, ext, host_id);
	}

out:
	free(seen);
	free(rsp);
	return rc;
}

static int hw_refresh_tags(struct fmd_ctx *ctx, struct fmd_component *dev,
			   struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti)
{
	struct cxlmi_cmd_fmapi_dc_list_tags_req req;
	struct cxlmi_cmd_fmapi_dc_list_tags_rsp *rsp;
	struct fmd_component *child;
	uint32_t start = 0;
	int rc = 0;

	rsp = calloc(1, sizeof(*rsp) + DC_TAG_LIST_PAGE * sizeof(rsp->tags_list[0]));
	if (!rsp)
		return -ENOMEM;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		ext->fm_referenced = false;
		ext->pending_mask = 0;
	}

	for (;;) {
		memset(&req, 0, sizeof(req));
		req.start_idx = start;
		req.tags_count = DC_TAG_LIST_PAGE;

		rc = cxlmi_cmd_fmapi_dc_list_tags(ep, ti, &req, rsp);
		if (rc) {
			/* Optional on r3.0 devices: not fatal */
			fmd_dbg(ctx, "%s: DC list tags failed: %d", dev->id, rc);
			rc = 0;
			break;
		}

		for (uint32_t i = 0; i < rsp->num_tags_returned; i++) {
			uint64_t pending = 0;

			if (rsp->validity_bitmap & 0x02) {
				for (int h = 0; h < FMD_MAX_DC_HOSTS; h++) {
					if (rsp->tags_list[i].pending_ref_bitmap[h / 8] & (1 << (h % 8)))
						pending |= fmd_dc_host_bit(h);
				}
			}

			fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
				struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
				if (memcmp(ext->tag, rsp->tags_list[i].tag, FMD_DC_TAG_LEN))
					continue;
				ext->fm_referenced = rsp->tags_list[i].flags & 0x01;
				ext->pending_mask = pending & ext->host_mask;
			}
		}

		start += rsp->num_tags_returned;
		if (rsp->num_tags_returned == 0 || start >= rsp->total_num_tags)
			break;
	}

	free(rsp);
	return rc;
}

/* Caller holds ctx->dcd_lock */
static int dcd_refresh_locked(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id)
{
	struct cxlmi_tunnel_info tbuf, *ti;
	struct cxlmi_endpoint *ep;
	int hosts[FMD_MAX_DC_HOSTS];
	int n, rc = 0;

	if (!dev_is_hw(dev) || !fmd_memdev_dcd(dev))
		return 0;

	ep = dev->fabric->ep;
	ti = dcd_tunnel(dev, &tbuf);

	if (host_id >= 0) {
		hosts[0] = host_id;
		n = 1;
	} else {
		n = dc_hosts(dev, hosts, FMD_MAX_DC_HOSTS);
	}

	for (int i = 0; i < n; i++) {
		rc = hw_refresh_regions(ctx, dev, ep, ti, hosts[i]);
		if (rc)
			return rc;
		rc = hw_refresh_extents(ctx, dev, ep, ti, hosts[i]);
		if (rc)
			return rc;
	}

	return hw_refresh_tags(ctx, dev, ep, ti);
}

int fmd_dcd_refresh(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dcd_refresh_locked(ctx, dev, host_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

int fmd_dcd_discover(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct cxlmi_cmd_fmapi_get_dcd_info_rsp info = {0};
	struct cxlmi_tunnel_info tbuf, *ti;
	struct fmd_dcd_info *dcd;
	int rc;

	if (!dev_is_hw(dev) || !fmd_is_memdev(dev))
		return 0;

	ti = dcd_tunnel(dev, &tbuf);

	/* Get DCD Info (5600h) - failure simply means not a DCD */
	rc = cxlmi_cmd_fmapi_get_dcd_info(dev->fabric->ep, ti, &info);
	if (rc) {
		fmd_dbg(ctx, "%s: no dynamic capacity support (%d)", dev->id, rc);
		return 0;
	}

	dcd = calloc(1, sizeof(*dcd));
	if (!dcd)
		return -ENOMEM;

	dcd->num_hosts = info.num_hosts;
	dcd->num_regions = info.num_supported_dc_regions > FMD_MAX_DC_REGIONS
		? FMD_MAX_DC_REGIONS : info.num_supported_dc_regions;
	dcd->add_policies = info.capacity_selection_policies;
	dcd->release_policies = info.capacity_removal_policies;
	dcd->sanitize_cfg_mask = info.sanitize_on_release_config_mask;
	dcd->total_dc_capacity = CXL_TO_BYTES(info.total_dynamic_capacity);
	/* region_0..7_supported_blk_sz_mask are contiguous in the packed response */
	memcpy(dcd->blk_sz_mask, &info.region_0_supported_blk_sz_mask,
	       sizeof(dcd->blk_sz_mask));

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		fmd_to_sld(dev)->dcd = dcd;
		break;
	case FMD_COMPONENT_MLD:
		fmd_to_mld(dev)->dcd = dcd;
		break;
	case FMD_COMPONENT_MHD:
		fmd_to_mhd(dev)->dcd = dcd;
		fmd_to_mhd(dev)->dynamic_capacity = true;
		break;
	default:
		free(dcd);
		return -EINVAL;
	}

	fmd_info(ctx, "%s: DCD with %d hosts, %d regions, %" PRIu64 " MB dynamic capacity",
		 dev->id, dcd->num_hosts, dcd->num_regions,
		 (uint64_t)(dcd->total_dc_capacity / FMD_MIB));

	return fmd_dcd_refresh(ctx, dev, -1);
}

/*
 * ============================================================================
 * Operations
 * ============================================================================
 */

static int dc_set_region_config_locked(struct fmd_ctx *ctx, struct fmd_component *dev,
				       struct fmd_dc_region *region, uint64_t block_size,
				       bool sanitize_on_release)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	if (!dcd || !region)
		return -ENOENT;

	/* Block size must be a supported power of two for this region */
	if (block_size == 0 || (block_size & (block_size - 1)))
		return -EINVAL;
	if (dcd->blk_sz_mask[region->region_id] &&
	    !(dcd->blk_sz_mask[region->region_id] & (1ULL << fmd_ilog2_u64(block_size))))
		return -EINVAL;

	if (sanitize_on_release != region->sanitize_on_release &&
	    !(dcd->sanitize_cfg_mask & (1 << region->region_id)))
		return -EINVAL;

	/* The device rejects reconfiguration while capacity is allocated */
	for (int h = 0; h < dcd->num_hosts; h++) {
		struct fmd_dc_region *r = fmd_dc_find_region(dev, h, region->region_id);
		if (r && fmd_dc_region_extent_count(dev, r) > 0)
			return -EBUSY;
	}

	if (dev_is_hw(dev)) {
		struct cxlmi_cmd_fmapi_set_dc_region_config_req req = {0};
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int rc;

		req.region_id = region->region_id;
		req.block_sz = block_size;
		req.sanitize_on_release = sanitize_on_release ? 1 : 0;

		rc = cxlmi_cmd_fmapi_set_dc_region_config(dev->fabric->ep, ti, &req);
		if (rc) {
			fmd_warn(ctx, "%s: set DC region %u config failed: %d",
				 dev->id, region->region_id, rc);
			return -EIO;
		}
		return dcd_refresh_locked(ctx, dev, -1);
	}

	fmd_info(ctx, "%s: mock set DC region %u block size %" PRIu64 " MB sanitize=%d",
		 dev->id, region->region_id, (uint64_t)(block_size / FMD_MIB),
		 sanitize_on_release);
	apply_region_config(dev, region->region_id, block_size, sanitize_on_release);
	return 0;
}

int fmd_dc_set_region_config(struct fmd_ctx *ctx, struct fmd_component *dev,
			     struct fmd_dc_region *region, uint64_t block_size,
			     bool sanitize_on_release)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dc_set_region_config_locked(ctx, dev, region, block_size,
					 sanitize_on_release);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

static int dc_add_locked(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			 uint8_t policy, uint8_t region_id, uint64_t length,
			 const uint8_t tag[FMD_DC_TAG_LEN],
			 struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	uint16_t cap_bit = fmd_dc_policy_cap_bit(false, policy);

	if (!dcd || host_id < 0 || host_id >= FMD_MAX_DC_HOSTS)
		return -ENOENT;
	if (!cap_bit || !(dcd->add_policies & cap_bit))
		return -EINVAL;
	if (!fmd_dc_host_component(dev, host_id))
		return -ENOENT;

	if (!dev_is_hw(dev))
		return mock_dc_add(ctx, dev, host_id, policy, region_id, length,
				   tag, extents, ext_count);

	{
		struct cxlmi_cmd_fmapi_initiate_dc_add_req *req;
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int n = (policy == FMD_DC_POLICY_PRESCRIPTIVE) ? ext_count : 0;
		int rc;

		if (policy == FMD_DC_POLICY_PRESCRIPTIVE && (n <= 0 || !extents))
			return -EINVAL;

		req = calloc(1, sizeof(*req) + n * sizeof(req->extents[0]));
		if (!req)
			return -ENOMEM;

		req->host_id = host_id;
		req->selection_policy = policy;
		req->region_num = region_id;
		req->length = length;
		memcpy(req->tag, tag, FMD_DC_TAG_LEN);
		req->ext_count = n;
		for (int i = 0; i < n; i++) {
			if (!extents[i]) {
				free(req);
				return -ENOENT;
			}
			req->extents[i].start_dpa = extents[i]->start_dpa;
			req->extents[i].len = extents[i]->len;
			memcpy(req->extents[i].tag, extents[i]->tag, FMD_DC_TAG_LEN);
			req->extents[i].shared_seq = extents[i]->shared_seq;
		}

		rc = cxlmi_cmd_fmapi_initiate_dc_add(dev->fabric->ep, ti, req);
		free(req);
		if (rc) {
			fmd_warn(ctx, "%s: initiate DC add (host %d) failed: %d",
				 dev->id, host_id, rc);
			return -EIO;
		}

		return dcd_refresh_locked(ctx, dev, host_id);
	}
}

int fmd_dc_add(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
	       uint8_t policy, uint8_t region_id, uint64_t length,
	       const uint8_t tag[FMD_DC_TAG_LEN],
	       struct fmd_dc_extent **extents, int ext_count)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dc_add_locked(ctx, dev, host_id, policy, region_id, length, tag,
			   extents, ext_count);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

static int dc_release_locked(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			     uint8_t policy, bool forced, bool sanitize, uint64_t length,
			     const uint8_t tag[FMD_DC_TAG_LEN],
			     struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	uint16_t cap_bit = fmd_dc_policy_cap_bit(true, policy);

	if (!dcd || host_id < 0 || host_id >= FMD_MAX_DC_HOSTS)
		return -ENOENT;
	if (!cap_bit || !(dcd->release_policies & cap_bit))
		return -EINVAL;
	if (!fmd_dc_host_component(dev, host_id))
		return -ENOENT;

	if (!dev_is_hw(dev))
		return mock_dc_release(ctx, dev, host_id, policy, length, tag,
				       extents, ext_count);

	{
		struct cxlmi_cmd_fmapi_initiate_dc_release_req *req;
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int n = (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE) ? ext_count : 0;
		int rc;

		if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE && (n <= 0 || !extents))
			return -EINVAL;

		req = calloc(1, sizeof(*req) + n * sizeof(req->extents[0]));
		if (!req)
			return -ENOMEM;

		req->host_id = host_id;
		req->flags = (policy & FMD_DC_REL_POLICY_MASK) |
			     (forced ? FMD_DC_REL_FORCED_REMOVAL : 0) |
			     (sanitize ? FMD_DC_REL_SANITIZE : 0);
		req->length = length;
		memcpy(req->tag, tag, FMD_DC_TAG_LEN);
		req->ext_count = n;
		for (int i = 0; i < n; i++) {
			if (!extents[i]) {
				free(req);
				return -ENOENT;
			}
			req->extents[i].start_dpa = extents[i]->start_dpa;
			req->extents[i].len = extents[i]->len;
			memcpy(req->extents[i].tag, extents[i]->tag, FMD_DC_TAG_LEN);
			req->extents[i].shared_seq = extents[i]->shared_seq;
		}

		rc = cxlmi_cmd_fmapi_initiate_dc_release(dev->fabric->ep, ti, req);
		free(req);
		if (rc) {
			fmd_warn(ctx, "%s: initiate DC release (host %d) failed: %d",
				 dev->id, host_id, rc);
			return -EIO;
		}

		return dcd_refresh_locked(ctx, dev, host_id);
	}
}

int fmd_dc_release(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		   uint8_t policy, bool forced, bool sanitize, uint64_t length,
		   const uint8_t tag[FMD_DC_TAG_LEN],
		   struct fmd_dc_extent **extents, int ext_count)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dc_release_locked(ctx, dev, host_id, policy, forced, sanitize, length,
			       tag, extents, ext_count);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

static int dc_reference_locked(struct fmd_ctx *ctx, struct fmd_component *dev,
			       const uint8_t tag[FMD_DC_TAG_LEN], bool add)
{
	if (!fmd_memdev_dcd(dev))
		return -ENOENT;

	if (!dev_is_hw(dev))
		return mock_dc_reference(ctx, dev, tag, add);

	{
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int rc;

		if (add) {
			struct cxlmi_cmd_fmapi_dc_add_ref_req req;
			memcpy(req.tag, tag, FMD_DC_TAG_LEN);
			rc = cxlmi_cmd_fmapi_dc_add_reference(dev->fabric->ep, ti, &req);
		} else {
			struct cxlmi_cmd_fmapi_dc_remove_ref_req req;
			memcpy(req.tag, tag, FMD_DC_TAG_LEN);
			rc = cxlmi_cmd_fmapi_dc_remove_reference(dev->fabric->ep, ti, &req);
		}

		if (rc) {
			fmd_warn(ctx, "%s: DC %s reference failed: %d",
				 dev->id, add ? "add" : "remove", rc);
			return -EIO;
		}

		return hw_refresh_tags(ctx, dev, dev->fabric->ep, ti);
	}
}

int fmd_dc_reference(struct fmd_ctx *ctx, struct fmd_component *dev,
		     const uint8_t tag[FMD_DC_TAG_LEN], bool add)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dc_reference_locked(ctx, dev, tag, add);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}
