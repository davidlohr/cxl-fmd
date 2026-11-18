// SPDX-License-Identifier: LGPL-2.1
/*
 * Connections derived from the switch.
 *
 * A switch-attached Connection is a vPPB binding, as Get Virtual CXL
 * Switch Info (5200h) last read it, together with the bind or unbind
 * cxl-fmd has under way on that vPPB: nothing is stored that could
 * disagree with the switch. See struct fmd_conn_view in cxl-fmd.h for
 * what the caller holds while it uses what is computed here.
 *
 * A direct-attached Connection has no vPPB: there is no VCS, nor any
 * bind, on a link from a host's root port to a device. The Virtual Switch
 * Command Set, Bind vPPB among it, "is supported only by the CXL switch"
 * (Section 7.6.7.2). That the link is the Connection is cxl-fmd's choice;
 * see for_each_direct().
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl-fmd.h"

#define VCS_STATE_ENABLED 0x01		/* VCS State, Table 7-34 */

struct fmd_head *fmd_ld_head(const struct fmd_ld *ld)
{
	struct fmd_component *mhd = ld ? ld->base.parent : NULL;
	struct fmd_component *child;

	if (!mhd || mhd->type != FMD_COMPONENT_MHD || ld->binding_point_id == 0xFF)
		return NULL;
	fmd_for_each_child_type(mhd, child, FMD_COMPONENT_HEAD) {
		if (fmd_to_head(child)->head_id == ld->binding_point_id)
			return fmd_to_head(child);
	}
	return NULL;
}

struct fmd_ld *fmd_head_whole_ld(const struct fmd_head *head)
{
	struct fmd_component *mhd = head ? head->base.parent : NULL;
	struct fmd_component *child;
	struct fmd_ld *found = NULL;

	if (!mhd || head->presentation != FMD_HEAD_SLD)
		return NULL;
	fmd_for_each_child_type(mhd, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->binding_point_id != head->head_id)
			continue;
		if (found)
			return NULL;
		found = fmd_to_ld(child);
	}
	return found;
}

/* The SLD, MLD or Multi-Headed device on switch port @port, if any */
static struct fmd_component *port_device(const struct fmd_port *port)
{
	struct fmd_component *child;

	fmd_for_each_child(&port->base, child) {
		if (child->type == FMD_COMPONENT_SLD || child->type == FMD_COMPONENT_MLD)
			return child;
	}
	if (port->head && port->head->base.parent)
		return port->head->base.parent;
	return NULL;
}

struct fmd_component *fmd_bind_target(const struct fmd_port *port, uint16_t ld_id)
{
	struct fmd_component *dev, *child;

	if (!port)
		return NULL;

	dev = port->base.first_child;
	if (dev && dev->type == FMD_COMPONENT_SLD)
		return ld_id == FMD_LD_NONE ? dev : NULL;
	if (dev && dev->type == FMD_COMPONENT_MLD) {
		if (ld_id == FMD_LD_NONE)
			return NULL;
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
			if (fmd_to_ld(child)->ld_id == ld_id)
				return child;
		}
		return NULL;
	}
	if (port->head) {
		struct fmd_ld *ld;

		if (ld_id != FMD_LD_NONE)
			return port->head->presentation == FMD_HEAD_MLD ?
			       &port->head->base : NULL;
		ld = fmd_head_whole_ld(port->head);
		return ld ? &ld->base : NULL;
	}
	return NULL;
}

static const struct fmd_ld *find_ld(const struct fmd_component *dev, int ld_id)
{
	struct fmd_component *child;

	if (ld_id < 0)
		return NULL;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->ld_id == ld_id)
			return fmd_to_ld(child);
	}
	return NULL;
}

const struct fmd_ld *fmd_chunk_ld(const struct fmd_component *dev,
				  const struct fmd_mem_chunk *chunk)
{
	if (!dev || !chunk ||
	    (dev->type != FMD_COMPONENT_MLD && dev->type != FMD_COMPONENT_MHD))
		return NULL;
	return find_ld(dev, chunk->backing_ld);
}

bool fmd_bind_key_eq(const struct fmd_bind_key *a, const struct fmd_bind_key *b)
{
	return !strcmp(a->switch_id, b->switch_id) && a->vcs_id == b->vcs_id &&
	       a->vppb_id == b->vppb_id && a->port_id == b->port_id &&
	       a->ld_id == b->ld_id;
}

