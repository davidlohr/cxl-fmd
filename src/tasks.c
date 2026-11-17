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
 * operation is over. While it runs, a struct fmd_binding_op says what the
 * vPPB is becoming, since a vPPB being bound or unbound reads 01h; the task
 * frees it when the operation is over, however that is learnt, and the
 * Connection is then what the vPPB is. That the operation is over is
 * judged on the vPPB read again after the switch says it is, and only then
 * (task_complete()).
 *
 * The task is made before the command is sent (fmd_task_alloc()), so that
 * nothing the switch has started is left without one. A task whose switch
 * stops answering, while its vPPB reads 01h, is Interrupted and keeps its
 * op, until a read of the VCSs sees the vPPB settled (task_lost(),
 * fmd_tasks_reconcile()).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxl-fmd.h"

/* finished tasks kept for a client to collect */
#define FMD_TASKS_KEPT 64
/* answers in a row that tell nothing before a task is lost (cxl-fmd's) */
#define FMD_TASK_TRIES 5

/* What the task's bind makes: its vPPB, port and LD, on its switch */
static void task_key(const struct fmd_task *task, struct fmd_bind_key *key)
{
	memset(key, 0, sizeof(*key));
	snprintf(key->switch_id, sizeof(key->switch_id), "%s", task->switch_id);
	key->vcs_id = task->vcs_id;
	key->vppb_id = task->vppb_id;
	key->port_id = task->port_id;
	key->ld_id = task->ld_id;
}

void *(*fmd_task_calloc)(size_t nmemb, size_t size) = calloc;

struct fmd_task *fmd_task_alloc(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				struct fmd_switch *sw,
				const struct fmd_bind_key *key)
{
	struct fmd_task *task = fmd_task_calloc(1, sizeof(*task));

	if (!task) {
		fmd_err(ctx, "%s: no task for the %s of vPPB %d in VCS %d: "
			"nothing is sent", sw->base.id,
			kind == FMD_TASK_BIND ? "bind" : "unbind",
			key->vppb_id, key->vcs_id);
		return NULL;
	}
	task->kind = kind;
	task->state = FMD_TASK_RUNNING;
	task->vcs_id = key->vcs_id;
	task->vppb_id = key->vppb_id;
	task->port_id = key->port_id;
	task->ld_id = key->ld_id;
	snprintf(task->switch_id, sizeof(task->switch_id), "%s", sw->base.id);
	fmd_conn_id_format(key, task->connection_id, sizeof(task->connection_id));
	return task;
}

void fmd_task_discard(struct fmd_task *task)
{
	free(task);
}

struct fmd_task *fmd_task_activate(struct fmd_ctx *ctx, struct fmd_task *task,
				   uint8_t unbind_option)
{
	enum fmd_bind_op_kind op_kind = task->kind == FMD_TASK_BIND ?
					FMD_BIND_OP_BIND : FMD_BIND_OP_UNBIND;
	struct fmd_binding_op *op;
	struct fmd_bind_key key;

	task_key(task, &key);
	op = fmd_binding_op_find(ctx, key.switch_id, key.vcs_id, key.vppb_id);
	if (op && op->kind != op_kind) {
		fmd_binding_op_remove(ctx, op);
		op = NULL;
	}
	if (!op)
		op = fmd_binding_op_add(ctx, &key, op_kind, unbind_option, NULL);
	if (!op) {
		/* nothing would say what the vPPB is becoming: the vPPB says */
		fmd_err(ctx, "%s: no task for the %s of vPPB %d in VCS %d",
			task->switch_id, task->kind == FMD_TASK_BIND ? "bind" : "unbind",
			task->vppb_id, task->vcs_id);
		free(task);
		return NULL;
	}

	task->id = ++ctx->task_next_id;
	task->start = time(NULL);
	snprintf(op->task_id, sizeof(op->task_id), "%d", task->id);
	op->lost = false;

	task->next = ctx->tasks;
	ctx->tasks = task;

	fmd_info(ctx, "Task %d: %s of vPPB %d in VCS %d of %s started (connection %s)",
		 task->id, task->kind == FMD_TASK_BIND ? "bind" : "unbind",
		 task->vppb_id, task->vcs_id, task->switch_id, task->connection_id);
	return task;
}

