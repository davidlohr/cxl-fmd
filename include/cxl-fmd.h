/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon
 *
 * Core data structures and function declarations for managing
 * CXL devices via Redfish APIs.
 */

#ifndef CXL_FMD_H
#define CXL_FMD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#include <libcxlmi.h>
#include <microhttpd.h>
#include <jansson.h>

#include "generated/enums/resource.h"
#include "generated/enums/cxl_logical_device.h"
#include "generated/enums/pcie_device.h"

/*
 * Branch hints. A mock topology (--mock) is for development and tests: the
 * branches that only it takes are the unlikely ones.
 */
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

/*
 * Forward declarations
 */
struct fmd_ctx;
struct fmd_mock_fabric;
struct fmd_component;
struct fmd_fabric;
struct fmd_switch;
struct fmd_port;
struct fmd_vcs;
struct fmd_vppb;
struct fmd_sld;
struct fmd_mld;
struct fmd_ld;
struct fmd_mem_domain;
struct fmd_mem_chunk;
struct fmd_host;
struct fmd_mhd;
struct fmd_head;
struct fmd_dcd_info;
struct fmd_dc_region;
struct fmd_dc_extent;

/*
 * Logging
 */
enum fmd_log_level {
	FMD_LOG_ERROR = 0,
	FMD_LOG_WARN,
	FMD_LOG_INFO,
	FMD_LOG_DEBUG,
};

