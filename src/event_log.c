// SPDX-License-Identifier: BSD-3-Clause
/*
 * CXL event records, as Redfish log entries
 *
 * A component's event records are read with Get Event Records (0100h) and
 * then cleared, so that it can record more: what cxl-fmd has read is the
 * only copy there is. It keeps them here, per component, and serves them as
 * LogEntry resources: "A LogEntry resource is used to capture CXL event
 * records. The CXLEntryType property shall contain the specific CXL entry
 * type, and the EntryType property shall contain CXL. Overflow information
 * is captured in properties of the LogEntry prior to the overflow
 * condition." (DMTF DSP0288, Table 1, Comment 1)
 *
 * Where they are is cxl-fmd's mapping. DSP0288 puts the records of a memory
 * device in the LogService of its Memory resource (Table 1, Comment 2), but
 * the schema allows that LogService, DeviceLog, only under
 * /redfish/v1/Systems/{id}/Memory/{id}, and cxl-fmd's Memory resources are
 * under Chassis. It does allow one under each CXLLogicalDevice, and so:
 *
 *  - The records of a memory device are in the DeviceLog of the CXL logical
 *    device they are about: LD 0 of an SLD, the LD an MLD's record names,
 *    or every LD of it for a record about the whole device. Its
 *    MaxNumberOfRecords is what DSP0288 says it is for a Memory resource,
 *    the sum of the sizes of the four logs.
 *  - The records of a switch are in a LogService of its Chassis. The
 *    Switch resource has LogServices, and the schema allows no URI for
 *    them; the Chassis is where it allows one.
 *
 * Dynamic Capacity Event Log records are acted on (src/dcd.c) and not kept:
 * nothing places them.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxl-fmd.h"
#include "redfish/handlers_internal.h"

/* Get Event Records, Table 8-232 */
#define EVENT_FLAG_OVERFLOW	(1 << 0)
#define EVENT_FLAG_MORE		(1 << 1)
#define EVENT_MAX_PAGES		32

/* how far back a record read again is looked for: see fmd_log_add() */
#define FMD_LOG_SAME_SCAN	256

/* records kept for a log whose size no command reports */
#define FMD_LOG_DEFAULT_SIZE	256

/* Common Event Record Format, Table 8-221: Event Record Flags Bit[7] */
#define EVENT_RECORD_LD_ID_VALID	(1 << 7)

/* Event Record Identifiers (Table 8-221), as RFC 4122 lays a UUID out */
static const struct {
	uint8_t uuid[16];
	const char *name;
} record_kinds[] = {
	{ { 0xfb, 0xcd, 0x0a, 0x77, 0xc2, 0x60, 0x41, 0x7f,
	    0x85, 0xa9, 0x08, 0x8b, 0x16, 0x21, 0xeb, 0xa6 }, "General Media Event Record" },
	{ { 0x60, 0x1d, 0xcb, 0xb3, 0x9c, 0x06, 0x4e, 0xab,
	    0xb8, 0xaf, 0x4e, 0x9b, 0xfb, 0x5c, 0x96, 0x24 }, "DRAM Event Record" },
	{ { 0xfe, 0x92, 0x74, 0x75, 0xdd, 0x59, 0x43, 0x39,
	    0xa5, 0x86, 0x79, 0xba, 0xb1, 0x13, 0xb7, 0x74 }, "Memory Module Event Record" },
	{ { 0xe7, 0x1f, 0x3a, 0x40, 0x2d, 0x29, 0x40, 0x92,
	    0x8a, 0x39, 0x4d, 0x1c, 0x96, 0x6c, 0x7c, 0x65 }, "Memory Sparing Event Record" },
	{ { 0x77, 0xcf, 0x92, 0x71, 0x9c, 0x02, 0x47, 0x0b,
	    0x9f, 0xe4, 0xbc, 0x7b, 0x75, 0xf2, 0xda, 0x97 }, "Physical Switch Event Record" },
	{ { 0x40, 0xd2, 0x64, 0x25, 0x33, 0x96, 0x4c, 0x4d,
	    0xa5, 0xda, 0x3d, 0x47, 0x26, 0x3a, 0xf4, 0x25 }, "Virtual Switch Event Record" },
	{ { 0x8d, 0xc4, 0x43, 0x63, 0x0c, 0x96, 0x47, 0x10,
	    0xb7, 0xbf, 0x04, 0xbb, 0x99, 0x53, 0x4c, 0x3f }, "MLD Port Event Record" },
};

