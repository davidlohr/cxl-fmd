/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Device Discovery
 *
 * Discovery Algorithm:
 * 1. For each MCTP endpoint, send Identify (0001h)
 * 2. Check component_type:
 *    - TYPE3: Send Get LD Info (5400h) to determine SLD vs MLD
 *    - SWITCH: Get Physical Port State (5100h), recurse into each port
 * 3. SLD discovery: Memdev Identify (4000h), Partition Info (4100h), Health (4200h)
 * 4. MLD discovery: LD Info (5400h), LD Allocations (5401h), QoS commands,
 *                   tunneled per-LD memdev commands
 *
 * For switch-attached devices, conn_dev_type from Get Physical Port State
 * tells us SLD (0x4) vs MLD (0x5) directly - no need for Get LD Info.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include "cxl-fmd.h"
#include "mock.h"  /* For FMD_CMD_* definitions */

/*
 * Command Effects Log (CEL) UUID
 */
static const uint8_t cel_uuid[16] = {
	0x0d, 0xa9, 0xc0, 0xb5, 0xbf, 0x41, 0x4b, 0x78,
	0x8f, 0x79, 0x96, 0xb1, 0x62, 0x3b, 0x3f, 0x17
};

/*
 * Map CCI opcode to supported_cmds bitmask
 *
 * Returns the FMD_CMD_* bit for known opcodes, 0 for unknown
 */
static uint64_t opcode_to_cmd_bit(uint16_t opcode)
{
	switch (opcode) {
	/* Generic Component Commands */
	case 0x0001: return FMD_CMD_IDENTIFY;
	case 0x0002: return FMD_CMD_BG_OP_STATUS;
	case 0x0003: return FMD_CMD_GET_RSP_MSG_LIMIT;
	case 0x0004: return FMD_CMD_SET_RSP_MSG_LIMIT;

	/* Log Commands */
	case 0x0400: return FMD_CMD_GET_SUPPORTED_LOGS;
	case 0x0401: return FMD_CMD_GET_LOG;
	case 0x0402: return FMD_CMD_GET_LOG_CAPS;
	case 0x0403: return FMD_CMD_CLEAR_LOG;

	/* Memory Device Commands */
	case 0x4000: return FMD_CMD_MEMDEV_IDENTIFY;
	case 0x4001: return FMD_CMD_MEMDEV_CAPACITY;
	case 0x4100: return FMD_CMD_GET_PARTITION_INFO;
	case 0x4101: return FMD_CMD_SET_PARTITION_INFO;
	case 0x4200: return FMD_CMD_GET_HEALTH_INFO;
	case 0x4201: return FMD_CMD_GET_ALERT_CONFIG;
	case 0x4202: return FMD_CMD_SET_ALERT_CONFIG;
	case 0x4203: return FMD_CMD_GET_SHUTDOWN_STATE;
	case 0x4204: return FMD_CMD_SET_SHUTDOWN_STATE;
	case 0x4300: return FMD_CMD_GET_POISON_LIST;
	case 0x4301: return FMD_CMD_INJECT_POISON;
	case 0x4302: return FMD_CMD_CLEAR_POISON;

	default:
		return 0;
	}
}

/*
 * The limit of a component reached through one more tunnel than @parent.
 * What it accepts itself is not known without asking it, so it is held to
 * the minimum, and to what its parent can carry.
 */
static uint16_t tunnelled_msg_limit(const struct fmd_component *parent)
{
	size_t limit = fmd_cci_msg_limit(parent) - FMD_CCI_TUNNEL_OVERHEAD;

	return limit < FMD_CCI_MSG_MIN ? limit : FMD_CCI_MSG_MIN;
}

/* A CEL entry is 4 bytes and there are 2^16 opcodes */
#define CEL_MAX_SIZE (4 * 65536)

/*
 * Get supported commands from Command Effects Log (CEL)
 *
 * Fetches the CEL from the device and converts the opcodes to a bitmask.
 * Returns 0 on success, -1 on error. On error, supported_cmds is set to
 * a reasonable default.
 */
static int get_supported_commands(struct fmd_ctx *ctx,
				  const struct fmd_component *comp,
				  struct cxlmi_tunnel_info *ti,
				  uint64_t *supported_cmds)
{
	struct cxlmi_endpoint *ep = comp->ep;
	struct cxlmi_cmd_get_supported_logs_rsp *logs_rsp = NULL;
	struct cxlmi_cmd_get_log_cel_rsp *cel_rsp = NULL;
	struct cxlmi_cmd_get_log_req log_req;
	uint32_t cel_size = 0;
	uint32_t chunk;
	int num_entries;
	int rc;

	*supported_cmds = FMD_CMD_DEFAULT_MEMDEV;

	/* Allocate buffer for supported logs response */
	/* the entry count is 16 bits and the device's to choose */
	logs_rsp = calloc(1, sizeof(*logs_rsp) +
			  UINT16_MAX * sizeof(logs_rsp->entries[0]));
	if (!logs_rsp)
		return -1;

	/* Get Supported Logs (0400h) */
	rc = cxlmi_cmd_get_supported_logs(ep, ti, logs_rsp);
	if (rc) {
		fmd_dbg(ctx, "Get Supported Logs failed: %d", rc);
		goto out;
	}

	/* Find CEL in the supported logs list */
	for (int i = 0; i < logs_rsp->num_supported_log_entries; i++) {
		if (memcmp(logs_rsp->entries[i].uuid, cel_uuid, sizeof(cel_uuid)) == 0) {
			cel_size = logs_rsp->entries[i].log_size;
			break;
		}
	}

	/* whole entries only, and no more than there are opcodes */
	if (cel_size > CEL_MAX_SIZE)
		cel_size = CEL_MAX_SIZE;
	cel_size -= cel_size % sizeof(*cel_rsp);

	if (cel_size == 0) {
		fmd_dbg(ctx, "CEL not found in supported logs");
		rc = -1;
		goto out;
	}

	/* Allocate buffer for CEL entries */
	cel_rsp = calloc(1, cel_size);
	if (!cel_rsp) {
		rc = -1;
		goto out;
	}

	/* Get Log (0401h) - fetch CEL */
	/*
	 * Get Log returns Length bytes from Offset: read the CEL in parts
	 * that fit a response, a whole number of entries each.
	 */
	chunk = fmd_cci_msg_limit(comp);
	chunk -= chunk % sizeof(*cel_rsp);
	for (uint32_t off = 0; off < cel_size; off += chunk) {
		memset(&log_req, 0, sizeof(log_req));
		memcpy(log_req.uuid, cel_uuid, sizeof(cel_uuid));
		log_req.offset = off;
		log_req.length = cel_size - off < chunk ? cel_size - off : chunk;

		rc = cxlmi_cmd_get_log_cel(ep, ti, &log_req,
					   cel_rsp + off / sizeof(*cel_rsp));
		if (rc) {
			fmd_dbg(ctx, "Get Log (CEL) failed: %d", rc);
			goto out;
		}
	}

	/* Parse CEL entries and build supported_cmds bitmask */
	*supported_cmds = 0;
	num_entries = cel_size / sizeof(struct cxlmi_cmd_get_log_cel_rsp);
	for (int i = 0; i < num_entries; i++) {
		uint64_t bit = opcode_to_cmd_bit(cel_rsp[i].opcode);
		*supported_cmds |= bit;

		fmd_dbg(ctx, "CEL[%d]: opcode=0x%04x effect=0x%04x%s",
			i, cel_rsp[i].opcode, cel_rsp[i].command_effect,
			bit ? "" : " (unmapped)");
	}

	fmd_dbg(ctx, "Supported commands: 0x%016" PRIx64, *supported_cmds);
	rc = 0;

out:
	free(logs_rsp);
	free(cel_rsp);
	return rc;
}

/* Forward declarations */
static int discover_sld(struct fmd_ctx *ctx, struct fmd_sld *sld);
static int discover_mld(struct fmd_ctx *ctx, struct fmd_mld *mld);
static int discover_ld(struct fmd_ctx *ctx, struct fmd_ld *ld,
		       struct fmd_mld *mld);
static int discover_switch(struct fmd_ctx *ctx, struct fmd_switch *sw);

/*
 * Link status from the LTSSM State of Get Physical Port State and of Get
 * Head Info (CXL r4.0 Tables 7-21 and 7-64, same encoding). The Link State
 * Flags field next to it is not the link state: it holds lane reversal,
 * PERST#, PRSNT# and power control.
 */
static enum fmd_link_status ltssm_to_link_status(uint8_t ltssm)
{
	switch (ltssm) {
	case 0x04: /* L0 */
	case 0x05: /* L0s */
	case 0x06: /* L1 */
		return FMD_LINK_UP;
	case 0x01: /* Polling */
	case 0x02: /* Configuration */
	case 0x03: /* Recovery */
		return FMD_LINK_TRAINING;
	default:   /* Detect, L2, Disabled, Loopback, Hot Reset, reserved */
		return FMD_LINK_DOWN;
	}
}


/*
 * Discover SLD - Simple memory device with single LD
 *
 * Commands: Get Supported Logs (0400h), Get Log/CEL (0401h),
 *           Memdev Identify (4000h), Get Partition Info (4100h),
 *           Get Health Info (4200h)
 *
 * SLDs never need tunneling - switch is transparent for SLD access.
 */
static int discover_sld(struct fmd_ctx *ctx, struct fmd_sld *sld)
{
	struct cxlmi_endpoint *ep;
	int rc;

	ep = sld->base.ep;
	if (!ep)
		return -1;

	/* Memdev Identify (4000h) - no tunneling for SLD */
	rc = cxlmi_cmd_memdev_identify(ep, CXLMI_NO_TUNNEL, &sld->memdev);
	if (rc) {
		fmd_warn(ctx, "%s: memdev identify failed: %d", sld->base.id, rc);
		return rc;
	}

	sld->memory_size = CXL_TO_BYTES(sld->memdev.total_capacity);
	sld->volatile_size = CXL_TO_BYTES(sld->memdev.volatile_capacity);
	sld->persistent_size = CXL_TO_BYTES(sld->memdev.persistent_capacity);

	/* Get supported commands from CEL */
	get_supported_commands(ctx, &sld->base, CXLMI_NO_TUNNEL, &sld->supported_cmds);

	fmd_dbg(ctx, "%s: FW=%.16s total=%" PRIu64 "MB volatile=%" PRIu64 "MB persistent=%" PRIu64 "MB",
		sld->base.id, sld->memdev.fw_revision,
		(uint64_t)(sld->memory_size / (1024ULL * 1024)),
		(uint64_t)(sld->volatile_size / (1024ULL * 1024)),
		(uint64_t)(sld->persistent_size / (1024ULL * 1024)));

	/* Get Partition Info (4100h) - no tunneling for SLD */
	rc = cxlmi_cmd_memdev_get_partition_info(ep, CXLMI_NO_TUNNEL, &sld->partition);
	if (rc)
		fmd_dbg(ctx, "%s: get partition info failed: %d", sld->base.id, rc);

	/* Get Health Info (4200h) - no tunneling for SLD */
	rc = cxlmi_cmd_memdev_get_health_info(ep, CXLMI_NO_TUNNEL, &sld->health);
	if (rc)
		fmd_dbg(ctx, "%s: get health info failed: %d", sld->base.id, rc);

	return 0;
}


/*
 * Discover per-LD info using tunneled commands
 *
 * LDs always need tunneling:
 * - Direct-attached MLD: CXLMI_TUNNEL_MLD(ld)
 * - Switch-attached MLD: CXLMI_TUNNEL_SWITCH_MLD(port, ld)
 *
 * Commands: Identify (0001h), Get Supported Logs (0400h), Get Log/CEL (0401h),
 *           Memdev Identify (4000h), Get Partition Info (4100h),
 *           Get Health Info (4200h)
 */
static int discover_ld(struct fmd_ctx *ctx, struct fmd_ld *ld,
		       struct fmd_mld *mld)
{
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info *ti;
	int rc;

	ep = mld->base.ep;
	if (!ep)
		return -1;

	/* Select tunnel based on MLD topology */
	ti = (mld->switch_port < 0)
		? CXLMI_TUNNEL_MLD(ld->ld_id)
		: CXLMI_TUNNEL_SWITCH_MLD(mld->switch_port, ld->ld_id);

	/* Identify (0001h) */
	rc = cxlmi_cmd_identify(ep, ti, &ld->identify);
	if (rc)
		fmd_dbg(ctx, "%s: LD%d identify failed: %d",
			mld->base.id, ld->ld_id, rc);

	/* Get supported commands from CEL */
	get_supported_commands(ctx, &ld->base, ti, &ld->supported_cmds);

	/* Memdev Identify (4000h) */
	rc = cxlmi_cmd_memdev_identify(ep, ti, &ld->memdev);
	if (rc)
		fmd_dbg(ctx, "%s: LD%d memdev identify failed: %d",
			mld->base.id, ld->ld_id, rc);

	/* Get Partition Info (4100h) */
	rc = cxlmi_cmd_memdev_get_partition_info(ep, ti, &ld->partition);
	if (rc)
		fmd_dbg(ctx, "%s: LD%d get partition info failed: %d",
			mld->base.id, ld->ld_id, rc);

	/* Get Health Info (4200h) */
	rc = cxlmi_cmd_memdev_get_health_info(ep, ti, &ld->health);
	if (rc)
		fmd_dbg(ctx, "%s: LD%d get health info failed: %d",
			mld->base.id, ld->ld_id, rc);

	fmd_dbg(ctx, "%s: LD%d memory=%" PRIu64 "MB health=%d media=%d",
		mld->base.id, ld->ld_id,
		(uint64_t)(ld->memory_size / (1024ULL * 1024)),
		ld->health.health_status, ld->health.media_status);

	return 0;
}

/*
 * Add @comp, discovered, under @parent (NULL: the root). An Id some
 * component has already is refused (fmd_topology_add()): logged, and
 * @comp freed. Returns 0 or the error; the caller is not to use @comp
 * after an error.
 */
static int discovered_add(struct fmd_ctx *ctx, struct fmd_component *comp,
			  struct fmd_component *parent)
{
	int rc = fmd_topology_add(&ctx->topology, comp, parent);

	if (rc) {
		fmd_err(ctx, "%s: not added: %s", comp->id,
			rc == -EEXIST ? "a component has that ID already" : "error");
		fmd_component_free(comp);
	}
	return rc;
}

