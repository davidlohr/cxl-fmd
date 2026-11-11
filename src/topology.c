/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Topology Management
 *
 * Tree-based representation of the discovered CXL fabric.
 * Thread-safe access via rwlock.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>

#include "cxl-fmd.h"


/*
 * Simple djb2 hash function
 */
static uint32_t hash_string(const char *str)
{
	uint32_t hash = 5381;
	int c;

	while ((c = *str++))
		hash = ((hash << 5) + hash) + c;

	return hash;
}

static void hash_table_insert(struct fmd_hash_table *ht, struct fmd_component *comp)
{
	uint32_t idx;

	comp->hash = hash_string(comp->id);
	idx = comp->hash % FMD_HASH_BUCKETS;

	comp->hash_next = ht->buckets[idx];
	ht->buckets[idx] = comp;
	ht->count++;
}

static void hash_table_remove(struct fmd_hash_table *ht, struct fmd_component *comp)
{
	uint32_t idx = comp->hash % FMD_HASH_BUCKETS;
	struct fmd_component **pp;

	for (pp = &ht->buckets[idx]; *pp; pp = &(*pp)->hash_next) {
		if (*pp == comp) {
			*pp = comp->hash_next;
			comp->hash_next = NULL;
			ht->count--;
			return;
		}
	}
}

static struct fmd_component *hash_table_find(struct fmd_hash_table *ht, const char *id)
{
	uint32_t hash = hash_string(id);
	uint32_t idx = hash % FMD_HASH_BUCKETS;
	struct fmd_component *comp;

	for (comp = ht->buckets[idx]; comp; comp = comp->hash_next) {
		if (comp->hash == hash && strcmp(comp->id, id) == 0)
			return comp;
	}

	return NULL;
}


/*
 * Add child to parent's children list (prepend for O(1))
 */
static void tree_add_child(struct fmd_component *parent, struct fmd_component *child)
{
	child->parent = parent;
	child->next_sibling = parent->first_child;
	parent->first_child = child;
}

/*
 * Remove child from parent's children list
 */
static void tree_remove_child(struct fmd_component *parent, struct fmd_component *child)
{
	struct fmd_component **pp;

	for (pp = &parent->first_child; *pp; pp = &(*pp)->next_sibling) {
		if (*pp == child) {
			*pp = child->next_sibling;
			child->parent = NULL;
			child->next_sibling = NULL;
			return;
		}
	}
}

/*
 * Walk subtree depth-first (recursive helper)
 */
static int walk_subtree(struct fmd_component *comp, fmd_walk_cb callback, void *ctx)
{
	struct fmd_component *child;
	int ret;

	ret = callback(comp, ctx);
	if (ret != 0)
		return ret;

	fmd_for_each_child(comp, child) {
		ret = walk_subtree(child, callback, ctx);
		if (ret != 0)
			return ret;
	}

	return 0;
}


void fmd_topology_rdlock(struct fmd_topology *topo)
{
	pthread_rwlock_rdlock(&topo->lock);
}

void fmd_topology_rdunlock(struct fmd_topology *topo)
{
	pthread_rwlock_unlock(&topo->lock);
}

void fmd_topology_wrlock(struct fmd_topology *topo)
{
	pthread_rwlock_wrlock(&topo->lock);
}

void fmd_topology_wrunlock(struct fmd_topology *topo)
{
	pthread_rwlock_unlock(&topo->lock);
}


int fmd_topology_init(struct fmd_topology *topo)
{
	if (!topo)
		return -1;

	memset(topo, 0, sizeof(*topo));

	if (pthread_rwlock_init(&topo->lock, NULL) != 0)
		return -1;

	/* Create fabric root */
	topo->root = fmd_fabric_create("CXL");
	if (!topo->root) {
		pthread_rwlock_destroy(&topo->lock);
		return -1;
	}

	/* Add root to hash table */
	hash_table_insert(&topo->components, &topo->root->base);

	topo->generation = 1;

	return 0;
}

/*
 * Recursive cleanup helper
 */
static void cleanup_subtree(struct fmd_component *comp)
{
	struct fmd_component *child, *next;

	for (child = comp->first_child; child; child = next) {
		next = child->next_sibling;
		cleanup_subtree(child);
	}

	fmd_component_free(comp);
}