enum { KIND_PHYSICAL_SWITCH = 4, KIND_VIRTUAL_SWITCH = 5, KIND_MLD_PORT = 6 };

static int record_kind(const uint8_t uuid[16])
{
	for (size_t i = 0; i < sizeof(record_kinds) / sizeof(record_kinds[0]); i++) {
		if (!memcmp(uuid, record_kinds[i].uuid, 16))
			return (int)i;
	}
	return -1;
}

int fmd_event_record_kind_from_name(const char *name, uint8_t uuid[16])
{
	static const char *const names[] = {
		"general_media", "dram", "memory_module", "memory_sparing",
		"physical_switch", "virtual_switch", "mld_port",
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(name, names[i])) {
			memcpy(uuid, record_kinds[i].uuid, 16);
			return 0;
		}
	}
	return -1;
}

/*
 * ============================================================================
 * The records kept
 * ============================================================================
 */

/*
 * How many records the log service of @comp keeps. For a memory device it
 * is what DSP0288 says MaxNumberOfRecords is, the sum of the sizes of its
 * four logs in Identify Memory Device (Table 8-309). Where those are not
 * known, and for a switch, no command reports a size.
 */
static int log_capacity(struct fmd_component *comp)
{
	const struct cxlmi_cmd_memdev_identify_rsp *id = NULL;
	int sum;

	if (comp->type == FMD_COMPONENT_SLD)
		id = &fmd_to_sld(comp)->memdev;
	else if (comp->type == FMD_COMPONENT_LD)
		id = &fmd_to_ld(comp)->memdev;
	if (!id)
		return FMD_LOG_DEFAULT_SIZE;

	sum = id->info_event_log_size + id->warning_event_log_size +
	      id->failure_event_log_size + id->fatal_event_log_size;
	return sum ? sum : FMD_LOG_DEFAULT_SIZE;
}

/*
 * The log of a component, made when it first has a record. Its capacity is
 * what the device says; the memory for it grows with the records kept, as
 * four 16-bit log sizes could ask for a quarter of a million of them.
 */
static struct fmd_log *log_of(struct fmd_component *comp)
{
	if (!comp->log) {
		struct fmd_log *log = calloc(1, sizeof(*log));

		if (!log)
			return NULL;
		log->capacity = log_capacity(comp);
		comp->log = log;
	}
	return comp->log;
}

/* The @i-th oldest record kept */
static struct fmd_log_entry *log_at(struct fmd_log *log, int i)
{
	return &log->entries[(log->first + i) % log->allocated];
}

/* room for one more record, up to the capacity: 0, or -1 if there is none */
static int log_grow(struct fmd_log *log)
{
	struct fmd_log_entry *entries;
	int allocated;

	if (log->count < log->allocated)
		return 0;
	if (log->allocated >= log->capacity)
		return -1;

	allocated = log->allocated ? log->allocated * 2 : 16;
	if (allocated > log->capacity)
		allocated = log->capacity;
	entries = calloc(allocated, sizeof(*entries));
	if (!entries)
		return -1;

	/* oldest first, from 0 */
	for (int i = 0; i < log->count; i++)
		entries[i] = *log_at(log, i);
	free(log->entries);
	log->entries = entries;
	log->allocated = allocated;
	log->first = 0;
	return 0;
}

