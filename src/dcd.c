/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Dynamic Capacity Device (DCD) management
 *
 * Implements the FM-API DCD Management command set (CXL r4.0 Section
 * 7.6.7.6, opcodes 5600h-5608h) on top of libcxlmi, plus a mock
 * implementation that keeps the same state in memory so the Redfish
 * surface can be exercised without hardware.
 *
 * State model (see docs/DCD-FM-API.md):
 *   - struct fmd_dcd_info  : device-wide capabilities (5600h)
 *   - struct fmd_dc_region : one host's view of a region (5601h), child of
 *                            the SLD (host 0) or of the LD the host sees
 *   - struct fmd_dc_extent : one host's extent (5603h), keyed by Host ID
 *                            and that host's DPA, child of the Type 3
 *                            device; capacity hosts share is one extent per
 *                            host with the same Tag
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

/* Dynamic Capacity Event Log (CXL r4.0 Section 8.2.10.2.2 and Table 8-229) */
#define DC_EVENT_LOG		4
#define DC_EVENT_ADD_RESPONSE	0x04
#define DC_EVENT_RELEASED	0x05
/* Flags of Get Event Records, Table 8-232: Bit[0] Overflow, Bit[1] More */
#define EVENT_FLAG_OVERFLOW	(1 << 0)
#define EVENT_FLAG_MORE		(1 << 1)
#define DC_EVENT_MAX_PAGES	16
#define DC_EVENT_MAX_DEVS	64

static const uint8_t dc_event_uuid[0x10] = {
	0xca, 0x95, 0xaf, 0xa7, 0xf1, 0x83, 0x40, 0x18,
	0x8c, 0x2f, 0x95, 0x26, 0x8e, 0x10, 0x1a, 0x2a,
};

static inline bool dev_is_hw(struct fmd_component *dev)
{
	return dev && dev->ep;
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

/*
 * The bytes of the extents the host of LD @ld_idx has, as its 5603h last
 * listed them: every one, as none is pending (dc_tags_check()). Each is a
 * child of @dev with the host's Host ID; extents are rebuilt under
 * dcd_lock, so it is taken, then the topology lock. 0 when @dev is not a
 * DCD or has no such LD; UINT64_MAX if the sum does not fit.
 */
uint64_t fmd_dc_host_bytes(struct fmd_ctx *ctx, struct fmd_component *dev, int ld_idx)
{
	struct fmd_component *child;
	uint64_t sum = 0;
	int host_id;

	if (!dev || !fmd_memdev_dcd(dev))
		return 0;

	pthread_mutex_lock(&ctx->dcd_lock);
	fmd_topology_rdlock(&ctx->topology);
	host_id = fmd_dc_host_id(dev, ld_idx);
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		if (host_id < 0 || ext->host_id != host_id)
			continue;
		if (__builtin_add_overflow(sum, ext->len, &sum)) {
			sum = UINT64_MAX;
			break;
		}
	}
	fmd_topology_rdunlock(&ctx->topology);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return sum;
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

struct fmd_dc_extent *fmd_dc_find_extent(struct fmd_component *dev, int host_id,
					 uint64_t dpa)
{
	struct fmd_component *child;

	if (!dev)
		return NULL;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		if (ext->host_id == host_id && ext->start_dpa == dpa)
			return ext;
	}
	return NULL;
}

unsigned int fmd_dc_host_extent_count(struct fmd_component *dev, int host_id,
				      uint8_t region_id)
{
	struct fmd_component *child;
	unsigned int count = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		if (ext->host_id == host_id && ext->region_id == region_id)
			count++;
	}
	return count;
}

void fmd_dcd_info_free(struct fmd_dcd_info *dcd)
{
	struct fmd_dc_mock_hold *h, *next;

	if (!dcd)
		return;
	for (h = dcd->mock_orphans; h; h = next) {
		next = h->next;
		free(h);
	}
	free(dcd->mock_refs);
	free(dcd->mock_tags);
	free(dcd->tags.v);
	free(dcd);
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
 * An all-zero tag, which cxl-fmd shows as the empty string and takes for
 * no tag. The specification calls the Tag "Optional" (Table 8-230) and does
 * not say which value means none: this is cxl-fmd's convention.
 */
static bool dc_tag_is_zero(const uint8_t *tag)
{
	static const uint8_t zero[FMD_DC_TAG_LEN];

	return !tag || memcmp(tag, zero, FMD_DC_TAG_LEN) == 0;
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

const struct fmd_dc_tag *fmd_dc_tag_find(struct fmd_component *dev,
					 const uint8_t tag[FMD_DC_TAG_LEN])
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	if (!dcd || !dcd->tags.known || dc_tag_is_zero(tag))
		return NULL;
	for (uint32_t i = 0; i < dcd->tags.n; i++) {
		if (!memcmp(dcd->tags.v[i].tag, tag, FMD_DC_TAG_LEN))
			return &dcd->tags.v[i];
	}
	return NULL;
}

/*
 * ============================================================================
 * Shared state helpers
 * ============================================================================
 */

/* "<dev>_h<host>_e<dpa>": at most 32 + 2 + 5 + 2 + 16 characters */
static void extent_id(struct fmd_component *dev, int host_id, uint64_t dpa,
		      char *buf, size_t len)
{
	snprintf(buf, len, "%.32s_h%u_e%" PRIx64, dev->id, (unsigned int)host_id, dpa);
}

static void region_id_str(struct fmd_component *owner, uint8_t region_id,
			  char *buf, size_t len)
{
	snprintf(buf, len, "%.50s_dcr%u", owner->id, region_id);
}

/*
 * Region containing a DPA, from the host's point of view. "All extents for
 * this region shall describe a memory range >= the Region Base and < the
 * Region Base + Region Decode Length" (Table 8-347): the decode length, not
 * the Region Length, bounds it.
 */
static struct fmd_dc_region *region_for_dpa(struct fmd_component *dev, int host_id,
					    uint64_t dpa)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_component *child;

	if (!owner)
		return NULL;

	fmd_for_each_child_type(owner, child, FMD_COMPONENT_DC_REGION) {
		struct fmd_dc_region *r = fmd_to_dc_region(child);
		if (dpa >= r->base_dpa && dpa - r->base_dpa < r->decode_len)
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
					int host_id, uint64_t dpa, uint64_t len,
					const uint8_t tag[FMD_DC_TAG_LEN],
					uint16_t shared_seq, uint8_t region_id)
{
	struct fmd_dc_extent *ext;
	char id[FMD_MAX_ID_LEN];

	if (host_id < 0 || host_id > 0xFFFF)
		return NULL;
	extent_id(dev, host_id, dpa, id, sizeof(id));
	ext = fmd_dc_extent_create(id);
	if (!ext)
		return NULL;

	ext->host_id = (uint16_t)host_id;
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

/*
 * ============================================================================
 * Mock implementation
 * ============================================================================
 *
 * The mock keeps one record per host extent, as a device's 5603h lists
 * them. "The DCD controls which DPA range it assigns to each region for
 * each host" (Section 9.13.3), and the mock lets a region's base differ
 * from host to host. Where the capacity is, is the mock's own notion: its
 * offset from the base of the region ("phys"), the same for every host.
 * Capacity two hosts share is one record on each, with the same Tag and
 * the same phys, at whatever DPA each host's base puts it. Capacity an FM
 * reference keeps after every host released it is a struct
 * fmd_dc_mock_hold, not an extent.
 */

/* Flags of a region, through any host that sees it */
static uint8_t region_flags_any(struct fmd_component *dev, uint8_t region_id)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	for (int h = 0; dcd && h < dcd->num_hosts; h++) {
		struct fmd_dc_region *r = fmd_dc_find_region(dev, h, region_id);

		if (r)
			return r->flags;
	}
	return 0;
}

/* Flags of the region an extent is in, as its own host sees it */
static uint8_t extent_region_flags(struct fmd_component *dev, struct fmd_dc_extent *ext)
{
	struct fmd_dc_region *r = fmd_dc_find_region(dev, ext->host_id, ext->region_id);

	return r ? r->flags : 0;
}

/* Offset of an extent from the base of its host's region (mock) */
static uint64_t extent_phys(struct fmd_component *dev, struct fmd_dc_extent *ext)
{
	struct fmd_dc_region *r = fmd_dc_find_region(dev, ext->host_id, ext->region_id);

	return r ? ext->start_dpa - r->base_dpa : ext->start_dpa;
}

/* Another host's record of the same tagged capacity (mock) */
static bool extent_shared(struct fmd_component *dev, struct fmd_dc_extent *ext)
{
	struct fmd_component *child;
	uint64_t phys = extent_phys(dev, ext);

	if (dc_tag_is_zero(ext->tag))
		return false;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *o = fmd_to_dc_extent(child);

		if (o != ext && o->host_id != ext->host_id &&
		    o->region_id == ext->region_id &&
		    !memcmp(o->tag, ext->tag, FMD_DC_TAG_LEN) &&
		    extent_phys(dev, o) == phys)
			return true;
	}
	return false;
}

static bool mock_ref_has(struct fmd_dcd_info *dcd, const uint8_t *tag)
{
	for (uint32_t i = 0; i < dcd->n_mock_refs; i++) {
		if (!memcmp(dcd->mock_refs[i], tag, FMD_DC_TAG_LEN))
			return true;
	}
	return false;
}

void fmd_dc_mock_ref_set(struct fmd_dcd_info *dcd, const uint8_t tag[FMD_DC_TAG_LEN],
			 bool on)
{
	if (dc_tag_is_zero(tag))
		return;
	for (uint32_t i = 0; i < dcd->n_mock_refs; i++) {
		if (memcmp(dcd->mock_refs[i], tag, FMD_DC_TAG_LEN))
			continue;
		if (!on)
			memmove(dcd->mock_refs[i], dcd->mock_refs[i + 1],
				(dcd->n_mock_refs - i - 1) * sizeof(dcd->mock_refs[0]));
		dcd->n_mock_refs -= !on;
		return;
	}
	if (on) {
		uint8_t (*tmp)[FMD_DC_TAG_LEN] = realloc(dcd->mock_refs,
				(dcd->n_mock_refs + 1) * sizeof(dcd->mock_refs[0]));

		if (!tmp)
			return;
		dcd->mock_refs = tmp;
		memcpy(dcd->mock_refs[dcd->n_mock_refs++], tag, FMD_DC_TAG_LEN);
	}
}

