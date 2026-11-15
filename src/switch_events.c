// SPDX-License-Identifier: BSD-3-Clause
/*
 * The event logs of a switch
 *
 * A switch tells the FM what has happened on it with event records in its
 * logs (CXL r4.0 Section 7.6.8), read with Get Event Records (0100h) and
 * returned to it with Clear Event Records (0101h):
 *
 *  - A Physical Switch Event Record (Table 7-79) for a port whose link
 *    state or slot status has changed: a device plugged in or pulled out.
 *    "FM issues the Get Physical Port State command for the physical port
 *    that has linked up to discover the connected device type" (Section
 *    7.6.6.7).
 *  - A Virtual CXL Switch Event Record (Table 7-80) for a vPPB, among other
 *    things when its binding has changed. "Successful bind and unbind
 *    operations generate events to the Informational Event Log. Failed bind
 *    and unbind operations generate events to the Warning Event Log."
 *
 * A record that is read is cleared: "event records shall be cleared from the
 * device for the device to recycle those entries for a future event". A
 * switch whose logs are never read fills them and loses what comes after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl-fmd.h"

/* Get Event Records, Tables 8-231 and 8-232 */
#define EVENT_LOG_INFO		0
#define EVENT_LOG_WARNING	1
#define EVENT_LOG_FATAL		3

/* MCTP Event Interrupt Policy, Table 8-236 */
#define EVENT_NOTIFY_LOGS	0x000f	/* Informational to Fatal */
#define EVENT_NOTIFY_BG_DONE	(1 << 15)

/* Event Record Identifiers, Table 8-221, as RFC 4122 lays a UUID out */
static const uint8_t physical_switch_uuid[0x10] = {
	0x77, 0xcf, 0x92, 0x71, 0x9c, 0x02, 0x47, 0x0b,
	0x9f, 0xe4, 0xbc, 0x7b, 0x75, 0xf2, 0xda, 0x97,
};
static const uint8_t virtual_switch_uuid[0x10] = {
	0x40, 0xd2, 0x64, 0x25, 0x33, 0x96, 0x4c, 0x4d,
	0xa5, 0xda, 0x3d, 0x47, 0x26, 0x3a, 0xf4, 0x25,
};

/* what the logs of one switch said */
struct switch_events {
	uint8_t ports[32];	/* bit n: port n has changed */
	int nports;
	bool everything;	/* a log overflowed: records were lost */
	int records;
};

/*
 * One record. The fields after the common part start at 30h, which is
 * data[0] of struct cxlmi_event_record.
 */
static void handle_record(struct fmd_ctx *ctx, struct fmd_switch *sw, int log,
			  struct cxlmi_event_record *rec,
			  struct switch_events *ev)
{
	ev->records++;

	if (!memcmp(rec->uuid, physical_switch_uuid, sizeof(physical_switch_uuid))) {
		/* Table 7-79: 30h Physical Port ID, 31h Event Type */
		uint8_t port = rec->data[0];

		fmd_dbg(ctx, "%s: physical switch event %d on port %d",
			sw->base.id, rec->data[1], port);
		if (!(ev->ports[port / 8] & (1 << (port % 8)))) {
			ev->ports[port / 8] |= 1 << (port % 8);
			ev->nports++;
		}
		return;
	}

	if (!memcmp(rec->uuid, virtual_switch_uuid, sizeof(virtual_switch_uuid))) {
		/* Table 7-80: 30h VCS ID, 31h vPPB ID, 32h Event Type */
		uint8_t vcs_id = rec->data[0], vppb_id = rec->data[1];

		fmd_dbg(ctx, "%s: virtual switch event %d on vPPB %d of VCS %d, "
			"binding status %d", sw->base.id, rec->data[2], vppb_id,
			vcs_id, rec->data[3]);
		/* 00h = Binding Change; a failed one is in the Warning log */
		if (rec->data[2] == 0x00)
			fmd_task_binding_changed(ctx, sw, vcs_id, vppb_id,
						 log == EVENT_LOG_WARNING);
		return;
	}

	fmd_dbg(ctx, "%s: event record of a kind that is not acted on", sw->base.id);
}

/* each record of a switch's log: kept, and acted on */
static void switch_record(struct fmd_ctx *ctx, struct fmd_component *comp,
			  int which, const struct cxlmi_event_record *rec,
			  void *arg)
{
	struct fmd_switch *sw = fmd_to_switch(comp);

	/* whatever it is: it is not in the switch any more */
	fmd_log_add(comp, which, rec);
	handle_record(ctx, sw, which, (struct cxlmi_event_record *)rec, arg);
}