void fmd_log(struct fmd_ctx *ctx, enum fmd_log_level level,
	     const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define fmd_err(ctx, ...)   fmd_log(ctx, FMD_LOG_ERROR, __VA_ARGS__)
#define fmd_warn(ctx, ...)  fmd_log(ctx, FMD_LOG_WARN, __VA_ARGS__)
#define fmd_info(ctx, ...)  fmd_log(ctx, FMD_LOG_INFO, __VA_ARGS__)
#define fmd_dbg(ctx, ...)   fmd_log(ctx, FMD_LOG_DEBUG, __VA_ARGS__)

/*
 * String length limits
 */
#define FMD_MAX_ID_LEN 64   /* Max length for component IDs */

/*
 * CXL Component Types (matches identify.component_type)
 */
#define CXL_COMPONENT_SWITCH         0x00
#define CXL_COMPONENT_TYPE3          0x03
#define CXL_COMPONENT_GFD            0x04

/*
 * CXL capacity unit conversion (CCI uses 256MB granularity)
 */
#define CXL_CAPACITY_UNIT  (256ULL * 1024 * 1024)

/*
 * libcxlmi copies as many list entries into a response as the device says
 * it returned, which need not be as many as were asked for. Every response
 * whose entry count is a single byte gets room for the most that byte can
 * hold, and no more entries are read from it than were requested.
 */
/*
 * Message Payload limits, CXL r4.0 Section 8.2.10.1. A component takes
 * requests of up to its Maximum Supported Message Size (Identify, Table
 * 8-216) and limits its responses to its Response Message Limit (Table
 * 8-218). Both are 2^n bytes, from 256 to 1024. A command with a list in
 * either direction is asked a part at a time so that neither is exceeded.
 *
 * A command tunnelled with Tunnel Management Command (5300h) is carried in
 * the payload of another, after 4 bytes of tunnel header and its own 12-byte
 * CCI message header (Tables 7-39 and 7-40).
 */
#define FMD_CCI_MSG_MIN          256
#define FMD_CCI_MSG_MAX          1024
#define FMD_CCI_TUNNEL_OVERHEAD  16

#define FMD_CCI_LIST_MAX   UINT8_MAX
#define FMD_CCI_LIST_CLAMP(count, asked) \
	do { if ((count) > (asked)) (count) = (asked); } while (0)
#define CXL_TO_BYTES(x)    ((x) * CXL_CAPACITY_UNIT)
#define FMD_MIB            (1024ULL * 1024)

/*
 * Maximum Logical Devices per MLD
 */
#define FMD_MAX_LD_COUNT 16

/*
 * Multi-Headed Device (MHD) limits. These are cxl-fmd's own: CXL r4.0 sets
 * none on the number of heads or on the size of the LD pool, which "may
 * support more than 16 LDs" (Section 2.5.1). Of a device with more, only
 * these many are kept.
 */
#define FMD_MAX_HEADS    8      /* Max heads per MHD */
#define FMD_MAX_LD_POOL  64     /* Max LDs in MHD pool (can exceed 16) */

/*
 * Dynamic Capacity Device (DCD) limits (CXL r4.0 Section 7.6.7.6)
 */
#define FMD_MAX_DC_REGIONS 8      /* DC regions per host */
#define FMD_DC_TAG_LEN     16     /* Extent tag is a 16-byte UUID */
#define FMD_DC_LD_BITMAP_BYTES 32  /* LD 0..255 in the 5608h bitmaps (Table 7-78) */
#define FMD_DC_MAX_HOST_ID 255    /* highest Host ID those bitmaps can name */
#define FMD_DC_REGION_NONE 0xFF   /* extent lies in no region of its host */

/*
 * Hash table size for component lookup
 */
#define FMD_HASH_BUCKETS 256

/*
 * Component Type System
 * Hierarchical tree model for CXL topology:
 *
 *   fmd_fabric (Root)
 *   ├── fmd_switch
 *   │   └── fmd_port (DSP) → fmd_sld or fmd_mld or fmd_mhd or fmd_switch
 *   ├── fmd_sld/fmd_mld (direct-attached)
 *   │   └── fmd_mem_domain
 *   │       └── fmd_mem_chunk
 *   └── fmd_mhd (multi-headed device)
 *       ├── fmd_head (0..N heads/ports)
 *       ├── fmd_ld (0..M pool LDs)
 *       └── fmd_mem_domain
 *
 * MLD/MHD children:
 *   fmd_mld or fmd_mhd
 *   ├── fmd_ld (0..N logical devices)
 *   ├── fmd_head (MHD only: CXL ports to hosts)
 *   └── fmd_mem_domain
 *
 * fmd_ld serves both MLD and MHD:
 * - Child of fmd_mld: traditional MLD LD (ld_id is what host sees)
 * - Child of fmd_mhd: LD pool entry (ld_id is pool index; visible_ld_id is
 *   the head-local LD-ID a host sees, FMD_LD_NONE when not known)
 */

/*
 * Component type enumeration
 */
enum fmd_component_type {
	FMD_COMPONENT_ROOT = 0,      /* Fabric root */
	FMD_COMPONENT_SWITCH,        /* CXL Switch */
	FMD_COMPONENT_PORT,          /* Switch DSP (downstream port) */
	FMD_COMPONENT_SLD,           /* Type 3 Single Logical Device */
	FMD_COMPONENT_MLD,           /* Type 3 Multi-Logical Device */
	FMD_COMPONENT_LD,            /* Logical Device (child of MLD) */
	FMD_COMPONENT_MHD,           /* Multi-Headed Device */
	FMD_COMPONENT_HEAD,          /* Head on MHD (CXL port) */
	FMD_COMPONENT_DOMAIN,        /* Memory Domain */
	FMD_COMPONENT_CHUNK,         /* Memory Chunk */
	FMD_COMPONENT_HOST,          /* Host system (initiator) */
	FMD_COMPONENT_DC_REGION,     /* DC region (per host, child of SLD/LD) */
	FMD_COMPONENT_DC_EXTENT,     /* DC extent (device-wide, child of memdev) */
};

/*
 * Base component structure - all topology nodes embed this
 */
struct fmd_component {
	char id[FMD_MAX_ID_LEN];             /* Unique identifier */
	enum fmd_component_type type;        /* Component type discriminator */

	/* Tree navigation */
	struct fmd_component *parent;        /* Parent node (NULL for root) */
	struct fmd_component *first_child;   /* Head of children list */
	struct fmd_component *next_sibling;  /* Next child of same parent */

	/* Direct pointer to fabric root (set by fmd_topology_add) */
	struct fmd_fabric *fabric;

	/*
	 * libcxlmi endpoint that commands for this component are sent on
	 * (NULL for mock, and for components with no CCI such as hosts).
	 * Set on the component discovered from an endpoint before it is
	 * added to the topology; everything added beneath it inherits it.
	 */
	struct cxlmi_endpoint *ep;

	/*
	 * Largest Message Payload, in bytes, of a request to this component
	 * or of a response from it; 0 if not known. See fmd_cci_msg_limit().
	 * Inherited like ep, less the tunnel overhead where a component is
	 * reached through one more Tunnel Management Command than its parent.
	 */
	uint16_t msg_limit;

	/*
	 * The event records read from this component, or for an LD from its
	 * MLD, kept for the Redfish log service (src/event_log.c). NULL
	 * until there is one.
	 */
	struct fmd_log *log;

	/* Hash table linkage for O(1) lookup, hash by string ID */
	uint32_t hash;
	struct fmd_component *hash_next; /* handle collisions */
};

/*
 * Container-of macro for type casting
 */
#define fmd_container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* Type casting macros */
#define fmd_to_fabric(c)   fmd_container_of(c, struct fmd_fabric, base)
#define fmd_to_switch(c)   fmd_container_of(c, struct fmd_switch, base)
#define fmd_to_port(c)     fmd_container_of(c, struct fmd_port, base)
#define fmd_to_sld(c)      fmd_container_of(c, struct fmd_sld, base)
#define fmd_to_mld(c)      fmd_container_of(c, struct fmd_mld, base)
#define fmd_to_ld(c)       fmd_container_of(c, struct fmd_ld, base)
#define fmd_to_domain(c)   fmd_container_of(c, struct fmd_mem_domain, base)
#define fmd_to_chunk(c)    fmd_container_of(c, struct fmd_mem_chunk, base)
#define fmd_to_host(c)     fmd_container_of(c, struct fmd_host, base)
#define fmd_to_mhd(c)      fmd_container_of(c, struct fmd_mhd, base)
#define fmd_to_head(c)     fmd_container_of(c, struct fmd_head, base)
#define fmd_to_dc_region(c) fmd_container_of(c, struct fmd_dc_region, base)
#define fmd_to_dc_extent(c) fmd_container_of(c, struct fmd_dc_extent, base)

/* Tree iteration macros */
#define fmd_for_each_child(parent, child) \
	for ((child) = (parent)->first_child; (child); (child) = (child)->next_sibling)

#define fmd_for_each_child_type(parent, child, ctype) \
	for ((child) = (parent)->first_child; (child); (child) = (child)->next_sibling) \
		if ((child)->type == (ctype))

/* The first child of @parent of type @ctype, or NULL */
static inline struct fmd_component *
fmd_first_child_type(struct fmd_component *parent, enum fmd_component_type ctype)
{
	struct fmd_component *child;

	fmd_for_each_child_type(parent, child, ctype)
		return child;
	return NULL;
}

/*
 * Iterate all components of a specific type in the topology hash table.
 * Usage:
 *   struct fmd_component *comp;
 *   fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
 *       struct fmd_switch *sw = fmd_to_switch(comp);
 *       // ...
 *   }
 *
 * Note: Caller must hold topology read lock.
 */
#define fmd_for_each_type(topo, comp, ctype) \
	for (int _bucket = 0; _bucket < FMD_HASH_BUCKETS; _bucket++) \
		for ((comp) = (topo)->components.buckets[_bucket]; \
		     (comp); (comp) = (comp)->hash_next) \
			if ((comp)->type == (ctype))

/* Check if component is a memory device (SLD, MLD, or MHD) */
static inline bool fmd_is_memdev(struct fmd_component *c)
{
	return c && (c->type == FMD_COMPONENT_SLD ||
		     c->type == FMD_COMPONENT_MLD ||
		     c->type == FMD_COMPONENT_MHD);
}

/*
 * An SLD on a switch port, as the switch reports it. It has the switch's
 * endpoint, inherited from the port, and cannot be reached through it:
 * "Valid targets for the tunneled commands include switch MLD Ports, valid
 * LDs within an MLD, and the LD Pool CCI in a Multi-Headed device"
 * (Section 7.6.7.3.1), and "It is prohibited for switches to support any
 * commands from the Memory Device Command Set" (Table 8-308, note 1).
 * Nothing is sent for it.
 */
static inline bool fmd_sld_behind_switch(const struct fmd_component *c)
{
	return c && c->type == FMD_COMPONENT_SLD && c->parent &&
	       c->parent->type == FMD_COMPONENT_PORT;
}

/* Check if component is a Multi-Headed Device */
static inline bool fmd_is_mhd(struct fmd_component *c)
{
	return c && c->type == FMD_COMPONENT_MHD;
}


/*
 * Fabric root - top-level container for the CXL topology
 *
 * One fabric holds everything discovered, from any number of libcxlmi
 * endpoints. The endpoint to reach a component is on the component
 * itself (base.ep), not here.
 */
struct fmd_fabric {
	struct fmd_component base;

	char description[256];

	/* Statistics */
	int switch_count;
	int device_count;
	int port_count;
	int ld_count;
	int domain_count;
	int chunk_count;
	int host_count;
};


/*
 * Link status
 */
enum fmd_link_status {
	FMD_LINK_DOWN = 0,
	FMD_LINK_UP,
	FMD_LINK_TRAINING,
};

/*
 * Port state (from FMAPI Get Physical Port State)
 */
enum fmd_port_state {
	FMD_PORT_DISABLED = 0,
	FMD_PORT_BIND_IN_PROGRESS,
	FMD_PORT_UNBIND_IN_PROGRESS,
	FMD_PORT_DSP,
	FMD_PORT_USP,
	FMD_PORT_FABRIC,		/* 5h, Fabric Port of a PBR switch */
	FMD_PORT_RESERVED,		/* any other encoding */
};

/*
 * Connected device type (from FMAPI Get Physical Port State)
 */
enum fmd_conn_dev_type {
	FMD_CONN_NONE   = 0x0,
	FMD_CONN_PCIE   = 0x1,       /* PCIe device (not CXL) */
	FMD_CONN_TYPE1  = 0x2,       /* CXL.cache */
	FMD_CONN_TYPE2  = 0x3,       /* CXL.cache + CXL.mem */
	FMD_CONN_SLD    = 0x4,       /* Type 3 SLD */
	FMD_CONN_MLD    = 0x5,       /* Type 3 MLD */
	FMD_CONN_PBR    = 0x6,       /* PBR component */
};

/*
 * VCS Configuration Limits (CXL spec)
 */
#define FMD_MAX_VCS      8       /* Max VCS per switch */
#define FMD_MAX_VPPB     32      /* Max vPPBs per VCS */

/*
 * vPPB Binding Status (matches DMTF VirtualPCI2PCIBridge.vPPBStatusTypes)
 */
enum fmd_vppb_binding_status {
	FMD_VPPB_UNBOUND = 0,        /* vPPB is not bound */
	FMD_VPPB_BUSY,               /* Binding/unbinding in progress */
	FMD_VPPB_BOUND_PHYSICAL_PORT,/* Bound to a physical port (SLD) */
	FMD_VPPB_BOUND_LD,           /* Bound to a logical device (MLD LD) */
	FMD_VPPB_BOUND_PBR,          /* Bound to a PBR ID (fabric) */
};

/*
 * Bound LD ID of a vPPB whose Binding Status is not 03h: "Valid only when
 * vPPB[Start vPPB] Binding Status is 03h; otherwise, the value is FFh"
 * (CXL r4.0 Table 7-34).
 */
#define FMD_VPPB_NO_LD   0xFF

/*
 * vPPB Unbind Options (CXL r4.0 Section 7.6.7.2.3)
 */
enum fmd_unbind_option {
	FMD_UNBIND_WAIT_FOR_CLEAN = 0,   /* Wait for host to perform clean removal */
	FMD_UNBIND_MANAGED_HOT_REMOVE,   /* Managed Hot-Remove */
	FMD_UNBIND_SURPRISE_HOT_REMOVE,  /* Surprise Hot-Remove */
};

/*
 * Virtual PCI-to-PCI Bridge (vPPB) - Configuration State
 *
 * A vPPB is a binding point within a VCS. It references physical
 * components (ports, LDs) but is not part of the component tree.
 * Discovered via FM-API Get VCS Binding Info (5201h).
 */
struct fmd_vppb {
	uint8_t vppb_id;
	enum fmd_vppb_binding_status binding_status;

	/* Binding target (interpretation depends on binding_status) */
	uint8_t bound_port_id;       /* Physical port number */
	uint8_t bound_ld_id;         /* LD ID within MLD if BoundLD, else FMD_VPPB_NO_LD */
	uint16_t bound_pbr_id;       /* PBR ID (if BoundPID) */

	/* Resolved references to physical components */
	struct fmd_port *port;       /* Resolved port pointer */
	struct fmd_ld *ld;           /* Resolved LD pointer (MLD only) */

	/*
	 * Mock only: a bind or unbind that the mock switch is carrying out
	 * in the background, and what the vPPB becomes when it is done.
	 */
	int64_t mock_done_ms;        /* CLOCK_MONOTONIC; 0 = nothing under way */
	enum fmd_vppb_binding_status mock_next_status;
	uint8_t mock_next_port_id;
	uint8_t mock_next_ld_id;
};

/*
 * Virtual CXL Switch (VCS) - Configuration State
 *
 * A VCS is a logical partition of the physical switch presented to
 * a single host. Discovered via FM-API Get Virtual CXL Switch Info (5200h).
 * This is configuration state, not a component tree node.
 */
struct fmd_vcs {
	uint8_t vcs_id;              /* VCS identifier (0-based) */
	uint8_t state;               /* VCS state from FM-API */
	uint8_t hdm_decoders;        /* Number of HDM decoders */
	uint8_t upstream_port_id;    /* USP this VCS is bound to */
	bool stale;                  /* the last fmd_vcs_refresh() could not
					read it: this is what was known before */

	/* vPPB bindings for this VCS */
	uint8_t num_vppbs;
	struct fmd_vppb vppbs[FMD_MAX_VPPB];
};

/*
 * CXL Switch
 *
 * Children: fmd_port nodes (both USPs and DSPs in component tree)
 * Uses fabric's endpoint for CCI communication.
 *
 * VCS configuration is stored separately from the component tree.
 * Physical topology: switch → ports → devices
 * Logical config: switch.vcs[] → vppbs[] → references to ports/LDs
 *
 * USP identification: Ports with state == FMD_PORT_USP are upstream ports.
 * Each VCS references its USP via upstream_port_id.
 */
struct fmd_switch {
	struct fmd_component base;

	/* CCI Identify (0001h) response */
	struct cxlmi_cmd_identify_rsp identify;

	/* FM-API Identify Switch Device (5100h) response */
	struct cxlmi_cmd_fmapi_identify_sw_device_rsp sw_identify;

	/* Port counts */
	uint8_t num_usps;
	uint8_t num_dsps;

	/* VCS configuration (from FM-API 5200h/5201h) */
	uint8_t num_vcs;
	uint8_t max_vcs;
	struct fmd_vcs vcs[FMD_MAX_VCS];

	/*
	 * Mock only: how long the mock switch takes over a bind or unbind,
	 * which it then does in the background as a switch does. 0: at once.
	 */
	unsigned int mock_bind_time_ms;

	/*
	 * Mock only: the return code the mock switch gives Bind vPPB and
	 * Unbind vPPB (CXLMI_RET_*: Success, Invalid Input, Internal Error,
	 * Busy or Unsupported), and the opcode and return code of the last
	 * one it ran in the background, which Background Operation Status
	 * reports.
	 */
	uint16_t mock_bind_result;
	uint16_t mock_unbind_result;
	uint16_t mock_bg_opcode;
	uint16_t mock_bg_rc;
	uint16_t mock_record_handle;
};

/*
 * What the return code of a command that tells whether a device has a
 * feature says (fmd_probe_result()). Only Unsupported, "The command is not
 * supported", and Unsupported Mailbox or CCI, "The command is not supported
 * on the mailbox or CCI it was issued on" (CXL r4.0 Table 8-208), say that
 * it has not. Retry Required is "a temporary error", and Busy is a
 * background operation that is still running: they say nothing of what the
 * device is. That every other return code is taken as one that does not
 * either is cxl-fmd's choice, as is how a failure to reach the device is
 * read: as one of the moment, unless the transport will never carry the
 * command (see fmd_probe_result()).
 */
enum fmd_probe {
	FMD_PROBE_OK,		/* Success */
	FMD_PROBE_ABSENT,	/* the device does not have it */
	FMD_PROBE_TRANSIENT,	/* not known for now: asked again later */
};

/*
 * @rc, the return of a libcxlmi command (0, a CCI return code, or -1 with
 * errno set), read as fmd_probe says. Clear errno before the command, and
 * call it before anything else can change errno.
 */
enum fmd_probe fmd_probe_result(int rc);

/*
 * A discovery step that failed for the moment, tried again with backoff
 * and up to a cap. The specification says only "An optional single retry
 * may resolve the issue" (Retry Required, Table 8-208): the cadence and
 * the cap are cxl-fmd's. After the first failure the step is tried the
 * next time it can be (a --rescan, or a read of a switch's ports), then
 * with 1, 3, 7 and 15 such times let pass between tries, and after
 * FMD_RETRY_MAX failures not again until cxl-fmd is restarted (or, for a
 * switch port, until the port reports another kind of device, or none).
 */
#define FMD_RETRY_MAX	8

struct fmd_retry {
	uint8_t failures;	/* in a row */
	uint8_t skip;		/* times left to let pass before the next try */
};

/* Whether to try now: not while backing off, and not once given up */
bool fmd_retry_due(struct fmd_retry *r);
/* A try failed for the moment; returns true when it was the last one */
bool fmd_retry_failed(struct fmd_retry *r);
static inline void fmd_retry_reset(struct fmd_retry *r)
{
	r->failures = 0;
	r->skip = 0;
}

/*
 * Switch Port (DSP - Downstream Switch Port)
 *
 * Children: fmd_sld, fmd_mld
 */
struct fmd_port {
	struct fmd_component base;

	uint8_t port_number;
	enum fmd_port_state state;
	enum fmd_link_status link_status;
	enum fmd_conn_dev_type conn_dev_type;

	/* CXL mode information (from Get Physical Port State) */
	uint8_t conn_dev_cxl_ver;         /* Connected Device Mode */
	uint8_t port_cxl_ver_bitmask;     /* Supported CXL Modes */

	/* Link properties */
	uint8_t max_link_width;
	uint8_t negotiated_link_width;
	uint8_t supported_link_speeds;    /* Supported Link Speeds Vector */
	/*
	 * Max Link Speed and Current Link Speed, encoded as in the PCIe Link
	 * Capabilities and Link Status registers: 1 = 2.5 GT/s up to
	 * 6 = 64 GT/s, 0 = not known.
	 */
	uint8_t max_link_speed;
	uint8_t current_link_speed;

	/* MLD binding support */
	uint8_t supported_ld_count;

	/* The head of a Multi-Headed device on this port, if that is what is */
	struct fmd_head *head;

	/*
	 * The device on the port could not be discovered for the moment:
	 * when it is asked again (fmd_retry).
	 */
	struct fmd_retry dev_retry;
};

/*
 * Type 3 Single Logical Device
 *
 * Simple memory device with a single logical device.
 * Children: fmd_mem_domain
 */
struct fmd_sld {
	struct fmd_component base;

	/* CCI Identify (0001h) response */
	struct cxlmi_cmd_identify_rsp identify;

	/* Memdev Identify (4000h) response */
	struct cxlmi_cmd_memdev_identify_rsp memdev;

	/* Get Partition Info (4100h) response */
	struct cxlmi_cmd_memdev_get_partition_info_rsp partition;

	/* Get Health Info (4200h) response, if health_valid */
	struct cxlmi_cmd_memdev_get_health_info_rsp health;
	bool health_valid;		/* 4200h was answered Success */

	/* Supported CCI commands bitmask */
	uint64_t supported_cmds;

	/* Memory sizes (derived from memdev response) */
	uint64_t memory_size;
	uint64_t volatile_size;
	uint64_t persistent_size;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
	/* Get DCD Info failed for the moment: asked again (fmd_dcd_retry_pending()) */
	bool dcd_pending;
	struct fmd_retry dcd_retry;
};


/*
 * Type 3 Multi-Logical Device
 *
 * Complex memory device with multiple logical devices, VCS, and QoS.
 * Children: fmd_ld nodes, fmd_mem_domain
 *
 * MLDs require tunneling when behind a switch (for MLD component commands).
 * Uses fabric's endpoint for CCI communication.
 */
struct fmd_mld {
	struct fmd_component base;

	/* CCI Identify (0001h) response */
	struct cxlmi_cmd_identify_rsp identify;

	/* Switch port number if behind switch (-1 = direct-attached) */
	int16_t switch_port;

	/* Supported CCI commands bitmask */
	uint64_t supported_cmds;

	/* Get LD Info (5400h) response data */
	uint8_t ld_count;
	uint8_t max_ld_count;
	uint8_t memory_granularity;          /* 256MB units */
	uint8_t qos_telemetry_capability;    /* Bit 0: Egress Port Backpressure */
	uint16_t total_memory_ranges;

	/*
	 * Get LD Info (5400h) Memory Size, "Total device memory capacity"
	 * (CXL r4.0 Table 7-46), in bytes (cxl-fmd's reading: the table
	 * gives no unit). Not valid if not read, or 0: the domain is then
	 * as large as the LDs' allocations (fmd_create_mld_memory_resources()).
	 */
	uint64_t pool_capacity;
	bool pool_capacity_valid;

	/* QoS Control (5403h) response data */
	struct {
		bool enabled;                           /* Egress Port Congestion Enable */
		bool temp_throughput_reduction_enabled; /* Temporary Throughput Reduction */
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
		uint8_t backpressure_sample_interval;
		uint16_t reqcmp_basis;                   /* ReqCmpBasis */
		uint16_t completion_collection_interval;
	} qos_control;

	/* QoS Status (5405h) response data */
	struct {
		bool backpressure_avg_valid;
		uint8_t backpressure_avg_pct;
	} qos_status;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
	/* Get DCD Info failed for the moment: asked again (fmd_dcd_retry_pending()) */
	bool dcd_pending;
	struct fmd_retry dcd_retry;
};


/*
 * Logical Device - child of MLD or MHD
 *
 * Represents one logical partition within an MLD or MHD.
 * All LDs (LD-0 through LD-15) are host-bindable.
 *
 * For MLD (parent is fmd_mld):
 * - ld_id is the LD identifier (0 to ld_count-1)
 * - binding_point_id tracks vPPB binding for switch-attached MLDs
 * - visible_ld_id equals ld_id (no translation needed)
 *
 * For MHD (parent is fmd_mhd):
 * - ld_id is the pool index (device-wide, 0 to total_lds-1)
 * - binding_point_id is the head ID this LD is mapped to
 * - visible_ld_id is the head-local LD-ID (what host sees, 0-15), or
 *   FMD_LD_NONE: "The MH-MLD maps the LD-IDs received at a head to the
 *   device-wide LD index in the MH-MLD's LD pool" (CXL r4.0 Section
 *   2.5.1), and no command reports that map, so discovery never knows it.
 *   Only a mock topology gives one.
 *
 * Note: The FM-owned LD (FMLD) at LD-ID FFFFh is separate from these
 * host-bindable LDs and is accessed implicitly via MLD component commands.
 * See CXL r4.0 Section 7.6.7.4.
 *
 * LDs always require MLD tunneling. If parent MLD is behind a switch,
 * combined switch+MLD tunneling is needed.
 * Uses fabric's endpoint, tunnel info derived from parent MLD/MHD.
 */
struct fmd_ld {
	struct fmd_component base;

	uint8_t ld_id;                       /* MLD: LD-ID (0-15), MHD: pool index */

	/* Host binding/mapping state */
	uint8_t binding_point_id;            /* vPPB ID (VCS) or head ID (MHD), 0xFF = unbound */
	uint16_t visible_ld_id;              /* LD-ID as host sees it, or FMD_LD_NONE */

	/* From Get LD Allocations (5401h) */
	uint64_t memory_size;
	uint64_t alloc_range2;               /* Range 2 Allocation Multiplier */
	bool alloc_unknown;                  /* see fmd_ld_alloc_set() */
	uint64_t range_start;
	uint64_t range_length;

	/* From Get QoS Allocated BW (5406h) */
	uint8_t qos_allocated_bw;            /* Fraction of 256 */

	/* From Get QoS BW Limit (5408h) */
	uint8_t qos_limit_bw;                /* Fraction of 256 */

	/* Supported CCI commands bitmask (from CEL) */
	uint64_t supported_cmds;

	/* From tunneled Identify (0001h) */
	struct cxlmi_cmd_identify_rsp identify;

	/* From tunneled Memdev Identify (4000h) */
	struct cxlmi_cmd_memdev_identify_rsp memdev;

	/* From tunneled Get Partition Info (4100h) */
	struct cxlmi_cmd_memdev_get_partition_info_rsp partition;

	/* From tunneled Get Health Info (4200h), if health_valid */
	struct cxlmi_cmd_memdev_get_health_info_rsp health;
	bool health_valid;		/* 4200h was answered Success */

	/* From Get Host DC Region Configuration (5601h) trailer, DCD only */
	uint32_t dc_extents_supported;
	uint32_t dc_extents_available;
	uint32_t dc_tags_supported;
	uint32_t dc_tags_available;

	/* The host's last Get DC Region Extent Lists (5603h) read, DCD only */
	uint32_t dc_ext_gen;                 /* Extent List Generation Number */
	uint8_t  dc_sync;                    /* enum fmd_dc_sync */
	int      dc_sync_err;                /* why it is FMD_DC_SYNC_STALE */
};


/*
 * Head Type - How the head presents to its host
 */
enum fmd_head_type {
	FMD_HEAD_SLD = 0,        /* Head presents as single-LD */
	FMD_HEAD_MLD,            /* Head presents as multi-LD */
};

/*
 * Head State
 */
enum fmd_head_state {
	FMD_HEAD_DISABLED = 0,
	FMD_HEAD_ENABLED,
	FMD_HEAD_ERROR,
};

/*
 * Head - CXL port on a Multi-Headed Device
 *
 * Each head represents a CXL upstream port connecting to a host.
 * - MH-SLD: 1:1 mapping between heads and LDs
 * - MH-MLD: Multiple LDs mapped per head (1-16 LDs each)
 *
 * The mapping from pool LDs to this head is tracked in fmd_ld:
 * - fmd_ld.binding_point_id == head_id indicates LD is mapped here
 * - fmd_ld.visible_ld_id is the head-local LD-ID the host sees, if known
 *
 * Child of fmd_mhd.
 */
struct fmd_head {
	struct fmd_component base;

	uint8_t head_id;                     /* 0 to num_heads-1 */
	/*
	 * Port Number of Get Head Info (Table 7-64). It is what the LD Map
	 * of Get Multi-Headed Info names a head by: "Port number of the head
	 * to which each LD is assigned" (Table 7-61). Not the head's index.
	 */
	uint8_t port_number;
	bool port_number_valid;
	enum fmd_head_type presentation;     /* How this head appears to host */
	enum fmd_head_state state;

	/* For MH-MLD presentation: count of LDs mapped to this head */
	uint8_t ld_count;                    /* Number of LDs assigned to head */

	/* Link properties (from Get MHD Info) */
	uint8_t max_link_width;
	uint8_t negotiated_link_width;

	/* Host attachment info */
	char attached_host_id[FMD_MAX_ID_LEN]; /* Connected host, if known */

	/*
	 * The switch port this head is on, NULL if it is on a host directly.
	 * A head is a CXL port of the device (CXL r4.0 Section 2.5), so each
	 * head has a link of its own: the heads of one device are on
	 * different ports, of one switch or of several.
	 */
	struct fmd_port *port;
};

/*
 * What a CCI that reaches a Multi-Headed device is, as cxl-fmd takes it.
 *
 * "Multi-Headed Devices shall expose a dedicated Component Command
 * Interface (CCI), the LD Pool CCI, for management of all LDs within the
 * device. The LD Pool CCI may be exposed as an MCTP-based CCI or can be
 * accessed via the Tunnel Management Command command through a head's
 * Mailbox CCI" (CXL r4.0 Section 2.5), and "The FMLD within each head of
 * an MH-MLD shall expose and manage only the LDs that are mapped to that
 * head" (Section 2.5.1). No command says which a CCI is: the role is
 * inferred from what the CCI answers (see the roles).
 */
enum fmd_cci_role {
	FMD_CCI_UNKNOWN = 0,	/* the device's, by its serial number; no more known */
	/*
	 * The LD Pool CCI: it answered Get Multi-Headed Info (5500h) with
	 * no tunnel. The MHD command set "is required on the LD Pool CCI"
	 * (Section 7.6.7.5); that it is not answered elsewhere is cxl-fmd's
	 * inference.
	 */
	FMD_CCI_POOL,
	/*
	 * A head's Mailbox CCI, through which the LD Pool CCI is reached
	 * with the Tunnel Management Command, Target Type 1 (Section
	 * 7.6.7.3.1). Discovery finds none (no tunnelled probe is sent).
	 */
	FMD_CCI_HEAD_MAILBOX,
	/*
	 * The FM-owned LD of a head that presents an MLD: it answered Get LD
	 * Info (5400h) and not Get Multi-Headed Info. It sees only that
	 * head's LDs, by head-local LD-IDs (Section 2.5.1), and is not sent
	 * commands for the pool.
	 */
	FMD_CCI_HEAD_FMLD,
};

/*
 * One way to reach a device: an endpoint, and the tunnel through it, if
 * any, to the CCI whose role this is. A switch-attached head is reached
 * on the switch's endpoint through its port.
 */
struct fmd_route {
	struct cxlmi_endpoint *ep;
	enum fmd_cci_role role;
	/* the tunnel from ep to the CCI: level 0 for none */
	struct cxlmi_tunnel_info tunnel;
	int head;			/* index of the head the CCI is of; -1 if not known */
	uint16_t msg_limit;		/* as fmd_component.msg_limit */
};

#define FMD_MAX_ROUTES 8

/*
 * Multi-Headed Device (MHD)
 *
 * A Type 3 device with multiple CXL upstream ports (heads), allowing
 * multiple hosts to share the device's memory. Can present as:
 * - MH-SLD: Each head sees one LD (1:1 head-to-LD mapping)
 * - MH-MLD: Each head sees multiple LDs (up to 16 per head)
 *
 * Uses LD Pool CCI for device-wide LD management.
 * Individual heads may use head-local CCIs for binding.
 *
 * Children: fmd_head nodes, fmd_ld nodes (pool LDs), fmd_mem_domain
 */
struct fmd_mhd {
	struct fmd_component base;

	/* CCI Identify (0001h) response */
	struct cxlmi_cmd_identify_rsp identify;

	/* Supported CCI commands bitmask */
	uint64_t supported_cmds;

	/*
	 * The CCIs the device was found on, each with its role. base.ep is
	 * the one discovery read the device on. See fmd_mhd_pool_route().
	 */
	struct fmd_route routes[FMD_MAX_ROUTES];
	int nroutes;

	/* Multi-Head configuration (from Get MHD Info or identify) */
	uint8_t num_heads;                   /* Number of heads (CXL ports) */
	uint8_t max_heads;                   /* Maximum supported heads */
	bool dynamic_capacity;               /* Supports dynamic capacity */

	/* LD Pool configuration */
	uint8_t total_lds;                   /* Total LDs in pool */
	uint8_t max_lds;                     /* Maximum pool size */
	uint8_t memory_granularity;          /* 256MB units */

	/*
	 * Get LD Info (5400h) Memory Size on the LD Pool CCI, as for an MLD
	 * (struct fmd_mld). Not valid if the LD Pool CCI does not answer it:
	 * nothing requires it to (cxl-fmd's reading of Section 7.6.7.5).
	 */
	uint64_t pool_capacity;
	bool pool_capacity_valid;

	/* QoS capability */
	uint8_t qos_telemetry_capability;    /* Bit 0: Egress Port Backpressure */

	/* QoS Control (device-wide, from Get QoS Control 5403h) */
	struct {
		bool enabled;
		bool temp_throughput_reduction_enabled;
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
		uint8_t backpressure_sample_interval;
		uint16_t reqcmp_basis;
		uint16_t completion_collection_interval;
	} qos_control;

	/* QoS Status (device-wide, from Get QoS Status 5405h) */
	struct {
		bool backpressure_avg_valid;
		uint8_t backpressure_avg_pct;
	} qos_status;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
	/* Get DCD Info failed for the moment: asked again (fmd_dcd_retry_pending()) */
	bool dcd_pending;
	struct fmd_retry dcd_retry;
};


/*
 * Memory Domain - addressable memory region
 *
 * Child of fmd_sld or fmd_mld.
 * Children: fmd_mem_chunk
 */
struct fmd_mem_domain {
	struct fmd_component base;

	uint64_t capacity;
	uint64_t remaining;

	/* the number in the Id of the next chunk a POST makes: only goes up */
	unsigned int next_chunk;

	/* QoS settings */
	struct {
		bool enabled;
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
	} qos;
};

/*
 * Memory Chunk - allocated memory region
 *
 * Child of fmd_mem_domain.
 */
struct fmd_mem_chunk {
	struct fmd_component base;

	uint64_t base_address;
	uint64_t size;
	char address_range_type[16];       /* "PMEM", "Volatile", or "" if not known */
	/*
	 * The LD whose allocation the chunk is part of: an LD-ID of an MLD,
	 * a pool index of a Multi-Headed device, -1 for none (an SLD's
	 * chunk). Its URIs are built from it when served.
	 */
	int backing_ld;
	/*
	 * A Volatile chunk of an SLD whose Set Partition Info (4101h) was not
	 * Immediate: "the change in partitioning shall become the "next"
	 * configuration, to become active on the next reset" (CXL r4.0
	 * Table 8-311). Until Get Partition Info (4100h) shows it active it
	 * is served StandbyOffline and is in no Connection, and it keeps its
	 * range of the domain, so that no other chunk takes it (cxl-fmd's
	 * choices): see fmd_sld_staged_refresh(). @staged_vmem is the Next
	 * Volatile Capacity it was staged with, in units of 256 MB.
	 */
	bool staged;
	uint64_t staged_vmem;
};


/*
 * Dynamic Capacity policies: the FMD_DC_ADD_* and FMD_DC_REL_* masks are those
 * of Get DCD Info (CXL r4.0 Table 7-65), whose Bit[3] of the add policies
 * "Must be 0": cxl-fmd uses it for Enable Shared Access, which a device
 * cannot advertise. The FMD_DC_POLICY_* values are the Selection Policy of
 * Table 7-72 and the Removal Policy of Table 7-73.
 */
#define FMD_DC_ADD_FREE            0x01
#define FMD_DC_ADD_CONTIGUOUS      0x02
#define FMD_DC_ADD_PRESCRIPTIVE    0x04
#define FMD_DC_ADD_SHARED_ACCESS   0x08

#define FMD_DC_REL_TAG_BASED       0x01
#define FMD_DC_REL_PRESCRIPTIVE    0x02

/* Initiate DC Add (5604h) selection policy values */
#define FMD_DC_POLICY_FREE          0
#define FMD_DC_POLICY_CONTIGUOUS    1
#define FMD_DC_POLICY_PRESCRIPTIVE  2
#define FMD_DC_POLICY_SHARED_ACCESS 3

/* Initiate DC Release (5605h) flags */
#define FMD_DC_REL_POLICY_MASK      0x0F
#define FMD_DC_REL_POLICY_TAG_BASED 0
#define FMD_DC_REL_POLICY_PRESCRIPTIVE 1
#define FMD_DC_REL_FORCED_REMOVAL   0x10
#define FMD_DC_REL_SANITIZE         0x20

/*
 * DC region flags: 5601h Region Configuration, Attributes1 byte
 * (CXL r4.0 Table 7-68). Bit positions follow the CDAT DSMAS Flags field.
 */
#define FMD_DC_REGION_NONVOLATILE   (1 << 2)
#define FMD_DC_REGION_SHAREABLE     (1 << 3)
#define FMD_DC_REGION_HW_COHERENT   (1 << 4)
#define FMD_DC_REGION_INTERCONNECT_DCM (1 << 5)
#define FMD_DC_REGION_READ_ONLY     (1 << 6)

/*
 * Whether a host's extents are those of its last Get DC Region Extent
 * Lists (5603h): never read, read, kept from an earlier read after the
 * last one failed (STALE), or to be read again (DIRTY): a DC event record
 * named the host, the log overflowed, a command to it was accepted and it
 * is about to be read, or a command's outcome is not known, and the host
 * has not been read since. A record that is cleared is not reported
 * again, so STALE and DIRTY hosts are read again on every --event-poll
 * tick until a read completes (cxl-fmd's choice; see
 * fmd_dcd_retry_stale()).
 */
enum fmd_dc_sync {
	FMD_DC_SYNC_NEVER = 0,		/* not read yet */
	FMD_DC_SYNC_OK,			/* the last read completed */
	FMD_DC_SYNC_STALE,		/* the last read failed */
	FMD_DC_SYNC_DIRTY,		/* to be read: a record, an overflow or a command */
};

/* Whether a host's DC state is not known to be what the device has now */
static inline bool fmd_dc_sync_behind(enum fmd_dc_sync sync)
{
	return sync == FMD_DC_SYNC_STALE || sync == FMD_DC_SYNC_DIRTY;
}

/*
 * One Dynamic Capacity Tag Information entry of Dynamic Capacity List Tags
 * (5608h, Table 7-78): the bitmaps are kept as the device sent them, bit n
 * of byte n / 8 for LD n.
 */
struct fmd_dc_tag {
	uint8_t tag[FMD_DC_TAG_LEN];
	bool    fm_ref;                             /* Flags Bit[0], FM Holds Reference */
	uint8_t ref[FMD_DC_LD_BITMAP_BYTES];        /* Reference Bitmap: LDs that accepted */
	uint8_t pend[FMD_DC_LD_BITMAP_BYTES];       /* Pending Reference Bitmap */
};

/* The device's tag list, as the last complete 5608h read gave it */
struct fmd_dc_tags {
	struct fmd_dc_tag *v;
	uint32_t n;
	uint32_t gen;                       /* Generation Number (Table 7-77) */
	uint8_t  validity;                  /* Validity Bitmap: Bit[0] ref, Bit[1] pending */
	bool     known;                     /* the last read completed */
	bool     unsupported;               /* the device answered Unsupported */
};

/*
 * The tag list is not known for a reason other than the device answering
 * Unsupported: a read failed, or kept changing, or none was made. What it
 * would say (an add an LD has not answered, an FM reference) is not known.
 */
static inline bool fmd_dc_tags_failed(const struct fmd_dc_tags *tags)
{
	return !tags->known && !tags->unsupported;
}

/* Bit @ld of a 5608h LD bitmap; false past LD 255 */
static inline bool fmd_dc_bm_test(const uint8_t *bm, unsigned int ld)
{
	return ld <= FMD_DC_MAX_HOST_ID && ((bm[ld / 8] >> (ld % 8)) & 1);
}

/*
 * Device-wide Dynamic Capacity capabilities
 *
 * From FM-API Get DCD Info (5600h). Attached to fmd_sld, fmd_mld or
 * fmd_mhd when the device supports dynamic capacity. Redfish exposes it as
 * PCIeDevice.CXLDevice/DynamicCapacity.
 */
struct fmd_dcd_info {
	uint8_t  num_hosts;
	uint8_t  num_regions;               /* Supported DC regions (1..8) */
	uint16_t add_policies;              /* FMD_DC_ADD_* bitmask */
	uint16_t release_policies;          /* FMD_DC_REL_* bitmask */
	uint8_t  sanitize_cfg_mask;         /* bit N: region N sanitize configurable */
	uint64_t total_dc_capacity;         /* bytes */
	uint64_t blk_sz_mask[FMD_MAX_DC_REGIONS]; /* bit n: 2^n byte block supported */

	/* Per-host counters for SLDs (MLD/MHD keep them on fmd_ld) */
	uint32_t extents_supported;
	uint32_t extents_available;
	uint32_t tags_supported;
	uint32_t tags_available;

	/* Host 0's 5603h state for SLDs (MLD/MHD keep it on fmd_ld) */
	uint32_t dc_ext_gen;
	uint8_t  dc_sync;                   /* enum fmd_dc_sync */
	int      dc_sync_err;

	/* Dynamic Capacity List Tags (5608h); the mock builds its own */
	struct fmd_dc_tags tags;

	/* Mock only: FM-referenced capacity no host holds */
	struct fmd_dc_mock_hold *mock_orphans;
	/* Mock only: the tags the FM holds a reference on (5606h) */
	uint8_t (*mock_refs)[FMD_DC_TAG_LEN];
	uint32_t n_mock_refs;
	/*
	 * Mock only: the file's "tags" entries, merged into every 5608h list
	 * the mock builds; their Pending Reference Bitmap bits model adds a
	 * device's 5603h does not list (cxl-fmd's mock, docs/mock-topology.md)
	 */
	struct fmd_dc_tag *mock_tags;
	uint32_t n_mock_tags;
};

/*
 * DC Region - one host's view of a dynamic capacity region
 *
 * From FM-API Get Host DC Region Configuration (5601h).
 * Child of fmd_sld (host 0) or of the fmd_ld the host is bound to.
 * Redfish: CXLLogicalDevices/{ld}/MemoryRegions/{region}
 */
struct fmd_dc_region {
	struct fmd_component base;

	uint8_t  region_id;                 /* 0..num_regions-1 */
	uint16_t host_id;                   /* FM-API Host ID */
	uint64_t base_dpa;                  /* Region Base */
	uint64_t decode_len;                /* Region Decode Length */
	uint64_t region_len;                /* Region Length */
	uint64_t block_size;                /* Region Block Size */
	uint8_t  flags;                     /* FMD_DC_REGION_* */
	bool     sanitize_on_release;
};

/*
 * DC Extent - one host's extent
 *
 * One entry of the Extent List that Get DC Region Extent Lists (5603h)
 * returns "for the specified host" (Table 7-71), keyed by Host ID and
 * Starting DPA. "The DPA ranges exposed by the device to each host are
 * independent of one another" (Section 9.13.3), so the same DPA on two
 * hosts is two extents, and capacity two hosts share is one extent per
 * host with the same Tag. Child of the fmd_sld/fmd_mld/fmd_mhd.
 * Redfish: PCIeDevices/{dev}/MemoryExtents/ext_<ld>_<dpa>
 */
struct fmd_dc_extent {
	struct fmd_component base;

	uint16_t host_id;                   /* Host ID (LD-ID) whose list has it */
	uint64_t start_dpa;                 /* in this host's DPA space */
	uint64_t len;
	uint8_t  tag[FMD_DC_TAG_LEN];       /* all zero: no tag (cxl-fmd's choice) */
	uint16_t shared_seq;
	uint8_t  region_id;                 /* this host's region, or FMD_DC_REGION_NONE */
	/*
	 * Mock only: the topology file has the host not answered the add. The
	 * mock's 5608h list and its Prescriptive release take it; Redfish
	 * does not show it, and nothing read from a device sets it.
	 */
	bool     pending;
};

/*
 * Mock only: capacity an FM reference keeps allocated that no host holds
 * any more (Section 7.6.7.6.7). It is not an extent of any host's list,
 * so it is no MemoryExtent; it counts against the region it is in.
 * @phys is the offset from the region base.
 */
struct fmd_dc_mock_hold {
	uint8_t  tag[FMD_DC_TAG_LEN];
	uint8_t  region_id;
	uint16_t shared_seq;                /* given back by Enable Shared Access */
	uint64_t phys;
	uint64_t len;
	struct fmd_dc_mock_hold *next;
};


/*
 * Host System - CXL memory initiator
 *
 * Represents a host that can access CXL memory. For switch-attached
 * topologies, hosts connect via VCS upstream ports (USPs). For direct-
 * attached devices, the host is implicit.
 *
 * Hosts are Redfish Systems with Fabric Endpoints (EntityRole=Initiator).
 */
struct fmd_host {
	struct fmd_component base;

	char name[64];               /* Human-readable name */

	/*
	 * Switch attachment (for switch-attached hosts). A VCS ID is the
	 * switch's: VCS 0 of one switch and VCS 0 of another are two hosts,
	 * and a host is given only what is on the switch its root port is on.
	 */
	char switch_id[FMD_MAX_ID_LEN]; /* switch whose VCS this host is on */
	uint8_t vcs_id;              /* VCS this host connects through */
	uint8_t usp_port_id;         /* USP port number */

	/* Direct-attached flag (no switch involved) */
	bool direct_attached;
};


/*
 * Connection Access Capabilities (bitmask)
 */
#define FMD_ACCESS_READ  0x01
#define FMD_ACCESS_WRITE 0x02

/*
 * Connections derived from the switch (src/conn_view.c)
 *
 * A switch-attached Connection is a vPPB binding, as last read with Get
 * Virtual CXL Switch Info (5200h): the FM "issues Get Virtual CXL Switch
 * Info for each enabled VCS to check for all bound vPPBs in the system
 * and create a list of binding targets" (CXL r4.0 Section 7.6.6.2). No
 * field of it records who made a binding, and bindings exist that
 * cxl-fmd never made, so none is stored: a Connection is computed, when
 * it is needed, from the VCS tables and the binds and unbinds cxl-fmd has
 * under way. That a Connection is a binding is cxl-fmd's choice.
 */

/* LD ID of a bind target that is not an LD: "Must be FFFFh for other EP types" (Table 7-35) */
#define FMD_LD_NONE 0xFFFF

/* Room for a Connection ID: "{switch}_vcs{V}_vppb{P}_p{N}_ld{L}" */
#define FMD_CONN_ID_LEN 160

/*
 * What a vPPB is bound to, and where: the fields of Bind vPPB (Table
 * 7-35), with the switch. @ld_id is FMD_LD_NONE unless the Binding Status
 * is 03h, Bound LD.
 */
struct fmd_bind_key {
	char     switch_id[FMD_MAX_ID_LEN];
	uint8_t  vcs_id;		/* Virtual CXL Switch ID, not an index */
	uint8_t  vppb_id;		/* index of the vPPB within the VCS */
	uint8_t  port_id;		/* Physical Port ID */
	uint16_t ld_id;			/* LD-ID, or FMD_LD_NONE */
};

enum fmd_bind_op_kind {
	FMD_BIND_OP_BIND,		/* Bind vPPB (5201h) */
	FMD_BIND_OP_UNBIND,		/* Unbind vPPB (5202h) */
};

/*
 * A bind or an unbind the switch is carrying out in the background for
 * cxl-fmd. It lives only while that runs: a vPPB being bound or unbound
 * reads 01h, and its Bound Port ID is "Valid only when Binding Status is
 * 02h or 03h" and its Bound LD ID "Valid only when vPPB[Start vPPB]
 * Binding Status is 03h" (Table 7-34), so what it is becoming is known
 * only from here. Kept on ctx->binding_ops,
 * fabric_lock held exclusively to change, shared to read.
 */
struct fmd_binding_op {
	struct fmd_bind_key key;
	enum fmd_bind_op_kind kind;
	uint8_t unbind_option;		/* Unbind Option (Table 7-36), for an unbind */
	char task_id[FMD_MAX_ID_LEN];
	/*
	 * Its task is Interrupted: the switch could not be asked how it went.
	 * Kept until the vPPB, read again, no longer reads 01h
	 * (fmd_tasks_reconcile()), or its switch is gone.
	 */
	bool lost;
	struct fmd_binding_op *next;
};

/*
 * A binding this run of cxl-fmd made. Advisory only: it feeds the
 * Oem.Cxl.Origin of a switch-attached Connection ("Service" or
 * "Discovered"; a direct-attached one's is "Inferred" or "Configured"),
 * and never decides whether a Connection exists. Dropped when the vPPB
 * no longer holds the key, and lost on a restart: nothing the switch
 * reports says who made a binding (Table 7-34, Table 7-80). Kept on
 * ctx->binding_marks, under fabric_lock as the operations are.
 */
struct fmd_binding_mark {
	struct fmd_bind_key key;
	struct fmd_binding_mark *next;
};

enum fmd_conn_kind {
	FMD_CONN_SWITCH,		/* a vPPB binding */
	FMD_CONN_DIRECT,		/* a device on a host's link, no switch */
};

/* What a Connection's Status says. Their meaning is cxl-fmd's choice. */
enum fmd_conn_state {
	FMD_CONN_ENABLED,		/* bound, and the target is known */
	FMD_CONN_STARTING,		/* a bind of it is under way */
	FMD_CONN_UPDATING,		/* an unbind of it is under way */
	FMD_CONN_ABSENT,		/* bound, and no device is on the port */
	FMD_CONN_UNRESOLVED,		/* bound, and what it reaches is not known */
};

/*
 * A Connection, computed by the functions below.
 *
 * LIFETIME: a view holds raw pointers into the topology, the VCS tables
 * and ctx->binding_ops. It is valid only while the caller holds both
 * ctx->fabric_lock (shared or exclusive) and the topology read lock, as it
 * did when the view was computed. Copy what is needed (the ID, the key:
 * switch, VCS, vPPB, port and LD) before letting either go. No view, nor
 * any pointer taken from one, may be kept across a lock release or across
 * a Bind vPPB (5201h) or Unbind vPPB (5202h) sent to the switch, which
 * reads the VCSs again. The functions take no locks themselves.
 */
struct fmd_conn_view {
	char id[FMD_CONN_ID_LEN];
	enum fmd_conn_kind kind;
	struct fmd_bind_key key;		/* FMD_CONN_SWITCH only */
	const struct fmd_host *host;		/* the initiator */
	const struct fmd_switch *sw;		/* NULL for FMD_CONN_DIRECT */
	/*
	 * What the binding reaches: an SLD, an LD of an MLD or a pool LD
	 * of a Multi-Headed device, or a head that presents an MLD (see
	 * fmd_bind_target()). NULL if no device is on the port, or the
	 * binding reaches nothing cxl-fmd knows on it.
	 */
	const struct fmd_component *target;
	const struct fmd_component *device;	/* the SLD, MLD or MHD; NULL if gone */
	const struct fmd_ld *ld;		/* the target if it is an LD, else NULL */
	const struct fmd_head *head;		/* the head of an MHD, if any */
	enum fmd_conn_state state;
	const struct fmd_binding_op *op;	/* the bind or unbind under way */
	bool made_here;				/* a struct fmd_binding_mark has it */
};


/*
 * Hash table for O(1) component lookup
 */
struct fmd_hash_table {
	struct fmd_component *buckets[FMD_HASH_BUCKETS];
	int count;
};

/*
 * Topology - tree representation of the CXL fabric
 */
struct fmd_topology {
	pthread_rwlock_t lock;

	/* Root of the tree */
	struct fmd_fabric *root;

	/* Hash table for O(1) lookup by ID */
	struct fmd_hash_table components;

	/* Generation counter for change tracking */
	uint32_t generation;

	/*
	 * Switches, ports, devices, LDs and hosts added and removed since
	 * the start: what changed, not only how many there are now.
	 */
	unsigned int added;
	unsigned int removed;
};


struct fmd_redfish {
	struct MHD_Daemon *daemon;
	uint16_t port;
	bool running;
};


struct fmd_ctx {
	/* libcxlmi context */
	struct cxlmi_ctx *cxlmi;

	/* Discovered topology */
	struct fmd_topology topology;

	/* Binds and unbinds under way, and bindings made by this run (src/conn_view.c) */
	struct fmd_binding_op *binding_ops;
	struct fmd_binding_mark *binding_marks;

	/* Redfish HTTP server */
	struct fmd_redfish redfish;

	/* Mock topology (NULL if using real hardware) */
	struct fmd_mock_fabric *mock_fabric;

	/* Runtime state */
	bool running;
	enum fmd_log_level log_level;

	/* AF_MCTP listeners for Event Notification: CXL CCI, FM-API (-1 if unbound) */
	int notify_sd[2];
	int64_t next_dc_poll_ms;	/* CLOCK_MONOTONIC, 0 = not armed */
	int64_t next_rescan_ms;		/* likewise, for fmd_rescan() */

	/* Endpoints that have been discovered (src/discovery.c) */
	struct cxlmi_endpoint **known_eps;
	int nknown_eps;

	/*
	 * Endpoints whose device answered Identify but could not be
	 * discovered for the moment, and when each is asked again
	 */
	struct fmd_ep_retry {
		struct cxlmi_endpoint *ep;
		struct fmd_retry retry;
	} *retry_eps;
	int nretry_eps;

	/*
	 * Endpoints opened through the kernel's mailbox (/dev/cxl). Their
	 * event logs are not the FM's: see fmd_endpoint_owns_events().
	 */
	struct cxlmi_endpoint **ioctl_eps;
	int nioctl_eps;
	/*
	 * Serializes DCD state changes (refresh, event drain, add/release)
	 * across the event loop and the Redfish threads. The topology rwlock
	 * only covers single add/remove calls, not a whole refresh.
	 */
	pthread_mutex_t dcd_lock;

	/* Operations in the background (src/tasks.c), newest first */
	struct fmd_task *tasks;
	int task_next_id;

	/*
	 * The state of the fabric: the topology, the VCS tables of the
	 * switches, the connections, and everything hanging off them. None of
	 * it has a lock of its own, and a change spans several of them and
	 * commands to the hardware: a bind reads the VCSs, picks a vPPB, sends
	 * the command and reads them again.
	 *
	 *  - A request that only reads (GET) holds it shared. Any number run
	 *    at once, on the threads of the HTTP server.
	 *  - A request that changes anything holds it exclusively, and so
	 *    does the event loop while it brings dynamic capacity up to date.
	 *
	 * libcxlmi takes no locks: commands on an endpoint are the caller's
	 * to keep apart. Whoever holds this exclusively is the only one
	 * sending any. The few commands a reader sends are kept apart by
	 * fmd_cci_read_lock().
	 *
	 * Taken before dcd_lock. Writers are preferred, so that a stream of
	 * readers does not keep a change waiting.
	 */
	pthread_rwlock_t fabric_lock;

	/* Configuration */
	struct {
		uint16_t port;
		bool scan_mctp;
		bool scan_ioctl;
		/*
		 * --allow-mctp-restricted: send the commands Table 8-308
		 * marks O4 for MCTP (see fmd_mctp_restricted_allowed())
		 */
		bool allow_mctp_restricted;
		unsigned int event_poll_sec;	/* 0 disables */
		unsigned int rescan_sec;	/* 0 disables */
		unsigned int http_threads;	/* threads serving requests */
		char *mock_topology_path;
	} config;
};

static inline bool fmd_is_mock(struct fmd_ctx *ctx)
{
	return ctx->config.mock_topology_path != NULL;
}

#define FMD_DEFAULT_EVENT_POLL_SEC 10
#define FMD_DEFAULT_RESCAN_SEC     30

struct fmd_ctx *fmd_ctx_new(void);
void fmd_ctx_free(struct fmd_ctx *ctx);


int fmd_topology_init(struct fmd_topology *topo);
void fmd_topology_cleanup(struct fmd_topology *topo);

/*
 * Add component to topology (handles tree insertion and hash table).
 * Returns 0, or -EEXIST, with nothing changed, if a component has its Id
 * already (-EINVAL with no topology). @comp is the topology's from the
 * call on, whatever it returns: on an error it has been freed, and the
 * caller is neither to use nor to free it.
 */
int fmd_topology_add(struct fmd_topology *topo, struct fmd_component *comp,
		     struct fmd_component *parent);

/* Remove component and all children from topology */
int fmd_topology_remove(struct fmd_topology *topo, struct fmd_component *comp);

/* Find component by ID (O(1) via hash table) */
struct fmd_component *fmd_topology_find(struct fmd_topology *topo, const char *id);

/* Type-specific finders */
struct fmd_switch *fmd_topology_find_switch(struct fmd_topology *topo, const char *id);
struct fmd_port *fmd_topology_find_port(struct fmd_topology *topo,
					const char *switch_id, uint8_t port_num);
struct fmd_sld *fmd_topology_find_sld(struct fmd_topology *topo, const char *id);
struct fmd_mld *fmd_topology_find_mld(struct fmd_topology *topo, const char *id);
struct fmd_ld *fmd_topology_find_ld(struct fmd_topology *topo,
				    const char *mld_id, uint8_t ld_id);
struct fmd_mem_domain *fmd_topology_find_domain(struct fmd_topology *topo, const char *id);
struct fmd_mem_chunk *fmd_topology_find_chunk(struct fmd_topology *topo, const char *id);
struct fmd_mhd *fmd_topology_find_mhd(struct fmd_topology *topo, const char *id);
struct fmd_head *fmd_topology_find_head(struct fmd_topology *topo,
					const char *mhd_id, uint8_t head_id);

/* Tree traversal callback */
typedef int (*fmd_walk_cb)(struct fmd_component *comp, void *ctx);

/* Walk entire tree depth-first */
int fmd_topology_walk(struct fmd_topology *topo, fmd_walk_cb callback, void *ctx);

/*
 * Count components of a specific type in the topology.
 * Caller must hold topology read lock.
 */
static inline int fmd_topology_count_type(struct fmd_topology *topo,
					  enum fmd_component_type type)
{
	int count = 0;
	struct fmd_component *comp;

	for (int i = 0; i < FMD_HASH_BUCKETS; i++) {
		for (comp = topo->components.buckets[i]; comp; comp = comp->hash_next) {
			if (comp->type == type)
				count++;
		}
	}
	return count;
}

/* Walk subtree from given component */
int fmd_component_walk(struct fmd_component *root, fmd_walk_cb callback, void *ctx);

/* Count children of specific type */
int fmd_component_count_children(struct fmd_component *comp, enum fmd_component_type type);

/* Thread safety helpers */
void fmd_topology_rdlock(struct fmd_topology *topo);
void fmd_topology_rdunlock(struct fmd_topology *topo);
void fmd_topology_wrlock(struct fmd_topology *topo);
void fmd_topology_wrunlock(struct fmd_topology *topo);


struct fmd_fabric *fmd_fabric_create(const char *id);
struct fmd_switch *fmd_switch_create(const char *id);
struct fmd_port *fmd_port_create(const char *id, uint8_t port_number);
struct fmd_sld *fmd_sld_create(const char *id);
struct fmd_mld *fmd_mld_create(const char *id);
struct fmd_ld *fmd_ld_create(const char *id, uint8_t ld_id);
struct fmd_mem_domain *fmd_domain_create(const char *id);
struct fmd_mem_chunk *fmd_chunk_create(const char *id);
struct fmd_host *fmd_host_create(const char *id);
struct fmd_mhd *fmd_mhd_create(const char *id);
struct fmd_head *fmd_head_create(const char *id, uint8_t head_id);
struct fmd_dc_region *fmd_dc_region_create(const char *id, uint8_t region_id,
					   uint16_t host_id);
struct fmd_dc_extent *fmd_dc_extent_create(const char *id);

/* The offset fmd_chunk_create_in_domain() is to choose itself */
#define FMD_CHUNK_OFFSET_AUTO UINT64_MAX

/*
 * Create a memory chunk in a domain with full configuration. The chunk
 * lies within the domain: [offset, offset + size) within [0, capacity).
 * offset = FMD_CHUNK_OFFSET_AUTO places it at the lowest offset where it
 * fits (first fit). @backing_ld is the chunk's backing_ld, -1 for none.
 * Returns NULL on error (insufficient capacity, out of the domain, overlap,
 * no free range of that size, etc.).
 */
struct fmd_mem_chunk *fmd_chunk_create_in_domain(
	struct fmd_topology *topo,
	struct fmd_mem_domain *domain,
	uint64_t size,
	uint64_t offset,
	const char *address_range_type,
	int backing_ld);

/*
 * Remove a memory chunk from its domain.
 * Returns 0 on success, -1 on error.
 */
int fmd_chunk_remove(struct fmd_topology *topo, struct fmd_mem_chunk *chunk);

/*
 * Connections derived from the switch (src/conn_view.c). See struct
 * fmd_conn_view for the locks the caller holds, and how long what they
 * return stays valid. None of them sends a command.
 */

/*
 * What a binding of physical port @port, with LD-ID @ld_id (FMD_LD_NONE
 * for the whole port), reaches. NULL if @port is NULL or nothing on it is
 * reached that way.
 *
 *  - the SLD on the port, bound whole;
 *  - LD @ld_id of the MLD on the port: "If the bind target is a physical
 *    port connected to an MLD, then a corresponding LD-ID must also be
 *    specified" (CXL r4.0 Section 7.6.7.2.2);
 *  - the port is a head of a Multi-Headed device, bound whole: the one
 *    pool LD mapped to the head, "A head with one LD mapped shall present
 *    itself as an SLD" (Section 2.5);
 *  - the port is a head that presents an MLD, bound by an LD-ID: the
 *    head. "The MH-MLD maps the LD-IDs received at a head to the
 *    device-wide LD index in the MH-MLD's LD pool" (Section 2.5.1) by no
 *    rule the FM is told.
 */
struct fmd_component *fmd_bind_target(const struct fmd_port *port, uint16_t ld_id);

/* A pool LD of a Multi-Headed device: the head it is mapped to, if any */
struct fmd_head *fmd_ld_head(const struct fmd_ld *ld);

/*
 * The pool LD a head that presents an SLD reaches: the one LD mapped to
 * it ("In MH-SLDs, there is a 1:1 mapping between heads and LDs", Section
 * 2.5). NULL if the head presents an MLD, or is mapped to no LD or to more
 * than one.
 */
struct fmd_ld *fmd_head_whole_ld(const struct fmd_head *head);

/*
 * The LD of @dev (an MLD or a Multi-Headed device) that chunk @chunk is
 * on, its backing_ld; NULL if it has none, or @dev has no such LD.
 */
const char *fmd_range_type(const struct cxlmi_cmd_memdev_identify_rsp *id,
			   const struct cxlmi_cmd_memdev_get_partition_info_rsp *part);
bool fmd_range_type_possible(const struct cxlmi_cmd_memdev_identify_rsp *id,
			     const struct cxlmi_cmd_memdev_get_partition_info_rsp *part,
			     const char *type);
bool fmd_partitionable(const struct cxlmi_cmd_memdev_identify_rsp *id);
json_t *fmd_chunk_range_type(const struct fmd_mem_chunk *chunk);
const struct fmd_ld *fmd_chunk_ld(const struct fmd_component *dev,
				  const struct fmd_mem_chunk *chunk);

bool fmd_bind_key_eq(const struct fmd_bind_key *a, const struct fmd_bind_key *b);

/*
 * The Connection ID of a switch-attached binding,
 * "{switch}_vcs{V}_vppb{P}_p{N}", with "_ld{L}" for an LD (cxl-fmd's
 * choice). The vPPB is in it so that a stale ID never names a later
 * binding of the same target or of the same vPPB.
 */
int fmd_conn_id_format(const struct fmd_bind_key *key, char *buf, size_t size);

/*
 * The key a switch-attached Connection ID names: parsed from the right,
 * since a switch ID may hold '_'. -EINVAL unless @id is exactly what
 * fmd_conn_id_format() makes of it.
 */
int fmd_conn_id_parse(const char *id, struct fmd_bind_key *key);

typedef int (*fmd_conn_cb)(const struct fmd_conn_view *v, void *arg);

/*
 * Each Connection: for each enabled VCS of each switch, each vPPB but the
 * USP's (vPPB 0, "the USP vPPB, then the DSP vPPBs", Section 7.1.4) that
 * is bound (02h or 03h) or that a bind under way names, to a port that is
 * a memory device's, or that has none. A vPPB bound to a PCIe device, a
 * Type 1 or Type 2 device or a switch, and one with a Bound PID (04h, made
 * with Configure PID Binding, not Bind vPPB), give none. Then the
 * direct-attached Connections, which have no vPPB (FMD_CONN_DIRECT): the
 * only direct-attached host with each SLD and MLD on no switch, and each
 * head of a Multi-Headed device on no switch with the host it is attached
 * to, if that is known. Both are cxl-fmd's choice.
 *
 * @cb's non-zero return stops the walk and is returned.
 */
int fmd_conn_for_each(const struct fmd_ctx *ctx, fmd_conn_cb cb, void *arg);

/* The Connection @id; -ENOENT if there is none, or its vPPB no longer holds it */
int fmd_conn_lookup(const struct fmd_ctx *ctx, const char *id,
		    struct fmd_conn_view *v);

/*
 * The Connection of VCS @vcs_id of @sw to port @port_id and LD @ld_id
 * (FMD_LD_NONE for the whole port), on any of its vPPBs, a bind under way
 * included. -ENOENT if there is none.
 */
int fmd_conn_find_target(const struct fmd_ctx *ctx, const struct fmd_switch *sw,
			 uint8_t vcs_id, uint8_t port_id, uint16_t ld_id,
			 struct fmd_conn_view *v);

/*
 * Each memory chunk a Connection makes reachable: every chunk of an SLD,
 * the chunks on an LD, none when the target is the head of a switch-attached
 * device or is unknown. A direct-attached MLD's are all of its chunks, and
 * those of a direct-attached head the chunks of the pool LDs mapped to it.
 * A staged chunk (struct fmd_mem_chunk) is none: it is not active yet.
 * @cb's non-zero return stops the walk and is returned.
 */
int fmd_conn_for_each_chunk(const struct fmd_conn_view *v,
			    int (*cb)(const struct fmd_mem_chunk *chunk, void *arg),
			    void *arg);

/*
 * The binds and unbinds under way. fabric_lock held exclusively to add or
 * remove, shared to look.
 */
struct fmd_binding_op *fmd_binding_op_add(struct fmd_ctx *ctx,
					  const struct fmd_bind_key *key,
					  enum fmd_bind_op_kind kind,
					  uint8_t unbind_option,
					  const char *task_id);
/* The op on vPPB @vppb_id of VCS @vcs_id of switch @switch_id */
struct fmd_binding_op *fmd_binding_op_find(const struct fmd_ctx *ctx,
					   const char *switch_id,
					   uint8_t vcs_id, uint8_t vppb_id);
/* The op on port @port_id, LD @ld_id of switch @switch_id, in any VCS */
struct fmd_binding_op *fmd_binding_op_find_target(const struct fmd_ctx *ctx,
						  const char *switch_id,
						  uint8_t port_id, uint16_t ld_id);
void fmd_binding_op_remove(struct fmd_ctx *ctx, struct fmd_binding_op *op);

/* The bindings this run made: fabric_lock as for the operations */
int fmd_binding_mark_add(struct fmd_ctx *ctx, const struct fmd_bind_key *key);
bool fmd_binding_mark_find(const struct fmd_ctx *ctx, const struct fmd_bind_key *key);
/*
 * Drop the marks of switch @switch_id (each switch's, if NULL) whose vPPB
 * no longer holds them, or whose switch is gone. Topology read lock held.
 */
void fmd_binding_marks_prune(struct fmd_ctx *ctx, const char *switch_id);

/* Free the operations and the marks */
void fmd_binding_state_free(struct fmd_ctx *ctx);

/**
 * fmd_host_find - Find a host by ID
 * @topo: Topology
 * @id: Host ID
 *
 * Return: Pointer to host, or NULL if not found
 */
struct fmd_host *fmd_topology_find_host(struct fmd_topology *topo, const char *id);

/* Free a component (does not remove from tree - use fmd_topology_remove) */
void fmd_component_free(struct fmd_component *comp);

/*
 * vPPB Binding Resolution
 *
 * Resolve vPPB port and LD pointers for a switch after all devices
 * are discovered/parsed.
 */
void fmd_switch_resolve_vppb_bindings(struct fmd_topology *topo, struct fmd_switch *sw);

/*
 * Check the topology against what the CXL specification lets a fabric be
 * (src/validate.c). Logs each violation with the rule it breaks and returns
 * how many there are.
 */
int fmd_topology_validate(struct fmd_ctx *ctx);

/*
 * A bind or unbind that the switch carries out in the background
 * (src/tasks.c). All of these are called with fabric_lock held exclusively,
 * except fmd_tasks_running() and fmd_tasks_poll(), which take it.
 */
enum fmd_task_kind { FMD_TASK_BIND, FMD_TASK_UNBIND };
enum fmd_task_state {
	FMD_TASK_RUNNING,
	FMD_TASK_COMPLETED,
	FMD_TASK_EXCEPTION,
	FMD_TASK_INTERRUPTED,	/* lost: not asked about, settled by a read */
};

struct fmd_task {
	int id;
	enum fmd_task_kind kind;
	enum fmd_task_state state;
	int percent;                         /* Percentage Complete of 0002h */
	int failed_polls;                    /* 0002h reads that failed in a row */
	int unobserved;                      /* 0002h said over, and the vPPB
						was not seen settled */
	char switch_id[FMD_MAX_ID_LEN];
	uint8_t vcs_id;
	uint8_t vppb_id;
	uint8_t port_id;                     /* what is bound, or unbound */
	uint16_t ld_id;                      /* FMD_LD_NONE for a whole port */
	char connection_id[FMD_CONN_ID_LEN];
	time_t start;
	time_t end;                          /* 0 while running */
	char message[128];                   /* how it ended */
	struct fmd_task *next;
};

/*
 * A task for the bind or unbind of @key on @sw, made before the command is
 * sent, so that nothing has to be allocated once the switch has started it:
 * NULL if it cannot be made, and then nothing is to be sent. It is nobody's
 * until fmd_task_activate(); fmd_task_discard() frees it if the command
 * was not started in the background.
 */
struct fmd_task *fmd_task_alloc(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				struct fmd_switch *sw,
				const struct fmd_bind_key *key);
/*
 * @sw has started the operation of @task in the background: the task is
 * listed, and takes over the struct fmd_binding_op of its vPPB, which the
 * caller added before it sent the command, and frees it when the task ends.
 * If there is no op one is added with @unbind_option; if that cannot be,
 * the task is freed and NULL returned.
 */
struct fmd_task *fmd_task_activate(struct fmd_ctx *ctx, struct fmd_task *task,
				   uint8_t unbind_option);
void fmd_task_discard(struct fmd_task *task);
/* fmd_task_alloc() and fmd_task_activate() */
struct fmd_task *fmd_task_start(struct fmd_ctx *ctx, enum fmd_task_kind kind,
				struct fmd_switch *sw,
				const struct fmd_bind_key *key,
				uint8_t unbind_option);
/* what fmd_task_alloc() allocates with; tests replace it */
extern void *(*fmd_task_calloc)(size_t nmemb, size_t size);
struct fmd_task *fmd_task_find(struct fmd_ctx *ctx, int id);
bool fmd_tasks_running(struct fmd_ctx *ctx);
void fmd_tasks_poll(struct fmd_ctx *ctx);
void fmd_tasks_free(struct fmd_ctx *ctx);
/*
 * The VCSs of @sw have just been read, @refresh_rc as fmd_vcs_refresh()
 * returns: each Interrupted task on @sw whose vPPB was read and no longer
 * reads 01h ends on what it reads. Called by fmd_vcs_refresh().
 */
void fmd_tasks_reconcile(struct fmd_ctx *ctx, struct fmd_switch *sw,
			 int refresh_rc);

/*
 * fmd_vppb_bind() and fmd_vppb_unbind() return this when the switch has
 * started the operation and has not finished it.
 */
#define FMD_OP_STARTED 1

/*
 * Background Operation Status (0002h) of a switch, for an operation with
 * @opcode. Returns 1 while it is in progress, with @percent; 0 when it is
 * over, with its return code in @rc; negative if the switch cannot be asked.
 */
int fmd_switch_bg_status(struct fmd_ctx *ctx, struct fmd_switch *sw,
			 uint16_t opcode, int *percent, uint16_t *rc);

/*
 * Around a command that a request sends while it holds fabric_lock shared:
 * two such requests must not have commands on one endpoint at once.
 */
void fmd_cci_read_lock(void);
void fmd_cci_read_unlock(void);

/*
 * Threads of the HTTP server: how many requests can be in hand at once.
 * Requests that only read run together on them; -N sets the number.
 */
#define FMD_DEFAULT_HTTP_THREADS 8
#define FMD_MAX_HTTP_THREADS     256

/*
 * A VCS of a switch by its Virtual CXL Switch ID, and a vPPB of a VCS by its
 * vPPB ID. sw->vcs holds the VCSs that are active, in the order they were
 * read: a VCS ID is not an index into it.
 */
struct fmd_vcs *fmd_switch_find_vcs(struct fmd_switch *sw, uint8_t vcs_id);
struct fmd_vppb *fmd_vcs_find_vppb(struct fmd_vcs *vcs, uint8_t vppb_id);

/* Largest Message Payload to send to @comp or to ask of it: the minimum
 * every component supports unless discovery learnt better. */
static inline size_t fmd_cci_msg_limit(const struct fmd_component *comp)
{
	return comp && comp->msg_limit ? comp->msg_limit : FMD_CCI_MSG_MIN;
}

/*
 * How many list entries of @entry bytes to ask @comp for at once, in a
 * payload that has @fixed bytes besides the list: at least 1, at most @max.
 */
static inline unsigned int fmd_cci_list_fit(const struct fmd_component *comp,
					    size_t fixed, size_t entry,
					    unsigned int max)
{
	size_t limit = fmd_cci_msg_limit(comp);
	size_t n = limit > fixed ? (limit - fixed) / entry : 0;

	if (n < 1)
		n = 1;
	return n < max ? (unsigned int)n : max;
}

/*
 * The vPPB that physical port @port_id is bound to, or that LD @ld_id of the
 * MLD on it is bound to when @ld_id is not FFFFh, with its VCS in @user.
 * NULL if the port, or that LD, is unused and so may be bound.
 */
struct fmd_vppb *fmd_switch_bind_target_user(struct fmd_switch *sw,
					     uint8_t port_id, uint16_t ld_id,
					     struct fmd_vcs **user);

/*
 * Memory Resource Creation
 *
 * Create memory domain and chunk resources for devices.
 * Used by both hardware discovery and mock topology loading.
 */
int fmd_create_sld_memory_resources(struct fmd_ctx *ctx, struct fmd_sld *sld);
int fmd_create_mld_memory_resources(struct fmd_ctx *ctx, struct fmd_mld *mld);
int fmd_create_mhd_memory_resources(struct fmd_ctx *ctx, struct fmd_mhd *mhd);

/*
 * Make the domain of MLD or Multi-Headed device @dev what its LDs and
 * Memory Size now are: made if it has none and there is capacity, its
 * capacity and what is left recomputed (see fmd_create_mld_memory_resources()),
 * and the chunks of each LD made to add up to the LD's allocation. What an
 * LD gained is a chunk of its own, {dev}_ld{N}_chunk if no component has
 * that Id, placed first fit (in as many pieces as the free ranges it takes,
 * if it fits in none); what it lost comes off its chunks, the newest
 * first, and a chunk left with nothing goes. Returns 0 or an error.
 */
int fmd_ld_domain_sync(struct fmd_ctx *ctx, struct fmd_component *dev);

/*
 * CCI Tunneling
 * Every component has a pointer to the endpoint it is reached through
 * (comp->ep), inherited from the component that was discovered on that
 * endpoint. No tree walking needed.
 *
 * Tunneling logic:
 * - SLD: No tunneling (switch is transparent)
 * - MLD commands: CXLMI_TUNNEL_SWITCH(port) if behind switch
 * - LD commands: CXLMI_TUNNEL_MLD(ld) or CXLMI_TUNNEL_SWITCH_MLD(port, ld)
 *
 * Usage (with libcxlmi inline macros):
 *   cxlmi_cmd_identify(comp->ep, CXLMI_NO_TUNNEL, &rsp);
 *   cxlmi_cmd_fmapi_get_ld_info(mld->base.ep, CXLMI_TUNNEL_SWITCH(port), &rsp);
 *   cxlmi_cmd_memdev_identify(ld->base.ep, CXLMI_TUNNEL_SWITCH_MLD(port, ld), &rsp);
 */


/*
 * Tunnel to LD @ld_id of @mld: straight to the LD if the MLD is attached
 * directly, through the switch port otherwise.
 *
 * The tunnel info is written to @buf, which the caller provides and which
 * must stay alive for as long as the returned pointer is used. Do not keep
 * the pointer that CXLMI_TUNNEL_MLD() and friends evaluate to instead: it
 * points to a compound literal that is gone at the end of the enclosing
 * block.
 */
static inline struct cxlmi_tunnel_info *
fmd_mld_ld_tunnel(const struct fmd_mld *mld, int ld_id,
		  struct cxlmi_tunnel_info *buf)
{
	if (mld->switch_port < 0) {
		DEFINE_CXLMI_TUNNEL_MLD(ti, ld_id);
		*buf = ti;
	} else {
		DEFINE_CXLMI_TUNNEL_SWITCH_MLD(ti, mld->switch_port, ld_id);
		*buf = ti;
	}

	return buf;
}


int fmd_discover(struct fmd_ctx *ctx);

/*
 * The bit of a supported_cmds mask (the commands a component's Command
 * Effects Log lists, FMD_CMD_* of mock.h) that stands for @opcode, or 0 if
 * cxl-fmd keeps none for it
 */
uint64_t fmd_cmd_bit(uint16_t opcode);

/*
 * Whether @opcode is one of the Memory Device commands that CXL r4.0 Table
 * 8-308 marks "O4" for MCTP: Set Partition Info, Set LSA, Clear Poison,
 * Scan Media, Sanitize, Secure Erase, Set Passphrase, Disable Passphrase,
 * Unlock, Freeze Security State, Passphrase Secure Erase, Security Send
 * and Set SLD QoS Control. Note 4: "Systems capable of management from
 * Mailbox registers and an MCTP-based CCI shall ensure that these commands
 * are not issued as MCTP messages while a device's mailboxes are
 * operational."
 */
bool fmd_mctp_restricted(uint16_t opcode);

/*
 * Whether cxl-fmd sends @opcode to memory device @dev (an SLD, an MLD for
 * one of its LDs, or a Multi-Headed device). Always for a command note 4
 * does not cover, and for any when the operator started cxl-fmd with
 * --allow-mctp-restricted. Otherwise only to an SLD attached directly
 * through the kernel's mailbox (ctx->ioctl_eps), which is not MCTP: the
 * FM cannot tell whether the device's mailboxes are operational, and
 * cxl-fmd takes every other route as MCTP (see the definition).
 */
bool fmd_mctp_restricted_allowed(struct fmd_ctx *ctx,
				 const struct fmd_component *dev,
				 uint16_t opcode);

/*
 * Bring the topology up to date with what is there now: new endpoints, new
 * ports, devices that have appeared on ports, new VCSs. Adds and never
 * removes. Takes fabric_lock. Returns how many components were added.
 */
int fmd_rescan(struct fmd_ctx *ctx);

/*
 * Get Health Info (4200h) again of a memory device whose health discovery
 * read: an SLD on an endpoint of its own, each LD of an MLD, each pool LD
 * of a Multi-Headed device, or one LD. A read that fails leaves its
 * health_valid false, so that no Health is reported from what was read
 * before. Returns how many reads failed. fabric_lock held exclusively.
 */
int fmd_refresh_health(struct fmd_ctx *ctx, struct fmd_component *dev);

/*
 * Whether cxl-fmd may read and clear the event logs of a component reached
 * through @ep. "The Primary and Secondary Mailbox register's CCI instance
 * pairs shall share the context, but the MCTP CCI within a component shall
 * have a unique context" for events, "including reading contents, clearing
 * entries, and configuring interrupt settings" (CXL r4.0 Section 9.20.1).
 * The logs of an endpoint opened through the kernel's mailbox are the
 * host's: records cleared there would be taken from the kernel's driver.
 */
bool fmd_endpoint_owns_events(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep);

/* One switch: some of its ports, or all of it. fabric_lock held exclusively */
int fmd_switch_refresh_ports(struct fmd_ctx *ctx, struct fmd_switch *sw,
			     const uint8_t *port_ids, int nports);
int fmd_switch_rediscover(struct fmd_ctx *ctx, struct fmd_switch *sw);

/*
 * A device that a switch port no longer has (src/discovery.c). What became
 * of its Connections is what the vPPBs say once they are read again.
 * fabric_lock held exclusively.
 */
void fmd_switch_port_device_removed(struct fmd_ctx *ctx, struct fmd_switch *sw,
				    struct fmd_port *port, struct fmd_component *dev);

/*
 * The event logs of the switches (src/switch_events.c): read, acted on and
 * cleared. Takes fabric_lock.
 */
int fmd_switch_events_poll(struct fmd_ctx *ctx);

/*
 * The logs of the components on endpoint @ep alone, which sent an MCTP
 * Event Notification: a switch, or a memory device on an endpoint of its
 * own. NULL is every component, as fmd_switch_events_poll() and
 * fmd_dcd_poll_events() read. Return how many components were read, or -1.
 */
int fmd_switch_events_poll_ep(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep);
int fmd_dcd_poll_events_ep(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep);

/*
 * Where the acknowledgement of a request received from @from is sent: to
 * its sender, as a response with its tag (src/events.c)
 */
struct sockaddr_mctp;
void fmd_events_ack_addr(const struct sockaddr_mctp *from, struct sockaddr_mctp *to);

/* An Event Notification from @net:@eid saying logs @events have records */
void fmd_events_route(struct fmd_ctx *ctx, unsigned int net, uint8_t eid,
		      uint16_t events);

/* Whether @comp is the component on @ep that sends its notifications */
bool fmd_component_on_ep(struct fmd_component *comp, struct cxlmi_endpoint *ep);

/*
 * Event records kept for Redfish (src/event_log.c). All of it runs with
 * fabric_lock held: exclusively to change, shared to read.
 */
struct fmd_log_entry {
	int id;                              /* LogEntry Id: increasing */
	int log;                             /* 0 Informational to 3 Fatal,
						FMD_LOG_DYNAMIC_CAPACITY */
	time_t created;                      /* when it was read */
	struct cxlmi_event_record record;    /* as Get Event Records gave it */
	bool overflow;                       /* the log overflowed after it */
	uint16_t overflow_count;
	uint64_t first_overflow_ns;
	uint64_t last_overflow_ns;
};

struct fmd_log {
	struct fmd_log_entry *entries;       /* a ring of allocated entries */
	int allocated;                       /* grows up to capacity */
	int capacity;                        /* MaxNumberOfRecords */
	int first;
	int count;
	int last_id;
	bool overflow;                       /* a log reported Overflow */
};

struct fmd_overflow {
	bool set;
	int log;
	uint16_t count;
	uint64_t first_ns;
	uint64_t last_ns;
	/*
	 * The last record of the first page that said Overflow: one stored
	 * before it, which carries it (fmd_log_overflowed()). Valid if
	 * @marked.
	 */
	bool marked;
	struct cxlmi_event_record before;
};

typedef void (*fmd_event_record_fn)(struct fmd_ctx *ctx, struct fmd_component *comp,
				    int log, const struct cxlmi_event_record *rec,
				    void *arg);

int fmd_event_log_drain(struct fmd_ctx *ctx, struct fmd_component *comp,
			struct cxlmi_tunnel_info *ti, int which,
			fmd_event_record_fn act, void *arg,
			struct fmd_overflow *overflow);
int fmd_log_add(struct fmd_component *comp, int which,
		const struct cxlmi_event_record *rec);
void fmd_log_overflowed(struct fmd_component *comp, int which, uint16_t count,
			uint64_t first_ns, uint64_t last_ns,
			const struct cxlmi_event_record *before);
void fmd_log_clear(struct fmd_component *comp);
void fmd_log_free(struct fmd_component *comp);
bool fmd_device_logs_read(struct fmd_ctx *ctx, struct fmd_component *dev);
void fmd_device_record_keep(struct fmd_ctx *ctx, struct fmd_component *dev,
			    int which, const struct cxlmi_event_record *rec,
			    void *arg);
void fmd_device_events_enable(struct fmd_ctx *ctx, struct fmd_component *dev);

/* The log of a kept record that came from the Dynamic Capacity Event Log */
#define FMD_LOG_DYNAMIC_CAPACITY	4
void fmd_dc_record_keep(struct fmd_component *dev,
			const struct cxlmi_event_record *rec);
void fmd_dc_log_overflowed(struct fmd_component *dev, uint16_t count,
			   uint64_t first_ns, uint64_t last_ns);
int fmd_device_logs_poll(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep);
int fmd_event_record_kind_from_name(const char *name, uint8_t uuid[16]);

/*
 * Mock: keep a Virtual CXL Switch Event Record (Table 7-80), Binding
 * Change, of vPPB @vppb of VCS @vcs_id in log @which of a mock switch, as
 * its state now is.
 */
void fmd_mock_binding_record(struct fmd_switch *sw, int which, uint8_t vcs_id,
			     const struct fmd_vppb *vppb);

json_t *fmd_redfish_ld_log(struct fmd_ctx *ctx, const char *chassis_id,
			   const char *pcie_id, const char *ld_id,
			   const char *what, int id);
int fmd_redfish_ld_log_clear(struct fmd_ctx *ctx, const char *chassis_id,
			     const char *pcie_id, const char *ld_id);
json_t *fmd_redfish_switch_log(struct fmd_ctx *ctx, const char *chassis_id,
			       const char *log_id, const char *what, int id);
int fmd_redfish_switch_log_clear(struct fmd_ctx *ctx, const char *chassis_id,
				 const char *log_id);

/* Ask a switch to say when its logs have new records. fabric_lock held */
void fmd_switch_events_enable(struct fmd_ctx *ctx, struct fmd_switch *sw);

/*
 * A Binding Change of a vPPB, from a Virtual CXL Switch Event Record, with
 * its vPPB Binding Status, Port ID and LD ID (33h-35h, Table 7-80): the
 * end of the bind or unbind a task is following, if those fit it. Returns
 * false if no task is following that vPPB, or the record does not fit the
 * one that is. fabric_lock held.
 */
bool fmd_task_binding_changed(struct fmd_ctx *ctx, struct fmd_switch *sw,
			      uint8_t vcs_id, uint8_t vppb_id, bool failed,
			      uint8_t status, uint8_t port_id, uint8_t ld_id);

/*
 * VCS Bind/Unbind Operations (CXL r4.0 Section 7.6.7.2)
 *
 * These functions implement the FM-API Bind vPPB (5201h) and Unbind vPPB (5202h)
 * commands for dynamically managing port/LD to vPPB bindings.
 */

/**
 * fmd_vppb_bind - Bind a physical port (or MLD LD) to a vPPB
 * @ctx: FMD context
 * @sw: Target switch
 * @vcs_id: VCS identifier (0-based)
 * @vppb_id: vPPB identifier within the VCS
 * @port_id: Physical port number to bind
 * @ld_id: LD ID for MLD binding (use 0xFFFF for SLD/physical port binding)
 *
 * For SLD binding: port_id identifies the port, ld_id should be 0xFFFF
 * For MLD binding: port_id identifies the MLD port, ld_id identifies the LD
 *
 * The switch may initiate Managed Hot-Add if the host has already booted.
 * Upon completion, the switch generates a Virtual CXL Switch Event Record.
 *
 * Return: 0 on success, negative errno on failure
 */
int fmd_vppb_bind(struct fmd_ctx *ctx, struct fmd_switch *sw,
		  uint8_t vcs_id, uint8_t vppb_id,
		  uint8_t port_id, uint16_t ld_id);

/**
 * fmd_vppb_unbind - Unbind a vPPB from its physical port/LD
 * @ctx: FMD context
 * @sw: Target switch
 * @vcs_id: VCS identifier (0-based)
 * @vppb_id: vPPB identifier within the VCS
 * @option: Unbind option (Managed Hot-Remove, Surprise Hot-Remove, etc.)
 *
 * The switch initiates Hot-Remove based on the option specified.
 * Upon completion, the switch generates a Virtual CXL Switch Event Record.
 *
 * Return: 0 on success, negative errno on failure
 */
int fmd_vppb_unbind(struct fmd_ctx *ctx, struct fmd_switch *sw,
		    uint8_t vcs_id, uint8_t vppb_id,
		    enum fmd_unbind_option option);

/**
 * fmd_vcs_refresh - Refresh VCS configuration from switch
 * @ctx: FMD context
 * @sw: Target switch
 *
 * Re-queries the switch for current VCS/vPPB state. Useful after bind/unbind
 * operations to update the local view of the switch configuration. A VCS
 * that could not be read keeps what was known of it, with stale set, and
 * one never read before is left out.
 *
 * Return: 0 when every active VCS was read, -EIO when one or more could not
 * be, another negative errno on failure
 */
int fmd_vcs_refresh(struct fmd_ctx *ctx, struct fmd_switch *sw);

/*
 * Get device timestamp via CCI Get Timestamp (0300h)
 * For mock mode, returns current system time.
 * Returns nanoseconds since Unix epoch, or 0 on error and for an SLD on a
 * switch port, which cannot be asked.
 */
uint64_t fmd_get_device_timestamp(struct fmd_component *comp);

/*
 * Set device timestamp via CCI Set Timestamp (0301h)
 * For mock mode, accepts silently (no persistence).
 * Returns 0 on success, -EOPNOTSUPP for an SLD on a switch port, which
 * cannot be reached, other negative values on error.
 */
int fmd_set_device_timestamp(struct fmd_component *comp, uint64_t timestamp_ns);

/*
 * Get security state via CCI Get Security State (4500h)
 * For mock mode, returns 0 (security disabled).
 * ld_id is used for MLD tunneling (ignored for SLD).
 */
uint32_t fmd_get_security_state(struct fmd_component *comp, int ld_id);

/*
 * Set LD allocation for an MLD via CCI Set LD Allocations (5402h)
 * For mock mode, updates local state only. Range 2 of the LD is kept as
 * the device reports it, and Range 1 takes the rest.
 * Returns 0 on success, -EINVAL if @size_bytes is not a multiple of the
 * Memory Granularity or is less than Range 2, other negative values on
 * error.
 */
int fmd_mld_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mld *mld,
			      uint8_t ld_id, uint64_t size_bytes);

