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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "cxl-fmd.h"
#include "mock.h"


/*
 * A topology file is read strictly. A value that is there and cannot be
 * used as what it is meant to be is an error, not a reason to fall back on
 * a default: a serial number one digit too long used to load as all ones.
 * Errors are counted while the file is read, and the load fails if there
 * were any. The file is read by one thread, at start-up.
 */
static struct fmd_ctx *load_ctx;
static int load_errors;

static void load_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void load_error(const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fmd_err(load_ctx, "mock topology: %s", msg);
	load_errors++;
}

/*
 * Add @comp under @parent (NULL: the root). A second component with its Id
 * is a load error, as validate.c would find it, and @comp is freed: the
 * caller is not to use it after false.
 */
static bool mock_add(struct fmd_ctx *ctx, struct fmd_component *comp,
		     struct fmd_component *parent)
{
	int rc = fmd_topology_add(&ctx->topology, comp, parent);

	if (!rc)
		return true;
	if (rc == -EEXIST)
		load_error("two components with ID %s", comp->id);
	else
		load_error("%s could not be added: %d", comp->id, rc);
	fmd_component_free(comp);
	return false;
}

/* a number, given as one or as a string in any base strtoull() takes */
static bool json_number(json_t *val, const char *what, bool is_signed,
			uint64_t max, uint64_t *out)
{
	*out = 0;

	if (json_is_integer(val)) {
		json_int_t n = json_integer_value(val);

		if (n < 0 && !is_signed) {
			load_error("%s: %lld is negative", what, (long long)n);
			return false;
		}
		*out = (uint64_t)n;
	} else if (json_is_string(val)) {
		const char *str = json_string_value(val);
		char *end;

		errno = 0;
		if (is_signed)
			*out = (uint64_t)strtoll(str, &end, 0);
		else
			*out = strtoull(str, &end, 0);
		if (errno || end == str || *end != '\0' ||
		    (!is_signed && strchr(str, '-'))) {
			load_error("%s: \"%s\" is not a number that fits", what, str);
			*out = 0;
			return false;
		}
	} else {
		load_error("%s: not a number", what);
		return false;
	}

	if (!is_signed && *out > max) {
		load_error("%s: %#llx is more than %#llx", what,
			   (unsigned long long)*out, (unsigned long long)max);
		*out = 0;
		return false;
	}
	return true;
}

/* Helper: parse hex string to uint16_t */
static uint16_t parse_hex16(json_t *val)
{
	uint64_t n;

	if (!val)
		return 0;	/* not given */
	json_number(val, "16-bit value", false, UINT16_MAX, &n);
	return (uint16_t)n;
}

/* Helper: parse hex string to uint64_t */
static uint64_t parse_hex64(json_t *val)
{
	uint64_t n;

	if (!val)
		return 0;	/* not given */
	json_number(val, "64-bit value", false, UINT64_MAX, &n);
	return n;
}

/* Helper: get integer with default */
static int64_t json_int_or(json_t *obj, const char *key, int64_t def)
{
	json_t *val = json_object_get(obj, key);
	uint64_t n;

	if (!val)
		return def;
	if (!json_number(val, key, true, 0, &n))
		return def;
	return (int64_t)n;
}

/* Helper: get unsigned integer with default */
static uint64_t json_uint_or(json_t *obj, const char *key, uint64_t def)
{
	json_t *val = json_object_get(obj, key);
	uint64_t n;

	if (!val)
		return def;
	if (!json_number(val, key, false, UINT64_MAX, &n))
		return def;
	return n;
}

/*
 * A capacity given in MB, in the 256 MB units Identify Memory Device reports
 * capacities in (Table 8-309). One that is not a whole number of them cannot
 * be reported, and is not rounded.
 */
static uint64_t json_capacity_units(json_t *obj, const char *key, uint64_t def_mb)
{
	uint64_t mb = json_uint_or(obj, key, def_mb);

	if (mb % 256)
		load_error("%s: %llu MB is not a multiple of 256 MB", key,
			   (unsigned long long)mb);
	return mb / 256;
}

/* Helper: get string with default */
static const char *json_str_or(json_t *obj, const char *key, const char *def)
{
	json_t *val = json_object_get(obj, key);

	if (!val)
		return def;
	if (!json_is_string(val)) {
		load_error("%s: not a string", key);
		return def;
	}
	return json_string_value(val);
}

/*
 * The event records of a component, as Get Event Records would have
 * returned them (Common Event Record Format, Table 8-221):
 *
 *   "event_records": [ { "log": "warning", "kind": "physical_switch",
 *                        "timestamp_ns": 1700000000000000000,
 *                        "ld_id": 1, "data": "0400" } ],
 *   "event_log_overflow": { "warning": { "count": 3, "first_ns": ...,
 *                                        "last_ns": ... } }
 *
 * "kind" names a record format, or "uuid" gives its Event Record
 * Identifier. "data" is the bytes from 30h on, in hex. "ld_id" sets the
 * LD-ID Valid Flag and the LD-ID, for an MLD. They are kept as records read
 * from the component are.
 */
static const char *const log_names[] = {
	"informational", "warning", "failure", "fatal", "dynamic_capacity",
};

static int log_from_name(const char *name)
{
	for (int i = 0; i < 5; i++) {
		if (!strcmp(name, log_names[i]))
			return i;
	}
	return -1;
}

static int parse_uuid(const char *str, uint8_t uuid[16])
{
	int n = 0;

	for (const char *p = str; *p && n < 32; p++) {
		if (*p == '-')
			continue;
		if (!isxdigit((unsigned char)p[0]))
			return -1;
		uuid[n / 2] = (uint8_t)((uuid[n / 2] << 4) |
			(isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)));
		n++;
	}
	return n == 32 ? 0 : -1;
}

/*
 * A record of the Dynamic Capacity Event Log (Table 8-229), as one kept:
 * an Add Capacity Response (04h) or Capacity Released (05h) whose Host ID
 * (32h) is the LD-ID of one of the device's LDs. Returns 0 if it is, and
 * loads it with the severity every one has.
 *
 * A record of a Multi-Headed DCD is refused. That is a limit of cxl-fmd's
 * mocks, not of the specification, which defines the record "for an LD-FAM
 * DCD" (Section 8.2.10.2.1.6): which LD-ID its Host ID carries on such a
 * device is unsettled (see docs/DCD-FM-API.md).
 */
static int parse_dc_record(struct fmd_ctx *ctx, struct fmd_component *comp,
			   size_t idx, json_t *item, struct cxlmi_event_record *rec)
{
	static const uint8_t dc_uuid[16] = {
		0xca, 0x95, 0xaf, 0xa7, 0xf1, 0x83, 0x40, 0x18,
		0x8c, 0x2f, 0x95, 0x26, 0x8e, 0x10, 0x1a, 0x2a,
	};
	int host = rec->data[2] | (rec->data[3] << 8);

	(void)ctx;
	if (!fmd_memdev_dcd(comp) || comp->type == FMD_COMPONENT_MHD) {
		load_error("%s: event record %zu: a dynamic_capacity record of a "
			   "device that is not a DCD, or is multi-headed", comp->id, idx);
		return -1;
	}
	if (memcmp(rec->uuid, dc_uuid, sizeof(dc_uuid))) {
		load_error("%s: event record %zu: a dynamic_capacity record is a "
			   "dynamic_capacity kind", comp->id, idx);
		return -1;
	}
	if (rec->data[0] != 0x04 && rec->data[0] != 0x05) {
		load_error("%s: event record %zu: type %02xh is the host's: only "
			   "04h and 05h are reported to the FM", comp->id, idx,
			   rec->data[0]);
		return -1;
	}
	if (json_object_get(item, "ld_id")) {
		load_error("%s: event record %zu: a dynamic_capacity record names "
			   "its host in its data, Host ID at 32h", comp->id, idx);
		return -1;
	}
	if (!fmd_dc_host_component(comp, host)) {
		load_error("%s: event record %zu: host %d is none of its LDs",
			   comp->id, idx, host);
		return -1;
	}
	/* "shall set the Event Record Severity field ... to Informational" */
	rec->flags[0] = 0;
	return 0;
}

static void parse_event_records(struct fmd_ctx *ctx, json_t *jobj,
				struct fmd_component *comp)
{
	json_t *arr = json_object_get(jobj, "event_records");
	json_t *over = json_object_get(jobj, "event_log_overflow");
	const char *key;
	json_t *item;
	size_t idx;