void fmd_topology_cleanup(struct fmd_topology *topo)
{
	if (!topo)
		return;

	pthread_rwlock_wrlock(&topo->lock);

	if (topo->root) {
		cleanup_subtree(&topo->root->base);
		topo->root = NULL;
	}

	/* Clear hash table */
	memset(&topo->components, 0, sizeof(topo->components));

	pthread_rwlock_unlock(&topo->lock);
	pthread_rwlock_destroy(&topo->lock);
}


int fmd_topology_add(struct fmd_topology *topo, struct fmd_component *comp,
		     struct fmd_component *parent)
{
	if (!topo || !comp)
		return -1;

	/* Default parent is root */
	if (!parent)
		parent = &topo->root->base;

	pthread_rwlock_wrlock(&topo->lock);

	/* Set fabric pointer for direct access to root/ep */
	comp->fabric = topo->root;

	/* Add to tree */
	tree_add_child(parent, comp);

	/* Add to hash table */
	hash_table_insert(&topo->components, comp);

	/* Update statistics */
	switch (comp->type) {
	case FMD_COMPONENT_SWITCH:
		topo->root->switch_count++;
		break;
	case FMD_COMPONENT_PORT:
		topo->root->port_count++;
		break;
	case FMD_COMPONENT_SLD:
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
		topo->root->device_count++;
		break;
	case FMD_COMPONENT_LD:
		topo->root->ld_count++;
		break;
	case FMD_COMPONENT_DOMAIN:
		topo->root->domain_count++;
		break;
	case FMD_COMPONENT_CHUNK:
		topo->root->chunk_count++;
		break;
	case FMD_COMPONENT_HOST:
		topo->root->host_count++;
		break;
	case FMD_COMPONENT_HEAD:
		/* Heads are tracked as part of MHD, no separate counter */
		break;
	default:
		break;
	}

	topo->generation++;

	pthread_rwlock_unlock(&topo->lock);

	return 0;
}

/*
 * Remove helper - removes from hash table recursively
 */
static void remove_from_hash(struct fmd_hash_table *ht, struct fmd_component *comp)
{
	struct fmd_component *child;

	fmd_for_each_child(comp, child) {
		remove_from_hash(ht, child);
	}

	hash_table_remove(ht, comp);
}

int fmd_topology_remove(struct fmd_topology *topo, struct fmd_component *comp)
{
	if (!topo || !comp)
		return -1;

	/* Cannot remove root */
	if (comp->type == FMD_COMPONENT_ROOT)
		return -1;

	pthread_rwlock_wrlock(&topo->lock);

	/* Remove from hash table (recursively for all children) */
	remove_from_hash(&topo->components, comp);

	/* Remove from parent's children list */
	if (comp->parent)
		tree_remove_child(comp->parent, comp);

	/* Free the subtree */
	cleanup_subtree(comp);

	topo->generation++;

	pthread_rwlock_unlock(&topo->lock);

	return 0;
}


struct fmd_component *fmd_topology_find(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp;

	if (!topo || !id)
		return NULL;

	pthread_rwlock_rdlock(&topo->lock);
	comp = hash_table_find(&topo->components, id);
	pthread_rwlock_unlock(&topo->lock);

	return comp;
}

struct fmd_switch *fmd_topology_find_switch(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_SWITCH)
		return fmd_to_switch(comp);

	return NULL;
}

struct fmd_port *fmd_topology_find_port(struct fmd_topology *topo,
					const char *switch_id, uint8_t port_num)
{
	struct fmd_switch *sw;
	struct fmd_component *child;
	char port_id[FMD_MAX_ID_LEN];

	sw = fmd_topology_find_switch(topo, switch_id);
	if (!sw)
		return NULL;

	/* Try direct ID lookup first */
	snprintf(port_id, sizeof(port_id), "%s_dsp%u", switch_id, port_num);
	struct fmd_component *comp = fmd_topology_find(topo, port_id);
	if (comp && comp->type == FMD_COMPONENT_PORT)
		return fmd_to_port(comp);

	/* Fall back to iterating children */
	pthread_rwlock_rdlock(&topo->lock);
	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		struct fmd_port *port = fmd_to_port(child);
		if (port->port_number == port_num) {
			pthread_rwlock_unlock(&topo->lock);
			return port;
		}
	}
	pthread_rwlock_unlock(&topo->lock);

	return NULL;
}

