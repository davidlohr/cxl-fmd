/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Redfish Handlers Internal Header
 *
 * Shared utilities and declarations for handler modules.
 */

#ifndef FMD_HANDLERS_INTERNAL_H
#define FMD_HANDLERS_INTERNAL_H

#define _GNU_SOURCE  /* For strptime, timegm */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

#include "cxl-fmd.h"
#include "generated/enums/pcie_device.h"

/*
 * Safely parse a non-negative integer from the start of a string.
 * Returns the integer value (>= 0) on success, -1 on parse failure.
 *
 * Unlike atoi(), this function:
 * - Returns -1 for non-numeric strings (atoi returns 0)
 * - Returns -1 for negative values
 * - Returns -1 for overflow
 *
 * Like atoi(), this function allows trailing content after the number
 * (e.g., "42_chunk" parses as 42). This is needed for parsing resource
 * IDs like "cxl_mld_0_ld0_chunk".
 */
static inline int safe_parse_int(const char *str)
{
	if (!str || *str == '\0')
		return -1;

	/* Must start with a digit */
	if (*str < '0' || *str > '9')
		return -1;

	char *endptr;
	long val = strtol(str, &endptr, 10);

	/* Must have consumed at least one digit */
	if (endptr == str)
		return -1;

	/* Validate range */
	if (val < 0 || val > INT_MAX)
		return -1;

	return (int)val;
}

/*
 * The largest number of MiB that a byte count of 64 bits holds: 2^44 - 1.
 * The CXL fields a size in MiB becomes are such counts: Length, "The
 * number of bytes of capacity to add" (Table 7-72) and "to remove" (Table
 * 7-73), each 8 bytes, and Region Block Size (Table 7-69), "in bytes"
 * (Table 8-347); so are the sizes and offsets cxl-fmd keeps. A value past
 * it cannot be carried, and is refused rather than wrapped (cxl-fmd's
 * bound, from that representation).
 */
#define RF_MIB_MAX (UINT64_MAX >> 20)

/*
 * A size or offset in MiB from a request, @v, as a number of bytes.
 * Returns 0 with *bytes set, or -EINVAL when @v is not an integer, is
 * negative, is 0 and !@allow_zero, or is past RF_MIB_MAX. Nothing is
 * written to *bytes on an error.
 */
static inline int rf_mib_to_bytes(const json_t *v, bool allow_zero, uint64_t *bytes)
{
	json_int_t mib;

	if (!json_is_integer(v))
		return -EINVAL;
	mib = json_integer_value(v);
	if (mib < 0 || (mib == 0 && !allow_zero) || (uint64_t)mib > RF_MIB_MAX)
		return -EINVAL;
	*bytes = (uint64_t)mib << 20;
	return 0;
}

/*
 * Parse numeric LD ID from a string.
 * Handles two formats:
 *   1. Full format with "_ld": "mld_0_ld2" -> 2
 *   2. Plain number: "0" or "2" -> 0 or 2
 *
 * Returns LD ID (>= 0) on success, -1 on parse failure.
 */
static inline int parse_ld_id(const char *str)
{
	if (!str)
		return -1;

	/* Try to find "_ld" prefix first */
	const char *ld_str = strstr(str, "_ld");
	if (ld_str)
		return safe_parse_int(ld_str + 3);

	/* Otherwise treat as plain number */
	return safe_parse_int(str);
}

/*
 * Parse numeric ID from a string containing "_vcs", "_vppb", etc.
 * Handles: "switch_0_vcs1" -> 1, "3" -> 3
 *
 * Returns ID (>= 0) on success, -1 on parse failure.
 */
static inline int parse_prefixed_id(const char *str, const char *prefix)
{
	if (!str)
		return -1;

	/* Try to find the prefix first */
	const char *num_str = strstr(str, prefix);
	if (num_str)
		return safe_parse_int(num_str + strlen(prefix));

	/* Otherwise treat as plain number */
	return safe_parse_int(str);
}

/*
 * Parse LD ID from Memory resource path (wrapper for compatibility).
 */
static inline int parse_ld_id_from_memory_path(const char *memory_path)
{
	return parse_ld_id(memory_path);
}

/*
 * Validate InterleaveSets[0].Memory reference for MLD.
 * Checks that the Memory path references a valid LD in the specified chassis.
 * Returns LD ID on success, -1 on error.
 */
