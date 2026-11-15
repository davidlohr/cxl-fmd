// SPDX-License-Identifier: BSD-3-Clause
/*
 * Tasks: operations a switch carries out in the background
 *
 * "All binding operations are executed as background commands" (CXL r4.0
 * Section 7.6.7.2.2), and unbinding likewise. A switch answers Bind vPPB or
 * Unbind vPPB with Background Command Started, and the outcome is in
 * Background Operation Status (0002h): whether the operation is still in
 * progress, how far it is, and once it is complete its return code.
 *
 * A request that starts one does not wait for it. It is answered with 202
 * Accepted and a task, and the event loop asks the switch until the
 * operation is over. Only then is the connection made, or removed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxl-fmd.h"

/* finished tasks kept for a client to collect */
#define FMD_TASKS_KEPT 64

struct fmd_task *fmd_task_start(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				struct fmd_switch *sw, uint8_t vcs_id,
				uint8_t vppb_id, const char *connection_id)
{
	struct fmd_task *task = calloc(1, sizeof(*task));

	if (!task)
		return NULL;

	task->id = ++ctx->task_next_id;
	task->kind = kind;
	task->state = FMD_TASK_RUNNING;
	task->vcs_id = vcs_id;
	task->vppb_id = vppb_id;
	task->start = time(NULL);
	snprintf(task->switch_id, sizeof(task->switch_id), "%s", sw->base.id);
	snprintf(task->connection_id, sizeof(task->connection_id), "%s",
		 connection_id);

	task->next = ctx->tasks;
	ctx->tasks = task;

	fmd_info(ctx, "Task %d: %s of vPPB %d in VCS %d of %s started (connection %s)",
		 task->id, kind == FMD_TASK_BIND ? "bind" : "unbind", vppb_id,
		 vcs_id, sw->base.id, connection_id);
	return task;
}

struct fmd_task *fmd_task_find(struct fmd_ctx *ctx, int id)
{
	struct fmd_task *task;

	for (task = ctx->tasks; task; task = task->next) {
		if (task->id == id)
			return task;
	}
	return NULL;
}

bool fmd_tasks_running(struct fmd_ctx *ctx)
{
	struct fmd_task *task;
	bool running = false;

	pthread_rwlock_rdlock(&ctx->fabric_lock);
	for (task = ctx->tasks; task && !running; task = task->next)
		running = task->state == FMD_TASK_RUNNING;
	pthread_rwlock_unlock(&ctx->fabric_lock);

	return running;
}

static void task_finish(struct fmd_ctx *ctx, struct fmd_task *task, bool ok,
			const char *message)
{
	task->state = ok ? FMD_TASK_COMPLETED : FMD_TASK_EXCEPTION;
	task->percent = ok ? 100 : task->percent;
	task->end = time(NULL);
	snprintf(task->message, sizeof(task->message), "%s", message);

	if (ok)
		fmd_info(ctx, "Task %d: %s", task->id, message);
	else
		fmd_err(ctx, "Task %d: %s", task->id, message);
}

/*
 * The outcome of one task whose operation is over, with return code @rc.
 * @logged_failed: the switch recorded it as failed, with no return code
 * known.
 */
static void task_complete(struct fmd_ctx *ctx, struct fmd_task *task,
			  struct fmd_switch *sw, uint16_t rc, bool logged_failed)
{
	struct fmd_connection *conn = fmd_connection_find(ctx, task->connection_id);
	struct fmd_vcs *vcs;
	struct fmd_vppb *vppb;
	char msg[128];

	/* what the switch has now */
	fmd_vcs_refresh(ctx, sw);
	vcs = fmd_switch_find_vcs(sw, task->vcs_id);
	vppb = vcs ? fmd_vcs_find_vppb(vcs, task->vppb_id) : NULL;

	if (task->kind == FMD_TASK_BIND) {
		bool bound = vppb &&
			(vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
			 vppb->binding_status == FMD_VPPB_BOUND_LD);

		if (rc == CXLMI_RET_SUCCESS && bound && !logged_failed) {
			if (conn) {
				conn->bound = true;
				conn->pending = FMD_CONN_SETTLED;
			}
			task_finish(ctx, task, true, "The vPPB is bound");
			return;
		}

		/* the host was never given the device: there is no connection */
		if (conn)
			fmd_connection_remove(ctx, conn);
		if (rc != CXLMI_RET_SUCCESS)
			snprintf(msg, sizeof(msg), "Bind vPPB failed: %s",
				 cxlmi_cmd_retcode_tostr(rc));
		else if (logged_failed)
			snprintf(msg, sizeof(msg), "Bind vPPB failed: the switch "
				 "logged it as failed");
		else
			snprintf(msg, sizeof(msg), "Bind vPPB completed and the "
				 "vPPB is not bound");
		task_finish(ctx, task, false, msg);
		return;
	}