	if (arr && !json_is_array(arr)) {
		load_error("%s: event_records is not a list", comp->id);
		return;
	}
	json_array_foreach(arr, idx, item) {
		struct cxlmi_event_record rec = { 0 };
		const char *log = json_str_or(item, "log", NULL);
		const char *kind = json_str_or(item, "kind", NULL);
		const char *uuid = json_str_or(item, "uuid", NULL);
		const char *data = json_str_or(item, "data", "");
		int which = log ? log_from_name(log) : -1;
		size_t len = strlen(data);

		if (which < 0) {
			load_error("%s: event record %zu: log is not informational, "
				   "warning, failure, fatal or dynamic_capacity",
				   comp->id, idx);
			continue;
		}
		if (kind ? fmd_event_record_kind_from_name(kind, rec.uuid) < 0 :
		    !uuid || parse_uuid(uuid, rec.uuid) < 0) {
			load_error("%s: event record %zu: no known kind, or uuid",
				   comp->id, idx);
			continue;
		}
		if (len % 2 || len / 2 > sizeof(rec.data)) {
			load_error("%s: event record %zu: data is not up to %zu bytes "
				   "of hex", comp->id, idx, sizeof(rec.data));
			continue;
		}
		for (size_t i = 0; i < len / 2; i++) {
			unsigned int byte;

			if (sscanf(data + 2 * i, "%2x", &byte) != 1 ||
			    !isxdigit((unsigned char)data[2 * i + 1])) {
				load_error("%s: event record %zu: data is not hex",
					   comp->id, idx);
				break;
			}
			rec.data[i] = (uint8_t)byte;
		}

		rec.length = 0x80;
		rec.flags[0] = (uint8_t)which;	/* Event Record Severity */
		rec.handle = (uint16_t)(idx + 1);
		rec.timestamp = json_uint_or(item, "timestamp_ns", 0);
		if (json_object_get(item, "ld_id")) {
			if (comp->type != FMD_COMPONENT_MLD) {
				load_error("%s: event record %zu: ld_id is for an MLD",
					   comp->id, idx);
				continue;
			}
			rec.flags[0] |= 1 << 7;	/* LD-ID Valid Flag */
			rec.ld_id = (uint16_t)json_uint_or(item, "ld_id", 0);
			if (!fmd_topology_find_ld(&ctx->topology, comp->id, rec.ld_id)) {
				load_error("%s: event record %zu: no LD %d", comp->id,
					   idx, rec.ld_id);
				continue;
			}
		}

		if (which == FMD_LOG_DYNAMIC_CAPACITY) {
			if (!parse_dc_record(ctx, comp, idx, item, &rec))
				fmd_dc_record_keep(comp, &rec);
			continue;
		}
		if (comp->type == FMD_COMPONENT_SWITCH)
			fmd_log_add(comp, which, &rec);
		else
			fmd_device_record_keep(ctx, comp, which, &rec, NULL);
	}

	if (over && !json_is_object(over)) {
		load_error("%s: event_log_overflow is not an object", comp->id);
		return;
	}
	json_object_foreach(over, key, item) {
		int which = log_from_name(key);
		struct fmd_component *child;

		if (which < 0) {
			load_error("%s: event_log_overflow: %s is not a log", comp->id, key);
			continue;
		}
		uint16_t count = (uint16_t)json_uint_or(item, "count", 0);
		uint64_t first = json_uint_or(item, "first_ns", 0);
		uint64_t last = json_uint_or(item, "last_ns", 0);

		if (which == FMD_LOG_DYNAMIC_CAPACITY) {
			if (!fmd_memdev_dcd(comp))
				load_error("%s: event_log_overflow: dynamic_capacity "
					   "of a device that is not a DCD", comp->id);
			else
				fmd_dc_log_overflowed(comp, count, first, last);
			continue;
		}
		if (comp->type != FMD_COMPONENT_MLD) {
			fmd_log_overflowed(comp, which, count, first, last, NULL);
			continue;
		}
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_LD)
			fmd_log_overflowed(child, which, count, first, last, NULL);
	}
}

/*
 * "unplug": the devices to pull out of their switch ports, in order, one
 * each time the daemon is sent SIGUSR1. Each is an SLD or an MLD on a
 * switch port, named once.
 */
static void parse_unplug(struct fmd_ctx *ctx, struct fmd_mock_fabric *fabric,
			 json_t *list)
{
	json_t *item;
	size_t idx;

	if (!list)
		return;
	if (!json_is_array(list)) {
		load_error("unplug is not a list of device IDs");
		return;
	}
	fabric->unplug = calloc(json_array_size(list) ? json_array_size(list) : 1,
				sizeof(*fabric->unplug));
	if (!fabric->unplug) {
		load_error("unplug: out of memory");
		return;
	}

	json_array_foreach(list, idx, item) {
		const char *id = json_string_value(item);
		struct fmd_component *dev = id ? fmd_topology_find(&ctx->topology, id) : NULL;

		if (!id) {
			load_error("unplug %zu: not a device ID", idx);
			continue;
		}
		if (!dev || (dev->type != FMD_COMPONENT_SLD &&
			     dev->type != FMD_COMPONENT_MLD)) {
			load_error("unplug %zu: %s is not an SLD or an MLD", idx, id);
			continue;
		}
		if (!dev->parent || dev->parent->type != FMD_COMPONENT_PORT) {
			load_error("unplug %zu: %s is not on a switch port: only a "
				   "switch reports a device that is pulled out", idx, id);
			continue;
		}
		for (int i = 0; i < fabric->unplug_count; i++) {
			if (!strcmp(fabric->unplug[i], id)) {
				load_error("unplug %zu: %s is pulled out twice", idx, id);
				id = NULL;
				break;
			}
		}
		if (id)
			snprintf(fabric->unplug[fabric->unplug_count++],
				 FMD_MAX_ID_LEN, "%s", id);
	}
}

/*
 * "external": binds and unbinds made by someone other than cxl-fmd, one
 * each time the daemon is sent SIGUSR2, in order:
 *
 *   "external": [ { "switch": "switch_0", "vcs": 0, "vppb": 3,
 *                   "action": "bind", "port": 4, "ld": 1,
 *                   "emit_event": true } ]
 *
 * "action" is "bind" or "unbind"; a bind names "port", and "ld" for an MLD
 * port only, as Bind vPPB does (Table 7-35). "emit_event" (default true)
 * keeps a Binding Change in the switch's Informational log and acts on it
 * as on one read from a switch; false changes the table and nothing else,
 * the case where no record reaches this FM.
 */
