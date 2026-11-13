// SPDX-License-Identifier: GPL-2.0-only
/*
 * MCTP Event Notification (CXL r3.2 Section 8.2.10.2.8) receive path.
 *
 * Devices send Event Notification requests to the EID that enabled them with
 * Set MCTP Event Interrupt Policy. AF_MCTP only delivers inbound requests to a
 * socket bound to the message type, so the daemon binds one listener for CXL
 * CCI messages and one for FM-API messages forwarded by a switch.
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/mctp.h>

#include "cxl-fmd.h"

#define MCTP_TYPE_CXL_FMAPI	0x7
#define MCTP_TYPE_CXL_CCI	0x8

#define CCI_CATEGORY_REQ	0
#define CCI_CATEGORY_RSP	1
#define CCI_CMDSET_EVENTS	0x01
#define CCI_CMD_EVENT_NOTIFY	0x06

/* Event Interrupt Settings bit 4: Dynamic Capacity Event Log */
#define CCI_EVENT_DC_LOG	(1 << 4)

static int bind_listener(struct fmd_ctx *ctx, uint8_t type)
{
	struct sockaddr_mctp addr = {
		.smctp_family = AF_MCTP,
		.smctp_network = MCTP_NET_ANY,
		.smctp_addr.s_addr = MCTP_ADDR_ANY,
		.smctp_type = type,
		.smctp_tag = MCTP_TAG_OWNER,
	};
	int sd;

	sd = socket(AF_MCTP, SOCK_DGRAM, 0);
	if (sd < 0) {
		fmd_dbg(ctx, "AF_MCTP socket: %s", strerror(errno));
		return -errno;
	}

	if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int rc = -errno;

		if (rc == -EADDRINUSE)
			fmd_warn(ctx, "MCTP type 0x%02x already bound by another process; "
				 "event notifications disabled", type);
		else
			fmd_warn(ctx, "bind MCTP type 0x%02x: %s", type, strerror(-rc));
		close(sd);
		return rc;
	}

	return sd;
}

int fmd_events_init(struct fmd_ctx *ctx)
{
	ctx->notify_sd[0] = bind_listener(ctx, MCTP_TYPE_CXL_CCI);
	ctx->notify_sd[1] = bind_listener(ctx, MCTP_TYPE_CXL_FMAPI);

	if (ctx->notify_sd[0] < 0 && ctx->notify_sd[1] < 0)
		return -1;

	fmd_info(ctx, "Listening for MCTP event notifications");
	return 0;
}

void fmd_events_cleanup(struct fmd_ctx *ctx)
{
	for (int i = 0; i < 2; i++) {
		if (ctx->notify_sd[i] >= 0)
			close(ctx->notify_sd[i]);
		ctx->notify_sd[i] = -1;
	}
}

static void handle_notification(struct fmd_ctx *ctx, int sd)
{
	uint8_t buf[sizeof(struct cxlmi_cci_msg) +
		    sizeof(struct cxlmi_cmd_event_notification_req)];
	struct cxlmi_cci_msg *msg = (struct cxlmi_cci_msg *)buf;
	struct cxlmi_cmd_event_notification_req *pl;
	struct sockaddr_mctp from;
	socklen_t fromlen = sizeof(from);
	struct cxlmi_cci_msg rsp;
	uint16_t events;
	ssize_t len;

	len = recvfrom(sd, buf, sizeof(buf), MSG_TRUNC,
		       (struct sockaddr *)&from, &fromlen);
	if (len < (ssize_t)sizeof(*msg))
		return;

	if (msg->category != CCI_CATEGORY_REQ ||
	    msg->command_set != CCI_CMDSET_EVENTS ||
	    msg->command != CCI_CMD_EVENT_NOTIFY) {
		fmd_dbg(ctx, "ignoring unsolicited CCI message %02x%02x from EID %u",
			msg->command_set, msg->command, from.smctp_addr.s_addr);
		return;
	}

	/* The device retransmits until it gets a response */
	rsp = (struct cxlmi_cci_msg) {
		.category = CCI_CATEGORY_RSP,
		.tag = msg->tag,
		.command = msg->command,
		.command_set = msg->command_set,
	};
	if (sendto(sd, &rsp, sizeof(rsp), 0, (struct sockaddr *)&from,
		   sizeof(from)) < 0)
		fmd_warn(ctx, "event notification ack: %s", strerror(errno));

	if (len < (ssize_t)sizeof(buf))
		return;

	pl = (struct cxlmi_cmd_event_notification_req *)msg->payload;
	memcpy(&events, &pl->event, sizeof(events));
	events = le16toh(events);

	fmd_dbg(ctx, "event notification from EID %u: logs 0x%04x",
		from.smctp_addr.s_addr, events);

	if (events & CCI_EVENT_DC_LOG)
		fmd_dcd_poll_events(ctx);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Drain the DC event log when the poll interval has elapsed. Notifications
 * only shorten the wait; the timer is what works on every transport.
 */
static void poll_dc_events_if_due(struct fmd_ctx *ctx, int64_t now)
{
	int64_t interval = (int64_t)ctx->config.event_poll_sec * 1000;

	if (!interval || fmd_is_mock(ctx))
		return;

	if (!ctx->next_dc_poll_ms) {
		ctx->next_dc_poll_ms = now + interval;
		return;
	}

	if (now < ctx->next_dc_poll_ms)
		return;

	fmd_dcd_poll_events(ctx);
	ctx->next_dc_poll_ms = now_ms() + interval;
}

int fmd_events_poll(struct fmd_ctx *ctx, int timeout_ms)
{
	struct pollfd fds[2];
	int64_t now = now_ms();
	int n = 0, rc;

	poll_dc_events_if_due(ctx, now);
	if (ctx->next_dc_poll_ms && ctx->next_dc_poll_ms - now < timeout_ms)
		timeout_ms = (int)(ctx->next_dc_poll_ms - now);

	for (int i = 0; i < 2; i++) {
		if (ctx->notify_sd[i] < 0)
			continue;
		fds[n].fd = ctx->notify_sd[i];
		fds[n].events = POLLIN;
		n++;
	}

	if (n == 0) {
		usleep(timeout_ms * 1000);
		return 0;
	}

	rc = poll(fds, n, timeout_ms);
	if (rc <= 0)
		return rc < 0 && errno != EINTR ? -errno : 0;

	for (int i = 0; i < n; i++) {
		if (fds[i].revents & POLLIN)
			handle_notification(ctx, fds[i].fd);
	}

	return 0;
}