int fmd_conn_id_format(const struct fmd_bind_key *key, char *buf, size_t size)
{
	int n;

	if (key->ld_id == FMD_LD_NONE)
		n = snprintf(buf, size, "%s_vcs%u_vppb%u_p%u", key->switch_id,
			     key->vcs_id, key->vppb_id, key->port_id);
	else
		n = snprintf(buf, size, "%s_vcs%u_vppb%u_p%u_ld%u", key->switch_id,
			     key->vcs_id, key->vppb_id, key->port_id, key->ld_id);
	return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

/*
 * Take "{tag}{N}" off the end of @s, the last @tag in it, N decimal and at
 * most @max. Returns N, or -1 if @s does not end that way.
 */
static long strip_suffix(char *s, const char *tag, long max)
{
	char *p, *found = NULL, *end;
	long v;

	for (p = s; (p = strstr(p, tag)); p++)
		found = p;
	if (!found || found == s)
		return -1;
	p = found + strlen(tag);
	if (*p < '0' || *p > '9')
		return -1;
	v = strtol(p, &end, 10);
	if (*end != '\0' || v > max)
		return -1;
	*found = '\0';
	return v;
}

int fmd_conn_id_parse(const char *id, struct fmd_bind_key *key)
{
	char buf[FMD_CONN_ID_LEN], again[FMD_CONN_ID_LEN];
	struct fmd_bind_key k = { 0 };
	long v;

	if (!id || !key || strlen(id) >= sizeof(buf))
		return -EINVAL;
	strcpy(buf, id);

	/* from the right: "_ld{L}" if it is there, then "_p", "_vppb", "_vcs" */
	k.ld_id = FMD_LD_NONE;
	{
		char probe[FMD_CONN_ID_LEN];

		strcpy(probe, buf);
		v = strip_suffix(probe, "_ld", 0xFFFE);
		if (v >= 0 && strip_suffix(probe, "_p", 0xFF) >= 0) {
			k.ld_id = (uint16_t)v;
			strip_suffix(buf, "_ld", 0xFFFE);
		}
	}
	if ((v = strip_suffix(buf, "_p", 0xFF)) < 0)
		return -EINVAL;
	k.port_id = (uint8_t)v;
	if ((v = strip_suffix(buf, "_vppb", 0xFF)) < 0)
		return -EINVAL;
	k.vppb_id = (uint8_t)v;
	if ((v = strip_suffix(buf, "_vcs", 0xFF)) < 0)
		return -EINVAL;
	k.vcs_id = (uint8_t)v;
	if (!buf[0] || strlen(buf) >= sizeof(k.switch_id))
		return -EINVAL;
	strcpy(k.switch_id, buf);

	/* one spelling only: "_p05" or "_p+5" is not this Connection's ID */
	if (fmd_conn_id_format(&k, again, sizeof(again)) || strcmp(again, id))
		return -EINVAL;
	*key = k;
	return 0;
}

struct fmd_binding_op *fmd_binding_op_find(const struct fmd_ctx *ctx,
					   const char *switch_id,
					   uint8_t vcs_id, uint8_t vppb_id)
{
	struct fmd_binding_op *op;

	for (op = ctx->binding_ops; op; op = op->next) {
		if (!strcmp(op->key.switch_id, switch_id) &&
		    op->key.vcs_id == vcs_id && op->key.vppb_id == vppb_id)
			return op;
	}
	return NULL;
}

struct fmd_binding_op *fmd_binding_op_find_target(const struct fmd_ctx *ctx,
						  const char *switch_id,
						  uint8_t port_id, uint16_t ld_id)
{
	struct fmd_binding_op *op;

	for (op = ctx->binding_ops; op; op = op->next) {
		if (!strcmp(op->key.switch_id, switch_id) &&
		    op->key.port_id == port_id && op->key.ld_id == ld_id)
			return op;
	}
	return NULL;
}

void fmd_vppb_seen(struct fmd_switch *sw, uint8_t vcs_id, uint8_t vppb_id,
		   struct fmd_vppb_seen *seen)
{
	struct fmd_vcs *vcs = fmd_switch_find_vcs(sw, vcs_id);
	const struct fmd_vppb *vppb = vcs ? fmd_vcs_find_vppb(vcs, vppb_id) : NULL;

	memset(seen, 0, sizeof(*seen));
	seen->ld_id = FMD_LD_NONE;
	if (!vppb || vcs->stale)
		return;
	seen->read = true;
	seen->status = vppb->binding_status;
	if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
	    vppb->binding_status == FMD_VPPB_BOUND_LD)
		seen->port_id = vppb->bound_port_id;
	if (vppb->binding_status == FMD_VPPB_BOUND_LD)
		seen->ld_id = vppb->bound_ld_id;
}