static void parse_external(struct fmd_ctx *ctx, struct fmd_mock_fabric *fabric,
			   json_t *list)
{
	json_t *item;
	size_t idx;

	if (!list)
		return;
	if (!json_is_array(list)) {
		load_error("external is not a list");
		return;
	}
	fabric->external = calloc(json_array_size(list) ? json_array_size(list) : 1,
				  sizeof(*fabric->external));
	if (!fabric->external) {
		load_error("external: out of memory");
		return;
	}

	json_array_foreach(list, idx, item) {
		struct fmd_mock_external *e = &fabric->external[fabric->external_count];
		const char *sw_id = json_str_or(item, "switch", NULL);
		const char *action = json_str_or(item, "action", NULL);
		struct fmd_switch *sw = sw_id ?
			fmd_topology_find_switch(&ctx->topology, sw_id) : NULL;
		struct fmd_vcs *vcs;
		struct fmd_port *port;
		json_t *jemit;

		if (!json_is_object(item) || !sw) {
			load_error("external %zu: no switch %s", idx,
				   sw_id ? sw_id : "named");
			continue;
		}
		e->vcs_id = (uint8_t)json_int_or(item, "vcs", 0);
		e->vppb_id = (uint8_t)json_int_or(item, "vppb", 0);
		vcs = fmd_switch_find_vcs(sw, e->vcs_id);
		if (!vcs || !fmd_vcs_find_vppb(vcs, e->vppb_id)) {
			load_error("external %zu: %s has no vPPB %d in VCS %d", idx,
				   sw_id, e->vppb_id, e->vcs_id);
			continue;
		}
		/* vPPB 0 is the USP's */
		if (e->vppb_id == 0) {
			load_error("external %zu: vPPB 0 is the upstream port's", idx);
			continue;
		}
		if (!action || (strcmp(action, "bind") && strcmp(action, "unbind"))) {
			load_error("external %zu: action is not bind or unbind", idx);
			continue;
		}
		e->bind = !strcmp(action, "bind");
		e->port_id = 0;
		e->ld_id = 0xFFFF;
		if (e->bind) {
			if (!json_object_get(item, "port")) {
				load_error("external %zu: a bind names a port", idx);
				continue;
			}
			e->port_id = (uint8_t)json_int_or(item, "port", 0);
			port = fmd_topology_find_port(&ctx->topology, sw_id, e->port_id);
			if (!port) {
				load_error("external %zu: %s has no port %d", idx,
					   sw_id, e->port_id);
				continue;
			}
			/* LD ID: the LD-ID of an MLD port, FFFFh otherwise */
			if (json_object_get(item, "ld"))
				e->ld_id = (uint16_t)json_int_or(item, "ld", 0);
			if ((port->conn_dev_type == FMD_CONN_MLD) != (e->ld_id != 0xFFFF)) {
				load_error("external %zu: port %d %s, and ld is the "
					   "LD-ID of an MLD port only (Table 7-35)",
					   idx, e->port_id,
					   port->conn_dev_type == FMD_CONN_MLD ?
					   "has an MLD" : "has no MLD");
				continue;
			}
		}
		jemit = json_object_get(item, "emit_event");
		if (jemit && !json_is_boolean(jemit)) {
			load_error("external %zu: emit_event is not true or false", idx);
			continue;
		}
		e->emit_event = !jemit || json_is_true(jemit);
		snprintf(e->switch_id, sizeof(e->switch_id), "%s", sw_id);
		fabric->external_count++;
	}
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
	sld->health_valid = true;	/* a mock answers Get Health Info */

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
			json_capacity_units(obj, "total_capacity_mb", 0);
		sld->memdev.volatile_capacity =
			json_capacity_units(obj, "volatile_capacity_mb", 0);
		sld->memdev.persistent_capacity =
			json_capacity_units(obj, "persistent_capacity_mb", 0);
		sld->memdev.partition_align =
			json_capacity_units(obj, "partition_align_mb", 256);
		sld->memdev.lsa_size =
			(uint32_t)json_int_or(obj, "lsa_size", 0);
		/* the sizes of its four event logs (Table 8-309) */
		sld->memdev.info_event_log_size =
			(uint16_t)json_uint_or(obj, "info_event_log_size", 0);
		sld->memdev.warning_event_log_size =
			(uint16_t)json_uint_or(obj, "warning_event_log_size", 0);
		sld->memdev.failure_event_log_size =
			(uint16_t)json_uint_or(obj, "failure_event_log_size", 0);
		sld->memdev.fatal_event_log_size =
			(uint16_t)json_uint_or(obj, "fatal_event_log_size", 0);
	}

	/* Compute memory sizes */
	sld->memory_size = CXL_TO_BYTES(sld->memdev.total_capacity);
	sld->volatile_size = CXL_TO_BYTES(sld->memdev.volatile_capacity);
	sld->persistent_size = CXL_TO_BYTES(sld->memdev.persistent_capacity);

	/* Partition Info section (CCI 0x4100) - values in 256MB units */
	obj = json_object_get(jdev, "partition_info");
	if (obj) {
		sld->partition.active_vmem =
			json_capacity_units(obj, "active_volatile_mb", 0);
		sld->partition.active_pmem =
			json_capacity_units(obj, "active_persistent_mb", 0);
		sld->partition.next_vmem =
			json_capacity_units(obj, "next_volatile_mb", 0);
		sld->partition.next_pmem =
			json_capacity_units(obj, "next_persistent_mb", 0);
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

	/*
	 * The device's Memory Size (Get LD Info, 5400h), which its domain
	 * is: identify_memory_device.total_capacity_mb. Without it the
	 * domain is as large as the LDs' allocations.
	 */
	obj = json_object_get(jdev, "identify_memory_device");
	mld->pool_capacity = obj ? json_uint_or(obj, "total_capacity_mb", 0) * FMD_MIB : 0;
	mld->pool_capacity_valid = mld->pool_capacity != 0;

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
			(uint16_t)json_int_or(obj, "reqcmp_basis", 0);
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
	ld->health_valid = true;	/* a mock answers Get Health Info */

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
	else if (strcmp(pres, "SLD") == 0)
		head->presentation = FMD_HEAD_SLD;
	else
		load_error("%s: presentation \"%s\" is not SLD or MLD",
			   head_id_str, pres);

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

	/*
	 * pool_capacity_mb: the Memory Size the LD Pool CCI answers Get LD
	 * Info (5400h) with, which the domain is. Without it the domain is
	 * as large as the pool LDs' allocations, as for a device that does
	 * not answer.
	 */
	mhd->pool_capacity = json_uint_or(jdev, "pool_capacity_mb", 0) * FMD_MIB;
	mhd->pool_capacity_valid = mhd->pool_capacity != 0;

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
	ld->health_valid = true;	/* a mock answers Get Health Info */

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

/*
 * "hosts": [..] or "host_id": N into @set, one byte per Host ID below
 * @num_hosts; with neither, the hosts below @def_count. A Host ID past
 * @num_hosts is a load error.
 */
static void parse_host_set(const char *what, json_t *obj, int num_hosts,
			   int def_count, bool set[FMD_DC_MAX_HOST_ID + 1])
{
	json_t *jhosts = json_object_get(obj, "hosts");
	json_t *jhost = json_object_get(obj, "host_id");
	size_t idx;
	json_t *v;

	memset(set, 0, (FMD_DC_MAX_HOST_ID + 1) * sizeof(set[0]));
	if (jhosts && json_is_array(jhosts)) {
		json_array_foreach(jhosts, idx, v) {
			json_int_t h = json_is_integer(v) ? json_integer_value(v) : -1;

			if (h < 0 || h >= num_hosts)
				load_error("%s: host %" JSON_INTEGER_FORMAT " is not one "
					   "of the %d hosts", what, h, num_hosts);
			else
				set[h] = true;
		}
		return;
	}
	if (jhost) {
		json_int_t h = json_is_integer(jhost) ? json_integer_value(jhost) : -1;

		if (h < 0 || h >= num_hosts)
			load_error("%s: host %" JSON_INTEGER_FORMAT " is not one of "
				   "the %d hosts", what, h, num_hosts);
		else
			set[h] = true;
		return;
	}
	for (int h = 0; h < def_count && h < num_hosts; h++)
		set[h] = true;
}

/*
 * A region's Region Base for each host in @hosts: "base_mb", or the host's
 * entry in "host_base_mb": { "1": 40960 }. "The DCD controls which DPA
 * range it assigns to each region for each host" (Section 9.13.3); that a
 * base may differ from host to host while the Region Length, Decode Length
 * and Block Size do not is the mock's choice.
 */
static void parse_host_bases(const char *what, json_t *obj,
			     const bool hosts[FMD_DC_MAX_HOST_ID + 1], uint64_t def,
			     uint64_t base[FMD_DC_MAX_HOST_ID + 1])
{
	const char *key;
	json_t *v;

	for (int h = 0; h <= FMD_DC_MAX_HOST_ID; h++)
		base[h] = def;
	if (!obj)
		return;
	if (!json_is_object(obj)) {
		load_error("%s: host_base_mb is not an object", what);
		return;
	}
	json_object_foreach(obj, key, v) {
		char *end;
		long h = strtol(key, &end, 10);
		uint64_t mb;

		if (end == key || *end || h < 0 || h > FMD_DC_MAX_HOST_ID || !hosts[h]) {
			load_error("%s: host_base_mb: \"%s\" is not a host of the region",
				   what, key);
			continue;
		}
		if (json_number(v, "host_base_mb", false, UINT64_MAX / FMD_MIB, &mb))
			base[h] = mb * FMD_MIB;
	}
}

/*
 * The region of host @h a DPA of that host's is in, bounded by its Region
 * Length as the mock allocates within it; -1 for none.
 */
static int host_region_of(struct fmd_component *dev, int h, uint64_t dpa)
{
	for (int r = 0; r < FMD_MAX_DC_REGIONS; r++) {
		struct fmd_dc_region *reg = fmd_dc_find_region(dev, h, (uint8_t)r);

		if (reg && reg->region_len && dpa >= reg->base_dpa &&
		    dpa - reg->base_dpa < reg->region_len)
			return r;
	}
	return -1;
}

static void mock_record_add(struct fmd_ctx *ctx, struct fmd_component *dev,
			    const char *what, int h, uint64_t dpa, uint64_t len,
			    const uint8_t tag[FMD_DC_TAG_LEN], uint16_t seq,
			    uint8_t rid, bool pending)
{
	struct fmd_dc_extent *ext;

	if (fmd_dc_find_extent(dev, h, dpa)) {
		load_error("%s: host %d already has an extent at %" PRIu64 " MB",
			   what, h, (uint64_t)(dpa / FMD_MIB));
		return;
	}
	ext = fmd_dc_extent_add(ctx, dev, h, dpa, len, tag, seq, rid);
	if (!ext) {
		load_error("%s: out of memory", what);
		return;
	}
	ext->pending = pending;
}

/*
 * "tags": [ { "tag": "<uuid>", "fm_referenced": true, "pending_hosts": [2] } ]
 * Entries of the device's 5608h list besides those its extents give.
 * "pending_hosts" sets bits of the Pending Reference Bitmap, "an LD for
 * which the tagged capacity has been added with no host response yet"
 * (Table 7-78), with no extent: a device whose 5603h does not list an add
 * the host has not answered (that it does or does not, the specification
 * does not say). That and "fm_referenced" with no extent are the mock's
 * means to show what a device could report.
 */
static void parse_dc_tags(struct fmd_component *dev, struct fmd_dcd_info *dcd,
			  json_t *arr)
{
	size_t idx;
	json_t *item;

	if (!arr)
		return;
	if (!json_is_array(arr)) {
		load_error("%s: dcd tags is not a list", dev->id);
		return;
	}
	free(dcd->mock_tags);
	dcd->mock_tags = calloc(json_array_size(arr) ? json_array_size(arr) : 1,
				sizeof(*dcd->mock_tags));
	dcd->n_mock_tags = 0;
	if (!dcd->mock_tags)
		return;

	json_array_foreach(arr, idx, item) {
		struct fmd_dc_tag *t = &dcd->mock_tags[dcd->n_mock_tags];
		const char *tag_str = json_str_or(item, "tag", "");
		json_t *jpend = json_object_get(item, "pending_hosts");
		size_t pidx;
		json_t *v;

		memset(t, 0, sizeof(*t));
		if (fmd_dc_tag_from_str(tag_str, t->tag) < 0 ||
		    !memcmp(t->tag, (uint8_t[FMD_DC_TAG_LEN]){0}, FMD_DC_TAG_LEN)) {
			load_error("%s: dcd tag %zu: '%s' is not a tag", dev->id, idx,
				   tag_str);
			continue;
		}
		if (jpend && !json_is_array(jpend)) {
			load_error("%s: dcd tag %zu: pending_hosts is not a list",
				   dev->id, idx);
			continue;
		}
		json_array_foreach(jpend, pidx, v) {
			json_int_t h = json_is_integer(v) ? json_integer_value(v) : -1;

			if (h < 0 || h >= dcd->num_hosts || h > FMD_DC_MAX_HOST_ID ||
			    !fmd_dc_host_component(dev, (int)h)) {
				load_error("%s: dcd tag %zu: pending host %" JSON_INTEGER_FORMAT
					   " is not one of the %d hosts", dev->id, idx, h,
					   dcd->num_hosts);
				continue;
			}
			t->pend[h / 8] |= (uint8_t)(1 << (h % 8));
		}
		if (json_is_true(json_object_get(item, "fm_referenced")))
			fmd_dc_mock_ref_set(dcd, t->tag, true);
		dcd->n_mock_tags++;
	}
}

/*
 * The mock's capacity is an offset into a region ("phys"), the same for
 * every host that has it. Two hosts, or a host and what an FM reference
 * keeps, have the same capacity only as shared capacity: with the same
 * Tag, "identical for all hosts sharing the same extent" (Table 8-230),
 * in a Sharable region. Anything else is two things in one place, which
 * the mock's device cannot have.
 */
struct mock_phys {
	int host;			/* -1: what an FM reference keeps */
	uint64_t dpa, phys, len;
	const uint8_t *tag;
	uint8_t rid, flags;
};

static void check_mock_phys(struct fmd_component *dev, struct fmd_dcd_info *dcd)
{
	static const uint8_t zero[FMD_DC_TAG_LEN];
	struct fmd_component *child;
	struct fmd_dc_mock_hold *hold;
	struct mock_phys *v;
	size_t n = 0;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT)
		n++;
	for (hold = dcd->mock_orphans; hold; hold = hold->next)
		n++;
	v = calloc(n ? n : 1, sizeof(*v));
	if (!v)
		return;

	n = 0;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT) {
		struct fmd_dc_extent *ext = fmd_to_dc_extent(child);
		struct fmd_dc_region *r = fmd_dc_find_region(dev, ext->host_id,
							      ext->region_id);

		if (!r)
			continue;
		v[n++] = (struct mock_phys){ ext->host_id, ext->start_dpa,
					     ext->start_dpa - r->base_dpa, ext->len,
					     ext->tag, ext->region_id, r->flags };
	}
	for (hold = dcd->mock_orphans; hold; hold = hold->next) {
		uint8_t flags = 0;

		for (int h = 0; h < dcd->num_hosts; h++) {
			struct fmd_dc_region *r = fmd_dc_find_region(dev, h, hold->region_id);

			if (r)
				flags = r->flags;
		}
		v[n++] = (struct mock_phys){ -1, 0, hold->phys, hold->len, hold->tag,
					     hold->region_id, flags };
	}

	for (size_t i = 0; i < n; i++) {
		for (size_t j = i + 1; j < n; j++) {
			struct mock_phys *a = &v[i], *b = &v[j];
			bool same_tag;

			/* one host's own overlaps are the validator's */
			if (a->rid != b->rid || (a->host == b->host && a->host >= 0))
				continue;
			if (a->phys >= b->phys + b->len || b->phys >= a->phys + a->len)
				continue;
			same_tag = !memcmp(a->tag, b->tag, FMD_DC_TAG_LEN) &&
				   memcmp(a->tag, zero, FMD_DC_TAG_LEN);
			if (same_tag && (a->flags & FMD_DC_REGION_SHAREABLE))
				continue;
			load_error("%s: dcd: region %u capacity at offset %" PRIu64 " MB of "
				   "host %d and at offset %" PRIu64 " MB of host %d (-1: "
				   "what an FM reference keeps) is the same: %s",
				   dev->id, a->rid,
				   (uint64_t)(a->phys / FMD_MIB), a->host,
				   (uint64_t)(b->phys / FMD_MIB), b->host,
				   !same_tag ? "they have different tags, or none" :
				   "the region is not Sharable");
		}
	}
	free(v);
}

