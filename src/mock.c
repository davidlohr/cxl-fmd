/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Mock Topology Implementation
 *
 * Parses JSON topology files and creates simulated CXL devices
 * for testing without real hardware.
 *
 * Builds the topology tree directly using fmd_switch, fmd_port,
 * fmd_sld, fmd_mld, fmd_ld components.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "cxl-fmd.h"
#include "mock.h"


/* Helper: parse hex string like "0x8086" to uint16_t */
static uint16_t parse_hex16(json_t *val)
{
	if (json_is_string(val)) {
		const char *s = json_string_value(val);
		return (uint16_t)strtoul(s, NULL, 0);
	}
	return (uint16_t)json_integer_value(val);
}

/* Helper: parse hex string to uint64_t */
static uint64_t parse_hex64(json_t *val)
{
	if (json_is_string(val)) {
		const char *s = json_string_value(val);
		return strtoull(s, NULL, 0);
	}
	return (uint64_t)json_integer_value(val);
}

/* Helper: get integer with default */
static int64_t json_int_or(json_t *obj, const char *key, int64_t def)
{
	json_t *val = json_object_get(obj, key);
	if (!val)
		return def;
	if (json_is_string(val))
		return strtoll(json_string_value(val), NULL, 0);
	return json_integer_value(val);
}

/* Helper: get unsigned integer with default (negative values become 0) */
static uint64_t json_uint_or(json_t *obj, const char *key, uint64_t def)
{
	int64_t val = json_int_or(obj, key, (int64_t)def);
	return (val < 0) ? 0 : (uint64_t)val;
}

/* Helper: get string with default */
static const char *json_str_or(json_t *obj, const char *key, const char *def)
{
	json_t *val = json_object_get(obj, key);
	if (!val || !json_is_string(val))
		return def;
	return json_string_value(val);
}

/*
 * Parse supported commands from CEL array
 */
static uint64_t parse_cel_commands(json_t *jcel)
{
	uint64_t cmds = 0;
	size_t idx;
	json_t *jcmd;

	if (!jcel || !json_is_array(jcel))
		return 0;

	json_array_foreach(jcel, idx, jcmd) {
		if (!json_is_string(jcmd))
			continue;
		const char *cmd = json_string_value(jcmd);

		if (strcmp(cmd, "identify") == 0)
			cmds |= FMD_CMD_IDENTIFY;
		else if (strcmp(cmd, "bg_op_status") == 0)
			cmds |= FMD_CMD_BG_OP_STATUS;
		else if (strcmp(cmd, "get_supported_logs") == 0)
			cmds |= FMD_CMD_GET_SUPPORTED_LOGS;
		else if (strcmp(cmd, "get_log") == 0)
			cmds |= FMD_CMD_GET_LOG;
		else if (strcmp(cmd, "memdev_identify") == 0)
			cmds |= FMD_CMD_MEMDEV_IDENTIFY;
		else if (strcmp(cmd, "memdev_capacity") == 0)
			cmds |= FMD_CMD_MEMDEV_CAPACITY;
		else if (strcmp(cmd, "get_partition_info") == 0)
			cmds |= FMD_CMD_GET_PARTITION_INFO;
		else if (strcmp(cmd, "set_partition_info") == 0)
			cmds |= FMD_CMD_SET_PARTITION_INFO;
		else if (strcmp(cmd, "get_health_info") == 0)
			cmds |= FMD_CMD_GET_HEALTH_INFO;
		else if (strcmp(cmd, "get_alert_config") == 0)
			cmds |= FMD_CMD_GET_ALERT_CONFIG;
		else if (strcmp(cmd, "set_alert_config") == 0)
			cmds |= FMD_CMD_SET_ALERT_CONFIG;
		else if (strcmp(cmd, "get_shutdown_state") == 0)
			cmds |= FMD_CMD_GET_SHUTDOWN_STATE;
		else if (strcmp(cmd, "set_shutdown_state") == 0)
			cmds |= FMD_CMD_SET_SHUTDOWN_STATE;
		else if (strcmp(cmd, "get_poison_list") == 0)
			cmds |= FMD_CMD_GET_POISON_LIST;
		else if (strcmp(cmd, "inject_poison") == 0)
			cmds |= FMD_CMD_INJECT_POISON;
		else if (strcmp(cmd, "clear_poison") == 0)
			cmds |= FMD_CMD_CLEAR_POISON;
	}

	return cmds;
}


static struct fmd_sld *parse_sld(struct fmd_ctx *ctx, json_t *jdev,
				 const char *dev_id)
{
	struct fmd_sld *sld;
	json_t *obj;

	(void)ctx;

	sld = fmd_sld_create(dev_id);
	if (!sld)
		return NULL;

	/* Identify info */
	sld->identify.vendor_id = parse_hex16(json_object_get(jdev, "vendor_id"));
	sld->identify.device_id = parse_hex16(json_object_get(jdev, "device_id"));
	sld->identify.serial_num = parse_hex64(json_object_get(jdev, "serial_number"));
	sld->identify.component_type = CXL_COMPONENT_TYPE3;

	/* SLDs don't store any access info - uses fabric's ep, no tunneling */

	/* Identify Memory Device section (CCI 0x4000) */
	obj = json_object_get(jdev, "identify_memory_device");
	if (obj) {
		const char *fw = json_str_or(obj, "fw_revision", "0.0.0");
		(void)snprintf(sld->memdev.fw_revision,
			       sizeof(sld->memdev.fw_revision), "%s", fw);

		/* Capacities in MB from JSON, stored in 256MB units (CCI format) */
		sld->memdev.total_capacity =
			json_uint_or(obj, "total_capacity_mb", 0) / 256;
		sld->memdev.volatile_capacity =
			json_uint_or(obj, "volatile_capacity_mb", 0) / 256;
		sld->memdev.persistent_capacity =
			json_uint_or(obj, "persistent_capacity_mb", 0) / 256;
		sld->memdev.partition_align =
			json_uint_or(obj, "partition_align_mb", 256) / 256;
		sld->memdev.lsa_size =
			(uint32_t)json_int_or(obj, "lsa_size", 0);
	}

	/* Compute memory sizes */
	sld->memory_size = CXL_TO_BYTES(sld->memdev.total_capacity);
	sld->volatile_size = CXL_TO_BYTES(sld->memdev.volatile_capacity);
	sld->persistent_size = CXL_TO_BYTES(sld->memdev.persistent_capacity);

	/* Partition Info section (CCI 0x4100) - values in 256MB units */
	obj = json_object_get(jdev, "partition_info");
	if (obj) {
		sld->partition.active_vmem =
			json_uint_or(obj, "active_volatile_mb", 0) / 256;
		sld->partition.active_pmem =
			json_uint_or(obj, "active_persistent_mb", 0) / 256;
		sld->partition.next_vmem =
			json_uint_or(obj, "next_volatile_mb", 0) / 256;
		sld->partition.next_pmem =
			json_uint_or(obj, "next_persistent_mb", 0) / 256;
	} else {
		/* Default: all capacity is active */
		sld->partition.active_pmem = sld->memdev.persistent_capacity;
		sld->partition.active_vmem = sld->memdev.volatile_capacity;
		sld->partition.next_pmem = sld->memdev.persistent_capacity;
		sld->partition.next_vmem = sld->memdev.volatile_capacity;
	}

	/* Health Info section (CCI 0x4200) */
	obj = json_object_get(jdev, "health_info");
	if (obj) {
		sld->health.health_status =
			(uint8_t)json_int_or(obj, "health_status", 0);
		sld->health.media_status =
			(uint8_t)json_int_or(obj, "media_status", 0);
		sld->health.life_used =
			(uint8_t)json_int_or(obj, "life_used", 0);
		sld->health.device_temperature =
			(uint16_t)json_int_or(obj, "temperature", 25);
		sld->health.dirty_shutdown_count =
			(uint32_t)json_int_or(obj, "dirty_shutdown_count", 0);
	}

	/* Supported Commands (CEL) */
	json_t *jcel = json_object_get(jdev, "cel_commands");
	if (jcel && json_is_array(jcel)) {
		sld->supported_cmds = parse_cel_commands(jcel);
	} else {
		sld->supported_cmds = FMD_CMD_DEFAULT_MEMDEV;
	}

	return sld;
}