int fmd_log_add(struct fmd_component *comp, int which,
		const struct cxlmi_event_record *rec)
{
	struct fmd_log *log = log_of(comp);
	struct fmd_log_entry *entry;

	if (!log)
		return -1;

	/*
	 * A record read again because its Clear Event Records failed is the
	 * same record: same handle, timestamp and contents. It is kept once.
	 */
	for (int i = log->count - 1, n = 0; i >= 0 && n < FMD_LOG_SAME_SCAN; i--, n++) {
		struct fmd_log_entry *e = log_at(log, i);

		if (e->log == which && !memcmp(&e->record, rec, sizeof(*rec)))
			return 0;
	}

	/* full: the oldest goes, as the service says it does */
	if (log_grow(log) < 0) {
		if (!log->allocated)
			return -1;
		log->first = (log->first + 1) % log->allocated;
		log->count--;
	}
	entry = log_at(log, log->count++);
	memset(entry, 0, sizeof(*entry));
	entry->id = ++log->last_id;
	entry->log = which;
	entry->created = time(NULL);
	entry->record = *rec;
	return 0;
}

/*
 * A log that reported Overflow. "When an event log overflows, the device
 * shall retain all event records, in the specific event log, that occurred
 * before the overflow event" (Section 8.2.10.2.2): the last record read
 * from it is the one before the overflow, and carries it.
 */
void fmd_log_overflowed(struct fmd_component *comp, int which, uint16_t count,
			uint64_t first_ns, uint64_t last_ns)
{
	struct fmd_log *log = log_of(comp);

	if (!log)
		return;
	log->overflow = true;

	for (int i = log->count - 1; i >= 0; i--) {
		struct fmd_log_entry *entry = log_at(log, i);

		if (entry->log != which)
			continue;
		entry->overflow = true;
		entry->overflow_count = count;
		entry->first_overflow_ns = first_ns;
		entry->last_overflow_ns = last_ns;
		return;
	}
}

void fmd_log_clear(struct fmd_component *comp)
{
	if (comp->log) {
		comp->log->first = 0;
		comp->log->count = 0;
		comp->log->overflow = false;
	}
}

void fmd_log_free(struct fmd_component *comp)
{
	if (comp->log) {
		free(comp->log->entries);
		free(comp->log);
		comp->log = NULL;
	}
}

/*
 * ============================================================================
 * Reading a log
 * ============================================================================
 */

/*
 * Read one event log of a component to its end, clearing what is read.
 * @act is called for each record before it is cleared: the record exists
 * nowhere else afterwards. If the log reported Overflow, @overflow says so.
 */
int fmd_event_log_drain(struct fmd_ctx *ctx, struct fmd_component *comp,
			struct cxlmi_tunnel_info *ti, int which,
			fmd_event_record_fn act, void *arg,
			struct fmd_overflow *overflow)
{
	struct cxlmi_cmd_get_event_records_req req = { .event_log = which };
	struct cxlmi_cmd_get_event_records_rsp *rsp;
	struct cxlmi_cmd_clear_event_records_req *clr;
	int rc = 0;

	memset(overflow, 0, sizeof(*overflow));
	overflow->log = which;

	rsp = calloc(1, sizeof(*rsp) +
		     CXLMI_MAX_SUPPORTED_EVENT_RECORDS * sizeof(rsp->records[0]));
	clr = calloc(1, sizeof(*clr) +
		     CXLMI_MAX_SUPPORTED_EVENT_RECORDS * sizeof(clr->handles[0]));
	if (!rsp || !clr) {
		free(rsp);
		free(clr);
		return -1;
	}

	for (int page = 0; page < EVENT_MAX_PAGES; page++) {
		rc = cxlmi_cmd_get_event_records(comp->ep, ti, &req, rsp);
		if (rc)
			break;
		if (rsp->flags & EVENT_FLAG_OVERFLOW) {
			overflow->set = true;
			overflow->count = rsp->overflow_err_count;
			overflow->first_ns = rsp->first_overflow_timestamp;
			overflow->last_ns = rsp->last_overflow_timestamp;
		}
		if (!rsp->record_count)
			break;
		FMD_CCI_LIST_CLAMP(rsp->record_count,
				   CXLMI_MAX_SUPPORTED_EVENT_RECORDS);

		for (int i = 0; i < rsp->record_count; i++) {
			clr->handles[i] = rsp->records[i].handle;
			act(ctx, comp, which, &rsp->records[i], arg);
		}

		clr->event_log = which;
		clr->clear_flags = 0;
		clr->nr_recs = rsp->record_count;
		rc = cxlmi_cmd_clear_event_records(comp->ep, ti, clr);
		if (rc) {
			fmd_warn(ctx, "%s: clear event records of log %d failed: %d",
				 comp->id, which, rc);
			break;
		}

		/*
		 * Read again even when the page did not say there were More
		 * Event Records: a log's Event Notification is sent when it goes
		 * from empty to not, and "After clearing log entries, the Get
		 * Event Records command should be used to confirm that all
		 * entries have been cleared from the log to ensure that the
		 * notification trigger has been re-armed" (Section 8.2.10.2.8).
		 */
	}