static int parse_dcd(struct fmd_ctx *ctx, json_t *jdev, struct fmd_component *dev)
{
	json_t *jdcd = json_object_get(jdev, "dcd");
	struct fmd_dcd_info *dcd;
	struct fmd_component *child;
	uint64_t region_base[FMD_MAX_DC_REGIONS] = {0};
	uint64_t region_len[FMD_MAX_DC_REGIONS] = {0};
	uint64_t host_base[FMD_DC_MAX_HOST_ID + 1];
	bool hosts[FMD_DC_MAX_HOST_ID + 1];
	char what[FMD_MAX_ID_LEN + 64];
	uint32_t max_extents, max_tags;
	int64_t num_hosts;
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
	/* Number of Hosts is one byte (Table 7-65) */
	num_hosts = json_int_or(jdcd, "num_hosts", default_hosts);
	if (num_hosts < 0 || num_hosts > FMD_DC_MAX_HOST_ID) {
		load_error("%s: dcd: more than %d hosts", dev->id, FMD_DC_MAX_HOST_ID);
		num_hosts = FMD_DC_MAX_HOST_ID;
	}
	dcd->num_hosts = (uint8_t)num_hosts;
	dcd->add_policies = parse_policies(json_object_get(jdcd, "add_policies"), false,
					   FMD_DC_ADD_FREE | FMD_DC_ADD_CONTIGUOUS |
					   FMD_DC_ADD_PRESCRIPTIVE);
	dcd->release_policies = parse_policies(json_object_get(jdcd, "release_policies"), true,
					       FMD_DC_REL_TAG_BASED | FMD_DC_REL_PRESCRIPTIVE);
	max_extents = (uint32_t)json_int_or(jdcd, "max_extents", 256);
	max_tags = (uint32_t)json_int_or(jdcd, "max_tags", 64);
	dcd->extents_supported = dcd->extents_available = max_extents;
	dcd->tags_supported = dcd->tags_available = max_tags;

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
			uint8_t flags = 0;

			if (rid < 0 || rid >= FMD_MAX_DC_REGIONS ||
			    !json_object_get(item, "region_len_mb") ||
			    (len_mb != 0 && bs_mb == 0)) {
				load_error("%s: dcd region %d: no region_len_mb, no "
					   "block size, or not one of regions 0 to %d",
					   dev->id, rid, FMD_MAX_DC_REGIONS - 1);
				continue;
			}

			if (rid + 1 > dcd->num_regions)
				dcd->num_regions = (uint8_t)(rid + 1);

			/*
			 * Region Length: "Report 0 if the region is unavailable
			 * for Dynamic Capacity" (Table 8-347). The region is
			 * supported and has no capacity: it is skipped, as
			 * discovery skips it, and no extent can be in it.
			 */
			if (len_mb == 0)
				continue;
			region_base[rid] = base_mb * FMD_MIB;
			region_len[rid] = len_mb * FMD_MIB;

			if (json_is_true(json_object_get(item, "shareable")))
				flags |= FMD_DC_REGION_SHAREABLE;
			if (json_is_true(json_object_get(item, "nonvolatile")))
				flags |= FMD_DC_REGION_NONVOLATILE;
			if (json_is_true(json_object_get(item, "hw_coherency")))
				flags |= FMD_DC_REGION_HW_COHERENT;
			/*
			 * A DCD "shall set the Dynamic Capacity DSMAS Flag" of
			 * each region (Section 9.13.3). Bit[5] of Attributes1,
			 * "Interconnect specific Dynamic Capacity Management",
			 * is taken to be that flag, which cannot be confirmed
			 * without the CDAT specification: set unless the file
			 * says false.
			 */
			if (!json_is_false(json_object_get(item, "interconnect_dcm")))
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

			snprintf(what, sizeof(what), "%s: dcd region %d", dev->id, rid);
			parse_host_set(what, item, dcd->num_hosts, dcd->num_hosts, hosts);
			parse_host_bases(what, json_object_get(item, "host_base_mb"), hosts,
					 region_base[rid], host_base);
			for (int h = 0; h < dcd->num_hosts; h++) {
				struct fmd_component *owner;
				struct fmd_dc_region *r;

				if (!hosts[h])
					continue;
				owner = fmd_dc_host_component(dev, h);
				if (!owner) {
					load_error("%s: dcd region %d: host %d has no logical device",
						   dev->id, rid, h);
					continue;
				}

				r = fmd_dc_region_add(ctx, owner, (uint8_t)rid, (uint16_t)h);
				if (!r)
					continue;
				r->base_dpa = host_base[h];
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

	/*
	 * Extents: Get DC Region Extent Lists (5603h) + DC List Tags (5608h).
	 * An entry of the file is capacity: one extent of each host it names.
	 * "hosts" or "host_id" put it at the same offset into the region of
	 * each, "start_mb" being in the DPA space "base_mb" gives; "per_host"
	 * gives each host's own "start_mb", in that host's DPA space. With no
	 * host at all, it is capacity an FM reference keeps that no host
	 * holds.
	 */
	arr = json_object_get(jdcd, "extents");
	if (arr && json_is_array(arr)) {
		json_array_foreach(arr, idx, item) {
			uint64_t dpa = json_uint_or(item, "start_mb", 0) * FMD_MIB;
			uint64_t len = json_uint_or(item, "len_mb", 0) * FMD_MIB;
			const char *tag_str = json_str_or(item, "tag", "");
			bool pending = json_is_true(json_object_get(item, "pending"));
			bool fm_ref = json_is_true(json_object_get(item, "fm_referenced"));
			uint16_t seq = (uint16_t)json_int_or(item, "shared_seq", 0);
			json_t *jper = json_object_get(item, "per_host");
			uint8_t tag[FMD_DC_TAG_LEN];
			int rid = -1, n_hosts = 0;

			snprintf(what, sizeof(what), "%s: dcd extent %zu", dev->id, idx);
			if (fmd_dc_tag_from_str(tag_str, tag) < 0)
				load_error("%s: invalid tag '%s'", what, tag_str);

			if (jper) {
				size_t pidx;
				json_t *p;

				if (!json_is_array(jper) || json_object_get(item, "hosts") ||
				    json_object_get(item, "host_id")) {
					load_error("%s: per_host is not a list, or is given "
						   "with hosts or host_id", what);
					continue;
				}
				json_array_foreach(jper, pidx, p) {
					json_t *jh = json_object_get(p, "host_id");
					json_int_t h = json_is_integer(jh) ? json_integer_value(jh) : -1;
					uint64_t hdpa = json_uint_or(p, "start_mb", 0) * FMD_MIB;
					uint64_t hlen = json_uint_or(p, "len_mb", len / FMD_MIB) * FMD_MIB;
					json_t *jpend = json_object_get(p, "pending");
					int hrid;

					if (h < 0 || h >= dcd->num_hosts ||
					    !fmd_dc_host_component(dev, (int)h)) {
						load_error("%s: per_host %zu: host_id is not one of "
							   "the %d hosts", what, pidx, dcd->num_hosts);
						continue;
					}
					if (!json_object_get(p, "start_mb")) {
						load_error("%s: per_host %zu: no start_mb", what, pidx);
						continue;
					}
					hrid = host_region_of(dev, (int)h, hdpa);
					if (hlen == 0 || hrid < 0) {
						load_error("%s: per_host %zu: %" PRIu64 " MB is empty or "
							   "is outside region base to base + length of "
							   "every region of host %d", what, pidx,
							   (uint64_t)(hdpa / FMD_MIB), (int)h);
						continue;
					}
					n_hosts++;
					mock_record_add(ctx, dev, what, (int)h, hdpa, hlen, tag,
							(uint16_t)json_int_or(p, "shared_seq", seq),
							(uint8_t)hrid,
							jpend ? json_is_true(jpend) : pending);
				}
			} else {
				for (int r = 0; r < FMD_MAX_DC_REGIONS; r++) {
					if (region_len[r] && dpa >= region_base[r] &&
					    dpa < region_base[r] + region_len[r]) {
						rid = r;
						break;
					}
				}
				if (len == 0 || rid < 0) {
					load_error("%s: %" PRIu64 " MB is empty or is outside region "
						   "base to base + length of every region",
						   what, (uint64_t)(dpa / FMD_MIB));
					continue;
				}

				parse_host_set(what, item, dcd->num_hosts, 1, hosts);
				for (int h = 0; h < dcd->num_hosts; h++) {
					struct fmd_dc_region *r;

					if (!hosts[h])
						continue;
					r = fmd_dc_find_region(dev, h, (uint8_t)rid);
					if (!r) {
						load_error("%s: host %d does not have region %d",
							   what, h, rid);
						continue;
					}
					n_hosts++;
					/* the same offset into the host's region */
					mock_record_add(ctx, dev, what, h,
							r->base_dpa + (dpa - region_base[rid]), len,
							tag, seq, (uint8_t)rid, pending);
				}
			}
			n_extents += n_hosts;

			if (fm_ref)
				fmd_dc_mock_ref_set(dcd, tag, true);
			if (!n_hosts) {
				struct fmd_dc_mock_hold *hold;

				if (!fm_ref) {
					load_error("%s: no host holds it, and it is not "
						   "fm_referenced", what);
					continue;
				}
				for (int r = 0; rid < 0 && r < FMD_MAX_DC_REGIONS; r++) {
					if (region_len[r] && dpa >= region_base[r] &&
					    dpa < region_base[r] + region_len[r])
						rid = r;
				}
				if (len == 0 || rid < 0) {
					load_error("%s: %" PRIu64 " MB is empty or is outside region "
						   "base to base + length of every region",
						   what, (uint64_t)(dpa / FMD_MIB));
					continue;
				}
				hold = calloc(1, sizeof(*hold));
				if (!hold)
					continue;
				memcpy(hold->tag, tag, FMD_DC_TAG_LEN);
				hold->region_id = (uint8_t)rid;
				hold->shared_seq = seq;
				hold->phys = dpa - region_base[rid];
				hold->len = len;
				hold->next = dcd->mock_orphans;
				dcd->mock_orphans = hold;
			}
		}
	}
	parse_dc_tags(dev, dcd, json_object_get(jdcd, "tags"));
	check_mock_phys(dev, dcd);

	/* Number of Available Extents and Tags, Section 9.13.3.3 */
	fmd_dc_mock_recount(dev);
	/* Dynamic Capacity List Tags (5608h) */
	fmd_dc_mock_retag(dev);

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

		if (!mock_add(ctx, &mhd->base, parent))
			return -1;

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
				if (ld_idx >= FMD_MAX_LD_POOL) {
					load_error("more than %d LDs in a pool, the most cxl-fmd keeps",
						   FMD_MAX_LD_POOL);
					break;
				}

				struct fmd_ld *ld = parse_ld_pool_entry(ctx, jld, id, ld_idx,
									def_health, def_media);
				if (ld)
					mock_add(ctx, &ld->base, &mhd->base);
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
				if (head_idx >= FMD_MAX_HEADS) {
					load_error("more than %d heads, the most cxl-fmd keeps",
						   FMD_MAX_HEADS);
					break;
				}

				struct fmd_head *head = parse_head(ctx, jhead, id, head_idx);
				if (head && mock_add(ctx, &head->base, &mhd->base)) {

					/* Update LD bindings based on head's lds array */
					json_t *jlds = json_object_get(jhead, "lds");
					update_ld_bindings_for_head(mhd, head, jlds);
				}
			}

			mhd->num_heads = (uint8_t)json_array_size(heads);
		}

		if (fmd_create_mhd_memory_resources(ctx, mhd))
			load_error("%s: its memory domain could not be made", id);
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

		if (!mock_add(ctx, &sld->base, parent))
			return -1;
		if (fmd_create_sld_memory_resources(ctx, sld))
			load_error("%s: its memory domain could not be made", id);
		parse_dcd(ctx, jdev, &sld->base);
		parse_event_records(ctx, jdev, &sld->base);

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

		if (!mock_add(ctx, &mld->base, parent))
			return -1;

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
				if (ld_idx >= FMD_MAX_LD_COUNT) {
					load_error("more than %d LDs in an MLD", FMD_MAX_LD_COUNT);
					break;
				}

				struct fmd_ld *ld = parse_ld(ctx, jld, id, ld_idx,
							    def_health, def_media);
				struct fmd_component *other;

				if (!ld)
					continue;
				/*
				 * One LD per LD-ID: a second would give the
				 * domain two chunks with one ID.
				 */
				fmd_for_each_child_type(&mld->base, other, FMD_COMPONENT_LD) {
					if (fmd_to_ld(other)->ld_id == ld->ld_id)
						break;
				}
				if (other) {
					load_error("%s: two LDs with LD-ID %d", id, ld->ld_id);
					fmd_component_free(&ld->base);
					continue;
				}
				mock_add(ctx, &ld->base, &mld->base);
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
					ld->health_valid = true;
					ld->memory_size = per_ld_mb * 1024ULL * 1024;
					/* all persistent, as parse_ld() has an LD by default */
					ld->memdev.total_capacity = per_ld_mb / 256;
					ld->memdev.persistent_capacity = per_ld_mb / 256;
					/* 100% ≈ 255 in CXL format (8-bit fraction) */
					ld->qos_allocated_bw = 255;
					ld->qos_limit_bw = 255;
					ld->health.health_status = def_health;
					ld->health.media_status = def_media;
					mock_add(ctx, &ld->base, &mld->base);
				}
			}
		}

		if (fmd_create_mld_memory_resources(ctx, mld))
			load_error("%s: its memory domain could not be made", id);
		parse_dcd(ctx, jdev, &mld->base);
		parse_event_records(ctx, jdev, &mld->base);

		fmd_info(ctx, "Mock MLD %s: %d LDs", id, mld->ld_count);
	}

	return 0;
}