static struct fmd_mld *parse_mld(struct fmd_ctx *ctx, json_t *jdev,
				 const char *dev_id)
{
	struct fmd_mld *mld;
	json_t *obj;
	json_t *jcel;

	(void)ctx;

	mld = fmd_mld_create(dev_id);
	if (!mld)
		return NULL;

	/* Identify info */
	mld->identify.vendor_id = parse_hex16(json_object_get(jdev, "vendor_id"));
	mld->identify.device_id = parse_hex16(json_object_get(jdev, "device_id"));
	mld->identify.serial_num = parse_hex64(json_object_get(jdev, "serial_number"));
	mld->identify.component_type = CXL_COMPONENT_TYPE3;

	/* switch_port will be set by caller if MLD is behind a switch */
	mld->switch_port = -1;  /* Direct-attached by default */

	/* LD count and allocation granularity (from Get LD Info 5400h) */
	mld->ld_count = (uint8_t)json_int_or(jdev, "ld_count", 1);
	mld->max_ld_count = (uint8_t)json_int_or(jdev, "max_ld_count", mld->ld_count);
	mld->memory_granularity = (uint8_t)json_int_or(jdev, "memory_granularity", 1);
	mld->qos_telemetry_capability = (uint8_t)json_int_or(jdev, "qos_telemetry_capability", 0);

	/* Supported Commands (CEL) */
	jcel = json_object_get(jdev, "cel_commands");
	if (jcel && json_is_array(jcel)) {
		mld->supported_cmds = parse_cel_commands(jcel);
	} else {
		mld->supported_cmds = FMD_CMD_DEFAULT_MEMDEV;
	}

	/* QoS Control (5403h) */
	obj = json_object_get(jdev, "qos_control");
	if (obj) {
		mld->qos_control.enabled = json_is_true(json_object_get(obj, "enabled"));
		mld->qos_control.temp_throughput_reduction_enabled =
			json_is_true(json_object_get(obj, "temp_throughput_reduction_enabled"));
		mld->qos_control.egress_moderate_pct =
			(uint8_t)json_int_or(obj, "egress_moderate_pct", 10);
		mld->qos_control.egress_severe_pct =
			(uint8_t)json_int_or(obj, "egress_severe_pct", 25);
		mld->qos_control.backpressure_sample_interval =
			(uint8_t)json_int_or(obj, "backpressure_sample_interval", 0);
		mld->qos_control.reqcmp_basis =
			(uint8_t)json_int_or(obj, "reqcmp_basis", 0);
		mld->qos_control.completion_collection_interval =
			(uint16_t)json_int_or(obj, "completion_collection_interval", 0);
	}

	/* QoS Status (5405h) */
	obj = json_object_get(jdev, "qos_status");
	if (obj) {
		mld->qos_status.backpressure_avg_valid = true;
		mld->qos_status.backpressure_avg_pct =
			(uint8_t)json_int_or(obj, "backpressure_avg_pct", 0);
	}

	return mld;
}

static struct fmd_ld *parse_ld(struct fmd_ctx *ctx, json_t *jld,
			       const char *mld_id, int ld_idx,
			       uint8_t default_health, uint8_t default_media)
{
	char ld_id_str[128];
	struct fmd_ld *ld;
	json_t *obj;
	uint8_t ld_num;
	uint64_t total_mb;
	uint64_t volatile_mb;
	uint64_t persistent_mb;
	int alloc_pct;
	int limit_pct;
	int alloc_cxl;
	int limit_cxl;

	(void)ctx;

	(void)snprintf(ld_id_str, sizeof(ld_id_str), "%.50s_ld%d", mld_id, ld_idx);

	ld_num = (uint8_t)json_int_or(jld, "ld_id", ld_idx);
	ld = fmd_ld_create(ld_id_str, ld_num);
	if (!ld)
		return NULL;

	/* Memory size (total, volatile, persistent) */
	ld->memory_size = json_uint_or(jld, "memory_size_mb", 0) * 1024ULL * 1024;

	/* Per-LD memdev identify info (volatile/persistent split) */
	total_mb = json_uint_or(jld, "memory_size_mb", 0);
	volatile_mb = json_uint_or(jld, "volatile_capacity_mb", 0);
	persistent_mb = json_uint_or(jld, "persistent_capacity_mb", 0);

	/* If no split specified, default all to persistent */
	if (volatile_mb == 0 && persistent_mb == 0)
		persistent_mb = total_mb;

	ld->memdev.total_capacity = total_mb / 256;      /* CCI uses 256MB units */
	ld->memdev.volatile_capacity = volatile_mb / 256;
	ld->memdev.persistent_capacity = persistent_mb / 256;

	/*
	 * QoS - JSON specifies percentages (0-100), convert to CXL format.
	 * QoS Allocation Fraction is "fraction multiplied by 256",
	 * e.g. 80% = 0.80 * 256 = 204.8. Add 50 before dividing by 100 to round.
	 */
	alloc_pct = (int)json_int_or(jld, "qos_allocated_bw", 100);
	limit_pct = (int)json_int_or(jld, "qos_limit_bw", 100);
	alloc_cxl = (alloc_pct * 256 + 50) / 100;
	limit_cxl = (limit_pct * 256 + 50) / 100;
	ld->qos_allocated_bw = (uint8_t)(alloc_cxl > 255 ? 255 : alloc_cxl);
	ld->qos_limit_bw = (uint8_t)(limit_cxl > 255 ? 255 : limit_cxl);

	/*
	 * Note: is_fm_owned field removed. All LDs (0-15) are host-bindable.
	 * The FMLD at LD-ID FFFFh is separate and accessed implicitly via
	 * MLD component commands.
	 */

	/* Per-LD health */
	obj = json_object_get(jld, "health_info");
	if (obj) {
		ld->health.health_status = (uint8_t)json_int_or(obj, "health_status", 0);
		ld->health.media_status = (uint8_t)json_int_or(obj, "media_status", 0);
	} else {
		ld->health.health_status = default_health;
		ld->health.media_status = default_media;
	}

	return ld;
}


/*
 * Parse a head from JSON (child of MHD)
 */
