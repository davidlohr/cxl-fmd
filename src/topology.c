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
#include <errno.h>

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



/* The components fmd_rescan() says it added or removed */
static bool counted_change(const struct fmd_component *comp)
{
	switch (comp->type) {
	case FMD_COMPONENT_SWITCH:
	case FMD_COMPONENT_PORT:
	case FMD_COMPONENT_SLD:
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
	case FMD_COMPONENT_LD:
	case FMD_COMPONENT_HOST:
		return true;
	default:
		return false;
	}
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

	/*
	 * An Id names one component: lookup would find one of two and hide
	 * the other. A second is refused before anything is changed.
	 */
	if (hash_table_find(&topo->components, comp->id)) {
		pthread_rwlock_unlock(&topo->lock);
		return -EEXIST;
	}

	/* Set fabric pointer for direct access to root */
	comp->fabric = topo->root;

	/* Reached through the same endpoint as its parent, unless it has its own */
	if (!comp->ep)
		comp->ep = parent->ep;
	if (!comp->msg_limit)
		comp->msg_limit = parent->msg_limit;

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

	if (counted_change(comp))
		topo->added++;
	topo->generation++;

	pthread_rwlock_unlock(&topo->lock);

	return 0;
}

/*
 * Remove helper - removes from hash table recursively
 */
static void remove_from_hash(struct fmd_topology *topo, struct fmd_component *comp)
{
	if (counted_change(comp))
		topo->removed++;

	struct fmd_fabric *root = topo->root;
	struct fmd_component *child;

	fmd_for_each_child(comp, child) {
		remove_from_hash(topo, child);
	}

	hash_table_remove(&topo->components, comp);

	/* the statistics fmd_topology_add() keeps */
	switch (comp->type) {
	case FMD_COMPONENT_SWITCH:
		root->switch_count--;
		break;
	case FMD_COMPONENT_PORT:
		root->port_count--;
		break;
	case FMD_COMPONENT_SLD:
	case FMD_COMPONENT_MLD:
	case FMD_COMPONENT_MHD:
		root->device_count--;
		break;
	case FMD_COMPONENT_LD:
		root->ld_count--;
		break;
	case FMD_COMPONENT_DOMAIN:
		root->domain_count--;
		break;
	case FMD_COMPONENT_CHUNK:
		root->chunk_count--;
		break;
	case FMD_COMPONENT_HOST:
		root->host_count--;
		break;
	default:
		break;
	}
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
	remove_from_hash(topo, comp);

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
	chunk->backing_ld = -1;		/* not LD 0 */

	return chunk;
}

/*
 * The Id of a new chunk of @domain: {domain}_chunk{N}, N from the domain's
 * own counter, which only goes up, so that an Id a chunk had is not given
 * to another while the daemon runs (cxl-fmd's choice: Ids are not kept
 * over a restart). An Id some component has already is skipped.
 * Takes the topology read lock: not to be called with it held.
 */
static void generate_chunk_id(struct fmd_topology *topo, struct fmd_mem_domain *domain,
			      char *buf, size_t len)
{
	do
		snprintf(buf, len, "%s_chunk%u", domain->base.id, domain->next_chunk++);
	while (fmd_topology_find(topo, buf));
}

/*
 * Whether [a, a + as) and [b, b + bs) overlap, both sizes > 0: written
 * without the sums, which need not fit in 64 bits.
 */
static bool ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	return a < b ? b - a < as : a - b < bs;
}

/*
 * Check if [offset, offset + size) overlaps an existing chunk.
 */
static bool chunk_overlaps(struct fmd_mem_domain *domain, uint64_t offset, uint64_t size)
{
	struct fmd_component *child;

	fmd_for_each_child_type(&domain->base, child, FMD_COMPONENT_CHUNK) {
		struct fmd_mem_chunk *c = fmd_to_chunk(child);

		if (c->size && ranges_overlap(offset, size, c->base_address, c->size))
			return true;
	}

	return false;
}