/*
 * Allocations of the LDs of an MLD or Multi-Headed device @dev made or
 * changed by others than cxl-fmd: the specification has no notification
 * of them, so they are read again. On hardware (in a mock each does
 * nothing and returns 0):
 *
 * fmd_ld_alloc_refresh() sends Get LD Info (5400h) for the Memory Size,
 * kept if it does not answer, and Get LD Allocations (5401h) for every LD,
 * then fmd_ld_domain_sync(). From fmd_rescan().
 *
 * fmd_ld_alloc_refresh_ld() sends Get LD Allocations for @ld alone, then
 * fmd_ld_domain_sync(): before a chunk of @ld is made or deleted.
 *
 * Each returns 0, -EOPNOTSUPP if no LD Pool CCI of a Multi-Headed device
 * is known (nothing sent), or -EIO if the allocations could not be read,
 * in which case nothing is changed.
 */
int fmd_ld_alloc_refresh(struct fmd_ctx *ctx, struct fmd_component *dev);
int fmd_ld_alloc_refresh_ld(struct fmd_ctx *ctx, struct fmd_component *dev,
			    struct fmd_ld *ld);

/*
 * Set LD Allocations (5402h) for @ld of @dev to have @size_bytes, of the
 * Memory Granularity and Range 2 fmd_ld_alloc_refresh_ld() last read:
 * Range 2 is kept and Range 1 takes the rest. @ld is sized from the
 * response, which "reflect[s] the state of the LD allocations after the
 * command is processed" (CXL r4.0 Section 7.6.7.4.3), whatever it says:
 * the caller compares. The chunks are not changed. In a mock, @ld is given
 * @size_bytes and nothing is sent.
 *
 * A return code that says nothing was done (Invalid Input, Unsupported,
 * Retry Required, Invalid Payload Length, Unsupported Mailbox or CCI) is
 * -EIO, @ld unchanged. Any other outcome (another error, a response that
 * cannot be read or is for another LD) is read back with Get LD
 * Allocations: 0 if that answers, @ld sized from it; if it does not, @ld
 * is marked alloc_unknown, as fmd_ld_alloc_refresh_ld() reads it again
 * before anything else of @dev is changed, and -EREMOTEIO returned.
 *
 * Returns 0, -EINVAL if @size_bytes is not a whole number of granules or
 * is less than Range 2 (nothing sent), -EOPNOTSUPP if no LD Pool CCI is
 * known, -EIO, or -EREMOTEIO.
 */
