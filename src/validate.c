// SPDX-License-Identifier: BSD-3-Clause
/*
 * Topology validation
 *
 * Checks a topology against what the CXL specification lets a fabric be.
 * A mock topology is whatever its JSON says, and one that could not exist
 * makes every test that runs on it a test of nothing: it is refused. A
 * discovered topology is what the hardware reported, so the same checks
 * there are warnings about the hardware or about discovery.
 *
 * Checks that come from CXL r4.0 cite their section or table. The others
 * are consistency checks of cxl-fmd's own model of a topology, or limits
 * of cxl-fmd, and are labelled "cxl-fmd consistency" or "cxl-fmd limit".
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cxl-fmd.h"

struct validation {
	struct fmd_ctx *ctx;
	int errors;
};

static void invalid(struct validation *v, const char *rule, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void invalid(struct validation *v, const char *rule, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fmd_err(v->ctx, "invalid topology: %s [%s]", msg, rule);
	v->errors++;
}

/*
 * What the specification does not forbid but a device would hardly report:
 * logged, and the topology is still valid.
 */
static void doubtful(struct validation *v, const char *rule, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void doubtful(struct validation *v, const char *rule, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fmd_warn(v->ctx, "doubtful topology: %s [%s]", msg, rule);
}

#define MB256 CXL_CAPACITY_UNIT
#define VCS_ENABLED 0x01	/* VCS State 01h, Table 7-34 */

static bool is_pow2(uint64_t x)
{
	return x && !(x & (x - 1));
}

static struct fmd_port *switch_port(struct fmd_switch *sw, int port_number)
{
	struct fmd_component *child;

	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		if (fmd_to_port(child)->port_number == port_number)
			return fmd_to_port(child);
	}
	return NULL;
}

/* the memory device on a port, and how many there are */
static struct fmd_component *port_device(struct fmd_port *port, int *count)
{
	struct fmd_component *child, *dev = NULL;

	*count = 0;
	fmd_for_each_child(&port->base, child) {
		if (fmd_is_memdev(child)) {
			if (!dev)
				dev = child;
			(*count)++;
		}
	}
	return dev;
}

static struct fmd_ld *device_ld(struct fmd_component *dev, int ld_id)
{
	struct fmd_component *child;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->ld_id == ld_id)
			return fmd_to_ld(child);
	}
	return NULL;
}

/*
 * Identify Memory Device, Table 8-309: capacities are in multiples of
 * 256 MB, and "Total Capacity shall be greater than or equal to the sum of
 * Volatile Only Capacity and Persistent Only Capacity".
 */
static void check_capacity(struct validation *v, const char *id, uint64_t total,
			   uint64_t vol, uint64_t pers)
{
	static const char rule[] = "Identify Memory Device, Table 8-309";

	if (total % MB256 || vol % MB256 || pers % MB256)
		invalid(v, rule, "%s: capacity is not a multiple of 256 MB "
			"(total %" PRIu64 ", volatile %" PRIu64 ", persistent %" PRIu64 ")",
			id, total, vol, pers);
	if (vol + pers > total)
		invalid(v, rule, "%s: volatile %" PRIu64 " + persistent %" PRIu64
			" is more than the total capacity %" PRIu64, id, vol, pers, total);
}

/*
 * Dynamic Capacity regions and extents: the region of Table 8-347 (to which
 * Table 7-68 refers) and the extent of Table 8-230 (to which Table 7-71
 * refers).
 */
/* An all-zero Tag, which cxl-fmd takes for no tag */
static bool tag_is_zero(const uint8_t *tag)
{
	static const uint8_t zero[FMD_DC_TAG_LEN];

	return memcmp(tag, zero, FMD_DC_TAG_LEN) == 0;
}

/* Flags of the region an extent is in, as its own host sees it; 0 for none */
static uint8_t extent_flags(struct fmd_component *dev, struct fmd_dc_extent *ext)
{
	struct fmd_dc_region *r = ext->region_id == FMD_DC_REGION_NONE ? NULL :
				  fmd_dc_find_region(dev, ext->host_id, ext->region_id);

	return r ? r->flags : 0;
}

/*
 * The Tag and Shared Extent Sequence of each extent (Table 8-230). The
 * Extent List is one host's: "n is the number of extents" is taken to be
 * the host's extents of the tag, which is cxl-fmd's reading.
 */