/*
 * "bind_result", "unbind_result": what the mock switch answers Bind vPPB
 * and Unbind vPPB with (see mock_bg_start() in src/discovery.c). The return
 * codes are those of Table 8-208.
 */
static uint16_t parse_bind_result(json_t *jsw, const char *key)
{
	static const struct {
		const char *name;
		uint16_t rc;
	} results[] = {
		{ "success", CXLMI_RET_SUCCESS },
		{ "invalid_input", CXLMI_RET_INPUT },
		{ "internal_error", CXLMI_RET_INTERNAL },
		{ "busy", CXLMI_RET_BUSY },
		{ "unsupported", CXLMI_RET_UNSUPPORTED },
	};
	const char *str = json_str_or(jsw, key, "success");

	for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
		if (!strcmp(str, results[i].name))
			return results[i].rc;
	}
	load_error("%s \"%s\" is not success, invalid_input, internal_error, "
		   "busy or unsupported", key, str);
	return CXLMI_RET_SUCCESS;
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
	load_error("binding_status \"%s\" is not unbound, busy, "
		   "bound_physical_port, bound_ld or bound_pbr", str);
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
		if (vcs_idx >= FMD_MAX_VCS) {
			load_error("more than %d VCSs in a switch", FMD_MAX_VCS);
			break;
		}

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
				if (vppb_idx >= FMD_MAX_VPPB) {
					load_error("more than %d vPPBs in a VCS", FMD_MAX_VPPB);
					break;
				}

				struct fmd_vppb *vppb = &vcs->vppbs[vppb_idx];

				vppb->vppb_id = (uint8_t)json_int_or(jvppb, "vppb_id", vppb_idx);

				const char *status = json_str_or(jvppb, "binding_status", "unbound");
				vppb->binding_status = parse_binding_status(status);

				vppb->bound_port_id = (uint8_t)json_int_or(jvppb, "bound_port_id", 0);
				/*
				 * Bound LD ID is FFh unless the vPPB is bound
				 * to an LD (Table 7-34): an absent one is FFh,
				 * and any other on a vPPB that is not bound_ld
				 * is warned of and made FFh.
				 */
				vppb->bound_ld_id = (uint8_t)json_int_or(jvppb, "bound_ld_id",
									 FMD_VPPB_NO_LD);
				if (vppb->binding_status != FMD_VPPB_BOUND_LD &&
				    vppb->bound_ld_id != FMD_VPPB_NO_LD) {
					fmd_warn(ctx, "mock topology: %s VCS %d vPPB %d is "
						 "%s with bound_ld_id %d: Bound LD ID is "
						 "FFh unless the vPPB is bound_ld (Table "
						 "7-34), taken as 255", sw->base.id,
						 vcs->vcs_id, vppb->vppb_id, status,
						 vppb->bound_ld_id);
					vppb->bound_ld_id = FMD_VPPB_NO_LD;
				}
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
	/* binds and unbinds in the background, as a switch does them */
	sw->mock_bind_time_ms = (unsigned int)json_uint_or(jsw, "bind_time_ms", 0);
	sw->mock_bind_result = parse_bind_result(jsw, "bind_result");
	sw->mock_unbind_result = parse_bind_result(jsw, "unbind_result");

	if (!mock_add(ctx, &sw->base, NULL))
		return -1;

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
			if (strcmp(link, "up") == 0)
				port->link_status = FMD_LINK_UP;
			else if (strcmp(link, "training") == 0)
				port->link_status = FMD_LINK_TRAINING;
			else if (strcmp(link, "down") == 0)
				port->link_status = FMD_LINK_DOWN;
			else
				load_error("port %d: link_status \"%s\" is not up, down "
					   "or training", port_num, link);
			/* PCIe encoding: 5 is 32.0 GT/s */
			port->max_link_speed = (uint8_t)json_int_or(jport, "max_link_speed", 5);
			port->current_link_speed = (port->link_status == FMD_LINK_UP) ?
				(uint8_t)json_int_or(jport, "current_link_speed", 5) : 0;

			if (is_upstream) {
				/* USP - mark as upstream and add to tree */
				port->state = FMD_PORT_USP;
				port->conn_dev_type = FMD_CONN_NONE;
				port->max_link_width = (uint8_t)json_int_or(jport, "max_link_width", 16);
				port->negotiated_link_width = (uint8_t)json_int_or(jport, "negotiated_link_width", 16);
				/* the same defaults as a DSP's, below */
				port->port_cxl_ver_bitmask = (uint8_t)json_int_or(jport, "port_cxl_ver_bitmask", 0x06);
				port->supported_link_speeds = (uint8_t)json_int_or(jport, "supported_link_speeds", 0x1F);
				if (mock_add(ctx, &port->base, &sw->base))
					sw->num_usps++;
				fmd_dbg(ctx, "Mock USP %s: link=%s",
					port_id, port->link_status == FMD_LINK_UP ? "up" : "down");
				continue;
			}

			/* DSP configuration */
			/* Connected Device Type, Get Physical Port State (Table 7-21) */
			const char *conn = json_str_or(jport, "conn_dev_type", "none");
			if (strcmp(conn, "none") == 0)
				port->conn_dev_type = FMD_CONN_NONE;
			else if (strcmp(conn, "pcie") == 0)
				port->conn_dev_type = FMD_CONN_PCIE;
			else if (strcmp(conn, "type1") == 0)
				port->conn_dev_type = FMD_CONN_TYPE1;
			else if (strcmp(conn, "type2") == 0)
				port->conn_dev_type = FMD_CONN_TYPE2;
			else if (strcmp(conn, "sld") == 0)
				port->conn_dev_type = FMD_CONN_SLD;
			else if (strcmp(conn, "mld") == 0)
				port->conn_dev_type = FMD_CONN_MLD;
			else
				load_error("port %d: conn_dev_type \"%s\" is not none, "
					   "pcie, type1, type2, sld or mld", port_num, conn);

			port->max_link_width = (uint8_t)json_int_or(jport, "max_link_width", 16);
			port->negotiated_link_width = (uint8_t)json_int_or(jport, "negotiated_link_width", 16);
			port->supported_ld_count = (uint8_t)json_int_or(jport, "supported_ld_count", 16);

			/*
			 * CXL mode information (Get Physical Port State). Connected
			 * Device Mode 0h is "Connection is not CXL or is
			 * disconnected" (Table 7-21): a CXL device on a port whose
			 * link is up is given 2h, CXL 68B Flit and VH mode, one of
			 * the modes of the default Supported CXL Modes below. That
			 * default is the mock's choice.
			 */
			bool cxl_dev = port->conn_dev_type == FMD_CONN_TYPE1 ||
				       port->conn_dev_type == FMD_CONN_TYPE2 ||
				       port->conn_dev_type == FMD_CONN_SLD ||
				       port->conn_dev_type == FMD_CONN_MLD;
			port->conn_dev_cxl_ver = (uint8_t)json_int_or(jport, "conn_dev_cxl_ver",
				(cxl_dev && port->link_status == FMD_LINK_UP) ? 0x2 : 0);
			port->port_cxl_ver_bitmask = (uint8_t)json_int_or(jport, "port_cxl_ver_bitmask", 0x06);
			port->supported_link_speeds = (uint8_t)json_int_or(jport, "supported_link_speeds", 0x1F);

			if (!mock_add(ctx, &port->base, &sw->base))
				continue;
			sw->num_dsps++;

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
				if (mock_add(ctx, &port->base, &sw->base))
					sw->num_dsps++;
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

			/*
			 * A head is a port of a Multi-Headed device, so the
			 * device is not on one switch port: each head is on
			 * a port of its own.
			 */
			if (!strcmp(json_str_or(jdev, "type", ""), "MHD")) {
				load_error("%s: a Multi-Headed device is not on one "
					   "switch port; give each head its "
					   "parent_switch and parent_port",
					   json_str_or(jdev, "id", "?"));
				continue;
			}

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
					/* as a port the file describes gets by default */
					port->conn_dev_cxl_ver = 0x2;
					port->port_cxl_ver_bitmask = 0x06;
					port->supported_link_speeds = 0x1F;
					if (mock_add(ctx, &port->base, &sw->base))
						sw->num_dsps++;
					else
						port = NULL;
				}
			}

			if (port) {
				/*
				 * A port the file describes is as the file
				 * describes it: whether that agrees with the
				 * device on it is for the validator to say.
				 */

				/*
				 * Only update conn_dev_type if not already set from explicit
				 * port configuration. MHD devices use ld_pool, not ld_count.
				 */
				if (port->conn_dev_type == FMD_CONN_NONE) {
					int ld_count = (int)json_int_or(jdev, "ld_count", 1);
					port->conn_dev_type = (ld_count > 1) ? FMD_CONN_MLD : FMD_CONN_SLD;
				}

				/* Parse device under this port */
				if (parse_device(ctx, jdev, &port->base) < 0)
					load_error("device %zu could not be loaded", idx);
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

	parse_event_records(ctx, jsw, &sw->base);

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
	 * Each VCS represents a host connection via its USP. A VCS that is
	 * not enabled has none: its USP ID is "Valid only when the VCS is
	 * enabled" (Table 7-34).
	 */
	for (int i = 0; i < sw->num_vcs; i++) {
		struct fmd_vcs *vcs = &sw->vcs[i];
		char host_id[FMD_MAX_ID_LEN];
		struct fmd_host *host;

		if (vcs->state != 0x01)		/* VCS State 01h: Enabled */
			continue;

		snprintf(host_id, sizeof(host_id), "%.50s_host%d", id, vcs->vcs_id);

		host = fmd_host_create(host_id);
		if (host) {
			snprintf(host->name, sizeof(host->name), "Host via %s VCS %d",
				 id, vcs->vcs_id);
			snprintf(host->switch_id, sizeof(host->switch_id), "%s", id);
			host->vcs_id = vcs->vcs_id;
			host->usp_port_id = vcs->upstream_port_id;
			host->direct_attached = false;

			if (mock_add(ctx, &host->base, NULL))
				fmd_info(ctx, "Mock host %s: VCS %d USP port %d",
					 host_id, vcs->vcs_id, vcs->upstream_port_id);
		}
	}

	return 0;
}


