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

	mld->ld_count = ld_info.ld_count;
	mld->max_ld_count = FMD_MAX_LD_COUNT;  /* Spec max for MLD */
	mld->memory_granularity = 1; /* 256 MB, until 5401h says otherwise */
	mld->total_memory_ranges = 0; /* Not available in this response */

	fmd_info(ctx, "%s: MLD with %d logical devices", mld->base.id, mld->ld_count);

	/* Get LD Allocations (5401h) */
	nld = mld->ld_count < FMD_MAX_LD_COUNT ? mld->ld_count : FMD_MAX_LD_COUNT;

	rsp_size = sizeof(struct cxlmi_cmd_fmapi_get_ld_allocations_rsp) +
		   FMD_CCI_LIST_MAX * sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	alloc_rsp = calloc(1, rsp_size);
	if (!alloc_rsp) {
		fmd_warn(ctx, "%s: failed to allocate LD allocations buffer", mld->base.id);
		return -1;
	}

	/*
	 * As many LDs at a time as fit a response, from Start LD ID on. The
	 * device returns min(limit, LDs left), so no more is asked for than
	 * there are: 16 LDs do not fit the 256 bytes every CCI supports.
	 */
	while (nalloc < nld) {
		uint8_t want = fmd_cci_list_fit(&mld->base, 4, 16, nld - nalloc);

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

		FMD_CCI_LIST_CLAMP(alloc_rsp->ld_allocation_list_len, want);
		if (alloc_rsp->ld_allocation_list_len == 0)
			break;
		for (i = 0; i < alloc_rsp->ld_allocation_list_len; i++)
			ld_units[nalloc++] =
				alloc_rsp->ld_allocation_list[i].range_1_allocation_mult +
				alloc_rsp->ld_allocation_list[i].range_2_allocation_mult;
	}
	free(alloc_rsp);

	/* Get QoS Control (5403h) */
	rc = cxlmi_cmd_fmapi_get_qos_control(ep, ti, &qos_ctrl);
	if (rc == 0) {
		mld->qos_control.enabled = qos_ctrl.qos_telemetry_control & 0x01;
		mld->qos_control.egress_moderate_pct = qos_ctrl.egress_moderate_percentage;
		mld->qos_control.egress_severe_pct = qos_ctrl.egress_severe_percentage;
		mld->qos_control.backpressure_sample_interval = qos_ctrl.backpressure_sample_interval;
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
		fmd_topology_add(&ctx->topology, &ld->base, &mld->base);

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

	FMD_CCI_LIST_CLAMP(mh_rsp->ld_map_len, FMD_MAX_LD_POOL);

	mhd->num_heads = mh_rsp->num_heads;
	mhd->max_heads = mh_rsp->num_heads;  /* Assume current = max */
	mhd->total_lds = mh_rsp->num_lds;
	mhd->max_lds = mh_rsp->num_lds;

	fmd_info(ctx, "%s: MHD with %d heads, %d LDs in pool",
		 mhd->base.id, mhd->num_heads, mhd->total_lds);

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

			head->max_link_width = hi->field_1;
			head->negotiated_link_width = hi->field_2;

			/* A head whose link is not up is not in use */
			if (ltssm_to_link_status(hi->ltssm_state) != FMD_LINK_UP)
				head->state = FMD_HEAD_DISABLED;
		}

		fmd_topology_add(&ctx->topology, &head->base, &mhd->base);

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

		/* Get binding from ld_map if available */
		if (mh_rsp->ld_map_len > 0 && i < mh_rsp->ld_map_len)
			binding_point = mh_rsp->ld_map[i];

		ld->binding_point_id = binding_point;
		ld->visible_ld_id = i;  /* Pool index */

		fmd_topology_add(&ctx->topology, &ld->base, &mhd->base);

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
		fmd_topology_add(&ctx->topology, &sld->base, &port->base);

		fmd_info(ctx, "%s: SLD %s on port %d: not manageable through "
			 "the switch, capacity unknown",
			 port->base.parent->id, dev_id, port->port_number);
		break;
	}

	case FMD_CONN_MLD: {
		struct cxlmi_cmd_identify_rsp identify = { 0 };
		struct fmd_mld *mld;

		/*
		 * A command tunnelled to an MLD port is processed by the
		 * FM-owned LD of the MLD (Section 7.6.7.3.1). Its Identify
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

		fmd_topology_add(&ctx->topology, &mld->base, &port->base);

		rc = discover_mld(ctx, mld);
		if (rc == 0)
			fmd_create_mld_memory_resources(ctx, mld);
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
		}

		count = total - got < want ? total - got : want;
		for (int i = 0; i < count; i++) {
			struct cxlmi_cmd_fmapi_vppb_info *src = &block->vppbs[i];
			struct fmd_vppb *vppb = &vcs->vppbs[vcs->num_vppbs++];

			vppb->vppb_id = got + i;
			vppb->binding_status = src->binding_status;
			vppb->bound_port_id = src->bound_port_id;
			vppb->bound_ld_id = src->bound_ld_id;
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
	if (!sw->base.ep)
		return 0;	/* mock: the table is all there is */

	table = calloc(FMD_MAX_VCS, sizeof(*table));
	if (!table)
		return -ENOMEM;

	for (int id = 0; id < sw->sw_identify.num_vcs && n < FMD_MAX_VCS; id++) {
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

	memcpy(sw->vcs, table, sizeof(sw->vcs));
	sw->num_vcs = n;
	free(table);

	/* the port and LD each bound vPPB points at */
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);

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
static void add_switch_port(struct fmd_ctx *ctx, struct fmd_switch *sw,
			    struct cxlmi_cmd_fmapi_port_state_info_block *pinfo)
{
	char port_id[128];
	struct fmd_port *port;

	(void)snprintf(port_id, sizeof(port_id), "%.50s_dsp%d", sw->base.id, pinfo->port_id);

	port = fmd_port_create(port_id, pinfo->port_id);
	if (!port)
		return;

	/* Map port state info to our structures */
	port->conn_dev_type = pinfo->conn_dev_type;
	port->link_status = ltssm_to_link_status(pinfo->ltssm_state);
	port->max_link_width = pinfo->max_link_width;
	port->negotiated_link_width = pinfo->negotiated_link_width;
	port->supported_ld_count = pinfo->supported_ld_count;

	/*
	 * Current Port Configuration State is Bits[3:0]; Bit[4] is GAE
	 * Support. A port that is not a DSP or a USP must not be left
	 * in the state fmd_port_create() starts it in, which is DSP.
	 */
	switch (pinfo->config_state & 0xf) {
	case 0x1: /* Bind in progress */
		port->state = FMD_PORT_BIND_IN_PROGRESS;
		break;
	case 0x2: /* Unbind in progress */
		port->state = FMD_PORT_UNBIND_IN_PROGRESS;
		break;
	case 0x3: /* DSP */
		port->state = FMD_PORT_DSP;
		sw->num_dsps++;
		break;
	case 0x4: /* USP */
		port->state = FMD_PORT_USP;
		sw->num_usps++;
		break;
	case 0x0: /* Disabled */
	default:  /* Fabric Port (PBR), Invalid Port_ID, reserved */
		port->state = FMD_PORT_DISABLED;
		break;
	}

	/* Add to topology */
	fmd_topology_add(&ctx->topology, &port->base, &sw->base);

	fmd_dbg(ctx, "%s: port %d conn_type=%d link=%s width=%d",
		sw->base.id, port->port_number,
		port->conn_dev_type,
		port->link_status == FMD_LINK_UP ? "up" : "down",
		port->negotiated_link_width);

	/* Connected Device Type is reserved unless the port is a DSP */
	if (port->state == FMD_PORT_DSP)
		discover_port_device(ctx, port);
}

static int discover_switch(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	struct cxlmi_cmd_fmapi_get_phys_port_state_req *port_req;
	struct cxlmi_cmd_fmapi_get_phys_port_state_rsp *port_rsp;
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
	 * Bitmask, as many at a time as fit a request and its response.
	 */
	nports = 0;
	for (int id = 0; id < 256 && nports < num_ports; id++) {
		if (sw->sw_identify.active_port_bitmask[id / 8] & (1 << (id % 8)))
			port_ids[nports++] = id;
	}

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

		rc = cxlmi_cmd_fmapi_get_phys_port_state(ep, CXLMI_NO_TUNNEL,
							 port_req, port_rsp);
		if (rc) {
			fmd_warn(ctx, "%s: get physical port state failed: %d",
				 sw->base.id, rc);
			/* the ports read so far are still ports of the switch */
			if (done == 0) {
				free(port_req);
				free(port_rsp);
				return rc;
			}
			break;
		}
		FMD_CCI_LIST_CLAMP(port_rsp->num_ports, want);

		for (int i = 0; i < port_rsp->num_ports; i++)
			add_switch_port(ctx, sw, &port_rsp->ports[i]);
		done += want;
	}

	free(port_req);
	free(port_rsp);

	/* Step 3: Discover VCS configuration */
	fmd_vcs_refresh(ctx, sw);

	/*
	 * Step 4: Create host entries for each VCS
	 *
	 * Each VCS represents a host connection via its USP (Upstream Switch Port).
	 * Per OCP CMS model, hosts are represented as Systems with Fabric Endpoints.
	 */
	for (int vcs_idx = 0; vcs_idx < sw->num_vcs; vcs_idx++) {
		struct fmd_vcs *vcs = &sw->vcs[vcs_idx];
		char host_id[FMD_MAX_ID_LEN];
		struct fmd_host *host;

		snprintf(host_id, sizeof(host_id), "%.50s_host%d", sw->base.id, vcs->vcs_id);

		host = fmd_host_create(host_id);
		if (host) {
			snprintf(host->name, sizeof(host->name), "Host via %.46s VCS %d",
				 sw->base.id, vcs->vcs_id);
			host->vcs_id = vcs->vcs_id;
			host->usp_port_id = vcs->upstream_port_id;
			host->direct_attached = false;  /* Switch-attached */

			fmd_topology_add(&ctx->topology, &host->base, NULL);

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
		fmd_topology_add(&ctx->topology, &sw->base, NULL);

		rc = discover_switch(ctx, sw);
		break;
	}

	case CXL_COMPONENT_TYPE3: {
		/*
		 * Direct-attached Type 3 - determine device type:
		 * 1. Try Get Multi-Headed Info (5500h) - if num_heads > 1, it's MHD
		 * 2. Otherwise, use Get LD Info (5400h) - ld_count > 1 means MLD
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
			fmd_topology_add(&ctx->topology, &mhd->base, NULL);

			rc = discover_mhd(ctx, mhd);
			if (rc == 0)
				fmd_create_mhd_memory_resources(ctx, mhd);
			if (rc == 0)
				fmd_dcd_discover(ctx, &mhd->base);

			fmd_info(ctx, "%s: MHD with %d heads, %d LDs",
				 dev_id, mhd->num_heads, mhd->total_lds);
			break;
		}

		/* Not MHD - check SLD vs MLD via LD count */
		rc = cxlmi_cmd_fmapi_get_ld_info(ep, CXLMI_NO_TUNNEL, &ld_info);
		if (rc || ld_info.ld_count <= 1) {
			/* SLD - direct-attached, no tunneling ever */
			struct fmd_sld *sld = fmd_sld_create(dev_id);
			if (!sld)
				return -1;

			sld->identify = identify;
			sld->base.ep = ep;
			sld->base.msg_limit = msg_limit;
			fmd_topology_add(&ctx->topology, &sld->base, NULL);

			rc = discover_sld(ctx, sld);
			if (rc == 0)
				fmd_create_sld_memory_resources(ctx, sld);
			if (rc == 0)
				fmd_dcd_discover(ctx, &sld->base);

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
			fmd_topology_add(&ctx->topology, &mld->base, NULL);

			rc = discover_mld(ctx, mld);
			if (rc == 0)
				fmd_create_mld_memory_resources(ctx, mld);
			if (rc == 0)
				fmd_dcd_discover(ctx, &mld->base);

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

int fmd_discover(struct fmd_ctx *ctx)
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

	if (!ctx || !ctx->cxlmi)
		return -1;

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
		fmd_dbg(ctx, "Scanning /dev/cxl/ for CXL devices...");
		rc = cxlmi_scan(ctx->cxlmi);
		if (rc < 0) {
			fmd_warn(ctx, "ioctl scan failed");
		} else {
			fmd_dbg(ctx, "ioctl scan found %d endpoint(s)", rc);
		}
	}

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
			rc = discover_endpoint(ctx, found[i].ep,
					       &found[i].identify, i);
			if (rc == 0)
				count++;
		}
	}
	free(found);

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
			fmd_topology_add(&ctx->topology, &local_host->base, NULL);
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
 * Returns 0 once the switch has accepted the bind, -EINVAL for a VCS, vPPB
 * or LD that does not exist, -EBUSY if the vPPB or the target is in use,
 * -EIO if the switch refused.
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
		fmd_warn(ctx, "%s: vPPB %d in VCS %d is already bound (status=%d)",
			 sw->base.id, vppb_id, vcs_id, vppb->binding_status);
		return -EBUSY;
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
		 * Supported LD Count is the number of additional LDs the port
		 * supports, every port supporting at least one (Table 7-21):
		 * LD-IDs 0 to Supported LD Count.
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

		vppb->bound_port_id = port_id;
		vppb->bound_ld_id = (ld_id != 0xFFFF) ? ld_id : 0;

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
	if (rc && rc != CXLMI_RET_BACKGROUND) {
		fmd_err(ctx, "%s: bind vPPB failed: %d (%s)",
			sw->base.id, rc, cxlmi_cmd_retcode_tostr(rc));
		return -EIO;
	}

	/* Refresh VCS state to reflect the binding */
	fmd_vcs_refresh(ctx, sw);

	fmd_info(ctx, "%s: bind of vPPB %d in VCS %d accepted",
		 sw->base.id, vppb_id, vcs_id);

	return 0;
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

		vppb->binding_status = FMD_VPPB_UNBOUND;
		vppb->bound_port_id = 0;
		vppb->bound_ld_id = 0;
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
	if (rc && rc != CXLMI_RET_BACKGROUND) {
		fmd_err(ctx, "%s: unbind vPPB failed: %d (%s)",
			sw->base.id, rc, cxlmi_cmd_retcode_tostr(rc));
		return -EIO;
	}

	/* Refresh VCS state to reflect the unbinding */
	fmd_vcs_refresh(ctx, sw);

	fmd_info(ctx, "%s: unbind of vPPB %d in VCS %d accepted",
		 sw->base.id, vppb_id, vcs_id);

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
uint64_t fmd_get_device_timestamp(struct fmd_component *comp)
{
	struct timespec ts;

	if (!comp || !comp->fabric)
		return 0;

	/* Real hardware: call CCI command */
	if (comp->ep) {
		struct cxlmi_cmd_get_timestamp_rsp rsp = {0};

		/* Use switch tunneling if device is behind a switch port */
		if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
			struct fmd_port *port = fmd_to_port(comp->parent);
			DEFINE_CXLMI_TUNNEL_SWITCH(ti, port->port_number);
			if (cxlmi_cmd_get_timestamp(comp->ep, &ti, &rsp) == 0)
				return rsp.timestamp;
			return 0;
		}

		if (cxlmi_cmd_get_timestamp(comp->ep, CXLMI_NO_TUNNEL, &rsp) == 0)
			return rsp.timestamp;

		return 0;
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
	if (comp->ep) {
		struct cxlmi_cmd_set_timestamp_req req = {
			.timestamp = timestamp_ns
		};

		/* Use switch tunneling if device is behind a switch port */
		if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
			struct fmd_port *port = fmd_to_port(comp->parent);
			DEFINE_CXLMI_TUNNEL_SWITCH(ti, port->port_number);
			return cxlmi_cmd_set_timestamp(comp->ep, &ti, &req);
		}

		return cxlmi_cmd_set_timestamp(comp->ep, CXLMI_NO_TUNNEL, &req);
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

	if (!comp || !comp->ep)
		return 0;

	if (comp->type == FMD_COMPONENT_MLD)
		ti = fmd_mld_ld_tunnel(fmd_to_mld(comp), ld_id, &ti_buf);

	if (cxlmi_cmd_memdev_get_security_state(comp->ep, ti, &rsp) != 0)
		return 0;

	return rsp.security_state;
}


/*
 * Set LD allocation for an MLD via CCI Set LD Allocations (5402h)
 *
 * For mock mode, updates local state only.
 * For hardware mode, issues the CCI command and updates local state on success.
 *
 * The allocation is converted from bytes to the device's memory granularity units.
 */
int fmd_mld_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mld *mld,
			      uint8_t ld_id, uint64_t size_bytes)
{
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info *ti;
	struct fmd_ld *ld = NULL;
	struct fmd_component *child;
	uint64_t granularity_bytes;
	uint64_t allocation_mult;
	int rc;

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

	/* Calculate allocation in granularity units (256MB * memory_granularity) */
	granularity_bytes = (uint64_t)mld->memory_granularity * CXL_CAPACITY_UNIT;
	if (granularity_bytes == 0)
		granularity_bytes = CXL_CAPACITY_UNIT;  /* Default 256MB */

	allocation_mult = size_bytes / granularity_bytes;

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

	/* Build request - update single LD */
	size_t req_size = sizeof(struct cxlmi_cmd_fmapi_set_ld_allocations_req) +
			  sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	struct cxlmi_cmd_fmapi_set_ld_allocations_req *req = calloc(1, req_size);
	if (!req)
		return -1;

	req->number_ld = 1;
	req->start_ld_id = ld_id;
	req->ld_allocation_list[0].range_1_allocation_mult = allocation_mult;
	req->ld_allocation_list[0].range_2_allocation_mult = 0;  /* Single range */

	/* Build response buffer */
	size_t rsp_size = sizeof(struct cxlmi_cmd_fmapi_set_ld_allocations_rsp) +
			  FMD_CCI_LIST_MAX * sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	struct cxlmi_cmd_fmapi_set_ld_allocations_rsp *rsp = calloc(1, rsp_size);
	if (!rsp) {
		free(req);
		return -1;
	}

	rc = cxlmi_cmd_fmapi_set_ld_allocations(ep, ti, req, rsp);
	free(req);

	if (rc != 0) {
		fmd_warn(ctx, "%s: set LD %d allocation failed: %d",
			 mld->base.id, ld_id, rc);
		free(rsp);
		return -1;
	}

	/* Update local state from response */
	ld->memory_size = rsp->ld_allocation_list[0].range_1_allocation_mult * granularity_bytes;
	fmd_info(ctx, "%s: LD %d allocation set to %lu MB",
		 mld->base.id, ld_id,
		 (unsigned long)(ld->memory_size / (1024 * 1024)));

	free(rsp);
	return 0;
}