static inline int validate_interleave_set_memory(struct fmd_ctx *ctx,
						 const char *chassis_id,
						 const char *memory_path)
{
	if (!ctx || !chassis_id || !memory_path)
		return -1;

	/* Check memory_path contains chassis_id */
	if (!strstr(memory_path, chassis_id))
		return -1;  /* Memory not in target chassis */

	int ld_id = parse_ld_id_from_memory_path(memory_path);
	if (ld_id < 0)
		return -1;  /* Invalid Memory path format */

	/* Verify LD exists */
	struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology, chassis_id, ld_id);
	if (!ld)
		return -1;  /* LD not found */

	return ld_id;
}

/*
 * Format CXL timestamp (nanoseconds since Unix epoch) as ISO 8601 DateTimeOffset.
 * Returns dynamically allocated string or NULL on error.
 * Caller must free the returned string.
 */
static inline char *format_cxl_timestamp(uint64_t timestamp_ns)
{
	time_t secs = timestamp_ns / 1000000000ULL;
	struct tm tm_info;

	if (!gmtime_r(&secs, &tm_info))
		return NULL;

	char *buf = malloc(32);
	if (!buf)
		return NULL;

	/* ISO 8601 format: YYYY-MM-DDTHH:MM:SSZ */
	strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
	return buf;
}

/*
 * Parse ISO 8601 DateTimeOffset to CXL timestamp (nanoseconds since Unix epoch).
 * Accepts formats: YYYY-MM-DDTHH:MM:SSZ or YYYY-MM-DDTHH:MM:SS+00:00
 * Returns nanoseconds since epoch, or 0 on parse error.
 */
static inline uint64_t parse_cxl_timestamp(const char *str)
{
	struct tm tm_info = {0};
	char *end;

	if (!str)
		return 0;

	/* Parse ISO 8601 format */
	end = strptime(str, "%Y-%m-%dT%H:%M:%S", &tm_info);
	if (!end)
		return 0;

	/* Convert to time_t (UTC) */
	time_t secs = timegm(&tm_info);
	if (secs == (time_t)-1)
		return 0;

	return (uint64_t)secs * 1000000000ULL;
}

/*
 * ============================================================================
 * Redfish Collection Builder Utilities
 * ============================================================================
 *
 * Helper for building Redfish collection JSON responses.
 * Handles the common pattern of Members array with @odata.count.
 *
 * Usage:
 *   struct rf_collection coll;
 *   rf_collection_init(&coll, "/redfish/v1/Fabrics/CXL/Switches",
 *                      "#SwitchCollection.SwitchCollection",
 *                      "CXL Switch Collection");
 *
 *   fmd_topology_rdlock(&ctx->topology);
 *   fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
 *       rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Switches/%s",
 *                         comp->id);
 *   }
 *   fmd_topology_rdunlock(&ctx->topology);
 *
 *   return rf_collection_finish(&coll);
 */

struct rf_collection {
	json_t *members;
	int count;
	const char *odata_id;
	const char *odata_type;
	const char *name;
};

static inline void rf_collection_init(struct rf_collection *coll,
				      const char *odata_id,
				      const char *odata_type,
				      const char *name)
{
	coll->members = json_array();
	coll->count = 0;
	coll->odata_id = odata_id;
	coll->odata_type = odata_type;
	coll->name = name;
}

/*
 * Add a member to the collection with formatted URI.
 * Printf-style format string for building the @odata.id.
 */
static inline void rf_collection_add(struct rf_collection *coll,
				     const char *fmt, ...)
{
	char uri[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(uri, sizeof(uri), fmt, ap);
	va_end(ap);

	json_array_append_new(coll->members,
		json_pack("{s:s}", "@odata.id", uri));
	coll->count++;
}

/*
 * Finalize collection and return the JSON object.
 * The rf_collection struct should not be used after this call.
 */
static inline json_t *rf_collection_finish(struct rf_collection *coll)
{
	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", coll->odata_id,
		"@odata.type", coll->odata_type,
		"Name", coll->name,
		"Members@odata.count", coll->count,
		"Members", coll->members
	);
}

/*
 * Helper for building dynamic URI strings.
 * Returns a statically allocated buffer - use immediately or copy. The
 * buffers are the calling thread's: requests run on several.
 */