static struct fmd_head *parse_head(struct fmd_ctx *ctx, json_t *jhead,
				   const char *mhd_id, int head_idx)
{
	char head_id_str[FMD_MAX_ID_LEN];
	struct fmd_head *head;
	uint8_t head_num;
	const char *pres;
	const char *attached;
	json_t *jlds;

	(void)ctx;

	(void)snprintf(head_id_str, sizeof(head_id_str), "%.50s_head%d", mhd_id, head_idx);

	head_num = (uint8_t)json_int_or(jhead, "head_id", head_idx);
	head = fmd_head_create(head_id_str, head_num);
	if (!head)
		return NULL;

	/* Parse presentation type (SLD or MLD) */
	pres = json_str_or(jhead, "presentation", "SLD");
	if (strcmp(pres, "MLD") == 0)
		head->presentation = FMD_HEAD_MLD;
	else
		head->presentation = FMD_HEAD_SLD;

	/* Head state - default to enabled */
	head->state = FMD_HEAD_ENABLED;

	/* Link properties */
	head->max_link_width = (uint8_t)json_int_or(jhead, "max_link_width", 16);
	head->negotiated_link_width = (uint8_t)json_int_or(jhead, "negotiated_link_width", 16);

	/* Attached host info */
	attached = json_str_or(jhead, "attached_host", NULL);
	if (attached)
		snprintf(head->attached_host_id, sizeof(head->attached_host_id), "%s", attached);

	/* Count LDs assigned to this head from the lds array */
	jlds = json_object_get(jhead, "lds");
	if (jlds && json_is_array(jlds))
		head->ld_count = (uint8_t)json_array_size(jlds);
	else
		head->ld_count = 0;

	return head;
}

/*
 * Parse MHD (Multi-Headed Device) from JSON
 */
static struct fmd_mhd *parse_mhd(struct fmd_ctx *ctx, json_t *jdev,
				 const char *dev_id)
{
	struct fmd_mhd *mhd;
	json_t *obj;
	json_t *jcel;

	(void)ctx;

	mhd = fmd_mhd_create(dev_id);
	if (!mhd)
		return NULL;

	/* Identify info */
	mhd->identify.vendor_id = parse_hex16(json_object_get(jdev, "vendor_id"));
	mhd->identify.device_id = parse_hex16(json_object_get(jdev, "device_id"));
	mhd->identify.serial_num = parse_hex64(json_object_get(jdev, "serial_number"));
	mhd->identify.component_type = CXL_COMPONENT_TYPE3;

	/* MHD configuration */
	mhd->num_heads = (uint8_t)json_int_or(jdev, "num_heads", 2);
	mhd->max_heads = (uint8_t)json_int_or(jdev, "max_heads", mhd->num_heads);
	mhd->dynamic_capacity = json_is_true(json_object_get(jdev, "dynamic_capacity"));
	mhd->memory_granularity = (uint8_t)json_int_or(jdev, "memory_granularity", 1);
	mhd->qos_telemetry_capability = (uint8_t)json_int_or(jdev, "qos_telemetry_capability", 0);

	/* Supported Commands (CEL) */
	jcel = json_object_get(jdev, "cel_commands");
	if (jcel && json_is_array(jcel)) {
		mhd->supported_cmds = parse_cel_commands(jcel);
	} else {
		mhd->supported_cmds = FMD_CMD_DEFAULT_MEMDEV;
	}

	/* QoS Control */
	obj = json_object_get(jdev, "qos_control");
	if (obj) {
		mhd->qos_control.enabled = json_is_true(json_object_get(obj, "enabled"));
		mhd->qos_control.temp_throughput_reduction_enabled =
			json_is_true(json_object_get(obj, "temp_throughput_reduction_enabled"));
		mhd->qos_control.egress_moderate_pct =
			(uint8_t)json_int_or(obj, "egress_moderate_pct", 10);
		mhd->qos_control.egress_severe_pct =
			(uint8_t)json_int_or(obj, "egress_severe_pct", 25);
	}

	/* QoS Status */
	obj = json_object_get(jdev, "qos_status");
	if (obj) {
		mhd->qos_status.backpressure_avg_valid = true;
		mhd->qos_status.backpressure_avg_pct =
			(uint8_t)json_int_or(obj, "backpressure_avg_pct", 0);
	}

	return mhd;
}

/*
 * Parse LD pool entry for MHD (similar to parse_ld but with MHD context)
 */
static struct fmd_ld *parse_ld_pool_entry(struct fmd_ctx *ctx, json_t *jld,
					  const char *mhd_id, int pool_idx,
					  uint8_t default_health, uint8_t default_media)
{
	char ld_id_str[128];
	struct fmd_ld *ld;
	json_t *obj;
	uint8_t ld_num;
	uint64_t total_mb;
	uint64_t volatile_mb;
	uint64_t persistent_mb;
	int alloc_pct;
	int limit_pct;
	int alloc_cxl;
	int limit_cxl;

	(void)ctx;

	(void)snprintf(ld_id_str, sizeof(ld_id_str), "%.50s_ld%d", mhd_id, pool_idx);

	ld_num = (uint8_t)json_int_or(jld, "pool_index", pool_idx);
	ld = fmd_ld_create(ld_id_str, ld_num);
	if (!ld)
		return NULL;

	/* Memory size */
	ld->memory_size = json_uint_or(jld, "memory_size_mb", 0) * 1024ULL * 1024;

	/* Per-LD memdev identify info (volatile/persistent split) */
	total_mb = json_uint_or(jld, "memory_size_mb", 0);
	volatile_mb = json_uint_or(jld, "volatile_capacity_mb", 0);
	persistent_mb = json_uint_or(jld, "persistent_capacity_mb", 0);

	/* If no split specified, default all to persistent */
	if (volatile_mb == 0 && persistent_mb == 0)
		persistent_mb = total_mb;

	ld->memdev.total_capacity = total_mb / 256;
	ld->memdev.volatile_capacity = volatile_mb / 256;
	ld->memdev.persistent_capacity = persistent_mb / 256;

	/* QoS */
	alloc_pct = (int)json_int_or(jld, "qos_allocated_bw", 100);
	limit_pct = (int)json_int_or(jld, "qos_limit_bw", 100);
	alloc_cxl = (alloc_pct * 256 + 50) / 100;
	limit_cxl = (limit_pct * 256 + 50) / 100;
	ld->qos_allocated_bw = (uint8_t)(alloc_cxl > 255 ? 255 : alloc_cxl);
	ld->qos_limit_bw = (uint8_t)(limit_cxl > 255 ? 255 : limit_cxl);

	/* Initially unbound */
	ld->binding_point_id = 0xFF;
	ld->visible_ld_id = ld_num;

	/* Per-LD health */
	obj = json_object_get(jld, "health_info");
	if (obj) {
		ld->health.health_status = (uint8_t)json_int_or(obj, "health_status", 0);
		ld->health.media_status = (uint8_t)json_int_or(obj, "media_status", 0);
	} else {
		ld->health.health_status = default_health;
		ld->health.media_status = default_media;
	}

	return ld;
}

/*
 * Update LD binding based on head's lds array
 */