static void check_dc_sharing(struct validation *v, struct fmd_component *dev)
{
	static const char rule[] = "Dynamic Capacity Extent, Table 8-230";
	struct fmd_component *child, *other;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		uint8_t flags = extent_flags(dev, ext);
		unsigned int n = 0;
		bool dup = false;

		if (ext->region_id == FMD_DC_REGION_NONE ||
		    !fmd_dc_find_region(dev, ext->host_id, ext->region_id))
			continue;	/* refused above */

		/* "For extents that describe non-shareable regions, this field shall be 0." */
		if (!(flags & FMD_DC_REGION_SHAREABLE)) {
			if (ext->shared_seq)
				invalid(v, rule, "%s: extent at %#" PRIx64 " of host %d has "
					"Shared Extent Sequence %u in region %d, which is not "
					"Sharable", dev->id, ext->start_dpa, ext->host_id,
					ext->shared_seq, ext->region_id);
			continue;
		}

		/*
		 * "Within sharable regions, all extents targeting the same
		 * range this field is required": a sharable extent without one
		 * is not shared, which the specification does not forbid.
		 */
		if (tag_is_zero(ext->tag)) {
			doubtful(v, rule, "%s: extent at %#" PRIx64 " of host %d in "
				 "Sharable region %d has no tag", dev->id,
				 ext->start_dpa, ext->host_id, ext->region_id);
			continue;
		}

		fmd_for_each_child_type(dev, other, FMD_COMPONENT_DC_EXTENT) {
			struct fmd_dc_extent *o = fmd_to_dc_extent(other);

			if (memcmp(o->tag, ext->tag, FMD_DC_TAG_LEN) ||
			    !(extent_flags(dev, o) & FMD_DC_REGION_SHAREABLE))
				continue;
			if (o->host_id == ext->host_id) {
				n++;
				if (o != ext && o->shared_seq == ext->shared_seq)
					dup = true;
			} else if (o->host_id > ext->host_id &&
				   o->shared_seq == ext->shared_seq && o->len != ext->len) {
				/*
				 * "The Shared Extent Sequence is used so that
				 * the same offsets reach the same data accessed
				 * by all hosts": that the same sequence is the
				 * same length on each host is cxl-fmd's
				 * inference from it.
				 */
				doubtful(v, rule, "%s: the extents of host %d at %#" PRIx64
					 " and of host %d at %#" PRIx64 " have the same Tag "
					 "and Shared Extent Sequence %u, and lengths %#"
					 PRIx64 " and %#" PRIx64, dev->id, ext->host_id,
					 ext->start_dpa, o->host_id, o->start_dpa,
					 ext->shared_seq, ext->len, o->len);
			}
		}

		/*
		 * "For extents describing shareable regions this field shall
		 * be within the range of 0 to n-1 where n is the number of
		 * extents, with each value appearing only once."
		 */
		if (ext->shared_seq >= n || dup)
			invalid(v, rule, "%s: extent at %#" PRIx64 " of host %d has "
				"Shared Extent Sequence %u, which is not one of 0 to %u "
				"once each for its %u extents of the tag", dev->id,
				ext->start_dpa, ext->host_id, ext->shared_seq, n - 1, n);
	}
}

/*
 * The device's Dynamic Capacity List Tags (5608h). "When the tagged
 * capacity has been released from all hosts, if the FM does not hold a
 * reference, ... the tag no longer exists" (Section 9.13.3.2, in an
 * example sequence): an entry with no LD in either bitmap and no FM
 * reference is a tag that should be gone. The bitmaps reach LD 255 only
 * ("bit[7] of the last byte represents LD 255", Table 7-78).
 */
static void check_dc_tags(struct validation *v, struct fmd_component *dev,
			  struct fmd_dcd_info *dcd)
{
	static const char rule[] = "Dynamic Capacity List Tags, Table 7-78";
	struct fmd_component *child;

	if (!dcd->tags.known)
		return;

	for (uint32_t i = 0; i < dcd->tags.n; i++) {
		const struct fmd_dc_tag *t = &dcd->tags.v[i];
		bool any = t->fm_ref;
		char str[40];

		for (int b = 0; b < FMD_DC_LD_BITMAP_BYTES && !any; b++)
			any = ((dcd->tags.validity & 0x01) && t->ref[b]) ||
			      ((dcd->tags.validity & 0x02) && t->pend[b]);
		if (any || (dcd->tags.validity & 0x03) != 0x03)
			continue;
		fmd_dc_tag_to_str(t->tag, str, sizeof(str));
		doubtful(v, rule, "%s: tag %s has no LD and no FM reference",
			 dev->id, str);
	}

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		if (ext->host_id > FMD_DC_MAX_HOST_ID)
			doubtful(v, rule, "%s: host %d of the extent at %#" PRIx64
				 " is past LD 255, which the tag bitmaps cannot name",
				 dev->id, ext->host_id, ext->start_dpa);
	}
}