/*
 * Parse a host system from JSON
 */
/*
 * The heads of a Multi-Headed device that are on switch ports: each head
 * that gives a parent_switch and parent_port is put on that port.
 *
 * Only a head that presents an SLD can be. A head that presents an MLD is
 * bound by LD, with LD-IDs "relative to that head" (CXL r4.0 Section 2.5.1),
 * and no command reports which LD of the pool each of them is; such a head
 * on a switch cannot be managed, and is refused rather than guessed at.
 */
static void place_heads(struct fmd_ctx *ctx, json_t *jdev)
{
	const char *dev_id = json_str_or(jdev, "id", NULL);
	json_t *heads = json_object_get(jdev, "heads");
	size_t idx;
	json_t *jhead;

	if (!dev_id || strcmp(json_str_or(jdev, "type", ""), "MHD") != 0 ||
	    !heads || !json_is_array(heads))
		return;

	json_array_foreach(heads, idx, jhead) {
		const char *sw_id = json_str_or(jhead, "parent_switch", NULL);
		int port_num = (int)json_int_or(jhead, "parent_port", -1);
		char head_id[FMD_MAX_ID_LEN];
		struct fmd_component *comp;
		struct fmd_port *port;
		struct fmd_head *head;

		if (!sw_id && port_num < 0)
			continue;	/* on a host directly */
		if (!sw_id || port_num < 0) {
			load_error("%s: head %zu needs both parent_switch and "
				   "parent_port", dev_id, idx);
			continue;
		}

		(void)snprintf(head_id, sizeof(head_id), "%.50s_head%d", dev_id, (int)idx);
		comp = fmd_topology_find(&ctx->topology, head_id);
		port = fmd_topology_find_port(&ctx->topology, sw_id, port_num);
		if (!comp || comp->type != FMD_COMPONENT_HEAD)
			continue;	/* the head itself did not load */
		head = fmd_to_head(comp);

		if (!port) {
			load_error("%s: head %zu is on switch %s port %d, which "
				   "the topology does not have", dev_id, idx,
				   sw_id, port_num);
			continue;
		}
		if (port->head || port->base.first_child) {
			load_error("%s: head %zu is on switch %s port %d, which "
				   "already has a device", dev_id, idx, sw_id, port_num);
			continue;
		}
		if (head->presentation != FMD_HEAD_SLD) {
			load_error("%s: head %zu presents an MLD on a switch "
				   "port: not supported, as nothing reports which "
				   "LD of the pool each of its LD-IDs is",
				   dev_id, idx);
			continue;
		}

		head->port = port;
		port->head = head;
	}
}

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

	/* a host on a VCS is on a VCS of some switch: which one */
	if (!host->direct_attached) {
		const char *sw_id = json_str_or(jhost, "switch", NULL);

		if (!sw_id)
			load_error("host %s is on VCS %d and does not say of which "
				   "\"switch\"", id, host->vcs_id);
		else
			snprintf(host->switch_id, sizeof(host->switch_id), "%s", sw_id);
	}

	if (!mock_add(ctx, &host->base, NULL))
		return -1;

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
	load_ctx = ctx;
	load_errors = 0;

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
			if (parse_switch(ctx, jsw, devices) < 0)
				load_error("switch %zu could not be loaded", idx);
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

			if (parse_device(ctx, jdev, NULL) < 0)
				load_error("device %zu could not be loaded", idx);
		}
	}

	/* Put the heads that are on switch ports on them */
	if (devices && json_is_array(devices)) {
		size_t idx;
		json_t *jdev;

		json_array_foreach(devices, idx, jdev)
			place_heads(ctx, jdev);
	}

	/* Parse hosts */
	json_t *hosts = json_object_get(root, "hosts");
	if (hosts && json_is_array(hosts)) {
		size_t idx;
		json_t *jhost;

		json_array_foreach(hosts, idx, jhost) {
			if (parse_host(ctx, jhost) < 0)
				load_error("host %zu could not be loaded", idx);
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
			if (mock_add(ctx, &local_host->base, NULL))
				fmd_info(ctx, "Created default local host for direct-attached devices");
		}
	}

	parse_unplug(ctx, fabric, json_object_get(root, "unplug"));
	parse_external(ctx, fabric, json_object_get(root, "external"));

	/* a device on a switch or port that is not there was never added */
	if (devices && json_is_array(devices)) {
		size_t idx;
		json_t *jdev;

		json_array_foreach(devices, idx, jdev) {
			const char *dev_id = json_str_or(jdev, "id", NULL);
			const char *parent_sw = json_str_or(jdev, "parent_switch", NULL);

			if (dev_id && parent_sw &&
			    !fmd_topology_find(&ctx->topology, dev_id))
				load_error("device %s is on switch %s port %lld, "
					   "which the topology does not have", dev_id,
					   parent_sw,
					   (long long)json_int_or(jdev, "parent_port", -1));
		}
	}

	if (load_errors) {
		fmd_err(ctx, "%s: %d error(s)", path, load_errors);
		goto out;
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
	free(fabric->unplug);
	free(fabric->external);
	free(fabric);
}