static void update_ld_bindings_for_head(struct fmd_mhd *mhd, struct fmd_head *head,
					json_t *jlds_array)
{
	size_t idx;
	json_t *jld_idx;
	uint8_t visible_id = 0;

	if (!jlds_array || !json_is_array(jlds_array))
		return;

	json_array_foreach(jlds_array, idx, jld_idx) {
		int pool_index = (int)json_integer_value(jld_idx);
		struct fmd_component *child;

		/* Find the LD with this pool index */
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			if (ld->ld_id == pool_index) {
				/* Bind this LD to the head */
				ld->binding_point_id = head->head_id;
				ld->visible_ld_id = visible_id++;
				break;
			}
		}
	}
}

/*
 * ============================================================================
 * Dynamic Capacity ("dcd" object on a Type 3 device)
 * ============================================================================
 *
 * Mirrors the FM-API DCD Management responses:
 *   - top level            -> Get DCD Info (5600h)
 *   - "regions"[]          -> Get Host DC Region Configuration (5601h)
 *   - "extents"[]          -> Get DC Region Extent Lists (5603h) + List Tags (5608h)
 * See docs/mock-topology.md "Dynamic Capacity".
 */

/* Policy names (Redfish spelling) into a 5600h capability mask; @def if absent */
static uint16_t parse_policies(json_t *arr, bool release, uint16_t def)
{
	uint16_t mask = 0;
	size_t idx;
	json_t *v;

	if (!arr || !json_is_array(arr))
		return def;

	json_array_foreach(arr, idx, v) {
		int p = fmd_dc_policy_from_str(release, json_string_value(v));

		if (p >= 0)
			mask |= fmd_dc_policy_cap_bit(release, p);
	}
	return mask;
}

/* "hosts": [..] or "host_id": N into a host bitmask; default is @def_mask */
static uint64_t parse_host_mask(json_t *obj, uint64_t def_mask)
{
	json_t *jhosts = json_object_get(obj, "hosts");
	json_t *jhost = json_object_get(obj, "host_id");
	uint64_t mask = 0;
	size_t idx;
	json_t *v;

	if (jhosts && json_is_array(jhosts)) {
		json_array_foreach(jhosts, idx, v) {
			if (json_is_integer(v))
				mask |= fmd_dc_host_bit((int)json_integer_value(v));
		}
		return mask;
	}
	if (jhost && json_is_integer(jhost) && fmd_dc_host_bit((int)json_integer_value(jhost)))
		return fmd_dc_host_bit((int)json_integer_value(jhost));

	return def_mask;
}

static int parse_dcd(struct fmd_ctx *ctx, json_t *jdev, struct fmd_component *dev)
{
	json_t *jdcd = json_object_get(jdev, "dcd");
	struct fmd_dcd_info *dcd;
	struct fmd_component *child;
	uint64_t region_base[FMD_MAX_DC_REGIONS] = {0};
	uint64_t region_len[FMD_MAX_DC_REGIONS] = {0};
	uint64_t all_hosts;
	uint32_t max_extents, max_tags;
	int default_hosts;
	int n_regions = 0, n_extents = 0;
	json_t *arr, *item;
	size_t idx;

	if (!jdcd || !json_is_object(jdcd))
		return 0;

	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		default_hosts = 1;
		break;
	case FMD_COMPONENT_MLD:
		default_hosts = fmd_to_mld(dev)->ld_count;
		break;
	case FMD_COMPONENT_MHD:
		/* Host IDs are pool LD indices, see docs/DCD-FM-API.md */
		default_hosts = fmd_to_mhd(dev)->total_lds;
		break;
	default:
		return -1;
	}

	dcd = calloc(1, sizeof(*dcd));
	if (!dcd)
		return -1;

	/* Get DCD Info (5600h) */
	dcd->num_hosts = (uint8_t)json_int_or(jdcd, "num_hosts", default_hosts);
	if (dcd->num_hosts > FMD_MAX_DC_HOSTS)
		dcd->num_hosts = FMD_MAX_DC_HOSTS;
	dcd->add_policies = parse_policies(json_object_get(jdcd, "add_policies"), false,
					   FMD_DC_ADD_FREE | FMD_DC_ADD_CONTIGUOUS |
					   FMD_DC_ADD_PRESCRIPTIVE);
	dcd->release_policies = parse_policies(json_object_get(jdcd, "release_policies"), true,
					       FMD_DC_REL_TAG_BASED | FMD_DC_REL_PRESCRIPTIVE);
	max_extents = (uint32_t)json_int_or(jdcd, "max_extents", 256);
	max_tags = (uint32_t)json_int_or(jdcd, "max_tags", 64);
	dcd->extents_supported = dcd->extents_available = max_extents;
	dcd->tags_supported = dcd->tags_available = max_tags;
	all_hosts = (dcd->num_hosts >= 64) ? ~0ULL : (1ULL << dcd->num_hosts) - 1;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		ld->dc_extents_supported = ld->dc_extents_available = max_extents;
		ld->dc_tags_supported = ld->dc_tags_available = max_tags;
	}

	/* Regions: Get Host DC Region Configuration (5601h), one copy per host */
	arr = json_object_get(jdcd, "regions");
	if (arr && json_is_array(arr)) {
		json_array_foreach(arr, idx, item) {
			int rid = (int)json_int_or(item, "region_id", (int64_t)idx);
			uint64_t len_mb = json_uint_or(item, "region_len_mb", 0);
			uint64_t base_mb = json_uint_or(item, "base_mb", 0);
			uint64_t decode_mb = json_uint_or(item, "decode_len_mb", len_mb);
			uint64_t bs_mb = json_uint_or(item, "block_size_mb", 256);
			bool sanitize = json_is_true(json_object_get(item, "sanitize_on_release"));
			json_t *jcfg = json_object_get(item, "sanitize_configurable");
			json_t *jbs = json_object_get(item, "supported_block_sizes_mb");
			uint64_t hosts;
			uint8_t flags = 0;

			if (rid < 0 || rid >= FMD_MAX_DC_REGIONS || len_mb == 0 || bs_mb == 0) {
				fmd_warn(ctx, "%s: dcd region %d: invalid or empty, skipping",
					 dev->id, rid);
				continue;
			}

			if (rid + 1 > dcd->num_regions)
				dcd->num_regions = (uint8_t)(rid + 1);
			region_base[rid] = base_mb * FMD_MIB;
			region_len[rid] = len_mb * FMD_MIB;

			if (json_is_true(json_object_get(item, "shareable")))
				flags |= FMD_DC_REGION_SHAREABLE;
			if (json_is_true(json_object_get(item, "nonvolatile")))
				flags |= FMD_DC_REGION_NONVOLATILE;
			if (json_is_true(json_object_get(item, "hw_coherency")))
				flags |= FMD_DC_REGION_HW_COHERENT;
			if (json_is_true(json_object_get(item, "interconnect_dcm")))
				flags |= FMD_DC_REGION_INTERCONNECT_DCM;
			if (json_is_true(json_object_get(item, "read_only")))
				flags |= FMD_DC_REGION_READ_ONLY;
			if (!jcfg || json_is_true(jcfg))
				dcd->sanitize_cfg_mask |= (uint8_t)(1 << rid);

			if (jbs && json_is_array(jbs)) {
				size_t bidx;
				json_t *jb;
				json_array_foreach(jbs, bidx, jb) {
					if (json_is_integer(jb) && json_integer_value(jb) > 0)
						dcd->blk_sz_mask[rid] |=
							1ULL << (20 + fmd_ilog2_u64((uint64_t)json_integer_value(jb)));
				}
			}
			dcd->blk_sz_mask[rid] |= 1ULL << (20 + fmd_ilog2_u64(bs_mb));

			hosts = parse_host_mask(item, all_hosts);
			for (int h = 0; h < dcd->num_hosts; h++) {
				struct fmd_component *owner;
				struct fmd_dc_region *r;

				if (!(hosts & fmd_dc_host_bit(h)))
					continue;
				owner = fmd_dc_host_component(dev, h);
				if (!owner) {
					fmd_warn(ctx, "%s: dcd region %d: host %d has no logical device, skipping",
						 dev->id, rid, h);
					continue;
				}

				r = fmd_dc_region_add(ctx, owner, (uint8_t)rid, (uint16_t)h);
				if (!r)
					continue;
				r->base_dpa = region_base[rid];
				r->decode_len = decode_mb * FMD_MIB;
				r->region_len = region_len[rid];
				r->block_size = bs_mb * FMD_MIB;
				r->flags = flags;
				r->sanitize_on_release = sanitize;
				n_regions++;
			}
		}
	}

	dcd->total_dc_capacity = json_uint_or(jdcd, "total_capacity_mb", 0) * FMD_MIB;
	if (dcd->total_dc_capacity == 0) {
		for (int r = 0; r < FMD_MAX_DC_REGIONS; r++)
			dcd->total_dc_capacity += region_len[r];
	}

	/* Attach before extents so the counters can be adjusted */
	switch (dev->type) {
	case FMD_COMPONENT_SLD:
		fmd_to_sld(dev)->dcd = dcd;
		break;
	case FMD_COMPONENT_MLD:
		fmd_to_mld(dev)->dcd = dcd;
		break;
	default:
		fmd_to_mhd(dev)->dcd = dcd;
		fmd_to_mhd(dev)->dynamic_capacity = true;
		break;
	}

	/* Extents: Get DC Region Extent Lists (5603h) + DC List Tags (5608h) */
	arr = json_object_get(jdcd, "extents");
	if (arr && json_is_array(arr)) {
		json_array_foreach(arr, idx, item) {
			uint64_t dpa = json_uint_or(item, "start_mb", 0) * FMD_MIB;
			uint64_t len = json_uint_or(item, "len_mb", 0) * FMD_MIB;
			const char *tag_str = json_str_or(item, "tag", "");
			uint64_t hosts = parse_host_mask(item, 1ULL);
			uint8_t tag[FMD_DC_TAG_LEN];
			struct fmd_dc_extent *ext;
			int rid = -1;

			for (int r = 0; r < FMD_MAX_DC_REGIONS; r++) {
				if (region_len[r] && dpa >= region_base[r] &&
				    dpa < region_base[r] + region_len[r]) {
					rid = r;
					break;
				}
			}
			if (len == 0 || rid < 0) {
				fmd_warn(ctx, "%s: dcd extent at %" PRIu64 " MB is empty or outside every region, skipping",
					 dev->id, (uint64_t)(dpa / FMD_MIB));
				continue;
			}

			if (fmd_dc_tag_from_str(tag_str, tag) < 0)
				fmd_warn(ctx, "%s: dcd extent at %" PRIu64 " MB: invalid tag '%s'",
					 dev->id, (uint64_t)(dpa / FMD_MIB), tag_str);
			ext = fmd_dc_extent_add(ctx, dev, dpa, len, tag,
						(uint16_t)json_int_or(item, "shared_seq", 0),
						(uint8_t)rid);
			if (!ext)
				continue;
			ext->host_mask = hosts & all_hosts;
			ext->pending_mask = json_is_true(json_object_get(item, "pending")) ?
				ext->host_mask : 0;
			ext->fm_referenced = json_is_true(json_object_get(item, "fm_referenced"));
			n_extents++;

			for (int h = 0; h < dcd->num_hosts; h++) {
				if (ext->host_mask & fmd_dc_host_bit(h))
					fmd_dc_adjust_available_extents(dev, h, -1);
			}
		}
	}

	fmd_info(ctx, "Mock DCD %s: %d hosts, %d regions (%d host views), %d extents, %" PRIu64 " MB dynamic",
		 dev->id, dcd->num_hosts, dcd->num_regions, n_regions, n_extents,
		 (uint64_t)(dcd->total_dc_capacity / FMD_MIB));

	return 0;
}