/*
 * The lowest offset at which @size bytes lie within the domain and overlap
 * no chunk (first fit), or FMD_CHUNK_OFFSET_AUTO if there is none. The
 * chunks are in no order: a candidate overlapping one moves past its end,
 * and the walk starts again until no chunk is in the way. Each move goes
 * up, so it ends.
 */
static uint64_t find_free_offset(struct fmd_mem_domain *domain, uint64_t size)
{
	struct fmd_component *child;
	uint64_t offset = 0;
	bool moved;

	if (size > domain->capacity)
		return FMD_CHUNK_OFFSET_AUTO;

	do {
		moved = false;
		fmd_for_each_child_type(&domain->base, child, FMD_COMPONENT_CHUNK) {
			struct fmd_mem_chunk *c = fmd_to_chunk(child);

			if (!c->size || !ranges_overlap(offset, size, c->base_address, c->size))
				continue;
			if (__builtin_add_overflow(c->base_address, c->size, &offset))
				return FMD_CHUNK_OFFSET_AUTO;
			moved = true;
		}
		if (offset > domain->capacity - size)
			return FMD_CHUNK_OFFSET_AUTO;
	} while (moved);

	return offset;
}

struct fmd_mem_chunk *fmd_chunk_create_in_domain(
	struct fmd_topology *topo,
	struct fmd_mem_domain *domain,
	uint64_t size,
	uint64_t offset,
	const char *address_range_type,
	int backing_ld)
{
	struct fmd_mem_chunk *chunk;
	char chunk_id[FMD_MAX_ID_LEN];

	if (!topo || !domain || size == 0)
		return NULL;

	/* Check capacity */
	if (size > domain->remaining)
		return NULL;

	/* Auto-assign offset if not specified */
	if (offset == FMD_CHUNK_OFFSET_AUTO) {
		offset = find_free_offset(domain, size);
		if (offset == FMD_CHUNK_OFFSET_AUTO)
			return NULL;
	}

	/* Within the domain */
	if (offset > domain->capacity || size > domain->capacity - offset)
		return NULL;

	/* Check for overlap with existing chunks */
	if (chunk_overlaps(domain, offset, size))
		return NULL;

	/* Generate chunk ID */
	generate_chunk_id(topo, domain, chunk_id, sizeof(chunk_id));

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

	chunk->backing_ld = backing_ld;

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


void fmd_component_free(struct fmd_component *comp)
{
	if (!comp)
		return;

	/* Devices own their DCD capability block */
	switch (comp->type) {
	case FMD_COMPONENT_SLD:
		fmd_dcd_info_free(fmd_to_sld(comp)->dcd);
		break;
	case FMD_COMPONENT_MLD:
		fmd_dcd_info_free(fmd_to_mld(comp)->dcd);
		break;
	case FMD_COMPONENT_MHD:
		fmd_dcd_info_free(fmd_to_mhd(comp)->dcd);
		break;
	default:
		break;
	}

	fmd_log_free(comp);

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

			/* nothing is left pointing at what it was bound to */
			vppb->port = NULL;
			vppb->ld = NULL;

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
 * Add @comp under @parent for a device's memory resources; on an error,
 * logged, @comp is freed and the error returned.
 */
static int add_resource(struct fmd_ctx *ctx, struct fmd_component *comp,
			struct fmd_component *parent)
{
	int rc = fmd_topology_add(&ctx->topology, comp, parent);

	if (rc) {
		fmd_err(ctx, "%s: %s not added: %s", parent->id, comp->id,
			rc == -EEXIST ? "a component has that ID" : "error");
		fmd_component_free(comp);
	}
	return rc;
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
	int rc;

	if (sld->memory_size == 0)
		return 0;

	/* Create domain */
	(void)snprintf(id, sizeof(id), "%.50s_domain0", sld->base.id);
	domain = fmd_domain_create(id);
	if (!domain)
		return -1;

	domain->capacity = sld->memory_size;
	domain->remaining = sld->memory_size;

	rc = add_resource(ctx, &domain->base, &sld->base);
	if (rc)
		return rc;

	/* Create chunk for existing allocation */
	(void)snprintf(id, sizeof(id), "%.50s_chunk0", sld->base.id);
	chunk = fmd_chunk_create(id);
	if (!chunk)
		return -1;

	chunk->base_address = 0;
	chunk->size = sld->memory_size;

	if (sld->memdev.volatile_capacity > 0 && sld->memdev.persistent_capacity == 0)
		strncpy(chunk->address_range_type, "Volatile",
			sizeof(chunk->address_range_type) - 1);
	else
		strncpy(chunk->address_range_type, "PMEM",
			sizeof(chunk->address_range_type) - 1);

	rc = add_resource(ctx, &chunk->base, &domain->base);
	if (rc)
		return rc;
	domain->remaining -= chunk->size;

	fmd_dbg(ctx, "%s: created domain and chunk (%" PRIu64 " MB)",
		sld->base.id, (uint64_t)(sld->memory_size / (1024ULL * 1024)));

	return 0;
}

/*
 * The capacity of the domain of an MLD or Multi-Headed device @dev, and
 * what its LDs have allocated of it. The capacity is the device's Memory
 * Size (Get LD Info, struct fmd_mld and fmd_mhd), or if it is not known
 * the sum of the allocations, which is no more than what the device has
 * (cxl-fmd's choice). Allocations larger than the Memory Size, which the
 * device should not report, are taken to be what it has: the domain is
 * then as large as they are, with nothing left (cxl-fmd's choice).
 * @floor is the least capacity of a device of no known Memory Size: the
 * domain's, once it has one, so that what the LDs give back stays in it.
 * Returns 0, or -EOVERFLOW if the allocations do not add up in 64 bits.
 */
static int ld_pool_sizes(struct fmd_ctx *ctx, struct fmd_component *dev,
			 uint64_t floor, uint64_t *capacity, uint64_t *allocated)
{
	struct fmd_component *child;
	uint64_t pool;
	bool valid;

	*allocated = 0;
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		if (__builtin_add_overflow(*allocated, fmd_to_ld(child)->memory_size,
					   allocated)) {
			fmd_warn(ctx, "%s: LD allocations add up to more than "
				 "64 bits", dev->id);
			return -EOVERFLOW;
		}
	}

	if (dev->type == FMD_COMPONENT_MLD) {
		pool = fmd_to_mld(dev)->pool_capacity;
		valid = fmd_to_mld(dev)->pool_capacity_valid;
	} else {
		pool = fmd_to_mhd(dev)->pool_capacity;
		valid = fmd_to_mhd(dev)->pool_capacity_valid;
	}
	*capacity = valid ? pool : *allocated > floor ? *allocated : floor;
	if (*allocated > *capacity) {
		fmd_warn(ctx, "%s: LDs have %" PRIu64 " MiB allocated, more than "
			 "the Memory Size of %" PRIu64 " MiB: the domain is taken "
			 "to be what they have", dev->id, *allocated >> 20,
			 *capacity >> 20);
		*capacity = *allocated;
	}
	return 0;
}

/*
 * The domain of an MLD or Multi-Headed device @dev, whose capacity is
 * ld_pool_sizes()', and a chunk for each LD that has memory allocated, of
 * that allocation: a domain of which nothing is allocated has none.
 */
static int create_ld_memory_resources(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct fmd_mem_domain *domain;
	struct fmd_component *child;
	uint64_t capacity, allocated, base_addr = 0;
	char id[128];
	int rc, nchunks = 0;

	if (ld_pool_sizes(ctx, dev, 0, &capacity, &allocated))
		return 0;		/* no domain: nothing can be told of it */
	if (capacity == 0)
		return 0;

	/* Create domain (MLD or MHD = MemoryDomain per OCP model) */
	(void)snprintf(id, sizeof(id), "%.50s_domain0", dev->id);
	domain = fmd_domain_create(id);
	if (!domain)
		return -1;

	domain->capacity = capacity;
	domain->remaining = capacity;

	rc = add_resource(ctx, &domain->base, dev);
	if (rc)
		return rc;

	/* a chunk per LD with an allocation (each LD = MemoryChunk per OCP model) */
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		struct fmd_ld *ld = fmd_to_ld(child);
		struct fmd_mem_chunk *chunk;

		if (!ld->memory_size)
			continue;

		(void)snprintf(id, sizeof(id), "%.50s_ld%u_chunk", dev->id, ld->ld_id);
		chunk = fmd_chunk_create(id);
		if (!chunk)
			return -1;

		chunk->base_address = base_addr;
		chunk->size = ld->memory_size;

		if (ld->memdev.volatile_capacity > 0 && ld->memdev.persistent_capacity == 0)
			strncpy(chunk->address_range_type, "Volatile",
				sizeof(chunk->address_range_type) - 1);
		else
			strncpy(chunk->address_range_type, "PMEM",
				sizeof(chunk->address_range_type) - 1);

		chunk->backing_ld = ld->ld_id;

		rc = add_resource(ctx, &chunk->base, &domain->base);
		if (rc)
			return rc;
		domain->remaining -= chunk->size;	/* allocated <= capacity */
		nchunks++;

		base_addr += ld->memory_size;
	}

	fmd_dbg(ctx, "%s: created domain of %" PRIu64 " MB and %d chunks (%" PRIu64
		" MB)", dev->id, capacity >> 20, nchunks, allocated >> 20);

	return 0;
}