	free(rsp);
	free(clr);
	return rc;
}

/*
 * ============================================================================
 * The logs of memory devices
 * ============================================================================
 *
 * Read only where they are the FM's: on a component's own MCTP CCI, or on
 * the FM-owned LD of an MLD behind a switch port, which "processes"
 * commands sent there (Section 7.6.3.1). Not through the kernel's mailbox,
 * whose logs are the host's (fmd_endpoint_owns_events()), and not for an
 * LD reached through a tunnel, whose logs are its host's.
 *
 * A Multi-Headed device's records name an LD-ID "relative to that head"
 * (Section 2.5.1), and nothing says which LD of the pool that is. Its logs
 * are not read.
 */

/* the LD of an MLD with @ld_id */
static struct fmd_ld *mld_ld(struct fmd_mld *mld, int ld_id)
{
	struct fmd_component *child;

	fmd_for_each_child_type(&mld->base, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->ld_id == ld_id)
			return fmd_to_ld(child);
	}
	return NULL;
}

void fmd_device_record_keep(struct fmd_ctx *ctx, struct fmd_component *dev,
			    int which, const struct cxlmi_event_record *rec,
			    void *arg)
{
	struct fmd_component *child;

	(void)arg;

	if (dev->type == FMD_COMPONENT_SLD) {
		fmd_log_add(dev, which, rec);
		return;
	}

	/*
	 * An MLD: "If set to 1, the LD-ID field in this record is valid";
	 * cleared, "the event is applicable to the entire device", and so
	 * to every LD (Table 8-221).
	 */
	if (rec->flags[0] & EVENT_RECORD_LD_ID_VALID) {
		struct fmd_ld *ld = mld_ld(fmd_to_mld(dev), rec->ld_id);

		if (ld)
			fmd_log_add(&ld->base, which, rec);
		else
			fmd_warn(ctx, "%s: event record for LD %d, which it does not have",
				 dev->id, rec->ld_id);
		return;
	}
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD)
		fmd_log_add(child, which, rec);
}

static void device_logs(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct cxlmi_tunnel_info tbuf, *ti = CXLMI_NO_TUNNEL;

	if (dev->type == FMD_COMPONENT_MLD && fmd_to_mld(dev)->switch_port >= 0) {
		DEFINE_CXLMI_TUNNEL_SWITCH(t, fmd_to_mld(dev)->switch_port);
		tbuf = t;
		ti = &tbuf;
	}

	/* Informational, Warning, Failure, Fatal */
	for (int which = 0; which <= 3; which++) {
		struct fmd_overflow o;
		struct fmd_component *child;

		fmd_event_log_drain(ctx, dev, ti, which, fmd_device_record_keep, NULL, &o);
		if (!o.set)
			continue;

		/* records were lost, to the device and so to every LD */
		if (dev->type == FMD_COMPONENT_SLD) {
			fmd_log_overflowed(dev, which, o.count, o.first_ns, o.last_ns);
			continue;
		}
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD)
			fmd_log_overflowed(child, which, o.count, o.first_ns, o.last_ns);
	}
}

bool fmd_device_logs_read(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	if (unlikely(!dev->ep) || !fmd_endpoint_owns_events(ctx, dev->ep))
		return false;
	/*
	 * An SLD on a switch port: the endpoint is the switch's, and an SLD
	 * port is not a target of the Tunnel Management Command.
	 */
	if (dev->type == FMD_COMPONENT_SLD)
		return !(dev->parent && dev->parent->type == FMD_COMPONENT_PORT);
	if (dev->type != FMD_COMPONENT_MLD)
		return false;

	/*
	 * An MLD whose LDs could not be read: its records would be cleared
	 * from it with nothing to keep them on, and they are the only copy.
	 */
	return fmd_first_child_type(dev, FMD_COMPONENT_LD) != NULL;
}