static int parse_device(struct fmd_ctx *ctx, json_t *jdev,
			struct fmd_component *parent)
{
	const char *id;
	const char *type_str;
	int ld_count;

	id = json_str_or(jdev, "id", NULL);
	if (!id || id[0] == '\0') {
		fmd_err(ctx, "Mock device missing or empty 'id' field");
		return -1;
	}

	/* Check for explicit type field first */
	type_str = json_str_or(jdev, "type", NULL);

	/* Handle MHD type explicitly */
	if (type_str && strcmp(type_str, "MHD") == 0) {
		struct fmd_mhd *mhd = parse_mhd(ctx, jdev, id);
		if (!mhd)
			return -1;

		fmd_topology_add(&ctx->topology, &mhd->base, parent);

		/* Get default health for LDs */
		json_t *health_obj = json_object_get(jdev, "health_info");
		uint8_t def_health = health_obj ?
			(uint8_t)json_int_or(health_obj, "health_status", 0) : 0;
		uint8_t def_media = health_obj ?
			(uint8_t)json_int_or(health_obj, "media_status", 0) : 0;

		/* Parse LD pool */
		json_t *ld_pool = json_object_get(jdev, "ld_pool");
		if (ld_pool && json_is_array(ld_pool)) {
			size_t ld_idx;
			json_t *jld;

			json_array_foreach(ld_pool, ld_idx, jld) {
				if (ld_idx >= FMD_MAX_LD_POOL)
					break;

				struct fmd_ld *ld = parse_ld_pool_entry(ctx, jld, id, ld_idx,
									def_health, def_media);
				if (ld)
					fmd_topology_add(&ctx->topology, &ld->base, &mhd->base);
			}

			mhd->total_lds = (uint8_t)json_array_size(ld_pool);
			mhd->max_lds = mhd->total_lds;
		}

		/* Parse heads */
		json_t *heads = json_object_get(jdev, "heads");
		if (heads && json_is_array(heads)) {
			size_t head_idx;
			json_t *jhead;

			json_array_foreach(heads, head_idx, jhead) {
				if (head_idx >= FMD_MAX_HEADS)
					break;

				struct fmd_head *head = parse_head(ctx, jhead, id, head_idx);
				if (head) {
					fmd_topology_add(&ctx->topology, &head->base, &mhd->base);

					/* Update LD bindings based on head's lds array */
					json_t *jlds = json_object_get(jhead, "lds");
					update_ld_bindings_for_head(mhd, head, jlds);
				}
			}

			mhd->num_heads = (uint8_t)json_array_size(heads);
		}

		fmd_create_mhd_memory_resources(ctx, mhd);
		parse_dcd(ctx, jdev, &mhd->base);

		fmd_info(ctx, "Mock MHD %s: %d heads, %d LDs", id, mhd->num_heads, mhd->total_lds);
		return 0;
	}

