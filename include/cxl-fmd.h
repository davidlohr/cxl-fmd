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

#include <libcxlmi.h>
#include <microhttpd.h>
#include <jansson.h>

#include "generated/enums/resource.h"
#include "generated/enums/cxl_logical_device.h"
#include "generated/enums/pcie_device.h"

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
struct fmd_connection;
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
#define CXL_TO_BYTES(x)    ((x) * CXL_CAPACITY_UNIT)
#define FMD_MIB            (1024ULL * 1024)

/*
 * Maximum Logical Devices per MLD
 */
#define FMD_MAX_LD_COUNT 16
#define FMD_MAX_INTERLEAVE 8    /* Max interleave sets per chunk */

/*
 * Multi-Headed Device (MHD) limits
 */
#define FMD_MAX_HEADS    8      /* Max heads per MHD */
#define FMD_MAX_LD_POOL  64     /* Max LDs in MHD pool (can exceed 16) */

/*
 * Dynamic Capacity Device (DCD) limits (CXL r3.2 Section 7.6.7.6)
 */
#define FMD_MAX_DC_REGIONS 8      /* DC regions per host */
#define FMD_DC_TAG_LEN     16     /* Extent tag is a 16-byte UUID */
#define FMD_MAX_DC_HOSTS   64     /* Hosts tracked per extent (bitmask) */

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
 * - Child of fmd_mhd: LD pool entry (ld_id is pool index, visible_ld_id is what host sees)
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

/* Check if component is a Multi-Headed Device */
static inline bool fmd_is_mhd(struct fmd_component *c)
{
	return c && c->type == FMD_COMPONENT_MHD;
}


/*
 * Fabric root - top-level container for the CXL topology
 *
 * Each libcxlmi endpoint discovers one fabric. The endpoint is stored
 * here and shared by all devices in this topology.
 */
struct fmd_fabric {
	struct fmd_component base;

	char description[256];

	/* libcxlmi endpoint for this fabric (NULL for mock) */
	struct cxlmi_endpoint *ep;

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
 * vPPB Unbind Options (CXL r3.2 Section 7.6.7.2.3)
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
	uint8_t bound_ld_id;         /* LD ID within MLD (if BoundLD) */
	uint16_t bound_pbr_id;       /* PBR ID (if BoundPID) */

	/* Resolved references to physical components */
	struct fmd_port *port;       /* Resolved port pointer */
	struct fmd_ld *ld;           /* Resolved LD pointer (MLD only) */
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
};

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

	/* MLD binding support */
	uint8_t supported_ld_count;
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

	/* Get Health Info (4200h) response */
	struct cxlmi_cmd_memdev_get_health_info_rsp health;

	/* Supported CCI commands bitmask */
	uint64_t supported_cmds;

	/* Memory sizes (derived from memdev response) */
	uint64_t memory_size;
	uint64_t volatile_size;
	uint64_t persistent_size;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
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

	/* QoS Control (5403h) response data */
	struct {
		bool enabled;                           /* Egress Port Congestion Enable */
		bool temp_throughput_reduction_enabled; /* Temporary Throughput Reduction */
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
		uint8_t backpressure_sample_interval;
		uint8_t reqcmp_basis;                   /* ReqCmpBasis */
		uint16_t completion_collection_interval;
	} qos_control;

	/* QoS Status (5405h) response data */
	struct {
		bool backpressure_avg_valid;
		uint8_t backpressure_avg_pct;
	} qos_status;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
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
 * - visible_ld_id is the head-local LD-ID (what host sees, 0-15)
 *
 * Note: The FM-owned LD (FMLD) at LD-ID FFFFh is separate from these
 * host-bindable LDs and is accessed implicitly via MLD component commands.
 * See CXL r3.2 Section 7.6.7.4.
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
	uint8_t visible_ld_id;               /* LD-ID as host sees it (may differ for MHD) */

	/* From Get LD Allocations (5401h) */
	uint64_t memory_size;
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

	/* From tunneled Get Health Info (4200h) */
	struct cxlmi_cmd_memdev_get_health_info_rsp health;

	/* From Get Host DC Region Configuration (5601h) trailer, DCD only */
	uint32_t dc_extents_supported;
	uint32_t dc_extents_available;
	uint32_t dc_tags_supported;
	uint32_t dc_tags_available;
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
 * - fmd_ld.visible_ld_id is the head-local LD-ID the host sees
 *
 * Child of fmd_mhd.
 */