static void check_dcd(struct validation *v, struct fmd_component *dev)
{
	static const char rule[] = "DC Region Configuration, Table 8-347";
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child, *other;

	if (!dcd)
		return;

	for (int h = 0; h < dcd->num_hosts; h++) {
		for (int r = 0; r < FMD_MAX_DC_REGIONS; r++) {
			struct fmd_dc_region *reg = fmd_dc_find_region(dev, h, r);

			if (!reg)
				continue;
			if (reg->base_dpa % MB256)
				invalid(v, rule, "%s: host %d region %d base %#" PRIx64
					" is not aligned to 256 MB", dev->id, h, r,
					reg->base_dpa);
			if (!is_pow2(reg->block_size) || reg->block_size % 0x40) {
				invalid(v, rule, "%s: host %d region %d block size %" PRIu64
					" is not a power of 2 and a multiple of 40h",
					dev->id, h, r, reg->block_size);
				continue;
			}
			if (reg->region_len % reg->block_size)
				invalid(v, rule, "%s: host %d region %d length %" PRIu64
					" is not divisible by its block size %" PRIu64,
					dev->id, h, r, reg->region_len, reg->block_size);
			if (reg->region_len > reg->decode_len)
				invalid(v, rule, "%s: host %d region %d length %" PRIu64
					" is more than its decode length %" PRIu64,
					dev->id, h, r, reg->region_len, reg->decode_len);
		}
	}

	/*
	 * Each extent is one entry of the Extent List of its host (Table
	 * 7-71), in that host's DPA space: "The DPA ranges exposed by the
	 * device to each host are independent of one another" (Section
	 * 9.13.3). Its region, and every rule below, is that host's.
	 */
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		struct fmd_dc_region *reg;
		int h = ext->host_id;

		/* "The Length shall be > 0." */
		if (ext->len == 0)
			invalid(v, "Dynamic Capacity Extent, Table 8-230", "%s: extent at %#" PRIx64
				" of host %d has no length", dev->id, ext->start_dpa, h);

		reg = ext->region_id == FMD_DC_REGION_NONE ? NULL :
		      fmd_dc_find_region(dev, h, ext->region_id);
		if (!reg) {
			if (ext->region_id == FMD_DC_REGION_NONE)
				invalid(v, rule, "%s: extent at %#" PRIx64 " is in no "
					"region of host %d", dev->id, ext->start_dpa, h);
			else
				invalid(v, rule, "%s: extent at %#" PRIx64 " is in region %d, "
					"which host %d does not have", dev->id,
					ext->start_dpa, ext->region_id, h);
		} else {
			if (ext->start_dpa < reg->base_dpa ||
			    ext->start_dpa + ext->len > reg->base_dpa + reg->decode_len)
				invalid(v, rule, "%s: extent %#" PRIx64 "+%#" PRIx64
					" is outside region %d of host %d", dev->id,
					ext->start_dpa, ext->len, ext->region_id, h);
			if (is_pow2(reg->block_size) &&
			    (ext->start_dpa % reg->block_size || ext->len % reg->block_size))
				invalid(v, rule, "%s: extent %#" PRIx64 "+%#" PRIx64
					" is not aligned to the block size %" PRIu64
					" of region %d", dev->id, ext->start_dpa, ext->len,
					reg->block_size, ext->region_id);
		}

		/*
		 * Capacity that one host has can be given to another only
		 * through a Sharable region: Initiate Dynamic Capacity Add with
		 * Enable Shared Access "shall also fail with Invalid Input"
		 * "When the tagged capacity is already mapped to any Host ID
		 * via a non-Sharable region" (Section 7.6.7.6.5). Capacity is
		 * known across hosts by its tag alone.
		 */
		if (reg && !(reg->flags & FMD_DC_REGION_SHAREABLE) && !tag_is_zero(ext->tag)) {
			fmd_for_each_child_type(dev, other, FMD_COMPONENT_DC_EXTENT) {
				struct fmd_dc_extent *o = fmd_to_dc_extent(other);

				if (o->host_id == ext->host_id ||
				    memcmp(o->tag, ext->tag, FMD_DC_TAG_LEN))
					continue;
				invalid(v, "Initiate Dynamic Capacity Add, "
					"Section 7.6.7.6.5", "%s: the tag of the extent at %#"
					PRIx64 " of host %d is held by host %d too, and "
					"region %d is not Sharable", dev->id, ext->start_dpa,
					h, o->host_id, ext->region_id);
				break;
			}
		}

		/*
		 * One DPA range of a host is one extent. An Extent List with
		 * "Overlapping DPA ranges for multiple extents" is malformed
		 * (Sections 8.2.10.9.9.3 and 8.2.10.9.9.4). Two hosts' lists
		 * are two lists.
		 */
		for (other = child->next_sibling; other; other = other->next_sibling) {
			struct fmd_dc_extent *o;

			if (other->type != FMD_COMPONENT_DC_EXTENT)
				continue;
			o = fmd_to_dc_extent(other);
			if (o->host_id != ext->host_id)
				continue;
			if (ext->start_dpa < o->start_dpa + o->len &&
			    o->start_dpa < ext->start_dpa + ext->len)
				invalid(v, "malformed Extent List, Sections 8.2.10.9.9.3 "
					"and 8.2.10.9.9.4", "%s: extents at %#" PRIx64 " and %#" PRIx64
					" of host %d overlap", dev->id, ext->start_dpa,
					o->start_dpa, h);
		}
	}

	check_dc_sharing(v, dev);
	check_dc_tags(v, dev, dcd);
}

static void check_sld(struct validation *v, struct fmd_sld *sld)
{
	check_capacity(v, sld->base.id, sld->memory_size, sld->volatile_size,
		       sld->persistent_size);
	check_dcd(v, &sld->base);
}

/*
 * An MLD has up to 16 LDs for hosts (Section 2.4), and its LD allocations
 * are multiples of its Memory Granularity (Tables 7-48 and 7-49).
 */