struct fmd_sld *fmd_topology_find_sld(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_SLD)
		return fmd_to_sld(comp);

	return NULL;
}

struct fmd_mld *fmd_topology_find_mld(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_MLD)
		return fmd_to_mld(comp);

	return NULL;
}

struct fmd_ld *fmd_topology_find_ld(struct fmd_topology *topo,
				    const char *device_id, uint8_t ld_id)
{
	struct fmd_component *parent;
	struct fmd_component *child;
	char id[FMD_MAX_ID_LEN];

	/* Try direct ID lookup first */
	snprintf(id, sizeof(id), "%s_ld%u", device_id, ld_id);
	struct fmd_component *comp = fmd_topology_find(topo, id);
	if (comp && comp->type == FMD_COMPONENT_LD)
		return fmd_to_ld(comp);

	/* Find the parent device (MLD or MHD) */
	parent = fmd_topology_find(topo, device_id);
	if (!parent)
		return NULL;

	if (parent->type != FMD_COMPONENT_MLD &&
	    parent->type != FMD_COMPONENT_MHD)
		return NULL;

	/* Fall back to iterating children */
	pthread_rwlock_rdlock(&topo->lock);
	fmd_for_each_child_type(parent, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		if (ld->ld_id == ld_id) {
			pthread_rwlock_unlock(&topo->lock);
			return ld;
		}
	}
	pthread_rwlock_unlock(&topo->lock);

	return NULL;
}

struct fmd_mem_domain *fmd_topology_find_domain(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_DOMAIN)
		return fmd_to_domain(comp);

	return NULL;
}

struct fmd_mem_chunk *fmd_topology_find_chunk(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_CHUNK)
		return fmd_to_chunk(comp);

	return NULL;
}


int fmd_topology_walk(struct fmd_topology *topo, fmd_walk_cb callback, void *ctx)
{
	int ret;

	if (!topo || !callback || !topo->root)
		return -1;

	pthread_rwlock_rdlock(&topo->lock);
	ret = walk_subtree(&topo->root->base, callback, ctx);
	pthread_rwlock_unlock(&topo->lock);

	return ret;
}

int fmd_component_walk(struct fmd_component *root, fmd_walk_cb callback, void *ctx)
{
	if (!root || !callback)
		return -1;

	return walk_subtree(root, callback, ctx);
}

int fmd_component_count_children(struct fmd_component *comp, enum fmd_component_type type)
{
	struct fmd_component *child;
	int count = 0;

	if (!comp)
		return 0;

	fmd_for_each_child(comp, child) {
		if (child->type == type)
			count++;
	}

	return count;
}


/*
 * Initialize base component fields
 * Returns 0 on success, -1 if ID is too long
 */
static int component_init(struct fmd_component *comp, const char *id,
			  enum fmd_component_type type)
{
	if (strlen(id) >= FMD_MAX_ID_LEN)
		return -1;

	memset(comp, 0, sizeof(*comp));
	snprintf(comp->id, sizeof(comp->id), "%s", id);
	comp->type = type;
	return 0;
}

struct fmd_fabric *fmd_fabric_create(const char *id)
{
	struct fmd_fabric *fabric;

	fabric = calloc(1, sizeof(*fabric));
	if (!fabric)
		return NULL;

	if (component_init(&fabric->base, id, FMD_COMPONENT_ROOT) < 0) {
		free(fabric);
		return NULL;
	}
	snprintf(fabric->description, sizeof(fabric->description), "CXL Fabric");

	return fabric;
}

struct fmd_switch *fmd_switch_create(const char *id)
{
	struct fmd_switch *sw;

	sw = calloc(1, sizeof(*sw));
	if (!sw)
		return NULL;

	if (component_init(&sw->base, id, FMD_COMPONENT_SWITCH) < 0) {
		free(sw);
		return NULL;
	}

	return sw;
}

struct fmd_port *fmd_port_create(const char *id, uint8_t port_number)
{
	struct fmd_port *port;