struct fmd_binding_op *fmd_binding_op_add(struct fmd_ctx *ctx,
					  const struct fmd_bind_key *key,
					  enum fmd_bind_op_kind kind,
					  uint8_t unbind_option,
					  const char *task_id,
					  const struct fmd_vppb_seen *before)
{
	struct fmd_binding_op *op = calloc(1, sizeof(*op));

	if (!op)
		return NULL;
	op->key = *key;
	op->kind = kind;
	op->unbind_option = unbind_option;
	if (before)
		op->before = *before;
	else
		op->before.ld_id = FMD_LD_NONE;
	if (task_id)
		snprintf(op->task_id, sizeof(op->task_id), "%s", task_id);
	op->next = ctx->binding_ops;
	ctx->binding_ops = op;
	return op;
}

void fmd_binding_op_remove(struct fmd_ctx *ctx, struct fmd_binding_op *op)
{
	struct fmd_binding_op **pp;

	for (pp = &ctx->binding_ops; *pp; pp = &(*pp)->next) {
		if (*pp == op) {
			*pp = op->next;
			free(op);
			return;
		}
	}
}

bool fmd_binding_mark_find(const struct fmd_ctx *ctx, const struct fmd_bind_key *key)
{
	const struct fmd_binding_mark *m;

	for (m = ctx->binding_marks; m; m = m->next) {
		if (fmd_bind_key_eq(&m->key, key))
			return true;
	}
	return false;
}

int fmd_binding_mark_add(struct fmd_ctx *ctx, const struct fmd_bind_key *key)
{
	struct fmd_binding_mark *m;

	if (fmd_binding_mark_find(ctx, key))
		return 0;
	m = calloc(1, sizeof(*m));
	if (!m)
		return -ENOMEM;
	m->key = *key;
	m->next = ctx->binding_marks;
	ctx->binding_marks = m;
	return 0;
}

/* The key vPPB @vppb of VCS @vcs of @sw holds, false if it is not bound */
static bool vppb_key(const struct fmd_switch *sw, const struct fmd_vcs *vcs,
		     const struct fmd_vppb *vppb, struct fmd_bind_key *key)
{
	/* Bound Port ID is "Valid only when Binding Status is 02h or 03h" */
	if (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
	    vppb->binding_status != FMD_VPPB_BOUND_LD)
		return false;
	memset(key, 0, sizeof(*key));
	snprintf(key->switch_id, sizeof(key->switch_id), "%s", sw->base.id);
	key->vcs_id = vcs->vcs_id;
	key->vppb_id = vppb->vppb_id;
	key->port_id = vppb->bound_port_id;
	/* Bound LD ID: "Valid only when ... Binding Status is 03h" */
	key->ld_id = vppb->binding_status == FMD_VPPB_BOUND_LD ?
		     vppb->bound_ld_id : FMD_LD_NONE;
	return true;
}

static const struct fmd_switch *find_switch(const struct fmd_ctx *ctx, const char *id)
{
	return fmd_topology_find_switch((struct fmd_topology *)&ctx->topology, id);
}

void fmd_binding_marks_prune(struct fmd_ctx *ctx, const char *switch_id)
{
	struct fmd_binding_mark **pp = &ctx->binding_marks;

	while (*pp) {
		struct fmd_binding_mark *m = *pp;
		const struct fmd_switch *sw;
		struct fmd_bind_key now;
		bool keep = false;

		if (switch_id && strcmp(m->key.switch_id, switch_id)) {
			pp = &m->next;
			continue;
		}
		sw = find_switch(ctx, m->key.switch_id);
		for (int v = 0; sw && v < sw->num_vcs && !keep; v++) {
			const struct fmd_vcs *vcs = &sw->vcs[v];

			if (vcs->vcs_id != m->key.vcs_id)
				continue;
			for (int i = 0; i < vcs->num_vppbs; i++) {
				if (vcs->vppbs[i].vppb_id == m->key.vppb_id &&
				    vppb_key(sw, vcs, &vcs->vppbs[i], &now) &&
				    fmd_bind_key_eq(&now, &m->key))
					keep = true;
			}
		}
		if (keep) {
			pp = &m->next;
		} else {
			*pp = m->next;
			free(m);
		}
	}
}