/*
 * Ask a memory device for an MCTP Event Notification when one of the logs
 * cxl-fmd reads goes from empty to not (Set MCTP Event Interrupt Policy,
 * Table 8-236): Bits[3:0] for the four logs it keeps, Bit[4] for the
 * Dynamic Capacity Event Log of a DCD. The bits already set stay set.
 *
 * Only a device on an MCTP endpoint of its own: one reached through a
 * switch's tunnel has no defined way to send the FM a notification, as the
 * requester it would answer is the switch. Its logs are read on the timer.
 */
#define NOTIFY_LOGS		0x000f
#define NOTIFY_DC_LOG		0x0010
#define NOTIFY_DEFINED		0x801f	/* Bits[14:5] are reserved */

void fmd_device_events_enable(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct cxlmi_cmd_get_mctp_event_interrupt_policy_rsp cur = { 0 };
	struct cxlmi_cmd_set_mctp_event_interrupt_policy_req pol = { 0 };
	uint16_t want = 0;
	int rc;

	if (unlikely(!dev->ep) || !fmd_endpoint_owns_events(ctx, dev->ep) ||
	    (dev->parent && dev->parent->type == FMD_COMPONENT_PORT))
		return;

	if (fmd_device_logs_read(ctx, dev))
		want |= NOTIFY_LOGS;
	if (fmd_memdev_dcd(dev))
		want |= NOTIFY_DC_LOG;
	if (!want)
		return;

	rc = cxlmi_cmd_get_mctp_event_interrupt_policy(dev->ep, CXLMI_NO_TUNNEL, &cur);
	if (rc) {
		fmd_dbg(ctx, "%s: no MCTP event notifications (%d)", dev->id, rc);
		return;
	}
	/*
	 * Set even when the bits are set already: "The receiver may capture
	 * the address of the requesting component ... The subsequent enabled
	 * events shall be sent to that address" (Section 8.2.10.2.7). Bits
	 * another requester set send it, not cxl-fmd, its notifications.
	 *
	 * "message reserved bits should be cleared to 0 by the sender"
	 */
	pol.event_interrupt_settings = (cur.event_interrupt_settings | want) &
				       NOTIFY_DEFINED;
	rc = cxlmi_cmd_set_mctp_event_interrupt_policy(dev->ep, CXLMI_NO_TUNNEL, &pol);

	/*
	 * Read it back: a libcxlmi that does not take Set's response payload
	 * reports a failure when the device has set the policy.
	 */
	if (rc && cxlmi_cmd_get_mctp_event_interrupt_policy(dev->ep, CXLMI_NO_TUNNEL,
							   &cur) == 0 &&
	    (cur.event_interrupt_settings & want) == want)
		rc = 0;
	if (rc)
		fmd_dbg(ctx, "%s: no MCTP event notifications (%d)", dev->id, rc);
	else
		fmd_dbg(ctx, "%s: MCTP event notifications for logs %#x", dev->id,
			want);
}

void fmd_device_logs_poll(struct fmd_ctx *ctx)
{
	static const enum fmd_component_type types[] = {
		FMD_COMPONENT_SLD, FMD_COMPONENT_MLD,
	};
	struct fmd_component *comp, **devs;
	int n = 0, max = ctx->topology.root->device_count;

	devs = calloc(max ? max : 1, sizeof(*devs));
	if (!devs)
		return;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		fmd_for_each_type(&ctx->topology, comp, types[t]) {
			if (fmd_device_logs_read(ctx, comp) && n < max)
				devs[n++] = comp;
		}
	}
	for (int i = 0; i < n; i++)
		device_logs(ctx, devs[i]);
	free(devs);
}

/*
 * ============================================================================
 * Redfish
 * ============================================================================
 */

static void iso_time(time_t t, char *buf, size_t len)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* A CXL timestamp: unsigned nanoseconds since midnight, 01-Jan-1970 UTC */
static void cxl_time(uint64_t ns, char *buf, size_t len)
{
	iso_time((time_t)(ns / 1000000000ULL), buf, len);
}