	port = calloc(1, sizeof(*port));
	if (!port)
		return NULL;

	if (component_init(&port->base, id, FMD_COMPONENT_PORT) < 0) {
		free(port);
		return NULL;
	}
	port->port_number = port_number;
	port->state = FMD_PORT_DSP;

	return port;
}

struct fmd_sld *fmd_sld_create(const char *id)
{
	struct fmd_sld *sld;

	sld = calloc(1, sizeof(*sld));
	if (!sld)
		return NULL;

	if (component_init(&sld->base, id, FMD_COMPONENT_SLD) < 0) {
		free(sld);
		return NULL;
	}

	return sld;
}

struct fmd_mld *fmd_mld_create(const char *id)
{
	struct fmd_mld *mld;

	mld = calloc(1, sizeof(*mld));
	if (!mld)
		return NULL;

	if (component_init(&mld->base, id, FMD_COMPONENT_MLD) < 0) {
		free(mld);
		return NULL;
	}
	mld->switch_port = -1;  /* Direct-attached by default */

	return mld;
}

struct fmd_ld *fmd_ld_create(const char *id, uint8_t ld_id)
{
	struct fmd_ld *ld;

	ld = calloc(1, sizeof(*ld));
	if (!ld)
		return NULL;

	if (component_init(&ld->base, id, FMD_COMPONENT_LD) < 0) {
		free(ld);
		return NULL;
	}
	ld->ld_id = ld_id;
	ld->binding_point_id = 0xFF;  /* Unbound by default */
	ld->visible_ld_id = ld_id;    /* Default: visible ID equals internal ID */

	return ld;
}

struct fmd_mem_domain *fmd_domain_create(const char *id)
{
	struct fmd_mem_domain *domain;

	domain = calloc(1, sizeof(*domain));
	if (!domain)
		return NULL;

	if (component_init(&domain->base, id, FMD_COMPONENT_DOMAIN) < 0) {
		free(domain);
		return NULL;
	}

	return domain;
}

struct fmd_mem_chunk *fmd_chunk_create(const char *id)
{
	struct fmd_mem_chunk *chunk;

	chunk = calloc(1, sizeof(*chunk));
	if (!chunk)
		return NULL;

	if (component_init(&chunk->base, id, FMD_COMPONENT_CHUNK) < 0) {
		free(chunk);
		return NULL;
	}

	/* Default address range type */
	strncpy(chunk->address_range_type, "PMEM", sizeof(chunk->address_range_type) - 1);

	return chunk;
}

/*
 * Generate a unique chunk ID within a domain.
 */
static void generate_chunk_id(struct fmd_mem_domain *domain, char *buf, size_t len)
{
	int next_num = 0;
	struct fmd_component *child;

	fmd_for_each_child(&domain->base, child) {
		if (child->type == FMD_COMPONENT_CHUNK)
			next_num++;
	}

	snprintf(buf, len, "%s_chunk%d", domain->base.id, next_num);
}

/*
 * Find the next available offset (append at end of existing chunks).
 */
static uint64_t find_next_offset(struct fmd_mem_domain *domain)
{
	uint64_t max_end = 0;
	struct fmd_component *child;

	fmd_for_each_child(&domain->base, child) {
		if (child->type == FMD_COMPONENT_CHUNK) {
			struct fmd_mem_chunk *c = fmd_to_chunk(child);
			uint64_t end = c->base_address + c->size;
			if (end > max_end)
				max_end = end;
		}
	}

	return max_end;
}

/*
 * Check if offset+size overlaps with existing chunks.
 */
static bool chunk_overlaps(struct fmd_mem_domain *domain, uint64_t offset, uint64_t size)
{
	struct fmd_component *child;
	uint64_t new_end = offset + size;

	fmd_for_each_child(&domain->base, child) {
		if (child->type == FMD_COMPONENT_CHUNK) {
			struct fmd_mem_chunk *c = fmd_to_chunk(child);
			uint64_t chunk_end = c->base_address + c->size;

			/* Check for overlap */
			if (offset < chunk_end && new_end > c->base_address)
				return true;
		}
	}

	return false;
}