/*
 * Discover MLD - Complex memory device with multiple LDs
 *
 * Commands: Get Supported Logs (0400h), Get Log/CEL (0401h),
 *           Get LD Info (5400h), Get LD Allocations (5401h),
 *           Get QoS Control (5403h), Get QoS Status (5405h),
 *           Get QoS Allocated BW (5406h), Get QoS BW Limit (5408h),
 *           Per-LD: tunneled Memdev Identify, Partition Info, Health Info
 */
static int discover_mld(struct fmd_ctx *ctx, struct fmd_mld *mld)
{
	struct cxlmi_cmd_fmapi_get_ld_info_rsp ld_info = {0};
	struct cxlmi_cmd_fmapi_get_ld_allocations_req alloc_req;
	struct cxlmi_cmd_fmapi_get_ld_allocations_rsp *alloc_rsp = NULL;
	struct cxlmi_cmd_fmapi_get_qos_control_rsp qos_ctrl = {0};
	struct cxlmi_cmd_fmapi_get_qos_status_rsp qos_status = {0};
	struct cxlmi_cmd_fmapi_get_qos_allocated_bw_req bw_req;
	struct cxlmi_cmd_fmapi_get_qos_allocated_bw_rsp *alloc_bw = NULL;
	struct cxlmi_cmd_fmapi_get_qos_bw_limit_rsp *limit_bw = NULL;
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info *ti;
	size_t rsp_size;
	size_t bw_rsp_size;
	uint64_t granularity = CXL_CAPACITY_UNIT; /* until 5401h says otherwise */
	uint64_t ld_units[FMD_MAX_LD_COUNT];	/* allocation of each LD */
	uint8_t nld, nalloc = 0;
	int rc;
	int i;

	ep = mld->base.ep;
	if (!ep)
		return -1;

	/* MLD component commands: tunnel through switch if behind one */
	ti = (mld->switch_port < 0)
		? CXLMI_NO_TUNNEL
		: CXLMI_TUNNEL_SWITCH(mld->switch_port);

	/* Get supported commands from CEL */
	//get_supported_commands(ctx, ep, ti, &mld->supported_cmds);

	/* Get LD Info (5400h) */
	rc = cxlmi_cmd_fmapi_get_ld_info(ep, ti, &ld_info);
	if (rc) {
		fmd_warn(ctx, "%s: get ld info failed: %d", mld->base.id, rc);
		return rc;
	}

	/*
	 * LD Count is the "number of logical devices supported" (Table
	 * 7-46). How many of them exist is the Number of LDs "enabled in the
	 * device" of Get LD Allocations (Table 7-48), read below; until then
	 * every supported LD is taken to.
	 */
	mld->max_ld_count = ld_info.ld_count < FMD_MAX_LD_COUNT ?
			    ld_info.ld_count : FMD_MAX_LD_COUNT;
	mld->ld_count = mld->max_ld_count;
	mld->qos_telemetry_capability = ld_info.qos_telemetry_capability;
	mld->memory_granularity = 1; /* 256 MB, until 5401h says otherwise */
	mld->total_memory_ranges = 0; /* Not available in this response */

	/* Get LD Allocations (5401h) */
	nld = mld->ld_count;

	rsp_size = sizeof(struct cxlmi_cmd_fmapi_get_ld_allocations_rsp) +
		   FMD_CCI_LIST_MAX * sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	alloc_rsp = calloc(1, rsp_size);
	if (!alloc_rsp) {
		fmd_warn(ctx, "%s: failed to allocate LD allocations buffer", mld->base.id);
		return -1;
	}

	/*
	 * As many LDs at a time as fit a response, from Start LD ID on. The
	 * device returns min(limit, LDs enabled), so no more is asked for
	 * than there are: the first request is for one LD, whose response
	 * says how many are enabled. 16 LDs do not fit the 256 bytes every
	 * CCI supports.
	 */
	while (nalloc < nld) {
		uint8_t want = nalloc ? fmd_cci_list_fit(&mld->base, 4, 16,
							 nld - nalloc) : 1;

		alloc_req.start_ld_id = nalloc;
		alloc_req.ld_allocation_list_limit = want;
		memset(alloc_rsp, 0, rsp_size);

		rc = cxlmi_cmd_fmapi_get_ld_allocations(ep, ti, &alloc_req, alloc_rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get ld allocations failed: %d", mld->base.id, rc);
			break;
		}
		if (alloc_rsp->memory_granularity > 2) {
			/* without the unit, the multipliers mean nothing */
			fmd_warn(ctx, "%s: reserved memory granularity %d, LD sizes unknown",
				 mld->base.id, alloc_rsp->memory_granularity);
			nalloc = 0;
			break;
		}

		/*
		 * Memory Granularity: 0h = 256 MB, 1h = 512 MB, 2h = 1 GB
		 * (CXL r4.0 Table 7-48). Kept as a count of 256 MB units,
		 * which is what Set LD Allocations converts with.
		 */
		mld->memory_granularity = 1 << alloc_rsp->memory_granularity;
		granularity = mld->memory_granularity * CXL_CAPACITY_UNIT;

		/* Number of LDs: those enabled, of the ones supported */
		if (nalloc == 0 && alloc_rsp->number_ld < nld) {
			nld = alloc_rsp->number_ld;
			mld->ld_count = nld;
		}

		FMD_CCI_LIST_CLAMP(alloc_rsp->ld_allocation_list_len, want);
		if (alloc_rsp->ld_allocation_list_len == 0)
			break;
		for (i = 0; i < alloc_rsp->ld_allocation_list_len; i++)
			ld_units[nalloc++] =
				alloc_rsp->ld_allocation_list[i].range_1_allocation_mult +
				alloc_rsp->ld_allocation_list[i].range_2_allocation_mult;
	}
	free(alloc_rsp);

	fmd_info(ctx, "%s: MLD with %d logical devices of %d supported",
		 mld->base.id, mld->ld_count, mld->max_ld_count);

	/* Get QoS Control (5403h) */
	rc = cxlmi_cmd_fmapi_get_qos_control(ep, ti, &qos_ctrl);
	if (rc == 0) {
		/* Table 7-52, all of it */
		mld->qos_control.enabled = qos_ctrl.qos_telemetry_control & 0x01;
		mld->qos_control.temp_throughput_reduction_enabled =
			qos_ctrl.qos_telemetry_control & 0x02;
		mld->qos_control.egress_moderate_pct = qos_ctrl.egress_moderate_percentage;
		mld->qos_control.egress_severe_pct = qos_ctrl.egress_severe_percentage;
		mld->qos_control.backpressure_sample_interval = qos_ctrl.backpressure_sample_interval;
		mld->qos_control.reqcmp_basis = qos_ctrl.recmpbasis;
		mld->qos_control.completion_collection_interval =
			qos_ctrl.completion_collection_interval;
	}

	/* Get QoS Status (5405h) */
	rc = cxlmi_cmd_fmapi_get_qos_status(ep, ti, &qos_status);
	if (rc == 0) {
		mld->qos_status.backpressure_avg_valid = true;
		mld->qos_status.backpressure_avg_pct = qos_status.backpressure_avg_percentage;
	}

	/* Get QoS Allocated BW (5406h) and BW Limit (5408h) */
	bw_req.number_ld = nld;
	bw_req.start_ld_id = 0;

	bw_rsp_size = sizeof(struct cxlmi_cmd_fmapi_get_qos_allocated_bw_rsp) +
		      FMD_CCI_LIST_MAX * sizeof(uint8_t);
	alloc_bw = calloc(1, bw_rsp_size);
	limit_bw = calloc(1, bw_rsp_size);

	if (alloc_bw) {
		rc = cxlmi_cmd_fmapi_get_qos_allocated_bw(ep, ti, &bw_req, alloc_bw);
		if (rc)
			fmd_dbg(ctx, "%s: get qos allocated bw failed: %d", mld->base.id, rc);
		FMD_CCI_LIST_CLAMP(alloc_bw->number_ld, nld);
	}

	if (limit_bw) {
		struct cxlmi_cmd_fmapi_get_qos_bw_limit_req limit_req = {
			.number_ld = nld,
			.start_ld_id = 0
		};
		rc = cxlmi_cmd_fmapi_get_qos_bw_limit(ep, ti, &limit_req, limit_bw);
		if (rc)
			fmd_dbg(ctx, "%s: get qos bw limit failed: %d", mld->base.id, rc);
		FMD_CCI_LIST_CLAMP(limit_bw->number_ld, nld);
	}

	/* Create LD children */
	for (i = 0; i < mld->ld_count && i < FMD_MAX_LD_COUNT; i++) {
		char ld_id_str[128];
		struct fmd_ld *ld;

		(void)snprintf(ld_id_str, sizeof(ld_id_str), "%.50s_ld%d", mld->base.id, i);

		ld = fmd_ld_create(ld_id_str, i);
		if (!ld)
			continue;
		ld->base.msg_limit = tunnelled_msg_limit(&mld->base);

		/* Memory size from allocations */
		if (i < nalloc)
			ld->memory_size = ld_units[i] * granularity;

		/* QoS from bandwidth responses */
		if (alloc_bw && i < alloc_bw->number_ld)
			ld->qos_allocated_bw = alloc_bw->qos_allocation_fraction[i];
		if (limit_bw && i < limit_bw->number_ld)
			ld->qos_limit_bw = limit_bw->qos_limit_fraction[i];

		/* Add to topology */
		if (discovered_add(ctx, &ld->base, &mld->base))
			continue;

		/* Discover per-LD info via tunneled commands */
		discover_ld(ctx, ld, mld);
	}

	free(alloc_bw);
	free(limit_bw);

	return 0;
}


/*
 * Discover MHD - Multi-Headed Device with multiple CXL upstream ports
 *
 * Commands: Get Multi-Headed Info (5500h), Get Head Info (5501h),
 *           Per-LD discovery similar to MLD
 */
static int discover_mhd(struct fmd_ctx *ctx, struct fmd_mhd *mhd)
{
	struct cxlmi_cmd_fmapi_get_multiheaded_info_req mh_req = {0};
	struct cxlmi_cmd_fmapi_get_multiheaded_info_rsp *mh_rsp = NULL;
	struct cxlmi_cmd_fmapi_get_head_info_req head_req = {0};
	struct cxlmi_cmd_fmapi_get_head_info_rsp *head_rsp = NULL;
	struct cxlmi_endpoint *ep;
	size_t mh_rsp_size;
	size_t head_rsp_size;
	uint8_t head_port[FMD_MAX_HEADS];	/* Port Number of each head */
	int nhead_ports = 0;
	int rc;
	int i;

	ep = mhd->base.ep;
	if (!ep)
		return -1;

	/* Get Multi-Headed Info (5500h) - query all LDs */
	mh_req.start_ld_id = 0;
	mh_req.ld_map_list_limit = FMD_MAX_LD_POOL;

	mh_rsp_size = sizeof(struct cxlmi_cmd_fmapi_get_multiheaded_info_rsp) +
		      FMD_CCI_LIST_MAX;
	mh_rsp = calloc(1, mh_rsp_size);
	if (!mh_rsp)
		return -1;

	rc = cxlmi_cmd_fmapi_get_multiheaded_info(ep, CXLMI_NO_TUNNEL, &mh_req, mh_rsp);
	if (rc) {
		fmd_warn(ctx, "%s: get multiheaded info failed: %d", mhd->base.id, rc);
		free(mh_rsp);
		return rc;
	}
	/* answered with no tunnel: see fmd_mhd_pool_route() */
	mhd->pool_cci = true;

	FMD_CCI_LIST_CLAMP(mh_rsp->ld_map_len, FMD_MAX_LD_POOL);

	mhd->num_heads = mh_rsp->num_heads;
	mhd->max_heads = mh_rsp->num_heads;  /* Assume current = max */
	mhd->total_lds = mh_rsp->num_lds;
	mhd->max_lds = mh_rsp->num_lds;

	fmd_info(ctx, "%s: MHD with %d heads, %d LDs in pool",
		 mhd->base.id, mhd->num_heads, mhd->total_lds);

	/* the specification sets no such limit: these are cxl-fmd's own */
	if (mhd->num_heads > FMD_MAX_HEADS || mhd->total_lds > FMD_MAX_LD_POOL)
		fmd_warn(ctx, "%s: only the first %d heads and %d pool LDs are "
			 "kept, of %d and %d: the device is only partly managed",
			 mhd->base.id, FMD_MAX_HEADS, FMD_MAX_LD_POOL,
			 mhd->num_heads, mhd->total_lds);

	/* Get Head Info (5501h) for all heads */
	/* no more heads than are kept: 8 blocks fit any response */
	head_req.start_head = 0;
	head_req.num_heads = mhd->num_heads < FMD_MAX_HEADS ?
			     mhd->num_heads : FMD_MAX_HEADS;

	head_rsp_size = sizeof(struct cxlmi_cmd_fmapi_get_head_info_rsp) +
			FMD_CCI_LIST_MAX * sizeof(struct cxlmi_cmd_fmapi_get_head_info_blkfmt);
	head_rsp = calloc(1, head_rsp_size);
	if (!head_rsp) {
		free(mh_rsp);
		return -1;
	}

	rc = cxlmi_cmd_fmapi_get_head_info(ep, CXLMI_NO_TUNNEL, &head_req, head_rsp);
	if (rc) {
		fmd_warn(ctx, "%s: get head info failed: %d", mhd->base.id, rc);
		/* Continue without head details */
		head_rsp->num_heads = 0;
	}
	FMD_CCI_LIST_CLAMP(head_rsp->num_heads, head_req.num_heads);

	/* Create head children */
	for (i = 0; i < mhd->num_heads && i < FMD_MAX_HEADS; i++) {
		char head_id_str[FMD_MAX_ID_LEN];
		struct fmd_head *head;

		(void)snprintf(head_id_str, sizeof(head_id_str), "%.50s_head%d",
			       mhd->base.id, i);

		head = fmd_head_create(head_id_str, i);
		if (!head)
			continue;

		head->state = FMD_HEAD_ENABLED;

		/* Fill from head info response if available */
		if (head_rsp && i < head_rsp->num_heads) {
			struct cxlmi_cmd_fmapi_get_head_info_blkfmt *hi =
				&head_rsp->head_info_list[i];

			head->port_number = hi->port_num;
			head->port_number_valid = true;
			head_port[i] = hi->port_num;
			nhead_ports = i + 1;
			head->max_link_width = hi->field_1;
			head->negotiated_link_width = hi->field_2;

			/* A head whose link is not up is not in use */
			if (ltssm_to_link_status(hi->ltssm_state) != FMD_LINK_UP)
				head->state = FMD_HEAD_DISABLED;
		}

		if (discovered_add(ctx, &head->base, &mhd->base))
			continue;

		fmd_dbg(ctx, "%s: head %d link_width=%d state=%d",
			mhd->base.id, i, head->negotiated_link_width, head->state);
	}

	/*
	 * Create LD pool children and map them to heads.
	 * ld_map[i] contains the binding point (head ID) for pool LD i.
	 * Value 0xFF means unbound.
	 */
	for (i = 0; i < mhd->total_lds && i < FMD_MAX_LD_POOL; i++) {
		char ld_id_str[FMD_MAX_ID_LEN];
		struct fmd_ld *ld;
		uint8_t binding_point = 0xFF;

		(void)snprintf(ld_id_str, sizeof(ld_id_str), "%.50s_ld%d",
			       mhd->base.id, i);

		ld = fmd_ld_create(ld_id_str, i);
		if (!ld)
			continue;

		/*
		 * The LD Map gives the port number of the head an LD is
		 * assigned to, FFh for none. The head with that Port Number
		 * in Get Head Info is the one: without the port numbers the
		 * LD cannot be placed, and is left unassigned.
		 */
		if (i < mh_rsp->ld_map_len && mh_rsp->ld_map[i] != 0xFF) {
			for (int h = 0; h < nhead_ports; h++) {
				if (head_port[h] == mh_rsp->ld_map[i]) {
					binding_point = h;
					break;
				}
			}
			if (binding_point == 0xFF)
				fmd_warn(ctx, "%s: pool LD %d is on port %d, "
					 "which is no head of the device",
					 mhd->base.id, i, mh_rsp->ld_map[i]);
		}

		ld->binding_point_id = binding_point;
		ld->visible_ld_id = i;  /* Pool index */
		ld->base.msg_limit = tunnelled_msg_limit(&mhd->base);

		if (discovered_add(ctx, &ld->base, &mhd->base))
			continue;

		/*
		 * The LD Pool CCI tunnels "management commands to all LDs
		 * within the device" (Section 2.5). Each LD is a Type 3
		 * device: Identify Memory Device gives its capacity.
		 */
		rc = cxlmi_cmd_memdev_identify(ep, CXLMI_TUNNEL_MLD(i), &ld->memdev);
		if (rc)
			fmd_dbg(ctx, "%s: pool LD %d memdev identify failed: %d",
				mhd->base.id, i, rc);
		else
			ld->memory_size = CXL_TO_BYTES(ld->memdev.total_capacity);

		/* and its partitioning and health, as for the LD of an MLD */
		rc = cxlmi_cmd_memdev_get_partition_info(ep, CXLMI_TUNNEL_MLD(i),
							 &ld->partition);
		if (rc)
			fmd_dbg(ctx, "%s: pool LD %d get partition info failed: %d",
				mhd->base.id, i, rc);
		rc = cxlmi_cmd_memdev_get_health_info(ep, CXLMI_TUNNEL_MLD(i),
						      &ld->health);
		if (rc)
			fmd_dbg(ctx, "%s: pool LD %d get health info failed: %d",
				mhd->base.id, i, rc);

		fmd_dbg(ctx, "%s: pool LD %d bound to head %d",
			mhd->base.id, i,
			binding_point == 0xFF ? -1 : binding_point);
	}

	/*
	 * Count LDs per head and determine head presentation type.
	 * If head has >1 LD mapped, it presents as MLD; otherwise SLD.
	 */
	for (i = 0; i < mhd->num_heads; i++) {
		struct fmd_component *child;
		struct fmd_head *head = NULL;
		int ld_count = 0;

		/* Find the head */
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
			struct fmd_head *h = fmd_to_head(child);
			if (h->head_id == i) {
				head = h;
				break;
			}
		}

		if (!head)
			continue;

		/* Count LDs bound to this head */
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			if (ld->binding_point_id == i)
				ld_count++;
		}

		head->ld_count = ld_count;
		head->presentation = (ld_count > 1) ? FMD_HEAD_MLD : FMD_HEAD_SLD;

		fmd_dbg(ctx, "%s: head %d has %d LDs, presents as %s",
			mhd->base.id, i, ld_count,
			head->presentation == FMD_HEAD_MLD ? "MLD" : "SLD");
	}

	free(mh_rsp);
	free(head_rsp);

	return 0;
}