struct fmd_task *fmd_task_start(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				struct fmd_switch *sw,
				const struct fmd_bind_key *key,
				uint8_t unbind_option)
{
	struct fmd_task *task = fmd_task_alloc(ctx, kind, sw, key);
	struct fmd_binding_op *op;

	if (task)
		return fmd_task_activate(ctx, task, unbind_option);
	/* nothing would follow it, or free the op: the vPPB says */
	op = fmd_binding_op_find(ctx, key->switch_id, key->vcs_id, key->vppb_id);
	if (op)
		fmd_binding_op_remove(ctx, op);
	return NULL;
}

/* The task is over: the op it followed goes, and the view reads the vPPB */
static void task_drop_op(struct fmd_ctx *ctx, struct fmd_task *task)
{
	struct fmd_binding_op *op;
	char id[16];

	snprintf(id, sizeof(id), "%d", task->id);
	for (op = ctx->binding_ops; op; op = op->next) {
		if (!strcmp(op->task_id, id)) {
			fmd_binding_op_remove(ctx, op);
			return;
		}
	}
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
	task_drop_op(ctx, task);

	if (ok)
		fmd_info(ctx, "Task %d: %s", task->id, message);
	else
		fmd_err(ctx, "Task %d: %s", task->id, message);
}


/*
 * What the task's vPPB is now: the switch's VCSs are read again. @observed
 * is false when the task's VCS could not be read, so that what is known of
 * it is from before; a VCS that is gone was read as gone only if every VCS
 * was read (fmd_vcs_refresh()).
 */
static struct fmd_vppb *task_observe(struct fmd_ctx *ctx, struct fmd_task *task,
				     struct fmd_switch *sw, bool *observed)
{
	int rc = fmd_vcs_refresh(ctx, sw);
	struct fmd_vcs *vcs = fmd_switch_find_vcs(sw, task->vcs_id);

	*observed = vcs ? !vcs->stale : rc == 0;
	return vcs ? fmd_vcs_find_vppb(vcs, task->vppb_id) : NULL;
}

static void task_lost(struct fmd_ctx *ctx, struct fmd_task *task,
		      struct fmd_switch *sw, const char *message);

/*
 * The switch said the operation is over, and the vPPB, read again, does not
 * show it: it still reads 01h, or could not be read. The task goes on, and
 * the switch is asked again; after FMD_TASK_TRIES such answers in a row it
 * is lost.
 */
static void task_unseen(struct fmd_ctx *ctx, struct fmd_task *task,
			struct fmd_switch *sw, bool busy)
{
	fmd_warn(ctx, "Task %d: the switch says the %s is over, and vPPB %d of "
		 "VCS %d %s", task->id,
		 task->kind == FMD_TASK_BIND ? "bind" : "unbind", task->vppb_id,
		 task->vcs_id, busy ? "still reads 01h (bind or unbind in progress)" :
		 "could not be read");
	if (++task->unobserved >= FMD_TASK_TRIES)
		task_lost(ctx, task, sw, busy ?
			  "The switch says the operation is over, and the vPPB "
			  "still reads in progress" :
			  "The switch says the operation is over, and the vPPB "
			  "could not be read");
}

/*
 * The outcome of @task, its vPPB read and not at 01h: @vppb, NULL if the
 * switch has no such vPPB. @rc and @logged_failed as task_complete().
 * @read_only: the switch could not be asked how it went, and the vPPB is
 * all there is to go by.
 */