struct fmd_head {
	struct fmd_component base;

	uint8_t head_id;                     /* 0 to num_heads-1 */
	enum fmd_head_type presentation;     /* How this head appears to host */
	enum fmd_head_state state;

	/* For MH-MLD presentation: count of LDs mapped to this head */
	uint8_t ld_count;                    /* Number of LDs assigned to head */

	/* Link properties (from Get MHD Info) */
	uint8_t max_link_width;
	uint8_t negotiated_link_width;

	/* Host attachment info */
	char attached_host_id[FMD_MAX_ID_LEN]; /* Connected host, if known */

	/* Per-head MCTP endpoint for FM access (NULL if using fabric's ep) */
	struct cxlmi_endpoint *ep;
};

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

	/* Multi-Head configuration (from Get MHD Info or identify) */
	uint8_t num_heads;                   /* Number of heads (CXL ports) */
	uint8_t max_heads;                   /* Maximum supported heads */
	bool dynamic_capacity;               /* Supports dynamic capacity */

	/* LD Pool configuration */
	uint8_t total_lds;                   /* Total LDs in pool */
	uint8_t max_lds;                     /* Maximum pool size */
	uint8_t memory_granularity;          /* 256MB units */

	/* QoS capability */
	uint8_t qos_telemetry_capability;    /* Bit 0: Egress Port Backpressure */

	/* QoS Control (device-wide, from Get QoS Control 5403h) */
	struct {
		bool enabled;
		bool temp_throughput_reduction_enabled;
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
		uint8_t backpressure_sample_interval;
		uint8_t reqcmp_basis;
		uint16_t completion_collection_interval;
	} qos_control;

	/* QoS Status (device-wide, from Get QoS Status 5405h) */
	struct {
		bool backpressure_avg_valid;
		uint8_t backpressure_avg_pct;
	} qos_status;

	/* Dynamic capacity capabilities (NULL if not a DCD) */
	struct fmd_dcd_info *dcd;
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

	/* QoS settings */
	struct {
		bool enabled;
		uint8_t egress_moderate_pct;
		uint8_t egress_severe_pct;
	} qos;
};

/*
 * Interleave Set - describes memory interleaving configuration
 */
struct fmd_interleave_set {
	char memory_id[FMD_MAX_ID_LEN];   /* Reference to Memory resource */
	int memory_level;                  /* Interleave level (1-based) */
	uint64_t offset_mib;               /* Offset within interleave set */
	char region_id[32];                /* Region identifier */
	uint64_t size_mib;                 /* Size contributed by this entry */
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
	bool interleaved;
	char address_range_type[16];       /* "PMEM" or "Volatile" */
	int interleave_set_count;
	struct fmd_interleave_set interleave_sets[FMD_MAX_INTERLEAVE];
};


/*
 * Dynamic Capacity policies (CXL r3.2 Table 7-xx, Get DCD Info 5600h)
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
 * (CXL r3.2 Table 7-68). Bit positions follow the CDAT DSMAS Flags field.
 */
#define FMD_DC_REGION_NONVOLATILE   (1 << 2)
#define FMD_DC_REGION_SHAREABLE     (1 << 3)
#define FMD_DC_REGION_HW_COHERENT   (1 << 4)
#define FMD_DC_REGION_INTERCONNECT_DCM (1 << 5)
#define FMD_DC_REGION_READ_ONLY     (1 << 6)

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
 * DC Extent - a block of dynamic capacity allocated to one or more hosts
 *
 * From FM-API Get DC Region Extent Lists (5603h), decorated with
 * Dynamic Capacity List Tags (5608h). Keyed by DPA, device-wide.
 * Child of the fmd_sld/fmd_mld/fmd_mhd.
 * Redfish: PCIeDevices/{dev}/MemoryExtents/{ext}
 */
struct fmd_dc_extent {
	struct fmd_component base;

	uint64_t start_dpa;
	uint64_t len;
	uint8_t  tag[FMD_DC_TAG_LEN];
	uint16_t shared_seq;
	uint8_t  region_id;

	uint64_t host_mask;                 /* bit h: host h has (or was offered) it */
	uint64_t pending_mask;              /* bit h: host h has not accepted yet */
	bool     fm_referenced;             /* FM holds a reference on the tag */
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