/*
 * Discover devices attached to a switch port
 *
 * The identify response is provided by the switch discovery - this function
 * does not send Identify itself.
 */
static int discover_port_device(struct fmd_ctx *ctx, struct fmd_port *port)
{
	char dev_id[FMD_MAX_ID_LEN];
	int rc;

	/*
	 * Generate device ID from parent switch and port number.
	 */
	(void)snprintf(dev_id, sizeof(dev_id), "%.32s_port%d_dev",
		       port->base.parent->id, port->port_number);

	switch (port->conn_dev_type) {
	case FMD_CONN_NONE:
		break;
	case FMD_CONN_PCIE:
		break;
	case FMD_CONN_TYPE1:
		break;
	case FMD_CONN_TYPE2:
		break;
	case FMD_CONN_SLD: {
		struct fmd_sld *sld = fmd_sld_create(dev_id);
		if (!sld)
			return -1;

		/*
		 * All the switch says of an SLD is that one is there. An SLD
		 * port is not a target of the Tunnel Management Command, which
		 * reaches "switch MLD Ports, valid LDs within an MLD, and the
		 * LD Pool CCI in a Multi-Headed device" (Section 7.6.7.3.1),
		 * so nothing can be asked of the device through the switch.
		 * If it has a CCI of its own it is discovered there, as a
		 * separate component: nothing either CCI reports ties the two
		 * together.
		 */
		rc = discovered_add(ctx, &sld->base, &port->base);
		if (rc)
			return rc;

		fmd_info(ctx, "%s: SLD %s on port %d: not manageable through "
			 "the switch, capacity unknown",
			 port->base.parent->id, dev_id, port->port_number);
		break;
	}

	case FMD_CONN_MLD: {
		struct cxlmi_cmd_identify_rsp identify = { 0 };
		struct fmd_mld *mld;

		/*
		 * A command tunnelled to an MLD port goes to the FM-owned LD:
		 * "This command instructs a switch to tunnel the provided
		 * command to the FM-owned LD of the MLD on the specified port"
		 * (Section 7.6.7.3.1). Its Identify
		 * gives the serial number that names the device, the same
		 * however it is reached. Without one it is named by its port.
		 */
		rc = cxlmi_cmd_identify(port->base.ep,
					CXLMI_TUNNEL_SWITCH(port->port_number),
					&identify);
		if (rc == 0 && identify.serial_num) {
			(void)snprintf(dev_id, sizeof(dev_id), "cxl_%016" PRIx64,
				       identify.serial_num);
			if (fmd_topology_find(&ctx->topology, dev_id)) {
				fmd_info(ctx, "%s: MLD %s on port %d is already known",
					 port->base.parent->id, dev_id,
					 port->port_number);
				break;
			}
		}

		mld = fmd_mld_create(dev_id);
		if (!mld)
			return -1;
		if (rc == 0)
			mld->identify = identify;

		/* MLD behind switch - store port for tunneling */
		mld->switch_port = port->port_number;
		mld->base.msg_limit = tunnelled_msg_limit(&port->base);

		rc = discovered_add(ctx, &mld->base, &port->base);
		if (rc)
			return rc;

		rc = discover_mld(ctx, mld);
		if (rc == 0)
			rc = fmd_create_mld_memory_resources(ctx, mld);
		if (rc == 0)
			fmd_dcd_discover(ctx, &mld->base);

		fmd_info(ctx, "%s: discovered MLD %s on port %d (%d LDs)",
			 port->base.parent->id, dev_id, port->port_number,
			 mld->ld_count);
		break;
	}

	case FMD_CONN_PBR:
		break;
	default:
		break;
	}

	return 0;
}

/*
 * The two bytes at 5h of a vPPB entry of Get Virtual CXL Switch Info, Table
 * 7-34, read as its Binding Status says:
 *  - 02h or 03h: "Bits[7:0]: vPPB[Start vPPB] Bound Port ID: Physical port
 *    number of the bound port. Valid only when Binding Status is 02h or
 *    03h."
 *  - "Bits[15:8]: vPPB[Start vPPB] Bound LD ID: LD-ID of the LD that is
 *    bound to the port from the MLD on an associated physical port. Valid
 *    only when vPPB[Start vPPB] Binding Status is 03h; otherwise, the value
 *    is FFh." It is kept as FFh for any other status, whatever the switch
 *    put there.
 *  - 04h, on a PBR switch: "Bits[11:0]: vPPB[Start vPPB] Bound PID: PID of
 *    the bound vPPB ... Bits[15:12]: Reserved." The bytes are no port and no
 *    LD then.
 * A Bound Port ID that is not valid is kept as 0 (cxl-fmd's choice).
 */
static void decode_vppb_binding(struct fmd_vppb *vppb,
				const struct cxlmi_cmd_fmapi_vppb_info *src)
{
	bool port_valid = src->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
			  src->binding_status == FMD_VPPB_BOUND_LD;

	vppb->bound_port_id = port_valid ? src->bound_port_id : 0;
	vppb->bound_ld_id = src->binding_status == FMD_VPPB_BOUND_LD ?
			    src->bound_ld_id : FMD_VPPB_NO_LD;
	vppb->bound_pbr_id = src->binding_status == FMD_VPPB_BOUND_PBR ?
			     (uint16_t)(src->bound_port_id |
					(src->bound_ld_id & 0x0f) << 8) : 0;
}

/*
 * Get Virtual CXL Switch Info (5200h) for one VCS, CXL r4.0 Tables 7-32 to
 * 7-34. Returns 0 with @vcs filled in, -ENOENT if the switch says the VCS ID
 * is invalid, -EIO if it could not be asked.
 */
static int query_vcs(struct fmd_ctx *ctx, struct fmd_switch *sw, uint8_t vcs_id,
		     struct fmd_vcs *vcs)
{
	struct cxlmi_cmd_fmapi_get_vcs_info_req *req;
	struct cxlmi_cmd_fmapi_get_vcs_info_rsp *rsp;
	struct cxlmi_cmd_fmapi_vcs_info_block *block;
	int total = FMD_MAX_VPPB, got = 0, rc = 0;

	req = calloc(1, sizeof(*req) + 1);
	rsp = calloc(1, sizeof(*rsp) + sizeof(*block) +
		     FMD_MAX_VPPB * sizeof(struct cxlmi_cmd_fmapi_vppb_info));
	if (!req || !rsp) {
		free(req);
		free(rsp);
		return -ENOMEM;
	}
	block = &rsp->vcs_info_list[0];

	/*
	 * Start vPPB and vPPB List Limit are there for "devices that have
	 * small maximum command message sizes": read the vPPBs as many at a
	 * time as fit a response. The list holds min(vPPB List Limit, Number
	 * of vPPBs) entries from Start vPPB on.
	 */
	while (got < total) {
		int want = fmd_cci_list_fit(&sw->base, 4 + sizeof(*block),
					    sizeof(block->vppbs[0]), total - got);
		int count;

		req->start_vppb = got;
		req->vppb_list_limit = want;
		req->num_vcs = 1;
		req->vcs_id_list[0] = vcs_id;

		rc = cxlmi_cmd_fmapi_get_vcs_info(sw->base.ep, CXLMI_NO_TUNNEL, req, rsp);
		if (rc || rsp->num_vcs == 0) {
			fmd_dbg(ctx, "%s: VCS %d query failed", sw->base.id, vcs_id);
			rc = -EIO;
			break;
		}

		/* FFh: Invalid VCS ID, all subsequent field values are invalid */
		if (block->vcs_state == 0xff) {
			rc = -ENOENT;
			break;
		}

		if (got == 0) {
			memset(vcs, 0, sizeof(*vcs));
			vcs->vcs_id = block->vcs_id;
			vcs->state = block->vcs_state;
			vcs->upstream_port_id = block->usp_id;
			vcs->hdm_decoders = sw->sw_identify.num_hdm_decoder_per_usp;
			if (block->num_vppbs < total)
				total = block->num_vppbs;
			else if (block->num_vppbs > total)
				fmd_warn(ctx, "%s: VCS %d has %d vPPBs, only the "
					 "first %d are managed", sw->base.id,
					 vcs_id, block->num_vppbs, total);
		}

		count = total - got < want ? total - got : want;
		for (int i = 0; i < count; i++) {
			struct cxlmi_cmd_fmapi_vppb_info *src = &block->vppbs[i];
			struct fmd_vppb *vppb = &vcs->vppbs[vcs->num_vppbs++];

			vppb->vppb_id = got + i;
			vppb->binding_status = src->binding_status;
			decode_vppb_binding(vppb, src);
		}
		got += count;
		if (count == 0)
			break;
	}

	free(req);
	free(rsp);
	if (rc)
		return rc;

	fmd_dbg(ctx, "%s: VCS %d state=%d USP=%d vPPBs=%d",
		sw->base.id, vcs->vcs_id, vcs->state,
		vcs->upstream_port_id, vcs->num_vppbs);
	return 0;
}

struct fmd_vcs *fmd_switch_find_vcs(struct fmd_switch *sw, uint8_t vcs_id)
{
	for (int i = 0; i < sw->num_vcs; i++) {
		if (sw->vcs[i].vcs_id == vcs_id)
			return &sw->vcs[i];
	}
	return NULL;
}

struct fmd_vppb *fmd_vcs_find_vppb(struct fmd_vcs *vcs, uint8_t vppb_id)
{
	for (int i = 0; i < vcs->num_vppbs; i++) {
		if (vcs->vppbs[i].vppb_id == vppb_id)
			return &vcs->vppbs[i];
	}
	return NULL;
}

/*
 * Read the VCSs of a switch: every VCS set in the Active VCS Bitmask of
 * Identify Switch Device.
 *
 * The table is replaced only once it has been read. A VCS that cannot be
 * asked about keeps what was last known of it, so a failed command does not
 * make its bindings look unbound, and one the switch calls invalid goes.
 * The table is in the order the VCSs were read, not indexed by VCS ID: use
 * fmd_switch_find_vcs().
 */