int fmd_ld_alloc_set(struct fmd_ctx *ctx, struct fmd_component *dev,
		     struct fmd_ld *ld, uint64_t size_bytes);

/*
 * The route to the LD Pool CCI of @mhd, from its routes: *@ep, through *@ti
 * (which points into @mhd, or is CXLMI_NO_TUNNEL). Returns 0, or
 * -EOPNOTSUPP if cxl-fmd knows of no route of @mhd to its LD Pool CCI.
 */
int fmd_mhd_pool_route(struct fmd_mhd *mhd, struct cxlmi_endpoint **ep,
		       struct cxlmi_tunnel_info **ti);

/* Record a route to @mhd (struct fmd_route); one known already is not */
void fmd_mhd_add_route(struct fmd_mhd *mhd, struct cxlmi_endpoint *ep,
		       const struct cxlmi_tunnel_info *ti, enum fmd_cci_role role,
		       int head, uint16_t msg_limit);

/*
 * Set LD allocation for an MHD via LD Pool CCI Set LD Allocations (5402h)
 * Uses pool LD index (not head-local LD-ID), on fmd_mhd_pool_route().
 * For mock mode, updates local state only. As fmd_mld_set_ld_allocation(),
 * and -EOPNOTSUPP, with nothing sent, if the route is not known.
 */