void fmd_binding_state_free(struct fmd_ctx *ctx)
{
	while (ctx->binding_ops) {
		struct fmd_binding_op *op = ctx->binding_ops;

		ctx->binding_ops = op->next;
		free(op);
	}
	while (ctx->binding_marks) {
		struct fmd_binding_mark *m = ctx->binding_marks;

		ctx->binding_marks = m->next;
		free(m);
	}
}

/* The host of VCS @vcs_id of @sw: the one fmd_discover() made for it */
static const struct fmd_host *vcs_host(const struct fmd_ctx *ctx,
				       const struct fmd_switch *sw, uint8_t vcs_id)
{
	struct fmd_component *comp;

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST) {
		const struct fmd_host *host = fmd_to_host(comp);

		if (!host->direct_attached && host->vcs_id == vcs_id &&
		    !strcmp(host->switch_id, sw->base.id))
			return host;
	}
	return NULL;
}

static struct fmd_port *switch_port(const struct fmd_switch *sw, uint8_t number)
{
	struct fmd_component *child;

	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		if (fmd_to_port(child)->port_number == number)
			return fmd_to_port(child);
	}
	return NULL;
}

/*
 * The Connection of vPPB @vppb of VCS @vcs of @sw, -ENOENT if it gives
 * none (see fmd_conn_for_each()). Which vPPBs give one, and their state,
 * are cxl-fmd's choice.
 */
static int build_view(const struct fmd_ctx *ctx, const struct fmd_switch *sw,
		      const struct fmd_vcs *vcs, const struct fmd_vppb *vppb,
		      struct fmd_conn_view *v)
{
	const struct fmd_binding_op *op;
	struct fmd_component *dev = NULL;
	const struct fmd_port *port;
	struct fmd_bind_key key;
	bool bound;

	/* USP ID, and so the host, is "Valid only when the VCS is enabled" */
	if (vcs->state != VCS_STATE_ENABLED)
		return -ENOENT;
	/* "the USP vPPB, then the DSP vPPBs" (Section 7.1.4): 0 is the USP's */
	if (vppb->vppb_id == 0)
		return -ENOENT;

	memset(v, 0, sizeof(*v));
	op = fmd_binding_op_find(ctx, sw->base.id, vcs->vcs_id, vppb->vppb_id);
	bound = vppb_key(sw, vcs, vppb, &key);

	if (op && op->kind == FMD_BIND_OP_BIND) {
		/* 00h or 01h until it is done: what it becomes is the op's */
		key = op->key;
		v->state = FMD_CONN_STARTING;
	} else if (bound) {
		v->state = FMD_CONN_ENABLED;	/* until the target says otherwise */
		if (op && fmd_bind_key_eq(&op->key, &key))
			v->state = FMD_CONN_UPDATING;
		else
			op = NULL;	/* an unbind of what the vPPB held before */
	} else if (op && vppb->binding_status == FMD_VPPB_BUSY) {
		/* an unbind under way, 01h until it is done */
		key = op->key;
		v->state = FMD_CONN_UPDATING;
	} else {
		/* 00h, 01h with nothing of cxl-fmd's under way, or 04h */
		return -ENOENT;
	}

	port = switch_port(sw, key.port_id);
	if (port)
		dev = port_device(port);

	/*
	 * What is on the port. A memory device: what the binding reaches,
	 * or Unresolved if it reaches none of it that cxl-fmd knows. No
	 * device, or one that was never discovered: Absent. Anything else (a
	 * PCIe, Type 1 or Type 2 device, a switch): no Connection.
	 */
	v->target = fmd_bind_target(port, key.ld_id);
	v->device = dev;
	if (dev) {
		if (!v->target || v->target->type == FMD_COMPONENT_HEAD)
			v->state = v->state == FMD_CONN_ENABLED ?
				   FMD_CONN_UNRESOLVED : v->state;
		if (v->target && v->target->type == FMD_COMPONENT_LD)
			v->ld = fmd_to_ld((struct fmd_component *)v->target);
		if (v->target && v->target->type == FMD_COMPONENT_HEAD)
			v->head = fmd_to_head((struct fmd_component *)v->target);
		else if (v->ld)
			v->head = fmd_ld_head(v->ld);
		if (!v->head && port->head)
			v->head = port->head;
	} else {
		if (port && port->conn_dev_type != FMD_CONN_NONE &&
		    port->conn_dev_type != FMD_CONN_SLD &&
		    port->conn_dev_type != FMD_CONN_MLD)
			return -ENOENT;
		if (v->state == FMD_CONN_ENABLED)
			v->state = FMD_CONN_ABSENT;
	}