int fmd_vcs_refresh(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	struct fmd_vcs *table;
	int n = 0;

	if (!ctx || !sw)
		return -EINVAL;
	if (!sw->base.ep) {
		/* mock: the table is all there is */
		fmd_binding_marks_prune(ctx, sw->base.id);
		return 0;
	}

	table = calloc(FMD_MAX_VCS, sizeof(*table));
	if (!table)
		return -ENOMEM;

	/* every bit of the Active VCS Bitmask is a VCS ID */
	for (int id = 0; id < 256 && n < FMD_MAX_VCS; id++) {
		struct fmd_vcs *old;
		int rc;

		if (!(sw->sw_identify.active_vcs_bitmask[id / 8] & (1 << (id % 8))))
			continue;

		rc = query_vcs(ctx, sw, id, &table[n]);
		if (rc == -ENOENT)
			continue;
		if (rc) {
			old = fmd_switch_find_vcs(sw, id);
			if (!old)
				continue;
			table[n] = *old;
		}
		n++;
	}

	/* an active VCS that was passed over because there was no room */
	for (int id = 0; id < 256; id++) {
		bool have = false;

		if (!(sw->sw_identify.active_vcs_bitmask[id / 8] & (1 << (id % 8))))
			continue;
		for (int i = 0; i < n; i++)
			have |= table[i].vcs_id == id;
		if (!have && n == FMD_MAX_VCS) {
			fmd_warn(ctx, "%s: VCS %d is active and is not managed "
				 "(at most %d VCSs are)", sw->base.id, id,
				 FMD_MAX_VCS);
			break;
		}
	}

	memcpy(sw->vcs, table, sizeof(sw->vcs));
	sw->num_vcs = n;
	free(table);

	/* the port and LD each bound vPPB points at */
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
	/* bindings this run made that the vPPBs no longer hold */
	fmd_binding_marks_prune(ctx, sw->base.id);

	return 0;
}


/*
 * Discover switch - enumerate ports and devices attached to them
 *
 * 1. Identify Switch Device (5100h) - port count, enabled ports/VCSs
 * 2. Get Physical Port State (5101h) - for each enabled port
 * 3. Get Virtual CXL Switch Info (5200h) - for each enabled VCS
 */
/* A port of a switch, from its Get Physical Port State information block */
/*
 * The device on a switch port is no longer there: when the port is read
 * again, it reports no device, or another kind of one. Deciding that from
 * Connected Device Type is cxl-fmd's; the specification does not say what
 * a port reports after a removal.
 *
 * How the FM learns of it depends on the port (CXL r4.0): "When a device
 * is Hot-Removed from an unbound port on a switch ... The switch notifies
 * the FM by generating Physical Switch Event Records" (Section 7.6.6.7);
 * of a surprise removal, "The switch notifies the FM of a surprise removal
 * by generating Virtual CXL Switch Event Records" (Section 7.6.6.8); of an
 * SLD or PCIe device on a bound port, "the FM can be notified but is not
 * involved" (Section 7.6.6.7), and it may hear of it only when the port is
 * read again on the --rescan timer.
 *
 * It goes, with its LDs, memory domains and chunks, its dynamic capacity
 * and the event records kept for it.
 *
 * The connections to it are the switch's to decide: a connection is a vPPB
 * bound for a host. For an SLD on a bound port, neither section says what
 * becomes of the binding; a vPPB bound to an SLD port "behaves the same as
 * a PPB in a PCIe switch" (Section 7.2.3.1), so cxl-fmd assumes that it
 * stays bound to the port, and that a device plugged back into it is the
 * host's again. On a surprise removal of an MLD, "The switch will
 * automatically unbind any existing LD bindings" (Section 7.6.6.8). On a
 * managed hot-removal of an MLD, "The FM unbinds each assigned LD with the
 * Unbind vPPB command as it receives notifications from each host"
 * (Section 7.6.6.7), which cxl-fmd does not do (a known gap). So the
 * vPPBs are read again after the ports: a Connection is a vPPB binding, and
 * one whose device has gone stands, Absent, for as long as its vPPB is
 * bound (see fmd_conn_for_each()).
 */
void fmd_switch_port_device_removed(struct fmd_ctx *ctx, struct fmd_switch *sw,
				    struct fmd_port *port, struct fmd_component *dev)
{
	fmd_info(ctx, "%s: port %d no longer has %s, which is removed",
		 sw->base.id, port->port_number, dev->id);

	fmd_topology_remove(&ctx->topology, dev);

	/* no vPPB points at what has gone */
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
}

/* The kind of device a memory device component is, as a port reports it */
static enum fmd_conn_dev_type memdev_conn_type(struct fmd_component *dev)
{
	return dev->type == FMD_COMPONENT_MLD ? FMD_CONN_MLD : FMD_CONN_SLD;
}

/*
 * A port of a switch, from its Get Physical Port State information block:
 * a port that is not known yet is added, and one that is has its state
 * brought up to date. A device that has appeared on it is discovered, and
 * one that is no longer there is removed.
 */
static void add_switch_port(struct fmd_ctx *ctx, struct fmd_switch *sw,
			    struct cxlmi_cmd_fmapi_port_state_info_block *pinfo)
{
	uint8_t state = pinfo->config_state & 0xf;
	struct fmd_component *child, *dev = NULL;
	char port_id[128];
	struct fmd_port *port = NULL;
	bool known;

	/* Fh: Invalid Port_ID. There is no such port */
	if (state == 0xf)
		return;

	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		if (fmd_to_port(child)->port_number == pinfo->port_id) {
			port = fmd_to_port(child);
			break;
		}
	}
	known = port != NULL;

	if (!known) {
		/* a USP is named as one, as the mock topologies name theirs */
		(void)snprintf(port_id, sizeof(port_id), "%.50s_%s%d", sw->base.id,
			       state == 0x4 ? "usp" : "dsp", pinfo->port_id);

		port = fmd_port_create(port_id, pinfo->port_id);
		if (!port)
			return;
	} else {
		port->conn_dev_cxl_ver = 0;
		port->conn_dev_type = FMD_CONN_NONE;
	}

	/*
	 * The port information block of Table 7-21. The widths and speeds
	 * are Bits[5:0] of their bytes, encoded as in the PCIe Link
	 * Capabilities, Link Capabilities 2 and Link Status registers.
	 */
	port->link_status = ltssm_to_link_status(pinfo->ltssm_state);
	port->port_cxl_ver_bitmask = pinfo->port_cxl_ver_bitmask;
	port->max_link_width = pinfo->max_link_width & 0x3f;
	port->negotiated_link_width = pinfo->negotiated_link_width & 0x3f;
	port->supported_link_speeds = pinfo->supported_link_speeds_vector & 0x3f;
	port->max_link_speed = pinfo->max_link_speed & 0x3f;
	port->current_link_speed = pinfo->current_link_speed & 0x3f;
	port->supported_ld_count = pinfo->supported_ld_count;

	/*
	 * Connected Device Mode is reserved unless the port is a DSP, and
	 * Connected Device Type unless it is a DSP or a Fabric Port.
	 */
	if (state == 0x3)
		port->conn_dev_cxl_ver = pinfo->conn_dev_cxl_ver & 0xf;
	if (state == 0x3 || state == 0x5)
		port->conn_dev_type = pinfo->conn_dev_type;

	/*
	 * Current Port Configuration State is Bits[3:0]; Bit[4] is GAE
	 * Support. A port that is not a DSP or a USP must not be left
	 * in the state fmd_port_create() starts it in, which is DSP.
	 */
	switch (state) {
	case 0x1: /* Bind in progress */
		port->state = FMD_PORT_BIND_IN_PROGRESS;
		break;
	case 0x2: /* Unbind in progress */
		port->state = FMD_PORT_UNBIND_IN_PROGRESS;
		break;
	case 0x3: /* DSP */
		port->state = FMD_PORT_DSP;
		break;
	case 0x4: /* USP */
		port->state = FMD_PORT_USP;
		break;
	case 0x0: /* Disabled */
		port->state = FMD_PORT_DISABLED;
		break;
	case 0x5: /* Fabric Port of a PBR switch */
		port->state = FMD_PORT_FABRIC;
		break;
	default:  /* reserved */
		port->state = FMD_PORT_RESERVED;
		break;
	}

	/* Add to topology */
	if (!known && discovered_add(ctx, &port->base, &sw->base))
		return;

	fmd_dbg(ctx, "%s: port %d conn_type=%d link=%s width=%d",
		sw->base.id, port->port_number,
		port->conn_dev_type,
		port->link_status == FMD_LINK_UP ? "up" : "down",
		port->negotiated_link_width);

	fmd_for_each_child(&port->base, child) {
		if (fmd_is_memdev(child)) {
			dev = child;
			break;
		}
	}

	/*
	 * Connected Device Type "is reserved for all values of Current Port
	 * Configuration State except 3h (DSP) or 5h (Fabric Port)" (Table
	 * 7-21). Only a DSP can have a memory device on it, so only DSPs are
	 * checked for removals. One that reports no device where there was
	 * one, or another kind of device ("A bound port will not advertise
	 * support for MLDs during negotiation, so MLD components will link up
	 * as an SLD"), no longer has the one that was there. A link that is
	 * down with the device still detected, a surprise Link Down, is not a
	 * removal: "the presence pin ... will remain asserted".
	 */
	if (port->state != FMD_PORT_DSP || port->head)
		return;

	if (dev && port->conn_dev_type != memdev_conn_type(dev)) {
		fmd_switch_port_device_removed(ctx, sw, port, dev);
		dev = NULL;
	}

	/* a device on a port that has none is discovered */
	if (!dev)
		discover_port_device(ctx, port);
}

/*
 * Get Physical Port State (5101h) for @nports ports of a switch, as many at
 * a time as fit a request and its response, and bring each of them up to
 * date: see add_switch_port().
 */
static int read_switch_ports(struct fmd_ctx *ctx, struct fmd_switch *sw,
			     const uint8_t *port_ids, int nports)
{
	struct cxlmi_cmd_fmapi_get_phys_port_state_req *port_req;
	struct cxlmi_cmd_fmapi_get_phys_port_state_rsp *port_rsp;
	struct fmd_component *child;
	int rc = 0;

	port_req = calloc(1, sizeof(*port_req) + FMD_CCI_LIST_MAX);
	port_rsp = calloc(1, sizeof(*port_rsp) + FMD_CCI_LIST_MAX *
			  sizeof(struct cxlmi_cmd_fmapi_port_state_info_block));
	if (!port_req || !port_rsp) {
		free(port_req);
		free(port_rsp);
		return -1;
	}

	for (int done = 0; done < nports; ) {
		int want = fmd_cci_list_fit(&sw->base, 4,
				sizeof(struct cxlmi_cmd_fmapi_port_state_info_block),
				nports - done);

		port_req->num_ports = want;
		memcpy(port_req->ports, port_ids + done, want);
		memset(port_rsp, 0, sizeof(*port_rsp));

		rc = cxlmi_cmd_fmapi_get_phys_port_state(sw->base.ep, CXLMI_NO_TUNNEL,
							 port_req, port_rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get physical port state failed: %d",
				 sw->base.id, rc);
			/* the ports read so far are still ports of the switch */
			if (done)
				rc = 0;
			break;
		}
		FMD_CCI_LIST_CLAMP(port_rsp->num_ports, want);

		for (int i = 0; i < port_rsp->num_ports; i++)
			add_switch_port(ctx, sw, &port_rsp->ports[i]);
		done += want;
	}

	free(port_req);
	free(port_rsp);

	/* what the ports are now */
	sw->num_dsps = sw->num_usps = 0;
	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		if (fmd_to_port(child)->state == FMD_PORT_DSP)
			sw->num_dsps++;
		else if (fmd_to_port(child)->state == FMD_PORT_USP)
			sw->num_usps++;
	}

	return rc;
}

/*
 * Read some ports of a switch again, the ones an event said have changed,
 * and its VCSs. Called with fabric_lock held exclusively.
 */
int fmd_switch_refresh_ports(struct fmd_ctx *ctx, struct fmd_switch *sw,
			     const uint8_t *port_ids, int nports)
{
	int rc;

	if (!sw->base.ep || nports <= 0)
		return 0;

	rc = read_switch_ports(ctx, sw, port_ids, nports);
	fmd_vcs_refresh(ctx, sw);
	return rc;
}

static int discover_switch(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	struct cxlmi_endpoint *ep;
	uint8_t port_ids[256];
	int rc;
	int num_ports;
	int nports;

	ep = sw->base.ep;
	if (!ep)
		return -1;

	/* FM-API Identify Switch Device (5100h) */
	rc = cxlmi_cmd_fmapi_identify_sw_device(ep, CXLMI_NO_TUNNEL, &sw->sw_identify);
	if (rc) {
		fmd_warn(ctx, "%s: FM-API switch identify failed: %d", sw->base.id, rc);
		return rc;
	}

	sw->max_vcs = sw->sw_identify.num_vcs;
	num_ports = sw->sw_identify.num_physical_ports;
	fmd_info(ctx, "%s: switch with %d ports, %d VCS(s), %d vPPBs",
		 sw->base.id, num_ports,
		 sw->sw_identify.num_vcs, sw->sw_identify.num_total_vppb);

	/*
	 * Get Physical Port State (5101h) for the ports in the Active Port
	 * Bitmask.
	 */
	nports = 0;
	for (int id = 0; id < 256 && nports < num_ports; id++) {
		if (sw->sw_identify.active_port_bitmask[id / 8] & (1 << (id % 8)))
			port_ids[nports++] = id;
	}

	rc = read_switch_ports(ctx, sw, port_ids, nports);
	if (rc)
		return rc;

	/* Step 3: Discover VCS configuration */
	fmd_vcs_refresh(ctx, sw);

	/*
	 * Step 4: Create host entries for each VCS
	 *
	 * Each VCS represents a host connection via its USP (Upstream Switch Port).
	 * Per OCP CMS model, hosts are represented as Systems with Fabric Endpoints.
	 * A VCS that is not enabled has none: its USP ID is "Valid only when
	 * the VCS is enabled" (Table 7-34), as for a mock switch.
	 */
	for (int vcs_idx = 0; vcs_idx < sw->num_vcs; vcs_idx++) {
		struct fmd_vcs *vcs = &sw->vcs[vcs_idx];
		char host_id[FMD_MAX_ID_LEN];
		struct fmd_host *host;

		if (vcs->state != 0x01)		/* VCS State 01h: Enabled */
			continue;

		snprintf(host_id, sizeof(host_id), "%.50s_host%d", sw->base.id, vcs->vcs_id);

		/* the host of a VCS that was there already */
		if (fmd_topology_find_host(&ctx->topology, host_id))
			continue;

		host = fmd_host_create(host_id);
		if (host) {
			snprintf(host->name, sizeof(host->name), "Host via %.46s VCS %d",
				 sw->base.id, vcs->vcs_id);
			snprintf(host->switch_id, sizeof(host->switch_id), "%s",
				 sw->base.id);
			host->vcs_id = vcs->vcs_id;
			host->usp_port_id = vcs->upstream_port_id;
			host->direct_attached = false;  /* Switch-attached */

			if (discovered_add(ctx, &host->base, NULL))
				continue;

			fmd_dbg(ctx, "Created host %s for VCS %d USP port %d",
				host_id, vcs->vcs_id, vcs->upstream_port_id);
		}
	}

	/* Resolve vPPB port and LD pointers now that all devices are discovered */
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);

	return 0;
}