struct fmd_mem_chunk *fmd_chunk_create_in_domain(
	struct fmd_topology *topo,
	struct fmd_mem_domain *domain,
	uint64_t size,
	uint64_t offset,
	const char *address_range_type,
	struct fmd_interleave_set *interleave_sets,
	int interleave_set_count)
{
	struct fmd_mem_chunk *chunk;
	char chunk_id[FMD_MAX_ID_LEN];

	if (!topo || !domain || size == 0)
		return NULL;

	/* Check capacity */
	if (size > domain->remaining)
		return NULL;

	/* Auto-assign offset if not specified */
	if (offset == UINT64_MAX)
		offset = find_next_offset(domain);

	/* Check for overlap with existing chunks */
	if (chunk_overlaps(domain, offset, size))
		return NULL;

	/* Generate chunk ID */
	generate_chunk_id(domain, chunk_id, sizeof(chunk_id));

	/* Create the chunk */
	chunk = fmd_chunk_create(chunk_id);
	if (!chunk)
		return NULL;

	/* Set properties */
	chunk->base_address = offset;
	chunk->size = size;

	if (address_range_type) {
		strncpy(chunk->address_range_type, address_range_type,
			sizeof(chunk->address_range_type) - 1);
	}

	/* Copy interleave sets */
	if (interleave_sets && interleave_set_count > 0) {
		chunk->interleaved = true;
		chunk->interleave_set_count = (interleave_set_count > FMD_MAX_INTERLEAVE)
			? FMD_MAX_INTERLEAVE : interleave_set_count;
		memcpy(chunk->interleave_sets, interleave_sets,
		       chunk->interleave_set_count * sizeof(struct fmd_interleave_set));
	}

	/* Add to topology */
	if (fmd_topology_add(topo, &chunk->base, &domain->base) < 0) {
		fmd_component_free(&chunk->base);
		return NULL;
	}

	/* Update domain remaining capacity */
	domain->remaining -= size;

	return chunk;
}

int fmd_chunk_remove(struct fmd_topology *topo, struct fmd_mem_chunk *chunk)
{
	struct fmd_mem_domain *domain;

	if (!topo || !chunk)
		return -1;

	/* Get parent domain to update remaining capacity */
	if (chunk->base.parent && chunk->base.parent->type == FMD_COMPONENT_DOMAIN) {
		domain = fmd_to_domain(chunk->base.parent);
		domain->remaining += chunk->size;
	}

	/* Remove from topology (this also frees the chunk) */
	return fmd_topology_remove(topo, &chunk->base);
}

struct fmd_host *fmd_host_create(const char *id)
{
	struct fmd_host *host;

	host = calloc(1, sizeof(*host));
	if (!host)
		return NULL;

	if (component_init(&host->base, id, FMD_COMPONENT_HOST) < 0) {
		free(host);
		return NULL;
	}
	host->direct_attached = true;  /* Default to direct-attached */

	return host;
}

struct fmd_mhd *fmd_mhd_create(const char *id)
{
	struct fmd_mhd *mhd;

	mhd = calloc(1, sizeof(*mhd));
	if (!mhd)
		return NULL;

	if (component_init(&mhd->base, id, FMD_COMPONENT_MHD) < 0) {
		free(mhd);
		return NULL;
	}

	return mhd;
}

struct fmd_head *fmd_head_create(const char *id, uint8_t head_id)
{
	struct fmd_head *head;

	head = calloc(1, sizeof(*head));
	if (!head)
		return NULL;

	if (component_init(&head->base, id, FMD_COMPONENT_HEAD) < 0) {
		free(head);
		return NULL;
	}
	head->head_id = head_id;
	head->state = FMD_HEAD_DISABLED;

	return head;
}

struct fmd_dc_region *fmd_dc_region_create(const char *id, uint8_t region_id,
					   uint16_t host_id)
{
	struct fmd_dc_region *region;

	region = calloc(1, sizeof(*region));
	if (!region)
		return NULL;

	if (component_init(&region->base, id, FMD_COMPONENT_DC_REGION) < 0) {
		free(region);
		return NULL;
	}
	region->region_id = region_id;
	region->host_id = host_id;

	return region;
}

struct fmd_dc_extent *fmd_dc_extent_create(const char *id)
{
	struct fmd_dc_extent *ext;