static const char *const entry_types[] = {
	"Informational", "Warning", "Failure", "Fatal",
};

/*
 * Severity of the Redfish entry, by the log the record was in. This is
 * cxl-fmd's mapping: nothing gives one. Informational is OK, Warning is
 * Warning, and Failure and Fatal are Critical.
 */
static const char *const severities[] = {
	"OK", "Warning", "Critical", "Critical",
};

/* What a record says, for the kinds whose fields are acted on */
static void entry_message(const struct fmd_log_entry *entry, char *buf, size_t len)
{
	const struct cxlmi_event_record *rec = &entry->record;
	static const char *const vcs_events[] = {
		"binding change", "secondary bus reset",
		"link control register updated", "slot control register updated",
	};
	static const char *const mld_events[] = {
		"correctable error message received",
		"non-fatal error message received",
		"fatal error message received",
	};
	int kind = record_kind(rec->uuid);

	switch (kind) {
	case KIND_PHYSICAL_SWITCH:	/* Table 7-79 */
		snprintf(buf, len, "Physical port %d: %s", rec->data[0],
			 rec->data[1] == 0 ? "link state change" :
			 rec->data[1] == 1 ? "slot status register updated" :
			 "event of a reserved type");
		return;
	case KIND_VIRTUAL_SWITCH:	/* Table 7-80 */
		snprintf(buf, len, "vPPB %d of VCS %d: %s", rec->data[1],
			 rec->data[0], rec->data[2] < 4 ? vcs_events[rec->data[2]] :
			 "event of a reserved type");
		return;
	case KIND_MLD_PORT:		/* Table 7-81 */
		snprintf(buf, len, "MLD port %d: %s", rec->data[1],
			 rec->data[0] < 3 ? mld_events[rec->data[0]] :
			 "event of a reserved type");
		return;
	case -1:
		snprintf(buf, len, "Event record "
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 rec->uuid[0], rec->uuid[1], rec->uuid[2], rec->uuid[3],
			 rec->uuid[4], rec->uuid[5], rec->uuid[6], rec->uuid[7],
			 rec->uuid[8], rec->uuid[9], rec->uuid[10], rec->uuid[11],
			 rec->uuid[12], rec->uuid[13], rec->uuid[14], rec->uuid[15]);
		return;
	default:
		snprintf(buf, len, "%s", record_kinds[kind].name);
		return;
	}
}

static json_t *log_service(struct fmd_component *comp, const char *base,
			   const char *id, bool enabled)
{
	struct fmd_log *log = comp->log;
	char now[32];

	iso_time(time(NULL), now, sizeof(now));

	return json_pack("{s:s, s:s, s:s, s:s, s:b, s:s, s:i, s:s, s:b, s:s, s:o,"
			 " s:{s:s}, s:{s:{s:s}}}",
		"@odata.id", base,
		"@odata.type", "#LogService.v1_9_0.LogService",
		"Id", id,
		"Name", "CXL Event Log",
		"ServiceEnabled", enabled,
		"LogEntryType", "CXL",
		"MaxNumberOfRecords", log ? log->capacity : log_capacity(comp),
		"OverWritePolicy", "WrapsWhenFull",
		"Overflow", log ? log->overflow : 0,
		"DateTime", now,
		"Status", rf_status(enabled ? "Enabled" : "Disabled", "OK"),
		"Entries", "@odata.id", rf_uri(0, "%s/Entries", base),
		"Actions", "#LogService.ClearLog", "target",
			rf_uri(1, "%s/Actions/LogService.ClearLog", base));
}

