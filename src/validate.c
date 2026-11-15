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
static void check_dcd(struct validation *v, struct fmd_component *dev)
{
	static const char rule[] = "DC Region Configuration, Table 8-347";
	struct fmd_dcd_info *dcd = fmd_memdev_dcd(dev);
	struct fmd_component *child, *other;

	if (!dcd)
		return;

	for (int h = 0; h < dcd->num_hosts && h < FMD_MAX_DC_HOSTS; h++) {
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

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);

		/* "The Length shall be > 0." */
		if (ext->len == 0)
			invalid(v, "Dynamic Capacity Extent, Table 8-230", "%s: extent at %#" PRIx64
				" has no length", dev->id, ext->start_dpa);

		for (int h = 0; h < dcd->num_hosts && h < FMD_MAX_DC_HOSTS; h++) {
			struct fmd_dc_region *reg;

			if (!(ext->host_mask & fmd_dc_host_bit(h)))
				continue;
			reg = fmd_dc_find_region(dev, h, ext->region_id);
			if (!reg) {
				invalid(v, rule, "%s: extent at %#" PRIx64 " is in region %d, "
					"which host %d does not have", dev->id,
					ext->start_dpa, ext->region_id, h);
				continue;
			}
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
		 * via a non-Sharable region" (Section 7.6.7.6.5).
		 */
		if (ext->host_mask & (ext->host_mask - 1)) {
			for (int h = 0; h < dcd->num_hosts && h < FMD_MAX_DC_HOSTS; h++) {
				struct fmd_dc_region *reg;

				if (!(ext->host_mask & fmd_dc_host_bit(h)))
					continue;
				reg = fmd_dc_find_region(dev, h, ext->region_id);
				if (reg && !(reg->flags & FMD_DC_REGION_SHAREABLE)) {
					invalid(v, "Initiate Dynamic Capacity Add, "
						"Section 7.6.7.6.5", "%s: extent at %#"
						PRIx64 " is held by more than one host "
						"and region %d is not Sharable",
						dev->id, ext->start_dpa, ext->region_id);
					break;
				}
			}
		}

		/*
		 * One DPA range is one extent. An Extent List with "Overlapping
		 * DPA ranges for multiple extents" is malformed (Sections
		 * 8.2.10.9.9.3 and 8.2.10.9.9.4).
		 */
		for (other = child->next_sibling; other; other = other->next_sibling) {
			struct fmd_dc_extent *o;

			if (other->type != FMD_COMPONENT_DC_EXTENT)
				continue;
			o = fmd_to_dc_extent(other);
			if (ext->start_dpa < o->start_dpa + o->len &&
			    o->start_dpa < ext->start_dpa + ext->len)
				invalid(v, "malformed Extent List, Sections 8.2.10.9.9.3 "
					"and 8.2.10.9.9.4", "%s: extents at %#" PRIx64 " and %#" PRIx64
					" overlap", dev->id, ext->start_dpa, o->start_dpa);
		}
	}
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

	/* with part of the pool, a head may have LDs that were not kept */
	if (!all_lds)
		goto out;

	for (int h = 0; h < FMD_MAX_HEADS; h++) {
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

		if (head->head_id >= FMD_MAX_HEADS)
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
			if (port->conn_dev_type != (head->presentation == FMD_HEAD_MLD ?
						    FMD_CONN_MLD : FMD_CONN_SLD))
				invalid(v, port_rule, "%s: port %d has head %d of "
					"%s, which presents an %s, and Connected "
					"Device Type %d", sw->base.id,
					port->port_number, head->head_id, mhd_id,
					head->presentation == FMD_HEAD_MLD ? "MLD" : "SLD",
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

			if (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
			    vppb->binding_status != FMD_VPPB_BOUND_LD)
				continue;

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