static void check_mld(struct validation *v, struct fmd_mld *mld)
{
	static const char rule[] = "MLD, Section 2.4";
	struct fmd_component *child, *other;
	int lds = 0;

	if (mld->memory_granularity != 1 && mld->memory_granularity != 2 &&
	    mld->memory_granularity != 4)
		invalid(v, "Get LD Allocations, Table 7-48", "%s: memory granularity "
			"of %d x 256 MB is not 256 MB, 512 MB or 1 GB",
			mld->base.id, mld->memory_granularity);

	fmd_for_each_child_type(&mld->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		uint64_t unit = (uint64_t)mld->memory_granularity * MB256;

		lds++;
		if (ld->ld_id >= FMD_MAX_LD_COUNT)
			invalid(v, rule, "%s: LD-ID %d: an MLD has LD-IDs 0 to 15",
				mld->base.id, ld->ld_id);
		if (unit && ld->memory_size % unit)
			invalid(v, "Get LD Allocations, Table 7-49", "%s: LD %d size %"
				PRIu64 " is not a multiple of the memory granularity",
				mld->base.id, ld->ld_id, ld->memory_size);

		for (other = child->next_sibling; other; other = other->next_sibling) {
			if (other->type == FMD_COMPONENT_LD &&
			    fmd_to_ld(other)->ld_id == ld->ld_id)
				invalid(v, rule, "%s: two LDs with LD-ID %d",
					mld->base.id, ld->ld_id);
		}
	}

	if (lds < 1 || lds > FMD_MAX_LD_COUNT)
		invalid(v, rule, "%s: %d LDs: an MLD has 1 to 16", mld->base.id, lds);
	if (lds != mld->ld_count)
		invalid(v, rule, "%s: reports %d LDs and has %d", mld->base.id,
			mld->ld_count, lds);

	check_dcd(v, &mld->base);
}

/*
 * A Multi-Headed device has multiple heads. Each LD is mapped to at most one
 * head, and "A head in a Multi-Headed Device shall have at least one and no
 * more than 16 LDs mapped" (Section 2.5).
 *
 * The specification sets no limit on the number of heads or on the size of
 * the LD pool ("The LD Pool in an MH-MLD may support more than 16 LDs",
 * Section 2.5.1). cxl-fmd keeps at most FMD_MAX_HEADS heads and
 * FMD_MAX_LD_POOL pool LDs: those are its own limits. Of a device that has
 * more, discovery keeps only part, and what depends on the rest is not
 * checked.
 */
static void check_mhd(struct validation *v, struct fmd_mhd *mhd)
{
	static const char rule[] = "Multi-Headed Devices, Section 2.5";
	int per_head[FMD_MAX_HEADS] = { 0 };
	bool have_head[FMD_MAX_HEADS] = { false };
	/*
	 * The LDs mapped to the head are known: it has a presentation, or
	 * its Port Number, which the LD Map names it by, was read (a head
	 * with none mapped has no presentation, and is the device's doing)
	 */
	bool head_known[FMD_MAX_HEADS] = { false };
	bool all_heads = mhd->num_heads <= FMD_MAX_HEADS;
	bool all_lds = mhd->total_lds <= FMD_MAX_LD_POOL;
	struct fmd_component *child;
	int heads = 0;

	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
		struct fmd_head *head = fmd_to_head(child);

		heads++;
		if (head->head_id >= FMD_MAX_HEADS) {
			invalid(v, "cxl-fmd limit", "%s: head %d: cxl-fmd keeps "
				"%d heads at most", mhd->base.id, head->head_id,
				FMD_MAX_HEADS);
			continue;
		}
		if (have_head[head->head_id])
			invalid(v, rule, "%s: two heads with ID %d", mhd->base.id,
				head->head_id);
		have_head[head->head_id] = true;
		head_known[head->head_id] = head->presentation != FMD_HEAD_UNKNOWN ||
					    head->port_number_valid;

		/*
		 * No command maps a head's Port Number (Table 7-64) to a
		 * switch port or to a host, so a head that is on neither, as
		 * cxl-fmd knows it, can have no Connection (cxl-fmd limit).
		 */
		if (!head->port && !head->attached_host_id[0])
			doubtful(v, "cxl-fmd limit", "%s: head %d is on no switch "
				 "port and no host cxl-fmd knows: it has no "
				 "Connection", mhd->base.id, head->head_id);
	}

	if (heads < 2)
		invalid(v, rule, "%s: %d head(s): a Multi-Headed device has more "
			"than one", mhd->base.id, heads);
	if (all_heads && heads != mhd->num_heads)
		invalid(v, rule, "%s: reports %d heads and has %d", mhd->base.id,
			mhd->num_heads, heads);

	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);

		if (ld->binding_point_id == 0xFF)
			continue;	/* not assigned to a head */
		if (ld->binding_point_id >= FMD_MAX_HEADS ||
		    !have_head[ld->binding_point_id]) {
			invalid(v, rule, "%s: pool LD %d is on head %d, which the "
				"device does not have", mhd->base.id, ld->ld_id,
				ld->binding_point_id);
			continue;
		}
		per_head[ld->binding_point_id]++;
	}

	/*
	 * With part of the pool, a head may have LDs that were not kept; and
	 * with part of the LD Map read, or a head's Port Number not, what is
	 * mapped to the heads is not what cxl-fmd counted. Neither is the
	 * device's doing.
	 */
	if (!all_lds || !mhd->ld_map_complete)
		goto out;

	for (int h = 0; h < FMD_MAX_HEADS; h++) {
		if (have_head[h] && !head_known[h])
			continue;
		if (have_head[h] && (per_head[h] < 1 || per_head[h] > 16))
			invalid(v, rule, "%s: head %d has %d LDs mapped: a head has "
				"1 to 16", mhd->base.id, h, per_head[h]);
	}

	/*
	 * "A head with one LD mapped shall present itself as an SLD and a
	 * head with more than one LD mapped shall present itself as an MLD."
	 */
	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
		struct fmd_head *head = fmd_to_head(child);
		int lds;

		if (head->head_id >= FMD_MAX_HEADS || !head_known[head->head_id])
			continue;
		lds = per_head[head->head_id];
		if (lds == 1 && head->presentation != FMD_HEAD_SLD)
			invalid(v, rule, "%s: head %d has one LD mapped and does "
				"not present as an SLD", mhd->base.id, head->head_id);
		if (lds > 1 && head->presentation != FMD_HEAD_MLD)
			invalid(v, rule, "%s: head %d has %d LDs mapped and does "
				"not present as an MLD", mhd->base.id,
				head->head_id, lds);
	}