/* Read one log of a switch to its end, clearing what is read */
static int drain_log(struct fmd_ctx *ctx, struct fmd_switch *sw, int log,
		     struct switch_events *ev)
{
	struct fmd_overflow o;
	int rc;

	rc = fmd_event_log_drain(ctx, &sw->base, CXLMI_NO_TUNNEL, log,
				 switch_record, ev, &o);
	if (o.set) {
		ev->everything = true;
		fmd_log_overflowed(&sw->base, log, o.count, o.first_ns, o.last_ns);
	}
	return rc;
}

/* The logs of one switch, and what follows from them */
static void switch_events(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	struct switch_events ev = { 0 };
	uint8_t ids[256];
	int n = 0;

	/* Informational, Warning, Failure, Fatal: a switch has no fifth */
	for (int log = EVENT_LOG_INFO; log <= EVENT_LOG_FATAL; log++) {
		/* a log the switch cannot be asked for is none of ours */
		if (drain_log(ctx, sw, log, &ev))
			fmd_dbg(ctx, "%s: event log %d not read", sw->base.id, log);
	}

	/* records were lost: what they said is only in the switch itself */
	if (ev.everything) {
		fmd_warn(ctx, "%s: an event log overflowed, reading the switch again",
			 sw->base.id);
		fmd_switch_rediscover(ctx, sw);
		return;
	}

	if (!ev.nports)
		return;
	for (int id = 0; id < 256; id++) {
		if (ev.ports[id / 8] & (1 << (id % 8)))
			ids[n++] = id;
	}
	fmd_info(ctx, "%s: %d port(s) changed, reading them again", sw->base.id, n);
	fmd_switch_refresh_ports(ctx, sw, ids, n);
}

int fmd_switch_events_poll(struct fmd_ctx *ctx)
{
	return fmd_switch_events_poll_ep(ctx, NULL) < 0 ? -1 : 0;
}

int fmd_switch_events_poll_ep(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep)
{
	struct fmd_component *comp;
	struct fmd_switch **switches;
	int n = 0, devices;

	if (!ctx || !ctx->cxlmi)
		return -1;

	pthread_rwlock_wrlock(&ctx->fabric_lock);

	/* the list first: reading a port again can add to the topology */
	switches = calloc(ctx->topology.root->switch_count ?
			  ctx->topology.root->switch_count : 1, sizeof(*switches));
	if (!switches) {
		pthread_rwlock_unlock(&ctx->fabric_lock);
		return -1;
	}
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
		/* a log that is the host's is not drained */
		if (likely(comp->ep) && fmd_endpoint_owns_events(ctx, comp->ep) &&
		    fmd_component_on_ep(comp, ep) &&
		    n < ctx->topology.root->switch_count)
			switches[n++] = fmd_to_switch(comp);
	}

	for (int i = 0; i < n; i++)
		switch_events(ctx, switches[i]);
	free(switches);

	/* and the logs of the memory devices, which are kept the same way */
	devices = fmd_device_logs_poll(ctx, ep);

	pthread_rwlock_unlock(&ctx->fabric_lock);
	return n + devices;
}

/*
 * Ask a switch to send an Event Notification when one of its four logs has
 * a new record, and when a background operation completes (Set MCTP Event
 * Interrupt Policy, 0105h). A switch that will not is still read on the
 * timer.
 */
void fmd_switch_events_enable(struct fmd_ctx *ctx, struct fmd_switch *sw)
{
	struct cxlmi_cmd_get_mctp_event_interrupt_policy_rsp cur = { 0 };
	struct cxlmi_cmd_set_mctp_event_interrupt_policy_req pol = { 0 };
	uint16_t want = EVENT_NOTIFY_LOGS | EVENT_NOTIFY_BG_DONE;
	int rc;

	if (unlikely(!sw->base.ep))
		return;

	rc = cxlmi_cmd_get_mctp_event_interrupt_policy(sw->base.ep,
						       CXLMI_NO_TUNNEL, &cur);
	if (rc == 0 && (cur.event_interrupt_settings & want) != want) {
		pol.event_interrupt_settings = cur.event_interrupt_settings | want;
		rc = cxlmi_cmd_set_mctp_event_interrupt_policy(sw->base.ep,
							       CXLMI_NO_TUNNEL, &pol);
	}
	if (rc)
		fmd_dbg(ctx, "%s: no MCTP event notifications (%d)", sw->base.id, rc);
}