	/* Switch attachment (for switch-attached hosts) */
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
 * Connection - binding between host and device memory
 *
 * A connection binds a CXL memory chunk to a host system, making it
 * visible in the host's memory map. For switch-attached topologies,
 * this involves vPPB binding.
 *
 * Connections are stored in a linked list in fmd_ctx.
 */
struct fmd_connection {
	char id[FMD_MAX_ID_LEN];

	/* Endpoints */
	char initiator_id[FMD_MAX_ID_LEN];  /* Host/System ID */
	char target_id[FMD_MAX_ID_LEN];     /* Device (SLD/MLD) ID */

	/* Memory info */
	char chunk_id[FMD_MAX_ID_LEN];      /* MemoryChunk being connected */
	uint8_t access_caps;                 /* FMD_ACCESS_READ | FMD_ACCESS_WRITE */

	/* Switch binding info (for switch-attached) */
	uint8_t vcs_id;
	uint8_t vppb_id;
	bool bound;                          /* vPPB bind succeeded */

	/* Linked list */
	struct fmd_connection *next;
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

	/* Connections (linked list) */
	struct fmd_connection *connections;
	int connection_count;

	/* Redfish HTTP server */
	struct fmd_redfish redfish;

	/* Mock topology (NULL if using real hardware) */
	struct fmd_mock_fabric *mock_fabric;

	/* Runtime state */
	bool running;
	enum fmd_log_level log_level;

	/*
	 * Serializes DCD state changes (refresh, add/release, reference,
	 * region configuration) across threads. The topology rwlock only
	 * covers single add/remove calls, not a whole refresh.
	 */
	pthread_mutex_t dcd_lock;