out:
	check_dcd(v, &mhd->base);
}

/*
 * What is bound to what, Sections 7.2.3.1 and 7.3.4 and Tables 7-34 and
 * 7-35. An SLD port "is bound to only one VCS" and the FM "can bind any
 * unused physical port to the unbound vPPB" (Section 7.2.3.1); "LDs within
 * an MLD component can be bound to a host or unbound" (Section 7.3.4); a
 * vPPB is bound to an MLD port by LD (Table 7-35). That a port, or an LD,
 * is bound to one vPPB is cxl-fmd's reading of that model: no text says
 * "one vPPB".
 */
static void check_switch(struct validation *v, struct fmd_switch *sw)
{
	static const char bind_rule[] = "Binding, Sections 7.2.3.1 and 7.3.4, "
					"as cxl-fmd reads them";
	static const char vcs_rule[] = "Get Virtual CXL Switch Info, Table 7-34";
	static const char port_rule[] = "Get Physical Port State, Table 7-21";
	struct fmd_component *child, *other;

	/* ports */
	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		struct fmd_port *port = fmd_to_port(child);
		struct fmd_component *dev;
		int ndev;

		for (other = child->next_sibling; other; other = other->next_sibling) {
			if (other->type == FMD_COMPONENT_PORT &&
			    fmd_to_port(other)->port_number == port->port_number)
				invalid(v, port_rule, "%s: two ports numbered %d",
					sw->base.id, port->port_number);
		}

		dev = port_device(port, &ndev);
		if (ndev > 1)
			invalid(v, port_rule, "%s: port %d has %d devices",
				sw->base.id, port->port_number, ndev);

		/*
		 * The head of a Multi-Headed device on this port. It presents
		 * an SLD or an MLD (Section 2.5), and that is what the port
		 * sees: Connected Device Type 04h or 05h, on a DSP.
		 *
		 * The link of a port with a device is not checked: Table 7-21
		 * does not tie Connected Device Type to the link, and after a
		 * surprise Link Down "the presence pin ... will remain
		 * asserted" (Section 7.6.6.8). cxl-fmd keeps the device then.
		 */
		if (port->head) {
			struct fmd_head *head = port->head;
			const char *mhd_id = head->base.parent ?
					     head->base.parent->id : "?";

			if (dev)
				invalid(v, port_rule, "%s: port %d has device %s "
					"and head %d of %s", sw->base.id,
					port->port_number, dev->id, head->head_id, mhd_id);
			if (head->port != port)
				invalid(v, "Multi-Headed Devices, Section 2.5",
					"%s: port %d has head %d of %s, which is "
					"on another port", sw->base.id,
					port->port_number, head->head_id, mhd_id);
			if (port->state != FMD_PORT_DSP)
				invalid(v, port_rule, "%s: port %d has head %d of "
					"%s and is not a DSP", sw->base.id,
					port->port_number, head->head_id, mhd_id);
			/* not known: no type to check against */
			if (head->presentation != FMD_HEAD_UNKNOWN &&
			    port->conn_dev_type != (head->presentation == FMD_HEAD_MLD ?
						    FMD_CONN_MLD : FMD_CONN_SLD))
				invalid(v, port_rule, "%s: port %d has head %d of "
					"%s, which presents an %s, and Connected "
					"Device Type %d", sw->base.id,
					port->port_number, head->head_id, mhd_id,
					fmd_head_presentation_name(head->presentation),
					port->conn_dev_type);
		}
		if (!dev)
			continue;

		/* Connected Device Type is that of the device, on a DSP */
		if (port->state != FMD_PORT_DSP) {
			invalid(v, port_rule, "%s: port %d has device %s and is not a DSP",
				sw->base.id, port->port_number, dev->id);
			continue;
		}
		if (dev->type == FMD_COMPONENT_SLD && port->conn_dev_type != FMD_CONN_SLD)
			invalid(v, port_rule, "%s: port %d has SLD %s and Connected "
				"Device Type %d, not 04h", sw->base.id, port->port_number,
				dev->id, port->conn_dev_type);
		/* a head is a port of the device: it has one per head */
		if (dev->type == FMD_COMPONENT_MHD)
			invalid(v, "Multi-Headed Devices, Section 2.5", "%s: port "
				"%d has the whole of Multi-Headed device %s: each "
				"of its heads is on a port of its own",
				sw->base.id, port->port_number, dev->id);
		if (dev->type == FMD_COMPONENT_MLD && port->conn_dev_type != FMD_CONN_MLD)
			invalid(v, port_rule, "%s: port %d has MLD %s and Connected "
				"Device Type %d, not 05h", sw->base.id, port->port_number,
				dev->id, port->conn_dev_type);
	}

	/*
	 * VCSs and their vPPBs. USP ID: "Physical Port ID of the current
	 * VCS's Upstream Port, or the current VCS's fabric physical Port ID of
	 * a Downstream ES VCS. Valid only when the VCS is enabled." (Table
	 * 7-34). So the USP ID of a VCS that is not enabled (VCS State 01h)
	 * is not checked, and it may name a Fabric Port. Two enabled VCSs on
	 * one Upstream Port is refused; the table does not say whether two
	 * Downstream ES VCSs may share a Fabric Port, so that is not checked.
	 */
	for (int i = 0; i < sw->num_vcs; i++) {
		struct fmd_vcs *vcs = &sw->vcs[i];
		struct fmd_port *usp = switch_port(sw, vcs->upstream_port_id);
		bool enabled = vcs->state == VCS_ENABLED;

		for (int j = i + 1; j < sw->num_vcs; j++) {
			if (sw->vcs[j].vcs_id == vcs->vcs_id)
				invalid(v, vcs_rule, "%s: two VCSs with ID %d",
					sw->base.id, vcs->vcs_id);
			if (enabled && sw->vcs[j].state == VCS_ENABLED &&
			    usp && usp->state == FMD_PORT_USP &&
			    sw->vcs[j].upstream_port_id == vcs->upstream_port_id)
				invalid(v, vcs_rule, "%s: VCS %d and VCS %d have the same "
					"upstream port %d", sw->base.id, vcs->vcs_id,
					sw->vcs[j].vcs_id, vcs->upstream_port_id);
		}

		if (enabled && (!usp || (usp->state != FMD_PORT_USP &&
					 usp->state != FMD_PORT_FABRIC)))
			invalid(v, vcs_rule, "%s: VCS %d: USP ID %d is not an upstream "
				"port or a Fabric Port of the switch", sw->base.id,
				vcs->vcs_id, vcs->upstream_port_id);

		for (int p = 0; p < vcs->num_vppbs; p++) {
			struct fmd_vppb *vppb = &vcs->vppbs[p];
			struct fmd_component *dev = NULL;
			struct fmd_port *port;
			int ndev;

			for (int q = p + 1; q < vcs->num_vppbs; q++) {
				if (vcs->vppbs[q].vppb_id == vppb->vppb_id)
					invalid(v, vcs_rule, "%s: VCS %d has two vPPBs "
						"with ID %d", sw->base.id, vcs->vcs_id,
						vppb->vppb_id);
			}

			/*
			 * Bound LD ID: "Valid only when vPPB[Start vPPB]
			 * Binding Status is 03h; otherwise, the value is FFh."
			 * Discovery and the mock loader both store FFh, so
			 * this is cxl-fmd's own state found inconsistent.
			 */
			if (vppb->binding_status != FMD_VPPB_BOUND_LD &&
			    vppb->bound_ld_id != FMD_VPPB_NO_LD)
				invalid(v, vcs_rule, "%s: vPPB %d of VCS %d has Binding "
					"Status %d and Bound LD ID %d, not FFh",
					sw->base.id, vppb->vppb_id, vcs->vcs_id,
					vppb->binding_status, vppb->bound_ld_id);

			if (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
			    vppb->binding_status != FMD_VPPB_BOUND_LD)
				continue;

			/*
			 * "vPPBs within a VCS are ordered in the following
			 * sequence: the USP vPPB, then the DSP vPPBs" (Section
			 * 7.1.4): vPPB 0 is the USP's. That a bound vPPB 0 is
			 * bound to the VCS's own USP, as a whole, is cxl-fmd's
			 * reading of it. The USP ID is "Valid only when the VCS
			 * is enabled" (Table 7-34).
			 */
			if (vppb->vppb_id == 0 && enabled &&
			    (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT ||
			     vppb->bound_port_id != vcs->upstream_port_id))
				invalid(v, "vPPB Ordering, Section 7.1.4, as cxl-fmd "
					"reads it", "%s: vPPB 0 of VCS %d, the USP vPPB, "
					"is bound to port %d%s, not to its USP %d",
					sw->base.id, vcs->vcs_id, vppb->bound_port_id,
					vppb->binding_status == FMD_VPPB_BOUND_LD ?
					" by LD" : "", vcs->upstream_port_id);

			/* a binding that gives no Connection (cxl-fmd's choice) */
			if (!enabled && vppb->vppb_id != 0)
				doubtful(v, "cxl-fmd: Connections are of enabled VCSs",
					 "%s: vPPB %d of VCS %d is bound, and the VCS "
					 "is not enabled: it gives no Connection",
					 sw->base.id, vppb->vppb_id, vcs->vcs_id);

			port = switch_port(sw, vppb->bound_port_id);
			if (!port) {
				invalid(v, vcs_rule, "%s: vPPB %d of VCS %d is bound to "
					"port %d, which the switch does not have",
					sw->base.id, vppb->vppb_id, vcs->vcs_id,
					vppb->bound_port_id);
				continue;
			}
			dev = port_device(port, &ndev);

			/* a USP is the upstream port of its own VCS and no other */
			if (port->state == FMD_PORT_USP &&
			    port->port_number != vcs->upstream_port_id)
				invalid(v, bind_rule, "%s: vPPB %d of VCS %d is bound to "
					"port %d, the upstream port of another VCS",
					sw->base.id, vppb->vppb_id, vcs->vcs_id,
					port->port_number);

			if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT) {
				/* LD ID "must be FFFFh for other EP types": not an MLD */
				if (port->conn_dev_type == FMD_CONN_MLD)
					invalid(v, "Bind vPPB, Table 7-35", "%s: vPPB %d of "
						"VCS %d is bound to port %d as a whole, and "
						"the port has an MLD: it is bound by LD",
						sw->base.id, vppb->vppb_id, vcs->vcs_id,
						port->port_number);
			} else {
				if (port->conn_dev_type != FMD_CONN_MLD)
					invalid(v, "Bind vPPB, Table 7-35", "%s: vPPB %d of "
						"VCS %d is bound to LD %d of port %d, which "
						"has no MLD", sw->base.id, vppb->vppb_id,
						vcs->vcs_id, vppb->bound_ld_id,
						port->port_number);
				/*
				 * Supported LD Count: "Number of additional LDs
				 * supported by this port. All ports must support
				 * at least one LD." (Table 7-21). cxl-fmd reads
				 * "additional" as past the one every port has, so
				 * LD-IDs 0 to the count; the text can also be read
				 * as the total. An LD the device does not have is
				 * refused below either way.
				 */
				else if (vppb->bound_ld_id > port->supported_ld_count)
					invalid(v, "Get Physical Port State, Table 7-21, "
						"as cxl-fmd reads it", "%s: vPPB %d of VCS %d is "
						"bound to LD %d of port %d, which supports "
						"LD-IDs 0 to %d", sw->base.id, vppb->vppb_id,
						vcs->vcs_id, vppb->bound_ld_id,
						port->port_number, port->supported_ld_count);
				else if (dev && !device_ld(dev, vppb->bound_ld_id))
					invalid(v, bind_rule, "%s: vPPB %d of VCS %d is "
						"bound to LD %d of %s, which has no such LD",
						sw->base.id, vppb->vppb_id, vcs->vcs_id,
						vppb->bound_ld_id, dev->id);
				/*
				 * "The MH-MLD maps the LD-IDs received at a head
				 * to the device-wide LD index in the MH-MLD's LD
				 * pool" (Section 2.5.1), by no rule the FM is
				 * told: the binding reaches no pool LD cxl-fmd
				 * can name, and its Connection is unresolved.
				 */
				else if (port->head &&
					 port->head->presentation == FMD_HEAD_MLD)
					doubtful(v, "Section 2.5.1", "%s: vPPB %d of VCS "
						 "%d is bound to LD %d of head %d of %s, "
						 "which presents an MLD: which pool LD "
						 "that is, is not known", sw->base.id,
						 vppb->vppb_id, vcs->vcs_id,
						 vppb->bound_ld_id, port->head->head_id,
						 port->head->base.parent ?
						 port->head->base.parent->id : "?");
			}

			/* the same port, or the same LD, bound a second time */
			for (int i2 = i; i2 < sw->num_vcs; i2++) {
				struct fmd_vcs *vcs2 = &sw->vcs[i2];

				for (int q = (i2 == i) ? p + 1 : 0; q < vcs2->num_vppbs; q++) {
					struct fmd_vppb *o = &vcs2->vppbs[q];

					if ((o->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
					     o->binding_status != FMD_VPPB_BOUND_LD) ||
					    o->bound_port_id != vppb->bound_port_id)
						continue;
					if (vppb->binding_status == FMD_VPPB_BOUND_LD &&
					    o->binding_status == FMD_VPPB_BOUND_LD &&
					    o->bound_ld_id != vppb->bound_ld_id)
						continue;
					invalid(v, bind_rule, "%s: port %d%s is bound to "
						"vPPB %d of VCS %d and to vPPB %d of VCS %d",
						sw->base.id, vppb->bound_port_id,
						vppb->binding_status == FMD_VPPB_BOUND_LD &&
						o->binding_status == FMD_VPPB_BOUND_LD ?
						" (same LD)" : "",
						vppb->vppb_id, vcs->vcs_id,
						o->vppb_id, vcs2->vcs_id);
				}
			}
		}
	}
}