	ext = calloc(1, sizeof(*ext));
	if (!ext)
		return NULL;

	if (component_init(&ext->base, id, FMD_COMPONENT_DC_EXTENT) < 0) {
		free(ext);
		return NULL;
	}

	return ext;
}

struct fmd_host *fmd_topology_find_host(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_HOST)
		return fmd_to_host(comp);

	return NULL;
}

struct fmd_mhd *fmd_topology_find_mhd(struct fmd_topology *topo, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(topo, id);

	if (comp && comp->type == FMD_COMPONENT_MHD)
		return fmd_to_mhd(comp);

	return NULL;
}

struct fmd_head *fmd_topology_find_head(struct fmd_topology *topo,
					const char *mhd_id, uint8_t head_id)
{
	struct fmd_mhd *mhd;
	struct fmd_component *child;
	char id[FMD_MAX_ID_LEN];

	mhd = fmd_topology_find_mhd(topo, mhd_id);
	if (!mhd)
		return NULL;

	/* Try direct ID lookup first */
	snprintf(id, sizeof(id), "%s_head%u", mhd_id, head_id);
	struct fmd_component *comp = fmd_topology_find(topo, id);
	if (comp && comp->type == FMD_COMPONENT_HEAD)
		return fmd_to_head(comp);

	/* Fall back to iterating children */
	pthread_rwlock_rdlock(&topo->lock);
	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
		struct fmd_head *head = fmd_to_head(child);
		if (head->head_id == head_id) {
			pthread_rwlock_unlock(&topo->lock);
			return head;
		}
	}
	pthread_rwlock_unlock(&topo->lock);

	return NULL;
}


/*
 * Connection management
 */

static int connection_next_id = 0;

struct fmd_connection *fmd_connection_create(struct fmd_ctx *ctx,
					     const char *initiator_id,
					     const char *target_id,
					     const char *chunk_id,
					     uint8_t access_caps)
{
	struct fmd_connection *conn;

	if (!ctx || !initiator_id || !target_id || !chunk_id)
		return NULL;

	conn = calloc(1, sizeof(*conn));
	if (!conn)
		return NULL;

	/* Generate connection ID */
	snprintf(conn->id, sizeof(conn->id), "conn%d", connection_next_id++);

	/* Copy endpoint IDs */
	snprintf(conn->initiator_id, sizeof(conn->initiator_id), "%s", initiator_id);
	snprintf(conn->target_id, sizeof(conn->target_id), "%s", target_id);
	snprintf(conn->chunk_id, sizeof(conn->chunk_id), "%s", chunk_id);

	conn->access_caps = access_caps;

	/* Add to linked list */
	conn->next = ctx->connections;
	ctx->connections = conn;
	ctx->connection_count++;

	return conn;
}

struct fmd_connection *fmd_connection_find(struct fmd_ctx *ctx, const char *id)
{
	struct fmd_connection *conn;

	if (!ctx || !id)
		return NULL;

	for (conn = ctx->connections; conn; conn = conn->next) {
		if (strcmp(conn->id, id) == 0)
			return conn;
	}

	return NULL;
}

struct fmd_connection *fmd_connection_find_by_chunk(struct fmd_ctx *ctx,
						    const char *chunk_id)
{
	struct fmd_connection *conn;

	if (!ctx || !chunk_id)
		return NULL;

	for (conn = ctx->connections; conn; conn = conn->next) {
		if (strcmp(conn->chunk_id, chunk_id) == 0)
			return conn;
	}

	return NULL;
}

int fmd_connection_remove(struct fmd_ctx *ctx, struct fmd_connection *conn)
{
	struct fmd_connection **pp;

	if (!ctx || !conn)
		return -1;

	for (pp = &ctx->connections; *pp; pp = &(*pp)->next) {
		if (*pp == conn) {
			*pp = conn->next;
			free(conn);
			ctx->connection_count--;
			return 0;
		}
	}

	return -1;  /* Not found */
}