static void task_judge(struct fmd_ctx *ctx, struct fmd_task *task,
		       const struct fmd_vppb *vppb, uint16_t rc,
		       bool logged_failed, bool read_only)
{
	bool failed = rc != CXLMI_RET_SUCCESS || logged_failed;
	const char *how = read_only ? " (read from the vPPB: the switch was "
				      "not asked how it went)" : "";
	char msg[sizeof(task->message)];

	task->percent = 100;
	/* how a failure is known: the monitor answers as the request would */
	task->rc = rc;
	task->outcome = rc != CXLMI_RET_SUCCESS ? FMD_TASK_RC : FMD_TASK_OBSERVED;

	if (task->kind == FMD_TASK_BIND) {
		/*
		 * Bound to what was asked: the port, and the LD for 03h. Bound
		 * Port ID is "Valid only when Binding Status is 02h or 03h",
		 * and Bound LD ID "Valid only when vPPB[Start vPPB] Binding
		 * Status is 03h" (Table 7-34).
		 */
		bool bound = vppb && vppb->bound_port_id == task->port_id &&
			((task->ld_id == FMD_LD_NONE &&
			  vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT) ||
			 (task->ld_id != FMD_LD_NONE &&
			  vppb->binding_status == FMD_VPPB_BOUND_LD &&
			  vppb->bound_ld_id == task->ld_id));

		if (!failed && bound) {
			struct fmd_bind_key key;

			task_key(task, &key);
			fmd_binding_mark_add(ctx, &key);
			snprintf(msg, sizeof(msg), "The vPPB is bound%s", how);
			task_finish(ctx, task, true, msg);
			return;
		}

		/* the host was never given the device: there is no connection */
		if (rc != CXLMI_RET_SUCCESS)
			snprintf(msg, sizeof(msg), "Bind vPPB failed: %s",
				 cxlmi_cmd_retcode_tostr(rc));
		else if (logged_failed)
			snprintf(msg, sizeof(msg), "Bind vPPB failed: the switch "
				 "logged it as failed");
		else if (read_only)
			snprintf(msg, sizeof(msg), "The vPPB is not bound to what "
				 "was asked%s", how);
		else
			snprintf(msg, sizeof(msg), "Bind vPPB completed and the "
				 "vPPB is not bound to what was asked");
		task_finish(ctx, task, false, msg);
		return;
	}

	/* unbind */
	if (!failed && (!vppb || vppb->binding_status == FMD_VPPB_UNBOUND)) {
		snprintf(msg, sizeof(msg), "The vPPB is unbound%s", how);
		task_finish(ctx, task, true, msg);
		return;
	}

	/* the host still has the device, and the connection stands */
	if (rc != CXLMI_RET_SUCCESS)
		snprintf(msg, sizeof(msg), "Unbind vPPB failed: %s",
			 cxlmi_cmd_retcode_tostr(rc));
	else if (logged_failed)
		snprintf(msg, sizeof(msg), "Unbind vPPB failed: the switch "
			 "logged it as failed");
	else if (read_only)
		snprintf(msg, sizeof(msg), "The vPPB is still bound%s", how);
	else
		snprintf(msg, sizeof(msg), "Unbind vPPB completed and the vPPB "
			 "is still bound");
	task_finish(ctx, task, false, msg);
}

/*
 * The outcome of one task whose operation the switch says is over, with
 * return code @rc. @logged_failed: the switch recorded it as failed, with
 * no return code known.
 *
 * Judged on the vPPB as it is read now, and only then (cxl-fmd's choice):
 * a vPPB that still reads 01h is still being bound or unbound, and one that
 * could not be read says nothing either way, so the task goes on. Success
 * is not taken without a vPPB that shows it; a failure the switch reported
 * is taken without one, unless the vPPB reads 01h.
 */
static void task_complete(struct fmd_ctx *ctx, struct fmd_task *task,
			  struct fmd_switch *sw, uint16_t rc, bool logged_failed)
{
	bool failed = rc != CXLMI_RET_SUCCESS || logged_failed;
	struct fmd_vppb *vppb;
	bool observed, busy;

	/* what the switch has now */
	vppb = task_observe(ctx, task, sw, &observed);
	busy = observed && vppb && vppb->binding_status == FMD_VPPB_BUSY;
	if (busy || (!observed && !failed)) {
		task_unseen(ctx, task, sw, busy);
		return;
	}
	task_judge(ctx, task, vppb, rc, logged_failed, false);
}