#define RF_URI_BUFS 4
static inline char *rf_uri(int idx, const char *fmt, ...)
{
	static _Thread_local char bufs[RF_URI_BUFS][256];
	va_list ap;

	idx = idx % RF_URI_BUFS;
	va_start(ap, fmt);
	vsnprintf(bufs[idx], sizeof(bufs[idx]), fmt, ap);
	va_end(ap);

	return bufs[idx];
}

/*
 * ============================================================================
 * Single-Resource Helper Utilities
 * ============================================================================
 *
 * Helpers for common patterns in single-resource handlers.
 */

/*
 * Build a Redfish Status object.
 * Common states: "Enabled", "Disabled", "StandbyOffline", "StandbySpare",
 *                "InTest", "Starting", "Absent", "UnavailableOffline",
 *                "Deferring", "Quiesced", "Updating"
 * Common health: "OK", "Warning", "Critical"
 *
 * Usage:
 *   json_object_set_new(result, "Status", rf_status("Enabled", "OK"));
 */
static inline json_t *rf_status(const char *state, const char *health)
{
	return json_pack("{s:s, s:s}", "State", state, "Health", health);
}

/*
 * Build a Redfish Status object with HealthRollup.
 * HealthRollup aggregates health from subordinate resources.
 */
static inline json_t *rf_status_rollup(const char *state, const char *health,
				       const char *health_rollup)
{
	return json_pack("{s:s, s:s, s:s}",
			 "State", state,
			 "Health", health,
			 "HealthRollup", health_rollup);
}

/*
 * Build an @odata.id link object.
 * Usage:
 *   json_array_append_new(endpoints, rf_link("/redfish/v1/.../%s", id));
 */
static inline json_t *rf_link(const char *fmt, ...)
{
	char uri[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(uri, sizeof(uri), fmt, ap);
	va_end(ap);

	return json_pack("{s:s}", "@odata.id", uri);
}

/*
 * Build a navigation property link (subordinate resource reference).
 * Usage:
 *   json_object_set_new(result, "Ports", rf_nav("/redfish/v1/.../Ports"));
 *   // Result: "Ports": {"@odata.id": "/redfish/v1/.../Ports"}
 */
static inline json_t *rf_nav(const char *fmt, ...)
{
	char uri[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(uri, sizeof(uri), fmt, ap);
	va_end(ap);

	return json_pack("{s:s}", "@odata.id", uri);
}

/*
 * Build standard Links object with single reference.
 * Usage:
 *   json_object_set_new(result, "Links",
 *       rf_links_single("Chassis", "/redfish/v1/Chassis/%s", chassis_id));
 */
static inline json_t *rf_links_single(const char *name, const char *fmt, ...)
{
	char uri[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(uri, sizeof(uri), fmt, ap);
	va_end(ap);

	return json_pack("{s:{s:s}}", name, "@odata.id", uri);
}

/*
 * Build Links object with single-element array reference.
 * Usage:
 *   json_object_set_new(result, "Links",
 *       rf_links_array_single("Zones", "/redfish/v1/.../Zones/%s", zone_id));
 *   // Result: "Links": {"Zones": [{"@odata.id": "/redfish/v1/.../Zones/..."}]}
 */
static inline json_t *rf_links_array_single(const char *name, const char *fmt, ...)
{
	char uri[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(uri, sizeof(uri), fmt, ap);
	va_end(ap);

	return json_pack("{s:[{s:s}]}", name, "@odata.id", uri);
}

/*
 * Map health status from CCI to Redfish string.
 */
static inline const char *rf_health_str(int health_status)
{
	/* Bits 0-1: overall device health */
	int overall = health_status & 0x03;
	switch (overall) {
	case 0: return "OK";
	case 1: return "Warning";   /* Non-critical */
	case 2: return "Critical";  /* Critical */
	case 3: return "Critical";  /* Fatal */
	default: return "OK";
	}
}

/*
 * Map link status to Redfish State string.
 */
static inline const char *rf_link_state_str(enum fmd_link_status status)
{
	switch (status) {
	case FMD_LINK_UP: return "Enabled";
	case FMD_LINK_DOWN: return "Disabled";
	case FMD_LINK_TRAINING: return "Starting";
	default: return "Disabled";
	}
}

#endif /* FMD_HANDLERS_INTERNAL_H */