int fmd_mhd_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mhd *mhd,
			      uint8_t pool_ld_id, uint64_t size_bytes);

/*
 * Find which head of an MHD a host is attached to.
 * Returns head pointer if found, NULL otherwise.
 */
struct fmd_head *fmd_mhd_find_host_head(struct fmd_mhd *mhd, const char *host_id);

/*
 * Validate that a pool LD is accessible from a given head.
 * Returns true if ld->binding_point_id matches head->head_id.
 */
bool fmd_mhd_ld_accessible_from_head(struct fmd_ld *ld, struct fmd_head *head);

/*
 * Set Partition Info (4101h). Capacities are in units of 256 MB, and
 * @next_volatile is the total volatile capacity wanted, Volatile Only
 * capacity included. See src/discovery.c.
 */
int fmd_partition_volatile(const struct cxlmi_cmd_memdev_identify_rsp *id,
			   uint64_t next_volatile, uint64_t *volatile_capacity);
int fmd_memdev_set_partition(struct fmd_ctx *ctx, const char *name,
			     struct cxlmi_endpoint *ep,
			     struct cxlmi_tunnel_info *ti,
			     const struct cxlmi_cmd_memdev_identify_rsp *id,
			     struct cxlmi_cmd_memdev_get_partition_info_rsp *part,
			     uint64_t next_volatile, bool immediate);