/*
 * A device pulled out of a switch port: the port links down and reports no
 * device (Connected Device Type 00h). This is a surprise removal, and for
 * an MLD "The switch will automatically unbind any existing LD bindings"
 * (CXL r4.0 Section 7.6.6.8). For an SLD on a bound port, "the FM can be notified but is not involved"
 * (Section 7.6.6.7), and neither section says what becomes of the binding.
 * A vPPB bound to an SLD port "behaves the same as a PPB in a PCIe switch"
 * (Section 7.2.3.1), so cxl-fmd assumes it stays bound, and so it stays.
 * Then cxl-fmd does what it does when a switch reports this: the same calls
 * as a port read again after a Physical Switch Event Record.
 */
int fmd_mock_unplug_next(struct fmd_ctx *ctx)
{
	struct fmd_mock_fabric *fabric = ctx->mock_fabric;
	struct fmd_component *dev;
	struct fmd_switch *sw;
	struct fmd_port *port;
	const char *id;

	if (!fabric || fabric->unplug_next >= fabric->unplug_count)
		return -1;
	id = fabric->unplug[fabric->unplug_next++];

	pthread_rwlock_wrlock(&ctx->fabric_lock);

	dev = fmd_topology_find(&ctx->topology, id);
	if (!dev || !dev->parent || dev->parent->type != FMD_COMPONENT_PORT ||
	    !dev->parent->parent ||
	    dev->parent->parent->type != FMD_COMPONENT_SWITCH) {
		pthread_rwlock_unlock(&ctx->fabric_lock);
		fmd_warn(ctx, "mock: %s is not on a switch port to be unplugged", id);
		return 0;
	}
	port = fmd_to_port(dev->parent);
	sw = fmd_to_switch(dev->parent->parent);
	fmd_info(ctx, "mock: %s is pulled out of port %d of %s", id,
		 port->port_number, sw->base.id);

	if (dev->type == FMD_COMPONENT_MLD) {
		for (int v = 0; v < sw->num_vcs; v++) {
			struct fmd_vcs *vcs = &sw->vcs[v];

			for (int i = 0; i < vcs->num_vppbs; i++) {
				struct fmd_vppb *vppb = &vcs->vppbs[i];

				if (vppb->binding_status != FMD_VPPB_BOUND_LD ||
				    vppb->bound_port_id != port->port_number)
					continue;
				vppb->binding_status = FMD_VPPB_UNBOUND;
				vppb->bound_port_id = 0;
				vppb->bound_ld_id = FMD_VPPB_NO_LD;
				vppb->port = NULL;
				vppb->ld = NULL;
			}
		}
	}

	port->conn_dev_type = FMD_CONN_NONE;
	port->conn_dev_cxl_ver = 0;
	port->link_status = FMD_LINK_DOWN;
	port->negotiated_link_width = 0;
	port->current_link_speed = 0;

	fmd_switch_port_device_removed(ctx, sw, port, dev);
	fmd_binding_marks_prune(ctx, sw->base.id);

	pthread_rwlock_unlock(&ctx->fabric_lock);
	return 0;
}