/*
 * Whether a Binding Change record whose vPPB Binding Status, Port ID and LD
 * ID are @status, @port_id and @ld_id can be the end of @task's operation.
 * Of a Binding Change, 33h "contains the updated binding state of a vPPB
 * following the binding change. Successful bind and unbind operations
 * generate events to the Informational Event Log. Failed bind and unbind
 * operations generate events to the Warning Event Log" (Table 7-80), and
 * 34h and 35h likewise. Nothing in a record says which command it is of
 * (an earlier unbind of the same vPPB, or another FM's bind), so the fields
 * are used to turn a record away, never to decide an outcome (cxl-fmd's
 * choice): a successful one fits only if it shows what the task asked
 * for, a failed one only if it does not, and neither if it shows 01h.
 */
static bool record_fits(const struct fmd_task *task, bool failed,
			uint8_t status, uint8_t port_id, uint8_t ld_id)
{
	bool asked;

	if (status == FMD_VPPB_BUSY)
		return false;
	if (task->kind == FMD_TASK_BIND)
		asked = port_id == task->port_id &&
			((task->ld_id == FMD_LD_NONE &&
			  status == FMD_VPPB_BOUND_PHYSICAL_PORT) ||
			 (task->ld_id != FMD_LD_NONE &&
			  status == FMD_VPPB_BOUND_LD && ld_id == task->ld_id));
	else
		asked = status == FMD_VPPB_UNBOUND;
	return failed ? !asked : asked;
}

/*
 * The switch has recorded a Binding Change of a vPPB. If a task is following
 * a bind or unbind of it, and the record fits it (record_fits()), this is
 * its end, and the switch's own word on how it went: "Failed bind and
 * unbind operations generate events to the Warning Event Log" (Table 7-80).
 * The record has no return code: that is in Background Operation Status,
 * if the switch has run nothing since. The task is still judged on the vPPB
 * read again (task_complete()).
 */
bool fmd_task_binding_changed(struct fmd_ctx *ctx, struct fmd_switch *sw,
			      uint8_t vcs_id, uint8_t vppb_id, bool failed,
			      uint8_t status, uint8_t port_id, uint8_t ld_id)
{
	struct fmd_task *task;
	uint16_t rc = CXLMI_RET_SUCCESS;

	for (task = ctx->tasks; task; task = task->next) {
		if (task->state != FMD_TASK_RUNNING ||
		    task->vcs_id != vcs_id || task->vppb_id != vppb_id ||
		    strcmp(task->switch_id, sw->base.id) != 0)
			continue;

		if (!record_fits(task, failed, status, port_id, ld_id)) {
			fmd_info(ctx, "Task %d: a Binding Change of vPPB %d of "
				 "VCS %d (%02xh, port %d, LD %d) that is not "
				 "the end of its %s", task->id, vppb_id, vcs_id,
				 status, port_id, ld_id,
				 task->kind == FMD_TASK_BIND ? "bind" : "unbind");
			return false;
		}
		if (failed) {
			int percent;

			if (fmd_switch_bg_status(ctx, sw, task->kind == FMD_TASK_BIND ?
						 0x5201 : 0x5202, &percent, &rc) != 0)
				rc = CXLMI_RET_SUCCESS;
		}
		task_complete(ctx, task, sw, rc, failed);
		return true;
	}
	return false;
}

/*
 * A task whose switch does not answer how its operation is going. The vPPB
 * is read again: one that no longer reads 01h says how it went (rc is not
 * known). Otherwise the task is Interrupted: Task_v1 has "the task has been
 * interrupted but is expected to restart and is therefore not complete".
 * Its op is kept, so its Connection is still Starting or Updating and a
 * DELETE of it is still refused, and fmd_tasks_reconcile() ends it on the
 * first read of the VCSs, by the events, the --rescan timer or a request,
 * that sees the vPPB settled (cxl-fmd's choice). The switch is not asked
 * about it again: Return Code is "Only valid when Percentage Complete =
 * 100" (Table 8-217), and the switch may have run another command since.
 * @sw NULL: the switch is gone, and so is what the task followed.
 */
static void task_lost(struct fmd_ctx *ctx, struct fmd_task *task,
		      struct fmd_switch *sw, const char *message)
{
	struct fmd_binding_op *op;
	struct fmd_vppb *vppb;
	bool observed;
	char id[16];