/* The mock's tag list entry of @tag, added if it has none */
static struct fmd_dc_tag *mock_tag_row(struct fmd_dcd_info *dcd, const uint8_t *tag)
{
	struct fmd_dc_tag *tmp;

	for (uint32_t i = 0; i < dcd->tags.n; i++) {
		if (!memcmp(dcd->tags.v[i].tag, tag, FMD_DC_TAG_LEN))
			return &dcd->tags.v[i];
	}
	tmp = realloc(dcd->tags.v, (dcd->tags.n + 1) * sizeof(*tmp));
	if (!tmp)
		return NULL;
	dcd->tags.v = tmp;
	tmp = &dcd->tags.v[dcd->tags.n++];
	memset(tmp, 0, sizeof(*tmp));
	memcpy(tmp->tag, tag, FMD_DC_TAG_LEN);
	return tmp;
}

/*
 * The mock answers 5608h as a device would: an entry for each tag an
 * extent or an FM reference has, the host of each extent in the Reference
 * Bitmap or, when it has not responded, in the Pending Reference Bitmap,
 * and both bitmaps valid, as they "shall" be for any device but a GFD
 * (Table 7-77).
 */
void fmd_dc_mock_retag(struct fmd_component *dev)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;

	if (!dcd)
		return;

	free(dcd->tags.v);
	dcd->tags.v = NULL;
	dcd->tags.n = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		struct fmd_dc_tag *t;
		uint8_t *bm;

		if (dc_tag_is_zero(ext->tag) || !(t = mock_tag_row(dcd, ext->tag)))
			continue;
		if (ext->host_id > FMD_DC_MAX_HOST_ID)
			continue;
		bm = ext->pending ? t->pend : t->ref;
		bm[ext->host_id / 8] |= 1 << (ext->host_id % 8);
	}
	for (hold = dcd->mock_orphans; hold; hold = hold->next)
		mock_tag_row(dcd, hold->tag);
	for (uint32_t i = 0; i < dcd->n_mock_refs; i++) {
		struct fmd_dc_tag *t = mock_tag_row(dcd, dcd->mock_refs[i]);

		if (t)
			t->fm_ref = true;
	}
	/*
	 * The file's own bitmap bits, e.g. an add 5603h does not list yet.
	 * Its FM references are in mock_refs: an entry with no bits is no
	 * tag of its own, and goes when they do.
	 */
	for (uint32_t i = 0; i < dcd->n_mock_tags; i++) {
		struct fmd_dc_tag *t;
		bool any = false;

		for (int b = 0; b < FMD_DC_LD_BITMAP_BYTES && !any; b++)
			any = dcd->mock_tags[i].ref[b] || dcd->mock_tags[i].pend[b];
		if (!any || !(t = mock_tag_row(dcd, dcd->mock_tags[i].tag)))
			continue;
		for (int b = 0; b < FMD_DC_LD_BITMAP_BYTES; b++) {
			t->ref[b] |= dcd->mock_tags[i].ref[b];
			t->pend[b] |= dcd->mock_tags[i].pend[b];
		}
	}

	dcd->tags.validity = 0x03;
	dcd->tags.known = true;
	dcd->tags.unsupported = false;
	dcd->tags.gen++;
}

/*
 * Remove a host's extent. Capacity of an FM-referenced tag "will remain
 * allocated ... even if all DCD Extents that reference it are removed"
 * (Section 7.6.7.6.7): when no other host holds it, the mock keeps it.
 */
static void mock_extent_remove(struct fmd_ctx *ctx, struct fmd_component *dev,
			       struct fmd_dc_extent *ext)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	if (dcd && mock_ref_has(dcd, ext->tag) && !extent_shared(dev, ext)) {
		struct fmd_dc_mock_hold *hold = calloc(1, sizeof(*hold));

		if (hold) {
			memcpy(hold->tag, ext->tag, FMD_DC_TAG_LEN);
			hold->region_id = ext->region_id;
			hold->shared_seq = ext->shared_seq;
			hold->phys = extent_phys(dev, ext);
			hold->len = ext->len;
			hold->next = dcd->mock_orphans;
			dcd->mock_orphans = hold;
		}
	}
	fmd_topology_remove(&ctx->topology, &ext->base);
}

/* Drop the mock's FM-held capacity of a tag */
static void mock_orphans_drop(struct fmd_dcd_info *dcd, const uint8_t *tag)
{
	struct fmd_dc_mock_hold **pp = &dcd->mock_orphans;

	while (*pp) {
		struct fmd_dc_mock_hold *h = *pp;

		if (!memcmp(h->tag, tag, FMD_DC_TAG_LEN)) {
			*pp = h->next;
			free(h);
		} else {
			pp = &h->next;
		}
	}
}

/*
 * The mock's Number of Available Extents and Tags of each host: "its total
 * capacity minus all extents and tags tracked for capacity in the Pending,
 * Added, and FM-referenced states, respectively" (Section 9.13.3.3). The
 * counts are "on this LD" (Table 7-67). An extent a host holds, pending or
 * not, counts against that host, and each distinct non-zero tag among them
 * once. Which LD an FM-referenced extent that no host holds any more counts
 * against the specification does not say: the mock counts it, and its tag,
 * against every host that sees its region.
 */
void fmd_dc_mock_recount(struct fmd_component *dev)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	const uint8_t **tags;
	size_t n_ext = 0;

	if (!dcd)
		return;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT)
		n_ext++;
	for (hold = dcd->mock_orphans; hold; hold = hold->next)
		n_ext++;
	tags = calloc(n_ext ? n_ext : 1, sizeof(*tags));
	if (!tags)
		return;

	for (int h = 0; h < dcd->num_hosts; h++) {
		struct fmd_component *owner = fmd_dc_host_component(dev, h);
		uint32_t *ext_avail, *tag_avail, ext_sup, tag_sup;
		uint32_t extents = 0, n_tags = 0;

		if (!owner)
			continue;
		if (owner->type == FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(owner);

			ext_avail = &ld->dc_extents_available;
			tag_avail = &ld->dc_tags_available;
			ext_sup = ld->dc_extents_supported;
			tag_sup = ld->dc_tags_supported;
		} else {
			ext_avail = &dcd->extents_available;
			tag_avail = &dcd->tags_available;
			ext_sup = dcd->extents_supported;
			tag_sup = dcd->tags_supported;
		}

		child = dev->first_child;
		hold = dcd->mock_orphans;
		for (;;) {
			const uint8_t *tag;
			uint32_t t;

			if (child) {
				struct fmd_dc_extent *ext;

				if (child->type != FMD_COMPONENT_DC_EXTENT) {
					child = child->next_sibling;
					continue;
				}
				ext = fmd_to_dc_extent(child);
				child = child->next_sibling;
				if (ext->host_id != h)
					continue;
				tag = ext->tag;
			} else if (hold) {
				struct fmd_dc_mock_hold *o = hold;

				hold = hold->next;
				if (!fmd_dc_find_region(dev, h, o->region_id))
					continue;
				tag = o->tag;
			} else {
				break;
			}

			extents++;
			if (dc_tag_is_zero(tag))
				continue;
			for (t = 0; t < n_tags; t++) {
				if (!memcmp(tags[t], tag, FMD_DC_TAG_LEN))
					break;
			}
			if (t == n_tags)
				tags[n_tags++] = tag;
		}

		*ext_avail = extents < ext_sup ? ext_sup - extents : 0;
		*tag_avail = n_tags < tag_sup ? tag_sup - n_tags : 0;
	}

	free(tags);
}

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

/*
 * Sorted list of the ranges of @region (one host's view of it) that are
 * taken: by any host's extent, or held by an FM reference. Each is placed
 * at its offset from the region base, in the DPA space of @region's host.
 */
static int collect_region_ranges(struct fmd_component *dev, struct fmd_dc_region *region,
				 struct dpa_range **out)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	struct dpa_range *ranges;
	int count = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		if (fmd_to_dc_extent(child)->region_id == region->region_id)
			count++;
	}
	for (hold = dcd->mock_orphans; hold; hold = hold->next) {
		if (hold->region_id == region->region_id)
			count++;
	}

	ranges = calloc(count ? count : 1, sizeof(*ranges));
	if (!ranges)
		return -1;

	count = 0;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		if (ext->region_id == region->region_id) {
			ranges[count].start = region->base_dpa + extent_phys(dev, ext);
			ranges[count].end = ranges[count].start + ext->len;
			count++;
		}
	}
	for (hold = dcd->mock_orphans; hold; hold = hold->next) {
		if (hold->region_id == region->region_id) {
			ranges[count].start = region->base_dpa + hold->phys;
			ranges[count].end = ranges[count].start + hold->len;
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
	unsigned int seq = 0;
	int rc = 0;

	n_used = collect_region_ranges(dev, region, &used);
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

	/*
	 * Shared Extent Sequence: "For extents that describe non-shareable
	 * regions, this field shall be 0. For extents describing shareable
	 * regions this field shall be within the range of 0 to n-1 where n
	 * is the number of extents, with each value appearing only once"
	 * (Table 8-230). The table is one host's Extent List: the pieces of
	 * a Sharable add are numbered on from the host's extents with the tag.
	 */
	if (region->flags & FMD_DC_REGION_SHAREABLE) {
		struct fmd_component *child;

		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *e = fmd_to_dc_extent(child);

			if (e->host_id == host_id &&
			    memcmp(e->tag, tag, FMD_DC_TAG_LEN) == 0 &&
			    (unsigned int)e->shared_seq + 1 > seq)
				seq = e->shared_seq + 1;
		}
	}

	for (int i = 0; i < n_pieces; i++) {
		bool shared = region->flags & FMD_DC_REGION_SHAREABLE;
		struct fmd_dc_extent *ext = fmd_dc_extent_add(ctx, dev, host_id, pieces[i].start,
							      pieces[i].end - pieces[i].start,
							      tag, shared ? seq + i : 0,
							      region->region_id);
		if (!ext) {
			rc = -ENOMEM;
			goto out;
		}
		/* The mock host accepts immediately, so nothing is left pending */

		fmd_info(ctx, "%s: mock DC add host %d region %u dpa 0x%" PRIx64
			 " len %" PRIu64 " MB", dev->id, host_id, region->region_id,
			 ext->start_dpa, (uint64_t)(ext->len / FMD_MIB));
	}

out:
	free(used);
	free(pieces);
	return rc;
}