static json_t *log_entry(struct fmd_component *comp, const char *base, int id,
			 const char *origin)
{
	struct fmd_log *log = comp->log;
	struct fmd_log_entry *entry = NULL;
	char created[32], msg[160], ts[32], uri[512];
	json_t *result;

	for (int i = 0; log && i < log->count && !entry; i++) {
		if (log_at(log, i)->id == id)
			entry = log_at(log, i);
	}
	if (!entry)
		return NULL;

	snprintf(uri, sizeof(uri), "%s/Entries/%d", base, id);
	iso_time(entry->created, created, sizeof(created));
	entry_message(entry, msg, sizeof(msg));

	result = json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s}",
		"@odata.id", uri,
		"@odata.type", "#LogEntry.v1_22_0.LogEntry",
		"Id", uri + strlen(base) + strlen("/Entries/"),
		"Name", "CXL Event Record",
		"EntryType", "CXL",
		"CXLEntryType", entry_types[entry->log],
		"Severity", severities[entry->log],
		"Created", created,
		"Message", msg);

	/* "If the device does not have a valid timestamp, return 0" */
	if (entry->record.timestamp) {
		cxl_time(entry->record.timestamp, ts, sizeof(ts));
		json_object_set_new(result, "EventTimestamp", json_string(ts));
	}

	if (origin)
		json_object_set_new(result, "Links",
			json_pack("{s:{s:s}}", "OriginOfCondition", "@odata.id", origin));

	/* the record before the log overflowed carries it */
	if (entry->overflow) {
		json_object_set_new(result, "OverflowErrorCount",
				    json_integer(entry->overflow_count));
		if (entry->first_overflow_ns) {
			cxl_time(entry->first_overflow_ns, ts, sizeof(ts));
			json_object_set_new(result, "FirstOverflowTimestamp",
					    json_string(ts));
		}
		if (entry->last_overflow_ns) {
			cxl_time(entry->last_overflow_ns, ts, sizeof(ts));
			json_object_set_new(result, "LastOverflowTimestamp",
					    json_string(ts));
		}
	}

	return result;
}

/* the CXL logical device a DeviceLog is of, and whether its logs are read */
static struct fmd_component *ld_component(struct fmd_ctx *ctx, const char *chassis_id,
					  const char *pcie_id, const char *ld_id,
					  bool *enabled)
{
	struct fmd_component *dev = fmd_topology_find(&ctx->topology, chassis_id);
	struct fmd_component *child;
	int ld_idx = parse_ld_id(ld_id);

	if (!dev || !fmd_is_memdev(dev) || strcmp(chassis_id, pcie_id) || ld_idx < 0)
		return NULL;

	/* in a mock topology records are given; on hardware they are read */
	*enabled = unlikely(!dev->ep) || fmd_device_logs_read(ctx, dev);

	/* an SLD is its own LD 0 */
	if (dev->type == FMD_COMPONENT_SLD)
		return ld_idx == 0 ? dev : NULL;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->ld_id != ld_idx)
			continue;
		/* a pool LD of a Multi-Headed device: see device_logs() */
		if (dev->type == FMD_COMPONENT_MHD)
			*enabled = false;
		return child;
	}
	return NULL;
}

/* where a switch record points: the port, or the vPPB */
static const char *switch_origin(struct fmd_switch *sw, const struct fmd_log_entry *e,
				 char *buf, size_t len)
{
	const struct cxlmi_event_record *rec = &e->record;
	struct fmd_component *child;
	int kind = record_kind(rec->uuid);
	int port = kind == KIND_PHYSICAL_SWITCH ? rec->data[0] :
		   kind == KIND_MLD_PORT ? rec->data[1] : -1;

	if (kind == KIND_VIRTUAL_SWITCH) {
		struct fmd_vcs *vcs = fmd_switch_find_vcs(sw, rec->data[0]);

		if (!vcs || !fmd_vcs_find_vppb(vcs, rec->data[1]))
			return NULL;
		snprintf(buf, len, "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%d/VPPBs/%d",
			 sw->base.id, rec->data[0], rec->data[1]);
		return buf;
	}
	if (port < 0)
		return NULL;
	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		if (fmd_to_port(child)->port_number == port) {
			snprintf(buf, len, "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
				 sw->base.id, child->id);
			return buf;
		}
	}
	return NULL;
}

/*
 * The entries of a log, expanded: LogEntryCollection has its Members
 * auto-expanded (OData.AutoExpand).
 */