int fmd_create_mld_memory_resources(struct fmd_ctx *ctx, struct fmd_mld *mld)
{
	return create_ld_memory_resources(ctx, &mld->base);
}

int fmd_create_mhd_memory_resources(struct fmd_ctx *ctx, struct fmd_mhd *mhd)
{
	return create_ld_memory_resources(ctx, &mhd->base);
}

/* the lowest free range of @domain, [*off, *off + *len); false if none */
static bool first_free_range(struct fmd_mem_domain *domain, uint64_t *off,
			     uint64_t *len)
{
	struct fmd_component *child;
	uint64_t end;

	*off = find_free_offset(domain, 1);
	if (*off == FMD_CHUNK_OFFSET_AUTO)
		return false;
	end = domain->capacity;
	fmd_for_each_child_type(&domain->base, child, FMD_COMPONENT_CHUNK) {
		struct fmd_mem_chunk *c = fmd_to_chunk(child);

		if (c->size && c->base_address > *off && c->base_address < end)
			end = c->base_address;
	}
	*len = end - *off;
	return true;
}

/* a chunk of @size bytes at @offset of @domain, for @ld of @dev */
static int sync_add_chunk(struct fmd_ctx *ctx, struct fmd_component *dev,
			  struct fmd_mem_domain *domain, const struct fmd_ld *ld,
			  uint64_t offset, uint64_t size)
{
	struct fmd_mem_chunk *chunk;
	char id[FMD_MAX_ID_LEN];

	(void)snprintf(id, sizeof(id), "%.50s_ld%u_chunk", dev->id, ld->ld_id);
	if (fmd_topology_find(&ctx->topology, id))
		generate_chunk_id(&ctx->topology, domain, id, sizeof(id));
	chunk = fmd_chunk_create(id);
	if (!chunk)
		return -ENOMEM;
	chunk->base_address = offset;
	chunk->size = size;
	if (ld->memdev.volatile_capacity > 0 && ld->memdev.persistent_capacity == 0)
		strncpy(chunk->address_range_type, "Volatile",
			sizeof(chunk->address_range_type) - 1);
	chunk->backing_ld = ld->ld_id;
	return add_resource(ctx, &chunk->base, &domain->base);
}