/*
 * Enable Shared Access: give @host_id, in @region, the capacity the tag
 * names. It is copied from the holder with the lowest Host ID, each piece
 * at the same offset into @host_id's view of the region, or from what an
 * FM reference keeps when no host holds it.
 */
static int mock_share(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		      struct fmd_dc_region *region, const uint8_t tag[FMD_DC_TAG_LEN])
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	int src = -1;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		if (ext->region_id == region->region_id &&
		    !memcmp(ext->tag, tag, FMD_DC_TAG_LEN) &&
		    (src < 0 || ext->host_id < src))
			src = ext->host_id;
	}

	if (src >= 0) {
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child), *copy;

			if (ext->host_id != src || ext->region_id != region->region_id ||
			    memcmp(ext->tag, tag, FMD_DC_TAG_LEN))
				continue;
			copy = fmd_dc_extent_add(ctx, dev, host_id,
						 region->base_dpa + extent_phys(dev, ext),
						 ext->len, tag, ext->shared_seq,
						 region->region_id);
			if (!copy)
				return -ENOMEM;
		}
		return 0;
	}

	for (hold = dcd->mock_orphans; hold; hold = hold->next) {
		struct fmd_dc_extent *copy;

		if (hold->region_id != region->region_id ||
		    memcmp(hold->tag, tag, FMD_DC_TAG_LEN))
			continue;
		copy = fmd_dc_extent_add(ctx, dev, host_id, region->base_dpa + hold->phys,
					 hold->len, tag, hold->shared_seq, region->region_id);
		if (!copy)
			return -ENOMEM;
	}
	/* a host holds it now */
	mock_orphans_drop(dcd, tag);
	return 0;
}

static int mock_dc_add(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		       uint8_t policy, uint8_t region_id, uint64_t length,
		       const uint8_t tag[FMD_DC_TAG_LEN],
		       struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_dc_region *region;
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	int matched = 0, held = 0;

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
			/*
			 * Invalid Extent List "When the Selection Policy is
			 * set to Prescriptive and any of the DPAs are already
			 * accessible to the same LD" (Section 7.6.7.6.5).
			 */
			if (ext->host_id == host_id)
				return -EINVAL;
		}
		/*
		 * Every extent is some host's, in that host's DPA space:
		 * capacity another host holds is shared with Enable Shared
		 * Access and its tag, not named again here. The specification
		 * does not say what a device does with such a list: refusing
		 * it is the mock's choice.
		 */
		return -EBUSY;

	case FMD_DC_POLICY_SHARED_ACCESS:
		/*
		 * Section 7.6.7.6.5: Enable Shared Access "shall also fail
		 * with Invalid Input" "When the specified region is not
		 * Sharable", "When the tagged capacity is already mapped to
		 * any Host ID via a non-Sharable region", "When the tagged
		 * capacity cannot be added to the requested region due to
		 * device-imposed restrictions" and "When the same tagged
		 * capacity is currently accessible by the same LD". The
		 * mock's restriction is that the capacity is added in the
		 * region number its holders have it in: phys is an offset
		 * into that region.
		 */
		region = fmd_dc_find_region(dev, host_id, region_id);
		if (!region)
			return -ENOENT;
		if (!(region->flags & FMD_DC_REGION_SHAREABLE))
			return -EINVAL;
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

			if (memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
				continue;
			if (!(extent_region_flags(dev, ext) & FMD_DC_REGION_SHAREABLE) ||
			    ext->host_id == host_id)
				return -EINVAL;
			held++;
			if (ext->region_id == region_id)
				matched++;
		}
		for (hold = dcd->mock_orphans; hold; hold = hold->next) {
			if (memcmp(hold->tag, tag, FMD_DC_TAG_LEN))
				continue;
			held++;
			if (hold->region_id == region_id)
				matched++;
		}
		/* no capacity has the tag at all */
		if (!held)
			return -ENOENT;
		if (!matched)
			return -EINVAL;
		return mock_share(ctx, dev, host_id, region, tag);

	default:
		return -EINVAL;
	}
}

/* Whether @ext is in a Sharable region of its own host and has a tag */
static bool extent_sharable_tagged(struct fmd_component *dev, struct fmd_dc_extent *ext)
{
	return !dc_tag_is_zero(ext->tag) &&
	       (extent_region_flags(dev, ext) & FMD_DC_REGION_SHAREABLE);
}

static int mock_dc_release(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			   uint8_t policy, bool forced, uint64_t length,
			   const uint8_t tag[FMD_DC_TAG_LEN],
			   struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_component *child, *next;
	uint64_t remaining = length, held = 0;
	bool sharable = false;

	switch (policy) {
	case FMD_DC_REL_POLICY_TAG_BASED:
		/*
		 * Release Length bytes carrying Tag. Length "Shall be > 0"
		 * (Table 7-73), and more than the host holds with the tag is
		 * Resources Exhausted (Section 7.6.7.6.6). Of sharable
		 * capacity, the host's access is removed with the whole of
		 * it: Invalid Input "When the Tag represents sharable
		 * capacity, and the Extent List covers only a portion of the
		 * capacity associated with the Tag" (Section 7.6.7.6.6). That
		 * a Length short of it is that portion is the mock's reading:
		 * the sentence names the Extent List. Capacity in a region
		 * that is not Sharable is trimmed from the tail.
		 */
		if (length == 0)
			return -EINVAL;
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

			if (ext->host_id != host_id ||
			    memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
				continue;
			held += ext->len;
			if (extent_sharable_tagged(dev, ext))
				sharable = true;
		}
		if (!held)
			return -ENOENT;
		if (length > held)
			return -ENOSPC;
		if (sharable && length < held)
			return -EINVAL;

		for (child = dev->first_child; child && remaining; child = next) {
			struct fmd_dc_extent *ext;

			next = child->next_sibling;
			if (child->type != FMD_COMPONENT_DC_EXTENT)
				continue;
			ext = fmd_to_dc_extent(child);
			if (ext->host_id != host_id ||
			    memcmp(ext->tag, tag, FMD_DC_TAG_LEN) != 0)
				continue;

			if (remaining < ext->len) {
				ext->len -= remaining;
				remaining = 0;
				continue;
			}
			remaining -= ext->len;

			mock_extent_remove(ctx, dev, ext);
		}
		return 0;

	case FMD_DC_REL_POLICY_PRESCRIPTIVE:
		if (ext_count <= 0 || !extents)
			return -EINVAL;
		for (int i = 0; i < ext_count; i++) {
			struct fmd_dc_extent *ext = extents[i];

			if (!ext || ext->host_id != host_id)
				return -ENOENT;
			/*
			 * Invalid Physical Address "if an extent in the Extent
			 * List covers a nonexistent or pending ... DPA range
			 * and the Forced Removal flag is not set" (Section
			 * 7.6.7.6.6); with it, the device marks the Extent
			 * Group "Dead", and the extent goes.
			 */
			if (ext->pending && !forced)
				return -EINVAL;
			/*
			 * A list that covers only part of the host's capacity
			 * of a sharable tag is Invalid Input (Section
			 * 7.6.7.6.6), as it is a malformed list for the host's
			 * own Release Dynamic Capacity (Section 8.2.10.9.9.4).
			 */
			if (!extent_sharable_tagged(dev, ext))
				continue;
			fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
				struct fmd_dc_extent *o = fmd_to_dc_extent(child);
				bool listed = false;

				if (o->host_id != host_id ||
				    memcmp(o->tag, ext->tag, FMD_DC_TAG_LEN) ||
				    !extent_sharable_tagged(dev, o))
					continue;
				for (int j = 0; j < ext_count && !listed; j++)
					listed = extents[j] == o;
				if (!listed)
					return -EINVAL;
			}
		}
		for (int i = 0; i < ext_count; i++) {
			mock_extent_remove(ctx, dev, extents[i]);
		}
		return 0;

	default:
		return -EINVAL;
	}
}

/*
 * 5606h and 5607h "shall return Invalid Input if the Tag in the payload
 * does not match an existing sharable tag" (Sections 7.6.7.6.7 and
 * 7.6.7.6.8): the tag must be on an extent in a Sharable region, or be
 * capacity an FM reference keeps.
 */
static int mock_dc_reference(struct fmd_ctx *ctx, struct fmd_component *dev,
			     const uint8_t tag[FMD_DC_TAG_LEN], bool add)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	bool sharable = false;

	(void)ctx;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		if (memcmp(ext->tag, tag, FMD_DC_TAG_LEN) == 0 &&
		    (extent_region_flags(dev, ext) & FMD_DC_REGION_SHAREABLE))
			sharable = true;
	}
	for (hold = dcd->mock_orphans; hold; hold = hold->next) {
		if (!memcmp(hold->tag, tag, FMD_DC_TAG_LEN) &&
		    (region_flags_any(dev, hold->region_id) & FMD_DC_REGION_SHAREABLE))
			sharable = true;
	}
	if (!sharable)
		return -EINVAL;

	/* adding it again succeeds (Section 7.6.7.6.7) */
	fmd_dc_mock_ref_set(dcd, tag, add);
	/* the reference was what kept it */
	if (!add)
		mock_orphans_drop(dcd, tag);

	return 0;
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
 * The 56xxh commands are sent untunnelled to a device on an MCTP endpoint
 * of its own, and to a switch-attached MLD with the Tunnel Management
 * Command, which "instructs a switch to tunnel the provided command to the
 * FM-owned LD of the MLD on the specified port" (Section 7.6.7.3.1). They are never tunnelled to an
 * LD: the command set is "used by the FM to manage memory assignment within
 * an LD-FAM DCD" (Section 7.6.7.6), and names the host by Host ID.
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