void fmd_component_free(struct fmd_component *comp)
{
	if (!comp)
		return;

	/* Devices own their DCD capability block */
	switch (comp->type) {
	case FMD_COMPONENT_SLD:
		free(fmd_to_sld(comp)->dcd);
		break;
	case FMD_COMPONENT_MLD:
		free(fmd_to_mld(comp)->dcd);
		break;
	case FMD_COMPONENT_MHD:
		free(fmd_to_mhd(comp)->dcd);
		break;
	default:
		break;
	}

	/* All component types are allocated with calloc, so free works */
	free(comp);
}

/*
 * Resolve vPPB bindings for a switch.
 *
 * After all devices are discovered/parsed, this function resolves the
 * port and LD pointer references for each vPPB based on binding status.
 * Used by both hardware discovery and mock topology loading.
 */
void fmd_switch_resolve_vppb_bindings(struct fmd_topology *topo, struct fmd_switch *sw)
{
	int v;
	int p;

	if (!topo || !sw)
		return;

	for (v = 0; v < sw->num_vcs; v++) {
		struct fmd_vcs *vcs = &sw->vcs[v];

		for (p = 0; p < vcs->num_vppbs; p++) {
			struct fmd_vppb *vppb = &vcs->vppbs[p];

			/* Resolve port reference */
			if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
			    vppb->binding_status == FMD_VPPB_BOUND_LD) {
				vppb->port = fmd_topology_find_port(topo,
								   sw->base.id,
								   vppb->bound_port_id);
			}

			/* Resolve LD reference for BoundLD */
			if (vppb->binding_status == FMD_VPPB_BOUND_LD && vppb->port) {
				struct fmd_component *child;

				fmd_for_each_child(&vppb->port->base, child) {
					if (child->type == FMD_COMPONENT_MLD) {
						struct fmd_component *ld_child;

						fmd_for_each_child(child, ld_child) {
							if (ld_child->type == FMD_COMPONENT_LD) {
								struct fmd_ld *ld = fmd_to_ld(ld_child);

								if (ld->ld_id == vppb->bound_ld_id) {
									vppb->ld = ld;
									break;
								}
							}
						}
						break;
					}
				}
			}
		}
	}
}

/*
 * Memory Resource Creation
 *
 * These functions create memory domain and chunk resources for devices.
 * Used by both hardware discovery and mock topology loading.
 */

int fmd_create_sld_memory_resources(struct fmd_ctx *ctx, struct fmd_sld *sld)
{
	struct fmd_mem_domain *domain;
	struct fmd_mem_chunk *chunk;
	char id[128];

	if (sld->memory_size == 0)
		return 0;

	/* Create domain */
	(void)snprintf(id, sizeof(id), "%.50s_domain0", sld->base.id);
	domain = fmd_domain_create(id);
	if (!domain)
		return -1;

	domain->capacity = sld->memory_size;
	domain->remaining = sld->memory_size;

	fmd_topology_add(&ctx->topology, &domain->base, &sld->base);

	/* Create chunk for existing allocation */
	(void)snprintf(id, sizeof(id), "%.50s_chunk0", sld->base.id);
	chunk = fmd_chunk_create(id);
	if (!chunk)
		return -1;

	chunk->base_address = 0;
	chunk->size = sld->memory_size;
	chunk->interleaved = false;

	if (sld->memdev.volatile_capacity > 0 && sld->memdev.persistent_capacity == 0)
		strncpy(chunk->address_range_type, "Volatile",
			sizeof(chunk->address_range_type) - 1);
	else
		strncpy(chunk->address_range_type, "PMEM",
			sizeof(chunk->address_range_type) - 1);

	fmd_topology_add(&ctx->topology, &chunk->base, &domain->base);
	domain->remaining -= chunk->size;

	fmd_dbg(ctx, "%s: created domain and chunk (%" PRIu64 " MB)",
		sld->base.id, (uint64_t)(sld->memory_size / (1024ULL * 1024)));

	return 0;
}