/*
 * Set LD allocation for an MHD via LD Pool CCI Set LD Allocations (5402h)
 *
 * MHD uses LD Pool CCI which is accessed via Tunnel Management command.
 * The pool_ld_id is the device-wide LD index (not head-local LD-ID).
 *
 * For mock mode, updates local state only.
 * For hardware mode, tunnels to LD Pool CCI and issues command.
 */
int fmd_mhd_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mhd *mhd,
			      uint8_t pool_ld_id, uint64_t size_bytes)
{
	struct cxlmi_endpoint *ep;
	struct fmd_ld *ld = NULL;
	struct fmd_component *child;
	uint64_t granularity_bytes;
	uint64_t allocation_mult;
	int rc;

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

	/* Calculate allocation in granularity units (256MB * memory_granularity) */
	granularity_bytes = (uint64_t)mhd->memory_granularity * CXL_CAPACITY_UNIT;
	if (granularity_bytes == 0)
		granularity_bytes = CXL_CAPACITY_UNIT;  /* Default 256MB */

	allocation_mult = size_bytes / granularity_bytes;

	/* Mock mode: update local state only */
	if (fmd_is_mock(ctx)) {
		fmd_info(ctx, "%s: mock set pool LD %d allocation to %lu MB",
			 mhd->base.id, pool_ld_id,
			 (unsigned long)(size_bytes / (1024 * 1024)));
		ld->memory_size = size_bytes;
		return 0;
	}

	/*
	 * Hardware mode: select head endpoint for FM access.
	 *
	 * MHD LD Pool CCI is accessed by tunneling through a head's Mailbox CCI.
	 * Each head has its own MCTP endpoint. We prefer:
	 * 1. The head that the target LD is bound to (if bound)
	 * 2. The first head with a valid endpoint
	 * 3. Fall back to fabric's endpoint
	 */
	struct fmd_head *head = NULL;
	struct fmd_component *hcomp;

	/* If LD is bound to a head, prefer that head */
	if (ld->binding_point_id != 0xFF) {
		fmd_for_each_child(&mhd->base, hcomp) {
			if (hcomp->type == FMD_COMPONENT_HEAD) {
				struct fmd_head *h = fmd_to_head(hcomp);
				if (h->head_id == ld->binding_point_id) {
					head = h;
					break;
				}
			}
		}
	}

	/* Otherwise find first head with an endpoint */
	if (!head) {
		fmd_for_each_child(&mhd->base, hcomp) {
			if (hcomp->type == FMD_COMPONENT_HEAD) {
				struct fmd_head *h = fmd_to_head(hcomp);
				if (h->ep) {
					head = h;
					break;
				}
			}
		}
	}

	/* Get endpoint from head, or fall back to the MHD's own */
	ep = head && head->ep ? head->ep : mhd->base.ep;
	if (!ep) {
		fmd_warn(ctx, "%s: no endpoint for hardware mode", mhd->base.id);
		return -1;
	}

	fmd_dbg(ctx, "%s: tunneling through head %d to LD Pool CCI",
		mhd->base.id, head ? head->head_id : -1);

	/*
	 * Hardware mode: tunnel to LD Pool CCI and issue command.
	 * The Tunnel Management Command with target_type=LD_POOL_CCI routes
	 * the command to the device-wide LD Pool management interface.
	 */

	/* Build request - update single LD */
	size_t req_size = sizeof(struct cxlmi_cmd_fmapi_set_ld_allocations_req) +
			  sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	struct cxlmi_cmd_fmapi_set_ld_allocations_req *req = calloc(1, req_size);
	if (!req)
		return -1;

	req->number_ld = 1;
	req->start_ld_id = pool_ld_id;
	req->ld_allocation_list[0].range_1_allocation_mult = allocation_mult;
	req->ld_allocation_list[0].range_2_allocation_mult = 0;

	/* Build response buffer */
	size_t rsp_size = sizeof(struct cxlmi_cmd_fmapi_set_ld_allocations_rsp) +
			  FMD_CCI_LIST_MAX * sizeof(struct cxlmi_cmd_fmapi_ld_allocations_list);
	struct cxlmi_cmd_fmapi_set_ld_allocations_rsp *rsp = calloc(1, rsp_size);
	if (!rsp) {
		free(req);
		return -1;
	}

	/* Send command to LD Pool CCI (endpoint already bound to specific head) */
	rc = cxlmi_cmd_fmapi_set_ld_allocations(ep, CXLMI_TUNNEL_MHD, req, rsp);
	free(req);

	if (rc != 0) {
		fmd_warn(ctx, "%s: set pool LD %d allocation failed: %d",
			 mhd->base.id, pool_ld_id, rc);
		free(rsp);
		return -1;
	}

	/* Update local state from response */
	ld->memory_size = rsp->ld_allocation_list[0].range_1_allocation_mult * granularity_bytes;
	fmd_info(ctx, "%s: pool LD %d allocation set to %lu MB",
		 mhd->base.id, pool_ld_id,
		 (unsigned long)(ld->memory_size / (1024 * 1024)));

	free(rsp);
	return 0;
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
 * Set volatile/persistent partition for an SLD via CCI Set Partition Info (4101h)
 *
 * For mock mode, updates local state only.
 * For hardware mode, issues the CCI command and updates local state on success.
 *
 * The volatile_bytes is converted to 256MB CXL capacity units.
 * If immediate is true, change takes effect immediately; else at next reset.
 */
int fmd_sld_set_partition(struct fmd_ctx *ctx, struct fmd_sld *sld,
			  uint64_t volatile_bytes, bool immediate)
{
	struct cxlmi_endpoint *ep;
	struct cxlmi_cmd_memdev_set_partition_info_req req = {0};
	uint64_t volatile_capacity;
	int rc;

	if (!ctx || !sld)
		return -1;

	/* Convert bytes to 256MB CXL capacity units */
	volatile_capacity = volatile_bytes / CXL_CAPACITY_UNIT;

	/* Mock mode: update local state only */
	ep = sld->base.ep;
	if (!ep) {
		fmd_info(ctx, "%s: mock set partition volatile=%lu MB",
			 sld->base.id,
			 (unsigned long)(volatile_bytes / (1024 * 1024)));
		sld->partition.next_vmem = volatile_capacity;
		if (immediate) {
			sld->partition.active_vmem = volatile_capacity;
			sld->volatile_size = volatile_bytes;
			sld->persistent_size = sld->memory_size - volatile_bytes;
		}
		return 0;
	}

	/* Hardware mode: issue CCI command */
	req.volatile_capacity = volatile_capacity;
	req.flags = immediate ? 0x01 : 0x00;

	rc = cxlmi_cmd_memdev_set_partition_info(ep, CXLMI_NO_TUNNEL, &req);
	if (rc != 0) {
		fmd_warn(ctx, "%s: set partition info failed: %d", sld->base.id, rc);
		return -1;
	}

	/* Update local state */
	sld->partition.next_vmem = volatile_capacity;
	if (immediate) {
		sld->partition.active_vmem = volatile_capacity;
		sld->volatile_size = volatile_bytes;
		sld->persistent_size = sld->memory_size - volatile_bytes;
	}

	fmd_info(ctx, "%s: partition set to volatile=%lu MB (immediate=%d)",
		 sld->base.id,
		 (unsigned long)(volatile_bytes / (1024 * 1024)),
		 immediate);

	return 0;
}