/*
 * What a DCD Management command that failed comes to. A negative @rc is
 * libcxlmi failing to carry the command: alike for a request not sent, one
 * not answered, and a response it could not read, so the device may have
 * carried it out, and the outcome is not known (-EREMOTEIO); anything else
 * is the Return Code the device gave. Initiate Dynamic
 * Capacity Add (5604h) and Release (5605h) fail with Invalid Input, Invalid
 * Extent List or Invalid Physical Address for what was asked, with
 * Resources Exhausted when the device has no room for it, and with Retry
 * Required when the LD's Dynamic Capacity Event Log would overflow (CXL
 * r4.0 Sections 7.6.7.6.5 and 7.6.7.6.6). Set DC Region Configuration
 * (5602h) "shall fail with Unsupported" when "one or more blocks are
 * allocated to the specified region" (Section 7.6.7.6.3); its other
 * Unsupported case, a Sanitize on Release that cannot be changed, is
 * refused before the command is sent.
 *
 * Returns -EINVAL, -ENOSPC, -EAGAIN (Retry Required or Busy), -EBUSY
 * (5602h Unsupported), -EREMOTEIO (outcome not known) or -EIO for anything
 * else.
 */
static int dc_cmd_errno(int rc, bool unsupported_is_busy)
{
	if (rc < 0)
		return -EREMOTEIO;

	switch (rc) {
	case CXLMI_RET_INPUT:
	case CXLMI_RET_EXTLIST:
	case CXLMI_RET_PADDR:
		return -EINVAL;
	case CXLMI_RET_RESOURCEEXHAUSTED:
		return -ENOSPC;
	case CXLMI_RET_RETRY:
	case CXLMI_RET_BUSY:
		return -EAGAIN;
	case CXLMI_RET_UNSUPPORTED:
		return unsupported_is_busy ? -EBUSY : -EIO;
	default:
		return -EIO;
	}
}

static void dc_cmd_failed(struct fmd_ctx *ctx, struct fmd_component *dev,
			  const char *what, int rc)
{
	if (rc < 0)
		fmd_warn(ctx, "%s: %s: outcome not known (not sent, not answered, or "
			 "the response could not be read): %d", dev->id, what, rc);
	else
		fmd_warn(ctx, "%s: %s failed: %d (%s)", dev->id, what, rc,
			 cxlmi_cmd_retcode_tostr(rc));
}

/*
 * The refresh after a command the device accepted, or whose outcome is not
 * known: see dc_refresh_after().
 */
static int dc_refresh_after(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			    const char *what);
static void dc_hosts_dirty(struct fmd_component *dev, int host_id);

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

/* One region of a 5601h read, staged until every page has been read */
struct dc_region_staged {
	bool     present;
	uint64_t base_dpa, decode_len, region_len, block_size;
	uint8_t  flags;
	bool     sanitize_on_release;
};

/*
 * The host's region table becomes what @st says: a region still there is
 * updated in place, so that its component, and what refers to it, stays;
 * one no longer there goes; a new one is added. An extent of the host
 * takes the region its DPA is in now, so that none names a region that
 * has gone, even when the read of the extents after this fails.
 */
static int apply_host_regions(struct fmd_ctx *ctx, struct fmd_component *dev,
			      struct fmd_component *owner, int host_id,
			      const struct dc_region_staged *st)
{
	struct fmd_component *child, *next;
	bool have[FMD_MAX_DC_REGIONS] = { false };

	for (child = owner->first_child; child; child = next) {
		struct fmd_dc_region *r;

		next = child->next_sibling;
		if (child->type != FMD_COMPONENT_DC_REGION)
			continue;
		r = fmd_to_dc_region(child);
		if (r->region_id >= FMD_MAX_DC_REGIONS || !st[r->region_id].present ||
		    have[r->region_id]) {
			fmd_topology_remove(&ctx->topology, child);
			continue;
		}
		have[r->region_id] = true;
	}

	for (int id = 0; id < FMD_MAX_DC_REGIONS; id++) {
		struct fmd_dc_region *r;

		if (!st[id].present)
			continue;
		r = have[id] ? fmd_dc_find_region(dev, host_id, id) :
			       fmd_dc_region_add(ctx, owner, id, host_id);
		if (!r)
			return -ENOMEM;
		r->base_dpa = st[id].base_dpa;
		r->decode_len = st[id].decode_len;
		r->region_len = st[id].region_len;
		r->block_size = st[id].block_size;
		r->flags = st[id].flags;
		r->sanitize_on_release = st[id].sanitize_on_release;
	}

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		struct fmd_dc_region *r;

		if (ext->host_id != host_id)
			continue;
		r = region_for_dpa(dev, host_id, ext->start_dpa);
		ext->region_id = r ? r->region_id : FMD_DC_REGION_NONE;
	}
	return 0;
}

/*
 * Read the host's regions with Get Host DC Region Configuration (5601h), a
 * page at a time, and change the table only once every page has been read
 * (cxl-fmd's choice). 5601h has no generation number, unlike 5603h, so a
 * table that changes between pages cannot be told from one that does not:
 * what is staged is what the pages said. A page that fails leaves the
 * table, and the host's counters, as they were, and the host is marked
 * stale by the caller.
 */
static int hw_refresh_regions(struct fmd_ctx *ctx, struct fmd_component *dev,
			      struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti,
			      int host_id)
{
	struct cxlmi_cmd_fmapi_get_host_dc_region_config_req req = {0};
	/* room for as many region entries as the device can claim to return */
	union {
		struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp rsp;
		uint8_t room[sizeof(struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp) +
			     FMD_CCI_LIST_MAX *
			     sizeof(((struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp *)0)->region_configs[0])];
	} u = { 0 };
	struct cxlmi_cmd_fmapi_get_host_dc_region_config_rsp *rsp = &u.rsp;
	struct dc_region_staged st[FMD_MAX_DC_REGIONS] = { 0 };
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	int rc;

	if (!owner)
		return -ENOENT;

	/*
	 * Region Count regions from Starting Region Index on, as many at a
	 * time as fit a response: 4h + regions * 28h + 10h (Table 7-67), and
	 * all eight do not fit the 256 bytes every CCI supports.
	 */
	for (int start = 0; start < FMD_MAX_DC_REGIONS; ) {
		int want = fmd_cci_list_fit(dev, 0x4 + 0x10, 0x28,
					    FMD_MAX_DC_REGIONS - start);

		req.host_id = host_id;
		req.region_cnt = want;
		req.start_region_id = start;

		rc = cxlmi_cmd_fmapi_get_dc_reg_config(ep, ti, &req, rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get host %d DC region config failed: %d",
				 dev->id, host_id, rc);
			return -EIO;
		}
		/* past the regions asked for, the counts after them are overwritten */
		if (rsp->regions_returned > want) {
			fmd_warn(ctx, "%s: host %d: %d DC regions returned, %d requested",
				 dev->id, host_id, rsp->regions_returned, want);
			return -EIO;
		}

		for (int i = 0; i < rsp->regions_returned; i++) {
			struct dc_region_staged *r = &st[start + i];

			/* "Report 0 if the region is unavailable" (Table 8-347) */
			if (rsp->region_configs[i].region_len == 0)
				continue;

			r->present = true;
			r->base_dpa = rsp->region_configs[i].base;
			r->decode_len = rsp->region_configs[i].decode_len;
			r->region_len = rsp->region_configs[i].region_len;
			r->block_size = rsp->region_configs[i].block_size;
			r->flags = rsp->region_configs[i].flags;
			r->sanitize_on_release = rsp->region_configs[i].sanitize_on_release & 0x01;
		}

		start += rsp->regions_returned;
		/* Number of Available Regions, and a device with nothing more */
		if (rsp->regions_returned == 0 || start >= rsp->num_regions)
			break;
	}

	rc = apply_host_regions(ctx, dev, owner, host_id, st);
	if (rc)
		return rc;

	/* the counters after the last page's regions */
	if (owner->type == FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(owner);
		ld->dc_extents_supported = rsp->num_extents_supported;
		ld->dc_extents_available = rsp->num_extents_available;
		ld->dc_tags_supported = rsp->num_tags_supported;
		ld->dc_tags_available = rsp->num_tags_available;
	} else if (dcd) {
		dcd->extents_supported = rsp->num_extents_supported;
		dcd->extents_available = rsp->num_extents_available;
		dcd->tags_supported = rsp->num_tags_supported;
		dcd->tags_available = rsp->num_tags_available;
	}

	return 0;
}

/* A host's 5603h state: on the SLD's dcd for host 0, on the LD otherwise */
static void dc_host_sync_ptrs(struct fmd_component *dev, int host_id, uint32_t **gen,
			      uint8_t **sync, int **err)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);

	*gen = NULL;
	if (owner && owner->type == FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(owner);

		*gen = &ld->dc_ext_gen;
		*sync = &ld->dc_sync;
		*err = &ld->dc_sync_err;
	} else if (owner && dcd) {
		*gen = &dcd->dc_ext_gen;
		*sync = &dcd->dc_sync;
		*err = &dcd->dc_sync_err;
	}
}

enum fmd_dc_sync fmd_dc_host_sync(struct fmd_component *dev, int host_id)
{
	uint32_t *gen;
	uint8_t *sync;
	int *err;

	dc_host_sync_ptrs(dev, host_id, &gen, &sync, &err);
	return gen ? (enum fmd_dc_sync)*sync : FMD_DC_SYNC_NEVER;
}

/* Record how a host's 5603h read went; @rc 0 is a read that completed */
static void dc_host_synced(struct fmd_component *dev, int host_id, int rc,
			   uint32_t generation)
{
	uint32_t *gen;
	uint8_t *sync;
	int *err;

	dc_host_sync_ptrs(dev, host_id, &gen, &sync, &err);
	if (!gen)
		return;
	if (rc) {
		*sync = FMD_DC_SYNC_STALE;
		*err = rc;
	} else {
		*gen = generation;
		*sync = FMD_DC_SYNC_OK;
		*err = 0;
	}
}

/*
 * The host is to be read again: what it has may have changed and nothing
 * may say so again. Only a read that completes, through dc_host_synced(),
 * takes it back to OK; until then fmd_dcd_retry_stale() reads it.
 */
static void dc_host_dirty(struct fmd_component *dev, int host_id)
{
	uint32_t *gen;
	uint8_t *sync;
	int *err;

	dc_host_sync_ptrs(dev, host_id, &gen, &sync, &err);
	if (gen && *sync != FMD_DC_SYNC_STALE)
		*sync = FMD_DC_SYNC_DIRTY;
}