uint64_t fmd_partition_next_volatile(const struct cxlmi_cmd_memdev_get_partition_info_rsp *part);

/*
 * Set the volatile/persistent partition of an SLD via Set Partition Info
 * (4101h). @volatile_bytes is the total volatile capacity wanted, Volatile
 * Only capacity included, a multiple of 256 MB.
 * For mock mode, updates local state only.
 * If immediate is true, change takes effect immediately; else at next reset.
 * Returns 0, -EINVAL if the device cannot be given that, -EOPNOTSUPP if it
 * cannot be partitioned or reached, other negative values on error.
 */
int fmd_sld_set_partition(struct fmd_ctx *ctx, struct fmd_sld *sld,
			  uint64_t volatile_bytes, bool immediate);

/*
 * Read Get Partition Info (4100h) of @sld again if it has a staged chunk
 * (struct fmd_mem_chunk), and nothing is sent if it has none. With no
 * change pending, a staged chunk the Active Volatile Capacity has reached
 * becomes an ordinary one, and one it has not is removed. A change still
 * pending, or a read that fails, leaves them as they are. Called by
 * fmd_rescan(). Returns 0, or -EIO if 4100h was not answered.
 */
int fmd_sld_staged_refresh(struct fmd_ctx *ctx, struct fmd_sld *sld);