	if (!sw) {
		/* whether it bound or unbound before it went is not known */
		task->outcome = FMD_TASK_UNKNOWN;
		task_finish(ctx, task, false, message);
		return;
	}

	vppb = task_observe(ctx, task, sw, &observed);
	if (observed && (!vppb || vppb->binding_status != FMD_VPPB_BUSY)) {
		task_judge(ctx, task, vppb, CXLMI_RET_SUCCESS, false, true);
		return;
	}

	task->state = FMD_TASK_INTERRUPTED;
	snprintf(task->message, sizeof(task->message), "%s; the vPPB is read "
		 "again until it settles", message);
	snprintf(id, sizeof(id), "%d", task->id);
	for (op = ctx->binding_ops; op; op = op->next) {
		if (!strcmp(op->task_id, id))
			op->lost = true;
	}
	fmd_err(ctx, "Task %d: Interrupted: %s (the vPPB %s)", task->id,
		task->message, observed ? "reads 01h" : "could not be read");
}

void fmd_tasks_reconcile(struct fmd_ctx *ctx, struct fmd_switch *sw,
			 int refresh_rc)
{
	struct fmd_task *task;

	for (task = ctx->tasks; task; task = task->next) {
		struct fmd_vcs *vcs;
		struct fmd_vppb *vppb;

		if (task->state != FMD_TASK_INTERRUPTED ||
		    strcmp(task->switch_id, sw->base.id) != 0)
			continue;
		vcs = fmd_switch_find_vcs(sw, task->vcs_id);
		if (vcs ? vcs->stale : refresh_rc != 0)
			continue;
		vppb = vcs ? fmd_vcs_find_vppb(vcs, task->vppb_id) : NULL;
		if (vppb && vppb->binding_status == FMD_VPPB_BUSY)
			continue;
		task_judge(ctx, task, vppb, CXLMI_RET_SUCCESS, false, true);
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

		if ((task->state == FMD_TASK_COMPLETED ||
		     task->state == FMD_TASK_EXCEPTION) &&
		    ++finished > FMD_TASKS_KEPT) {
			*pp = task->next;
			free(task);
			continue;
		}
		pp = &task->next;
	}
}

/*
 * Ask each switch with an operation under way how it is doing, and end the
 * Interrupted tasks of a switch that is gone. Called from
 * the event loop, which has the fabric to itself while it does: commands
 * are sent, and the VCS tables and connections change.
 */
void fmd_tasks_poll(struct fmd_ctx *ctx)
{
	struct fmd_task *task;
	bool open = false;

	pthread_rwlock_rdlock(&ctx->fabric_lock);
	for (task = ctx->tasks; task && !open; task = task->next)
		open = task->state == FMD_TASK_RUNNING ||
		       task->state == FMD_TASK_INTERRUPTED;
	pthread_rwlock_unlock(&ctx->fabric_lock);
	if (!open)
		return;

	pthread_rwlock_wrlock(&ctx->fabric_lock);
	for (task = ctx->tasks; task; task = task->next) {
		struct fmd_switch *sw;
		uint16_t rc = 0;
		int percent = 0, status;

		if (task->state != FMD_TASK_RUNNING &&
		    task->state != FMD_TASK_INTERRUPTED)
			continue;

		sw = fmd_topology_find_switch(&ctx->topology, task->switch_id);
		if (!sw) {
			task_lost(ctx, task, NULL, "The switch is gone");
			continue;
		}
		/* one that is Interrupted waits for a read of the VCSs */
		if (task->state == FMD_TASK_INTERRUPTED)
			continue;

		status = fmd_switch_bg_status(ctx, sw,
				task->kind == FMD_TASK_BIND ? 0x5201 : 0x5202,
				&percent, &rc);
		if (status < 0) {
			/* a switch that cannot be asked, several times over */
			if (++task->failed_polls >= FMD_TASK_TRIES)
				task_lost(ctx, task, sw, "The switch does not "
					  "answer Background Operation Status");
			continue;
		}
		task->failed_polls = 0;
		if (status == 0) {
			task_complete(ctx, task, sw, rc, false);
			continue;
		}
		/* in progress: the switch is working on it */
		task->percent = percent;
		task->unobserved = 0;
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