/*
 * Bring the host's extents in line with 5603h, the "Extent list for the
 * specified host" (Table 7-71). The list is read a page at a time, and from
 * Starting Extent Index 0 again when the Extent List Generation Number, "a
 * device-generated value that is used to indicate that the list has
 * changed", changes on a later page. Only a list read to its end is used:
 * new DPAs are added, the host's extents the device no longer lists are
 * dropped, and the rest are updated in place so their identity survives the
 * refresh. A read that fails leaves the host's extents as they were, marked
 * stale. The extents of other hosts are not touched: "The DPA ranges
 * exposed by the device to each host are independent of one another"
 * (Section 9.13.3).
 */
static int hw_refresh_extents(struct fmd_ctx *ctx, struct fmd_component *dev,
			      struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti,
			      int host_id)
{
	struct cxlmi_cmd_fmapi_get_dc_region_ext_list_req req;
	struct cxlmi_cmd_fmapi_get_dc_region_ext_list_rsp *rsp;
	struct fmd_component *child, *next;
	typeof(rsp->extents[0]) *list = NULL;
	uint32_t n = 0, cap = 0;
	uint32_t start = 0, generation = 0;
	int retries = 0;
	int rc = 0;

	rsp = calloc(1, sizeof(*rsp) + DC_EXT_LIST_PAGE * sizeof(rsp->extents[0]));
	if (!rsp)
		return -ENOMEM;

	for (;;) {
		memset(&req, 0, sizeof(req));
		req.host_id = host_id;
		req.extent_count = fmd_cci_list_fit(dev, sizeof(*rsp),
						    sizeof(rsp->extents[0]),
						    DC_EXT_LIST_PAGE);
		req.start_ext_index = start;

		rc = cxlmi_cmd_fmapi_get_dc_region_ext_list(ep, ti, &req, rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get host %d DC extent list failed: %d",
				 dev->id, host_id, rc);
			rc = -EIO;
			goto out;
		}
		FMD_CCI_LIST_CLAMP(rsp->extents_returned, DC_EXT_LIST_PAGE);

		if (start == 0) {
			generation = rsp->list_generation_num;
		} else if (rsp->list_generation_num != generation) {
			/* List changed underneath us: start over */
			if (++retries > DC_LIST_RETRIES) {
				fmd_warn(ctx, "%s: host %d DC extent list kept changing "
					 "as it was read", dev->id, host_id);
				rc = -EAGAIN;
				goto out;
			}
			n = 0;
			start = 0;
			continue;
		}

		if (n + rsp->extents_returned > cap) {
			uint32_t ncap = 2 * (n + rsp->extents_returned);
			typeof(list) tmp = realloc(list, ncap * sizeof(*list));

			if (!tmp) {
				rc = -ENOMEM;
				goto out;
			}
			list = tmp;
			cap = ncap;
		}
		if (rsp->extents_returned)
			memcpy(&list[n], rsp->extents,
			       rsp->extents_returned * sizeof(*list));
		n += rsp->extents_returned;

		start += rsp->extents_returned;
		if (rsp->extents_returned == 0 || start >= rsp->total_extents)
			break;
	}

	for (uint32_t i = 0; i < n; i++) {
		uint64_t dpa = list[i].start_dpa;
		struct fmd_dc_extent *ext = fmd_dc_find_extent(dev, host_id, dpa);
		struct fmd_dc_region *region = region_for_dpa(dev, host_id, dpa);
		uint8_t rid = region ? region->region_id : FMD_DC_REGION_NONE;

		if (!region)
			fmd_warn(ctx, "%s: host %d extent at %#" PRIx64
				 " is in none of its regions", dev->id,
				 host_id, dpa);
		if (!ext) {
			ext = fmd_dc_extent_add(ctx, dev, host_id, dpa, list[i].len,
						list[i].tag, list[i].shared_seq, rid);
			if (!ext) {
				rc = -ENOMEM;
				goto out;
			}
		} else {
			ext->len = list[i].len;
			memcpy(ext->tag, list[i].tag, FMD_DC_TAG_LEN);
			ext->shared_seq = list[i].shared_seq;
			/* the regions were read again just before */
			ext->region_id = rid;
		}
	}

	/* Drop the host's extents the device no longer lists for it */
	for (child = dev->first_child; child; child = next) {
		struct fmd_dc_extent *ext;
		uint32_t i;

		next = child->next_sibling;
		if (child->type != FMD_COMPONENT_DC_EXTENT)
			continue;
		ext = fmd_to_dc_extent(child);
		if (ext->host_id != host_id)
			continue;
		for (i = 0; i < n && list[i].start_dpa != ext->start_dpa; i++)
			;
		if (i == n)
			fmd_topology_remove(&ctx->topology, &ext->base);
	}

out:
	dc_host_synced(dev, host_id, rc, generation);
	free(list);
	free(rsp);
	return rc;
}

/*
 * The tag list against the extents 5603h lists. No extent is made pending
 * from it (cxl-fmd's choice): the Pending Reference Bitmap is per tag,
 * "Each 1 indicates an LD for which the tagged capacity has been added with
 * no host response yet" (Table 7-78), and a host can hold accepted capacity
 * and an add it has not answered under one tag; which of its extents, if
 * any, is that add, the bitmap does not say. An extent 5603h lists is not
 * pending; an add an LD has not answered is shown only in the tag view
 * (PendingLDs).
 */
static void dc_tags_check(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		const struct fmd_dc_tag *t = fmd_dc_tag_find(dev, ext->tag);

		/*
		 * 5603h lists it for the host, and the tag list says the
		 * host neither accepted it nor has it pending: the two were
		 * read at different times.
		 */
		if (t && (dcd->tags.validity & 0x01) && ext->host_id <= FMD_DC_MAX_HOST_ID &&
		    !fmd_dc_bm_test(t->ref, ext->host_id) &&
		    !fmd_dc_bm_test(t->pend, ext->host_id))
			fmd_dbg(ctx, "%s: host %u extent at %#" PRIx64 " is not in its "
				"tag's bitmaps", dev->id, ext->host_id, ext->start_dpa);
	}
}

/* Rows a 5608h read keeps at most: Total Number of Tags is not trusted */
#define DC_TAG_LIST_MAX 4096

/*
 * Dynamic Capacity List Tags (5608h) into the device's tag list. The list
 * is read a page at a time from Starting Index 0, and read again from 0
 * when the Generation Number, which "shall change every time the remainder
 * of the command's payload would change" (Table 7-77), changes on a later
 * page. Total Number of Tags is described as "Maximum number of tags to
 * return in the response payload", the text of Max Tags: the read stops at
 * it, or at a page with no tags, whichever comes first, and keeps at most
 * DC_TAG_LIST_MAX entries.
 *
 * Only a complete read replaces the list. 5608h is "DCO" and "DCS" in
 * Table 8-397, as every 56xxh command is: "Optional for DCDs", and
 * "Mandatory for all DCDs that implement an MCTP-based CCI". A device
 * without it answers Unsupported, and has no list; on any other failure
 * the last list is kept, and is not known to be current. Neither fails
 * the refresh, and the extents are kept either way.
 */
static int hw_refresh_tags(struct fmd_ctx *ctx, struct fmd_component *dev,
			   struct cxlmi_endpoint *ep, struct cxlmi_tunnel_info *ti)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct cxlmi_cmd_fmapi_dc_list_tags_req req;
	struct cxlmi_cmd_fmapi_dc_list_tags_rsp *rsp;
	struct fmd_dc_tag *v = NULL;
	uint32_t n = 0, cap = 0, start = 0, gen = 0;
	uint8_t validity = 0;
	int retries = 0;
	int rc = 0;

	rsp = calloc(1, sizeof(*rsp) + DC_TAG_LIST_PAGE * sizeof(rsp->tags_list[0]));
	if (!rsp)
		return -ENOMEM;

	for (;;) {
		memset(&req, 0, sizeof(req));
		req.start_idx = start;
		req.tags_count = fmd_cci_list_fit(dev, sizeof(*rsp),
						  sizeof(rsp->tags_list[0]),
						  DC_TAG_LIST_PAGE);

		rc = cxlmi_cmd_fmapi_dc_list_tags(ep, ti, &req, rsp);
		if (rc == CXLMI_RET_UNSUPPORTED) {
			fmd_dbg(ctx, "%s: DC list tags unsupported", dev->id);
			free(dcd->tags.v);
			dcd->tags.v = NULL;
			dcd->tags.n = 0;
			dcd->tags.known = false;
			dcd->tags.unsupported = true;
			goto done;
		}
		if (rc) {
			fmd_warn(ctx, "%s: DC list tags failed: %d", dev->id, rc);
			goto failed;
		}

		FMD_CCI_LIST_CLAMP(rsp->num_tags_returned, DC_TAG_LIST_PAGE);

		if (start == 0) {
			gen = rsp->generation_num;
			validity = rsp->validity_bitmap;
		} else if (rsp->generation_num != gen) {
			/* the list changed while it was read: again from 0 */
			if (++retries > DC_LIST_RETRIES) {
				fmd_warn(ctx, "%s: DC tag list kept changing as it was read",
					 dev->id);
				goto failed;
			}
			n = 0;
			start = 0;
			continue;
		}

		if (n + rsp->num_tags_returned > DC_TAG_LIST_MAX) {
			fmd_warn(ctx, "%s: DC tag list is longer than %d tags",
				 dev->id, DC_TAG_LIST_MAX);
			goto failed;
		}
		if (n + rsp->num_tags_returned > cap) {
			uint32_t ncap = 2 * (n + rsp->num_tags_returned);
			struct fmd_dc_tag *tmp = realloc(v, ncap * sizeof(*v));

			if (!tmp) {
				rc = -ENOMEM;
				goto out;
			}
			v = tmp;
			cap = ncap;
		}

		for (uint32_t i = 0; i < rsp->num_tags_returned; i++) {
			struct fmd_dc_tag *t = &v[n++];

			memcpy(t->tag, rsp->tags_list[i].tag, FMD_DC_TAG_LEN);
			t->fm_ref = rsp->tags_list[i].flags & 0x01;
			memcpy(t->ref, rsp->tags_list[i].ref_bitmap, FMD_DC_LD_BITMAP_BYTES);
			memcpy(t->pend, rsp->tags_list[i].pending_ref_bitmap,
			       FMD_DC_LD_BITMAP_BYTES);
		}

		start += rsp->num_tags_returned;
		if (rsp->num_tags_returned == 0 || start >= rsp->total_num_tags)
			break;
	}

	free(dcd->tags.v);
	dcd->tags.v = v;
	dcd->tags.n = n;
	dcd->tags.gen = gen;
	dcd->tags.validity = validity;
	dcd->tags.known = true;
	dcd->tags.unsupported = false;
	v = NULL;
	goto done;