/*
 * Discover a single MCTP endpoint
 *
 * Sends Identify command, then dispatches to appropriate discovery function
 * based on component type.
 */
/*
 * The Message Payload limit of a component on its own endpoint: the smaller
 * of what it accepts in a request, from Identify, and what it sends in a
 * response, from Get Response Message Limit (0003h). A component that will
 * not say is held to the 256 bytes all of them support.
 */
static uint16_t endpoint_msg_limit(struct cxlmi_endpoint *ep,
				   const struct cxlmi_cmd_identify_rsp *identify)
{
	struct cxlmi_cmd_get_response_msg_limit_rsp rsp = { 0 };
	unsigned int req_n = identify->max_msg_size, rsp_n = 8;

	if (cxlmi_cmd_get_response_msg_limit(ep, NULL, &rsp) == 0)
		rsp_n = rsp.limit;

	if (req_n < 8 || req_n > 10)	/* 2^8 to 2^10; anything else is not a size */
		req_n = 8;
	if (rsp_n < 8 || rsp_n > 10)
		rsp_n = 8;

	return 1 << (req_n < rsp_n ? req_n : rsp_n);
}

/*
 * Discover the component on an endpoint that has answered Identify.
 * Returns 0 if it was added, 1 if it is one that is already known, and a
 * negative value or a CCI return code if it could not be discovered.
 */
static int discover_endpoint(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
			     const struct cxlmi_cmd_identify_rsp *id, int index)
{
	struct cxlmi_cmd_identify_rsp identify = *id;
	char dev_id[FMD_MAX_ID_LEN];
	uint16_t msg_limit;
	int rc;

	/*
	 * The Device Serial Number of Identify is the "unique identifier for
	 * this device" (Table 8-216), and names the component. A device can
	 * be reached more than one way: on two interconnects, by MCTP and by
	 * its mailbox, or as the MLD on a switch port and on its own. It is
	 * one component however it is reached, and the first way stands.
	 *
	 * A serial number of 0 identifies nothing. Such a device is named
	 * after its endpoint, and is never taken for another.
	 */
	if (identify.serial_num) {
		(void)snprintf(dev_id, sizeof(dev_id), "cxl_%016" PRIx64,
			       identify.serial_num);
		if (fmd_topology_find(&ctx->topology, dev_id)) {
			fmd_info(ctx, "%s: reached on another endpoint as well, "
				 "discovered once", dev_id);
			return 1;
		}
	} else {
		(void)snprintf(dev_id, sizeof(dev_id), "cxl_ep%d", index);
	}

	msg_limit = endpoint_msg_limit(ep, &identify);

	fmd_info(ctx, "Discovered endpoint %s: vendor=%04x device=%04x type=%d",
		 dev_id, identify.vendor_id, identify.device_id,
		 identify.component_type);

	switch (identify.component_type) {
	case CXL_COMPONENT_SWITCH: {
		struct fmd_switch *sw = fmd_switch_create(dev_id);
		if (!sw)
			return -1;

		sw->identify = identify;
		sw->base.ep = ep;
		sw->base.msg_limit = msg_limit;
		rc = discovered_add(ctx, &sw->base, NULL);
		if (rc)
			return rc;

		/*
		 * Have it say when something changes before it is read, so
		 * that nothing changes unseen between the two.
		 */
		fmd_switch_events_enable(ctx, sw);
		rc = discover_switch(ctx, sw);
		break;
	}

	case CXL_COMPONENT_TYPE3: {
		/*
		 * Direct-attached Type 3 - determine device type:
		 * 1. Try Get Multi-Headed Info (5500h) - if num_heads > 1, it's MHD
		 * 2. Otherwise, try Get LD Info (5400h) - an MLD answers it
		 * 3. Otherwise it's an SLD
		 */
		struct cxlmi_cmd_fmapi_get_multiheaded_info_req mh_req = {0};
		union {
			struct cxlmi_cmd_fmapi_get_multiheaded_info_rsp rsp;
			uint8_t room[sizeof(struct cxlmi_cmd_fmapi_get_multiheaded_info_rsp) +
				     FMD_CCI_LIST_MAX];
		} mh = { 0 };
		struct cxlmi_cmd_fmapi_get_ld_info_rsp ld_info = {0};

		/*
		 * Try MHD detection first. Only the number of heads is wanted,
		 * but an LD Map List Limit of 0 is not a request for no
		 * entries: the field shall be at least 1 (CXL r4.0 Table
		 * 7-60), and libcxlmi expects one entry back.
		 */
		mh_req.start_ld_id = 0;
		mh_req.ld_map_list_limit = 1;
		rc = cxlmi_cmd_fmapi_get_multiheaded_info(ep, CXLMI_NO_TUNNEL,
							  &mh_req, &mh.rsp);

		if (rc == 0 && mh.rsp.num_heads > 1) {
			/* Multi-Headed Device */
			struct fmd_mhd *mhd = fmd_mhd_create(dev_id);
			if (!mhd)
				return -1;

			mhd->identify = identify;
			mhd->base.ep = ep;
			mhd->base.msg_limit = msg_limit;
			rc = discovered_add(ctx, &mhd->base, NULL);
			if (rc)
				return rc;

			rc = discover_mhd(ctx, mhd);
			if (rc == 0)
				rc = fmd_create_mhd_memory_resources(ctx, mhd);
			if (rc == 0)
				fmd_dcd_discover(ctx, &mhd->base);
			fmd_device_events_enable(ctx, &mhd->base);

			fmd_info(ctx, "%s: MHD with %d heads, %d LDs",
				 dev_id, mhd->num_heads, mhd->total_lds);
			break;
		}

		/*
		 * Not multi-headed. Get LD Info is mandatory for an MLD and
		 * prohibited for an SLD (Table 8-397, MM and MMS): a device
		 * that answers it is an MLD, however many LDs it supports.
		 */
		rc = cxlmi_cmd_fmapi_get_ld_info(ep, CXLMI_NO_TUNNEL, &ld_info);
		if (rc) {
			/* SLD - direct-attached, no tunneling ever */
			struct fmd_sld *sld = fmd_sld_create(dev_id);
			if (!sld)
				return -1;

			sld->identify = identify;
			sld->base.ep = ep;
			sld->base.msg_limit = msg_limit;
			rc = discovered_add(ctx, &sld->base, NULL);
			if (rc)
				return rc;

			rc = discover_sld(ctx, sld);
			if (rc == 0)
				rc = fmd_create_sld_memory_resources(ctx, sld);
			if (rc == 0)
				fmd_dcd_discover(ctx, &sld->base);
			fmd_device_events_enable(ctx, &sld->base);

			fmd_info(ctx, "%s: SLD with %" PRIu64 " MB",
				 dev_id, (uint64_t)(sld->memory_size / (1024ULL * 1024)));
		} else {
			/* FM-owned LD on a type 3 MLD directly attached */
			struct fmd_mld *mld = fmd_mld_create(dev_id);
			if (!mld)
				return -1;

			mld->identify = identify;
			mld->switch_port = -1;  /* Direct-attached */
			mld->base.ep = ep;
			mld->base.msg_limit = msg_limit;
			rc = discovered_add(ctx, &mld->base, NULL);
			if (rc)
				return rc;

			rc = discover_mld(ctx, mld);
			if (rc == 0)
				rc = fmd_create_mld_memory_resources(ctx, mld);
			if (rc == 0)
				fmd_dcd_discover(ctx, &mld->base);
			fmd_device_events_enable(ctx, &mld->base);

			fmd_info(ctx, "%s: MLD with %d LDs",
				 dev_id, mld->ld_count);
		}
		break;
	}

	case CXL_COMPONENT_GFD:
		fmd_warn(ctx, "%s: GFD not supported, skipping", dev_id);
		return 0;

	default:
		fmd_warn(ctx, "%s: unknown component type %d, skipping",
			 dev_id, identify.component_type);
		return 0;
	}

	return rc;
}

/* The endpoints that have been discovered, or found to be a known device */
static bool endpoint_known(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	for (int i = 0; i < ctx->nknown_eps; i++) {
		if (ctx->known_eps[i] == ep)
			return true;
	}
	return false;
}

static void endpoint_mark_known(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	struct cxlmi_endpoint **tmp = realloc(ctx->known_eps,
			(ctx->nknown_eps + 1) * sizeof(*tmp));

	if (!tmp)
		return;
	ctx->known_eps = tmp;
	ctx->known_eps[ctx->nknown_eps++] = ep;
}

static void endpoint_mark_ioctl(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	struct cxlmi_endpoint **tmp = realloc(ctx->ioctl_eps,
			(ctx->nioctl_eps + 1) * sizeof(*tmp));

	if (!tmp)
		return;
	ctx->ioctl_eps = tmp;
	ctx->ioctl_eps[ctx->nioctl_eps++] = ep;
}

bool fmd_endpoint_owns_events(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	if (!ep)
		return false;
	for (int i = 0; i < ctx->nioctl_eps; i++) {
		if (ctx->ioctl_eps[i] == ep)
			return false;
	}
	return true;
}

/* Look for endpoints. libcxlmi does not open one that is open already */
static void scan_endpoints(struct fmd_ctx *ctx)
{
	int rc;

	/* Scan for MCTP endpoints */
	if (ctx->config.scan_mctp) {
		fmd_dbg(ctx, "Scanning MCTP for CXL devices...");
		rc = cxlmi_scan_mctp(ctx->cxlmi);
		if (rc < 0) {
			fmd_warn(ctx, "MCTP scan failed (is mctpd running?)");
		} else {
			fmd_dbg(ctx, "MCTP scan found %d endpoint(s)", rc);
		}
	}

	/* Scan for ioctl endpoints */
	if (ctx->config.scan_ioctl) {
		struct cxlmi_endpoint *ep, **before = NULL;
		int nbefore = 0;

		/* what was open before: the rest is what the scan opened */
		cxlmi_for_each_endpoint(ctx->cxlmi, ep)
			nbefore++;
		before = calloc(nbefore ? nbefore : 1, sizeof(*before));
		nbefore = 0;
		if (before) {
			cxlmi_for_each_endpoint(ctx->cxlmi, ep)
				before[nbefore++] = ep;
		}

		fmd_dbg(ctx, "Scanning /dev/cxl/ for CXL devices...");
		rc = cxlmi_scan(ctx->cxlmi);
		if (rc < 0) {
			fmd_warn(ctx, "ioctl scan failed");
		} else {
			fmd_dbg(ctx, "ioctl scan found %d endpoint(s)", rc);
		}

		cxlmi_for_each_endpoint(ctx->cxlmi, ep) {
			bool old = false;

			for (int i = 0; i < nbefore && !old; i++)
				old = before[i] == ep;
			if (!old)
				endpoint_mark_ioctl(ctx, ep);
		}
		free(before);
	}
}

/*
 * Discover the endpoints that have not been. An endpoint whose Identify
 * fails stays unknown and is asked again the next time: "Retry Required"
 * is how a component says it is not ready yet.
 */
static int discover_new_endpoints(struct fmd_ctx *ctx)
{
	struct {
		struct cxlmi_endpoint *ep;
		struct cxlmi_cmd_identify_rsp identify;
		int rc;
	} *found;
	struct cxlmi_endpoint *ep;
	int nfound = 0;
	int count = 0;
	int rc;

	/*
	 * Identify (0001h) every endpoint, then discover the switches before
	 * anything else: a switch says what is on its ports, and a device
	 * found there is placed in the fabric, which the same device seen on
	 * an endpoint of its own is not.
	 */
	cxlmi_for_each_endpoint(ctx->cxlmi, ep)
		nfound++;
	found = calloc(nfound ? nfound : 1, sizeof(*found));
	if (!found)
		return -1;

	nfound = 0;
	cxlmi_for_each_endpoint(ctx->cxlmi, ep) {
		if (endpoint_known(ctx, ep))
			continue;
		found[nfound].ep = ep;
		found[nfound].rc = cxlmi_cmd_identify(ep, NULL, &found[nfound].identify);
		if (found[nfound].rc)
			fmd_err(ctx, "Identify command failed: %d", found[nfound].rc);
		nfound++;
	}

	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < nfound; i++) {
			bool is_switch = found[i].identify.component_type ==
					 CXL_COMPONENT_SWITCH;

			if (found[i].rc || is_switch != (pass == 0))
				continue;
			/* numbered in the order they are discovered */
			rc = discover_endpoint(ctx, found[i].ep,
					       &found[i].identify, ctx->nknown_eps);
			endpoint_mark_known(ctx, found[i].ep);
			if (rc == 0)
				count++;
		}
	}
	free(found);

	return count;
}

/*
 * Bring the topology up to date with what is there now, adding and never
 * removing.
 *
 * "When a device is Hot-Added to an unbound port on a switch, the FM
 * receives a notification ... FM issues the Get Physical Port State
 * command for the physical port that has linked up to discover the
 * connected device type" (CXL r4.0 Section 7.6.6.7). So, for every switch:
 * its ports are read again, a port it did not have is added, the state of
 * the others is brought up to date, and a device that has appeared on one
 * is discovered. Its VCSs are read again, and a VCS it did not have gets
 * its host. Endpoints that have appeared, or that were not ready, are
 * discovered as at start-up.
 *
 * A device that a port no longer reports is removed, with what it had:
 * see fmd_switch_port_device_removed(). Requests hold pointers into the topology only
 * while they hold fabric_lock, which this holds exclusively.
 *
 * Returns how many components were added.
 */
/* Everything a switch says of itself, again. fabric_lock held exclusively */
int fmd_switch_rediscover(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	return sw->base.ep ? discover_switch(ctx, sw) : 0;
}

int fmd_rescan(struct fmd_ctx *ctx)
{
	struct fmd_component *comp;
	struct fmd_switch **switches;
	struct fmd_fabric *root;
	unsigned int added, removed;
	int n = 0;

	if (!ctx || !ctx->cxlmi)
		return -1;

	pthread_rwlock_wrlock(&ctx->fabric_lock);
	root = ctx->topology.root;
	added = ctx->topology.added;
	removed = ctx->topology.removed;

	/* the switches there are now: new ones are discovered whole below */
	switches = calloc(root->switch_count ? root->switch_count : 1,
			  sizeof(*switches));
	if (!switches) {
		pthread_rwlock_unlock(&ctx->fabric_lock);
		return -1;
	}
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
		if (comp->ep && n < root->switch_count)
			switches[n++] = fmd_to_switch(comp);
	}

	for (int i = 0; i < n; i++)
		discover_switch(ctx, switches[i]);
	free(switches);

	scan_endpoints(ctx);
	discover_new_endpoints(ctx);

	added = ctx->topology.added - added;
	removed = ctx->topology.removed - removed;
	if (added || removed)
		fmd_info(ctx, "Re-scan: %u component(s) added, %u removed: "
			 "%d switch(es), %d device(s), %d LD(s), %d host(s)",
			 added, removed, root->switch_count, root->device_count,
			 root->ld_count, root->host_count);
	pthread_rwlock_unlock(&ctx->fabric_lock);

	return added;
}