int fmd_create_mld_memory_resources(struct fmd_ctx *ctx, struct fmd_mld *mld)
{
	struct fmd_mem_domain *domain;
	struct fmd_component *child;
	uint64_t total_size = 0;
	uint64_t base_addr = 0;
	char id[128];

	/* Sum up all LD memory sizes */
	fmd_for_each_child_type(&mld->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		total_size += ld->memory_size;
	}

	if (total_size == 0)
		return 0;

	/* Create domain (MLD = MemoryDomain per OCP model) */
	(void)snprintf(id, sizeof(id), "%.50s_domain0", mld->base.id);
	domain = fmd_domain_create(id);
	if (!domain)
		return -1;

	domain->capacity = total_size;
	domain->remaining = total_size;

	fmd_topology_add(&ctx->topology, &domain->base, &mld->base);

	/* Create one chunk per LD (each LD = MemoryChunk per OCP model) */
	fmd_for_each_child_type(&mld->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		struct fmd_mem_chunk *chunk;

		(void)snprintf(id, sizeof(id), "%.50s_ld%u_chunk", mld->base.id, ld->ld_id);
		chunk = fmd_chunk_create(id);
		if (!chunk)
			return -1;

		chunk->base_address = base_addr;
		chunk->size = ld->memory_size;
		chunk->interleaved = false;

		if (ld->memdev.volatile_capacity > 0 && ld->memdev.persistent_capacity == 0)
			strncpy(chunk->address_range_type, "Volatile",
				sizeof(chunk->address_range_type) - 1);
		else
			strncpy(chunk->address_range_type, "PMEM",
				sizeof(chunk->address_range_type) - 1);

		/* Set interleave sets to reference the LD's Memory resource */
		chunk->interleave_set_count = 1;
		strncpy(chunk->interleave_sets[0].memory_id, ld->base.id,
			sizeof(chunk->interleave_sets[0].memory_id) - 1);
		chunk->interleave_sets[0].size_mib = ld->memory_size / (1024ULL * 1024);

		fmd_topology_add(&ctx->topology, &chunk->base, &domain->base);
		domain->remaining -= chunk->size;

		base_addr += ld->memory_size;
	}

	fmd_dbg(ctx, "%s: created domain and %d chunks (%" PRIu64 " MB total)",
		mld->base.id, mld->ld_count, (uint64_t)(total_size / (1024ULL * 1024)));

	return 0;
}

int fmd_create_mhd_memory_resources(struct fmd_ctx *ctx, struct fmd_mhd *mhd)
{
	struct fmd_mem_domain *domain;
	struct fmd_component *child;
	uint64_t total_size = 0;
	uint64_t base_addr = 0;
	char id[128];

	/* Sum up all LD pool memory sizes */
	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		total_size += ld->memory_size;
	}

	if (total_size == 0)
		return 0;

	/* Create domain (MHD = MemoryDomain per OCP model) */
	(void)snprintf(id, sizeof(id), "%.50s_domain0", mhd->base.id);
	domain = fmd_domain_create(id);
	if (!domain)
		return -1;

	domain->capacity = total_size;
	domain->remaining = total_size;

	fmd_topology_add(&ctx->topology, &domain->base, &mhd->base);

	/* Create one chunk per LD in the pool */
	fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		struct fmd_mem_chunk *chunk;

		(void)snprintf(id, sizeof(id), "%.50s_ld%u_chunk", mhd->base.id, ld->ld_id);
		chunk = fmd_chunk_create(id);
		if (!chunk)
			return -1;

		chunk->base_address = base_addr;
		chunk->size = ld->memory_size;
		chunk->interleaved = false;

		if (ld->memdev.volatile_capacity > 0 && ld->memdev.persistent_capacity == 0)
			strncpy(chunk->address_range_type, "Volatile",
				sizeof(chunk->address_range_type) - 1);
		else
			strncpy(chunk->address_range_type, "PMEM",
				sizeof(chunk->address_range_type) - 1);

		/* Set interleave sets to reference the LD's Memory resource */
		chunk->interleave_set_count = 1;
		strncpy(chunk->interleave_sets[0].memory_id, ld->base.id,
			sizeof(chunk->interleave_sets[0].memory_id) - 1);
		chunk->interleave_sets[0].size_mib = ld->memory_size / (1024ULL * 1024);

		fmd_topology_add(&ctx->topology, &chunk->base, &domain->base);
		domain->remaining -= chunk->size;

		base_addr += ld->memory_size;
	}

	fmd_dbg(ctx, "%s: created domain and %d chunks (%" PRIu64 " MB total)",
		mhd->base.id, mhd->total_lds, (uint64_t)(total_size / (1024ULL * 1024)));

	return 0;
}