failed:
	/* the last list is kept, not known to be current; nor is this Unsupported */
	dcd->tags.known = false;
	dcd->tags.unsupported = false;
done:
	rc = 0;
	dc_tags_check(ctx, dev);
out:
	free(v);
	free(rsp);
	return rc;
}

/* Caller holds ctx->dcd_lock */
static int dcd_refresh_hosts_locked(struct fmd_ctx *ctx, struct fmd_component *dev,
				    const int *hosts, int n)
{
	struct cxlmi_tunnel_info tbuf, *ti;
	struct cxlmi_endpoint *ep;
	int rc = 0;

	if (unlikely(!dev_is_hw(dev)) || !fmd_memdev_dcd(dev))
		return 0;

	ep = dev->ep;
	ti = dcd_tunnel(dev, &tbuf);

	/*
	 * A host whose regions or extents could not be read keeps what it
	 * had, marked stale, and the other hosts and the tag list are still
	 * read: the first failure is returned (cxl-fmd's choice).
	 */
	for (int i = 0; i < n; i++) {
		int r = hw_refresh_regions(ctx, dev, ep, ti, hosts[i]);

		if (r)
			dc_host_synced(dev, hosts[i], r, 0);
		else
			r = hw_refresh_extents(ctx, dev, ep, ti, hosts[i]);
		if (r && !rc)
			rc = r;
	}

	{
		int r = hw_refresh_tags(ctx, dev, ep, ti);

		if (r && !rc)
			rc = r;
	}
	return rc;
}

/* Caller holds ctx->dcd_lock: one host, or every host for @host_id -1 */
static int dcd_refresh_locked(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id)
{
	int hosts[FMD_DC_MAX_HOST_ID + 1];
	int n;

	if (host_id >= 0) {
		hosts[0] = host_id;
		n = 1;
	} else {
		n = dc_hosts(dev, hosts, FMD_DC_MAX_HOST_ID + 1);
	}
	return dcd_refresh_hosts_locked(ctx, dev, hosts, n);
}

/* One host, or every host for @host_id -1, is to be read again */
static void dc_hosts_dirty(struct fmd_component *dev, int host_id)
{
	int hosts[FMD_DC_MAX_HOST_ID + 1];
	int n;

	if (host_id >= 0) {
		dc_host_dirty(dev, host_id);
		return;
	}
	n = dc_hosts(dev, hosts, FMD_DC_MAX_HOST_ID + 1);
	for (int i = 0; i < n; i++)
		dc_host_dirty(dev, hosts[i]);
}

/*
 * The read after a command the device answered Success for. The command
 * was carried out, whatever the read says: 5604h "shall complete when the
 * device initiates the Add Capacity procedure" (CXL r4.0 Section
 * 7.6.7.6.5), 5605h "when the device initiates the Remove Capacity
 * procedure" (Section 7.6.7.6.6). A read that fails, a list that keeps
 * changing as it is read (-EAGAIN) among them, is not a failure of the
 * command, nor a reason to send it again, which a client told it failed,
 * or to retry, would do: it is FMD_DC_ACCEPTED_STALE, and the hosts it did
 * not read are read again by fmd_dcd_retry_stale(). They are marked before
 * the read, so that one that does not get as far as recording itself is
 * read again too.
 */
static int dc_refresh_after(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
			    const char *what)
{
	int rc;

	dc_hosts_dirty(dev, host_id);
	rc = dcd_refresh_locked(ctx, dev, host_id);
	/*
	 * A tag list that could not be read is stale too: an add a host has
	 * not answered shows only there (PendingLDs).
	 */
	if (!rc && fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags))
		rc = -EIO;
	if (!rc)
		return 0;
	fmd_warn(ctx, "%s: %s was carried out, but the read after it failed (%d): "
		 "its state is stale until it is read again", dev->id, what, rc);
	return FMD_DC_ACCEPTED_STALE;
}

int fmd_dcd_refresh(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id)
{
	int rc;

	pthread_mutex_lock(&ctx->dcd_lock);
	rc = dcd_refresh_locked(ctx, dev, host_id);
	pthread_mutex_unlock(&ctx->dcd_lock);
	return rc;
}

/* Whether a memory device's Get DCD Info is owed, and its retry state */
static bool *dcd_pending_of(struct fmd_component *dev, struct fmd_retry **retry)
{
	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		*retry = &fmd_to_sld(dev)->dcd_retry;
		return &fmd_to_sld(dev)->dcd_pending;
	case FMD_COMPONENT_MLD:
		*retry = &fmd_to_mld(dev)->dcd_retry;
		return &fmd_to_mld(dev)->dcd_pending;
	case FMD_COMPONENT_MHD:
		*retry = &fmd_to_mhd(dev)->dcd_retry;
		return &fmd_to_mhd(dev)->dcd_pending;
	default:
		*retry = NULL;
		return NULL;
	}
}