	/* unbind */
	if (rc == CXLMI_RET_SUCCESS && !logged_failed &&
	    (!vppb || vppb->binding_status == FMD_VPPB_UNBOUND)) {
		if (conn)
			fmd_connection_remove(ctx, conn);
		task_finish(ctx, task, true, "The vPPB is unbound");
		return;
	}

	/* the host still has the device, and the connection stands */
	if (conn)
		conn->pending = FMD_CONN_SETTLED;
	if (rc != CXLMI_RET_SUCCESS)
		snprintf(msg, sizeof(msg), "Unbind vPPB failed: %s",
			 cxlmi_cmd_retcode_tostr(rc));
	else if (logged_failed)
		snprintf(msg, sizeof(msg), "Unbind vPPB failed: the switch "
			 "logged it as failed");
	else
		snprintf(msg, sizeof(msg), "Unbind vPPB completed and the vPPB "
			 "is still bound");
	task_finish(ctx, task, false, msg);
}

/*
 * The switch has recorded a Binding Change of a vPPB. If a task is following
 * a bind or unbind of it, this is its end, and the switch's own word on how
 * it went: "Failed bind and unbind operations generate events to the
 * Warning Event Log" (Table 7-80). The record has no return code: that is
 * in Background Operation Status, if the switch has run nothing since.
 */
void fmd_task_binding_changed(struct fmd_ctx *ctx, struct fmd_switch *sw,
			      uint8_t vcs_id, uint8_t vppb_id, bool failed)
{
	struct fmd_task *task;
	uint16_t rc = CXLMI_RET_SUCCESS;

	for (task = ctx->tasks; task; task = task->next) {
		if (task->state != FMD_TASK_RUNNING ||
		    task->vcs_id != vcs_id || task->vppb_id != vppb_id ||
		    strcmp(task->switch_id, sw->base.id) != 0)
			continue;

		task->percent = 100;
		if (failed) {
			int percent;

			if (fmd_switch_bg_status(ctx, sw, task->kind == FMD_TASK_BIND ?
						 0x5201 : 0x5202, &percent, &rc) != 0)
				rc = CXLMI_RET_SUCCESS;
		}
		task_complete(ctx, task, sw, rc, failed);
		return;
	}
}

/* forget the oldest finished tasks beyond the number kept */
static void tasks_prune(struct fmd_ctx *ctx)
{
	struct fmd_task **pp = &ctx->tasks;
	int finished = 0;

	/* the list is newest first */
	while (*pp) {
		struct fmd_task *task = *pp;

		if (task->state != FMD_TASK_RUNNING && ++finished > FMD_TASKS_KEPT) {
			*pp = task->next;
			free(task);
			continue;
		}
		pp = &task->next;
	}
}

/*
 * Ask each switch with an operation under way how it is doing. Called from
 * the event loop, which has the fabric to itself while it does: commands
 * are sent, and the VCS tables and connections change.
 */
void fmd_tasks_poll(struct fmd_ctx *ctx)
{
	struct fmd_task *task;

	if (!fmd_tasks_running(ctx))
		return;

	pthread_rwlock_wrlock(&ctx->fabric_lock);
	for (task = ctx->tasks; task; task = task->next) {
		struct fmd_switch *sw;
		uint16_t rc = 0;
		int percent = 0, status;

		if (task->state != FMD_TASK_RUNNING)
			continue;

		sw = fmd_topology_find_switch(&ctx->topology, task->switch_id);
		if (!sw) {
			task_finish(ctx, task, false, "The switch is gone");
			continue;
		}

		status = fmd_switch_bg_status(ctx, sw,
				task->kind == FMD_TASK_BIND ? 0x5201 : 0x5202,
				&percent, &rc);
		if (status < 0) {
			/* a switch that cannot be asked, several times over */
			if (++task->failed_polls >= 5)
				task_finish(ctx, task, false, "The switch does not "
					    "answer Background Operation Status");
			continue;
		}
		task->failed_polls = 0;
		task->percent = percent;
		if (status == 0)
			task_complete(ctx, task, sw, rc, false);
	}
	tasks_prune(ctx);
	pthread_rwlock_unlock(&ctx->fabric_lock);
}

void fmd_tasks_free(struct fmd_ctx *ctx)
{
	while (ctx->tasks) {
		struct fmd_task *task = ctx->tasks;

		ctx->tasks = task->next;
		free(task);
	}
}