	ld_count = (int)json_int_or(jdev, "ld_count", 1);

	if (ld_count <= 1) {
		/* SLD */
		struct fmd_sld *sld = parse_sld(ctx, jdev, id);
		if (!sld)
			return -1;

		fmd_topology_add(&ctx->topology, &sld->base, parent);
		fmd_create_sld_memory_resources(ctx, sld);
		parse_dcd(ctx, jdev, &sld->base);

		fmd_info(ctx, "Mock SLD %s: %" PRIu64 " MB",
			 id, (uint64_t)(sld->memory_size / (1024ULL * 1024)));
	} else {
		/* MLD */
		struct fmd_mld *mld = parse_mld(ctx, jdev, id);
		if (!mld)
			return -1;

		/* Set switch_port if MLD is behind a switch port */
		if (parent && parent->type == FMD_COMPONENT_PORT) {
			struct fmd_port *port = fmd_to_port(parent);
			mld->switch_port = port->port_number;
		}

		fmd_topology_add(&ctx->topology, &mld->base, parent);

		/* Get default health for LDs */
		json_t *health_obj = json_object_get(jdev, "health_info");
		uint8_t def_health = health_obj ?
			(uint8_t)json_int_or(health_obj, "health_status", 0) : 0;
		uint8_t def_media = health_obj ?
			(uint8_t)json_int_or(health_obj, "media_status", 0) : 0;

		/* Parse LDs */
		json_t *lds = json_object_get(jdev, "logical_devices");
		if (lds && json_is_array(lds)) {
			size_t ld_idx;
			json_t *jld;

			json_array_foreach(lds, ld_idx, jld) {
				if (ld_idx >= FMD_MAX_LD_COUNT)
					break;

				struct fmd_ld *ld = parse_ld(ctx, jld, id, ld_idx,
							    def_health, def_media);
				if (ld)
					fmd_topology_add(&ctx->topology, &ld->base, &mld->base);
			}

			/* Update ld_count based on array size */
			size_t array_size = json_array_size(lds);
			if (array_size > 0 && array_size <= FMD_MAX_LD_COUNT)
				mld->ld_count = (uint8_t)array_size;
		} else {
			/* Create default LDs with equal memory distribution */
			json_t *memdev = json_object_get(jdev, "identify_memory_device");
			uint64_t total_mb = memdev ?
				json_uint_or(memdev, "total_capacity_mb", 0) : 0;
			uint64_t per_ld_mb = total_mb / mld->ld_count;

			for (int i = 0; i < mld->ld_count; i++) {
				char ld_id_str[FMD_MAX_ID_LEN];
				(void)snprintf(ld_id_str, sizeof(ld_id_str), "%s_ld%d", id, i);

				struct fmd_ld *ld = fmd_ld_create(ld_id_str, i);
				if (ld) {
					ld->memory_size = per_ld_mb * 1024ULL * 1024;
					/* 100% ≈ 255 in CXL format (8-bit fraction) */
					ld->qos_allocated_bw = 255;
					ld->qos_limit_bw = 255;
					ld->health.health_status = def_health;
					ld->health.media_status = def_media;
					fmd_topology_add(&ctx->topology, &ld->base, &mld->base);
				}
			}
		}

		fmd_create_mld_memory_resources(ctx, mld);
		parse_dcd(ctx, jdev, &mld->base);

		fmd_info(ctx, "Mock MLD %s: %d LDs", id, mld->ld_count);
	}

	return 0;
}


/*
 * Parse vPPB binding status from string
 */
static enum fmd_vppb_binding_status parse_binding_status(const char *str)
{
	if (!str || strcmp(str, "unbound") == 0)
		return FMD_VPPB_UNBOUND;
	if (strcmp(str, "bound_physical_port") == 0)
		return FMD_VPPB_BOUND_PHYSICAL_PORT;
	if (strcmp(str, "bound_ld") == 0)
		return FMD_VPPB_BOUND_LD;
	if (strcmp(str, "bound_pbr") == 0)
		return FMD_VPPB_BOUND_PBR;
	if (strcmp(str, "busy") == 0)
		return FMD_VPPB_BUSY;
	return FMD_VPPB_UNBOUND;
}

/*
 * Parse VCS configuration for a switch.
 * Resolves vPPB bindings to physical port and LD pointers.
 */
static int parse_vcs_config(struct fmd_ctx *ctx, struct fmd_switch *sw,
			    json_t *jvcs_array)
{
	size_t vcs_idx;
	json_t *jvcs;

	if (!jvcs_array || !json_is_array(jvcs_array))
		return 0;

	json_array_foreach(jvcs_array, vcs_idx, jvcs) {
		if (vcs_idx >= FMD_MAX_VCS)
			break;

		struct fmd_vcs *vcs = &sw->vcs[vcs_idx];

		vcs->vcs_id = (uint8_t)json_int_or(jvcs, "vcs_id", vcs_idx);
		vcs->state = (uint8_t)json_int_or(jvcs, "state", 1); /* 1 = enabled */
		vcs->hdm_decoders = (uint8_t)json_int_or(jvcs, "hdm_decoders", 8);
		vcs->upstream_port_id = (uint8_t)json_int_or(jvcs, "upstream_port_id", 0);

		/* Parse vPPB bindings */
		json_t *jvppbs = json_object_get(jvcs, "vppbs");
		if (jvppbs && json_is_array(jvppbs)) {
			size_t vppb_idx;
			json_t *jvppb;

			json_array_foreach(jvppbs, vppb_idx, jvppb) {
				if (vppb_idx >= FMD_MAX_VPPB)
					break;

				struct fmd_vppb *vppb = &vcs->vppbs[vppb_idx];

				vppb->vppb_id = (uint8_t)json_int_or(jvppb, "vppb_id", vppb_idx);

				const char *status = json_str_or(jvppb, "binding_status", "unbound");
				vppb->binding_status = parse_binding_status(status);

				vppb->bound_port_id = (uint8_t)json_int_or(jvppb, "bound_port_id", 0);
				vppb->bound_ld_id = (uint8_t)json_int_or(jvppb, "bound_ld_id", 0);
				vppb->bound_pbr_id = (uint16_t)json_int_or(jvppb, "bound_pbr_id", 0);

				vcs->num_vppbs++;
			}
		}

		sw->num_vcs++;
		fmd_dbg(ctx, "Mock VCS %d: %d vPPBs, USP=%d",
			vcs->vcs_id, vcs->num_vppbs, vcs->upstream_port_id);
	}

	return 0;
}