int fmd_dcd_discover(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct cxlmi_cmd_fmapi_get_dcd_info_rsp info = {0};
	struct cxlmi_tunnel_info tbuf, *ti;
	struct fmd_dcd_info *dcd;
	int rc;

	if (unlikely(!dev_is_hw(dev)) || !fmd_is_memdev(dev))
		return 0;

	ti = dcd_tunnel(dev, &tbuf);

	/*
	 * Get DCD Info (5600h). Its Possible Command Return Codes are
	 * Success, Unsupported, Internal Error and Retry Required (Section
	 * 7.6.7.6.1): only Unsupported says the device has no Dynamic
	 * Capacity. One that fails otherwise is asked again, with backoff
	 * and up to a cap (fmd_dcd_retry_pending(), cxl-fmd's policy).
	 */
	errno = 0;	/* see fmd_probe_result() */
	rc = cxlmi_cmd_fmapi_get_dcd_info(dev->ep, ti, &info);
	if (rc) {
		struct fmd_retry *retry;
		bool *pending = dcd_pending_of(dev, &retry);

		if (fmd_probe_result(rc) != FMD_PROBE_TRANSIENT || !pending) {
			fmd_dbg(ctx, "%s: no dynamic capacity support (%d)", dev->id, rc);
			if (pending)
				*pending = false;
			return 0;
		}
		*pending = !fmd_retry_failed(retry);
		if (*pending)
			fmd_warn(ctx, "%s: get dcd info failed for now (%d): "
				 "asked again", dev->id, rc);
		else
			fmd_err(ctx, "%s: get dcd info failed in %d tries (%d): "
				"taken as no DCD until cxl-fmd is restarted",
				dev->id, FMD_RETRY_MAX, rc);
		return -EAGAIN;
	}
	{
		struct fmd_retry *retry;
		bool *pending = dcd_pending_of(dev, &retry);

		if (pending) {
			*pending = false;
			fmd_retry_reset(retry);
		}
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
	/* libcxlmi returns Total Dynamic Capacity in bytes, not in 256 MB units */
	dcd->total_dc_capacity = info.total_dynamic_capacity;
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

	/*
	 * An Event Notification when the DC event log goes non-empty is asked
	 * for with the device's other logs: see fmd_device_events_enable().
	 */
	return fmd_dcd_refresh(ctx, dev, -1);
}

/*
 * Number of Available Extents (78h) and Number of Available Tags (7Ch) of
 * a Dynamic Capacity Event Record, valid "When set" is Bit[0] of its
 * Validity Flags (31h, Table 8-229), into the counters of the host the
 * record names. @d is the record from 30h on.
 */
static void dc_event_counts(struct fmd_component *dev, int host_id, const uint8_t *d)
{
	struct fmd_component *owner = fmd_dc_host_component(dev, host_id);
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	uint32_t ext, tags;

	if (!owner || !(d[0x31 - 0x30] & 0x01))
		return;
	ext = (uint32_t)d[0x48] | (uint32_t)d[0x49] << 8 |
	      (uint32_t)d[0x4a] << 16 | (uint32_t)d[0x4b] << 24;
	tags = (uint32_t)d[0x4c] | (uint32_t)d[0x4d] << 8 |
	       (uint32_t)d[0x4e] << 16 | (uint32_t)d[0x4f] << 24;
	if (owner->type == FMD_COMPONENT_LD) {
		fmd_to_ld(owner)->dc_extents_available = ext;
		fmd_to_ld(owner)->dc_tags_available = tags;
	} else {
		dcd->extents_available = ext;
		dcd->tags_available = tags;
	}
}

/*
 * Read and clear the FM-side DC event log of one device, then refresh every
 * host that an Add Capacity Response or Capacity Released record names,
 * and the tag list once. The Host ID is "the LD-ID of the host interface
 * that triggered the generation of the event" (Table 8-229), up to LD 255
 * as the 5608h bitmaps have them. The extent a record carries (38h) is not
 * applied: the host's 5603h, read again, says what the host has.
 */
static int drain_dc_events(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct cxlmi_cmd_get_event_records_req req = { .event_log = DC_EVENT_LOG };
	struct cxlmi_cmd_get_event_records_rsp *rsp;
	struct cxlmi_cmd_clear_event_records_req *clr;
	struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
	struct cxlmi_endpoint *ep = dev->ep;
	uint8_t hosts[FMD_DC_LD_BITMAP_BYTES] = { 0 };	/* bit h: host h */
	int list[FMD_DC_MAX_HOST_ID + 1], n = 0;
	bool overflow = false, named = false;
	uint16_t overflow_count = 0;
	uint64_t first_ns = 0, last_ns = 0;
	int rc = 0, err = 0;

	rsp = calloc(1, sizeof(*rsp) +
		     CXLMI_MAX_SUPPORTED_EVENT_RECORDS * sizeof(rsp->records[0]));
	clr = calloc(1, sizeof(*clr) +
		     CXLMI_MAX_SUPPORTED_EVENT_RECORDS * sizeof(clr->handles[0]));
	if (!rsp || !clr) {
		free(rsp);
		free(clr);
		return -ENOMEM;
	}

	pthread_mutex_lock(&ctx->dcd_lock);

	for (int page = 0; page < DC_EVENT_MAX_PAGES; page++) {
		rc = cxlmi_cmd_get_event_records(ep, ti, &req, rsp);
		if (rc && !page)
			goto out;
		/*
		 * The pages before were cleared: what they named is refreshed
		 * below, as nothing else will name it again.
		 */
		if (rc) {
			err = rc;
			rc = 0;
			break;
		}
		if (rsp->flags & EVENT_FLAG_OVERFLOW) {
			int all[FMD_DC_MAX_HOST_ID + 1];
			int nall = dc_hosts(dev, all, FMD_DC_MAX_HOST_ID + 1);

			/* records were lost: any host may have changed */
			for (int h = 0; h < nall; h++)
				dc_host_dirty(dev, all[h]);
			overflow = true;
			overflow_count = rsp->overflow_err_count;
			first_ns = rsp->first_overflow_timestamp;
			last_ns = rsp->last_overflow_timestamp;
		}
		if (!rsp->record_count)
			break;
		/* clr has room for no more handles than this */
		FMD_CCI_LIST_CLAMP(rsp->record_count,
				   CXLMI_MAX_SUPPORTED_EVENT_RECORDS);

		for (int i = 0; i < rsp->record_count; i++) {
			struct cxlmi_event_record *rec = &rsp->records[i];
			uint8_t type = rec->data[0];
			int host = rec->data[2] | (rec->data[3] << 8);

			clr->handles[i] = rec->handle;
			/* the record is gone once cleared: kept for Redfish */
			fmd_dc_record_keep(dev, rec);
			if (memcmp(rec->uuid, dc_event_uuid, sizeof(dc_event_uuid)))
				continue;
			if (type != DC_EVENT_ADD_RESPONSE && type != DC_EVENT_RELEASED)
				continue;
			if (host > FMD_DC_MAX_HOST_ID) {
				fmd_warn(ctx, "%s: DC event record names host %d, past LD 255",
					 dev->id, host);
				continue;
			}
			hosts[host / 8] |= 1 << (host % 8);
			named = true;
			/* before the clear: the record will not name it again */
			dc_host_dirty(dev, host);
			dc_event_counts(dev, host, rec->data);
		}

		clr->event_log = DC_EVENT_LOG;
		clr->nr_recs = rsp->record_count;
		rc = cxlmi_cmd_clear_event_records(ep, ti, clr);
		if (rc) {
			err = rc;
			rc = 0;
			break;
		}
		/* read again until empty: see fmd_event_log_drain() */
	}

	if (overflow) {
		fmd_dc_log_overflowed(dev, overflow_count, first_ns, last_ns);
		fmd_warn(ctx, "%s: DC event log overflowed, refreshing all hosts", dev->id);
		rc = dcd_refresh_locked(ctx, dev, -1);
		goto out;
	}

	for (int h = 0; h <= FMD_DC_MAX_HOST_ID; h++) {
		if (!fmd_dc_bm_test(hosts, h))
			continue;
		if (!fmd_dc_host_component(dev, h)) {
			fmd_warn(ctx, "%s: DC event record names host %d, which it does not have",
				 dev->id, h);
			continue;
		}
		list[n++] = h;
	}
	if (named)
		rc = dcd_refresh_hosts_locked(ctx, dev, list, n);
	if (!rc)
		rc = err;
out:
	pthread_mutex_unlock(&ctx->dcd_lock);
	free(rsp);
	free(clr);
	return rc;
}

int fmd_dcd_poll_events(struct fmd_ctx *ctx)
{
	return fmd_dcd_poll_events_ep(ctx, NULL) < 0 ? -1 : 0;
}

int fmd_dcd_poll_events_ep(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	static const enum fmd_component_type types[] = {
		FMD_COMPONENT_SLD, FMD_COMPONENT_MLD, FMD_COMPONENT_MHD,
	};
	struct fmd_component *devs[DC_EVENT_MAX_DEVS], *comp;
	int n = 0, rc = 0;

	/*
	 * A drain sends commands and replaces a device's regions and extents:
	 * it has the fabric to itself, as a request that changes it does.
	 */
	pthread_rwlock_wrlock(&ctx->fabric_lock);

	/* Collect under the read lock; each drain then runs under dcd_lock */
	fmd_topology_rdlock(&ctx->topology);
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		fmd_for_each_type(&ctx->topology, comp, types[t]) {
			/* a log that is the host's is not drained */
			if (likely(dev_is_hw(comp)) && fmd_memdev_dcd(comp) &&
			    fmd_endpoint_owns_events(ctx, comp->ep) &&
			    fmd_component_on_ep(comp, ep) &&
			    n < DC_EVENT_MAX_DEVS)
				devs[n++] = comp;
		}
	}
	fmd_topology_rdunlock(&ctx->topology);

	for (int i = 0; i < n; i++) {
		int r = drain_dc_events(ctx, devs[i]);

		if (r && !rc)
			rc = r;
	}
	pthread_rwlock_unlock(&ctx->fabric_lock);
	return rc ? -1 : n;
}

/*
 * Nothing else reads a host again once the record that named it has been
 * cleared (CXL r4.0 Section 8.2.10.2.3: "Clear Event Records provides a
 * mechanism for the host to clear events that it has consumed from the
 * device's Event Log"), nor once a command to it was accepted and the read
 * after it failed: a GET serves what was read last. The hosts whose read
 * failed (STALE), or is owed (DIRTY), are read here, with the tag list,
 * and so is a tag list that could not be read. The event thread calls this
 * on every --event-poll tick, one attempt each; with --event-poll 0 there
 * is no retry, and a host stays stale until something else reads it. The
 * devices whose DC event log is the host's are read too: their staleness
 * comes from cxl-fmd's own reads. All of it is cxl-fmd's choice; the
 * specification has no retry policy for an FM. A host never read (NEVER)
 * is not retried.
 */
int fmd_dcd_retry_stale(struct fmd_ctx *ctx)
{
	static const enum fmd_component_type types[] = {
		FMD_COMPONENT_SLD, FMD_COMPONENT_MLD, FMD_COMPONENT_MHD,
	};
	struct fmd_component *devs[DC_EVENT_MAX_DEVS], *comp;
	int n = 0, read = 0, rc = 0;

	pthread_rwlock_wrlock(&ctx->fabric_lock);

	fmd_topology_rdlock(&ctx->topology);
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		fmd_for_each_type(&ctx->topology, comp, types[t]) {
			if (likely(dev_is_hw(comp)) && fmd_memdev_dcd(comp) &&
			    n < DC_EVENT_MAX_DEVS)
				devs[n++] = comp;
		}
	}
	fmd_topology_rdunlock(&ctx->topology);

	for (int i = 0; i < n; i++) {
		struct fmd_component *dev = devs[i];
		int all[FMD_DC_MAX_HOST_ID + 1], list[FMD_DC_MAX_HOST_ID + 1];
		int nall, m = 0, r = 0;
		bool tried = true;

		pthread_mutex_lock(&ctx->dcd_lock);
		nall = dc_hosts(dev, all, FMD_DC_MAX_HOST_ID + 1);
		for (int h = 0; h < nall; h++) {
			if (fmd_dc_sync_behind(fmd_dc_host_sync(dev, all[h])))
				list[m++] = all[h];
		}
		if (m) {
			fmd_dbg(ctx, "%s: reading %d DC host(s) again", dev->id, m);
			r = dcd_refresh_hosts_locked(ctx, dev, list, m);
		} else if (fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags)) {
			struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);

			fmd_dbg(ctx, "%s: reading the DC tag list again", dev->id);
			r = hw_refresh_tags(ctx, dev, dev->ep, ti);
		} else {
			tried = false;
		}
		if (tried) {
			read++;
			/* a tag list read that fails returns 0, and stays failed */
			if (!r && fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags))
				r = -EIO;
		}
		pthread_mutex_unlock(&ctx->dcd_lock);
		if (r && !rc)
			rc = r;
	}

	pthread_rwlock_unlock(&ctx->fabric_lock);
	return rc ? -1 : read;
}

/*
 * ============================================================================
 * Operations
 * ============================================================================
 */

int fmd_dc_set_region_config(struct fmd_ctx *ctx, struct fmd_component *dev,
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

	/*
	 * The device rejects reconfiguration while capacity is allocated:
	 * 5602h "shall fail with Unsupported" "When all capacity has been
	 * released from the DC Region on all hosts, and one or more blocks
	 * are allocated to the specified region" (Section 7.6.7.6.3). An
	 * extent any host holds or is offered is allocated, and so is one an
	 * FM reference keeps: that capacity "will remain allocated ... even
	 * if all DCD Extents that reference it are removed" (Section
	 * 7.6.7.6.7). The mock knows where that is; on hardware an FM
	 * reference no host holds names no region, and the device decides.
	 */
	{
		struct fmd_component *child;
		struct fmd_dc_mock_hold *hold;

		fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
			if (fmd_to_dc_extent(child)->region_id == region->region_id)
				return -EBUSY;
		}
		for (hold = dcd->mock_orphans; hold; hold = hold->next) {
			if (hold->region_id == region->region_id)
				return -EBUSY;
		}
	}

	if (likely(dev_is_hw(dev))) {
		struct cxlmi_cmd_fmapi_set_dc_region_config_req req = {0};
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int rc;

		req.region_id = region->region_id;
		req.block_sz = block_size;
		req.sanitize_on_release = sanitize_on_release ? 1 : 0;

		rc = cxlmi_cmd_fmapi_set_dc_region_config(dev->ep, ti, &req);
		if (rc) {
			dc_cmd_failed(ctx, dev, "Set DC Region Configuration", rc);
			if (rc < 0)
				dc_hosts_dirty(dev, -1);
			return dc_cmd_errno(rc, true);
		}
		return dc_refresh_after(ctx, dev, -1, "Set DC Region Configuration");
	}

	fmd_info(ctx, "%s: mock set DC region %u block size %" PRIu64 " MB sanitize=%d",
		 dev->id, region->region_id, (uint64_t)(block_size / FMD_MIB),
		 sanitize_on_release);
	apply_region_config(dev, region->region_id, block_size, sanitize_on_release);
	return 0;
}