/*
 * Main discovery entry point
 */
int fmd_discover(struct fmd_ctx *ctx)
{
	int count;

	if (!ctx || !ctx->cxlmi)
		return -1;

	scan_endpoints(ctx);
	count = discover_new_endpoints(ctx);
	if (count < 0)
		return -1;

	/*
	 * If no hosts were discovered (no switches with VCS), create a default
	 * local host. This represents the system running cxl-fmd for direct-
	 * attached devices.
	 */
	if (ctx->topology.root->host_count == 0 &&
	    ctx->topology.root->device_count > 0) {
		struct fmd_host *local_host = fmd_host_create("local");
		if (local_host) {
			snprintf(local_host->name, sizeof(local_host->name),
				 "Local System");
			local_host->direct_attached = true;
			if (!discovered_add(ctx, &local_host->base, NULL))
				fmd_dbg(ctx, "Created default local host for direct-attached devices");
		}
	}

	fmd_info(ctx, "Discovery complete: %d switch(es), %d device(s), %d LD(s), %d host(s)",
		 ctx->topology.root->switch_count,
		 ctx->topology.root->device_count,
		 ctx->topology.root->ld_count,
		 ctx->topology.root->host_count);

	return count;
}


static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Mock: whether the switch has a bind or unbind under way. A component runs
 * one background command at a time, and answers another with Busy.
 */
static bool mock_bg_busy(struct fmd_switch *sw)
{
	for (int v = 0; v < sw->num_vcs; v++) {
		for (int i = 0; i < sw->vcs[v].num_vppbs; i++) {
			if (sw->vcs[v].vppbs[i].mock_done_ms)
				return true;
		}
	}
	return false;
}

void fmd_mock_binding_record(struct fmd_switch *sw, int which, uint8_t vcs_id,
			     const struct fmd_vppb *vppb)
{
	struct cxlmi_event_record rec = { 0 };
	struct timespec ts;

	fmd_event_record_kind_from_name("virtual_switch", rec.uuid);
	rec.length = 0x80;
	rec.flags[0] = (uint8_t)which;		/* Event Record Severity */
	rec.handle = (uint16_t)(0x8000 | ++sw->mock_record_handle);
	clock_gettime(CLOCK_REALTIME, &ts);
	rec.timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	rec.data[0] = vcs_id;			/* 30h VCS ID */
	rec.data[1] = vppb->vppb_id;		/* 31h vPPB ID */
	rec.data[2] = 0x00;			/* 32h Event Type: Binding Change */
	rec.data[3] = vppb->binding_status;	/* 33h vPPB Binding Status */
	rec.data[4] = vppb->bound_port_id;	/* 34h vPPB Port ID */
	rec.data[5] = vppb->bound_ld_id;	/* 35h vPPB LD ID */
	fmd_log_add(&sw->base, which, &rec);
}

/*
 * Mock: how the mock switch answers a Bind vPPB or Unbind vPPB
 * (@opcode) of @vppb, given its "bind_result" or "unbind_result":
 *  - busy: Busy, at once, as a switch running another background command
 *    does; unsupported: Unsupported, at once. Each is then what the same
 *    return code from a switch is.
 *  - invalid_input, internal_error: Background Command Started. When it is
 *    over the vPPB is as it was, a Binding Change goes to the Warning log
 *    ("Failed bind and unbind operations generate events to the Warning
 *    Event Log", Table 7-80), and Background Operation Status has the
 *    return code. Even with no bind_time_ms: the outcome is in 0002h.
 *  - success, with bind_time_ms: Background Command Started, and the vPPB
 *    becomes @next, @port, @ld when it is over.
 * Returns FMD_OP_STARTED, -EAGAIN or -EIO, or 0 for a success at once, which
 * the caller makes.
 */
static int mock_bg_start(struct fmd_ctx *ctx, struct fmd_switch *sw,
			 struct fmd_vppb *vppb, uint16_t opcode,
			 enum fmd_vppb_binding_status next, uint8_t port,
			 uint8_t ld)
{
	uint16_t result = opcode == 0x5201 ? sw->mock_bind_result :
					     sw->mock_unbind_result;
	const char *what = opcode == 0x5201 ? "bind" : "unbind";

	if (result == CXLMI_RET_BUSY) {
		fmd_warn(ctx, "%s: mock %s vPPB: the switch is busy", sw->base.id,
			 what);
		return -EAGAIN;
	}
	if (result == CXLMI_RET_UNSUPPORTED) {
		fmd_err(ctx, "%s: mock %s vPPB failed: %d (%s)", sw->base.id,
			what, result, cxlmi_cmd_retcode_tostr(result));
		return -EOPNOTSUPP;
	}
	if (result == CXLMI_RET_SUCCESS && !sw->mock_bind_time_ms)
		return 0;
	if (mock_bg_busy(sw))
		return -EAGAIN;

	if (result != CXLMI_RET_SUCCESS) {
		/* it fails: the vPPB stays what it is */
		next = vppb->binding_status;
		port = vppb->bound_port_id;
		ld = vppb->bound_ld_id;
	}
	vppb->binding_status = FMD_VPPB_BUSY;
	vppb->mock_next_status = next;
	vppb->mock_next_port_id = port;
	vppb->mock_next_ld_id = ld;
	vppb->mock_done_ms = mono_ms() + sw->mock_bind_time_ms;
	sw->mock_bg_opcode = opcode;
	sw->mock_bg_rc = result;
	return FMD_OP_STARTED;
}

/*
 * The vPPB that a bind target is already bound to, NULL if it is unused.
 *
 * The target is a physical port, or one LD of the MLD on it when @ld_id is
 * not FFFFh. A port bound as a whole is in use whatever is asked for, and an
 * LD is in use when that LD is bound. Asking for the whole of a port that has
 * an LD bound finds that LD.
 */
struct fmd_vppb *fmd_switch_bind_target_user(struct fmd_switch *sw,
					     uint8_t port_id, uint16_t ld_id,
					     struct fmd_vcs **user)
{
	for (int v = 0; v < sw->num_vcs; v++) {
		struct fmd_vcs *vcs = &sw->vcs[v];

		for (int i = 0; i < vcs->num_vppbs; i++) {
			struct fmd_vppb *vppb = &vcs->vppbs[i];

			/* Bound Port ID is valid only for these two */
			if (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
			    vppb->binding_status != FMD_VPPB_BOUND_LD)
				continue;
			if (vppb->bound_port_id != port_id)
				continue;
			if (vppb->binding_status == FMD_VPPB_BOUND_LD &&
			    ld_id != 0xFFFF && vppb->bound_ld_id != ld_id)
				continue;

			*user = vcs;
			return vppb;
		}
	}
	return NULL;
}

/*
 * Bind a physical port (or MLD LD) to a vPPB: Bind vPPB (5201h), CXL r4.0
 * Section 7.6.7.2.2.
 *
 * What the specification asks of a bind, and is checked here before the
 * switch is asked:
 *  - @vcs_id is a Virtual CXL Switch ID and @vppb_id the index of the vPPB
 *    within that VCS (Table 7-35). Neither is an index into sw->vcs.
 *  - The vPPB is unbound, and the port is unused: "The Fabric Manager can
 *    bind any unused physical port to the unbound vPPB" (Section 7.2.3.1).
 *    An SLD port is bound to only one VCS; an LD is bound to a host or is
 *    unbound (Section 7.3.4).
 *  - LD ID is the LD-ID if the target port is an MLD port and FFFFh for
 *    other endpoint types (Table 7-35).
 *
 * Returns 0 once the switch has bound the vPPB, FMD_OP_STARTED if it is
 * binding it in the background, -EINVAL for a VCS, vPPB or LD that does not
 * exist, -EALREADY if the vPPB is not unbound, -EBUSY if the target is bound
 * to a vPPB, -EAGAIN if the switch answered Busy, or Retry Required twice,
 * -EOPNOTSUPP if it answered Unsupported, -EPROTO if Invalid Input, and -EIO
 * if it refused otherwise. fmd_vppb_unbind() returns the same.
 */
int fmd_vppb_bind(struct fmd_ctx *ctx, struct fmd_switch *sw,
		  uint8_t vcs_id, uint8_t vppb_id,
		  uint8_t port_id, uint16_t ld_id)
{
	struct cxlmi_cmd_fmapi_bind_vppb_req req;
	struct fmd_vcs *vcs, *user_vcs = NULL;
	struct fmd_vppb *vppb, *user;
	struct fmd_port *target_port;
	int rc;

	if (!ctx || !sw)
		return -EINVAL;

	/* decide on what the switch has now, not on what discovery found */
	fmd_vcs_refresh(ctx, sw);

	vcs = fmd_switch_find_vcs(sw, vcs_id);
	if (!vcs) {
		fmd_err(ctx, "%s: no VCS %d", sw->base.id, vcs_id);
		return -EINVAL;
	}

	vppb = fmd_vcs_find_vppb(vcs, vppb_id);
	if (!vppb) {
		fmd_err(ctx, "%s: no vPPB %d in VCS %d (%d vPPBs)",
			sw->base.id, vppb_id, vcs_id, vcs->num_vppbs);
		return -EINVAL;
	}

	if (vppb->binding_status != FMD_VPPB_UNBOUND) {
		fmd_warn(ctx, "%s: vPPB %d in VCS %d is not unbound (status=%d)",
			 sw->base.id, vppb_id, vcs_id, vppb->binding_status);
		return -EALREADY;
	}

	/* If the port is not in the topology, the switch validates the LD ID */
	target_port = fmd_topology_find_port(&ctx->topology, sw->base.id, port_id);
	if (target_port) {
		bool is_mld = (target_port->conn_dev_type == FMD_CONN_MLD);

		if (is_mld && ld_id == 0xFFFF) {
			fmd_err(ctx, "%s: MLD port %d requires specific LD-ID (not FFFFh)",
				sw->base.id, port_id);
			return -EINVAL;
		}

		if (!is_mld && ld_id != 0xFFFF) {
			fmd_err(ctx, "%s: non-MLD port %d requires LD-ID=FFFFh (got %d)",
				sw->base.id, port_id, ld_id);
			return -EINVAL;
		}

		/*
		 * Supported LD Count: "Number of additional LDs supported by
		 * this port. All ports must support at least one LD." (Table
		 * 7-21). cxl-fmd reads "additional" as past the one every port
		 * has, so LD-IDs 0 to Supported LD Count; the text can also be
		 * read as the total.
		 */
		if (is_mld && ld_id > target_port->supported_ld_count) {
			fmd_err(ctx, "%s: LD-ID %d is past the %d LDs port %d supports",
				sw->base.id, ld_id,
				target_port->supported_ld_count + 1, port_id);
			return -EINVAL;
		}
	}

	user = fmd_switch_bind_target_user(sw, port_id, ld_id, &user_vcs);
	if (user) {
		if (user->binding_status == FMD_VPPB_BOUND_LD)
			fmd_warn(ctx, "%s: LD %d of port %d is bound to vPPB %d in VCS %d",
				 sw->base.id, user->bound_ld_id, port_id,
				 user->vppb_id, user_vcs->vcs_id);
		else
			fmd_warn(ctx, "%s: port %d is bound to vPPB %d in VCS %d",
				 sw->base.id, port_id, user->vppb_id,
				 user_vcs->vcs_id);
		return -EBUSY;
	}

	if (!sw->base.ep) {
		/* Mock mode - update local state only */
		fmd_info(ctx, "%s: mock bind vPPB %d in VCS %d to port %d%s",
			 sw->base.id, vppb_id, vcs_id, port_id,
			 ld_id != 0xFFFF ? " (MLD)" : "");

		/* in the background, as a switch does it, or refused */
		rc = mock_bg_start(ctx, sw, vppb, 0x5201, (ld_id != 0xFFFF) ?
				   FMD_VPPB_BOUND_LD : FMD_VPPB_BOUND_PHYSICAL_PORT,
				   port_id, (ld_id != 0xFFFF) ? ld_id : FMD_VPPB_NO_LD);
		if (rc)
			return rc;

		vppb->bound_port_id = port_id;
		vppb->bound_ld_id = (ld_id != 0xFFFF) ? ld_id : FMD_VPPB_NO_LD;

		if (ld_id != 0xFFFF)
			vppb->binding_status = FMD_VPPB_BOUND_LD;
		else
			vppb->binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT;

		fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
		return 0;
	}

	/* Real hardware - send FM-API command */
	memset(&req, 0, sizeof(req));
	req.vcs_id = vcs_id;
	req.vppb_id = vppb_id;
	req.port_id = port_id;
	req.ld_id = ld_id;

	fmd_info(ctx, "%s: binding vPPB %d in VCS %d to port %d%s",
		 sw->base.id, vppb_id, vcs_id, port_id,
		 ld_id != 0xFFFF ? " (MLD LD)" : "");

	/* the switch may bind in the background: that is not a failure */
	rc = cxlmi_cmd_fmapi_bind_vppb(sw->base.ep, CXLMI_NO_TUNNEL, &req);
	/* "An optional single retry may resolve the issue" (Table 8-208) */
	if (rc == CXLMI_RET_RETRY)
		rc = cxlmi_cmd_fmapi_bind_vppb(sw->base.ep, CXLMI_NO_TUNNEL, &req);
	if (rc == CXLMI_RET_RETRY) {
		fmd_warn(ctx, "%s: bind vPPB: Retry Required, twice", sw->base.id);
		return -EAGAIN;
	}
	if (rc == CXLMI_RET_BUSY) {
		/* it runs one background command at a time */
		fmd_warn(ctx, "%s: bind vPPB: the switch is busy", sw->base.id);
		return -EAGAIN;
	}
	if (rc && rc != CXLMI_RET_BACKGROUND) {
		fmd_err(ctx, "%s: bind vPPB failed: %d (%s)",
			sw->base.id, rc, cxlmi_cmd_retcode_tostr(rc));
		/* Bind and Unbind vPPB are optional (Table 7-31) */
		if (rc == CXLMI_RET_UNSUPPORTED)
			return -EOPNOTSUPP;
		if (rc == CXLMI_RET_INPUT)
			return -EPROTO;
		return -EIO;
	}

	/* Refresh VCS state to reflect the binding */
	fmd_vcs_refresh(ctx, sw);

	fmd_info(ctx, "%s: bind of vPPB %d in VCS %d %s",
		 sw->base.id, vppb_id, vcs_id,
		 rc == CXLMI_RET_BACKGROUND ? "started" : "done");

	return rc == CXLMI_RET_BACKGROUND ? FMD_OP_STARTED : 0;
}