/* every component, each ID once */
static void walk(struct validation *v, struct fmd_component *comp, int depth)
{
	struct fmd_component *child;

	if (depth > 16) {
		invalid(v, "cxl-fmd consistency", "%s: nested more than 16 deep",
			comp->id);
		return;
	}

	if (depth > 0 && fmd_topology_find(&v->ctx->topology, comp->id) != comp)
		invalid(v, "cxl-fmd consistency", "two components with ID %s",
			comp->id);

	switch (comp->type) {
	case FMD_COMPONENT_SWITCH:
		check_switch(v, fmd_to_switch(comp));
		break;
	case FMD_COMPONENT_SLD:
		check_sld(v, fmd_to_sld(comp));
		break;
	case FMD_COMPONENT_MLD:
		check_mld(v, fmd_to_mld(comp));
		break;
	case FMD_COMPONENT_MHD:
		check_mhd(v, fmd_to_mhd(comp));
		break;
	default:
		break;
	}

	fmd_for_each_child(comp, child)
		walk(v, child, depth + 1);
}

/*
 * Device Serial Number: "Unique identifier for this device" (Identify,
 * Table 8-216). 0 is taken to mean the device has none.
 */
static void check_serials(struct validation *v)
{
	static const enum fmd_component_type types[] = {
		FMD_COMPONENT_SWITCH, FMD_COMPONENT_SLD,
		FMD_COMPONENT_MLD, FMD_COMPONENT_MHD,
	};
	struct { uint64_t serial; const char *id; } seen[256];
	struct fmd_component *comp;
	int n = 0;

	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		fmd_for_each_type(&v->ctx->topology, comp, types[t]) {
			uint64_t serial = 0;

			switch (comp->type) {
			case FMD_COMPONENT_SWITCH:
				serial = fmd_to_switch(comp)->identify.serial_num;
				break;
			case FMD_COMPONENT_SLD:
				serial = fmd_to_sld(comp)->identify.serial_num;
				break;
			case FMD_COMPONENT_MLD:
				serial = fmd_to_mld(comp)->identify.serial_num;
				break;
			case FMD_COMPONENT_MHD:
				serial = fmd_to_mhd(comp)->identify.serial_num;
				break;
			default:
				break;
			}
			if (!serial)
				continue;

			for (int i = 0; i < n; i++) {
				if (seen[i].serial == serial)
					invalid(v, "Identify, Table 8-216", "%s and %s have "
						"the same serial number %#" PRIx64,
						seen[i].id, comp->id, serial);
			}
			if (n < (int)(sizeof(seen) / sizeof(seen[0]))) {
				seen[n].serial = serial;
				seen[n].id = comp->id;
				n++;
			}
		}
	}
}