	v->host = vcs_host(ctx, sw, vcs->vcs_id);
	if (!v->host)
		return -ENOENT;

	v->kind = FMD_CONN_SWITCH;
	v->key = key;
	v->sw = sw;
	v->op = op;
	v->made_here = fmd_binding_mark_find(ctx, &key);
	if (fmd_conn_id_format(&key, v->id, sizeof(v->id)))
		return -ENOENT;
	return 0;
}

/*
 * The only direct-attached host, NULL if there is none or more than one.
 * A device that is on no switch has one link, to one root port, and
 * nothing cxl-fmd reads says which host's that is; with one host to choose
 * from, it is that one (cxl-fmd's choice).
 */
static const struct fmd_host *sole_direct_host(const struct fmd_ctx *ctx)
{
	const struct fmd_host *found = NULL;
	struct fmd_component *comp;

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST) {
		if (!fmd_to_host(comp)->direct_attached)
			continue;
		if (found)
			return NULL;
		found = fmd_to_host(comp);
	}
	return found;
}

static void build_direct(const struct fmd_host *host, const struct fmd_component *dev,
			 const struct fmd_head *head, struct fmd_conn_view *v)
{
	memset(v, 0, sizeof(*v));
	v->kind = FMD_CONN_DIRECT;
	v->host = host;
	v->device = dev;
	v->head = head;
	v->target = head ? &head->base : dev;
	v->state = FMD_CONN_ENABLED;
	if (head)
		snprintf(v->id, sizeof(v->id), "%s_direct_%s_h%u", host->base.id,
			 dev->id, head->head_id);
	else
		snprintf(v->id, sizeof(v->id), "%s_direct_%s", host->base.id, dev->id);
}

/*
 * The direct-attached Connections, cxl-fmd's choice, each always Enabled:
 *
 *  - the only direct-attached host (sole_direct_host()) with each SLD and
 *    MLD that is on no switch port. Of an MLD, all of its LDs: what an MLD
 *    on a root port gives its host, the specification does not say;
 *  - each head of a Multi-Headed device that is on no switch port, and
 *    whose host is known (attached_host_id, which only a mock topology
 *    sets: no command maps a head to a host), with the pool LDs the LD Map
 *    maps to that head (Table 7-61).
 */
static int for_each_direct(const struct fmd_ctx *ctx, fmd_conn_cb cb, void *arg)
{
	const struct fmd_host *sole = sole_direct_host(ctx);
	struct fmd_component *comp, *child;
	struct fmd_conn_view v;
	int rc;

	for (int t = 0; sole && t < 2; t++) {
		fmd_for_each_type(&ctx->topology, comp,
				  t ? FMD_COMPONENT_MLD : FMD_COMPONENT_SLD) {
			if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT)
				continue;
			build_direct(sole, comp, NULL, &v);
			rc = cb(&v, arg);
			if (rc)
				return rc;
		}
	}

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MHD) {
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_HEAD) {
			const struct fmd_head *head = fmd_to_head(child);
			const struct fmd_host *host;

			if (head->port || !head->attached_host_id[0])
				continue;
			host = fmd_topology_find_host((struct fmd_topology *)&ctx->topology,
						      head->attached_host_id);
			if (!host || !host->direct_attached)
				continue;
			build_direct(host, comp, head, &v);
			rc = cb(&v, arg);
			if (rc)
				return rc;
		}
	}
	return 0;
}