/*
 * Dynamic Capacity Device (DCD) management (CXL r4.0 Section 7.6.7.6)
 *
 * See docs/DCD-FM-API.md for the FM-API to Redfish mapping. All functions
 * work on the Type 3 device component (SLD, MLD or MHD). For mock mode they
 * update local state only; for hardware they issue the FM-API command and
 * refresh the affected host's regions and extents from the device.
 */

/* Device-level accessor: NULL when the device is not a DCD */
struct fmd_dcd_info *fmd_memdev_dcd(struct fmd_component *dev);

/*
 * Resolve the FM-API Host ID for a CXLLogicalDevice index:
 *   SLD: 0, MLD: LD-ID, MHD: pool LD index (docs/DCD-FM-API.md,
 *   "Host ID Resolution").
 * Returns the host id, or -1 if the LD does not exist.
 */
int fmd_dc_host_id(struct fmd_component *dev, int ld_idx);

/*
 * Bytes of dynamic capacity the host of LD @ld_idx holds: the sum of its
 * extents. 0 for a device that is not a DCD; UINT64_MAX if it overflows.
 * Takes ctx->dcd_lock and the topology read lock.
 */
uint64_t fmd_dc_host_bytes(struct fmd_ctx *ctx, struct fmd_component *dev, int ld_idx);

/* floor(log2(v)); 0 for v <= 1 */
static inline int fmd_ilog2_u64(uint64_t v)
{
	int n = 0;

	while (v > 1) {
		v >>= 1;
		n++;
	}
	return n;
}

/*
 * Policy tables (CXL r4.0 Tables 7-72 and 7-73) keyed by the FMD_DC_POLICY_* /
 * FMD_DC_REL_POLICY_* wire values; names come from the generated
 * PCIeDevice.CXLDynamicCapacityPolicies enum.
 */
const char *fmd_dc_policy_name(bool release, int value);	/* NULL past the end */
uint16_t fmd_dc_policy_cap_bit(bool release, int value);	/* 5600h bit, 0 if invalid */
int fmd_dc_policy_from_str(bool release, const char *name);	/* -1 if unknown */

/* Free a device's fmd_dcd_info and what hangs off it */
void fmd_dcd_info_free(struct fmd_dcd_info *dcd);

/* Create and attach a host's view of a region / one host's extent */
struct fmd_dc_region *fmd_dc_region_add(struct fmd_ctx *ctx, struct fmd_component *owner,
					uint8_t region_id, uint16_t host_id);
struct fmd_dc_extent *fmd_dc_extent_add(struct fmd_ctx *ctx, struct fmd_component *dev,
					int host_id, uint64_t dpa, uint64_t len,
					const uint8_t tag[FMD_DC_TAG_LEN],
					uint16_t shared_seq, uint8_t region_id);

/*
 * Recount a mock device's available extents and tags of every host (SLD
 * keeps them on dcd, MLD/MHD on the LD) from its extents, Section 9.13.3.3
 */
void fmd_dc_mock_recount(struct fmd_component *dev);

/* Build a mock device's 5608h tag list from its extents and FM references */
void fmd_dc_mock_retag(struct fmd_component *dev);

/* The 5608h entry of a tag, NULL when the list has none or is not known */
const struct fmd_dc_tag *fmd_dc_tag_find(struct fmd_component *dev,
					 const uint8_t tag[FMD_DC_TAG_LEN]);

/* Mock only: hold, or drop, an FM reference on a tag */
void fmd_dc_mock_ref_set(struct fmd_dcd_info *dcd, const uint8_t tag[FMD_DC_TAG_LEN],
			 bool on);

/* Whether a host's extents are those of its last 5603h (enum fmd_dc_sync) */
enum fmd_dc_sync fmd_dc_host_sync(struct fmd_component *dev, int host_id);

/* Component (SLD or LD) that owns the DC regions of a host */
struct fmd_component *fmd_dc_host_component(struct fmd_component *dev, int host_id);

/* Find a host's region by region number */
struct fmd_dc_region *fmd_dc_find_region(struct fmd_component *dev, int host_id,
					 uint8_t region_id);

/* Find the extent of a host that starts at a DPA of that host */
struct fmd_dc_extent *fmd_dc_find_extent(struct fmd_component *dev, int host_id,
					 uint64_t dpa);

/* Count a host's extents in one of its regions */
unsigned int fmd_dc_host_extent_count(struct fmd_component *dev, int host_id,
				      uint8_t region_id);

/*
 * Hardware discovery: 5600h, then 5601h/5603h per host and 5608h. A 5600h
 * answered Unsupported (or Unsupported Mailbox or CCI) says the device is
 * no DCD; one that fails otherwise (enum fmd_probe) leaves it dcd_pending,
 * and returns -EAGAIN.
 */
int fmd_dcd_discover(struct fmd_ctx *ctx, struct fmd_component *dev);

/*
 * Get DCD Info again of the memory devices whose last one failed for the
 * moment (dcd_pending), each when its backoff says (struct fmd_retry);
 * one found to be a DCD has its DC event notification asked for. Called
 * by fmd_rescan(), fabric_lock held exclusively.
 */
void fmd_dcd_retry_pending(struct fmd_ctx *ctx);

/*
 * Drain the Dynamic Capacity Event Log of every DCD and refresh the hosts
 * named in Add Capacity Response / Capacity Released records.
 */
int fmd_dcd_poll_events(struct fmd_ctx *ctx);