static json_t *log_entries(struct fmd_component *comp, const char *base,
			   const char *(*origin)(struct fmd_component *,
						 const struct fmd_log_entry *,
						 char *, size_t),
			   const char *fixed_origin)
{
	struct fmd_log *log = comp->log;
	json_t *members = json_array();
	char buf[512];

	for (int i = 0; log && i < log->count; i++) {
		struct fmd_log_entry *e = log_at(log, i);

		json_array_append_new(members, log_entry(comp, base, e->id,
			origin ? origin(comp, e, buf, sizeof(buf)) : fixed_origin));
	}

	return json_pack("{s:s, s:s, s:s, s:I, s:o}",
		"@odata.id", rf_uri(0, "%s/Entries", base),
		"@odata.type", "#LogEntryCollection.LogEntryCollection",
		"Name", "CXL Event Log Entries",
		"Members@odata.count", (json_int_t)json_array_size(members),
		"Members", members);
}

json_t *fmd_redfish_ld_log(struct fmd_ctx *ctx, const char *chassis_id,
			   const char *pcie_id, const char *ld_id,
			   const char *what, int id)
{
	char base[600], origin[512];
	bool enabled = false;
	struct fmd_component *comp = ld_component(ctx, chassis_id, pcie_id, ld_id,
						  &enabled);

	if (!comp)
		return NULL;
	snprintf(origin, sizeof(origin),
		 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s",
		 chassis_id, pcie_id, ld_id);
	snprintf(base, sizeof(base), "%s/DeviceLog", origin);

	if (!strcmp(what, "service"))
		return log_service(comp, base, "DeviceLog", enabled);
	if (!strcmp(what, "entries"))
		return log_entries(comp, base, NULL, origin);
	return log_entry(comp, base, id, origin);
}

int fmd_redfish_ld_log_clear(struct fmd_ctx *ctx, const char *chassis_id,
			     const char *pcie_id, const char *ld_id)
{
	bool enabled;
	struct fmd_component *comp = ld_component(ctx, chassis_id, pcie_id, ld_id,
						  &enabled);

	if (!comp)
		return -1;
	fmd_log_clear(comp);
	return 0;
}

#define SWITCH_LOG_ID "EventLog"

static const char *switch_entry_origin(struct fmd_component *comp,
				       const struct fmd_log_entry *e,
				       char *buf, size_t len)
{
	return switch_origin(fmd_to_switch(comp), e, buf, len);
}

/* The log services of a switch's Chassis */
json_t *fmd_redfish_switch_log(struct fmd_ctx *ctx, const char *chassis_id,
			       const char *log_id, const char *what, int id)
{
	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, chassis_id);
	char base[512], origin[512];
	struct rf_collection coll;
	bool enabled;

	if (!sw)
		return NULL;
	enabled = unlikely(!sw->base.ep) ||
		  fmd_endpoint_owns_events(ctx, sw->base.ep);

	if (!strcmp(what, "services")) {
		rf_collection_init(&coll,
			rf_uri(0, "/redfish/v1/Chassis/%s/LogServices", chassis_id),
			"#LogServiceCollection.LogServiceCollection", "Log Services");
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s/LogServices/%s",
				  chassis_id, SWITCH_LOG_ID);
		return rf_collection_finish(&coll);
	}

	if (!log_id || strcmp(log_id, SWITCH_LOG_ID))
		return NULL;
	snprintf(base, sizeof(base), "/redfish/v1/Chassis/%s/LogServices/%s",
		 chassis_id, SWITCH_LOG_ID);

	if (!strcmp(what, "service"))
		return log_service(&sw->base, base, SWITCH_LOG_ID, enabled);
	if (!strcmp(what, "entries"))
		return log_entries(&sw->base, base, switch_entry_origin, NULL);

	for (int i = 0; sw->base.log && i < sw->base.log->count; i++) {
		struct fmd_log_entry *e = log_at(sw->base.log, i);

		if (e->id == id)
			return log_entry(&sw->base, base, id,
					 switch_origin(sw, e, origin, sizeof(origin)));
	}
	return NULL;
}

int fmd_redfish_switch_log_clear(struct fmd_ctx *ctx, const char *chassis_id,
				 const char *log_id)
{
	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, chassis_id);

	if (!sw || strcmp(log_id, SWITCH_LOG_ID))
		return -1;
	fmd_log_clear(&sw->base);
	return 0;
}