	/* Configuration */
	struct {
		uint16_t port;
		bool scan_mctp;
		bool scan_ioctl;
		char *mock_topology_path;
	} config;
};

static inline bool fmd_is_mock(struct fmd_ctx *ctx)
{
	return ctx->config.mock_topology_path != NULL;
}

struct fmd_ctx *fmd_ctx_new(void);
void fmd_ctx_free(struct fmd_ctx *ctx);


int fmd_topology_init(struct fmd_topology *topo);
void fmd_topology_cleanup(struct fmd_topology *topo);

/* Add component to topology (handles tree insertion and hash table) */
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

/*
 * Create a memory chunk in a domain with full configuration.
 * offset = UINT64_MAX means auto-assign (append at end of existing chunks).
 * Returns NULL on error (insufficient capacity, overlap, etc.).
 */
struct fmd_mem_chunk *fmd_chunk_create_in_domain(
	struct fmd_topology *topo,
	struct fmd_mem_domain *domain,
	uint64_t size,
	uint64_t offset,
	const char *address_range_type,
	struct fmd_interleave_set *interleave_sets,
	int interleave_set_count);

/*
 * Remove a memory chunk from its domain.
 * Returns 0 on success, -1 on error.
 */
int fmd_chunk_remove(struct fmd_topology *topo, struct fmd_mem_chunk *chunk);

/*
 * Connection management
 */

/**
 * fmd_connection_create - Create a new connection
 * @ctx: FMD context
 * @initiator_id: Host/System ID (initiator endpoint)
 * @target_id: Device ID (target endpoint, SLD or MLD)
 * @chunk_id: MemoryChunk ID being connected
 * @access_caps: Access capabilities (FMD_ACCESS_READ | FMD_ACCESS_WRITE)
 *
 * Creates a connection record and adds it to ctx->connections list.
 * For switch-attached devices, caller should call fmd_vppb_bind() separately.
 *
 * Return: Pointer to new connection, or NULL on error
 */
struct fmd_connection *fmd_connection_create(struct fmd_ctx *ctx,
					     const char *initiator_id,
					     const char *target_id,
					     const char *chunk_id,
					     uint8_t access_caps);

/**
 * fmd_connection_find - Find a connection by ID
 * @ctx: FMD context
 * @id: Connection ID
 *
 * Return: Pointer to connection, or NULL if not found
 */
struct fmd_connection *fmd_connection_find(struct fmd_ctx *ctx, const char *id);

/**
 * fmd_connection_find_by_chunk - Find connections using a specific chunk
 * @ctx: FMD context
 * @chunk_id: MemoryChunk ID
 *
 * Return: Pointer to first matching connection, or NULL if none
 */
struct fmd_connection *fmd_connection_find_by_chunk(struct fmd_ctx *ctx,
						    const char *chunk_id);

/**
 * fmd_connection_remove - Remove and free a connection
 * @ctx: FMD context
 * @conn: Connection to remove
 *
 * Removes the connection from ctx->connections list and frees it.
 * For switch-attached devices, caller should call fmd_vppb_unbind() first.
 *
 * Return: 0 on success, -1 on error
 */
int fmd_connection_remove(struct fmd_ctx *ctx, struct fmd_connection *conn);

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
 * Memory Resource Creation
 *
 * Create memory domain and chunk resources for devices.
 * Used by both hardware discovery and mock topology loading.
 */
int fmd_create_sld_memory_resources(struct fmd_ctx *ctx, struct fmd_sld *sld);
int fmd_create_mld_memory_resources(struct fmd_ctx *ctx, struct fmd_mld *mld);
int fmd_create_mhd_memory_resources(struct fmd_ctx *ctx, struct fmd_mhd *mhd);

/*
 * CCI Tunneling
 * Every component has a direct pointer to the fabric root (comp->fabric),
 * which holds the MCTP endpoint (fabric->ep). No tree walking needed.
 *
 * Tunneling logic:
 * - SLD: No tunneling (switch is transparent)
 * - MLD commands: CXLMI_TUNNEL_SWITCH(port) if behind switch
 * - LD commands: CXLMI_TUNNEL_MLD(ld) or CXLMI_TUNNEL_SWITCH_MLD(port, ld)
 *
 * Usage (with libcxlmi inline macros):
 *   cxlmi_cmd_identify(comp->fabric->ep, CXLMI_NO_TUNNEL, &rsp);
 *   cxlmi_cmd_fmapi_get_ld_info(mld->base.fabric->ep, CXLMI_TUNNEL_SWITCH(port), &rsp);
 *   cxlmi_cmd_memdev_identify(ld->base.fabric->ep, CXLMI_TUNNEL_SWITCH_MLD(port, ld), &rsp);
 */


int fmd_discover(struct fmd_ctx *ctx);

/*
 * VCS Bind/Unbind Operations (CXL r3.2 Section 7.6.7.2)
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
 * operations to update the local view of the switch configuration.
 *
 * Return: 0 on success, negative errno on failure
 */
int fmd_vcs_refresh(struct fmd_ctx *ctx, struct fmd_switch *sw);

/*
 * Get device timestamp via CCI Get Timestamp (0300h)
 * For mock mode, returns current system time.
 * Returns nanoseconds since Unix epoch, or 0 on error.
 */
uint64_t fmd_get_device_timestamp(struct fmd_component *comp);

/*
 * Set device timestamp via CCI Set Timestamp (0301h)
 * For mock mode, accepts silently (no persistence).
 * Returns 0 on success, negative on error.
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
 * For mock mode, updates local state only.
 * Returns 0 on success, negative on error.
 */
int fmd_mld_set_ld_allocation(struct fmd_ctx *ctx, struct fmd_mld *mld,
			      uint8_t ld_id, uint64_t size_bytes);

/*
 * Set LD allocation for an MHD via LD Pool CCI Set LD Allocations (5402h)
 * Uses pool LD index (not head-local LD-ID).
 * For mock mode, updates local state only.
 * Returns 0 on success, negative on error.
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
 * Set volatile/persistent partition for an SLD via CCI Set Partition Info (4101h)
 * For mock mode, updates local state only.
 * If immediate is true, change takes effect immediately; else at next reset.
 * Returns 0 on success, negative on error.
 */
int fmd_sld_set_partition(struct fmd_ctx *ctx, struct fmd_sld *sld,
			  uint64_t volatile_bytes, bool immediate);



/*
 * Dynamic Capacity Device (DCD) management (CXL r3.2 Section 7.6.7.6)
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

/* Bit for @host_id in fmd_dc_extent host/pending masks; 0 when out of range */
static inline uint64_t fmd_dc_host_bit(int host_id)
{
	if (host_id < 0 || host_id >= FMD_MAX_DC_HOSTS)
		return 0;
	return 1ULL << host_id;
}

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
 * Policy tables (CXL r3.2 Table 7-xx) keyed by the FMD_DC_POLICY_* /
 * FMD_DC_REL_POLICY_* wire values; names come from the generated
 * PCIeDevice.CXLDynamicCapacityPolicies enum.
 */
const char *fmd_dc_policy_name(bool release, int value);	/* NULL past the end */
uint16_t fmd_dc_policy_cap_bit(bool release, int value);	/* 5600h bit, 0 if invalid */
int fmd_dc_policy_from_str(bool release, const char *name);	/* -1 if unknown */

/* Create and attach a host's view of a region / a device-wide extent */
struct fmd_dc_region *fmd_dc_region_add(struct fmd_ctx *ctx, struct fmd_component *owner,
					uint8_t region_id, uint16_t host_id);
struct fmd_dc_extent *fmd_dc_extent_add(struct fmd_ctx *ctx, struct fmd_component *dev,
					uint64_t dpa, uint64_t len,
					const uint8_t tag[FMD_DC_TAG_LEN],
					uint16_t shared_seq, uint8_t region_id);

/* Per-host available-extent counter (SLD keeps it on dcd, MLD/MHD on the LD) */
void fmd_dc_adjust_available_extents(struct fmd_component *dev, int host_id, int delta);

/* Component (SLD or LD) that owns the DC regions of a host */
struct fmd_component *fmd_dc_host_component(struct fmd_component *dev, int host_id);

/* Find a host's region by region number */
struct fmd_dc_region *fmd_dc_find_region(struct fmd_component *dev, int host_id,
					 uint8_t region_id);

/* Find the extent that starts at a DPA */
struct fmd_dc_extent *fmd_dc_find_extent(struct fmd_component *dev, uint64_t dpa);

/* Count extents visible to a host in a region */
int fmd_dc_region_extent_count(struct fmd_component *dev, struct fmd_dc_region *region);

/* Hardware discovery: 5600h, then 5601h/5603h per host and 5608h */
int fmd_dcd_discover(struct fmd_ctx *ctx, struct fmd_component *dev);

/* Refresh one host's regions and extents, and the tag list (hardware only) */
int fmd_dcd_refresh(struct fmd_ctx *ctx, struct fmd_component *dev, int host_id);

/*
 * Set DC Region Configuration (5602h)
 * Returns 0, -EINVAL (unsupported value), -EBUSY (extents allocated), -EIO.
 */
int fmd_dc_set_region_config(struct fmd_ctx *ctx, struct fmd_component *dev,
			     struct fmd_dc_region *region, uint64_t block_size,
			     bool sanitize_on_release);

/*
 * Initiate Dynamic Capacity Add (5604h)
 * @policy: FMD_DC_POLICY_*; @extents/@ext_count used for Prescriptive.
 * Returns 0, -EINVAL, -ENOSPC (no room), -ENOENT (region/extent missing), -EIO.
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

/* POST/DELETE handlers for Connections */
int fmd_redfish_post_connection(struct fmd_ctx *ctx, const char *fabric_id,
				const char *body, size_t body_size,
				json_t **response);
int fmd_redfish_delete_connection(struct fmd_ctx *ctx, const char *fabric_id,
				  const char *connection_id);

/* PATCH handlers */
int fmd_redfish_patch_pcie_device(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *dev_id, const char *body, size_t body_size);
int fmd_redfish_patch_memory(struct fmd_ctx *ctx, const char *chassis_id,
			     const char *mem_id, const char *body, size_t body_size);

/* POST action handlers */
int fmd_redfish_action_scan_media(struct fmd_ctx *ctx, const char *chassis_id,
				  const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_set_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_set_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_disable_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					  const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_disable_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
						 const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_freeze_security_state(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id);
int fmd_redfish_action_secure_erase_unit(struct fmd_ctx *ctx, const char *chassis_id,
					 const char *mem_id, const char *body, size_t body_size);
int fmd_redfish_action_reset_ppb(struct fmd_ctx *ctx, const char *switch_id,
				 const char *port_id);

/*
 * Dynamic Capacity handlers (src/redfish/dcd_handlers.c)
 *
 * Write handlers return 0 on success or a negative errno that server.c
 * maps to an HTTP status: -ENOENT 404, -EINVAL 400, -EBUSY/-ENOSPC/-EEXIST 409,
 * anything else 500. @msg receives a static error message when non-NULL.
 */
json_t *fmd_redfish_dc_capabilities(struct fmd_ctx *ctx, struct fmd_component *dev,
				    const char *chassis_id, const char *dev_id);
json_t *fmd_redfish_dc_pcie_actions(const char *chassis_id, const char *dev_id);
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

#endif /* CXL_FMD_H */