static int parse_switch(struct fmd_ctx *ctx, json_t *jsw, json_t *devices)
{
	const char *id;
	struct fmd_switch *sw;

	id = json_str_or(jsw, "id", NULL);
	if (!id || id[0] == '\0') {
		fmd_err(ctx, "Mock switch missing or empty 'id' field");
		return -1;
	}

	sw = fmd_switch_create(id);
	if (!sw)
		return -1;

	/* Identify info */
	sw->identify.vendor_id = parse_hex16(json_object_get(jsw, "vendor_id"));
	sw->identify.device_id = parse_hex16(json_object_get(jsw, "device_id"));
	sw->identify.serial_num = parse_hex64(json_object_get(jsw, "serial_number"));
	sw->identify.component_type = CXL_COMPONENT_SWITCH;

	/* VCS configuration - default to 0 if no vcs array provided */
	sw->num_vcs = 0;
	sw->max_vcs = (uint8_t)json_int_or(jsw, "max_vcs", 8);

	fmd_topology_add(&ctx->topology, &sw->base, NULL);

	/* Parse ports array if present */
	json_t *ports = json_object_get(jsw, "ports");
	if (ports && json_is_array(ports)) {
		size_t port_idx;
		json_t *jport;

		json_array_foreach(ports, port_idx, jport) {
			uint8_t port_num = (uint8_t)json_int_or(jport, "port_number", port_idx);
			const char *type_str = json_str_or(jport, "type", "downstream");
			bool is_upstream = (strcmp(type_str, "upstream") == 0);

			char port_id[FMD_MAX_ID_LEN];
			if (is_upstream)
				(void)snprintf(port_id, sizeof(port_id), "%s_usp%d", id, port_num);
			else
				(void)snprintf(port_id, sizeof(port_id), "%s_dsp%d", id, port_num);

			struct fmd_port *port = fmd_port_create(port_id, port_num);
			if (!port)
				continue;

			const char *link = json_str_or(jport, "link_status", is_upstream ? "up" : "down");
			port->link_status = (strcmp(link, "up") == 0) ? FMD_LINK_UP : FMD_LINK_DOWN;

			if (is_upstream) {
				/* USP - mark as upstream and add to tree */
				port->state = FMD_PORT_USP;
				port->conn_dev_type = FMD_CONN_NONE;
				port->max_link_width = (uint8_t)json_int_or(jport, "max_link_width", 16);
				port->negotiated_link_width = (uint8_t)json_int_or(jport, "negotiated_link_width", 16);
				sw->num_usps++;
				fmd_topology_add(&ctx->topology, &port->base, &sw->base);
				fmd_dbg(ctx, "Mock USP %s: link=%s",
					port_id, port->link_status == FMD_LINK_UP ? "up" : "down");
				continue;
			}

			/* DSP configuration */
			const char *conn = json_str_or(jport, "conn_dev_type", "none");
			if (strcmp(conn, "sld") == 0)
				port->conn_dev_type = FMD_CONN_SLD;
			else if (strcmp(conn, "mld") == 0)
				port->conn_dev_type = FMD_CONN_MLD;
			else if (strcmp(conn, "switch") == 0)
				port->conn_dev_type = FMD_CONN_PCIE; /* Use for cascaded */
			else
				port->conn_dev_type = FMD_CONN_NONE;

			port->max_link_width = (uint8_t)json_int_or(jport, "max_link_width", 16);
			port->negotiated_link_width = (uint8_t)json_int_or(jport, "negotiated_link_width", 16);
			port->supported_ld_count = (uint8_t)json_int_or(jport, "supported_ld_count", 16);

			/* CXL mode information (Get Physical Port State) */
			port->conn_dev_cxl_ver = (uint8_t)json_int_or(jport, "conn_dev_cxl_ver", 0);
			port->port_cxl_ver_bitmask = (uint8_t)json_int_or(jport, "port_cxl_ver_bitmask", 0x06);
			port->supported_link_speeds = (uint8_t)json_int_or(jport, "supported_link_speeds", 0x1F);

			sw->num_dsps++;
			fmd_topology_add(&ctx->topology, &port->base, &sw->base);

			fmd_dbg(ctx, "Mock port %s: conn=%d link=%s",
				port_id, port->conn_dev_type,
				port->link_status == FMD_LINK_UP ? "up" : "down");
		}
	} else {
		/* Create default ports based on physical_ports count */
		int num_ports = (int)json_int_or(jsw, "physical_ports", 8);
		for (int i = 1; i < num_ports; i++) { /* Skip port 0 (USP) */
			char port_id[FMD_MAX_ID_LEN];
			(void)snprintf(port_id, sizeof(port_id), "%s_dsp%d", id, i);

			struct fmd_port *port = fmd_port_create(port_id, i);
			if (port) {
				port->link_status = FMD_LINK_DOWN;
				port->conn_dev_type = FMD_CONN_NONE;
				sw->num_dsps++;
				fmd_topology_add(&ctx->topology, &port->base, &sw->base);
			}
		}
	}

	/* Find devices attached to this switch */
	if (devices && json_is_array(devices)) {
		size_t idx;
		json_t *jdev;

		json_array_foreach(devices, idx, jdev) {
			const char *parent_sw = json_str_or(jdev, "parent_switch", NULL);
			if (!parent_sw || strcmp(parent_sw, id) != 0)
				continue;

			int parent_port = (int)json_int_or(jdev, "parent_port", -1);
			if (parent_port < 0)
				continue;

			/* Find the port */
			struct fmd_port *port = fmd_topology_find_port(&ctx->topology, id, parent_port);
			if (!port) {
				/* Create the port if it doesn't exist */
				char port_id[FMD_MAX_ID_LEN];
				(void)snprintf(port_id, sizeof(port_id), "%s_dsp%d", id, parent_port);
				port = fmd_port_create(port_id, parent_port);
				if (port) {
					port->link_status = FMD_LINK_UP;
					int ld_count = (int)json_int_or(jdev, "ld_count", 1);
					port->conn_dev_type = (ld_count > 1) ? FMD_CONN_MLD : FMD_CONN_SLD;
					sw->num_dsps++;
					fmd_topology_add(&ctx->topology, &port->base, &sw->base);
				}
			}

			if (port) {
				/* Update port connection info */
				port->link_status = FMD_LINK_UP;

				/*
				 * Only update conn_dev_type if not already set from explicit
				 * port configuration. MHD devices use ld_pool, not ld_count.
				 */
				if (port->conn_dev_type == FMD_CONN_NONE) {
					int ld_count = (int)json_int_or(jdev, "ld_count", 1);
					port->conn_dev_type = (ld_count > 1) ? FMD_CONN_MLD : FMD_CONN_SLD;
				}

				/* Parse device under this port */
				parse_device(ctx, jdev, &port->base);
			}
		}
	}

	fmd_info(ctx, "Mock switch %s: %d USP(s), %d DSPs",
		 id, sw->num_usps, sw->num_dsps);

	/* Parse VCS configuration (after devices so references resolve) */
	json_t *jvcs = json_object_get(jsw, "vcs");
	if (jvcs) {
		sw->num_vcs = 0;  /* Reset - will be counted during parsing */
		parse_vcs_config(ctx, sw, jvcs);
		fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
		fmd_info(ctx, "Mock switch %s: %d VCS(s) configured", id, sw->num_vcs);
	}

	/*
	 * Populate sw_identify (Identify Switch Device response) per DSP0288.
	 * These values are used by Redfish handlers for Switch resource.
	 */
	sw->sw_identify.num_vcs = sw->num_vcs;
	sw->sw_identify.num_physical_ports = sw->num_dsps + 1;  /* DSPs + USP */
	sw->sw_identify.num_total_vppb = 0;
	sw->sw_identify.num_active_vppb = 0;
	for (int i = 0; i < sw->num_vcs; i++) {
		sw->sw_identify.num_total_vppb += sw->vcs[i].num_vppbs;
		for (int j = 0; j < sw->vcs[i].num_vppbs; j++) {
			if (sw->vcs[i].vppbs[j].binding_status != FMD_VPPB_UNBOUND)
				sw->sw_identify.num_active_vppb++;
		}
		/* Use first VCS's HDM decoder count */
		if (i == 0)
			sw->sw_identify.num_hdm_decoder_per_usp = sw->vcs[i].hdm_decoders;
	}

	/*
	 * Create hosts for each VCS (switch-attached hosts)
	 * Each VCS represents a host connection via its USP.
	 */
	for (int i = 0; i < sw->num_vcs; i++) {
		struct fmd_vcs *vcs = &sw->vcs[i];
		char host_id[FMD_MAX_ID_LEN];
		struct fmd_host *host;

		snprintf(host_id, sizeof(host_id), "%.50s_host%d", id, vcs->vcs_id);

		host = fmd_host_create(host_id);
		if (host) {
			snprintf(host->name, sizeof(host->name), "Host via %s VCS %d",
				 id, vcs->vcs_id);
			host->vcs_id = vcs->vcs_id;
			host->usp_port_id = vcs->upstream_port_id;
			host->direct_attached = false;

			fmd_topology_add(&ctx->topology, &host->base, NULL);

			fmd_info(ctx, "Mock host %s: VCS %d USP port %d",
				 host_id, vcs->vcs_id, vcs->upstream_port_id);
		}
	}

	return 0;
}