/*
 * A bind or unbind made outside cxl-fmd: the next of the topology's
 * "external" list. The switch's table changes with no task. With
 * emit_event, the switch records a Binding Change, as "Successful bind and
 * unbind operations generate events to the Informational Event Log" (Table
 * 7-80), and it is acted on as one read from a switch's log.
 */
int fmd_mock_external_next(struct fmd_ctx *ctx)
{
	struct fmd_mock_fabric *fabric = ctx->mock_fabric;
	struct fmd_mock_external *e;
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	struct fmd_vppb *vppb;

	if (!fabric || fabric->external_next >= fabric->external_count)
		return -1;
	e = &fabric->external[fabric->external_next++];

	pthread_rwlock_wrlock(&ctx->fabric_lock);
	sw = fmd_topology_find_switch(&ctx->topology, e->switch_id);
	vcs = sw ? fmd_switch_find_vcs(sw, e->vcs_id) : NULL;
	vppb = vcs ? fmd_vcs_find_vppb(vcs, e->vppb_id) : NULL;
	if (!vppb || vppb->mock_done_ms) {
		pthread_rwlock_unlock(&ctx->fabric_lock);
		fmd_warn(ctx, "mock: external %s of vPPB %d of VCS %d of %s not "
			 "made: %s", e->bind ? "bind" : "unbind", e->vppb_id,
			 e->vcs_id, e->switch_id,
			 vppb ? "the switch is binding or unbinding it" :
				"it is not there");
		return 0;
	}

	if (e->bind) {
		vppb->binding_status = e->ld_id != 0xFFFF ?
			FMD_VPPB_BOUND_LD : FMD_VPPB_BOUND_PHYSICAL_PORT;
		vppb->bound_port_id = e->port_id;
		vppb->bound_ld_id = e->ld_id != 0xFFFF ? (uint8_t)e->ld_id :
							 FMD_VPPB_NO_LD;
	} else {
		vppb->binding_status = FMD_VPPB_UNBOUND;
		vppb->bound_port_id = 0;
		vppb->bound_ld_id = FMD_VPPB_NO_LD;
		vppb->port = NULL;
		vppb->ld = NULL;
	}
	vppb->bound_pbr_id = 0;
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);
	fmd_info(ctx, "mock: vPPB %d of VCS %d of %s %s outside cxl-fmd%s",
		 e->vppb_id, e->vcs_id, sw->base.id,
		 e->bind ? "bound" : "unbound",
		 e->emit_event ? "" : ", with no event record");

	/*
	 * The table is the mock switch's, and what a Connection is computed
	 * from: with or without a record, it is seen at once. What the record
	 * changes is that a task following the vPPB ends.
	 */
	fmd_binding_marks_prune(ctx, sw->base.id);
	if (e->emit_event) {
		fmd_mock_binding_record(sw, 0, e->vcs_id, vppb);
		fmd_task_binding_changed(ctx, sw, e->vcs_id, e->vppb_id, false,
					 vppb->binding_status,
					 vppb->bound_port_id,
					 vppb->bound_ld_id);
	}

	pthread_rwlock_unlock(&ctx->fabric_lock);
	return 0;
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