/*
 * A host that reaches its devices through a switch does so through a VCS of
 * that switch. A host is cxl-fmd's model, not the specification's: it is the
 * hierarchy of one VCS, which "Includes entities within the physical switch
 * belonging to a single VH" (glossary, VCS), a VH being "Everything from the
 * CXL RP down" (glossary, VH). This checks that the model is consistent.
 */
static void check_hosts(struct validation *v)
{
	struct fmd_component *comp, *sw_comp;

	fmd_for_each_type(&v->ctx->topology, comp, FMD_COMPONENT_HOST) {
		struct fmd_host *host = fmd_to_host(comp);
		bool found = false;

		if (host->direct_attached)
			continue;

		fmd_for_each_type(&v->ctx->topology, sw_comp, FMD_COMPONENT_SWITCH) {
			struct fmd_vcs *vcs;

			/* a VCS ID is one switch's */
			if (strcmp(sw_comp->id, host->switch_id) != 0)
				continue;
			vcs = fmd_switch_find_vcs(fmd_to_switch(sw_comp), host->vcs_id);
			if (vcs && vcs->upstream_port_id == host->usp_port_id)
				found = true;
		}
		if (!found)
			invalid(v, "cxl-fmd consistency", "host %s is on "
				"VCS %d of switch %s with upstream port %d, which is "
				"no VCS of the topology", host->base.id, host->vcs_id,
				host->switch_id[0] ? host->switch_id : "(none)",
				host->usp_port_id);
	}
}

/* Returns the number of ways the topology is not a valid CXL fabric. */
int fmd_topology_validate(struct fmd_ctx *ctx)
{
	struct validation v = { .ctx = ctx };

	if (!ctx || !ctx->topology.root)
		return -1;

	walk(&v, &ctx->topology.root->base, 0);
	check_serials(&v);
	check_hosts(&v);

	return v.errors;
}