/*
 * The chunks of @ld add up to its allocation: see fmd_ld_domain_sync().
 * The chunks a domain has are newest first (tree_add_child()).
 */
static int sync_ld_chunks(struct fmd_ctx *ctx, struct fmd_component *dev,
			  struct fmd_mem_domain *domain, const struct fmd_ld *ld)
{
	struct fmd_component *child, *next;
	uint64_t have = 0, off, len;
	int rc;

	fmd_for_each_child_type(&domain->base, child, FMD_COMPONENT_CHUNK) {
		struct fmd_mem_chunk *c = fmd_to_chunk(child);

		if (c->backing_ld == ld->ld_id && __builtin_add_overflow(have, c->size, &have))
			return -EOVERFLOW;
	}

	if (have > ld->memory_size) {
		uint64_t lose = have - ld->memory_size;

		fmd_info(ctx, "%s: LD %u has %" PRIu64 " MB less than its chunks: "
			 "taken off them", dev->id, ld->ld_id, lose >> 20);
		for (child = domain->base.first_child; child && lose; child = next) {
			struct fmd_mem_chunk *c;

			next = child->next_sibling;
			if (child->type != FMD_COMPONENT_CHUNK)
				continue;
			c = fmd_to_chunk(child);
			if (c->backing_ld != ld->ld_id)
				continue;
			if (c->size > lose) {
				fmd_topology_wrlock(&ctx->topology);
				c->size -= lose;
				fmd_topology_wrunlock(&ctx->topology);
				lose = 0;
			} else {
				lose -= c->size;
				fmd_info(ctx, "%s: chunk %s goes", dev->id, c->base.id);
				fmd_topology_remove(&ctx->topology, &c->base);
			}
		}
		return 0;
	}