/*
 * Read again every host of every DCD whose state is STALE or DIRTY, and a
 * tag list that could not be read, whoever owns the device's event log.
 * Return how many devices were read, or -1 if a read failed.
 */
int fmd_dcd_retry_stale(struct fmd_ctx *ctx);

/* MCTP Event Notification listeners (src/events.c) */
int fmd_events_init(struct fmd_ctx *ctx);
void fmd_events_cleanup(struct fmd_ctx *ctx);
int fmd_events_poll(struct fmd_ctx *ctx, int timeout_ms);

/* Refresh one host's regions and extents, and the tag list (hardware only) */
int fmd_dcd_refresh(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id);

/*
 * The four operations below are called with ctx->dcd_lock held. The regions
 * and extents they are given, and those the caller looked at to decide on
 * the call, are freed and rebuilt under that lock whenever a device is
 * refreshed, which the event thread does on its own: look them up, use them
 * and call in a single hold of the lock.
 */

/*
 * The four return, when the device refused the command: -EINVAL for
 * Invalid Input, Invalid Extent List or Invalid Physical Address, -ENOSPC
 * for Resources Exhausted, -EAGAIN for Retry Required or Busy; -EIO for
 * any other return code. -EREMOTEIO when the outcome is not known: no
 * response, or one libcxlmi could not read, which cannot be told from a
 * request that was not sent; the host (every host for 5602h, the tag list
 * for 5606h and 5607h) is then marked to be read again.
 *
 * FMD_DC_ACCEPTED_STALE (positive) when the device answered Success and
 * the read after it failed: the command was carried out (5604h "shall
 * complete when the device initiates the Add Capacity procedure", Section
 * 7.6.7.6.5; 5605h likewise, Section 7.6.7.6.6), and what cxl-fmd shows
 * is stale until fmd_dcd_retry_stale() reads it. It is not a failure, and
 * the command is not to be sent again.
 */
#define FMD_DC_ACCEPTED_STALE 1

/*
 * Set DC Region Configuration (5602h)
 * Returns 0, -EINVAL (unsupported value), -EBUSY (extents allocated, or the
 * device answered Unsupported), -EAGAIN, -EIO.
 */
int fmd_dc_set_region_config(struct fmd_ctx *ctx, struct fmd_component *dev,
			     struct fmd_dc_region *region, uint64_t block_size,
			     bool sanitize_on_release);

/*
 * Initiate Dynamic Capacity Add (5604h)
 * @policy: FMD_DC_POLICY_*; @extents/@ext_count used for Prescriptive.
 * Returns 0, -EINVAL, -ENOSPC (no room), -ENOENT (region/extent missing),
 * -EEXIST, -EBUSY, -EAGAIN, -EIO.
 */
int fmd_dc_add(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
	       uint8_t policy, uint8_t region_id, uint64_t length,
	       const uint8_t tag[FMD_DC_TAG_LEN],
	       struct fmd_dc_extent **extents, int ext_count);

/*
 * Initiate Dynamic Capacity Release (5605h)
 * @policy: FMD_DC_REL_POLICY_*; @extents/@ext_count used for Prescriptive.
 */
int fmd_dc_release(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id,
		   uint8_t policy, bool forced, bool sanitize, uint64_t length,
		   const uint8_t tag[FMD_DC_TAG_LEN],
		   struct fmd_dc_extent **extents, int ext_count);

/* Dynamic Capacity Add Reference (5606h) / Remove Reference (5607h) */
int fmd_dc_reference(struct fmd_ctx *ctx, struct fmd_component *dev,
		     const uint8_t tag[FMD_DC_TAG_LEN], bool add);

/* Tag helpers: 16-byte tag <-> UUID string (37 bytes incl. NUL) */
void fmd_dc_tag_to_str(const uint8_t tag[FMD_DC_TAG_LEN], char *buf, size_t len);
int fmd_dc_tag_from_str(const char *str, uint8_t tag[FMD_DC_TAG_LEN]);


int fmd_redfish_start(struct fmd_ctx *ctx);
void fmd_redfish_stop(struct fmd_ctx *ctx);

/*
 * The 501 body for the @n query parameters that start with $, @found
 * (src/redfish/server.c): NULL unless @found holds all @n and all of the
 * body could be made. @found is not consumed.
 */
json_t *fmd_redfish_unsupported_query_body(json_t *found, size_t n);


/* Service root and metadata */
json_t *fmd_redfish_odata(struct fmd_ctx *ctx);
char *fmd_redfish_metadata(struct fmd_ctx *ctx);
json_t *fmd_redfish_service_root(struct fmd_ctx *ctx);

/* Collections */
json_t *fmd_redfish_systems_collection(struct fmd_ctx *ctx);
json_t *fmd_redfish_managers_collection(struct fmd_ctx *ctx);
json_t *fmd_redfish_sessions_collection(struct fmd_ctx *ctx);

/* Systems endpoints */
json_t *fmd_redfish_system(struct fmd_ctx *ctx, const char *system_id);
json_t *fmd_redfish_system_memory_domains(struct fmd_ctx *ctx, const char *system_id);
json_t *fmd_redfish_system_memory_domain(struct fmd_ctx *ctx, const char *system_id, const char *domain_id);
json_t *fmd_redfish_system_memory_chunks(struct fmd_ctx *ctx, const char *system_id, const char *domain_id);
json_t *fmd_redfish_system_memory_chunk(struct fmd_ctx *ctx, const char *system_id, const char *domain_id, const char *chunk_id);

/* Fabric endpoints */
json_t *fmd_redfish_fabrics_collection(struct fmd_ctx *ctx);
json_t *fmd_redfish_fabric(struct fmd_ctx *ctx, const char *fabric_id);
json_t *fmd_redfish_switches_collection(struct fmd_ctx *ctx, const char *fabric_id);
json_t *fmd_redfish_switch(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id);
json_t *fmd_redfish_ports_collection(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id);
json_t *fmd_redfish_port(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id, const char *port_id);
json_t *fmd_redfish_port_metrics(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id, const char *port_id);
json_t *fmd_redfish_vcs_collection(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id);
json_t *fmd_redfish_vcs(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id, const char *vcs_id);
json_t *fmd_redfish_vppb_collection(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id, const char *vcs_id);
json_t *fmd_redfish_vppb(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id, const char *vcs_id, const char *vppb_id);
json_t *fmd_redfish_endpoints_collection(struct fmd_ctx *ctx, const char *fabric_id);
json_t *fmd_redfish_endpoint(struct fmd_ctx *ctx, const char *fabric_id, const char *endpoint_id);
json_t *fmd_redfish_connections_collection(struct fmd_ctx *ctx, const char *fabric_id);
json_t *fmd_redfish_connection(struct fmd_ctx *ctx, const char *fabric_id, const char *connection_id);
json_t *fmd_redfish_zones_collection(struct fmd_ctx *ctx, const char *fabric_id);
json_t *fmd_redfish_zone(struct fmd_ctx *ctx, const char *fabric_id, const char *zone_id);

/* Chassis endpoints */
json_t *fmd_redfish_chassis_collection(struct fmd_ctx *ctx);
json_t *fmd_redfish_chassis(struct fmd_ctx *ctx, const char *id);
json_t *fmd_redfish_pcie_devices(struct fmd_ctx *ctx, const char *chassis_id);
json_t *fmd_redfish_pcie_device(struct fmd_ctx *ctx, const char *chassis_id, const char *dev_id);
json_t *fmd_redfish_cxl_logical_devices(struct fmd_ctx *ctx, const char *chassis_id, const char *pcie_id);
json_t *fmd_redfish_cxl_logical_device(struct fmd_ctx *ctx, const char *chassis_id, const char *pcie_id, const char *ld_id);
json_t *fmd_redfish_memory_collection(struct fmd_ctx *ctx, const char *chassis_id);
json_t *fmd_redfish_memory(struct fmd_ctx *ctx, const char *chassis_id, const char *mem_id);
json_t *fmd_redfish_memory_metrics(struct fmd_ctx *ctx, const char *chassis_id, const char *mem_id);
json_t *fmd_redfish_memory_domains(struct fmd_ctx *ctx, const char *chassis_id);
json_t *fmd_redfish_memory_domain(struct fmd_ctx *ctx, const char *chassis_id, const char *domain_id);
json_t *fmd_redfish_memory_chunks(struct fmd_ctx *ctx, const char *chassis_id, const char *domain_id);
json_t *fmd_redfish_memory_chunk(struct fmd_ctx *ctx, const char *chassis_id, const char *domain_id, const char *chunk_id);

/* POST/DELETE handlers for MemoryChunks provisioning */
int fmd_redfish_post_memory_chunk(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *domain_id, const char *body,
				  size_t body_size, json_t **response);
int fmd_redfish_delete_memory_chunk(struct fmd_ctx *ctx, const char *chassis_id,
				    const char *domain_id, const char *chunk_id);

/*
 * POST/DELETE handlers for Connections. A POST of a Connection there is
 * already returns FMD_CONN_EXISTS, answered 200.
 */
#define FMD_CONN_EXISTS 2
int fmd_redfish_post_connection(struct fmd_ctx *ctx, const char *fabric_id,
				const char *body, size_t body_size,
				json_t **response);
int fmd_redfish_delete_connection(struct fmd_ctx *ctx, const char *fabric_id,
				  const char *connection_id, json_t **response);
int fmd_redfish_action_force_unbind(struct fmd_ctx *ctx, const char *fabric_id,
				    const char *connection_id, json_t **response);

/* Tasks: a bind or unbind in the background (src/tasks.c) */
json_t *fmd_redfish_task_service(struct fmd_ctx *ctx);
json_t *fmd_redfish_tasks(struct fmd_ctx *ctx);
json_t *fmd_redfish_task(struct fmd_ctx *ctx, int id);
int fmd_redfish_task_monitor(struct fmd_ctx *ctx, int id, json_t **response,
			     char *location, size_t len);

/* PATCH handlers */
int fmd_redfish_patch_pcie_device(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *dev_id, const char *body, size_t body_size);
int fmd_redfish_patch_memory(struct fmd_ctx *ctx, const char *chassis_id,
			     const char *mem_id, const char *body, size_t body_size);

/*
 * A request refused because it would send a command fmd_mctp_restricted()
 * names where fmd_mctp_restricted_allowed() does not let it go: answered
 * 409, with nothing sent or changed.
 */
#define FMD_MCTP_RESTRICTED 3

/* POST action handlers */
int fmd_redfish_action_scan_media(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *mem_id, const char *body, size_t body_size);
/*
 * The security actions (src/redfish/handlers.c). Each returns 0, a
 * negative errno (-ENOENT, -EINVAL, -EBUSY, -EAGAIN, -EOPNOTSUPP, -EIO)
 * with *msg saying why where there is more to say, or
 * FMD_MCTP_RESTRICTED.
 *
 * CXLLogicalDevice.{action}: SetPassphrase, DisablePassphrase, Unlock,
 * FreezeSecurityState or PassphraseSecureErase (DSP0288 1.4.0 Table 1).
 */
int fmd_redfish_ld_security_action(struct fmd_ctx *ctx, const char *chassis_id,
				   const char *dev_id, const char *ld_id,
				   const char *action, const char *body,
				   size_t body_size, const char **msg);
/* The deprecated aliases of them on Memory */
int fmd_redfish_action_set_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *mem_id, const char *body, size_t body_size,
				      const char **msg);
int fmd_redfish_action_set_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id, const char *body, size_t body_size,
					     const char **msg);
int fmd_redfish_action_disable_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					  const char *mem_id, const char *body, size_t body_size,
					  const char **msg);
int fmd_redfish_action_disable_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
						 const char *mem_id, const char *body, size_t body_size,
						 const char **msg);
int fmd_redfish_action_freeze_security_state(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id, const char **msg);
int fmd_redfish_action_secure_erase_unit(struct fmd_ctx *ctx, const char *chassis_id,
					 const char *mem_id, const char *body, size_t body_size,
					 const char **msg);
int fmd_redfish_action_reset_ppb(struct fmd_ctx *ctx, const char *switch_id,
				 const char *port_id);

/*
 * Dynamic Capacity handlers (src/redfish/dcd_handlers.c)
 *
 * Write handlers return 0 on success or a negative errno that server.c
 * maps to an HTTP status: -ENOENT 404, -EINVAL 400, -EBUSY/-ENOSPC/-EEXIST 409,
 * -EAGAIN 503, anything else 500. @msg receives a static error message when
 * non-NULL. FMD_DC_ACCEPTED_STALE is a success, 200 with @msg in the body.
 */
json_t *fmd_redfish_dc_capabilities(struct fmd_ctx *ctx, struct fmd_component *dev,
				    const char *chassis_id, const char *dev_id);
json_t *fmd_redfish_dc_pcie_actions(const char *chassis_id, const char *dev_id);
json_t *fmd_redfish_dc_tags_oem(struct fmd_ctx *ctx, struct fmd_component *dev,
				const char *chassis_id, const char *dev_id);
void fmd_redfish_dc_logical_device_props(struct fmd_ctx *ctx, struct fmd_component *dev,
					 int ld_idx, const char *chassis_id,
					 const char *dev_id, const char *ld_id,
					 json_t *result);
json_t *fmd_redfish_dc_memory_extents(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id);
json_t *fmd_redfish_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ext_id);
json_t *fmd_redfish_dc_memory_regions(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *dev_id, const char *ld_id);
json_t *fmd_redfish_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				     const char *dev_id, const char *ld_id,
				     const char *region_id);
int fmd_redfish_patch_dc_memory_region(struct fmd_ctx *ctx, const char *chassis_id,
				       const char *dev_id, const char *ld_id,
				       const char *region_id, const char *body,
				       size_t body_size, const char **msg);
int fmd_redfish_delete_dc_memory_extent(struct fmd_ctx *ctx, const char *chassis_id,
					const char *dev_id, const char *ext_id,
					const char **msg);
int fmd_redfish_action_dc_add(struct fmd_ctx *ctx, const char *chassis_id,
			      const char *dev_id, const char *ld_id,
			      const char *body, size_t body_size, const char **msg);
int fmd_redfish_action_dc_release(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *dev_id, const char *ld_id,
				  const char *body, size_t body_size, const char **msg);
int fmd_redfish_action_dc_reference(struct fmd_ctx *ctx, const char *chassis_id,
				    const char *dev_id, const char *body,
				    size_t body_size, const char **msg);


enum rf_health fmd_map_health_status(uint8_t cci_health_status);
enum rf_state fmd_map_media_status(uint8_t cci_media_status);
uint8_t fmd_worse_media_status(uint8_t a, uint8_t b);

/*
 * The Get Health Info (4200h) of one or more devices or LDs, added up for
 * one Status: see fmd_rollup_status(). Zero it before the first add.
 */
struct fmd_status_rollup {
	uint8_t health_bits;		/* Health Status of each, OR-ed */
	uint8_t media;			/* the worst Media Status */
	unsigned int known;		/* how many were read */
	unsigned int unknown;		/* how many were not */
};

void fmd_rollup_add(struct fmd_status_rollup *r,
		    const struct cxlmi_cmd_memdev_get_health_info_rsp *h, bool valid);
bool fmd_rollup_status(const struct fmd_status_rollup *r,
		       enum rf_health *health, enum rf_state *state);

#endif /* CXL_FMD_H */