int fmd_conn_for_each(const struct fmd_ctx *ctx, fmd_conn_cb cb, void *arg)
{
	struct fmd_component *comp;
	struct fmd_conn_view v;
	int rc;

	if (!ctx || !cb)
		return -EINVAL;

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
		const struct fmd_switch *sw = fmd_to_switch(comp);

		for (int i = 0; i < sw->num_vcs; i++) {
			const struct fmd_vcs *vcs = &sw->vcs[i];

			for (int p = 0; p < vcs->num_vppbs; p++) {
				if (build_view(ctx, sw, vcs, &vcs->vppbs[p], &v))
					continue;
				rc = cb(&v, arg);
				if (rc)
					return rc;
			}
		}
	}
	return for_each_direct(ctx, cb, arg);
}

struct direct_find {
	const char *id;
	struct fmd_conn_view *v;
};

static int direct_match(const struct fmd_conn_view *v, void *arg)
{
	struct direct_find *f = arg;

	if (strcmp(v->id, f->id))
		return 0;
	*f->v = *v;
	return 1;
}

int fmd_conn_lookup(const struct fmd_ctx *ctx, const char *id,
		    struct fmd_conn_view *v)
{
	const struct fmd_switch *sw;
	struct fmd_bind_key key;

	if (!ctx || !id || !v)
		return -EINVAL;
	if (fmd_conn_id_parse(id, &key)) {
		struct direct_find f = { .id = id, .v = v };

		/* not a vPPB's: a direct-attached one, by its ID */
		return for_each_direct(ctx, direct_match, &f) == 1 ? 0 : -ENOENT;
	}
	sw = find_switch(ctx, key.switch_id);
	if (!sw)
		return -ENOENT;

	for (int i = 0; i < sw->num_vcs; i++) {
		const struct fmd_vcs *vcs = &sw->vcs[i];

		if (vcs->vcs_id != key.vcs_id)
			continue;
		for (int p = 0; p < vcs->num_vppbs; p++) {
			if (vcs->vppbs[p].vppb_id != key.vppb_id)
				continue;
			/* the vPPB has to hold what the ID says it does */
			if (build_view(ctx, sw, vcs, &vcs->vppbs[p], v) ||
			    strcmp(v->id, id))
				return -ENOENT;
			return 0;
		}
	}
	return -ENOENT;
}

int fmd_conn_find_target(const struct fmd_ctx *ctx, const struct fmd_switch *sw,
			 uint8_t vcs_id, uint8_t port_id, uint16_t ld_id,
			 struct fmd_conn_view *v)
{
	if (!ctx || !sw || !v)
		return -EINVAL;

	for (int i = 0; i < sw->num_vcs; i++) {
		const struct fmd_vcs *vcs = &sw->vcs[i];

		if (vcs->vcs_id != vcs_id)
			continue;
		for (int p = 0; p < vcs->num_vppbs; p++) {
			if (build_view(ctx, sw, vcs, &vcs->vppbs[p], v))
				continue;
			if (v->key.port_id == port_id && v->key.ld_id == ld_id)
				return 0;
		}
	}
	return -ENOENT;
}

int fmd_conn_for_each_chunk(const struct fmd_conn_view *v,
			    int (*cb)(const struct fmd_mem_chunk *chunk, void *arg),
			    void *arg)
{
	const struct fmd_component *dev;
	struct fmd_component *dom, *child;
	int rc;

	if (!v || !cb)
		return -EINVAL;
	if (!v->target || !v->device)
		return 0;
	/*
	 * A head of a switch-attached device: which pool LDs its LD-IDs are
	 * is not known. One on a host's link has the pool LDs the LD Map
	 * maps to it. A direct-attached MLD has all of its LDs.
	 */
	if (v->target->type == FMD_COMPONENT_HEAD && v->kind != FMD_CONN_DIRECT)
		return 0;

	dev = v->device;
	fmd_for_each_child_type(dev, dom, FMD_COMPONENT_DOMAIN) {
		fmd_for_each_child_type(dom, child, FMD_COMPONENT_CHUNK) {
			const struct fmd_mem_chunk *chunk = fmd_to_chunk(child);

			if (chunk->staged)
				continue;
			if (v->ld && fmd_chunk_ld(dev, chunk) != v->ld)
				continue;
			if (v->target->type == FMD_COMPONENT_HEAD) {
				const struct fmd_ld *ld = fmd_chunk_ld(dev, chunk);

				if (!ld || ld->binding_point_id != v->head->head_id)
					continue;
			}
			rc = cb(chunk, arg);
			if (rc)
				return rc;
		}
	}
	return 0;
}