	if (have < ld->memory_size) {
		uint64_t gain = ld->memory_size - have;

		fmd_info(ctx, "%s: LD %u has %" PRIu64 " MB more than its chunks: "
			 "a chunk of it", dev->id, ld->ld_id, gain >> 20);
		off = find_free_offset(domain, gain);
		if (off != FMD_CHUNK_OFFSET_AUTO)
			return sync_add_chunk(ctx, dev, domain, ld, off, gain);
		/* in pieces, as no free range is that large */
		while (gain && first_free_range(domain, &off, &len)) {
			if (len > gain)
				len = gain;
			rc = sync_add_chunk(ctx, dev, domain, ld, off, len);
			if (rc)
				return rc;
			gain -= len;
		}
		if (gain) {
			fmd_warn(ctx, "%s: no room in the domain for %" PRIu64
				 " MB of LD %u", dev->id, gain >> 20, ld->ld_id);
			return -ENOSPC;
		}
	}
	return 0;
}

int fmd_ld_domain_sync(struct fmd_ctx *ctx, struct fmd_component *dev)
{
	struct fmd_mem_domain *domain = NULL;
	struct fmd_component *child;
	uint64_t capacity, allocated;
	int rc, err = 0;

	if (!ctx || !dev || (dev->type != FMD_COMPONENT_MLD &&
			     dev->type != FMD_COMPONENT_MHD))
		return -EINVAL;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DOMAIN) {
		domain = fmd_to_domain(child);
		break;
	}
	if (!domain)
		return create_ld_memory_resources(ctx, dev);

	rc = ld_pool_sizes(ctx, dev, domain->capacity, &capacity, &allocated);
	if (rc)
		return rc;
	if (capacity != domain->capacity)
		fmd_info(ctx, "%s: domain of %" PRIu64 " MB, not %" PRIu64 " MB",
			 dev->id, capacity >> 20, domain->capacity >> 20);
	fmd_topology_wrlock(&ctx->topology);
	domain->capacity = capacity;
	fmd_topology_wrunlock(&ctx->topology);

	/* what LDs lost first, so that what others gained has the room */
	for (int pass = 0; pass < 2; pass++) {
		fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			uint64_t have = 0;
			struct fmd_component *c;

			fmd_for_each_child_type(&domain->base, c, FMD_COMPONENT_CHUNK)
				if (fmd_to_chunk(c)->backing_ld == ld->ld_id)
					have += fmd_to_chunk(c)->size;
			if ((pass == 0) != (have > ld->memory_size))
				continue;
			rc = sync_ld_chunks(ctx, dev, domain, ld);
			if (rc && !err)
				err = rc;
		}
	}

	fmd_topology_wrlock(&ctx->topology);
	domain->remaining = capacity - allocated;	/* allocated <= capacity */
	fmd_topology_wrunlock(&ctx->topology);
	return err;
}