int fmd_dc_add(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
	       uint8_t policy, uint8_t region_id, uint64_t length,
	       const uint8_t tag[FMD_DC_TAG_LEN],
	       struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	uint16_t cap_bit = fmd_dc_policy_cap_bit(false, policy);

	if (!dcd || host_id < 0)
		return -ENOENT;
	/*
	 * The policies a device supports are Bits[2:0] of Supported Add
	 * Capacity Selection Policies: Free, Contiguous and Prescriptive.
	 * Bit[3] "Must be 0" (CXL r4.0 Table 7-65), so a device has no way to
	 * say it supports Enable Shared Access, and one that follows the
	 * table would have every such request refused here. It is the device
	 * that refuses it, with Invalid Input, as when the region is not
	 * Sharable (Section 7.6.7.6.5); the mock does as a device does.
	 */
	if (!cap_bit)
		return -EINVAL;
	if (!(dcd->add_policies & cap_bit) && policy != FMD_DC_POLICY_SHARED_ACCESS)
		return -EINVAL;
	if (!fmd_dc_host_component(dev, host_id))
		return -ENOENT;
	/*
	 * Enable Shared Access adds the capacity "designated by the
	 * specified tag value" (Section 7.6.7.6.5); in a Sharable region
	 * the Tag "is required" (Table 8-230). cxl-fmd takes an all-zero
	 * Tag for none.
	 */
	if (policy == FMD_DC_POLICY_SHARED_ACCESS && dc_tag_is_zero(tag))
		return -EINVAL;

	if (unlikely(!dev_is_hw(dev))) {
		int rc = mock_dc_add(ctx, dev, host_id, policy, region_id, length,
				     tag, extents, ext_count);

		fmd_dc_mock_recount(dev);
		fmd_dc_mock_retag(dev);
		return rc;
	}

	{
		struct cxlmi_cmd_fmapi_initiate_dc_add_req *req;
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int n = (policy == FMD_DC_POLICY_PRESCRIPTIVE) ? ext_count : 0;
		int rc;

		if (policy == FMD_DC_POLICY_PRESCRIPTIVE && (n <= 0 || !extents))
			return -EINVAL;

		/*
		 * Length is "Always a multiple of the configured Region Block
		 * Size" (Table 7-72), which the device would refuse with
		 * Invalid Input: not sent when the region is known.
		 */
		if (policy == FMD_DC_POLICY_FREE || policy == FMD_DC_POLICY_CONTIGUOUS) {
			struct fmd_dc_region *region = fmd_dc_find_region(dev, host_id, region_id);

			if (length == 0 || (region && region->block_size &&
					    length % region->block_size))
				return -EINVAL;
		}

		req = calloc(1, sizeof(*req) + n * sizeof(req->extents[0]));
		if (!req)
			return -ENOMEM;

		/*
		 * Table 7-72: Region Number and Tag are "reserved when the
		 * Selection Policy is set to Prescriptive", Length "when the
		 * Selection Policy is set to Prescriptive or Enable Shared
		 * Access". Reserved fields are sent as zero.
		 */
		req->host_id = host_id;
		req->selection_policy = policy;
		if (policy != FMD_DC_POLICY_PRESCRIPTIVE) {
			req->region_num = region_id;
			if (tag)
				memcpy(req->tag, tag, FMD_DC_TAG_LEN);
		}
		if (policy == FMD_DC_POLICY_FREE || policy == FMD_DC_POLICY_CONTIGUOUS)
			req->length = length;
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

		rc = cxlmi_cmd_fmapi_initiate_dc_add(dev->ep, ti, req);
		free(req);
		if (rc) {
			dc_cmd_failed(ctx, dev, "Initiate Dynamic Capacity Add", rc);
			if (rc < 0)
				dc_hosts_dirty(dev, host_id);
			return dc_cmd_errno(rc, false);
		}

		return dc_refresh_after(ctx, dev, host_id, "Initiate Dynamic Capacity Add");
	}
}

int fmd_dc_release(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		   uint8_t policy, bool forced, bool sanitize, uint64_t length,
		   const uint8_t tag[FMD_DC_TAG_LEN],
		   struct fmd_dc_extent **extents, int ext_count)
{
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	uint16_t cap_bit = fmd_dc_policy_cap_bit(true, policy);

	if (!dcd || host_id < 0)
		return -ENOENT;
	if (!cap_bit || !(dcd->release_policies & cap_bit))
		return -EINVAL;
	/* Length: "Shall be > 0" unless the policy is Prescriptive (Table 7-73) */
	if (policy == FMD_DC_REL_POLICY_TAG_BASED && length == 0)
		return -EINVAL;
	if (!fmd_dc_host_component(dev, host_id))
		return -ENOENT;

	/*
	 * Releasing an extent can free it, so one that is listed twice
	 * would be used after that, and freed again.
	 */
	if (policy == FMD_DC_REL_POLICY_PRESCRIPTIVE && extents) {
		for (int i = 1; i < ext_count; i++) {
			for (int j = 0; j < i; j++) {
				if (extents[i] && extents[i] == extents[j])
					return -EINVAL;
			}
		}
		/*
		 * The Extent List is in the DPA space of the host released
		 * from: an extent of another host is not the host's, even at
		 * the same DPA (Section 9.13.3).
		 */
		for (int i = 0; i < ext_count; i++) {
			if (extents[i] && extents[i]->host_id != host_id)
				return -ENOENT;
		}
	}

	if (unlikely(!dev_is_hw(dev))) {
		int rc = mock_dc_release(ctx, dev, host_id, policy, forced, length,
					 tag, extents, ext_count);

		fmd_dc_mock_recount(dev);
		fmd_dc_mock_retag(dev);
		return rc;
	}

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

		/*
		 * Table 7-73: Length and Tag are reserved "when the Removal
		 * Policy is set to Prescriptive", and sent as zero.
		 */
		req->host_id = host_id;
		req->flags = (policy & FMD_DC_REL_POLICY_MASK) |
			     (forced ? FMD_DC_REL_FORCED_REMOVAL : 0) |
			     (sanitize ? FMD_DC_REL_SANITIZE : 0);
		if (policy != FMD_DC_REL_POLICY_PRESCRIPTIVE) {
			req->length = length;
			if (tag)
				memcpy(req->tag, tag, FMD_DC_TAG_LEN);
		}
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

		rc = cxlmi_cmd_fmapi_initiate_dc_release(dev->ep, ti, req);
		free(req);
		if (rc) {
			dc_cmd_failed(ctx, dev, "Initiate Dynamic Capacity Release", rc);
			if (rc < 0)
				dc_hosts_dirty(dev, host_id);
			return dc_cmd_errno(rc, false);
		}

		return dc_refresh_after(ctx, dev, host_id, "Initiate Dynamic Capacity Release");
	}
}

int fmd_dc_reference(struct fmd_ctx *ctx, struct fmd_component *dev,
		     const uint8_t tag[FMD_DC_TAG_LEN], bool add)
{
	if (!fmd_memdev_dcd(dev))
		return -ENOENT;
	/* cxl-fmd takes an all-zero Tag for none: it names no sharable tag */
	if (dc_tag_is_zero(tag))
		return -EINVAL;

	if (unlikely(!dev_is_hw(dev))) {
		int rc = mock_dc_reference(ctx, dev, tag, add);

		fmd_dc_mock_recount(dev);
		fmd_dc_mock_retag(dev);
		return rc;
	}

	{
		struct cxlmi_tunnel_info tbuf, *ti = dcd_tunnel(dev, &tbuf);
		int rc;

		if (add) {
			struct cxlmi_cmd_fmapi_dc_add_ref_req req;
			memcpy(req.tag, tag, FMD_DC_TAG_LEN);
			rc = cxlmi_cmd_fmapi_dc_add_reference(dev->ep, ti, &req);
		} else {
			struct cxlmi_cmd_fmapi_dc_remove_ref_req req;
			memcpy(req.tag, tag, FMD_DC_TAG_LEN);
			rc = cxlmi_cmd_fmapi_dc_remove_reference(dev->ep, ti, &req);
		}

		if (rc) {
			dc_cmd_failed(ctx, dev, add ? "Dynamic Capacity Add Reference" :
				      "Dynamic Capacity Remove Reference", rc);
			/* the tag list is to be read again */
			if (rc < 0 && !fmd_memdev_dcd(dev)->tags.unsupported)
				fmd_memdev_dcd(dev)->tags.known = false;
			return dc_cmd_errno(rc, false);
		}

		/* carried out: a tag list that cannot be read is stale, as above */
		rc = hw_refresh_tags(ctx, dev, dev->ep, ti);
		if (rc || fmd_dc_tags_failed(&fmd_memdev_dcd(dev)->tags)) {
			fmd_warn(ctx, "%s: %s was carried out, but the tag list could not "
				 "be read after it", dev->id,
				 add ? "Dynamic Capacity Add Reference" :
				 "Dynamic Capacity Remove Reference");
			return FMD_DC_ACCEPTED_STALE;
		}
		return 0;
	}
}

void fmd_dcd_retry_pending(struct fmd_ctx *ctx)
{
	static const enum fmd_component_type types[] = {
		FMD_COMPONENT_SLD, FMD_COMPONENT_MLD, FMD_COMPONENT_MHD,
	};
	char (*ids)[FMD_MAX_ID_LEN];
	struct fmd_component *comp;
	int n = 0, max = ctx->topology.root->device_count;

	/* by ID: discovering a DCD adds to the topology */
	ids = calloc(max ? max : 1, sizeof(*ids));
	if (!ids)
		return;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		fmd_for_each_type(&ctx->topology, comp, types[t]) {
			struct fmd_retry *retry;
			bool *pending = dcd_pending_of(comp, &retry);

			if (n < max && *pending && fmd_retry_due(retry))
				snprintf(ids[n++], FMD_MAX_ID_LEN, "%s", comp->id);
		}
	}
	for (int i = 0; i < n; i++) {
		comp = fmd_topology_find(&ctx->topology, ids[i]);
		if (!comp || fmd_dcd_discover(ctx, comp) == -EAGAIN)
			continue;
		/* the DC log's notification, which was not asked for */
		if (fmd_memdev_dcd(comp))
			fmd_device_events_enable(ctx, comp);
	}
	free(ids);
}