/*
 * Parse a host system from JSON
 */
static int parse_host(struct fmd_ctx *ctx, json_t *jhost)
{
	const char *id;
	struct fmd_host *host;
	const char *name;

	id = json_str_or(jhost, "id", NULL);
	if (!id || id[0] == '\0') {
		fmd_err(ctx, "Mock host missing or empty 'id' field");
		return -1;
	}

	host = fmd_host_create(id);
	if (!host)
		return -1;

	/* Parse host properties */
	name = json_str_or(jhost, "name", id);
	snprintf(host->name, sizeof(host->name), "%s", name);

	/* Switch attachment info */
	host->vcs_id = (uint8_t)json_int_or(jhost, "vcs_id", 0);
	host->usp_port_id = (uint8_t)json_int_or(jhost, "usp_port_id", 0);
	host->direct_attached = json_is_true(json_object_get(jhost, "direct_attached"));

	/* If vcs_id is specified and direct_attached is not, assume switch-attached */
	if (!host->direct_attached && json_object_get(jhost, "vcs_id"))
		host->direct_attached = false;
	else if (!json_object_get(jhost, "direct_attached"))
		host->direct_attached = true;  /* Default to direct-attached */

	fmd_topology_add(&ctx->topology, &host->base, NULL);

	fmd_info(ctx, "Mock host %s: %s%s",
		 id, host->name,
		 host->direct_attached ? " (direct-attached)" : "");

	return 0;
}


int fmd_mock_load(struct fmd_ctx *ctx, const char *path)
{
	json_t *root = NULL;
	json_error_t error;
	struct fmd_mock_fabric *fabric = NULL;
	int ret = -1;

	fmd_info(ctx, "Loading mock topology from %s", path);

	/* Parse JSON file */
	root = json_load_file(path, 0, &error);
	if (!root) {
		fmd_err(ctx, "Failed to parse %s: %s (line %d)",
			path, error.text, error.line);
		return -1;
	}

	/* Allocate fabric metadata */
	fabric = calloc(1, sizeof(*fabric));
	if (!fabric) {
		fmd_err(ctx, "Failed to allocate mock fabric");
		goto out;
	}

	/* Parse metadata */
	fabric->version = (int)json_int_or(root, "version", 1);
	const char *desc = json_str_or(root, "description", "Mock topology");
	(void)snprintf(fabric->description, sizeof(fabric->description), "%s", desc);
	const char *fab_id = json_str_or(root, "id", "mock_fabric");
	(void)snprintf(fabric->id, sizeof(fabric->id), "%s", fab_id);

	fmd_info(ctx, "Mock fabric: %s (v%d) - %s",
		 fabric->id, fabric->version, fabric->description);

	/* Get devices array for later use */
	json_t *devices = json_object_get(root, "devices");

	/* Parse switches (which will also parse their attached devices) */
	json_t *switches = json_object_get(root, "switches");
	if (switches && json_is_array(switches)) {
		size_t idx;
		json_t *jsw;

		json_array_foreach(switches, idx, jsw) {
			parse_switch(ctx, jsw, devices);
		}
	}

	/* Parse direct-attached devices (no parent_switch) */
	if (devices && json_is_array(devices)) {
		size_t idx;
		json_t *jdev;

		json_array_foreach(devices, idx, jdev) {
			const char *parent_sw = json_str_or(jdev, "parent_switch", NULL);
			if (parent_sw)
				continue; /* Already handled by switch parsing */

			parse_device(ctx, jdev, NULL);
		}
	}

	/* Parse hosts */
	json_t *hosts = json_object_get(root, "hosts");
	if (hosts && json_is_array(hosts)) {
		size_t idx;
		json_t *jhost;

		json_array_foreach(hosts, idx, jhost) {
			parse_host(ctx, jhost);
		}
	}

	/*
	 * If no hosts were defined, create a default local host.
	 * This represents the system running cxl-fmd for direct-attached devices.
	 */
	if (ctx->topology.root->host_count == 0) {
		struct fmd_host *local_host = fmd_host_create("local");
		if (local_host) {
			snprintf(local_host->name, sizeof(local_host->name),
				 "Local System");
			local_host->direct_attached = true;
			fmd_topology_add(&ctx->topology, &local_host->base, NULL);
			fmd_info(ctx, "Created default local host for direct-attached devices");
		}
	}

	fmd_info(ctx, "Loaded: %d switch(es), %d device(s), %d LD(s), %d host(s)",
		 ctx->topology.root->switch_count,
		 ctx->topology.root->device_count,
		 ctx->topology.root->ld_count,
		 ctx->topology.root->host_count);

	/* Store in context */
	ctx->mock_fabric = fabric;
	fabric = NULL;
	ret = 0;

out:
	if (fabric)
		fmd_mock_free(fabric);
	if (root)
		json_decref(root);
	return ret;
}

void fmd_mock_free(struct fmd_mock_fabric *fabric)
{
	if (!fabric)
		return;

	/* The topology tree owns all components now */
	free(fabric);
}

int fmd_mock_discover(struct fmd_ctx *ctx)
{
	/*
	 * For mock mode, fmd_mock_load already builds the topology tree.
	 * This function is here for API compatibility.
	 */
	if (!ctx || !ctx->mock_fabric)
		return -1;

	fmd_info(ctx, "Mock discovery: %d device(s) already in topology",
		 ctx->topology.root->device_count);

	return ctx->topology.root->device_count;
}

bool fmd_is_mock_mode(struct fmd_ctx *ctx)
{
	return ctx && ctx->mock_fabric != NULL;
}