/*
 * Unbind a vPPB from its physical port/LD: Unbind vPPB (5202h), CXL r4.0
 * Section 7.6.7.2.3. As for bind, @vcs_id and @vppb_id are the switch's IDs.
 *
 * @option is the Unbind Option of Table 7-36:
 *   0: Wait for port Link Down before unbinding
 *   1: Simulate Managed Hot-Remove
 *   2: Simulate Surprise Hot-Remove
 */
int fmd_vppb_unbind(struct fmd_ctx *ctx, struct fmd_switch *sw,
		    uint8_t vcs_id, uint8_t vppb_id,
		    enum fmd_unbind_option option)
{
	struct cxlmi_cmd_fmapi_unbind_vppb_req req;
	const char *option_str;
	struct fmd_vcs *vcs;
	struct fmd_vppb *vppb;
	int rc;

	if (!ctx || !sw)
		return -EINVAL;

	fmd_vcs_refresh(ctx, sw);

	vcs = fmd_switch_find_vcs(sw, vcs_id);
	if (!vcs) {
		fmd_err(ctx, "%s: no VCS %d", sw->base.id, vcs_id);
		return -EINVAL;
	}

	vppb = fmd_vcs_find_vppb(vcs, vppb_id);
	if (!vppb) {
		fmd_err(ctx, "%s: no vPPB %d in VCS %d (%d vPPBs)",
			sw->base.id, vppb_id, vcs_id, vcs->num_vppbs);
		return -EINVAL;
	}

	/* Check if vPPB is bound */
	if (vppb->binding_status == FMD_VPPB_UNBOUND) {
		fmd_warn(ctx, "%s: vPPB %d in VCS %d is not bound",
			 sw->base.id, vppb_id, vcs_id);
		return -ENOENT;
	}

	/* Don't allow unbinding USP (vPPB #0) */
	if (vppb_id == 0) {
		fmd_err(ctx, "%s: cannot unbind USP vPPB (vPPB #0) in VCS %d",
			sw->base.id, vcs_id);
		return -EPERM;
	}

	switch (option) {
	case FMD_UNBIND_WAIT_FOR_CLEAN:
		option_str = "wait-for-clean";
		break;
	case FMD_UNBIND_MANAGED_HOT_REMOVE:
		option_str = "managed";
		break;
	case FMD_UNBIND_SURPRISE_HOT_REMOVE:
		option_str = "surprise";
		break;
	default:
		/* all other encodings are reserved */
		return -EINVAL;
	}

	if (!sw->base.ep) {
		/* Mock mode - update local state only */
		fmd_info(ctx, "%s: mock unbind vPPB %d in VCS %d (%s hot-remove)",
			 sw->base.id, vppb_id, vcs_id, option_str);

		rc = mock_bg_start(ctx, sw, vppb, 0x5202, FMD_VPPB_UNBOUND, 0,
				   FMD_VPPB_NO_LD);
		if (rc)
			return rc;

		vppb->binding_status = FMD_VPPB_UNBOUND;
		vppb->bound_port_id = 0;
		vppb->bound_ld_id = FMD_VPPB_NO_LD;
		vppb->bound_pbr_id = 0;
		vppb->port = NULL;
		vppb->ld = NULL;

		return 0;
	}

	/* Real hardware - send FM-API command */
	memset(&req, 0, sizeof(req));
	req.vcs_id = vcs_id;
	req.vppb_id = vppb_id;
	req.option = (uint8_t)option;

	fmd_info(ctx, "%s: unbinding vPPB %d in VCS %d (%s hot-remove)",
		 sw->base.id, vppb_id, vcs_id, option_str);

	/* as for bind, the switch may do this in the background */
	rc = cxlmi_cmd_fmapi_unbind_vppb(sw->base.ep, CXLMI_NO_TUNNEL, &req);
	if (rc == CXLMI_RET_RETRY)
		rc = cxlmi_cmd_fmapi_unbind_vppb(sw->base.ep, CXLMI_NO_TUNNEL, &req);
	if (rc == CXLMI_RET_RETRY) {
		fmd_warn(ctx, "%s: unbind vPPB: Retry Required, twice", sw->base.id);
		return -EAGAIN;
	}
	if (rc == CXLMI_RET_BUSY) {
		fmd_warn(ctx, "%s: unbind vPPB: the switch is busy", sw->base.id);
		return -EAGAIN;
	}
	if (rc && rc != CXLMI_RET_BACKGROUND) {
		fmd_err(ctx, "%s: unbind vPPB failed: %d (%s)",
			sw->base.id, rc, cxlmi_cmd_retcode_tostr(rc));
		/* Bind and Unbind vPPB are optional (Table 7-31) */
		if (rc == CXLMI_RET_UNSUPPORTED)
			return -EOPNOTSUPP;
		if (rc == CXLMI_RET_INPUT)
			return -EPROTO;
		return -EIO;
	}

	/* Refresh VCS state to reflect the unbinding */
	fmd_vcs_refresh(ctx, sw);

	fmd_info(ctx, "%s: unbind of vPPB %d in VCS %d %s",
		 sw->base.id, vppb_id, vcs_id,
		 rc == CXLMI_RET_BACKGROUND ? "started" : "done");

	return rc == CXLMI_RET_BACKGROUND ? FMD_OP_STARTED : 0;
}

/*
 * How a background bind or unbind on a switch is doing: Background
 * Operation Status (0002h), CXL r4.0 Table 8-217.
 */
int fmd_switch_bg_status(struct fmd_ctx *ctx, struct fmd_switch *sw,
			 uint16_t opcode, int *percent, uint16_t *rc)
{
	struct cxlmi_cmd_bg_op_status_rsp rsp = { 0 };
	int ret;

	*percent = 0;
	*rc = CXLMI_RET_SUCCESS;

	if (!sw->base.ep) {
		/* Mock: the operation is over when its time is up */
		int64_t now = mono_ms();
		bool running = false;

		for (int v = 0; v < sw->num_vcs; v++) {
			for (int i = 0; i < sw->vcs[v].num_vppbs; i++) {
				struct fmd_vppb *vppb = &sw->vcs[v].vppbs[i];

				if (!vppb->mock_done_ms)
					continue;
				if (now < vppb->mock_done_ms) {
					running = true;
					continue;
				}
				vppb->binding_status = vppb->mock_next_status;
				vppb->bound_port_id = vppb->mock_next_port_id;
				vppb->bound_ld_id = vppb->mock_next_ld_id;
				if (vppb->binding_status != FMD_VPPB_BOUND_PBR)
					vppb->bound_pbr_id = 0;
				vppb->mock_done_ms = 0;
				/* "Failed bind and unbind operations generate
				 * events to the Warning Event Log" */
				if (sw->mock_bg_rc != CXLMI_RET_SUCCESS)
					fmd_mock_binding_record(sw, 1, sw->vcs[v].vcs_id,
								vppb);
			}
		}
		/* the return code of the last command run in the background */
		if (sw->mock_bg_opcode == opcode)
			*rc = sw->mock_bg_rc;
		if (running) {
			*percent = 50;
			return 1;
		}
		fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
		*percent = 100;
		return 0;
	}

	ret = cxlmi_cmd_bg_op_status(sw->base.ep, CXLMI_NO_TUNNEL, &rsp);
	if (ret) {
		fmd_warn(ctx, "%s: background operation status failed: %d",
			 sw->base.id, ret);
		return -EIO;
	}

	/* Bit[0]: in progress; Bits[7:1]: Percentage Complete */
	*percent = rsp.status >> 1;
	if (rsp.status & 0x01)
		return 1;

	/*
	 * Return Code is that of "the last command executed in the
	 * background", and is valid when Percentage Complete is 100. If
	 * that command is not ours, the switch has run another since: how
	 * ours ended is then only in what the vPPB is now, which the caller
	 * looks at in any case.
	 */
	if (rsp.opcode == opcode && *percent == 100)
		*rc = rsp.returncode;
	*percent = 100;
	return 0;
}

/*
 * Get device timestamp - abstraction for Get Timestamp (0300h)
 *
 * For real hardware: calls cxlmi_cmd_get_timestamp()
 * For mock mode: returns current system time
 *
 * Returns timestamp in nanoseconds since Unix epoch, or 0 on error.
 */
/*
 * Commands sent by requests that hold fabric_lock shared. libcxlmi does not
 * keep two commands on one endpoint apart, and two readers may be on the
 * same one. One lock for all endpoints: there are two such commands, both
 * short, and a reader that sends one is the exception.
 */
static pthread_mutex_t cci_read_lock = PTHREAD_MUTEX_INITIALIZER;

void fmd_cci_read_lock(void)
{
	pthread_mutex_lock(&cci_read_lock);
}

void fmd_cci_read_unlock(void)
{
	pthread_mutex_unlock(&cci_read_lock);
}

/*
 * Where Get and Set Timestamp go. A device on an endpoint of its own takes
 * them directly. Behind a switch, only an MLD can be reached: its port is
 * one of the "switch MLD Ports" that are valid targets of the Tunnel
 * Management Command (Section 7.6.7.3.1), as in discover_port_device(). An
 * SLD on a switch port is not one, and is not asked at all
 * (fmd_sld_behind_switch()).
 */
static struct cxlmi_tunnel_info *timestamp_tunnel(struct fmd_component *comp,
						  struct cxlmi_tunnel_info *ti)
{
	if (comp->type == FMD_COMPONENT_MLD && fmd_to_mld(comp)->switch_port >= 0) {
		*ti = *CXLMI_TUNNEL_SWITCH(fmd_to_mld(comp)->switch_port);
		return ti;
	}
	return CXLMI_NO_TUNNEL;
}

uint64_t fmd_get_device_timestamp(struct fmd_component *comp)
{
	struct timespec ts;

	if (!comp || !comp->fabric)
		return 0;

	/* Real hardware: call CCI command */
	if (likely(comp->ep)) {
		struct cxlmi_cmd_get_timestamp_rsp rsp = {0};
		struct cxlmi_tunnel_info ti;
		int rc;

		if (fmd_sld_behind_switch(comp))
			return 0;

		/* sent by a reader: see fmd_cci_read_lock() */
		fmd_cci_read_lock();
		rc = cxlmi_cmd_get_timestamp(comp->ep, timestamp_tunnel(comp, &ti),
					     &rsp);
		fmd_cci_read_unlock();

		return rc == 0 ? rsp.timestamp : 0;
	}

	/* Mock mode: return current system time */
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
		return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	return 0;
}

/*
 * Set device timestamp - abstraction for Set Timestamp (0301h)
 *
 * For real hardware: calls cxlmi_cmd_set_timestamp()
 * For mock mode: no-op (accepts the value but doesn't persist)
 *
 * Returns 0 on success, negative on error.
 */
int fmd_set_device_timestamp(struct fmd_component *comp, uint64_t timestamp_ns)
{
	if (!comp || !comp->fabric)
		return -1;

	/* Real hardware: call CCI command */
	if (likely(comp->ep)) {
		struct cxlmi_cmd_set_timestamp_req req = {
			.timestamp = timestamp_ns
		};

		struct cxlmi_tunnel_info ti;

		if (fmd_sld_behind_switch(comp))
			return -EOPNOTSUPP;

		return cxlmi_cmd_set_timestamp(comp->ep,
					       timestamp_tunnel(comp, &ti), &req);
	}

	/* Mock mode: accept silently (no persistence needed) */
	return 0;
}

/*
 * Get security state - abstraction for Get Security State (4500h)
 *
 * For real hardware: calls cxlmi_cmd_memdev_get_security_state()
 * For mock mode: returns 0 (security disabled)
 *
 * Returns security state bitmask, or 0 on error/mock mode.
 * Security state bits:
 *   Bit 0: User Passphrase Set (security enabled)
 *   Bit 1: Master Passphrase Set
 *   Bit 2: Locked
 *   Bit 3: Frozen
 *   Bit 4: User Passphrase Attempt Count Reached
 *   Bit 5: Master Passphrase Attempt Count Reached
 */
uint32_t fmd_get_security_state(struct fmd_component *comp, int ld_id)
{
	struct cxlmi_cmd_memdev_get_security_state_rsp rsp = {0};
	struct cxlmi_tunnel_info ti_buf, *ti = CXLMI_NO_TUNNEL;

	if (!comp || unlikely(!comp->ep) || fmd_sld_behind_switch(comp))
		return 0;

	if (comp->type == FMD_COMPONENT_MLD)
		ti = fmd_mld_ld_tunnel(fmd_to_mld(comp), ld_id, &ti_buf);

	int rc;

	/* sent by a reader: see fmd_cci_read_lock() */
	fmd_cci_read_lock();
	rc = cxlmi_cmd_memdev_get_security_state(comp->ep, ti, &rsp);
	fmd_cci_read_unlock();
	if (rc != 0)
		return 0;

	return rsp.security_state;
}


/*
 * Set LD Allocations (5402h) of one LD, on @ep through @ti, for it to have
 * @size_bytes in all.
 *
 * Each LD has two ranges, each "multiplied with Memory Granularity to
 * calculate the memory allocation range in bytes" (Table 7-49), and the
 * request rewrites both. Range 2 is kept as Get LD Allocations (5401h)
 * reads it now, and Range 1 takes the rest: an LD's second range is not
 * folded into its first. @size_bytes must be a multiple of the Memory
 * Granularity, and at least Range 2.
 *
 * "The allocations provided in the response reflect the state of the LD
 * allocations after the command is processed" (Section 7.6.7.4.3): the LD
 * is sized from them.
 *
 * Returns 0, -EINVAL if the LD cannot be given @size_bytes, -EIO if a
 * command failed or its response is not for that LD.
 */
