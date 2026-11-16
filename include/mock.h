/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Mock Topology Support
 *
 * Allows running cxl-fmd with simulated CXL topologies for testing
 * without requiring actual hardware.
 *
 * Mock topology is defined in JSON format, supporting:
 * - Direct-attached SLD/MLD devices
 * - Switch-attached topologies with ports
 * - Multi-headed devices
 * - Complex fabric configurations
 */

#ifndef FMD_MOCK_H
#define FMD_MOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <jansson.h>

#include "cxl-fmd.h"

struct fmd_ctx;

/*
 * CCI Command Support Bits (derived from CEL - Command Effects Log)
 *
 * These bits indicate which CCI commands a device supports.
 * Used to populate supported_cmds bitmask.
 */
#define FMD_CMD_IDENTIFY		(1ULL << 0)   /* 0x0001 */
#define FMD_CMD_BG_OP_STATUS		(1ULL << 1)   /* 0x0002 */
#define FMD_CMD_GET_RSP_MSG_LIMIT	(1ULL << 2)   /* 0x0003 */
#define FMD_CMD_SET_RSP_MSG_LIMIT	(1ULL << 3)   /* 0x0004 */
#define FMD_CMD_GET_SUPPORTED_LOGS	(1ULL << 4)   /* 0x0400 */
#define FMD_CMD_GET_LOG			(1ULL << 5)   /* 0x0401 */
#define FMD_CMD_GET_LOG_CAPS		(1ULL << 6)   /* 0x0402 */
#define FMD_CMD_CLEAR_LOG		(1ULL << 7)   /* 0x0403 */
#define FMD_CMD_MEMDEV_IDENTIFY		(1ULL << 16)  /* 0x4000 */
#define FMD_CMD_MEMDEV_CAPACITY		(1ULL << 17)  /* 0x4001 */
#define FMD_CMD_GET_PARTITION_INFO	(1ULL << 18)  /* 0x4100 */
#define FMD_CMD_SET_PARTITION_INFO	(1ULL << 19)  /* 0x4101 */
#define FMD_CMD_GET_HEALTH_INFO		(1ULL << 20)  /* 0x4200 */
#define FMD_CMD_GET_ALERT_CONFIG	(1ULL << 21)  /* 0x4201 */
#define FMD_CMD_SET_ALERT_CONFIG	(1ULL << 22)  /* 0x4202 */
#define FMD_CMD_GET_SHUTDOWN_STATE	(1ULL << 23)  /* 0x4203 */
#define FMD_CMD_SET_SHUTDOWN_STATE	(1ULL << 24)  /* 0x4204 */
#define FMD_CMD_GET_POISON_LIST		(1ULL << 25)  /* 0x4300 */
#define FMD_CMD_INJECT_POISON		(1ULL << 26)  /* 0x4301 */
#define FMD_CMD_CLEAR_POISON		(1ULL << 27)  /* 0x4302 */

/* Default command set for Type 3 memory devices */
#define FMD_CMD_DEFAULT_MEMDEV ( \
	FMD_CMD_IDENTIFY | \
	FMD_CMD_GET_SUPPORTED_LOGS | \
	FMD_CMD_GET_LOG | \
	FMD_CMD_MEMDEV_IDENTIFY | \
	FMD_CMD_GET_PARTITION_INFO | \
	FMD_CMD_GET_HEALTH_INFO \
)

/*
 * Mock fabric - metadata container
 *
 * The actual topology is built directly into ctx->topology using
 * the standard component structures (fmd_switch, fmd_port, fmd_sld, etc.)
 */
struct fmd_mock_fabric {
	char id[FMD_MAX_ID_LEN];
	char description[256];
	int version;

	/*
	 * Devices to pull out of their switch ports, in order: one each time
	 * the daemon is sent SIGUSR1 (see fmd_mock_unplug_next()).
	 */
	char (*unplug)[FMD_MAX_ID_LEN];
	int unplug_count;
	int unplug_next;

	/*
	 * Binds and unbinds made outside cxl-fmd, in order: one each time
	 * the daemon is sent SIGUSR2 (see fmd_mock_external_next()).
	 */
	struct fmd_mock_external *external;
	int external_count;
	int external_next;
};

struct fmd_mock_external {
	char switch_id[FMD_MAX_ID_LEN];
	uint8_t vcs_id;
	uint8_t vppb_id;
	bool bind;                   /* else unbind */
	uint8_t port_id;
	uint16_t ld_id;              /* FFFFh: the whole port */
	bool emit_event;
};

/*
 * Load mock topology from JSON file
 *
 * Parses JSON and builds the topology tree directly using
 * fmd_switch, fmd_port, fmd_sld, fmd_mld, fmd_ld components.
 *
 * Returns 0 on success, -1 on error
 */
int fmd_mock_load(struct fmd_ctx *ctx, const char *path);

/*
 * Free mock fabric resources
 */
void fmd_mock_free(struct fmd_mock_fabric *fabric);

/*
 * Pull the next device of the topology's "unplug" list out of its switch
 * port. Returns 0, or -1 if there is none left. Takes fabric_lock.
 */
int fmd_mock_unplug_next(struct fmd_ctx *ctx);

/*
 * Make the next bind or unbind of the topology's "external" list, as one
 * made by another FM or through another CCI. Returns 0, or -1 if there is
 * none left. Takes fabric_lock.
 */
int fmd_mock_external_next(struct fmd_ctx *ctx);

/*
 * Discover devices from mock topology
 *
 * For mock mode, this is a no-op since fmd_mock_load already
 * builds the topology tree.
 */
int fmd_mock_discover(struct fmd_ctx *ctx);

/*
 * Check if running in mock mode
 */
bool fmd_is_mock_mode(struct fmd_ctx *ctx);

#endif /* FMD_MOCK_H */