static int set_ld_allocation(struct fmd_ctx *ctx, const char *name,
			     struct cxlmi_endpoint *ep,
			     struct cxlmi_tunnel_info *ti, struct fmd_ld *ld,
			     uint8_t ld_id, uint64_t size_bytes)
{
	struct cxlmi_cmd_fmapi_get_ld_allocations_req get_req = {
		.start_ld_id = ld_id,
		.ld_allocation_list_limit = 1,
	};
	struct cxlmi_cmd_fmapi_get_ld_allocations_rsp *get_rsp;
	struct cxlmi_cmd_fmapi_set_ld_allocations_req *req;
	struct cxlmi_cmd_fmapi_set_ld_allocations_rsp *rsp;
	uint64_t granularity, range2, mult;
	int rc;

	get_rsp = calloc(1, sizeof(*get_rsp) +
			 FMD_CCI_LIST_MAX * sizeof(get_rsp->ld_allocation_list[0]));
	if (!get_rsp)
		return -ENOMEM;
	rc = cxlmi_cmd_fmapi_get_ld_allocations(ep, ti, &get_req, get_rsp);
	if (rc || get_rsp->ld_allocation_list_len < 1 ||
	    get_rsp->start_ld_id != ld_id || get_rsp->memory_granularity > 2) {
		fmd_warn(ctx, "%s: cannot read LD %d allocation: %d", name,
			 ld_id, rc);
		free(get_rsp);
		return -EIO;
	}
	/* Memory Granularity: 0h = 256 MB, 1h = 512 MB, 2h = 1 GB */
	granularity = (uint64_t)CXL_CAPACITY_UNIT << get_rsp->memory_granularity;
	range2 = get_rsp->ld_allocation_list[0].range_2_allocation_mult;
	free(get_rsp);

	mult = size_bytes / granularity;
	if (size_bytes % granularity || mult < range2) {
		fmd_warn(ctx, "%s: LD %d cannot have %" PRIu64 " MB: granularity %"
			 PRIu64 " MB, Range 2 %" PRIu64 " MB", name, ld_id,
			 size_bytes >> 20, granularity >> 20,
			 (range2 * granularity) >> 20);
		return -EINVAL;
	}

	req = calloc(1, sizeof(*req) + sizeof(req->ld_allocation_list[0]));
	rsp = calloc(1, sizeof(*rsp) +
		     FMD_CCI_LIST_MAX * sizeof(rsp->ld_allocation_list[0]));
	if (!req || !rsp) {
		free(req);
		free(rsp);
		return -ENOMEM;
	}
	req->number_ld = 1;
	req->start_ld_id = ld_id;
	req->ld_allocation_list[0].range_1_allocation_mult = mult - range2;
	req->ld_allocation_list[0].range_2_allocation_mult = range2;

	rc = cxlmi_cmd_fmapi_set_ld_allocations(ep, ti, req, rsp);
	free(req);
	if (rc != 0 || rsp->number_ld < 1 || rsp->start_ld_id != ld_id) {
		fmd_warn(ctx, "%s: set LD %d allocation failed: %d", name,
			 ld_id, rc);
		free(rsp);
		return -EIO;
	}

	ld->memory_size = (rsp->ld_allocation_list[0].range_1_allocation_mult +
			   rsp->ld_allocation_list[0].range_2_allocation_mult) *
			  granularity;
	fmd_info(ctx, "%s: LD %d allocation set to %" PRIu64 " MB", name, ld_id,
		 ld->memory_size >> 20);
	free(rsp);
	return 0;
}

/*
 * Set LD allocation for an MLD via CCI Set LD Allocations (5402h)
 *
 * For mock mode, updates local state only.
 * For hardware mode, see set_ld_allocation(): Range 2 is kept, and
 * @size_bytes must be a multiple of the Memory Granularity.
 */
int fmd_mld_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mld *mld,
			      uint8_t ld_id, uint64_t size_bytes)
{
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info *ti;
	struct fmd_ld *ld = NULL;
	struct fmd_component *child;

	if (!ctx || !mld)
		return -1;

	/* Find the target LD */
	fmd_for_each_child(&mld->base, child) {
		if (child->type == FMD_COMPONENT_LD) {
			struct fmd_ld *candidate = fmd_to_ld(child);
			if (candidate->ld_id == ld_id) {
				ld = candidate;
				break;
			}
		}
	}

	if (!ld) {
		fmd_warn(ctx, "%s: LD %d not found", mld->base.id, ld_id);
		return -1;
	}

	/* Mock mode: update local state only */
	ep = mld->base.ep;
	if (!ep) {
		fmd_info(ctx, "%s: mock set LD %d allocation to %lu MB",
			 mld->base.id, ld_id,
			 (unsigned long)(size_bytes / (1024 * 1024)));
		ld->memory_size = size_bytes;
		return 0;
	}

	/* Hardware mode: issue CCI command */
	ti = (mld->switch_port < 0)
		? CXLMI_NO_TUNNEL
		: CXLMI_TUNNEL_SWITCH(mld->switch_port);

	return set_ld_allocation(ctx, mld->base.id, ep, ti, ld, ld_id, size_bytes);
}


/*
 * The LD Pool CCI of a Multi-Headed device "may be exposed as an MCTP-based
 * CCI or can be accessed via the Tunnel Management Command command through
 * a head's Mailbox CCI" (CXL r4.0 Section 2.5), and the MHD command set
 * "is required on the LD Pool CCI" (Section 7.6.7.5). An endpoint that
 * answered Get Multi-Headed Info (5500h) with no tunnel is taken to be the
 * LD Pool CCI, and is sent its commands with no tunnel: cxl-fmd's
 * inference, as "required" does not say "only". Discovery reads the pool
 * there (Get Multi-Headed Info, then each pool LD through it), so that
 * commands that change the pool go where the pool was read. No other
 * route is known: a head's Mailbox CCI is not discovered as one.
 */
int fmd_mhd_pool_route(const struct fmd_mhd *mhd, struct cxlmi_endpoint **ep,
		       struct cxlmi_tunnel_info **ti)
{
	if (!mhd->pool_cci || !mhd->base.ep)
		return -EOPNOTSUPP;
	*ep = mhd->base.ep;
	*ti = CXLMI_NO_TUNNEL;
	return 0;
}

/*
 * Set LD allocation for an MHD via LD Pool CCI Set LD Allocations (5402h)
 *
 * The pool_ld_id is the device-wide LD index (not head-local LD-ID).
 *
 * For mock mode, updates local state only.
 * For hardware mode, sent on fmd_mhd_pool_route(), or not at all.
 */
int fmd_mhd_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mhd *mhd,
			      uint8_t pool_ld_id, uint64_t size_bytes)
{
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info *ti;
	struct fmd_ld *ld = NULL;
	struct fmd_component *child;

	if (!ctx || !mhd)
		return -1;

	/* Find the target LD in the pool */
	fmd_for_each_child(&mhd->base, child) {
		if (child->type == FMD_COMPONENT_LD) {
			struct fmd_ld *candidate = fmd_to_ld(child);
			if (candidate->ld_id == pool_ld_id) {
				ld = candidate;
				break;
			}
		}
	}

	if (!ld) {
		fmd_warn(ctx, "%s: pool LD %d not found", mhd->base.id, pool_ld_id);
		return -1;
	}

	/* Mock mode: update local state only */
	if (unlikely(fmd_is_mock(ctx))) {
		fmd_info(ctx, "%s: mock set pool LD %d allocation to %lu MB",
			 mhd->base.id, pool_ld_id,
			 (unsigned long)(size_bytes / (1024 * 1024)));
		ld->memory_size = size_bytes;
		return 0;
	}

	if (fmd_mhd_pool_route(mhd, &ep, &ti)) {
		fmd_warn(ctx, "%s: no LD Pool CCI known: pool LD %d not changed",
			 mhd->base.id, pool_ld_id);
		return -EOPNOTSUPP;
	}
	return set_ld_allocation(ctx, mhd->base.id, ep, ti, ld, pool_ld_id,
				 size_bytes);
}


/*
 * Find which head of an MHD a host is attached to.
 * Searches MHD's head children for one with matching attached_host_id.
 */
struct fmd_head *fmd_mhd_find_host_head(struct fmd_mhd *mhd, const char *host_id)
{
	struct fmd_component *child;

	if (!mhd || !host_id)
		return NULL;

	fmd_for_each_child(&mhd->base, child) {
		if (child->type == FMD_COMPONENT_HEAD) {
			struct fmd_head *head = fmd_to_head(child);
			if (head->attached_host_id[0] != '\0' &&
			    strcmp(head->attached_host_id, host_id) == 0) {
				return head;
			}
		}
	}

	return NULL;
}


/*
 * Validate that a pool LD is accessible from a given head.
 * An LD is accessible if its binding_point_id matches the head's head_id.
 */
bool fmd_mhd_ld_accessible_from_head(struct fmd_ld *ld, struct fmd_head *head)
{
	if (!ld || !head)
		return false;

	return ld->binding_point_id == head->head_id;
}


/*
 * The Volatile Capacity of Set Partition Info (4101h) that leaves a device
 * with @next_volatile of volatile capacity in all, in units of 256 MB.
 *
 * @next_volatile is a total, as Next Volatile Capacity is: "the sum of the
 * device's Volatile Only capacity and the capacity that is partitioned for
 * volatile use" (Table 8-310, of Active Volatile Capacity), which is what
 * Memory's StagedVolatileSizeMiB maps to. Volatile Capacity is not:
 * "The amount of partitionable capacity that shall be allocated to
 * volatile capacity, in multiples of 256 MB aligned to the partition
 * alignment requirement reported in the Identify Memory Device command"
 * (Table 8-311), where "Partitionable capacity is equal to (Total Capacity
 * - Volatile Only Capacity - Persistent Only Capacity)" (Section
 * 8.2.10.9.2.2).
 *
 * Returns 0 with *@volatile_capacity set, -EOPNOTSUPP if the device cannot
 * be partitioned: "This command shall fail with an Unsupported error if
 * there is no partitionable capacity (i.e., Identify Memory Device reports
 * Partition Alignment as zero)", and -EINVAL if @next_volatile is less
 * than the Volatile Only capacity, leaves less than the Persistent Only
 * capacity, or is not on the partition alignment: "The device shall
 * return Invalid Input if the specified capacity is not aligned".
 */
int fmd_partition_volatile(const struct cxlmi_cmd_memdev_identify_rsp *id,
			   uint64_t next_volatile, uint64_t *volatile_capacity)
{
	uint64_t fixed = id->volatile_capacity + id->persistent_capacity;
	uint64_t part;

	if (!id->partition_align)
		return -EOPNOTSUPP;
	if (id->total_capacity < fixed || next_volatile < id->volatile_capacity)
		return -EINVAL;

	part = next_volatile - id->volatile_capacity;
	if (part > id->total_capacity - fixed || part % id->partition_align)
		return -EINVAL;

	*volatile_capacity = part;
	return 0;
}

/*
 * Set Partition Info (4101h): have @next_volatile of volatile capacity in
 * all, in units of 256 MB, at the next reset, or at once if @immediate.
 * Sent on @ep through @ti; with no @ep (mock mode) only @part changes.
 *
 * Changing the persistent capacity loses its data: "Using this command to
 * change the size of the persistent capacity shall result in the loss of
 * data stored" (Section 8.2.10.9.2.2).
 *
 * Returns 0, -EOPNOTSUPP or -EINVAL as fmd_partition_volatile() does, or
 * -EIO if the command failed.
 */
int fmd_memdev_set_partition(struct fmd_ctx *ctx, const char *name,
			     struct cxlmi_endpoint *ep,
			     struct cxlmi_tunnel_info *ti,
			     const struct cxlmi_cmd_memdev_identify_rsp *id,
			     struct cxlmi_cmd_memdev_get_partition_info_rsp *part,
			     uint64_t next_volatile, bool immediate)
{
	struct cxlmi_cmd_memdev_set_partition_info_req req = { 0 };
	uint64_t volatile_capacity;
	int rc;

	rc = fmd_partition_volatile(id, next_volatile, &volatile_capacity);
	if (rc) {
		fmd_warn(ctx, "%s: cannot partition for %" PRIu64 " MB volatile: %s",
			 name, next_volatile * 256, rc == -EOPNOTSUPP ?
			 "Partition Alignment is 0" : "out of range or not aligned");
		return rc;
	}

	if (ep) {
		req.volatile_capacity = volatile_capacity;
		req.flags = immediate ? 0x01 : 0x00;	/* Bit[0]: Immediate */
		rc = cxlmi_cmd_memdev_set_partition_info(ep, ti, &req);
		if (rc != 0) {
			fmd_warn(ctx, "%s: set partition info failed: %d", name, rc);
			return -EIO;
		}
	}

	/*
	 * Not immediate: "the new configuration shall be reported in the
	 * Next Volatile Capacity and Next Persistent Capacity fields"
	 * (Table 8-311). Both are totals, so the persistent one is what the
	 * volatile one leaves. What an immediate change does to a pending
	 * one the specification does not say; cxl-fmd takes it as replaced.
	 */
	if (immediate) {
		part->active_vmem = next_volatile;
		part->active_pmem = id->total_capacity - next_volatile;
		part->next_vmem = 0;
		part->next_pmem = 0;
	} else {
		part->next_vmem = next_volatile;
		part->next_pmem = id->total_capacity - next_volatile;
	}

	fmd_info(ctx, "%s: partition set to volatile=%" PRIu64 " MB (immediate=%d)",
		 name, next_volatile * 256, immediate);
	return 0;
}

/*
 * The volatile capacity of a memory device, in all and in units of 256 MB,
 * once what is staged takes effect: Next Volatile Capacity if a change is
 * pending ("If both this field and the Next Persistent Capacity field are
 * 0, there is no pending change to the partitioning", Table 8-310), else
 * Active Volatile Capacity.
 */
uint64_t fmd_partition_next_volatile(const struct cxlmi_cmd_memdev_get_partition_info_rsp *part)
{
	if (part->next_vmem || part->next_pmem)
		return part->next_vmem;
	return part->active_vmem;
}

/*
 * Set volatile/persistent partition for an SLD via Set Partition Info
 * (4101h): see fmd_memdev_set_partition(). @volatile_bytes is the total
 * volatile capacity wanted, a multiple of 256 MB.
 */
int fmd_sld_set_partition(struct fmd_ctx *ctx, struct fmd_sld *sld,
			  uint64_t volatile_bytes, bool immediate)
{
	if (!ctx || !sld)
		return -1;

	if (fmd_sld_behind_switch(&sld->base))
		return -EOPNOTSUPP;

	if (volatile_bytes % CXL_CAPACITY_UNIT)
		return -EINVAL;

	/* without an endpoint (mock mode), local state only */
	return fmd_memdev_set_partition(ctx, sld->base.id, sld->base.ep,
					CXLMI_NO_TUNNEL, &sld->memdev,
					&sld->partition,
					volatile_bytes / CXL_CAPACITY_UNIT,
					immediate);
}
