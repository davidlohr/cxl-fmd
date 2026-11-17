// SPDX-License-Identifier: LGPL-2.1
/*
 * Tests for discovery, without hardware.
 *
 * The daemon's --mock mode builds the topology straight from JSON and never
 * runs src/discovery.c. These tests run it: each device is a libcxlmi mock
 * endpoint that answers CCI commands with what the test queued for it, and
 * fmd_discover() and the functions that act on the result are the real ones.
 *
 * Responses are built here byte by byte from the payload tables of the CXL
 * specification (r4.0 table numbers are given), not with libcxlmi's
 * structures, so that the decoding is tested against the specification.
 *
 * Limits of the mock endpoint: it matches on the opcode alone, so the tunnel
 * a command is sent through is not checked, and it only keeps the last
 * command it received.
 *
 * XFAIL() marks a check for a known defect. It does not fail the run; once
 * the defect is fixed it reports XPASS, which does, so that the check gets
 * turned into a CHECK().
 */
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/mctp.h>

#include <libcxlmi.h>
#include <cxlmi/test.h>

#include "cxl-fmd.h"

static int failures, checks, xfails;

#define CHECK(cond, fmt, ...) do {					\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		printf("FAIL  %s: " fmt "\n", __func__, ##__VA_ARGS__);	\
	}								\
} while (0)

#define XFAIL(cond, fmt, ...) do {					\
	checks++;							\
	if (cond) {							\
		failures++;						\
		printf("XPASS %s: " fmt "\n", __func__, ##__VA_ARGS__);	\
	} else {							\
		xfails++;						\
		printf("xfail %s: " fmt "\n", __func__, ##__VA_ARGS__);	\
	}								\
} while (0)

static void report(const char *test, int before)
{
	printf("%s %s\n", failures == before ? "ok   " : "FAIL ", test);
}

/* little-endian fields of a payload */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void put64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = v >> (8 * i);
}

/* a context with nothing to scan: the tests open the endpoints */

static struct fmd_ctx *new_ctx(void)
{
	struct fmd_ctx *ctx = fmd_ctx_new();

	if (!ctx) {
		printf("FAIL  cannot create a context\n");
		exit(1);
	}
	ctx->config.scan_mctp = false;
	ctx->config.scan_ioctl = false;
	ctx->log_level = FMD_LOG_ERROR;

	return ctx;
}

static struct cxlmi_endpoint *new_device(struct fmd_ctx *ctx)
{
	struct cxlmi_endpoint *ep = cxlmi_open_mock(ctx->cxlmi);

	if (!ep) {
		printf("FAIL  cannot open a mock endpoint\n");
		exit(1);
	}

	return ep;
}

/* queue a successful response to @opcode */
static void answer(struct cxlmi_endpoint *ep, uint16_t opcode, void *payload,
		   size_t size)
{
	cxlmi_mock_set_response(ep, opcode >> 8, opcode & 0xff,
				CXLMI_RET_SUCCESS, payload, size);
}

/* queue a response with no payload and return code @rc */
static void answer_rc(struct cxlmi_endpoint *ep, uint16_t opcode, uint16_t rc)
{
	cxlmi_mock_set_response(ep, opcode >> 8, opcode & 0xff, rc, NULL, 0);
}

static uint16_t last_opcode(struct cxlmi_endpoint *ep)
{
	uint8_t set = 0, cmd = 0, payload[256];
	size_t size = sizeof(payload);

	cxlmi_mock_get_last_command(ep, &set, &cmd, payload, &size);
	return set << 8 | cmd;
}

/* the last command sent to @ep and its payload; returns the payload size */
static size_t last_payload(struct cxlmi_endpoint *ep, uint16_t *opcode,
			   uint8_t *payload, size_t size)
{
	uint8_t set = 0, cmd = 0;

	cxlmi_mock_get_last_command(ep, &set, &cmd, payload, &size);
	if (opcode)
		*opcode = set << 8 | cmd;
	return size;
}

static uint64_t get64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static unsigned int commands_sent(struct cxlmi_endpoint *ep)
{
	unsigned int sent = 0;

	cxlmi_mock_get_stats(ep, &sent, NULL);
	return sent;
}

/* Identify (0001h), Table 8-216. Component type: 00h switch, 03h type 3. */
static void answer_identify(struct cxlmi_endpoint *ep, uint64_t serial,
			    uint8_t component_type)
{
	uint8_t pl[0x12] = { 0 };

	put16(pl + 0x00, 0x8086);		/* PCIe Vendor ID */
	put16(pl + 0x02, 0x0d93);		/* PCIe Device ID */
	put64(pl + 0x08, serial);		/* Device Serial Number */
	pl[0x10] = 9;				/* Maximum Supported Message Size */
	pl[0x11] = component_type;
	answer(ep, 0x0001, pl, sizeof(pl));
}

static void device_id(char *id, size_t size, uint64_t serial)
{
	snprintf(id, size, "cxl_%016" PRIx64, serial);
}

/* Identify Memory Device (4000h), Table 8-309. Capacities in 256 MB. */
static void answer_memdev_identify(struct cxlmi_endpoint *ep,
				   uint64_t total, uint64_t vol, uint64_t pers)
{
	uint8_t pl[0x45] = { 0 };

	memcpy(pl, "TEST-1.0", 8);		/* FW Revision */
	put64(pl + 0x10, total);		/* Total Capacity */
	put64(pl + 0x18, vol);			/* Volatile Only Capacity */
	put64(pl + 0x20, pers);			/* Persistent Only Capacity */
	answer(ep, 0x4000, pl, sizeof(pl));
}

/* Get LD Info (5400h), Table 7-46 */
static void answer_ld_info(struct cxlmi_endpoint *ep, uint64_t memory_size,
			   uint16_t ld_count)
{
	uint8_t pl[0x0b] = { 0 };

	put64(pl + 0x00, memory_size);		/* Memory Size */
	put16(pl + 0x08, ld_count);		/* LD Count: LDs supported */
	answer(ep, 0x5400, pl, sizeof(pl));
}

/*
 * Get LD Allocations (5401h), Tables 7-48 and 7-49: @nr LDs, each with
 * Range 1 Allocation Multiplier @mult. Granularity: 0h 256 MB, 1h 512 MB,
 * 2h 1 GB.
 */
static void answer_ld_allocations(struct cxlmi_endpoint *ep, uint8_t nr,
				  uint8_t granularity, uint64_t mult)
{
	uint8_t pl[4 + 16 * 16] = { 0 };

	/*
	 * Asked twice: for one LD, to learn how many are enabled, then for
	 * the rest. The list is min(limit, Number of LDs) entries long.
	 */
	pl[0] = nr;				/* Number of LDs: enabled */
	pl[1] = granularity;			/* Memory Granularity */
	pl[2] = 0;				/* Start LD ID */
	pl[3] = 1;				/* LD Allocation List Length */
	for (int i = 0; i < nr; i++)
		put64(pl + 4 + 16 * i, mult);	/* Range 1 Allocation Multiplier */
	answer(ep, 0x5401, pl, 4 + 16);
	if (nr > 1) {
		pl[2] = 1;
		pl[3] = nr - 1;
		answer(ep, 0x5401, pl, 4 + 16 * (nr - 1));
	}
}

/*
 * A directly attached MLD with @nr LDs of @mult allocation units each, of
 * a Memory Size (Get LD Info) of @memory_size bytes.
 */
static struct cxlmi_endpoint *new_mld_sized(struct fmd_ctx *ctx, uint64_t serial,
					    uint8_t nr, uint8_t granularity,
					    uint64_t mult, uint64_t memory_size)
{
	struct cxlmi_endpoint *ep = new_device(ctx);

	answer_identify(ep, serial, 0x03);
	/* asked twice: to tell an MLD from an SLD, then to discover the MLD */
	answer_ld_info(ep, memory_size, nr);
	answer_ld_info(ep, memory_size, nr);
	answer_ld_allocations(ep, nr, granularity, mult);

	return ep;
}

/* as new_mld_sized(), all of whose Memory Size the LDs have */
static struct cxlmi_endpoint *new_mld(struct fmd_ctx *ctx, uint64_t serial,
				      uint8_t nr, uint8_t granularity,
				      uint64_t mult)
{
	return new_mld_sized(ctx, serial, nr, granularity, mult,
			     (uint64_t)nr * mult * (256ULL << 20 << granularity));
}

/*
 * A directly attached SLD: Identify, then Identify Memory Device. It does
 * not implement the MLD or Multi-Headed command sets, which are prohibited
 * for an SLD (Table 8-397), so those return Unsupported.
 */
static void test_sld(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	int before = failures, rc;
	struct fmd_sld *sld;
	char id[FMD_MAX_ID_LEN];

	answer_identify(ep, 0x1001, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);		/* 16 GiB, volatile */

	rc = fmd_discover(ctx);
	CHECK(rc == 1, "fmd_discover: %d", rc);

	device_id(id, sizeof(id), 0x1001);
	sld = fmd_topology_find_sld(&ctx->topology, id);
	CHECK(sld, "no SLD %s", id);
	if (sld) {
		CHECK(sld->base.ep == ep, "SLD is not on its endpoint");
		CHECK(sld->memory_size == 64ULL << 28,
		      "total capacity %" PRIu64, sld->memory_size);
		CHECK(sld->volatile_size == 64ULL << 28 &&
		      sld->persistent_size == 0, "volatile %" PRIu64
		      ", persistent %" PRIu64, sld->volatile_size,
		      sld->persistent_size);
		CHECK(sld->identify.serial_num == 0x1001, "serial %" PRIx64,
		      (uint64_t)sld->identify.serial_num);
	}
	CHECK(ctx->topology.root->device_count == 1, "%d devices",
	      ctx->topology.root->device_count);
	CHECK(fmd_topology_find_host(&ctx->topology, "local"),
	      "no local host for a directly attached device");

	fmd_ctx_free(ctx);
	report("directly attached SLD", before);
}

/*
 * Two directly attached MLDs, each on its own endpoint. Every component has
 * to be reached through the endpoint it was discovered on, and a command for
 * one device must not go to the other.
 */
static void test_two_mlds(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep[2];
	struct fmd_mld *mld[2];
	int before = failures, rc;
	char id[FMD_MAX_ID_LEN];
	unsigned int sent[2];

	ep[0] = new_mld(ctx, 0xa, 2, 0, 4);
	ep[1] = new_mld(ctx, 0xb, 2, 0, 4);

	rc = fmd_discover(ctx);
	CHECK(rc == 2, "fmd_discover: %d", rc);

	for (int i = 0; i < 2; i++) {
		struct fmd_ld *ld;

		device_id(id, sizeof(id), 0xa + i);
		mld[i] = fmd_topology_find_mld(&ctx->topology, id);
		CHECK(mld[i], "no MLD %s", id);
		if (!mld[i])
			goto out;
		CHECK(mld[i]->base.ep == ep[i], "MLD %d is not on its endpoint",
		      i);
		CHECK(mld[i]->ld_count == 2, "MLD %d: %d LDs", i,
		      mld[i]->ld_count);

		ld = fmd_topology_find_ld(&ctx->topology, id, 1);
		CHECK(ld, "MLD %d: no LD 1", i);
		if (ld) {
			CHECK(ld->base.ep == ep[i],
			      "LD 1 of MLD %d is not on its endpoint", i);
			/* 4 units of 256 MB (granularity 0h) */
			CHECK(ld->memory_size == 4ULL << 28,
			      "LD 1 of MLD %d: %" PRIu64 " bytes", i,
			      ld->memory_size);
		}
	}

	/* Set LD Allocations (5402h) for the second MLD goes to it alone */
	sent[0] = commands_sent(ep[0]);
	sent[1] = commands_sent(ep[1]);
	answer_ld_allocations(ep[1], 2, 0, 8);
	cxlmi_mock_set_response(ep[1], 0x54, 0x02, CXLMI_RET_SUCCESS,
				(uint8_t[4 + 16]){ 1, 0, 0, 0, 8 }, 4 + 16);

	fmd_mld_set_ld_allocation(ctx, mld[1], 0, 8ULL << 28);

	CHECK(commands_sent(ep[0]) == sent[0],
	      "%u commands went to the other MLD",
	      commands_sent(ep[0]) - sent[0]);
	CHECK(commands_sent(ep[1]) > sent[1], "nothing sent to the MLD");
	CHECK(last_opcode(ep[1]) == 0x5402 || last_opcode(ep[1]) == 0x5401,
	      "last command to the MLD: %04xh", last_opcode(ep[1]));
out:
	fmd_ctx_free(ctx);
	report("two MLDs on separate endpoints", before);
}

/*
 * Memory Granularity (Table 7-48): the allocation multipliers of
 * Table 7-49 are in units of 256 MB, 512 MB or 1 GB.
 */
static void test_mld_granularity(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mld *mld;
	struct fmd_ld *ld;

	ep = new_mld(ctx, 0xc, 2, 1, 4);	/* 4 units of 512 MB */
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xc);
	ld = fmd_topology_find_ld(&ctx->topology, id, 0);
	CHECK(ld, "no LD 0");
	if (ld)
		CHECK(ld->memory_size == 4ULL << 29, "LD size %" PRIu64,
		      ld->memory_size);

	/* and back: 4 GiB in units of 512 MB is a multiplier of 8 */
	mld = fmd_topology_find_mld(&ctx->topology, id);
	CHECK(mld, "no MLD");
	if (mld) {
		uint8_t set = 0, cmd = 0, pl[64] = { 0 };
		size_t size = sizeof(pl);

		CHECK(mld->memory_granularity == 2,
		      "granularity %d units of 256 MB",
		      mld->memory_granularity);

		/* the allocation as it is now, which Set LD Allocations keeps */
		answer(ep, 0x5401, (uint8_t[4 + 16]){ 2, 1, 0, 1, 4 }, 4 + 16);
		cxlmi_mock_set_response(ep, 0x54, 0x02, CXLMI_RET_SUCCESS,
					(uint8_t[4 + 16]){ 1, 0, 0, 0, 8 },
					4 + 16);
		fmd_mld_set_ld_allocation(ctx, mld, 0, 4ULL << 30);
		cxlmi_mock_get_last_command(ep, &set, &cmd, pl, &size);
		CHECK(set == 0x54 && cmd == 0x02,
		      "last command %02x%02xh", set, cmd);
		/* Table 7-50: multiplier of the first LD at offset 4h */
		CHECK(pl[4] == 8 && pl[5] == 0,
		      "Range 1 Allocation Multiplier %d", pl[4]);
		if (ld)
			CHECK(ld->memory_size == 4ULL << 30,
			      "LD size after set %" PRIu64, ld->memory_size);
	}

	fmd_ctx_free(ctx);
	report("MLD with 512 MB granularity", before);
}

/*
 * Set LD Allocations (5402h) rewrites both ranges of an LD (Tables 7-49
 * and 7-50), each a multiplier of the Memory Granularity. Range 2 stays as
 * Get LD Allocations reads it, and the size must be a whole number of
 * granules.
 */
static void test_ld_allocation_ranges(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld(ctx, 0xd, 2, 1, 4);
	int before = failures, rc;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mld *mld;
	struct fmd_ld *ld;
	uint8_t pl[64] = { 0 }, get[4 + 16] = { 2, 1, 0, 1 };
	uint16_t opcode = 0;
	unsigned int sent;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xd);
	mld = fmd_topology_find_mld(&ctx->topology, id);
	ld = fmd_topology_find_ld(&ctx->topology, id, 0);
	CHECK(mld && ld, "no MLD or LD 0");
	if (!mld || !ld)
		goto out;

	/* LD 0: Range 1 of 2 and Range 2 of 2 units of 512 MB */
	put64(get + 4, 2);
	put64(get + 12, 2);

	/* to 4 GiB, 8 units: Range 1 becomes 6, Range 2 stays 2 */
	answer(ep, 0x5401, get, sizeof(get));
	{
		uint8_t set_rsp[4 + 16] = { 1, 0 };

		put64(set_rsp + 4, 6);
		put64(set_rsp + 12, 2);
		answer(ep, 0x5402, set_rsp, sizeof(set_rsp));
	}
	rc = fmd_mld_set_ld_allocation(ctx, mld, 0, 4ULL << 30);
	last_payload(ep, &opcode, pl, sizeof(pl));
	CHECK(rc == 0 && opcode == 0x5402, "set: %d, last command %04xh", rc,
	      opcode);
	CHECK(get64(pl + 4) == 6 && get64(pl + 12) == 2,
	      "Range 1 %" PRIu64 ", Range 2 %" PRIu64, get64(pl + 4),
	      get64(pl + 12));
	CHECK(ld->memory_size == 4ULL << 30, "LD size after set %" PRIu64,
	      ld->memory_size);

	/* not a whole number of 512 MB, or less than Range 2: not sent */
	put64(get + 4, 6);			/* as the set left it */
	answer(ep, 0x5401, get, sizeof(get));
	sent = commands_sent(ep);
	rc = fmd_mld_set_ld_allocation(ctx, mld, 0, (4ULL << 30) + (256 << 20));
	CHECK(rc == -EINVAL && commands_sent(ep) == sent + 1 &&
	      last_opcode(ep) == 0x5401, "4.25 GiB: %d, last command %04xh",
	      rc, last_opcode(ep));
	answer(ep, 0x5401, get, sizeof(get));
	rc = fmd_mld_set_ld_allocation(ctx, mld, 0, 512 << 20);
	CHECK(rc == -EINVAL && last_opcode(ep) == 0x5401,
	      "below Range 2: %d, last command %04xh", rc, last_opcode(ep));
	CHECK(ld->memory_size == 4ULL << 30, "LD size after refusals %" PRIu64,
	      ld->memory_size);
out:
	fmd_ctx_free(ctx);
	report("Set LD Allocations keeps Range 2", before);
}

/* one port information block of Get Physical Port State, Table 7-21 */
static void port_block(uint8_t *b, uint8_t port, uint8_t state, uint8_t mode,
		       uint8_t dev_type, uint8_t ltssm, uint16_t flags)
{
	memset(b, 0, 16);
	b[0x0] = port;				/* Port ID */
	b[0x1] = state;				/* Bits[3:0] config state, Bit[4] GAE */
	b[0x2] = mode;				/* Connected Device Mode */
	b[0x4] = dev_type;			/* Connected Device Type */
	b[0x5] = 0x06;				/* Supported CXL Modes: 68B, 256B */
	b[0x6] = 16;				/* Maximum Link Width */
	b[0x7] = 8;				/* Negotiated Link Width */
	b[0x8] = 0x1f;				/* Supported Link Speeds Vector */
	b[0x9] = 5;				/* Max Link Speed */
	b[0xa] = 5;				/* Current Link Speed */
	b[0xb] = ltssm;				/* LTSSM State: 04h is L0 */
	put16(b + 0xd, flags);			/* Link State Flags */
	b[0xf] = 0;				/* Supported LD Count */
}

/*
 * A switch with four ports and two VCSs, nothing attached that needs
 * commands of its own:
 *
 *   port 0  USP of VCS 0, link up
 *   port 1  USP of VCS 1, link up, GAE Support bit set
 *   port 2  DSP, link up, PCIe device, bound to vPPB 1 of VCS 0
 *   port 3  disabled
 */
/*
 * Get Virtual CXL Switch Info (5200h) for one VCS of three vPPBs, Tables 7-33
 * and 7-34: vPPB 0 bound to the USP, then vPPBs 1 and 2 as given, each
 * { Binding Status, Bound Port ID, Bound LD ID }.
 */
static const uint8_t unbound[3] = { 0x00, 0x00, 0xff };

static void answer_vcs_state(struct cxlmi_endpoint *ep, uint8_t vcs_id,
			     uint8_t state, uint8_t usp,
			     const uint8_t *vppb1, const uint8_t *vppb2)
{
	uint8_t pl[4 + 4 + 3 * 4] = { 0 }, *b = pl + 4;

	pl[0] = 1;				/* Number of VCSs */
	b[0] = vcs_id;				/* Virtual CXL Switch ID */
	b[1] = state;				/* VCS State: 00h, 01h enabled */
	b[2] = usp;				/* USP ID */
	b[3] = 3;				/* Number of vPPBs */
	b[4] = 0x02;				/* vPPB 0: bound physical port */
	b[5] = usp;
	b[6] = 0xff;
	memcpy(b + 8, vppb1, 3);
	memcpy(b + 12, vppb2, 3);
	answer(ep, 0x5200, pl, sizeof(pl));
}

static void answer_vcs(struct cxlmi_endpoint *ep, uint8_t vcs_id, uint8_t usp,
		       const uint8_t *vppb1, const uint8_t *vppb2)
{
	answer_vcs_state(ep, vcs_id, 0x01, usp, vppb1, vppb2);
}

/*
 * A switch with six ports and two VCSs, whose IDs are 0 and 2:
 *
 *   port 0  USP of VCS 0             port 3  disabled
 *   port 1  USP of VCS 2             port 4  DSP, nothing attached
 *   port 2  DSP, PCIe device,        port 5  DSP, MLD, LD-IDs 0 and 1
 *           bound to VCS 0 vPPB 1
 */
static const uint8_t port2[3] = { 0x02, 2, 0xff };

/*
 * Identify Switch Device and Get Physical Port State of that switch, with
 * port 4 as given: its Connected Device Type and LTSSM State.
 */
static void answer_switch_ports(struct cxlmi_endpoint *ep, uint8_t claimed_ports,
				uint8_t port4_type, uint8_t port4_ltssm)
{
	uint8_t id[0x49] = { 0 }, ports[4 + 6 * 16] = { 0 };

	/* Identify Switch Device (5100h), Table 7-18 */
	id[0x02] = 6;				/* Number of Physical Ports */
	id[0x03] = 3;				/* Number of VCSs */
	id[0x04] = 0x3f;			/* Active Port Bitmask */
	id[0x24] = 0x05;			/* Active VCS Bitmask: 0 and 2 */
	put16(id + 0x44, 6);			/* Total Number of vPPBs */
	put16(id + 0x46, 3);			/* Number of Bound vPPBs */
	id[0x48] = 2;				/* Number of HDM Decoders */
	answer(ep, 0x5100, id, sizeof(id));

	/* Get Physical Port State (5101h), Table 7-20 */
	ports[0] = claimed_ports;		/* Number of Ports */
	port_block(ports + 4 + 0 * 16, 0, 0x04, 0, 0x00, 0x04, 0x0004);
	port_block(ports + 4 + 1 * 16, 1, 0x14, 0, 0x00, 0x04, 0x0000);
	port_block(ports + 4 + 2 * 16, 2, 0x03, 0x02, 0x01, 0x04, 0x0004);
	port_block(ports + 4 + 3 * 16, 3, 0x00, 0, 0x00, 0x00, 0x0004);
	port_block(ports + 4 + 4 * 16, 4, 0x03, port4_type ? 0x02 : 0,
		   port4_type, port4_ltssm, port4_type ? 0x0004 : 0x0000);
	port_block(ports + 4 + 5 * 16, 5, 0x03, 0x02, 0x05, 0x04, 0x0004);
	ports[4 + 5 * 16 + 0xf] = 1;		/* Supported LD Count: 1 more */
	answer(ep, 0x5101, ports, sizeof(ports));
}

static struct cxlmi_endpoint *new_switch_claiming(struct fmd_ctx *ctx,
						  uint64_t serial,
						  uint8_t claimed_ports)
{
	struct cxlmi_endpoint *ep = new_device(ctx);

	answer_identify(ep, serial, 0x00);
	answer_switch_ports(ep, claimed_ports, 0x00, 0x00);

	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);

	return ep;
}

static struct cxlmi_endpoint *new_switch(struct fmd_ctx *ctx, uint64_t serial)
{
	return new_switch_claiming(ctx, serial, 6);
}

static void test_switch(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5000);
	int before = failures, rc;
	struct fmd_switch *sw;
	struct fmd_port *port;
	char id[FMD_MAX_ID_LEN];

	rc = fmd_discover(ctx);
	CHECK(rc == 1, "fmd_discover: %d", rc);

	device_id(id, sizeof(id), 0x5000);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	CHECK(sw->base.ep == ep, "switch is not on its endpoint");
	CHECK(ctx->topology.root->port_count == 6, "%d ports",
	      ctx->topology.root->port_count);
	CHECK(sw->num_vcs == 2, "%d VCSs", sw->num_vcs);
	CHECK(sw->num_usps == 2 && sw->num_dsps == 3, "%d USPs, %d DSPs",
	      sw->num_usps, sw->num_dsps);
	CHECK(sw->vcs[0].vcs_id == 0 && sw->vcs[0].upstream_port_id == 0 &&
	      sw->vcs[0].num_vppbs == 3, "VCS 0: id %d, USP %d, %d vPPBs",
	      sw->vcs[0].vcs_id, sw->vcs[0].upstream_port_id,
	      sw->vcs[0].num_vppbs);
	CHECK(sw->vcs[0].vppbs[1].bound_port_id == 2,
	      "vPPB 1 of VCS 0 bound to port %d",
	      sw->vcs[0].vppbs[1].bound_port_id);
	CHECK(ctx->topology.root->host_count == 2, "%d hosts",
	      ctx->topology.root->host_count);

	/* a DSP with a PCIe device, in L0 */
	port = fmd_topology_find_port(&ctx->topology, id, 2);
	CHECK(port, "no port 2");
	if (port) {
		CHECK(port->base.ep == ep, "port is not on the switch endpoint");
		CHECK(port->state == FMD_PORT_DSP, "port 2 state %d",
		      port->state);
		CHECK(port->conn_dev_type == FMD_CONN_PCIE,
		      "port 2 device type %d", port->conn_dev_type);
		CHECK(port->link_status == FMD_LINK_UP, "port 2 link %d",
		      port->link_status);
		CHECK(port->max_link_width == 16 &&
		      port->negotiated_link_width == 8,
		      "port 2 widths %d/%d", port->max_link_width,
		      port->negotiated_link_width);
	}

	/* a USP */
	port = fmd_topology_find_port(&ctx->topology, id, 0);
	CHECK(port, "no port 0");
	if (port)
		CHECK(port->state == FMD_PORT_USP, "port 0 state %d",
		      port->state);

	/*
	 * Current Port Configuration State is Bits[3:0] of byte 1h; Bit[4] is
	 * GAE Support (Table 7-21). Port 1 reports 14h: a USP.
	 */
	port = fmd_topology_find_port(&ctx->topology, id, 1);
	CHECK(port, "no port 1");
	if (port) {
		CHECK(port->state == FMD_PORT_USP, "port 1 state %d",
		      port->state);
		/*
		 * The link state is the LTSSM State, 04h for L0. Link State
		 * Flags are lane reversal, PERST#, PRSNT# and power, and are
		 * all 0 for this port.
		 */
		CHECK(port->link_status == FMD_LINK_UP, "port 1 link %d",
		      port->link_status);
	}

	/*
	 * A disabled port is not a DSP. It is in Detect (00h) with PRSNT#
	 * set in Link State Flags: present, but the link is down.
	 */
	port = fmd_topology_find_port(&ctx->topology, id, 3);
	CHECK(port, "no port 3");
	if (port)
		CHECK(port->link_status == FMD_LINK_DOWN, "port 3 link %d",
		      port->link_status);
	if (port)
		CHECK(port->state == FMD_PORT_DISABLED, "port 3 state %d",
		      port->state);

out:
	fmd_ctx_free(ctx);
	report("switch with two VCSs", before);
}

/*
 * The Port resource of a discovered switch. DSP0288 maps Get Physical Port
 * State to it: Max Link Speed to MaxSpeedGbps, Current Link Speed to
 * CurrentSpeedGbps, the Supported Link Speeds Vector to
 * LinkConfiguration/CapableLinkSpeedGbps, and the widths.
 */
static void test_port_resource(void)
{
	struct fmd_ctx *ctx = new_ctx();
	int before = failures;
	char sw_id[FMD_MAX_ID_LEN], port_id[FMD_MAX_ID_LEN + 8];
	json_t *port, *cxl, *speeds;
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5006);
	unsigned int sent;
	int rc;

	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x5006);

	/* port 1 is a USP: named as one, and an UpstreamPort */
	snprintf(port_id, sizeof(port_id), "%s_usp1", sw_id);
	port = fmd_redfish_port(ctx, "CXL", sw_id, port_id);
	CHECK(port, "no Port resource %s", port_id);
	if (port) {
		const char *type = json_string_value(json_object_get(port, "PortType"));

		cxl = json_object_get(port, "CXL");
		CHECK(type && !strcmp(type, "UpstreamPort"), "USP PortType %s",
		      type ? type : "(none)");
		/* reserved unless the port is a DSP */
		CHECK(!json_object_get(cxl, "ConnectedDeviceMode"),
		      "a USP reports a Connected Device Mode");
		/*
		 * Physical Port Control "is used by the FM to control unbound
		 * ports and MLD ports" (Section 7.6.7.1.3). cxl-fmd offers Reset
		 * PPB only on a DSP: not on a USP, and it sends nothing there.
		 */
		CHECK(!json_object_get(port, "Actions"), "a USP offers Reset PPB");
		json_decref(port);
	}
	sent = commands_sent(ep);
	rc = fmd_redfish_action_reset_ppb(ctx, sw_id, port_id);
	CHECK(rc == -2 && commands_sent(ep) == sent,
	      "Reset PPB of a USP: %d, %u commands sent", rc,
	      commands_sent(ep) - sent);

	/* port 2: a DSP in L0, x16 trained to x8, 32 GT/s of 2.5 to 32 */
	snprintf(port_id, sizeof(port_id), "%s_dsp2", sw_id);
	port = fmd_redfish_port(ctx, "CXL", sw_id, port_id);
	CHECK(port, "no Port resource %s", port_id);
	if (port) {
		const char *type = json_string_value(json_object_get(port, "PortType"));

		CHECK(type && !strcmp(type, "DownstreamPort"), "DSP PortType %s",
		      type ? type : "(none)");
		CHECK(json_number_value(json_object_get(port, "MaxSpeedGbps")) == 32 &&
		      json_number_value(json_object_get(port, "CurrentSpeedGbps")) == 32,
		      "speeds %g/%g",
		      json_number_value(json_object_get(port, "MaxSpeedGbps")),
		      json_number_value(json_object_get(port, "CurrentSpeedGbps")));
		CHECK(json_integer_value(json_object_get(port, "Width")) == 16 &&
		      json_integer_value(json_object_get(port, "ActiveWidth")) == 8,
		      "widths");
		speeds = json_object_get(json_array_get(
				json_object_get(port, "LinkConfiguration"), 0),
				"CapableLinkSpeedGbps");
		CHECK(json_array_size(speeds) == 5 &&
		      json_number_value(json_array_get(speeds, 0)) == 2.5 &&
		      json_number_value(json_array_get(speeds, 4)) == 32,
		      "%d capable speeds from %g", (int)json_array_size(speeds),
		      json_number_value(json_array_get(speeds, 0)));
		CHECK(json_object_get(json_object_get(port, "Actions"),
				      "#Port.ResetPPB"), "a DSP offers no Reset PPB");
		json_decref(port);
	}
	/* Physical Port Control (5102h), Port Opcode 02h: Reset PPB */
	answer_rc(ep, 0x5102, CXLMI_RET_SUCCESS);
	rc = fmd_redfish_action_reset_ppb(ctx, sw_id, port_id);
	CHECK(rc == 0 && last_opcode(ep) == 0x5102,
	      "Reset PPB of a DSP: %d, last command %04xh", rc, last_opcode(ep));

	/*
	 * 128 GT/s, Max and Current Link Speed encoding 7 as in PCIe 7.0
	 * (CXL 4.0 "doubles the data rate to 128 GT/s", Section 1.4.1). The
	 * Supported Link Speeds Vector is Bits[5:0], Bits[7:6] Reserved
	 * (Table 7-21): bit 6 is not a speed.
	 */
	{
		struct fmd_port *p2 = fmd_topology_find_port(&ctx->topology,
							    sw_id, 2);

		CHECK(p2, "no port 2");
		if (p2) {
			p2->max_link_speed = 7;
			p2->current_link_speed = 7;
			p2->supported_link_speeds = 0x7f;
		}
		snprintf(port_id, sizeof(port_id), "%s_dsp2", sw_id);
		port = fmd_redfish_port(ctx, "CXL", sw_id, port_id);
		CHECK(port, "no Port resource %s", port_id);
		if (port) {
			speeds = json_object_get(json_array_get(
					json_object_get(port, "LinkConfiguration"), 0),
					"CapableLinkSpeedGbps");
			CHECK(json_number_value(json_object_get(port, "MaxSpeedGbps")) == 128 &&
			      json_number_value(json_object_get(port, "CurrentSpeedGbps")) == 128,
			      "128 GT/s: speeds %g/%g",
			      json_number_value(json_object_get(port, "MaxSpeedGbps")),
			      json_number_value(json_object_get(port, "CurrentSpeedGbps")));
			CHECK(json_array_size(speeds) == 6,
			      "%d capable speeds from a vector of 7Fh",
			      (int)json_array_size(speeds));
			json_decref(port);
		}
	}

	/* port 3 is disabled: it has no direction, and its link is down */
	snprintf(port_id, sizeof(port_id), "%s_dsp3", sw_id);
	port = fmd_redfish_port(ctx, "CXL", sw_id, port_id);
	CHECK(port, "no Port resource %s", port_id);
	if (port) {
		CHECK(!json_object_get(port, "PortType"),
		      "a disabled port has a PortType");
		CHECK(json_number_value(json_object_get(port, "CurrentSpeedGbps")) == 0,
		      "a port that is down has a current speed");
		json_decref(port);
	}

	fmd_ctx_free(ctx);
	report("Port resource of a discovered switch", before);
}

/* bind, and say whether Bind vPPB (5201h) went to the switch */
static int try_bind(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
		    struct fmd_switch *sw, uint8_t vcs_id, uint8_t vppb_id,
		    uint8_t port_id, uint16_t ld_id, bool *sent)
{
	int rc = fmd_vppb_bind(ctx, sw, vcs_id, vppb_id, port_id, ld_id);

	*sent = (last_opcode(ep) == 0x5201);
	return rc;
}

/*
 * Bind vPPB (5201h), Section 7.6.7.2.2 and Table 7-35, and what Section
 * 7.2.3.1 says may be bound: "The Fabric Manager can bind any unused physical
 * port to the unbound vPPB". An SLD port is bound to only one VCS, and each
 * LD of an MLD is bound to one host or is unbound (Section 7.3.4).
 */
static void test_switch_bind(void)
{
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5003);
	int before = failures, rc;
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	char id[FMD_MAX_ID_LEN];
	bool sent;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x5003);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	/* the VCS IDs are 0 and 2: 2 is a VCS, 1 is not */
	CHECK(sw->num_vcs == 2 && fmd_switch_find_vcs(sw, 2) &&
	      !fmd_switch_find_vcs(sw, 1), "%d VCSs", sw->num_vcs);
	rc = try_bind(ctx, ep, sw, 1, 1, 4, 0xffff, &sent);
	CHECK(rc == -EINVAL && !sent, "bind in VCS 1, which is no VCS: %d", rc);

	/* port 2 is bound to VCS 0: it is not an unused port */
	rc = try_bind(ctx, ep, sw, 2, 1, 2, 0xffff, &sent);
	CHECK(rc == -EBUSY && !sent, "bind of a port bound to another VCS: %d",
	      rc);

	/* nor is port 0, the USP of VCS 0 */
	rc = try_bind(ctx, ep, sw, 2, 1, 0, 0xffff, &sent);
	CHECK(rc == -EBUSY && !sent, "bind of a USP: %d", rc);

	/*
	 * A vPPB that is bound is not an unbound vPPB. That is told apart
	 * from a target in use: a vPPB taken since it was chosen is not the
	 * client's conflict.
	 */
	rc = try_bind(ctx, ep, sw, 0, 1, 4, 0xffff, &sent);
	CHECK(rc == -EALREADY && !sent, "bind of a bound vPPB: %d", rc);

	/*
	 * LD ID: the LD-ID if the target port is an MLD port, FFFFh for
	 * other EP types (Table 7-35). Port 5 has Supported LD Count 1, the
	 * number of additional LDs: LD-IDs 0 and 1.
	 */
	rc = try_bind(ctx, ep, sw, 2, 1, 5, 0xffff, &sent);
	CHECK(rc == -EINVAL && !sent, "bind of an MLD port without an LD: %d",
	      rc);
	rc = try_bind(ctx, ep, sw, 2, 1, 4, 0, &sent);
	CHECK(rc == -EINVAL && !sent, "bind of an LD of a port with no MLD: %d",
	      rc);
	rc = try_bind(ctx, ep, sw, 2, 1, 5, 2, &sent);
	CHECK(rc == -EINVAL && !sent, "bind of LD 2 of an MLD with LDs 0 and 1: %d",
	      rc);

	/*
	 * LD 1 of port 5 to vPPB 1 of VCS 2. The switch starts the bind in
	 * the background, and then reports the vPPB bound. The VCSs are read
	 * before the bind and after it.
	 */
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld1, unbound);
	rc = try_bind(ctx, ep, sw, 2, 1, 5, 1, &sent);
	CHECK(rc == FMD_OP_STARTED, "bind of LD 1 of port 5: %d", rc);

	vcs = fmd_switch_find_vcs(sw, 2);
	CHECK(vcs, "VCS 2 is gone after a bind");
	if (vcs) {
		struct fmd_vppb *vppb = fmd_vcs_find_vppb(vcs, 1);

		CHECK(vppb && vppb->binding_status == FMD_VPPB_BOUND_LD &&
		      vppb->bound_port_id == 5 && vppb->bound_ld_id == 1,
		      "vPPB 1 of VCS 2 after the bind: status %d",
		      vppb ? (int)vppb->binding_status : -1);
		/* and it points at the port it is now bound to */
		CHECK(vppb && vppb->port ==
		      fmd_topology_find_port(&ctx->topology, id, 5),
		      "vPPB 1 of VCS 2 does not point at port 5");
	}

	/* LD 1 is now bound to a host; LD 0 of the same MLD is not */
	rc = try_bind(ctx, ep, sw, 0, 2, 5, 1, &sent);
	CHECK(rc == -EBUSY && !sent, "bind of an LD bound to another VCS: %d",
	      rc);
	answer_rc(ep, 0x5201, CXLMI_RET_SUCCESS);
	rc = try_bind(ctx, ep, sw, 0, 2, 5, 0, &sent);
	CHECK(rc == 0, "bind of the other LD of the MLD: %d", rc);

	/*
	 * A switch that cannot be asked about its VCSs has not unbound
	 * anything: what was last known stays.
	 */
	fmd_vcs_refresh(ctx, sw);
	vcs = fmd_switch_find_vcs(sw, 2);
	CHECK(sw->num_vcs == 2 && vcs &&
	      vcs->vppbs[1].binding_status == FMD_VPPB_BOUND_LD,
	      "bindings lost when Get Virtual CXL Switch Info failed");

	/* Unbind vPPB (5202h) takes the VCS ID as well (Table 7-36) */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_unbind(ctx, sw, 2, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == FMD_OP_STARTED, "unbind in VCS 2: %d", rc);
	/* all other encodings of the Unbind Option are reserved */
	rc = fmd_vppb_unbind(ctx, sw, 2, 1, (enum fmd_unbind_option)3);
	CHECK(rc == -EINVAL, "unbind with a reserved option: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("binding on a switch", before);
}

/* Unbind vPPB (5202h) runs in the background too (Section 7.6.7.2.3) */
static void test_switch_unbind(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5001);
	int before = failures, rc;
	struct fmd_switch *sw;
	char id[FMD_MAX_ID_LEN];

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x5001);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	/* a refusal by the switch is an error */
	answer_rc(ep, 0x5202, CXLMI_RET_INPUT);
	rc = fmd_vppb_unbind(ctx, sw, 0, 1, FMD_UNBIND_WAIT_FOR_CLEAN);
	CHECK(last_opcode(ep) == 0x5202, "unbind not sent: last command %04xh",
	      last_opcode(ep));
	CHECK(rc != 0, "unbind returns %d for Invalid Input", rc);

	/* its starting the unbind is not */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_unbind(ctx, sw, 0, 1, FMD_UNBIND_WAIT_FOR_CLEAN);
	CHECK(rc == FMD_OP_STARTED,
	      "unbind returns %d for Background Command Started", rc);
out:
	fmd_ctx_free(ctx);
	report("unbind on a switch", before);
}

/*
 * A device that is not ready answers Identify with Retry Required
 * (Section 7.6.6.1). It is not discovered, and nothing else is sent to it.
 */
/*
 * Devices that say they returned more list entries than were asked for.
 * The count is the device's to choose; what was asked for is all there is
 * room for. Run under ASan, these are overflows unless the count is bounded.
 *
 * A libcxlmi that bounds the count itself fails the command, so what is
 * checked is that nothing past what was asked for is used, not that the
 * rest of the response is.
 */
static void test_overcount_switch(void)
{
	struct fmd_ctx *ctx = new_ctx();
	int before = failures, ports = 0;
	char id[FMD_MAX_ID_LEN];

	new_switch_claiming(ctx, 0x5002, 255);	/* 6 ports asked for */
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0x5002);
	CHECK(fmd_topology_find_switch(&ctx->topology, id), "no switch %s", id);
	for (int i = 0; i < 256; i++)
		if (fmd_topology_find_port(&ctx->topology, id, i))
			ports++;
	CHECK(ports <= 6, "%d ports from a response to a request for 6", ports);

	fmd_ctx_free(ctx);
	report("switch returning more ports than asked", before);
}

static void test_overcount_mld(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t alloc[4 + 2 * 16] = { 0 }, bw[4 + 2] = { 0 };
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mld *mld;
	struct fmd_ld *ld;

	answer_identify(ep, 0xd, 0x03);
	answer_ld_info(ep, 2ULL << 30, 2);
	answer_ld_info(ep, 2ULL << 30, 2);

	/* Get LD Allocations (5401h): 2 LDs asked for, 200 "returned" */
	alloc[0] = 2;
	alloc[3] = 200;				/* LD Allocation List Length */
	put64(alloc + 4, 4);
	put64(alloc + 4 + 16, 4);
	answer(ep, 0x5401, alloc, sizeof(alloc));

	/* Get QoS Allocated BW (5406h) and BW Limit (5408h), likewise */
	bw[0] = 255;				/* Number of LDs */
	bw[2] = 50;
	bw[3] = 50;
	answer(ep, 0x5406, bw, sizeof(bw));
	answer(ep, 0x5408, bw, sizeof(bw));

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xd);
	mld = fmd_topology_find_mld(&ctx->topology, id);
	CHECK(mld, "no MLD %s", id);
	ld = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(ld, "no LD 1");
	if (ld) {
		CHECK(ld->memory_size == 4ULL << 28 || ld->memory_size == 0,
		      "LD 1: %" PRIu64 " bytes", ld->memory_size);
		CHECK(ld->qos_allocated_bw == 50 || ld->qos_allocated_bw == 0,
		      "LD 1 allocated BW %d", ld->qos_allocated_bw);
	}
	CHECK(!fmd_topology_find_ld(&ctx->topology, id, 2),
	      "an LD past the 2 the MLD has");

	fmd_ctx_free(ctx);
	report("MLD returning more LDs than asked", before);
}

/*
 * A Multi-Headed device: Get Multi-Headed Info (5500h) on the LD Pool CCI,
 * Tables 7-60 and 7-61, asked once to tell what the device is and once to
 * discover it, then Get Head Info (5501h).
 */
static void test_mhd(bool overcount)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t probe[8 + 1] = { 0 }, info[8 + 4] = { 0 };
	uint8_t heads[4 + 2 * 9] = { 0 };
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mhd *mhd;

	answer_identify(ep, 0xe, 0x03);

	probe[0] = 4;				/* Number of LDs */
	probe[1] = 2;				/* Number of Heads */
	probe[5] = 1;				/* LD Map Length */
	answer(ep, 0x5500, probe, sizeof(probe));

	info[0] = 4;
	info[1] = 2;
	info[5] = overcount ? 200 : 4;		/* LD Map Length: 64 asked for */
	/* the LD Map names a head by its port number, not by its index */
	info[8] = 1;				/* LD 0: port 1, head 0 */
	info[9] = 1;				/* LD 1: port 1, head 0 */
	info[10] = 2;				/* LD 2: port 2, head 1 */
	info[11] = 0xff;			/* LD 3: unassigned */
	answer(ep, 0x5500, info, sizeof(info));

	/* Identify Memory Device (4000h), tunnelled to each LD of the pool */
	for (int i = 0; i < 4; i++)
		answer_memdev_identify(ep, 4 * (i + 1), 4 * (i + 1), 0);

	heads[0] = overcount ? 255 : 2;		/* Number of Heads: 2 asked for */
	heads[4 + 0] = 1;			/* head 0: port 1 */
	heads[4 + 6] = 0x04;			/*   LTSSM State: L0 */
	heads[4 + 9 + 0] = 2;			/* head 1: port 2 */
	heads[4 + 9 + 6] = 0x00;		/*   LTSSM State: Detect */
	answer(ep, 0x5501, heads, sizeof(heads));

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xe);
	mhd = fmd_topology_find_mhd(&ctx->topology, id);
	CHECK(mhd, "no MHD %s: the probe did not find a multi-headed device",
	      id);
	if (mhd) {
		struct fmd_component *child;
		int nheads = 0, nlds = 0;

		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD)
			nheads++;
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD)
			nlds++;
		CHECK(nheads <= 2 && nlds <= 4, "%d heads, %d LDs", nheads,
		      nlds);
	}
	if (mhd && !overcount) {
		struct fmd_component *child;
		int nheads = 0, nlds = 0, up = 0;

		CHECK(mhd->num_heads == 2 && mhd->total_lds == 4,
		      "%d heads, %d LDs", mhd->num_heads, mhd->total_lds);
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
			nheads++;
			if (fmd_to_head(child)->state == FMD_HEAD_ENABLED)
				up++;
		}
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD)
			nlds++;
		CHECK(nheads == 2, "%d head components", nheads);
		CHECK(nlds == 4, "%d LD components", nlds);
		/* head 0 is in L0 (04h), head 1 is not */
		CHECK(up == 1, "%d heads enabled", up);

		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			/* LDs 0 and 1 on head 0, LD 2 on head 1, LD 3 on none */
			int want = ld->ld_id < 2 ? 0 : ld->ld_id == 2 ? 1 : 0xff;

			CHECK(ld->binding_point_id == want,
			      "pool LD %d on head %d", ld->ld_id,
			      ld->binding_point_id);
			/* 4, 8, 12 and 16 units of 256 MB */
			CHECK(ld->memory_size ==
			      (uint64_t)4 * (ld->ld_id + 1) << 28,
			      "pool LD %d: %" PRIu64 " bytes", ld->ld_id,
			      ld->memory_size);
		}
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD) {
			struct fmd_head *head = fmd_to_head(child);

			CHECK(head->ld_count == (head->head_id == 0 ? 2 : 1),
			      "head %d has %d LDs", head->head_id,
			      head->ld_count);
		}
	}

	fmd_ctx_free(ctx);
	report(overcount ? "multi-headed device returning more than asked" :
			   "multi-headed device", before);
}

/*
 * A Multi-Headed device with more heads, or a larger LD pool, than cxl-fmd
 * keeps (FMD_MAX_HEADS, FMD_MAX_LD_POOL). The specification sets no such
 * limit, so a device past them is not invalid: discovery keeps part of it,
 * and the checks that would need the rest are not made.
 *
 * @heads: 9 heads, each with one LD; head 8 and its LD are not kept.
 * Otherwise: 2 heads and 70 LDs, LD 0 on head 0 and LDs 64 to 69 on head
 * 1; of the pool, LDs 0 to 63 are kept, so head 1 has none of its own.
 */
static void test_mhd_past_limits(bool heads)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t probe[8 + 1] = { 0 }, info[8 + 64] = { 0 };
	uint8_t head_info[4 + 8 * 9] = { 0 };
	int before = failures, nheads = heads ? 9 : 2, nlds = heads ? 9 : 70;
	int kept_heads = heads ? 8 : 2, kept_lds = heads ? 9 : 64, invalid;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mhd *mhd;

	answer_identify(ep, 0xf, 0x03);

	probe[0] = nlds;			/* Number of LDs */
	probe[1] = nheads;			/* Number of Heads */
	probe[5] = 1;				/* LD Map Length */
	answer(ep, 0x5500, probe, sizeof(probe));

	info[0] = nlds;
	info[1] = nheads;
	info[5] = heads ? 9 : 64;		/* LD Map Length: 64 asked for */
	for (int i = 0; i < 64; i++)
		info[8 + i] = heads ? (i < 9 ? i + 1 : 0) : (i == 0 ? 1 : 0xff);
	answer(ep, 0x5500, info, 8 + info[5]);

	for (int i = 0; i < kept_lds; i++)
		answer_memdev_identify(ep, 4, 4, 0);

	head_info[0] = kept_heads;		/* Number of Heads: 8 asked for */
	for (int h = 0; h < kept_heads; h++) {
		head_info[4 + 9 * h] = h + 1;	/* head h: port h + 1 */
		head_info[4 + 9 * h + 6] = 0x04;	/*   LTSSM State: L0 */
	}
	answer(ep, 0x5501, head_info, 4 + 9 * kept_heads);

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xf);
	mhd = fmd_topology_find_mhd(&ctx->topology, id);
	CHECK(mhd, "no MHD %s", id);
	if (mhd) {
		struct fmd_component *child;
		int h = 0, l = 0;

		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_HEAD)
			h++;
		fmd_for_each_child_type(&mhd->base, child, FMD_COMPONENT_LD)
			l++;
		CHECK(mhd->num_heads == nheads && mhd->total_lds == nlds,
		      "reports %d heads, %d LDs", mhd->num_heads,
		      mhd->total_lds);
		CHECK(h == kept_heads && l == kept_lds, "%d heads, %d LDs kept",
		      h, l);
	}

	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 0, "%d rule(s) broken by a device past cxl-fmd's "
	      "limits", invalid);

	fmd_ctx_free(ctx);
	report(heads ? "multi-headed device with more heads than are kept" :
		       "multi-headed device with more pool LDs than are kept",
	       before);
}

/*
 * Message sizes, Section 8.2.10.1. A component takes requests of up to its
 * Maximum Supported Message Size (Identify) and limits its responses to its
 * Response Message Limit (0003h): 256 bytes unless it says otherwise. A list
 * that does not fit is asked for a part at a time.
 */
static void test_paging_ports(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t id[0x49] = { 0 }, ports[4 + 15 * 16] = { 0 };
	uint8_t set = 0, cmd = 0, pl[64] = { 0 };
	size_t size = sizeof(pl);
	int before = failures, found = 0;
	char name[FMD_MAX_ID_LEN];

	answer_identify(ep, 0x5004, 0x00);

	/* 20 ports and no VCS; a block is 16 bytes, so 15 fit 256 */
	id[0x02] = 20;				/* Number of Physical Ports */
	id[0x04] = 0xff;			/* Active Port Bitmask: 0 to 19 */
	id[0x05] = 0xff;
	id[0x06] = 0x0f;
	answer(ep, 0x5100, id, sizeof(id));

	ports[0] = 15;
	for (int i = 0; i < 15; i++)
		port_block(ports + 4 + i * 16, i, 0x03, 0, 0x00, 0x00, 0);
	answer(ep, 0x5101, ports, 4 + 15 * 16);
	ports[0] = 5;
	for (int i = 0; i < 5; i++)
		port_block(ports + 4 + i * 16, 15 + i, 0x03, 0, 0x00, 0x00, 0);
	answer(ep, 0x5101, ports, 4 + 5 * 16);

	fmd_discover(ctx);

	device_id(name, sizeof(name), 0x5004);
	for (int i = 0; i < 20; i++)
		if (fmd_topology_find_port(&ctx->topology, name, i))
			found++;
	CHECK(found == 20, "%d of 20 ports", found);

	/* the second request is for the 5 ports the first had no room for */
	cxlmi_mock_get_last_command(ep, &set, &cmd, pl, &size);
	CHECK(set == 0x51 && cmd == 0x01, "last command %02x%02xh", set, cmd);
	CHECK(pl[0] == 5 && pl[1] == 15 && pl[5] == 19,
	      "last request: %d ports from port %d", pl[0], pl[1]);

	fmd_ctx_free(ctx);
	report("more ports than fit a response", before);
}

static void test_paging_lds(bool large)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t id[0x12] = { 0 }, alloc[4 + 16 * 16] = { 0 };
	int before = failures;
	char name[FMD_MAX_ID_LEN];
	uint64_t serial = large ? 0xf1 : 0xf0;
	struct fmd_ld *ld;

	put64(id + 0x08, serial);
	id[0x10] = large ? 10 : 8;		/* Maximum Supported Message Size */
	id[0x11] = 0x03;
	answer(ep, 0x0001, id, sizeof(id));
	if (large)				/* Response Message Limit: 2^10 */
		answer(ep, 0x0003, (uint8_t[1]){ 10 }, 1);

	answer_ld_info(ep, 136ULL << 28, 16);
	answer_ld_info(ep, 136ULL << 28, 16);

	/*
	 * 16 LDs of 1 to 16 units. The first request is for one LD; an
	 * entry is 16 bytes, so the other 15 fit a response of 256 bytes.
	 */
	alloc[0] = 16;				/* Number of LDs */
	alloc[3] = 1;				/* LD Allocation List Length */
	put64(alloc + 4, 1);
	answer(ep, 0x5401, alloc, 4 + 16);
	alloc[2] = 1;				/* Start LD ID */
	alloc[3] = 15;
	for (int i = 0; i < 15; i++)
		put64(alloc + 4 + 16 * i, i + 2);
	answer(ep, 0x5401, alloc, 4 + 15 * 16);

	fmd_discover(ctx);

	device_id(name, sizeof(name), serial);
	ld = fmd_topology_find_ld(&ctx->topology, name, 0);
	CHECK(ld && ld->memory_size == 1ULL << 28, "LD 0: %" PRIu64 " bytes",
	      ld ? ld->memory_size : 0);
	ld = fmd_topology_find_ld(&ctx->topology, name, 15);
	CHECK(ld && ld->memory_size == 16ULL << 28, "LD 15: %" PRIu64 " bytes",
	      ld ? ld->memory_size : 0);

	fmd_ctx_free(ctx);
	report(large ? "16 LDs in one response of 1024 bytes" :
		       "more LDs than fit a response", before);
}

/*
 * One device, reached two ways. The Device Serial Number of Identify is
 * the "unique identifier for this device" (Table 8-216): the same serial
 * number is the same device.
 */
static void test_same_device_twice(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep[2] = { new_device(ctx), new_device(ctx) };
	int before = failures, rc;
	char id[FMD_MAX_ID_LEN];

	for (int i = 0; i < 2; i++) {
		answer_identify(ep[i], 0x1002, 0x03);
		answer_memdev_identify(ep[i], 64, 64, 0);
	}

	rc = fmd_discover(ctx);
	CHECK(rc == 1, "fmd_discover: %d components from one device", rc);

	device_id(id, sizeof(id), 0x1002);
	CHECK(fmd_topology_find_sld(&ctx->topology, id), "no SLD %s", id);
	CHECK(ctx->topology.root->device_count == 1, "%d devices",
	      ctx->topology.root->device_count);

	fmd_ctx_free(ctx);
	report("one device on two endpoints", before);
}

/* a serial number of 0 identifies nothing: two such devices are two */
static void test_no_serial(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep[2] = { new_device(ctx), new_device(ctx) };
	int before = failures, rc;

	for (int i = 0; i < 2; i++) {
		answer_identify(ep[i], 0, 0x03);
		answer_memdev_identify(ep[i], 64, 64, 0);
	}

	rc = fmd_discover(ctx);
	CHECK(rc == 2, "fmd_discover: %d", rc);
	CHECK(ctx->topology.root->device_count == 2, "%d devices",
	      ctx->topology.root->device_count);
	CHECK(fmd_topology_find_sld(&ctx->topology, "cxl_ep0") &&
	      fmd_topology_find_sld(&ctx->topology, "cxl_ep1"),
	      "devices without a serial number are not named by endpoint");

	fmd_ctx_free(ctx);
	report("two devices without a serial number", before);
}

/*
 * An MLD on a switch port that also has an MCTP interconnect of its own. A
 * command tunnelled to the port is processed by the FM-owned LD of the MLD
 * (Section 7.6.7.3.1), whose Identify has the same serial number as the
 * device's own endpoint. The device is found first here, and is still
 * placed where the switch has it.
 */
static void test_mld_on_switch_and_alone(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *dev = new_device(ctx), *sw;
	struct fmd_component *comp;
	int before = failures;
	char id[FMD_MAX_ID_LEN], sw_id[FMD_MAX_ID_LEN];

	answer_identify(dev, 0x3001, 0x03);

	sw = new_switch(ctx, 0x5005);
	answer_identify(sw, 0x3001, 0x03);	/* tunnelled to port 5 */

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0x3001);
	device_id(sw_id, sizeof(sw_id), 0x5005);
	comp = fmd_topology_find(&ctx->topology, id);
	CHECK(comp && comp->type == FMD_COMPONENT_MLD, "no MLD %s", id);
	if (comp) {
		CHECK(comp->parent == (struct fmd_component *)
		      fmd_topology_find_port(&ctx->topology, sw_id, 5),
		      "the MLD is not on port 5 of the switch");
		CHECK(comp->ep == sw, "the MLD is not reached through the switch");
		CHECK(fmd_to_mld(comp)->switch_port == 5, "tunnel port %d",
		      fmd_to_mld(comp)->switch_port);
	}
	CHECK(ctx->topology.root->device_count == 1, "%d devices",
	      ctx->topology.root->device_count);

	fmd_ctx_free(ctx);
	report("MLD on a switch port and on its own endpoint", before);
}

/*
 * What an MLD is, and how many LDs it has.
 *
 * Get LD Info is mandatory for an MLD and prohibited for an SLD (Table
 * 8-397): a device that answers it is an MLD, even with a single LD. Its LD
 * Count is the number of LDs supported (Table 7-46); the number that exist
 * is the Number of LDs enabled, from Get LD Allocations (Table 7-48).
 */
static void test_mld_ld_counts(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *one = new_device(ctx), *some = new_device(ctx);
	uint8_t info[0x0b] = { 0 }, qos[7] = { 0 };
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_mld *mld;

	/* an MLD that supports one LD */
	answer_identify(one, 0xa1, 0x03);
	answer_ld_info(one, 4ULL << 28, 1);
	answer_ld_info(one, 4ULL << 28, 1);
	answer_ld_allocations(one, 1, 0, 4);

	/* an MLD that supports 8 LDs and has 2 enabled */
	answer_identify(some, 0xa2, 0x03);
	for (int i = 0; i < 2; i++) {
		put64(info + 0x00, 32ULL << 28);	/* Memory Size */
		put16(info + 0x08, 8);			/* LD Count: supported */
		info[0x0a] = 0x03;			/* QoS Telemetry Capability */
		answer(some, 0x5400, info, sizeof(info));
	}
	answer_ld_allocations(some, 2, 0, 4);

	/* Get QoS Control (5403h), Table 7-52 */
	qos[0] = 0x03;				/* both mechanisms enabled */
	qos[1] = 10;				/* Egress Moderate Percentage */
	qos[2] = 25;				/* Egress Severe Percentage */
	qos[3] = 8;				/* Backpressure Sample Interval */
	put16(qos + 4, 1000);			/* ReqCmpBasis */
	qos[6] = 64;				/* Completion Collection Interval */
	answer(some, 0x5403, qos, sizeof(qos));

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xa1);
	mld = fmd_topology_find_mld(&ctx->topology, id);
	CHECK(mld, "a device with one LD that answers Get LD Info is not an MLD");
	CHECK(!fmd_topology_find_sld(&ctx->topology, id),
	      "an MLD with one LD was taken for an SLD");
	if (mld)
		CHECK(mld->ld_count == 1 && mld->max_ld_count == 1,
		      "%d LDs of %d", mld->ld_count, mld->max_ld_count);

	device_id(id, sizeof(id), 0xa2);
	mld = fmd_topology_find_mld(&ctx->topology, id);
	CHECK(mld, "no MLD %s", id);
	if (mld) {
		CHECK(mld->ld_count == 2 && mld->max_ld_count == 8,
		      "%d LDs enabled of %d supported", mld->ld_count,
		      mld->max_ld_count);
		CHECK(fmd_topology_find_ld(&ctx->topology, id, 1) &&
		      !fmd_topology_find_ld(&ctx->topology, id, 2),
		      "an LD that is supported and not enabled was created");
		CHECK(mld->qos_telemetry_capability == 0x03,
		      "QoS Telemetry Capability %#x",
		      mld->qos_telemetry_capability);
		CHECK(mld->qos_control.enabled &&
		      mld->qos_control.temp_throughput_reduction_enabled,
		      "QoS Telemetry Control bits");
		CHECK(mld->qos_control.reqcmp_basis == 1000 &&
		      mld->qos_control.completion_collection_interval == 64,
		      "ReqCmpBasis %d, Completion Collection Interval %d",
		      mld->qos_control.reqcmp_basis,
		      mld->qos_control.completion_collection_interval);
	}

	fmd_ctx_free(ctx);
	report("MLD LD counts and QoS control", before);
}

/*
 * Get Health Info to Redfish Status (Table 8-315). Health Status is a
 * bitmask of four conditions; Media Status is an enumeration of ten values.
 */
static void test_health_mapping(void)
{
	int before = failures;

	CHECK(fmd_map_health_status(0x00) == HEALTH_OK, "no condition");
	CHECK(fmd_map_health_status(0x01) == HEALTH_WARNING, "Maintenance Needed");
	CHECK(fmd_map_health_status(0x02) == HEALTH_WARNING, "Performance Degraded");
	CHECK(fmd_map_health_status(0x04) == HEALTH_CRITICAL,
	      "Hardware Replacement Needed");
	CHECK(fmd_map_health_status(0x08) == HEALTH_WARNING,
	      "Memory Capacity Degraded is reported as healthy");

	CHECK(fmd_map_media_status(0x00) == STATE_ENABLED, "Normal");
	CHECK(fmd_map_media_status(0x01) == STATE_STARTING, "Not Ready");
	CHECK(fmd_map_media_status(0x03) == STATE_UNAVAILABLE_OFFLINE,
	      "All data lost");
	for (int media = 0x04; media <= 0x09; media++)
		CHECK(fmd_map_media_status(media) == STATE_DEGRADED,
		      "Media Status %02xh is reported as normal", media);
	/* "All other encodings are reserved": not Normal either */
	CHECK(fmd_map_media_status(0x0a) == STATE_DEGRADED &&
	      fmd_map_media_status(0xff) == STATE_DEGRADED,
	      "a reserved Media Status is reported as normal");
	CHECK(fmd_worse_media_status(0x00, 0x0a) == 0x0a,
	      "worse of 00h and reserved 0Ah");

	/* the worse of two is by meaning: 03h (all data lost) over 09h */
	CHECK(fmd_worse_media_status(0x03, 0x09) == 0x03 &&
	      fmd_worse_media_status(0x09, 0x03) == 0x03, "worse of 03h and 09h");
	CHECK(fmd_worse_media_status(0x00, 0x02) == 0x02, "worse of 00h and 02h");

	report("health and media status", before);
}

/* Status of @j as JSON text, for the checks below; the caller frees it */
static char *status_text(json_t *j)
{
	json_t *st = j ? json_object_get(j, "Status") : NULL;

	return st ? json_dumps(st, JSON_COMPACT | JSON_SORT_KEYS) : strdup("(none)");
}

/*
 * The Status of what Get Health Info said, added up (fmd_rollup_status()):
 * the Health Status bits OR-ed then mapped, never compared as a number,
 * and a read that failed is not OK. Review F17.
 */
static void test_health_rollup(void)
{
	struct cxlmi_cmd_memdev_get_health_info_rsp h = { 0 };
	struct fmd_status_rollup r;
	enum rf_health health;
	enum rf_state state;
	int before = failures;
	bool known;

	/* Maintenance Needed and Performance Degraded: Warning, Enabled */
	memset(&r, 0, sizeof(r));
	h.health_status = 0x01;
	fmd_rollup_add(&r, &h, true);
	h.health_status = 0x02;
	fmd_rollup_add(&r, &h, true);
	known = fmd_rollup_status(&r, &health, &state);
	CHECK(known && health == HEALTH_WARNING && state == STATE_ENABLED,
	      "01h and 02h: known %d health %d state %d", known, health, state);

	/* Memory Capacity Degraded alone was Critical, UnavailableOffline */
	memset(&r, 0, sizeof(r));
	h.health_status = 0x08;
	fmd_rollup_add(&r, &h, true);
	known = fmd_rollup_status(&r, &health, &state);
	CHECK(known && health == HEALTH_WARNING && state == STATE_ENABLED,
	      "08h: health %d state %d", health, state);

	/* Media Status: only 03h is Critical and UnavailableOffline */
	h.health_status = 0;
	for (int media = 0; media <= 0x0a; media++) {
		memset(&r, 0, sizeof(r));
		h.media_status = media;
		fmd_rollup_add(&r, &h, true);
		known = fmd_rollup_status(&r, &health, &state);
		CHECK(known && health == (media == 3 ? HEALTH_CRITICAL :
					  media <= 1 ? HEALTH_OK : HEALTH_WARNING) &&
		      state == fmd_map_media_status(media),
		      "media %02xh: health %d state %d", media, health, state);
	}

	/* not read: no Health, unless one read is known to be worse than OK */
	memset(&r, 0, sizeof(r));
	fmd_rollup_add(&r, &h, false);
	known = fmd_rollup_status(&r, &health, &state);
	CHECK(!known && state == STATE_ENABLED, "nothing read: known %d", known);
	h.media_status = 0;
	fmd_rollup_add(&r, &h, true);
	CHECK(!fmd_rollup_status(&r, &health, &state),
	      "one OK and one not read is known");
	h.health_status = 0x04;
	fmd_rollup_add(&r, &h, true);
	known = fmd_rollup_status(&r, &health, &state);
	CHECK(known && health == HEALTH_CRITICAL,
	      "a Critical one and one not read: known %d health %d", known, health);

	report("health rollup", before);
}

/*
 * Get Health Info answered Internal Error at discovery: the SLD's views
 * have no Health. They reported Enabled, OK, from the zeroed response.
 * Review F17 claim 4 (from its case A).
 */
static void test_health_read_failed(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	struct fmd_sld *sld;
	char id[FMD_MAX_ID_LEN];
	json_t *views[4];
	int before = failures, rc;

	answer_identify(ep, 0x1701, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	answer_rc(ep, 0x4200, CXLMI_RET_INTERNAL);
	rc = fmd_discover(ctx);
	CHECK(rc == 1, "fmd_discover: %d", rc);
	device_id(id, sizeof(id), 0x1701);
	sld = fmd_topology_find_sld(&ctx->topology, id);
	CHECK(sld && !sld->health_valid, "the health read is taken to be valid");

	views[0] = fmd_redfish_chassis(ctx, id);
	views[1] = fmd_redfish_memory(ctx, id, id);
	views[2] = fmd_redfish_pcie_device(ctx, id, id);
	views[3] = fmd_redfish_cxl_logical_device(ctx, id, id, "0");
	for (int i = 0; i < 4; i++) {
		char *text = status_text(views[i]);

		CHECK(!strcmp(text, "{\"State\":\"Enabled\"}"),
		      "view %d Status %s", i, text);
		free(text);
		json_decref(views[i]);
	}

	/* and when it is read, Health is there */
	fmd_ctx_free(ctx);
	ctx = new_ctx();
	ep = new_device(ctx);
	answer_identify(ep, 0x1702, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	{
		uint8_t pl[0x12] = { 0 };

		pl[0] = 0x03;		/* Maintenance Needed, Performance Degraded */
		answer(ep, 0x4200, pl, sizeof(pl));
	}
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x1702);
	views[0] = fmd_redfish_chassis(ctx, id);
	{
		char *text = status_text(views[0]);

		CHECK(!strcmp(text, "{\"Health\":\"Warning\",\"HealthRollup\":\"Warning\","
			      "\"State\":\"Enabled\"}"), "Chassis Status %s", text);
		free(text);
	}
	json_decref(views[0]);
	fmd_ctx_free(ctx);
	report("a failed health read is not OK", before);
}

/*
 * A directly attached SLD of @total, Volatile Only @vol and Persistent Only
 * @pers, with Partition Alignment @align, all in units of 256 MB, and
 * @active_vol of it volatile now (Get Partition Info, Table 8-310).
 */
static struct fmd_sld *direct_sld(struct fmd_ctx *ctx,
				  struct cxlmi_endpoint **epp, uint64_t serial,
				  uint64_t total, uint64_t vol, uint64_t pers,
				  uint64_t align, uint64_t active_vol)
{
	struct cxlmi_endpoint *ep = new_device(ctx);
	char id[FMD_MAX_ID_LEN];
	uint8_t pl[0x45] = { 0 }, part[0x20] = { 0 };

	put64(part + 0x00, active_vol);		/* Active Volatile Capacity */
	put64(part + 0x08, total - active_vol);	/* Active Persistent Capacity */
	answer(ep, 0x4100, part, sizeof(part));

	answer_identify(ep, serial, 0x03);
	/* Identify Memory Device (4000h), Table 8-309 */
	memcpy(pl, "TEST-1.0", 8);
	put64(pl + 0x10, total);		/* Total Capacity */
	put64(pl + 0x18, vol);			/* Volatile Only Capacity */
	put64(pl + 0x20, pers);			/* Persistent Only Capacity */
	put64(pl + 0x28, align);		/* Partition Alignment */
	answer(ep, 0x4000, pl, sizeof(pl));
	fmd_discover(ctx);

	*epp = ep;
	device_id(id, sizeof(id), serial);
	return fmd_topology_find_sld(&ctx->topology, id);
}

/*
 * Scan Media (4304h), Table 8-327: "Scan Media Physical Address Length:
 * The range of physical addresses to scan. This length shall be in units
 * of 64 bytes", and Bits[5:0] of the address are Reserved. The Redfish
 * Length is in bytes.
 */
static void test_scan_media(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_sld *sld = direct_sld(ctx, &ep, 0x1101, 64, 64, 0, 0, 64);
	int before = failures, rc;
	uint8_t pl[64];
	uint16_t opcode;
	unsigned int sent;
	char id[FMD_MAX_ID_LEN];
	static const char *const bad[] = {
		"{\"PhysicalAddress\": \"0x1000\", \"Length\": 100}",
		"{\"PhysicalAddress\": \"0x1001\", \"Length\": 4096}",
		"{\"PhysicalAddress\": \"0x1000\", \"Length\": 0}",
		"{\"PhysicalAddress\": \"0x1000\", \"Length\": -64}",
	};
	const char *ok = "{\"PhysicalAddress\": \"0x1000\", \"Length\": 4096}";

	CHECK(sld, "no SLD");
	if (!sld)
		goto out;
	snprintf(id, sizeof(id), "%s", sld->base.id);

	answer(ep, 0x4304, NULL, 0);
	rc = fmd_redfish_action_scan_media(ctx, id, id, ok, strlen(ok));
	CHECK(rc == 0, "Scan Media of 4096 bytes: %d", rc);
	last_payload(ep, &opcode, pl, sizeof(pl));
	CHECK(opcode == 0x4304, "last command %04xh", opcode);
	CHECK(get64(pl) == 0x1000, "Scan Media Physical Address %" PRIx64,
	      get64(pl));
	CHECK(get64(pl + 8) == 4096 / 64,
	      "Scan Media Physical Address Length %" PRIu64 " for 4096 bytes",
	      get64(pl + 8));

	/* not in whole 64-byte lines, or empty: refused, nothing sent */
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		sent = commands_sent(ep);
		rc = fmd_redfish_action_scan_media(ctx, id, id, bad[i],
						   strlen(bad[i]));
		CHECK(rc != 0 && commands_sent(ep) == sent,
		      "%s: %d, %u commands sent", bad[i], rc,
		      commands_sent(ep) - sent);
	}
out:
	fmd_ctx_free(ctx);
	report("Scan Media length in 64-byte units", before);
}

/*
 * Life Used (Table 8-315): "The device's used life as a percentage value
 * (0 to 100) of factory-expected life span. ... Returns FFh if not
 * implemented." Only a percentage gives a PredictedMediaLifeLeftPercent.
 */
static void test_life_used(void)
{
	static const uint8_t life[] = { 0, 30, 100, 101, 0xff };
	int before = failures;

	for (size_t i = 0; i < sizeof(life); i++) {
		struct fmd_ctx *ctx = new_ctx();
		struct cxlmi_endpoint *ep = new_device(ctx);
		uint8_t health[0x12] = { 0 };
		char id[FMD_MAX_ID_LEN];
		json_t *m, *left;

		answer_identify(ep, 0x1201, 0x03);
		answer_memdev_identify(ep, 64, 64, 0);
		health[0x03] = life[i];			/* Life Used */
		answer(ep, 0x4200, health, sizeof(health));
		fmd_discover(ctx);

		device_id(id, sizeof(id), 0x1201);
		m = fmd_redfish_memory_metrics(ctx, id, id);
		CHECK(m, "no MemoryMetrics");
		left = json_object_get(json_object_get(m, "HealthData"),
				       "PredictedMediaLifeLeftPercent");
		if (life[i] <= 100)
			CHECK(left && json_integer_value(left) == 100 - life[i],
			      "Life Used %u: %" JSON_INTEGER_FORMAT " left",
			      life[i], left ? json_integer_value(left) : -1);
		else
			CHECK(!left, "Life Used %02xh: %" JSON_INTEGER_FORMAT
			      " left", life[i], json_integer_value(left));
		json_decref(m);
		fmd_ctx_free(ctx);
	}
	report("PredictedMediaLifeLeftPercent from Life Used", before);
}

/* PATCH Memory with @body, and what it sent as Volatile Capacity, or -1 */
static int64_t patch_partition(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
			       const char *id, const char *body, int *rc)
{
	unsigned int sent = commands_sent(ep);
	uint8_t pl[16] = { 0 };
	uint16_t opcode;

	answer(ep, 0x4101, NULL, 0);
	*rc = fmd_redfish_patch_memory(ctx, id, id, body, strlen(body));
	if (commands_sent(ep) == sent)
		return -1;
	if (last_payload(ep, &opcode, pl, sizeof(pl)) < 9 || opcode != 0x4101)
		return -2;
	return (int64_t)get64(pl);
}

/*
 * Set Partition Info (4101h). Its Volatile Capacity is "The amount of
 * partitionable capacity that shall be allocated to volatile capacity, in
 * multiples of 256 MB aligned to the partition alignment requirement"
 * (Table 8-311), where "Partitionable capacity is equal to (Total Capacity
 * - Volatile Only Capacity - Persistent Only Capacity)". The Redfish
 * StagedVolatileSizeMiB and StagedNonVolatileSizeMiB are totals, as Get
 * Partition Info's Next capacities are (Table 8-310).
 *
 * The device: 16 GiB, 2 GiB Volatile Only, 4 GiB Persistent Only, so 10 GiB
 * partitionable in steps of 1 GiB; 6 GiB volatile now.
 */
static void test_set_partition(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_sld *sld = direct_sld(ctx, &ep, 0x1102, 64, 8, 16, 4, 24);
	struct fmd_component *dom, *chunk;
	int before = failures, rc;
	int64_t sent;
	char id[FMD_MAX_ID_LEN], dom_id[FMD_MAX_ID_LEN + 16];
	json_t *rsp = NULL;
	static const char *const bad[] = {
		"{\"CXL\": {\"StagedVolatileSizeMiB\": 3000}}",	/* not 256 MB */
		"{\"CXL\": {\"StagedVolatileSizeMiB\": 1024}}",	/* < Volatile Only */
		"{\"CXL\": {\"StagedVolatileSizeMiB\": 13312}}",	/* > partitionable */
		"{\"CXL\": {\"StagedVolatileSizeMiB\": 6656}}",	/* not aligned */
		"{\"CXL\": {\"StagedNonVolatileSizeMiB\": 512}}",	/* < Persistent Only */
	};

	CHECK(sld, "no SLD");
	if (!sld)
		goto out;
	snprintf(id, sizeof(id), "%s", sld->base.id);

	/* 10 GiB volatile in all: 8 GiB of the partitionable capacity */
	sent = patch_partition(ctx, ep, id,
			       "{\"CXL\": {\"StagedVolatileSizeMiB\": 10240}}", &rc);
	CHECK(rc == 0 && sent == 32, "StagedVolatileSizeMiB 10240: %d, "
	      "Volatile Capacity %" PRId64, rc, sent);
	CHECK(sld->partition.next_vmem == 40 && sld->partition.next_pmem == 24,
	      "next %" PRIu64 "/%" PRIu64, (uint64_t)sld->partition.next_vmem,
	      (uint64_t)sld->partition.next_pmem);

	/* 6 GiB persistent in all: 10 GiB volatile, as above */
	sent = patch_partition(ctx, ep, id,
			       "{\"CXL\": {\"StagedNonVolatileSizeMiB\": 6144}}", &rc);
	CHECK(rc == 0 && sent == 32, "StagedNonVolatileSizeMiB 6144: %d, "
	      "Volatile Capacity %" PRId64, rc, sent);

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		sent = patch_partition(ctx, ep, id, bad[i], &rc);
		CHECK(rc == -2 && sent == -1, "%s: %d, Volatile Capacity %" PRId64,
		      bad[i], rc, sent);
	}

	/*
	 * A Volatile chunk of 1 GiB adds to the volatile capacity staged:
	 * 10 GiB, so 11 GiB in all, 9 of them partitionable. The chunk of
	 * the whole device is deleted first to make room; it is PMEM, and
	 * sends nothing.
	 */
	dom = fmd_first_child_type(&sld->base, FMD_COMPONENT_DOMAIN);
	chunk = dom ? fmd_first_child_type(dom, FMD_COMPONENT_CHUNK) : NULL;
	CHECK(dom && chunk, "no domain or chunk");
	if (!dom || !chunk)
		goto out;
	snprintf(dom_id, sizeof(dom_id), "%s", dom->id);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom_id, chunk->id);
	CHECK(rc == 0, "delete %s: %d", dom_id, rc);
	answer(ep, 0x4101, NULL, 0);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom_id,
		"{\"MemoryChunkSizeMiB\": 1024, \"AddressRangeType\": \"Volatile\"}",
		strlen("{\"MemoryChunkSizeMiB\": 1024, \"AddressRangeType\": \"Volatile\"}"),
		&rsp);
	json_decref(rsp);
	{
		uint8_t pl[16] = { 0 };
		uint16_t opcode = 0;

		last_payload(ep, &opcode, pl, sizeof(pl));
		CHECK(rc == 0 && opcode == 0x4101 && get64(pl) == 36,
		      "Volatile chunk: %d, %04xh, Volatile Capacity %" PRIu64,
		      rc, opcode, get64(pl));
	}
out:
	fmd_ctx_free(ctx);

	/* Partition Alignment 0: "shall fail with an Unsupported error" */
	ctx = new_ctx();
	sld = direct_sld(ctx, &ep, 0x1103, 64, 32, 32, 0, 32);
	if (sld) {
		snprintf(id, sizeof(id), "%s", sld->base.id);
		sent = patch_partition(ctx, ep, id,
			"{\"CXL\": {\"StagedVolatileSizeMiB\": 8192}}", &rc);
		CHECK(rc == -5 && sent == -1,
		      "no partitionable capacity: %d, Volatile Capacity %" PRId64,
		      rc, sent);
	}
	fmd_ctx_free(ctx);
	report("Set Partition Info of partitionable capacity", before);
}

/*
 * An SLD on a switch port has the switch's endpoint and cannot be reached
 * through it: "Valid targets for the tunneled commands include switch MLD
 * Ports, valid LDs within an MLD, and the LD Pool CCI in a Multi-Headed
 * device" (Section 7.6.7.3.1), and "It is prohibited for switches to
 * support any commands from the Memory Device Command Set" (Table 8-308, note 1).
 * Nothing is sent for it. An MLD on a switch port is reached through the
 * port.
 */
static void test_switch_sld_placeholder(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16];
	struct cxlmi_tunnel_info ti = { 0 };
	struct fmd_component *sld, *mld;
	int before = failures, rc;
	unsigned int sent;
	const char *scan = "{\"PhysicalAddress\": \"0x0\", \"Length\": 64}";
	const char *ts = "{\"CXLDevice\": {\"Timestamp\": \"2025-11-13T02:13:20Z\"}}";
	const char *part = "{\"CXL\": {\"StagedVolatileSizeMiB\": 0}}";

	/* port 4 has an SLD: Type 3 SLD (04h), LTSSM L0 (04h) */
	answer_identify(ep, 0x5013, 0x00);
	answer_switch_ports(ep, 6, 0x04, 0x04);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_discover(ctx);

	device_id(sw_id, sizeof(sw_id), 0x5013);
	snprintf(id, sizeof(id), "%.32s_port4_dev", sw_id);
	sld = fmd_topology_find(&ctx->topology, id);
	CHECK(sld && sld->type == FMD_COMPONENT_SLD, "no SLD %s", id);
	if (!sld)
		goto out;

	sent = commands_sent(ep);
	CHECK(fmd_get_device_timestamp(sld) == 0, "a timestamp for the SLD");
	rc = fmd_redfish_patch_pcie_device(ctx, id, id, ts, strlen(ts));
	CHECK(rc == -4, "Set Timestamp of the SLD: %d", rc);
	CHECK(fmd_get_security_state(sld, 0) == 0, "a security state");
	rc = fmd_redfish_action_scan_media(ctx, id, id, scan, strlen(scan));
	CHECK(rc != 0, "Scan Media of the SLD: %d", rc);
	rc = fmd_redfish_patch_memory(ctx, id, id, part, strlen(part));
	CHECK(rc != 0, "Set Partition Info of the SLD: %d", rc);
	rc = fmd_sld_set_partition(ctx, fmd_to_sld(sld), 0, false);
	CHECK(rc != 0, "fmd_sld_set_partition of the SLD: %d", rc);
	CHECK(commands_sent(ep) == sent,
	      "%u commands for the SLD went to the switch",
	      commands_sent(ep) - sent);

	/* the MLD on port 5: through the port */
	snprintf(id, sizeof(id), "%.32s_port5_dev", sw_id);
	mld = fmd_topology_find(&ctx->topology, id);
	CHECK(mld && mld->type == FMD_COMPONENT_MLD, "no MLD %s", id);
	if (mld) {
		answer(ep, 0x0300, (uint8_t[8]){ 1 }, 8);
		CHECK(fmd_get_device_timestamp(mld) == 1, "no MLD timestamp");
		cxlmi_mock_get_last_tunnel(ep, &ti);
		CHECK(last_opcode(ep) == 0x0300 && ti.level == 1 &&
		      ti.port == 5, "Get Timestamp: %04xh, level %d, port %d",
		      last_opcode(ep), ti.level, ti.port);
	}
out:
	fmd_ctx_free(ctx);
	report("an SLD on a switch port is not sent commands", before);
}

/*
 * Get DCD Info (5600h), Table 7-65. Total Dynamic Capacity is "in multiples
 * of 256 MB"; libcxlmi returns it in bytes.
 */
static void test_dcd_info(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t info[0x54] = { 0 };
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_sld *sld;

	answer_identify(ep, 0xdc, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);

	info[0x00] = 1;				/* Number of Hosts */
	info[0x01] = 1;				/* Number of Supported DC Regions */
	put16(info + 0x04, 0x07);		/* Free, Contiguous, Prescriptive */
	put16(info + 0x08, 0x03);		/* Tag-based, Prescriptive */
	put64(info + 0x0c, 4);			/* Total Dynamic Capacity: 1 GiB */
	put64(info + 0x14, 1ULL << 28);		/* Region 0: 256 MB blocks */
	answer(ep, 0x5600, info, sizeof(info));

	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xdc);
	sld = fmd_topology_find_sld(&ctx->topology, id);
	CHECK(sld && sld->dcd, "no DCD %s", id);
	if (sld && sld->dcd) {
		json_t *cap = fmd_redfish_dc_capabilities(ctx, &sld->base, id, id);

		CHECK(sld->dcd->total_dc_capacity == 4ULL << 28,
		      "total dynamic capacity %" PRIu64 " bytes",
		      sld->dcd->total_dc_capacity);
		CHECK(cap && json_integer_value(json_object_get(cap,
		      "TotalDynamicCapacityMiB")) == 1024,
		      "TotalDynamicCapacityMiB %lld", cap ? (long long)
		      json_integer_value(json_object_get(cap,
		      "TotalDynamicCapacityMiB")) : -1);
		/* Bit[3] of the add policies must be 0: three are supported */
		CHECK(cap && json_array_size(json_object_get(cap,
		      "AddCapacityPoliciesSupported")) == 3, "add policies");
		json_decref(cap);
	}

	/*
	 * Draining the Dynamic Capacity Event Log. It is read until Get Event
	 * Records returns no records, whatever More Event Records (Bit[1] of
	 * its Flags) says: "After clearing log entries, the Get Event Records
	 * command should be used to confirm that all entries have been
	 * cleared from the log to ensure that the notification trigger has
	 * been re-armed" (Section 8.2.10.2.8). One record, no more said; then
	 * none.
	 */
	if (sld && sld->dcd) {
		uint8_t page[0x20 + 0x80] = { 0 };

		page[0x00] = 0x00;			/* not More Event Records */
		put16(page + 0x14, 1);			/* Event Record Count */
		put16(page + 0x20 + 0x14, 7);		/* Event Record Handle */
		answer(ep, 0x0100, page, sizeof(page));
		answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
		memset(page, 0, sizeof(page));
		answer(ep, 0x0100, page, 0x20);

		fmd_dcd_poll_events(ctx);
		CHECK(last_opcode(ep) == 0x0100,
		      "the log was not read again after it was cleared: "
		      "last command %04xh", last_opcode(ep));
	}

	fmd_ctx_free(ctx);
	report("Get DCD Info", before);
}

/*
 * A one-host DCD (an SLD) with region 0 at 1 GiB, 1 GiB long, in 256 MB
 * blocks, with Attributes1 @flags, and one 256 MB extent at its base tagged
 * dc_tag. The refresh after a command the device accepts is answered as
 * discovery was: Get Host DC Region Configuration (5601h, Tables 7-67 and
 * 7-68), Get DC Region Extent Lists (5603h, Tables 7-70 and 7-71), and
 * Dynamic Capacity List Tags (5608h) unsupported.
 */
static const uint8_t dc_tag[16] = {
	0x6f, 0x1a, 0x2c, 0x3e, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x01,
};

static void answer_dc_refresh(struct cxlmi_endpoint *ep, uint8_t flags)
{
	uint8_t regions[0x04 + 0x28 + 0x10] = { 0 };
	uint8_t extents[0x18 + 0x28] = { 0 };

	regions[0x02] = 1;			/* Number of Available Regions */
	regions[0x03] = 1;			/* Number of Regions Returned */
	put64(regions + 0x04, 1ULL << 30);	/* Region Base */
	put64(regions + 0x0c, 4);		/* Region Decode Length, 256 MB units */
	put64(regions + 0x14, 1ULL << 30);	/* Region Length */
	put64(regions + 0x1c, 1ULL << 28);	/* Region Block Size */
	regions[0x24] = flags;			/* Attributes1 */
	put16(regions + 0x2c, 64);		/* Number of Extents Supported */
	put16(regions + 0x30, 63);		/* Number of Extents Available */
	put16(regions + 0x34, 16);		/* Number of Tags Supported */
	put16(regions + 0x38, 15);		/* Number of Tags Available */
	answer(ep, 0x5601, regions, sizeof(regions));

	put16(extents + 0x08, 1);		/* Number of Extents Returned */
	put16(extents + 0x0c, 1);		/* Total Number of Extents */
	put64(extents + 0x18, 1ULL << 30);	/* Starting DPA */
	put64(extents + 0x20, 1ULL << 28);	/* Length */
	memcpy(extents + 0x28, dc_tag, 16);	/* Tag */
	answer(ep, 0x5603, extents, sizeof(extents));

	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
}

static struct fmd_component *hw_dcd(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
				    uint64_t serial, uint8_t flags)
{
	uint8_t info[0x54] = { 0 };
	char id[FMD_MAX_ID_LEN];
	struct fmd_component *dev;

	answer_identify(ep, serial, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	info[0x00] = 1;				/* Number of Hosts */
	info[0x01] = 1;				/* Number of Supported DC Regions */
	put16(info + 0x04, 0x07);		/* Free, Contiguous, Prescriptive */
	put16(info + 0x08, 0x03);		/* Tag-based, Prescriptive */
	put64(info + 0x0c, 4);			/* Total Dynamic Capacity: 1 GiB */
	put64(info + 0x14, 1ULL << 28);		/* Region 0: 256 MB blocks */
	answer(ep, 0x5600, info, sizeof(info));
	answer_dc_refresh(ep, flags);

	fmd_discover(ctx);
	device_id(id, sizeof(id), serial);
	dev = fmd_topology_find(&ctx->topology, id);
	CHECK(dev && fmd_memdev_dcd(dev) && fmd_dc_find_region(dev, 0, 0) &&
	      fmd_dc_find_extent(dev, 0, 1ULL << 30), "no DCD %s with its region and extent", id);
	if (!dev || !fmd_dc_find_region(dev, 0, 0) || !fmd_dc_find_extent(dev, 0, 1ULL << 30))
		return NULL;
	return dev;
}

/*
 * An Id names one component (review F10): a second component with an Id
 * the topology has is refused, before anything of the topology changes,
 * so that lookup cannot find one and hide the other. The first is kept.
 */
static void test_duplicate_id_refused(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct fmd_topology *topo = &ctx->topology;
	struct fmd_sld *a = fmd_sld_create("dup_dev"), *b = fmd_sld_create("dup_dev");
	struct fmd_host *h = fmd_host_create("dup_dev");
	struct fmd_component *first_child;
	unsigned int added;
	int count, devices, hosts;
	uint32_t generation;
	int before = failures, rc;

	rc = fmd_topology_add(topo, &a->base, NULL);
	CHECK(rc == 0, "first add: %d", rc);

	count = topo->components.count;
	added = topo->added;
	generation = topo->generation;
	devices = topo->root->device_count;
	hosts = topo->root->host_count;
	first_child = topo->root->base.first_child;

	/* the same Id, of the same type and of another */
	rc = fmd_topology_add(topo, &b->base, NULL);
	CHECK(rc == -EEXIST, "second SLD dup_dev: %d", rc);
	rc = fmd_topology_add(topo, &h->base, NULL);
	CHECK(rc == -EEXIST, "host dup_dev: %d", rc);

	CHECK(topo->components.count == count && topo->added == added &&
	      topo->generation == generation && topo->root->device_count == devices &&
	      topo->root->host_count == hosts && topo->root->base.first_child == first_child,
	      "the topology changed: %d/%d components, generation %u/%u",
	      topo->components.count, count, topo->generation, generation);
	CHECK(!b->base.parent && !b->base.fabric && !h->base.parent,
	      "a refused component was linked in");
	CHECK(fmd_topology_find(topo, "dup_dev") == &a->base, "dup_dev is not the first");

	fmd_component_free(&b->base);
	fmd_component_free(&h->base);
	fmd_ctx_free(ctx);
	report("a second component with an Id is refused", before);
}

/*
 * A size in MiB that no byte count of 64 bits holds is refused before
 * anything is sent (review F11). MemoryChunkSizeMiB 2^44 + 256 wrapped to
 * 256 MiB, which Set LD Allocations (5402h) then gave; Length 2^44 + 256
 * of a DC add to 256 MiB, which Initiate Dynamic Capacity Add (5604h) then
 * asked for, though Length is "The number of bytes of capacity to add"
 * (Table 7-72). The same requests with 256 do send, so the mock sees it.
 */
static void test_mib_out_of_range_sends_nothing(void)
{
	static const char *const sizes[] = { "17592186044672", "17592186044416",
					     "-1", "0" };
	struct fmd_ctx *ctx = new_ctx();
	/* 2 x 1 GiB, of 2.25 GiB: room for a chunk of 256 MiB */
	struct cxlmi_endpoint *ep = new_mld_sized(ctx, 0x1170, 2, 0, 4,
						  (2ULL << 30) + (256ULL << 20));
	struct fmd_mem_domain *domain;
	struct fmd_component *dev;
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], body[256];
	uint8_t alloc[4 + 16] = { 0 }, set[4 + 16] = { 0 };
	const char *msg = NULL;
	int before = failures, rc, chunks;
	unsigned int sent;
	json_t *resp = NULL;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x1170);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	CHECK(domain, "no domain %s", dom);
	if (!domain)
		goto dcd;
	chunks = fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK);

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(body, sizeof(body),
			 "{\"MemoryChunkSizeMiB\": %s, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld0\"}}]}",
			 sizes[i], id, id);
		sent = commands_sent(ep);
		rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
		CHECK(rc == -10 && !resp && commands_sent(ep) == sent &&
		      fmd_component_count_children(&domain->base,
						   FMD_COMPONENT_CHUNK) == chunks,
		      "MemoryChunkSizeMiB %s: %d, %u commands sent", sizes[i], rc,
		      commands_sent(ep) - sent);
		json_decref(resp);
		resp = NULL;
	}

	/* 256 MiB: LD 0 goes from 4 to 5 units of 256 MB */
	alloc[0] = 2;				/* Number of LDs */
	alloc[3] = 1;				/* LD Allocation List Length */
	put64(alloc + 4, 4);			/* Range 1 Allocation Multiplier */
	answer(ep, 0x5401, alloc, sizeof(alloc));
	set[0] = 1;				/* Number of LDs */
	put64(set + 4, 5);			/* LD 0 as it is after */
	answer(ep, 0x5402, set, sizeof(set));
	snprintf(body, sizeof(body),
		 "{\"MemoryChunkSizeMiB\": 256, \"InterleaveSets\": [{\"Memory\": "
		 "{\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld0\"}}]}", id, id);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == 0 && last_opcode(ep) == 0x5402,
	      "MemoryChunkSizeMiB 256: %d, last %04xh", rc, last_opcode(ep));
	json_decref(resp);

dcd:
	fmd_ctx_free(ctx);

	ctx = new_ctx();
	ep = new_device(ctx);
	dev = hw_dcd(ctx, ep, 0x1171, 0);
	if (!dev)
		goto out;
	device_id(id, sizeof(id), 0x1171);
	for (size_t i = 0; i < 2; i++) {
		snprintf(body, sizeof(body),
			 "{\"Policy\": \"Free\", \"RegionNumber\": 0, \"Length\": %s, "
			 "\"Tag\": \"6f1a2c3e-0000-4000-8000-0000000000aa\"}", sizes[i]);
		sent = commands_sent(ep);
		rc = fmd_redfish_action_dc_add(ctx, id, id, "0", body, strlen(body), &msg);
		CHECK(rc == -EINVAL && commands_sent(ep) == sent,
		      "DC add Length %s: %d, %u commands sent", sizes[i], rc,
		      commands_sent(ep) - sent);
	}
	answer(ep, 0x5604, NULL, 0);
	snprintf(body, sizeof(body),
		 "{\"Policy\": \"Free\", \"RegionNumber\": 0, \"Length\": 256, "
		 "\"Tag\": \"6f1a2c3e-0000-4000-8000-0000000000aa\"}");
	sent = commands_sent(ep);
	fmd_redfish_action_dc_add(ctx, id, id, "0", body, strlen(body), &msg);
	CHECK(commands_sent(ep) > sent, "DC add Length 256: nothing sent");
out:
	fmd_ctx_free(ctx);
	report("a size in MiB past 2^44 - 1 sends nothing", before);
}

/*
 * What a DCD Management command the device refused comes to. Initiate
 * Dynamic Capacity Add (5604h) "shall fail with Invalid Input", "with
 * Invalid Extent List", "with Resources Exhausted" and "with Retry Required"
 * (Section 7.6.7.6.5); Release (5605h) also "with Invalid Physical Address"
 * (Section 7.6.7.6.6); Set DC Region Configuration (5602h) "shall fail with
 * Unsupported" while blocks are allocated (Section 7.6.7.6.3). A command
 * that is not carried is a failure of the daemon, not of the request.
 */
static void test_dc_return_codes(void)
{
	static const struct { uint16_t rc; int err; } add[] = {
		{ CXLMI_RET_INPUT, -EINVAL },
		{ CXLMI_RET_EXTLIST, -EINVAL },
		{ CXLMI_RET_RESOURCEEXHAUSTED, -ENOSPC },
		{ CXLMI_RET_RETRY, -EAGAIN },
		{ CXLMI_RET_BUSY, -EAGAIN },
		{ CXLMI_RET_INTERNAL, -EIO },
	};
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	struct fmd_component *dev = hw_dcd(ctx, ep, 0xd1, 0);
	struct fmd_dc_extent *ext;
	int before = failures, rc;

	if (!dev)
		goto out;

	/*
	 * 5608h answered Unsupported at discovery: the extents are kept, with
	 * no tag list, and none of them pending.
	 */
	CHECK(!fmd_memdev_dcd(dev)->tags.known && fmd_memdev_dcd(dev)->tags.unsupported &&
	      fmd_dc_find_extent(dev, 0, 1ULL << 30) &&
	      !fmd_dc_find_extent(dev, 0, 1ULL << 30)->pending,
	      "a device without 5608h: known %d, unsupported %d",
	      fmd_memdev_dcd(dev)->tags.known, fmd_memdev_dcd(dev)->tags.unsupported);

	for (size_t i = 0; i < sizeof(add) / sizeof(add[0]); i++) {
		answer_rc(ep, 0x5604, add[i].rc);
		rc = fmd_dc_add(ctx, dev, 0, FMD_DC_POLICY_FREE, 0, 1ULL << 28,
				dc_tag, NULL, 0);
		CHECK(rc == add[i].err, "5604h return code %u: %d, not %d",
		      add[i].rc, rc, add[i].err);
	}

	/* a payload where 5604h has none: libcxlmi fails to carry it */
	answer(ep, 0x5604, (uint8_t[4]){ 0 }, 4);
	rc = fmd_dc_add(ctx, dev, 0, FMD_DC_POLICY_FREE, 0, 1ULL << 28,
			dc_tag, NULL, 0);
	CHECK(rc == -EIO, "5604h not carried: %d", rc);

	ext = fmd_dc_find_extent(dev, 0, 1ULL << 30);
	answer_rc(ep, 0x5605, CXLMI_RET_PADDR);
	rc = fmd_dc_release(ctx, dev, 0, FMD_DC_REL_POLICY_PRESCRIPTIVE, false,
			    false, 0, dc_tag, &ext, 1);
	CHECK(rc == -EINVAL, "5605h Invalid Physical Address: %d", rc);

	answer_rc(ep, 0x5605, CXLMI_RET_RESOURCEEXHAUSTED);
	rc = fmd_dc_release(ctx, dev, 0, FMD_DC_REL_POLICY_TAG_BASED, false,
			    false, 1ULL << 29, dc_tag, NULL, 0);
	CHECK(rc == -ENOSPC, "5605h Resources Exhausted: %d", rc);

	answer_rc(ep, 0x5606, CXLMI_RET_INPUT);
	rc = fmd_dc_reference(ctx, dev, dc_tag, true);
	CHECK(rc == -EINVAL, "5606h Invalid Input: %d", rc);

	answer_rc(ep, 0x5607, CXLMI_RET_RETRY);
	rc = fmd_dc_reference(ctx, dev, dc_tag, false);
	CHECK(rc == -EAGAIN, "5607h Retry Required: %d", rc);

	/*
	 * cxl-fmd refuses 5602h itself while it knows of an extent; the
	 * device's refusal is seen once that extent has been released.
	 */
	fmd_topology_remove(&ctx->topology, &ext->base);
	answer_rc(ep, 0x5602, CXLMI_RET_UNSUPPORTED);
	rc = fmd_dc_set_region_config(ctx, dev, fmd_dc_find_region(dev, 0, 0),
				      1ULL << 28, false);
	CHECK(rc == -EBUSY, "5602h Unsupported: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("a DCD's return codes", before);
}

static bool all_zero(const uint8_t *p, size_t n)
{
	while (n--) {
		if (*p++)
			return false;
	}
	return true;
}

/*
 * Fields a policy leaves reserved are sent as zero. Table 7-72 (5604h):
 * Region Number (03h) and Tag (0Ch) are "reserved when the Selection Policy
 * is set to Prescriptive", Length (04h) "when the Selection Policy is set to
 * Prescriptive or Enable Shared Access". Table 7-73 (5605h): Length (04h)
 * and Tag (0Ch) are reserved "when the Removal Policy is set to
 * Prescriptive". The device refuses each command, so that it is the last
 * one sent. A Free Length that is not a multiple of the region's block size
 * is not sent.
 */
static void test_dc_reserved_fields(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	struct fmd_component *dev = hw_dcd(ctx, ep, 0xd2, 0);
	struct fmd_dc_extent *ext;
	uint8_t pl[256];
	uint16_t op;
	unsigned int sent;
	size_t n;
	int before = failures, rc;

	if (!dev)
		goto out;
	ext = fmd_dc_find_extent(dev, 0, 1ULL << 30);

	answer_rc(ep, 0x5604, CXLMI_RET_INPUT);
	fmd_dc_add(ctx, dev, 0, FMD_DC_POLICY_PRESCRIPTIVE, 3, 1ULL << 28,
		   dc_tag, &ext, 1);
	n = last_payload(ep, &op, pl, sizeof(pl));
	CHECK(op == 0x5604 && n >= 0x48 && pl[0x02] == 2 && pl[0x03] == 0 &&
	      all_zero(pl + 0x04, 8) && all_zero(pl + 0x0c, 16) &&
	      get64(pl + 0x20) == 1ULL << 30,
	      "Prescriptive add: %04xh, region %u, length %" PRIu64 ", tag %s",
	      op, pl[0x03], get64(pl + 0x04),
	      all_zero(pl + 0x0c, 16) ? "zero" : "sent");

	answer_rc(ep, 0x5604, CXLMI_RET_INPUT);
	fmd_dc_add(ctx, dev, 0, FMD_DC_POLICY_SHARED_ACCESS, 0, 1ULL << 28,
		   dc_tag, NULL, 0);
	n = last_payload(ep, &op, pl, sizeof(pl));
	CHECK(op == 0x5604 && n >= 0x20 && pl[0x02] == 3 &&
	      all_zero(pl + 0x04, 8) && !memcmp(pl + 0x0c, dc_tag, 16),
	      "Enable Shared Access: %04xh, length %" PRIu64, op, get64(pl + 0x04));

	answer_rc(ep, 0x5605, CXLMI_RET_INPUT);
	fmd_dc_release(ctx, dev, 0, FMD_DC_REL_POLICY_PRESCRIPTIVE, false, false,
		       1ULL << 28, dc_tag, &ext, 1);
	n = last_payload(ep, &op, pl, sizeof(pl));
	CHECK(op == 0x5605 && n >= 0x48 && (pl[0x02] & 0x0f) == 1 &&
	      all_zero(pl + 0x04, 8) && all_zero(pl + 0x0c, 16),
	      "Prescriptive release: %04xh, length %" PRIu64 ", tag %s",
	      op, get64(pl + 0x04), all_zero(pl + 0x0c, 16) ? "zero" : "sent");

	sent = commands_sent(ep);
	rc = fmd_dc_add(ctx, dev, 0, FMD_DC_POLICY_FREE, 0, 100ULL << 20,
			dc_tag, NULL, 0);
	CHECK(rc == -EINVAL && commands_sent(ep) == sent,
	      "a Length of 100 MB in 256 MB blocks: %d, %u commands sent",
	      rc, commands_sent(ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("reserved fields of a DCD request", before);
}

/*
 * A DCD MLD of @nr LDs, each LD a host (Host ID = LD-ID, Table 7-67), with
 * two DC regions: region 0 unavailable ("Report 0 if the region is
 * unavailable", Table 8-347) and region 1 in 256 MB blocks. Discovery is
 * left without answers to 5601h, so each host is then refreshed on its own,
 * with the answers its test queues: the order discovery walks the LDs in
 * does not matter.
 */
static struct fmd_component *hw_dcd_mld(struct fmd_ctx *ctx, struct cxlmi_endpoint **epp,
					 uint64_t serial, uint8_t nr)
{
	struct cxlmi_endpoint *ep = new_mld(ctx, serial, nr, 0, 4);
	uint8_t info[0x54] = { 0 };
	char id[FMD_MAX_ID_LEN];
	struct fmd_component *dev;

	info[0x00] = nr;			/* Number of Hosts */
	info[0x01] = 2;				/* Number of Supported DC Regions */
	put16(info + 0x04, 0x07);		/* Free, Contiguous, Prescriptive */
	put16(info + 0x08, 0x03);		/* Tag-based, Prescriptive */
	put64(info + 0x0c, 8);			/* Total Dynamic Capacity: 2 GiB */
	put64(info + 0x14, 1ULL << 28);		/* Region 0: 256 MB blocks */
	put64(info + 0x1c, 1ULL << 28);		/* Region 1: 256 MB blocks */
	answer(ep, 0x5600, info, sizeof(info));
	fmd_discover(ctx);
	cxlmi_mock_clear_responses(ep);

	device_id(id, sizeof(id), serial);
	dev = fmd_topology_find(&ctx->topology, id);
	CHECK(dev && fmd_memdev_dcd(dev), "no DCD MLD %s", id);
	*epp = ep;
	return dev && fmd_memdev_dcd(dev) ? dev : NULL;
}

/*
 * Get Host DC Region Configuration (5601h, Tables 7-67 and 7-68): region 0
 * with no length, region 1 at @base, @decode_units of 256 MB decoded,
 * @len long, in 256 MB blocks, with Attributes1 @flags.
 */
static void answer_dc_regions(struct cxlmi_endpoint *ep, uint64_t base,
			      uint64_t decode_units, uint64_t len, uint8_t flags)
{
	uint8_t pl[0x04 + 2 * 0x28 + 0x10] = { 0 };
	uint8_t *r1 = pl + 0x04 + 0x28, *tail = pl + 0x04 + 2 * 0x28;

	pl[0x02] = 2;				/* Number of Available Regions */
	pl[0x03] = 2;				/* Number of Regions Returned */
	put64(r1 + 0x00, base);			/* Region Base */
	put64(r1 + 0x08, decode_units);		/* Region Decode Length */
	put64(r1 + 0x10, len);			/* Region Length */
	put64(r1 + 0x18, 1ULL << 28);		/* Region Block Size */
	r1[0x20] = flags;			/* Attributes1 */
	put16(tail + 0x00, 64);			/* Number of Extents Supported */
	put16(tail + 0x04, 63);			/* Number of Extents Available */
	put16(tail + 0x08, 16);			/* Number of Tags Supported */
	put16(tail + 0x0c, 15);			/* Number of Tags Available */
	answer(ep, 0x5601, pl, sizeof(pl));
}

struct dc_ext {
	uint64_t dpa, len;
	const uint8_t *tag;
	uint16_t seq;
};

/*
 * One page of Get DC Region Extent Lists (5603h, Tables 7-71 and 8-230):
 * @n extents from Starting Extent Index @start of @total, generation @gen.
 */
static void answer_dc_extents(struct cxlmi_endpoint *ep, uint32_t gen, uint32_t start,
			      uint32_t total, const struct dc_ext *e, int n)
{
	uint8_t pl[0x18 + 4 * 0x28] = { 0 };

	put16(pl + 0x00, 0);			/* Host ID */
	pl[0x04] = start;			/* Starting Extent Index */
	pl[0x08] = n;				/* Returned Extent Count */
	pl[0x0c] = total;			/* Total Extent Count */
	put64(pl + 0x10, gen);			/* Extent List Generation Number */
	for (int i = 0; i < n; i++) {
		uint8_t *x = pl + 0x18 + i * 0x28;

		put64(x + 0x00, e[i].dpa);	/* Starting DPA */
		put64(x + 0x08, e[i].len);	/* Length */
		if (e[i].tag)
			memcpy(x + 0x10, e[i].tag, 16);
		put16(x + 0x20, e[i].seq);	/* Shared Extent Sequence */
	}
	answer(ep, 0x5603, pl, 0x18 + n * 0x28);
}

/* refresh one host: its regions, its extents, and no 5608h */
static int dc_refresh_host(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
			   struct fmd_component *dev, int host, uint64_t base,
			   uint64_t decode_units, uint8_t flags,
			   const struct dc_ext *e, int n)
{
	answer_dc_regions(ep, base, decode_units, 1ULL << 30, flags);
	answer_dc_extents(ep, 1, 0, n, e, n);
	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	return fmd_dcd_refresh(ctx, dev, host);
}

static const uint8_t dc_tag_b[16] = {
	0x6f, 0x1a, 0x2c, 0x3e, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x0b,
};
static const uint8_t dc_tag_c[16] = {
	0x6f, 0x1a, 0x2c, 0x3e, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x0c,
};

#define DC_EXT_FIELDS(e) ((char *)(e) + offsetof(struct fmd_dc_extent, host_id))
#define DC_EXT_FIELDS_LEN (sizeof(struct fmd_dc_extent) - offsetof(struct fmd_dc_extent, host_id))

/*
 * 5603h returns the "Extent list for the specified host" (Table 7-71), and
 * "The DPA ranges exposed by the device to each host are independent of
 * one another" (Section 9.13.3): host 0 and host 1 can both list DPA 1 GiB,
 * with different tags. They are two extents, each its own, and refreshing
 * one host leaves the other's as it was.
 */
static void test_dc_same_dpa_two_hosts(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd3, 2);
	struct fmd_dc_extent *e0, *e1, saved;
	int before = failures, rc;

	if (!dev)
		goto out;

	rc = dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 4, 0,
			     &(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag, 0 }, 1);
	CHECK(rc == 0, "refresh of host 0: %d", rc);
	rc = dc_refresh_host(ctx, ep, dev, 1, 1ULL << 30, 4, 0,
			     &(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag_b, 0 }, 1);
	CHECK(rc == 0, "refresh of host 1: %d", rc);

	e0 = fmd_dc_find_extent(dev, 0, 1ULL << 30);
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	CHECK(e0 && e1 && e0 != e1, "the same DPA on two hosts is not two extents");
	if (!e0 || !e1 || e0 == e1)
		goto out;
	CHECK(!memcmp(e0->tag, dc_tag, 16) && !memcmp(e1->tag, dc_tag_b, 16),
	      "an extent has the other host's tag");
	CHECK(e0->region_id == 1 && e1->region_id == 1,
	      "regions %u and %u, not 1", e0->region_id, e1->region_id);
	CHECK(strcmp(e0->base.id, e1->base.id) &&
	      fmd_topology_find(&ctx->topology, e0->base.id) == &e0->base &&
	      fmd_topology_find(&ctx->topology, e1->base.id) == &e1->base,
	      "the two extents collide: %s, %s", e0->base.id, e1->base.id);

	/* host 1 again, with another tag: host 0's extent stays as it was */
	memcpy(DC_EXT_FIELDS(&saved), DC_EXT_FIELDS(e0), DC_EXT_FIELDS_LEN);
	rc = dc_refresh_host(ctx, ep, dev, 1, 1ULL << 30, 4, 0,
			     &(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag_c, 0 }, 1);
	CHECK(rc == 0, "refresh of host 1: %d", rc);
	CHECK(fmd_dc_find_extent(dev, 0, 1ULL << 30) == e0 &&
	      !memcmp(DC_EXT_FIELDS(&saved), DC_EXT_FIELDS(e0), DC_EXT_FIELDS_LEN),
	      "a refresh of host 1 changed host 0's extent");
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	CHECK(e1 && !memcmp(e1->tag, dc_tag_c, 16), "host 1's extent was not updated");

	/* host 1 lists nothing: its extent goes, host 0's stays */
	rc = dc_refresh_host(ctx, ep, dev, 1, 1ULL << 30, 4, 0, NULL, 0);
	CHECK(rc == 0 && !fmd_dc_find_extent(dev, 1, 1ULL << 30) &&
	      fmd_dc_find_extent(dev, 0, 1ULL << 30) == e0,
	      "host 1's empty list did not remove its extent alone");
out:
	fmd_ctx_free(ctx);
	report("the same DPA on two hosts", before);
}

/*
 * A host's extent is in a region of that host: "The DCD controls which DPA
 * range it assigns to each region for each host" (Section 9.13.3). Region 1
 * is at 1 GiB for host 0 and at 2 GiB for host 1; an extent of each at its
 * own base is in region 1. Host 1 listing DPA 1 GiB, which is in no region
 * of its own, is in no region, and the topology is not valid.
 */
static void test_dc_region_per_host(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd4, 2);
	struct fmd_dc_extent *e0, *e1;
	int before = failures, invalid;

	if (!dev)
		goto out;

	dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 4, 0,
			&(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag, 0 }, 1);
	dc_refresh_host(ctx, ep, dev, 1, 2ULL << 30, 4, 0,
			&(struct dc_ext){ 2ULL << 30, 1ULL << 28, dc_tag_b, 0 }, 1);
	e0 = fmd_dc_find_extent(dev, 0, 1ULL << 30);
	e1 = fmd_dc_find_extent(dev, 1, 2ULL << 30);
	CHECK(e0 && e0->region_id == 1 && e1 && e1->region_id == 1,
	      "extents at each host's base are not in its region 1");
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 0, "%d violations with each extent in its region", invalid);

	dc_refresh_host(ctx, ep, dev, 1, 2ULL << 30, 4, 0,
			&(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag_b, 0 }, 1);
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	CHECK(e1 && e1->region_id == FMD_DC_REGION_NONE,
	      "host 1's extent at host 0's base is in region %d",
	      e1 ? e1->region_id : -1);
	CHECK(fmd_dc_find_extent(dev, 0, 1ULL << 30) == e0 && e0->region_id == 1,
	      "host 0's extent changed");
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid > 0, "an extent in no region of its host is valid");
out:
	fmd_ctx_free(ctx);
	report("a host's extents in its own regions", before);
}

/*
 * "All extents for this region shall describe a memory range >= the Region
 * Base and < the Region Base + Region Decode Length" (Table 8-347): an
 * extent past Region Length, within the decode length, is in the region.
 * One past the decode length is in none.
 */
static void test_dc_extent_in_decode_tail(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd5, 2);
	struct dc_ext e[2] = {
		{ (1ULL << 30) + (5ULL << 28), 1ULL << 28, dc_tag, 0 },	/* 2.25 GiB */
		{ 3ULL << 30, 1ULL << 28, dc_tag, 0 },			/* past it */
	};
	struct fmd_dc_extent *ext;
	int before = failures;

	if (!dev)
		goto out;

	/* 1 GiB long, 2 GiB (8 x 256 MB) decoded */
	dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 8, 0, e, 2);
	ext = fmd_dc_find_extent(dev, 0, e[0].dpa);
	CHECK(ext && ext->region_id == 1, "an extent past Region Length is in region %d",
	      ext ? ext->region_id : -1);
	ext = fmd_dc_find_extent(dev, 0, e[1].dpa);
	CHECK(ext && ext->region_id == FMD_DC_REGION_NONE,
	      "an extent past the decode length is in region %d",
	      ext ? ext->region_id : -1);
out:
	fmd_ctx_free(ctx);
	report("an extent in a region's decode length", before);
}

/*
 * A Prescriptive Initiate Dynamic Capacity Release (5605h, Table 7-73) names
 * extents of the host released from, in its DPA space. Host 0 and host 1
 * share tag dc_tag at different DPAs: releasing host 1's sends host 1's
 * Starting DPA, Length, Tag and Shared Extent Sequence; host 0's extent is
 * not host 1's to release, and nothing is sent.
 */
static void test_dc_prescriptive_release_payload(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd6, 2);
	struct fmd_dc_extent *e0, *e1;
	uint8_t pl[256];
	unsigned int sent;
	uint16_t op;
	size_t n;
	int before = failures, rc;

	if (!dev)
		goto out;

	dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 4, 0x08,
			&(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag, 0 }, 1);
	dc_refresh_host(ctx, ep, dev, 1, 2ULL << 30, 4, 0x08,
			(struct dc_ext[2]){ { 2ULL << 30, 1ULL << 28, dc_tag_b, 0 },
					    { (2ULL << 30) + (1ULL << 29), 1ULL << 28, dc_tag, 0 } }, 2);
	e0 = fmd_dc_find_extent(dev, 0, 1ULL << 30);
	e1 = fmd_dc_find_extent(dev, 1, (2ULL << 30) + (1ULL << 29));
	CHECK(e0 && e1, "no extents of tag dc_tag on hosts 0 and 1");
	if (!e0 || !e1)
		goto out;

	answer_rc(ep, 0x5605, CXLMI_RET_INPUT);
	rc = fmd_dc_release(ctx, dev, 1, FMD_DC_REL_POLICY_PRESCRIPTIVE, false, false,
			    0, NULL, &e1, 1);
	n = last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == -EINVAL && op == 0x5605 && n >= 0x48 && pl[0] == 1 && pl[1] == 0 &&
	      pl[0x1c] == 1 &&
	      get64(pl + 0x20) == (2ULL << 30) + (1ULL << 29) &&
	      get64(pl + 0x28) == 1ULL << 28 &&
	      !memcmp(pl + 0x30, dc_tag, 16) && pl[0x40] == 0 && pl[0x41] == 0,
	      "5605h for host 1: %d, %04xh, host %u, DPA %#" PRIx64, rc, op,
	      pl[0] | pl[1] << 8, get64(pl + 0x20));

	sent = commands_sent(ep);
	rc = fmd_dc_release(ctx, dev, 1, FMD_DC_REL_POLICY_PRESCRIPTIVE, false, false,
			    0, NULL, &e0, 1);
	CHECK(rc == -ENOENT && commands_sent(ep) == sent,
	      "host 0's extent released from host 1: %d, %u commands", rc,
	      commands_sent(ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("a Prescriptive release names its host's extents", before);
}

/*
 * Set DC Region Configuration (5602h) "shall fail with Unsupported" "When
 * all capacity has been released from the DC Region on all hosts, and one or
 * more blocks are allocated to the specified region" (Section 7.6.7.6.3).
 * Host 1 alone has an extent in region 1: reconfiguring it, through host
 * 0's view, is refused without asking the device.
 */
static void test_dc_set_region_busy_any_host(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd7, 2);
	unsigned int sent;
	int before = failures, rc;

	if (!dev)
		goto out;

	dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 4, 0, NULL, 0);
	dc_refresh_host(ctx, ep, dev, 1, 1ULL << 30, 4, 0,
			&(struct dc_ext){ 1ULL << 30, 1ULL << 28, dc_tag, 0 }, 1);
	sent = commands_sent(ep);
	rc = fmd_dc_set_region_config(ctx, dev, fmd_dc_find_region(dev, 0, 1),
				      1ULL << 28, false);
	CHECK(rc == -EBUSY && commands_sent(ep) == sent,
	      "region 1 with host 1's extent in it: %d, %u commands sent", rc,
	      commands_sent(ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("a region in use by any host", before);
}

struct dc_tag_row {
	const uint8_t *tag;
	bool fm_ref;
	int ref[4], n_ref;		/* LDs in the Reference Bitmap */
	int pend[4], n_pend;		/* and in the Pending Reference Bitmap */
};

/*
 * One page of Dynamic Capacity List Tags (5608h, Tables 7-77 and 7-78):
 * @n entries of @total, generation @gen, Validity Bitmap @validity.
 */
static void answer_dc_tags(struct cxlmi_endpoint *ep, uint32_t gen, uint32_t total,
			   uint8_t validity, const struct dc_tag_row *r, int n)
{
	uint8_t pl[0x10 + 4 * 0x54] = { 0 };

	put64(pl + 0x00, gen);			/* Generation Number */
	pl[0x04] = total;			/* Total Number of Tags */
	pl[0x08] = n;				/* Number of Tags Returned */
	pl[0x0c] = validity;			/* Validity Bitmap */
	for (int i = 0; i < n; i++) {
		uint8_t *t = pl + 0x10 + i * 0x54;

		memcpy(t, r[i].tag, 16);	/* Tag */
		t[0x10] = r[i].fm_ref;		/* Flags Bit[0]: FM Holds Reference */
		for (int j = 0; j < r[i].n_ref; j++)
			t[0x14 + r[i].ref[j] / 8] |= 1 << (r[i].ref[j] % 8);
		for (int j = 0; j < r[i].n_pend; j++)
			t[0x34 + r[i].pend[j] / 8] |= 1 << (r[i].pend[j] % 8);
	}
	answer(ep, 0x5608, pl, 0x10 + n * 0x54);
}

/* refresh one host, with the 5608h answers the caller queued */
static int dc_refresh_host_tags(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
				struct fmd_component *dev, int host,
				const struct dc_ext *e, int n)
{
	answer_dc_regions(ep, 1ULL << 30, 4, 1ULL << 30, 0x08);
	answer_dc_extents(ep, 1, 0, n, e, n);
	return fmd_dcd_refresh(ctx, dev, host);
}

/* "Pending" of the first of @dev's region (@ld, @region)'s MemoryExtents[] linking @ext */
static int dc_region_pending(struct fmd_component *dev, struct fmd_ctx *ctx,
			     const char *ld, const char *region, const char *ext)
{
	json_t *j = fmd_redfish_dc_memory_region(ctx, dev->id, dev->id, ld, region);
	json_t *arr = json_object_get(j, "MemoryExtents");
	int ret = -1;

	for (size_t i = 0; i < json_array_size(arr); i++) {
		json_t *e = json_array_get(arr, i);
		const char *u = json_string_value(json_object_get(json_object_get(e, "ExtentLink"),
								  "@odata.id"));

		if (u && strrchr(u, '/') && !strcmp(strrchr(u, '/') + 1, ext)) {
			json_t *p = json_object_get(e, "Pending");

			ret = json_is_boolean(p) ? json_is_true(p) : -2;
			break;
		}
	}
	json_decref(j);
	return ret;
}

/* Status.State and Status.Health of MemoryExtent @ext of @dev, or "" */
static void dc_extent_status(struct fmd_component *dev, struct fmd_ctx *ctx, const char *ext,
			     char *state, char *health, size_t len)
{
	json_t *j = fmd_redfish_dc_memory_extent(ctx, dev->id, dev->id, ext);
	json_t *st = json_object_get(j, "Status");
	const char *s = json_string_value(json_object_get(st, "State"));
	const char *h = json_string_value(json_object_get(st, "Health"));

	snprintf(state, len, "%s", s ? s : "");
	snprintf(health, len, "%s", h ? h : "");
	json_decref(j);
}

/* Status.Health of MemoryRegion (@ld, @region) of @dev, or "" */
static void dc_region_health(struct fmd_component *dev, struct fmd_ctx *ctx, const char *ld,
			     const char *region, char *health, size_t len)
{
	json_t *j = fmd_redfish_dc_memory_region(ctx, dev->id, dev->id, ld, region);
	const char *h = json_string_value(json_object_get(json_object_get(j, "Status"), "Health"));

	snprintf(health, len, "%s", h ? h : "");
	json_decref(j);
}

/*
 * The Pending Reference Bitmap is per tag, "Each 1 indicates an LD for
 * which the tagged capacity has been added with no host response yet"
 * (Table 7-78): it is the tag's, and no extent is made pending from it
 * (cxl-fmd's choice). Hosts 0 and 1 both list tag dc_tag, host 0 in the
 * Reference Bitmap and host 1 in the Pending one: neither listed extent is
 * pending, and host 1's add is in the tag's row. FM Holds Reference is the
 * tag's.
 */
static void test_dc_tags_pending(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd8, 2);
	struct dc_ext e = { 1ULL << 30, 1ULL << 28, dc_tag, 0 };
	struct fmd_dc_extent *e0, *e1;
	const struct fmd_dc_tag *t;
	char state[32], health[32];
	json_t *view, *pend;
	int before = failures;

	if (!dev)
		goto out;

	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, true,
		       { 0 }, 1, { 1 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 0, &e, 1);
	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, true,
		       { 0 }, 1, { 1 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, &e, 1);
	e0 = fmd_dc_find_extent(dev, 0, 1ULL << 30);
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(e0 && e1 && t, "no extents, or no tag entry");
	if (!e0 || !e1 || !t)
		goto out;
	CHECK(dc_region_pending(dev, ctx, "1", "1", "ext_1_40000000") == 0,
	      "host 1's listed extent is Pending %d",
	      dc_region_pending(dev, ctx, "1", "1", "ext_1_40000000"));
	dc_extent_status(dev, ctx, "ext_1_40000000", state, health, sizeof(state));
	CHECK(!strcmp(state, "Enabled") && !strcmp(health, "OK"),
	      "host 1's listed extent is %s, %s", state, health);
	CHECK(t->fm_ref && fmd_dc_bm_test(t->ref, 0) && fmd_dc_bm_test(t->pend, 1) &&
	      !fmd_dc_bm_test(t->ref, 1), "the tag's entry is not as the device sent it");
	view = fmd_redfish_dc_tags_oem(ctx, dev, dev->id, dev->id);
	pend = json_object_get(json_object_get(json_object_get(view, "Cxl"),
					       "DynamicCapacityTags"), "Tags");
	pend = json_object_get(json_array_get(pend, 0), "PendingLDs");
	CHECK(json_array_size(pend) == 1 && json_integer_value(json_array_get(pend, 0)) == 1,
	      "the tag view has no PendingLDs [1]");
	json_decref(view);

	answer_dc_tags(ep, 2, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 0, 1 }, 2, { 0 }, 0 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, &e, 1);
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(t && !t->fm_ref && !fmd_dc_bm_test(t->pend, 1) &&
	      fmd_memdev_dcd(dev)->tags.gen == 2, "the tag list was not replaced");
out:
	fmd_ctx_free(ctx);
	report("an add not answered is the tag's, not an extent's", before);
}

/*
 * Review F12 and DCD-Q T4b: one host holds accepted capacity and an add it
 * has not answered under one tag, its bit set in both the Reference and the
 * Pending Reference Bitmap (Table 7-78). Its two extents that 5603h lists
 * are neither of them pending, nor Starting: the bitmap does not say which
 * extent, if any, is the add.
 */
static void test_dc_tags_accepted_and_pending(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xf1, 2);
	struct dc_ext e[2] = {
		{ 1ULL << 30, 1ULL << 28, dc_tag, 0 },
		{ (1ULL << 30) + (1ULL << 28), 1ULL << 28, dc_tag, 0 },
	};
	static const char *const ids[2] = { "ext_0_40000000", "ext_0_50000000" };
	char state[32], health[32];
	int before = failures;

	if (!dev)
		goto out;
	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 0 }, 1, { 0 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 0, e, 2);
	for (int i = 0; i < 2; i++) {
		int p = dc_region_pending(dev, ctx, "0", "1", ids[i]);

		dc_extent_status(dev, ctx, ids[i], state, health, sizeof(state));
		CHECK(p == 0 && !strcmp(state, "Enabled"),
		      "%s, accepted under a tag with an add outstanding: Pending %d, %s",
		      ids[i], p, state);
	}
out:
	fmd_ctx_free(ctx);
	report("accepted and pending capacity under one tag", before);
}

/*
 * Review F12: a tag list that cannot be read is not "nothing pending". The
 * bitmaps are "reserved if the Pending Reference Bitmaps Valid bit is not
 * set" (Table 7-78), and with no read there is not even that. A failed
 * 5608h, with no list read before and after one was: the tag view has
 * TagsKnown false and no Tags, the host's region and its tagged extent are
 * Warning. A device that answers Unsupported has no list to fail: OK.
 */
static void test_dc_tags_unreadable(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xf2, 2);
	struct dc_ext e = { 1ULL << 30, 1ULL << 28, dc_tag, 0 };
	char state[32], health[32], rhealth[32];
	json_t *view, *tags;
	int before = failures;

	if (!dev)
		goto out;
	for (int round = 0; round < 3; round++) {
		/* 0: never read; 1: read with host 0 pending; 2: then failed */
		if (round == 1)
			answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
				       { 0 }, 0, { 0 }, 1 }, 1);
		else
			answer_rc(ep, 0x5608, CXLMI_RET_INTERNAL);
		dc_refresh_host_tags(ctx, ep, dev, 0, &e, 1);

		dc_extent_status(dev, ctx, "ext_0_40000000", state, health, sizeof(state));
		dc_region_health(dev, ctx, "0", "1", rhealth, sizeof(rhealth));
		view = fmd_redfish_dc_tags_oem(ctx, dev, dev->id, dev->id);
		tags = json_object_get(json_object_get(json_object_get(view, "Cxl"),
						       "DynamicCapacityTags"), "Tags");
		CHECK(dc_region_pending(dev, ctx, "0", "1", "ext_0_40000000") == 0 &&
		      !strcmp(state, "Enabled"), "round %d: Pending %d, %s", round,
		      dc_region_pending(dev, ctx, "0", "1", "ext_0_40000000"), state);
		if (round == 1)
			CHECK(!strcmp(health, "OK") && !strcmp(rhealth, "OK") &&
			      json_array_size(tags) == 1,
			      "a list read: extent %s, region %s, %zu tags", health, rhealth,
			      json_array_size(tags));
		else
			CHECK(!strcmp(health, "Warning") && !strcmp(rhealth, "Warning") && !tags,
			      "round %d, no list: extent %s, region %s, Tags %s", round,
			      health, rhealth, tags ? "given" : "absent");
		json_decref(view);
	}

	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	dc_refresh_host_tags(ctx, ep, dev, 0, &e, 1);
	dc_extent_status(dev, ctx, "ext_0_40000000", state, health, sizeof(state));
	dc_region_health(dev, ctx, "0", "1", rhealth, sizeof(rhealth));
	CHECK(!strcmp(health, "OK") && !strcmp(rhealth, "OK"),
	      "5608h Unsupported: extent %s, region %s", health, rhealth);
out:
	fmd_ctx_free(ctx);
	report("a tag list that cannot be read", before);
}

/*
 * The bitmaps are "reserved if the ... Valid bit is not set" (Table 7-78):
 * with a Validity Bitmap of 0 nothing is pending, whatever they hold.
 */
static void test_dc_tags_validity0(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd9, 2);
	struct dc_ext e = { 1ULL << 30, 1ULL << 28, dc_tag, 0 };
	struct fmd_dc_extent *e1;
	int before = failures;

	if (!dev)
		goto out;

	answer_dc_tags(ep, 1, 1, 0x00, &(struct dc_tag_row){ dc_tag, false,
		       { 0, 1 }, 2, { 0, 1 }, 2 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, &e, 1);
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	CHECK(e1 && !e1->pending, "pending from a Pending Reference Bitmap that is not valid");
	CHECK(fmd_memdev_dcd(dev)->tags.known && fmd_memdev_dcd(dev)->tags.validity == 0,
	      "the list was not kept, with its Validity Bitmap");
out:
	fmd_ctx_free(ctx);
	report("tag bitmaps that are not valid", before);
}

/*
 * "Bit[0] of the first byte represents LD 0, and bit[7] of the last byte
 * represents LD 255" (Table 7-78): LDs past 63 are kept.
 */
static void test_dc_tags_ld200(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xda, 2);
	const struct fmd_dc_tag *t;
	int before = failures;

	if (!dev)
		goto out;

	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 64, 255 }, 2, { 200 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 0, NULL, 0);
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(t && fmd_dc_bm_test(t->pend, 200) && !fmd_dc_bm_test(t->pend, 199) &&
	      fmd_dc_bm_test(t->ref, 64) && fmd_dc_bm_test(t->ref, 255) &&
	      !fmd_dc_bm_test(t->ref, 256), "LDs 64, 200 and 255 are not in the entry");
out:
	fmd_ctx_free(ctx);
	report("tag bitmaps up to LD 255", before);
}

/*
 * The Generation Number "shall change every time the remainder of the
 * command's payload would change" (Table 7-77): when it changes between
 * pages, the list is read again from Starting Index 0, and nothing of the
 * old generation is kept. A list that keeps changing past the retries
 * leaves the last complete one, not known to be current.
 */
static void test_dc_tags_generation_restart(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xdb, 2);
	struct fmd_dcd_info *dcd;
	struct dc_tag_row a = { dc_tag, false, { 0 }, 1, { 0 }, 0 };
	struct dc_tag_row b = { dc_tag_b, false, { 0 }, 1, { 0 }, 0 };
	struct dc_tag_row c = { dc_tag_c, false, { 1 }, 1, { 0 }, 0 };
	uint8_t pl[16];
	uint16_t op;
	int before = failures;

	if (!dev)
		goto out;
	dcd = fmd_memdev_dcd(dev);

	answer_dc_tags(ep, 1, 2, 0x03, &a, 1);	/* page 1, generation 1 */
	answer_dc_tags(ep, 2, 2, 0x03, &c, 1);	/* page 2, generation 2 */
	answer_dc_tags(ep, 2, 2, 0x03, &b, 1);	/* again: page 1 */
	answer_dc_tags(ep, 2, 2, 0x03, &c, 1);	/* page 2 */
	dc_refresh_host_tags(ctx, ep, dev, 0, NULL, 0);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(dcd->tags.known && dcd->tags.gen == 2 && dcd->tags.n == 2 &&
	      !fmd_dc_tag_find(dev, dc_tag) && fmd_dc_tag_find(dev, dc_tag_b) &&
	      fmd_dc_tag_find(dev, dc_tag_c),
	      "after a restart: known %d, generation %u, %u tags, old tag %s",
	      dcd->tags.known, dcd->tags.gen, dcd->tags.n,
	      fmd_dc_tag_find(dev, dc_tag) ? "kept" : "gone");
	CHECK(op == 0x5608 && pl[0] == 1, "the last page read was not page 2: %04xh at %u",
	      op, pl[0]);

	/* every second page of a new generation: four reads, then given up */
	for (int i = 0; i < 4; i++) {
		answer_dc_tags(ep, 10 + 2 * i, 2, 0x03, &a, 1);
		answer_dc_tags(ep, 11 + 2 * i, 2, 0x03, &a, 1);
	}
	dc_refresh_host_tags(ctx, ep, dev, 0, NULL, 0);
	CHECK(!dcd->tags.known && dcd->tags.gen == 2 && dcd->tags.n == 2 &&
	      fmd_dc_tag_find(dev, dc_tag) == NULL,
	      "a list that kept changing: known %d, generation %u, %u tags",
	      dcd->tags.known, dcd->tags.gen, dcd->tags.n);
	CHECK(dcd->tags.v && !memcmp(dcd->tags.v[0].tag, dc_tag_b, 16),
	      "the last complete list was not kept");
	answer_rc(ep, 0x5608, CXLMI_RET_INTERNAL);
	dc_refresh_host_tags(ctx, ep, dev, 0, NULL, 0);
	CHECK(!dcd->tags.known && dcd->tags.n == 2 && !dcd->tags.unsupported,
	      "a failed read dropped the list");
out:
	fmd_ctx_free(ctx);
	report("a tag list that changes as it is read", before);
}

/*
 * Capacity the FM holds a reference on, that no host has: 5608h has its
 * tag, FM Holds Reference set, no LD in either bitmap; no host's 5603h
 * lists it. It is an entry of the tag list, and no extent.
 */
static void test_dc_tags_orphan(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xdc, 2);
	struct fmd_component *child;
	const struct fmd_dc_tag *t;
	int before = failures, n = 0;

	if (!dev)
		goto out;

	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, true,
		       { 0 }, 0, { 0 }, 0 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 0, NULL, 0);
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_DC_EXTENT)
		n++;
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(fmd_memdev_dcd(dev)->tags.n == 1 && t && t->fm_ref && n == 0,
	      "%u tags, %d extents", fmd_memdev_dcd(dev)->tags.n, n);
out:
	fmd_ctx_free(ctx);
	report("an FM reference no host holds", before);
}

/* MemorySizeMiB of CXLLogicalDevice @ld of @dev, or -1 */
static long long ld_size_mib(struct fmd_ctx *ctx, struct fmd_component *dev, const char *ld)
{
	json_t *j = fmd_redfish_cxl_logical_device(ctx, dev->id, dev->id, ld);
	json_t *v = json_object_get(j, "MemorySizeMiB");
	long long ret = json_is_integer(v) ? (long long)json_integer_value(v) : -1;

	json_decref(j);
	return ret;
}

/*
 * Review F12: an LD's MemorySizeMiB "shall equate to the sum of the dynamic
 * capacity extents and the static capacity assigned to this logical device"
 * (CXLLogicalDevice_v1.xml). Host 1 of a DCD MLD gets two 256 MB extents,
 * under a tag with an add of host 1 outstanding: LD 1 grows by 512 MiB,
 * LD 0 does not; when 5603h no longer lists them, it
 * is back.
 */
static void test_dc_ld_size(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xde, 2);
	struct dc_ext e[2] = {
		{ 1ULL << 30, 1ULL << 28, dc_tag, 0 },
		{ (1ULL << 30) + (1ULL << 28), 1ULL << 28, dc_tag, 0 },
	};
	long long ld0, ld1;
	int before = failures;

	if (!dev)
		goto out;
	ld0 = ld_size_mib(ctx, dev, "0");
	ld1 = ld_size_mib(ctx, dev, "1");
	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 1 }, 1, { 1 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, e, 2);
	CHECK(ld_size_mib(ctx, dev, "1") == ld1 + 512 && ld_size_mib(ctx, dev, "0") == ld0,
	      "LD 1 %lld MiB, LD 0 %lld MiB; %lld and %lld before, LD 1's host has 512 MiB",
	      ld_size_mib(ctx, dev, "1"), ld_size_mib(ctx, dev, "0"), ld1, ld0);
	dc_refresh_host(ctx, ep, dev, 1, 1ULL << 30, 4, 0, NULL, 0);
	CHECK(ld_size_mib(ctx, dev, "1") == ld1, "LD 1 %lld MiB with no extents, not %lld",
	      ld_size_mib(ctx, dev, "1"), ld1);
out:
	fmd_ctx_free(ctx);
	report("an LD's size includes its host's extents", before);
}

/*
 * Whether 5603h lists an extent the host has not responded to is not said
 * (only Get Dynamic Capacity Extent List, 4801h, excludes them): host 1's
 * list does not have it, and the tag's Pending Reference Bitmap has host 1.
 * No extent is made up for it; the tag list has it.
 */
static void test_dc_pending_unlisted(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xdd, 2);
	const struct fmd_dc_tag *t;
	int before = failures;

	if (!dev)
		goto out;

	answer_dc_tags(ep, 1, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 0 }, 0, { 1 }, 1 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, NULL, 0);
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(!dev->first_child || !fmd_dc_find_extent(dev, 1, 1ULL << 30),
	      "an extent was made up");
	CHECK(t && fmd_dc_bm_test(t->pend, 1), "the tag list has no pending host 1");
out:
	fmd_ctx_free(ctx);
	report("a pending add 5603h does not list", before);
}

/*
 * A host's 5603h that does not complete leaves its extents as the last one
 * that did, marked stale, and the other hosts and 5608h are still read.
 * Host 0's list changes on page 2 of every read, past the retries: its
 * extent at 1 GiB is kept, stale, with Status.Health Warning; host 1 is
 * refreshed, 5608h is sent last, and the refresh returns host 0's -EAGAIN.
 */
static void test_dc_list_stale_keeps_old(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xde, 2);
	struct dc_ext old = { 1ULL << 30, 1ULL << 28, dc_tag, 0 };
	struct dc_ext e[2] = {
		{ (1ULL << 30) + (1ULL << 28), 1ULL << 28, dc_tag_b, 0 },
		{ (1ULL << 30) + (1ULL << 29), 1ULL << 28, dc_tag_b, 1 },
	};
	struct fmd_component *child;
	struct fmd_ld *ld0;
	char id[FMD_MAX_ID_LEN];
	json_t *ext;
	int before = failures, rc;

	if (!dev)
		goto out;
	device_id(id, sizeof(id), 0xde);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);

	rc = dc_refresh_host(ctx, ep, dev, 0, 1ULL << 30, 4, 0x08, &old, 1);
	CHECK(rc == 0 && ld0 && ld0->dc_sync == FMD_DC_SYNC_OK && ld0->dc_ext_gen == 1,
	      "host 0's first read: %d, sync %d", rc, ld0 ? ld0->dc_sync : -1);

	/* every host, in the order the refresh takes them */
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		answer_dc_regions(ep, 1ULL << 30, 4, 1ULL << 30, 0x08);
		if (fmd_to_ld(child)->ld_id == 1) {
			answer_dc_extents(ep, 7, 0, 1, &e[0], 1);
			continue;
		}
		for (int i = 0; i < 4; i++) {
			answer_dc_extents(ep, 20 + 2 * i, 0, 2, &e[0], 1);
			answer_dc_extents(ep, 21 + 2 * i, 1, 2, &e[1], 1);
		}
	}
	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	rc = fmd_dcd_refresh(ctx, dev, -1);

	CHECK(rc == -EAGAIN, "the refresh returned %d, not host 0's -EAGAIN", rc);
	CHECK(last_opcode(ep) == 0x5608, "the last command was %04xh, not 5608h",
	      last_opcode(ep));
	CHECK(fmd_dc_find_extent(dev, 0, old.dpa) && !fmd_dc_find_extent(dev, 0, e[0].dpa),
	      "host 0's extents are not those of its last complete read");
	CHECK(ld0 && ld0->dc_sync == FMD_DC_SYNC_STALE && ld0->dc_sync_err == -EAGAIN &&
	      ld0->dc_ext_gen == 1, "host 0: sync %d, error %d, generation %u",
	      ld0 ? ld0->dc_sync : -1, ld0 ? ld0->dc_sync_err : 0,
	      ld0 ? ld0->dc_ext_gen : 0);
	CHECK(fmd_dc_find_extent(dev, 1, e[0].dpa) &&
	      fmd_dc_host_sync(dev, 1) == FMD_DC_SYNC_OK,
	      "host 1 was not refreshed after host 0 failed");

	ext = fmd_redfish_dc_memory_extent(ctx, id, id, "ext_0_40000000");
	CHECK(ext && !strcmp(json_string_value(json_object_get(json_object_get(ext,
	      "Status"), "Health")) ?: "", "Warning"),
	      "a stale host's extent is not Health Warning");
	json_decref(ext);
	ext = fmd_redfish_dc_memory_extent(ctx, id, id, "ext_1_50000000");
	CHECK(ext && !strcmp(json_string_value(json_object_get(json_object_get(ext,
	      "Status"), "Health")) ?: "", "OK"),
	      "host 1's extent is not Health OK");
	json_decref(ext);
out:
	fmd_ctx_free(ctx);
	report("a host whose extent list cannot be read", before);
}

/*
 * A bind in the background, to its end. The switch answers Bind vPPB with
 * Background Command Started; Background Operation Status (0002h, Table
 * 8-217) then says whether it is in progress (Bit[0]), how far it is
 * (Bits[7:1]) and, when it is complete, its return code.
 */
static struct fmd_bind_key bind_key(const char *sw, uint8_t vcs, uint8_t vppb,
				    uint8_t port, uint16_t ld);

/* The state of Connection @id, -1 if there is none */
static int conn_state(struct fmd_ctx *ctx, const char *id)
{
	struct fmd_conn_view v;
	int state = -1;

	fmd_topology_rdlock(&ctx->topology);
	if (!fmd_conn_lookup(ctx, id, &v))
		state = v.state;
	fmd_topology_rdunlock(&ctx->topology);
	return state;
}

static int ops_count(struct fmd_ctx *ctx)
{
	int n = 0;

	for (struct fmd_binding_op *op = ctx->binding_ops; op; op = op->next)
		n++;
	return n;
}

static void answer_bg_status(struct cxlmi_endpoint *ep, bool running,
			     uint8_t percent, uint16_t opcode, uint16_t rc)
{
	uint8_t pl[8] = { 0 };

	pl[0] = (percent << 1) | (running ? 1 : 0);
	put16(pl + 2, opcode);			/* Command Opcode */
	put16(pl + 4, rc);			/* Return Code */
	answer(ep, 0x0002, pl, sizeof(pl));
}

static void test_bind_task(bool succeeds)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, succeeds ? 0x5007 : 0x5008);
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	int before = failures, rc;
	char id[FMD_MAX_ID_LEN], cid[FMD_CONN_ID_LEN];
	struct fmd_bind_key key;
	struct fmd_switch *sw;
	struct fmd_task *task;

	fmd_discover(ctx);
	device_id(id, sizeof(id), succeeds ? 0x5007 : 0x5008);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	/* LD 1 of port 5 to vPPB 1 of VCS 2: started, not done */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_bind(ctx, sw, 2, 1, 5, 1);
	CHECK(rc == FMD_OP_STARTED, "bind: %d", rc);

	key = bind_key(id, 2, 1, 5, 1);
	fmd_conn_id_format(&key, cid, sizeof(cid));
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, &key, 0);
	CHECK(task && task->state == FMD_TASK_RUNNING, "no running task");
	if (!task)
		goto out;
	CHECK(!strcmp(task->connection_id, cid), "task for %s, not %s",
	      task->connection_id, cid);

	/* in progress, 40% */
	answer_bg_status(ep, true, 40, 0x5201, 0);
	fmd_tasks_poll(ctx);
	CHECK(last_opcode(ep) == 0x0002, "the switch was not asked: %04xh",
	      last_opcode(ep));
	CHECK(task->state == FMD_TASK_RUNNING && task->percent == 40,
	      "state %d at %d%%", task->state, task->percent);
	/* the vPPB reads unbound: the op says what it is becoming */
	CHECK(conn_state(ctx, cid) == FMD_CONN_STARTING && ops_count(ctx) == 1,
	      "Connection %s while it is bound: state %d, %d ops", cid,
	      conn_state(ctx, cid), ops_count(ctx));

	/* a switch that does not answer once is asked again */
	fmd_tasks_poll(ctx);
	CHECK(task->state == FMD_TASK_RUNNING, "one failed poll ended the task");

	if (succeeds) {
		/* complete, Success; the VCSs are read to see the binding */
		answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_SUCCESS);
		answer_vcs(ep, 0, 0, port2, unbound);
		answer_vcs(ep, 2, 1, ld1, unbound);
		fmd_tasks_poll(ctx);
		CHECK(task->state == FMD_TASK_COMPLETED && task->percent == 100,
		      "state %d at %d%%: %s", task->state, task->percent,
		      task->message);
		CHECK(conn_state(ctx, cid) >= 0 && conn_state(ctx, cid) != FMD_CONN_STARTING &&
		      !ops_count(ctx) && fmd_binding_mark_find(ctx, &key),
		      "after the bind: state %d, %d ops", conn_state(ctx, cid),
		      ops_count(ctx));
	} else {
		/* complete, with the return code of a failure */
		answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_INTERNAL);
		fmd_tasks_poll(ctx);
		CHECK(task->state == FMD_TASK_EXCEPTION, "state %d: %s",
		      task->state, task->message);
		CHECK(conn_state(ctx, cid) < 0 && !ops_count(ctx) &&
		      !fmd_binding_mark_find(ctx, &key),
		      "a bind that failed leaves Connection state %d, %d ops",
		      conn_state(ctx, cid), ops_count(ctx));
	}

	/* over: the switch is not asked again */
	rc = commands_sent(ep);
	fmd_tasks_poll(ctx);
	CHECK((int)commands_sent(ep) == rc, "a finished task is still polled");
out:
	fmd_ctx_free(ctx);
	report(succeeds ? "bind in the background, to success" :
			  "bind in the background, to failure", before);
}

/* a switch that is busy with a background command says so */
static void test_bind_busy(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5009);
	int before = failures, rc;
	struct fmd_switch *sw;
	char id[FMD_MAX_ID_LEN];

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x5009);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (sw) {
		unsigned int sent, once;

		answer_rc(ep, 0x5201, CXLMI_RET_BUSY);
		sent = commands_sent(ep);
		rc = fmd_vppb_bind(ctx, sw, 2, 1, 4, 0xffff);
		CHECK(rc == -EAGAIN, "bind on a busy switch: %d", rc);
		/* the VCSs are read, then Bind vPPB is sent once */
		once = commands_sent(ep) - sent;

		/*
		 * Retry Required: "The command was not completed because of a
		 * temporary error. An optional single retry may resolve the
		 * issue." (Table 8-208). It is sent once more; a second Retry
		 * Required is a temporary condition, as Busy is, not a failure.
		 */
		answer_rc(ep, 0x5201, CXLMI_RET_RETRY);
		answer_rc(ep, 0x5201, CXLMI_RET_RETRY);
		sent = commands_sent(ep);
		rc = fmd_vppb_bind(ctx, sw, 2, 1, 4, 0xffff);
		CHECK(rc == -EAGAIN, "bind answered Retry Required twice: %d", rc);
		/* Bind vPPB is sent twice, and not a third time */
		CHECK(commands_sent(ep) - sent == once + 1 &&
		      last_opcode(ep) == 0x5201, "%u commands sent, %u for Busy",
		      commands_sent(ep) - sent, once);

		answer_rc(ep, 0x5201, CXLMI_RET_RETRY);
		answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
		rc = fmd_vppb_bind(ctx, sw, 2, 1, 4, 0xffff);
		CHECK(rc == FMD_OP_STARTED, "bind retried after Retry Required: %d",
		      rc);

		answer_rc(ep, 0x5202, CXLMI_RET_RETRY);
		answer_rc(ep, 0x5202, CXLMI_RET_RETRY);
		rc = fmd_vppb_unbind(ctx, sw, 0, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
		CHECK(rc == -EAGAIN, "unbind answered Retry Required twice: %d", rc);
	}

	fmd_ctx_free(ctx);
	report("bind on a busy switch, or one that asks for a retry", before);
}

/*
 * Re-scan: what has appeared since start-up is added, and nothing is
 * removed. "When a device is Hot-Added to an unbound port on a switch ...
 * FM issues the Get Physical Port State command for the physical port that
 * has linked up to discover the connected device type" (Section 7.6.6.7).
 */
static void test_rescan(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *sw_ep = new_switch(ctx, 0x500a), *late, *slow;
	struct fmd_fabric *root = ctx->topology.root;
	int before = failures, rc, ports, devices;
	unsigned int removed;
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16];
	struct fmd_port *port;

	/* an endpoint that is not ready when cxl-fmd starts */
	slow = new_device(ctx);
	answer_rc(slow, 0x0001, CXLMI_RET_RETRY);

	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x500a);
	ports = root->port_count;
	devices = root->device_count;
	port = fmd_topology_find_port(&ctx->topology, sw_id, 4);
	CHECK(port && port->link_status == FMD_LINK_DOWN && !port->base.first_child,
	      "port 4 is not empty to begin with");

	/* nothing has changed: nothing is added, and nothing twice */
	answer_rc(slow, 0x0001, CXLMI_RET_RETRY);
	answer_switch_ports(sw_ep, 6, 0x00, 0x00);
	rc = fmd_rescan(ctx);
	CHECK(rc == 0, "%d components added by a re-scan of an unchanged fabric", rc);
	CHECK(root->port_count == ports && root->switch_count == 1 &&
	      root->host_count == 2, "%d ports, %d switches, %d hosts",
	      root->port_count, root->switch_count, root->host_count);

	/* an SLD is plugged into port 4: Type 3 SLD (04h), LTSSM L0 (04h) */
	answer_rc(slow, 0x0001, CXLMI_RET_RETRY);
	answer_switch_ports(sw_ep, 6, 0x04, 0x04);
	rc = fmd_rescan(ctx);
	CHECK(rc == 1, "%d components added when an SLD appears on a port", rc);
	snprintf(id, sizeof(id), "%.32s_port4_dev", sw_id);
	CHECK(fmd_topology_find_sld(&ctx->topology, id), "no SLD %s", id);
	if (port) {
		CHECK(port->link_status == FMD_LINK_UP &&
		      port->conn_dev_type == FMD_CONN_SLD,
		      "port 4: link %d, device type %d", port->link_status,
		      port->conn_dev_type);
		CHECK(port->base.first_child && !port->base.first_child->next_sibling,
		      "port 4 does not have exactly one device");
	}
	CHECK(root->port_count == ports, "%d ports after a re-scan", root->port_count);

	/*
	 * and is pulled out: the port reports no device (Connected Device
	 * Type 00h), and the device is removed
	 */
	answer_rc(slow, 0x0001, CXLMI_RET_RETRY);
	answer_switch_ports(sw_ep, 6, 0x00, 0x00);
	removed = ctx->topology.removed;
	rc = fmd_rescan(ctx);
	CHECK(rc == 0, "%d components added when a device is removed", rc);
	CHECK(!fmd_topology_find_sld(&ctx->topology, id),
	      "a device that is gone was kept");
	CHECK(ctx->topology.removed == removed + 1, "%u components removed",
	      ctx->topology.removed - removed);
	CHECK(root->device_count == devices, "%d devices, %d to begin with",
	      root->device_count, devices);
	if (port)
		CHECK(port->link_status == FMD_LINK_DOWN &&
		      port->conn_dev_type == FMD_CONN_NONE,
		      "port 4 after removal: link %d, device type %d",
		      port->link_status, port->conn_dev_type);

	/* an endpoint appears, and the one that was not ready now is */
	late = new_device(ctx);
	answer_identify(late, 0x1003, 0x03);
	answer_memdev_identify(late, 64, 64, 0);
	answer_identify(slow, 0x1004, 0x03);
	answer_memdev_identify(slow, 32, 32, 0);
	answer_switch_ports(sw_ep, 6, 0x00, 0x00);
	rc = fmd_rescan(ctx);
	CHECK(rc == 2, "%d components added for two new endpoints", rc);
	device_id(id, sizeof(id), 0x1003);
	CHECK(fmd_topology_find_sld(&ctx->topology, id), "no SLD %s", id);
	device_id(id, sizeof(id), 0x1004);
	CHECK(fmd_topology_find_sld(&ctx->topology, id),
	      "an endpoint that was not ready was not asked again");
	CHECK(root->device_count == devices + 2, "%d devices, %d to begin with",
	      root->device_count, devices);

	/* and once known, an endpoint is not discovered again */
	answer_switch_ports(sw_ep, 6, 0x00, 0x00);
	rc = fmd_rescan(ctx);
	CHECK(rc == 0 && root->device_count == devices + 2,
	      "%d components added by a second look at the same endpoints", rc);

	fmd_ctx_free(ctx);
	report("re-scan", before);
}

/*
 * The event logs of a switch (Section 7.6.8). A page of Get Event Records
 * (Table 8-232) with one record in the Common Event Record Format (Table
 * 8-221): the identifier at 00h, the handle at 14h, and from 30h on what
 * the kind of record defines.
 */
static const uint8_t physical_switch_event[16] = {	/* 77cf9271-9c02-470b-... */
	0x77, 0xcf, 0x92, 0x71, 0x9c, 0x02, 0x47, 0x0b,
	0x9f, 0xe4, 0xbc, 0x7b, 0x75, 0xf2, 0xda, 0x97,
};
static const uint8_t virtual_switch_event[16] = {	/* 40d26425-3396-4c4d-... */
	0x40, 0xd2, 0x64, 0x25, 0x33, 0x96, 0x4c, 0x4d,
	0xa5, 0xda, 0x3d, 0x47, 0x26, 0x3a, 0xf4, 0x25,
};

static void answer_event(struct cxlmi_endpoint *ep, const uint8_t *uuid,
			 uint16_t handle, const uint8_t *data, size_t len)
{
	uint8_t page[0x20 + 0x80] = { 0 }, *rec = page + 0x20;

	put16(page + 0x14, 1);			/* Event Record Count */
	memcpy(rec, uuid, 16);			/* Event Record Identifier */
	rec[0x10] = 0x80;			/* Event Record Length */
	put16(rec + 0x14, handle);		/* Event Record Handle */
	memcpy(rec + 0x30, data, len);
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
}

/* a log with nothing in it, or one that has lost records */
static void answer_no_events(struct cxlmi_endpoint *ep, bool overflow)
{
	uint8_t page[0x20] = { overflow ? 0x01 : 0x00 };

	answer(ep, 0x0100, page, sizeof(page));
}

static uint16_t last_policy_set(struct cxlmi_endpoint *ep)
{
	uint8_t pl[2] = { 0 }, set = 0, cmd = 0;
	size_t size = sizeof(pl);

	cxlmi_mock_get_last_command(ep, &set, &cmd, pl, &size);
	return set == 0x01 && cmd == 0x05 ? (uint16_t)(pl[0] | pl[1] << 8) : 0xffff;
}

static void test_switch_events(void)
{
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x500c);
	struct fmd_fabric *root = ctx->topology.root;
	uint8_t ports[4 + 16] = { 0 }, pl[8] = { 0 }, set = 0, cmd = 0;
	size_t size = sizeof(pl);
	int before = failures, nports, rc;
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16], cid[FMD_CONN_ID_LEN];
	struct fmd_bind_key key;
	struct fmd_switch *sw;
	struct fmd_task *task;
	struct fmd_port *port;

	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x500c);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	CHECK(sw, "no switch %s", sw_id);
	if (!sw)
		goto out;
	nports = root->port_count;

	/*
	 * Set MCTP Event Interrupt Policy (0105h), Table 8-236: the four logs
	 * (Bits[3:0]) and a background operation that has completed (Bit[15]).
	 */
	answer(ep, 0x0104, (uint8_t[2]){ 0, 0 }, 2);
	answer_rc(ep, 0x0105, CXLMI_RET_SUCCESS);
	fmd_switch_events_enable(ctx, sw);
	cxlmi_mock_get_last_command(ep, &set, &cmd, pl, &size);
	CHECK(set == 0x01 && cmd == 0x05 && pl[0] == 0x0f && pl[1] == 0x80,
	      "event notifications asked for with %02x%02xh, %02x%02xh", set,
	      cmd, pl[1], pl[0]);

	/*
	 * Set even when the bits are set already, by another requester
	 * maybe: "The subsequent enabled events shall be sent to that
	 * address" (Section 8.2.10.2.7). Reserved Bit[5] is cleared.
	 */
	answer(ep, 0x0104, (uint8_t[2]){ 0x2f, 0x80 }, 2);
	answer_rc(ep, 0x0105, CXLMI_RET_SUCCESS);
	fmd_switch_events_enable(ctx, sw);
	CHECK(last_policy_set(ep) == 0x800f,
	      "a switch policy already set: %04x set", last_policy_set(ep));

	/* a Set reported as failed is read back */
	answer(ep, 0x0104, (uint8_t[2]){ 0x00, 0x00 }, 2);
	answer_rc(ep, 0x0105, CXLMI_RET_INTERNAL);
	answer(ep, 0x0104, (uint8_t[2]){ 0x0f, 0x80 }, 2);
	fmd_switch_events_enable(ctx, sw);
	CHECK(last_opcode(ep) == 0x0104, "the switch policy was not read back");

	/*
	 * A Physical Switch Event Record (Table 7-79) for port 4, in the
	 * Informational log: a device has been plugged in. Only that port is
	 * read again: Type 3 SLD (04h), LTSSM L0 (04h).
	 */
	answer_event(ep, physical_switch_event, 0x11,
		     (uint8_t[2]){ 4, 0x00 }, 2);	/* port 4, Link State Change */
	ports[0] = 1;
	port_block(ports + 4, 4, 0x03, 0x02, 0x04, 0x04, 0x0004);
	answer(ep, 0x5101, ports, sizeof(ports));
	fmd_switch_events_poll(ctx);

	port = fmd_topology_find_port(&ctx->topology, sw_id, 4);
	snprintf(id, sizeof(id), "%.32s_port4_dev", sw_id);
	CHECK(fmd_topology_find_sld(&ctx->topology, id),
	      "the device on the port the event named was not discovered");
	CHECK(port && port->link_status == FMD_LINK_UP, "port 4 link");
	CHECK(root->port_count == nports, "%d ports", root->port_count);

	/*
	 * A bind in the background, and the Virtual CXL Switch Event Record
	 * (Table 7-80) that says it is over: a Binding Change of vPPB 1 of
	 * VCS 2, in the Informational log, which is where a successful one
	 * goes.
	 */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_bind(ctx, sw, 2, 1, 5, 1);
	CHECK(rc == FMD_OP_STARTED, "bind: %d", rc);
	key = bind_key(sw_id, 2, 1, 5, 1);
	fmd_conn_id_format(&key, cid, sizeof(cid));
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, &key, 0);
	CHECK(task, "no task");
	if (!task)
		goto out;

	answer_event(ep, virtual_switch_event, 0x12,
		     (uint8_t[6]){ 2, 1, 0x00, 0x03, 5, 1 }, 6);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld1, unbound);
	fmd_switch_events_poll(ctx);
	CHECK(task->state == FMD_TASK_COMPLETED, "task state %d: %s",
	      task->state, task->message);
	CHECK(conn_state(ctx, cid) >= 0 && conn_state(ctx, cid) != FMD_CONN_STARTING &&
	      !ops_count(ctx) && fmd_binding_mark_find(ctx, &key),
	      "the connection is not made by the binding change: state %d, %d ops",
	      conn_state(ctx, cid), ops_count(ctx));
	/* both records are kept: the switch no longer has them */
	CHECK(sw->base.log && sw->base.log->count == 2,
	      "%d records kept of the switch", sw->base.log ? sw->base.log->count : 0);

	/*
	 * An unbind that fails: its Binding Change is in the Warning log. The
	 * Informational log is read first, and is empty.
	 */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_unbind(ctx, sw, 2, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == FMD_OP_STARTED, "unbind: %d", rc);
	task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw, &key, 1);
	CHECK(task, "no task");
	if (!task)
		goto out;

	answer_no_events(ep, false);
	answer_event(ep, virtual_switch_event, 0x13,
		     (uint8_t[6]){ 2, 1, 0x00, 0x03, 5, 1 }, 6);
	fmd_switch_events_poll(ctx);
	CHECK(task->state == FMD_TASK_EXCEPTION, "task state %d: %s",
	      task->state, task->message);
	/* the record has no return code, and Background Operation Status none */
	CHECK(!strcmp(task->message, "Unbind vPPB failed: the switch logged it "
		      "as failed"), "task message: %s", task->message);
	CHECK(conn_state(ctx, cid) >= 0 && conn_state(ctx, cid) != FMD_CONN_UPDATING &&
	      !ops_count(ctx),
	      "the connection does not stand after an unbind that failed: %d",
	      conn_state(ctx, cid));

	/*
	 * Another that fails, and Background Operation Status (Table 8-217)
	 * has its return code: 100% complete, Unbind vPPB, Invalid Input.
	 */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_unbind(ctx, sw, 2, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == FMD_OP_STARTED, "unbind: %d", rc);
	task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw, &key, 1);
	CHECK(task, "no task");
	if (!task)
		goto out;

	answer_no_events(ep, false);
	answer_event(ep, virtual_switch_event, 0x14,
		     (uint8_t[6]){ 2, 1, 0x00, 0x03, 5, 1 }, 6);
	answer(ep, 0x0002, (uint8_t[8]){ 100 << 1, 0, 0x02, 0x52,
					 CXLMI_RET_INPUT, 0 }, 8);
	fmd_switch_events_poll(ctx);
	CHECK(task->state == FMD_TASK_EXCEPTION, "task state %d: %s",
	      task->state, task->message);
	CHECK(strstr(task->message, cxlmi_cmd_retcode_tostr(CXLMI_RET_INPUT)) &&
	      !strstr(task->message, cxlmi_cmd_retcode_tostr(CXLMI_RET_INTERNAL)),
	      "task message: %s", task->message);

	/*
	 * A log that has overflowed has lost records, and what they said is
	 * only in the switch: all of it is read again. Port 4 has no device
	 * now.
	 */
	answer_no_events(ep, true);
	answer_switch_ports(ep, 6, 0x00, 0x00);
	fmd_switch_events_poll(ctx);
	CHECK(port && port->link_status == FMD_LINK_DOWN &&
	      port->conn_dev_type == FMD_CONN_NONE,
	      "the switch was not read again after an overflow");
out:
	fmd_ctx_free(ctx);
	report("event logs of a switch", before);
}

/*
 * The event logs of a component reached through the kernel's mailbox are
 * shared with the kernel's driver (Section 9.20.1): they are not drained.
 */
/*
 * Records are kept before they are cleared: the component has no other
 * copy afterwards. Those of a device go to the LD they are about.
 */
static const uint8_t general_media_event[16] = {	/* fbcd0a77-c260-... */
	0xfb, 0xcd, 0x0a, 0x77, 0xc2, 0x60, 0x41, 0x7f,
	0x85, 0xa9, 0x08, 0x8b, 0x16, 0x21, 0xeb, 0xa6,
};
static const uint8_t unknown_event[16] = { 0x12, 0x34 };

/* a page with one record, with Event Record Flags and an LD-ID */
/* the page alone; its Clear Event Records is answered with @clear_rc */
static void answer_device_page(struct cxlmi_endpoint *ep, const uint8_t *uuid,
			       uint16_t handle, uint8_t flags0, uint16_t ld_id,
			       uint8_t page_flags, uint16_t clear_rc)
{
	uint8_t page[0x20 + 0x80] = { 0 }, *rec = page + 0x20;

	page[0x00] = page_flags;
	if (page_flags & 0x01) {			/* Overflow */
		put16(page + 0x02, 7);			/* Overflow Error Count */
		put64(page + 0x04, 1000000000ULL);	/* First Overflow Event */
		put64(page + 0x0c, 2000000000ULL);	/* Last Overflow Event */
	}
	put16(page + 0x14, 1);
	memcpy(rec, uuid, 16);
	rec[0x10] = 0x80;
	rec[0x11] = flags0;				/* Event Record Flags */
	put16(rec + 0x14, handle);
	put16(rec + 0x22, ld_id);			/* LD-ID */
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, clear_rc);
}

static void answer_device_event(struct cxlmi_endpoint *ep, const uint8_t *uuid,
				uint16_t handle, uint8_t flags0, uint16_t ld_id,
				uint8_t page_flags)
{
	answer_device_page(ep, uuid, handle, flags0, ld_id, page_flags,
			   CXLMI_RET_SUCCESS);
}

static int log_count(struct fmd_component *comp)
{
	return comp->log ? comp->log->count : 0;
}

static struct fmd_log_entry *log_newest(struct fmd_component *comp)
{
	struct fmd_log *log = comp->log;

	return log && log->count ?
	       &log->entries[(log->first + log->count - 1) % log->allocated] : NULL;
}

static void test_device_logs(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *sld_ep = new_device(ctx), *mld_ep;
	int before = failures;
	char id[FMD_MAX_ID_LEN];
	struct fmd_sld *sld;
	struct fmd_mld *mld;
	struct fmd_log_entry *e;

	answer_identify(sld_ep, 0xe1, 0x03);
	answer_memdev_identify(sld_ep, 64, 64, 0);
	mld_ep = new_mld(ctx, 0xe2, 2, 0, 4);
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xe1);
	sld = fmd_topology_find_sld(&ctx->topology, id);
	device_id(id, sizeof(id), 0xe2);
	mld = fmd_topology_find_mld(&ctx->topology, id);
	CHECK(sld && mld, "no SLD or MLD");
	if (!sld || !mld)
		goto out;

	/*
	 * The SLD: a General Media record and one of a kind that is not
	 * decoded in the Informational log; one in the Warning log, which
	 * has overflowed; the others empty. The MLD's logs are empty.
	 */
	answer_device_event(sld_ep, general_media_event, 1, 0x00, 0, 0x02);
	answer_device_event(sld_ep, unknown_event, 2, 0x00, 0, 0x00);
	answer_no_events(sld_ep, false);	/* read again: empty */
	answer_device_event(sld_ep, general_media_event, 3, 0x01, 0, 0x01);
	answer_no_events(sld_ep, false);
	answer_no_events(sld_ep, false);
	answer_no_events(sld_ep, false);
	for (int log = 0; log < 4; log++)
		answer_no_events(mld_ep, false);

	fmd_switch_events_poll(ctx);
	CHECK(last_opcode(sld_ep) == 0x0100, "the SLD's logs were not read");
	CHECK(log_count(&sld->base) == 3, "%d records kept of the SLD",
	      log_count(&sld->base));
	e = log_newest(&sld->base);
	CHECK(e && e->log == 1 && e->overflow && e->overflow_count == 7 &&
	      e->first_overflow_ns == 1000000000ULL &&
	      e->last_overflow_ns == 2000000000ULL,
	      "the overflow is not on the last record of the Warning log");
	CHECK(sld->base.log && sld->base.log->overflow, "the log service did not overflow");
	if (sld->base.log) {
		struct fmd_log_entry *first = &sld->base.log->entries[sld->base.log->first];

		CHECK(!first->overflow, "an earlier record carries the overflow");
		CHECK(!memcmp(sld->base.log->entries[(sld->base.log->first + 1) %
			      sld->base.log->allocated].record.uuid, unknown_event, 16),
		      "a record of a kind that is not decoded was not kept");
	}

	/*
	 * The MLD: a record for LD 1 (LD-ID Valid Flag, Event Record Flags
	 * Bit[7]), and one about the whole device.
	 */
	answer_no_events(sld_ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(sld_ep, false);
	answer_device_event(mld_ep, general_media_event, 4, 0x80, 1, 0x02);
	answer_device_event(mld_ep, general_media_event, 5, 0x00, 0, 0x00);
	answer_no_events(mld_ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(mld_ep, false);
	fmd_switch_events_poll(ctx);

	{
		struct fmd_ld *ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
		struct fmd_ld *ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);

		CHECK(ld0 && ld1, "no LDs");
		if (ld0 && ld1) {
			CHECK(log_count(&ld1->base) == 2, "LD 1 has %d records",
			      log_count(&ld1->base));
			CHECK(log_count(&ld0->base) == 1, "LD 0 has %d records",
			      log_count(&ld0->base));
		}
		CHECK(log_count(&mld->base) == 0, "the MLD itself has records");
	}
out:
	fmd_ctx_free(ctx);
	report("event logs of memory devices", before);
}

/* the record kept in @comp's log with @handle */
static struct fmd_log_entry *log_handle(struct fmd_component *comp, uint16_t handle)
{
	struct fmd_log *log = comp->log;

	for (int i = 0; log && i < log->count; i++) {
		struct fmd_log_entry *e = &log->entries[(log->first + i) % log->allocated];

		if (e->record.handle == handle)
			return e;
	}
	return NULL;
}

/*
 * The overflow is on a record stored before it. The Overflow flag "shall
 * remain set until the host has consumed one or more events" (Table
 * 8-232): a record read after the first page that said Overflow was cleared
 * may have been stored after it, in the room that clearing made. The last
 * record of that page carries it. An LD of an MLD that the record was not
 * about has none tagged. An Overflow Error Count of 0 is no count, and
 * OverflowErrorCount is left out.
 */
static void test_overflow_entry(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *sld_ep = new_device(ctx), *mld_ep;
	int before = failures;
	char sld_id[FMD_MAX_ID_LEN], mld_id[FMD_MAX_ID_LEN];
	struct fmd_component *sld;
	struct fmd_ld *ld0, *ld1;
	struct fmd_log_entry *a, *b;
	json_t *entry;

	answer_identify(sld_ep, 0xe7, 0x03);
	answer_memdev_identify(sld_ep, 64, 64, 0);
	mld_ep = new_mld(ctx, 0xe8, 2, 0, 4);
	fmd_discover(ctx);
	device_id(sld_id, sizeof(sld_id), 0xe7);
	sld = fmd_topology_find(&ctx->topology, sld_id);
	device_id(mld_id, sizeof(mld_id), 0xe8);
	ld0 = fmd_topology_find_ld(&ctx->topology, mld_id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, mld_id, 1);
	CHECK(sld && ld0 && ld1, "devices not discovered");
	if (!sld || !ld0 || !ld1)
		goto out;

	/*
	 * The SLD's Warning log: a page that says Overflow, with record 0x21,
	 * then one that does not, with 0x22, then empty.
	 */
	answer_no_events(sld_ep, false);
	answer_device_event(sld_ep, general_media_event, 0x21, 0x01, 0, 0x03);
	answer_device_event(sld_ep, general_media_event, 0x22, 0x01, 0, 0x00);
	answer_no_events(sld_ep, false);
	answer_no_events(sld_ep, false);
	answer_no_events(sld_ep, false);
	/* the MLD's Informational log: Overflow, on a record about LD 1 */
	answer_device_event(mld_ep, general_media_event, 0x31, 0x80, 1, 0x01);
	answer_no_events(mld_ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(mld_ep, false);
	fmd_switch_events_poll(ctx);

	a = log_handle(sld, 0x21);
	b = log_handle(sld, 0x22);
	CHECK(a && b, "records not kept");
	CHECK(a && a->overflow && a->overflow_count == 7,
	      "the record of the page that said Overflow does not carry it");
	CHECK(b && !b->overflow, "a record read after the overflow carries it");

	a = log_handle(&ld1->base, 0x31);
	CHECK(a && a->overflow, "LD 1's record does not carry the overflow");
	CHECK(ld0->base.log && ld0->base.log->overflow,
	      "LD 0's log service does not say Overflow");

	/* no count: OverflowErrorCount left out, the timestamps given */
	if (b) {
		b->overflow = true;
		b->overflow_count = 0;
		b->first_overflow_ns = 1000000000ULL;
		entry = fmd_redfish_ld_log(ctx, sld_id, sld_id, "0", "entry", b->id);
		CHECK(entry && !json_object_get(entry, "OverflowErrorCount") &&
		      json_object_get(entry, "FirstOverflowTimestamp"),
		      "an Overflow Error Count of 0 is given as a count");
		json_decref(entry);
	}
	if (a) {
		entry = fmd_redfish_ld_log(ctx, mld_id, mld_id, "1", "entry", a->id);
		CHECK(entry && json_integer_value(json_object_get(entry,
			"OverflowErrorCount")) == 7, "a count of 7 is not given");
		json_decref(entry);
	}
out:
	fmd_ctx_free(ctx);
	report("overflow on the record before it", before);
}

/*
 * A log keeps as many records as its capacity, the newest; the memory for
 * them grows as they come. And a switch-attached SLD's logs, which cannot
 * be reached, are not said to be read.
 */
static void test_log_ring(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x500e);
	struct cxlmi_event_record rec = { 0 };
	int before = failures;
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16];
	struct fmd_component *sld;
	struct fmd_switch *sw;

	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x500e);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	CHECK(sw, "no switch");
	if (!sw)
		goto out;

	for (int i = 1; i <= 300; i++) {
		rec.handle = (uint16_t)i;
		fmd_log_add(&sw->base, 0, &rec);
		if (i == 20)
			CHECK(sw->base.log->allocated < sw->base.log->capacity,
			      "%d entries allocated for 20 records",
			      sw->base.log->allocated);
	}
	CHECK(sw->base.log->count == sw->base.log->capacity &&
	      sw->base.log->allocated == sw->base.log->capacity,
	      "%d records, %d allocated, capacity %d", sw->base.log->count,
	      sw->base.log->allocated, sw->base.log->capacity);
	/* the oldest went: the first kept is 300 - capacity + 1 */
	CHECK(sw->base.log->entries[sw->base.log->first].record.handle ==
	      300 - sw->base.log->capacity + 1, "oldest kept is %d",
	      sw->base.log->entries[sw->base.log->first].record.handle);

	/* an SLD on port 4 */
	answer_switch_ports(ep, 6, 0x04, 0x04);
	fmd_rescan(ctx);
	snprintf(id, sizeof(id), "%.32s_port4_dev", sw_id);
	sld = fmd_topology_find(&ctx->topology, id);
	CHECK(sld && !fmd_device_logs_read(ctx, sld),
	      "a switch-attached SLD's logs are said to be read");
out:
	fmd_ctx_free(ctx);
	report("records kept, and their memory", before);
}

/* Get Physical Port State (5101h) of one port */
static void answer_one_port(struct cxlmi_endpoint *ep, uint8_t id,
			    uint8_t type, uint8_t ltssm)
{
	uint8_t ports[4 + 16] = { 1 };

	port_block(ports + 4, id, 0x03, type ? 0x02 : 0, type, ltssm,
		   type ? 0x0004 : 0x0000);
	answer(ep, 0x5101, ports, sizeof(ports));
}

/* the host on VCS @vcs_id of a switch */
static struct fmd_host *vcs_host(struct fmd_ctx *ctx, const char *sw_id, int vcs_id)
{
	struct fmd_component *comp;

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST) {
		struct fmd_host *host = fmd_to_host(comp);

		if (!strcmp(host->switch_id, sw_id) && host->vcs_id == vcs_id)
			return host;
	}
	return NULL;
}

/* a switch event for @port, which then reads as @type */
static void port_event(struct fmd_ctx *ctx, struct cxlmi_endpoint *ep,
		       uint16_t handle, uint8_t port, uint8_t type, uint8_t ltssm,
		       const uint8_t *vcs2_vppb1)
{
	answer_event(ep, physical_switch_event, handle, (uint8_t[2]){ port, 0x00 }, 2);
	answer_one_port(ep, port, type, ltssm);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, vcs2_vppb1, unbound);
	fmd_switch_events_poll(ctx);
}

/* GET of a connection: its Status.State, and whether it links to its device */
static const char *connection_state(struct fmd_ctx *ctx, const char *id,
				    bool *has_target, char *buf, size_t len)
{
	json_t *c = fmd_redfish_connection(ctx, "CXL", id);
	const char *state;

	if (!c)
		return NULL;
	state = json_string_value(json_object_get(json_object_get(c, "Status"), "State"));
	snprintf(buf, len, "%s", state ? state : "");
	*has_target = json_object_get(json_object_get(c, "Links"), "TargetEndpoints") &&
		      json_object_get(c, "MemoryChunkInfo");
	json_decref(c);
	return buf;
}

/*
 * A device that is removed from a switch port (CXL r4.0 Sections 7.6.6.7
 * and 7.6.6.8): the switch records it, the port is read again, and what
 * the device was goes. Its connections are the switch's to decide: one
 * whose vPPB the switch unbinds goes, and one whose vPPB stays bound stands,
 * absent, until it is deleted.
 */
static void test_device_removal(void)
{
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	static const uint8_t port4[3] = { 0x02, 4, 0xff };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x500f);
	struct fmd_fabric *root = ctx->topology.root;
	char sw_id[FMD_MAX_ID_LEN], mld_id[FMD_MAX_ID_LEN + 16];
	char sld_id[FMD_MAX_ID_LEN + 16], a_id[FMD_CONN_ID_LEN], b_id[FMD_CONN_ID_LEN];
	char c_id[FMD_CONN_ID_LEN], state[32];
	int before = failures, devices, lds, domains, chunks, rc;
	struct fmd_bind_key key;
	struct fmd_component *dev;
	struct fmd_host *host;
	struct fmd_switch *sw;
	struct fmd_task *task;
	struct fmd_vcs *vcs;
	struct fmd_port *port;
	struct fmd_vppb *vppb;
	json_t *resp = NULL;
	bool linked;

	/* the MLD on port 5 has 2 LDs of 4 x 256 MiB (Get LD Info, Allocations) */
	answer_ld_info(ep, 2ULL * 4 << 28, 2);
	answer_ld_allocations(ep, 2, 0, 4);

	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x500f);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	host = vcs_host(ctx, sw_id, 2);
	snprintf(mld_id, sizeof(mld_id), "%.32s_port5_dev", sw_id);
	snprintf(sld_id, sizeof(sld_id), "%.32s_port4_dev", sw_id);
	CHECK(sw && host && fmd_topology_find_mld(&ctx->topology, mld_id),
	      "no switch, host of VCS 2 or MLD %s", mld_id);
	if (!sw || !host || !fmd_topology_find_mld(&ctx->topology, mld_id))
		goto out;
	CHECK(fmd_topology_find_ld(&ctx->topology, mld_id, 1), "no LD 1");

	/* LD 1 of it is bound to vPPB 1 of VCS 2, for a connection */
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld1, unbound);
	fmd_vcs_refresh(ctx, sw);
	vcs = fmd_switch_find_vcs(sw, 2);
	vppb = vcs ? fmd_vcs_find_vppb(vcs, 1) : NULL;
	CHECK(vppb && vppb->ld, "the vPPB does not point at LD 1");
	key = bind_key(sw_id, 2, 1, 5, 1);
	fmd_conn_id_format(&key, a_id, sizeof(a_id));
	CHECK(conn_state(ctx, a_id) == FMD_CONN_ENABLED, "Connection %s: %d",
	      a_id, conn_state(ctx, a_id));

	/* and LD 0, which the switch is still binding to vPPB 2, with its task */
	key = bind_key(sw_id, 2, 2, 5, 0);
	fmd_conn_id_format(&key, b_id, sizeof(b_id));
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, &key, 0);
	CHECK(task, "no task");
	if (!task)
		goto out;
	CHECK(conn_state(ctx, b_id) == FMD_CONN_STARTING, "Connection %s: %d",
	      b_id, conn_state(ctx, b_id));

	devices = root->device_count;
	lds = root->ld_count;
	domains = root->domain_count;
	chunks = root->chunk_count;

	/*
	 * The MLD is pulled out: a Physical Switch Event Record for port 5,
	 * which then reports no device (00h). The VCSs cannot be read after
	 * it, so their tables are kept as they were: the vPPB that pointed at
	 * LD 1 must not still point at it, and the connection, whose vPPB the
	 * switch has not said is unbound, stands.
	 */
	answer_event(ep, physical_switch_event, 0x31, (uint8_t[2]){ 5, 0x00 }, 2);
	answer_one_port(ep, 5, 0x00, 0x00);
	answer_rc(ep, 0x5200, CXLMI_RET_INTERNAL);
	fmd_switch_events_poll(ctx);

	CHECK(!fmd_topology_find(&ctx->topology, mld_id), "the MLD is still there");
	CHECK(!fmd_topology_find_ld(&ctx->topology, mld_id, 1), "LD 1 is still there");
	CHECK(root->device_count == devices - 1, "%d devices, %d before",
	      root->device_count, devices);
	CHECK(root->ld_count < lds && root->domain_count < domains &&
	      root->chunk_count < chunks, "LDs %d/%d, domains %d/%d, chunks %d/%d",
	      root->ld_count, lds, root->domain_count, domains,
	      root->chunk_count, chunks);
	vcs = fmd_switch_find_vcs(sw, 2);
	vppb = vcs ? fmd_vcs_find_vppb(vcs, 1) : NULL;
	CHECK(vppb && !vppb->ld, "the vPPB still points at an LD that was removed");
	CHECK(connection_state(ctx, a_id, &linked, state, sizeof(state)) &&
	      !strcmp(state, "Absent") && !linked,
	      "connection to the removed MLD: %s, %slinked to it",
	      state, linked ? "" : "not ");
	port = fmd_topology_find_port(&ctx->topology, sw_id, 5);
	CHECK(port && !port->base.first_child && port->conn_dev_type == FMD_CONN_NONE,
	      "port 5 is not empty");

	/*
	 * "The switch will automatically unbind any existing LD bindings":
	 * read again, VCS 2 says so, and the connection is gone with them.
	 * The one being made is its task's to settle.
	 */
	port_event(ctx, ep, 0x32, 5, 0x00, 0x00, unbound);
	CHECK(conn_state(ctx, a_id) < 0,
	      "a connection whose vPPB the switch unbound is still there");
	CHECK(conn_state(ctx, b_id) == FMD_CONN_STARTING &&
	      task->state == FMD_TASK_RUNNING,
	      "a connection being bound was not left to its task");
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_task_binding_changed(ctx, sw, 2, 2, true);
	CHECK(task->state == FMD_TASK_EXCEPTION && conn_state(ctx, b_id) < 0 &&
	      !ops_count(ctx), "task %d: %s", task->state, task->message);

	/* an SLD is plugged into port 4, and bound to vPPB 1 of VCS 2 */
	port_event(ctx, ep, 0x33, 4, 0x04, 0x04, port4);
	CHECK(fmd_topology_find_sld(&ctx->topology, sld_id), "no SLD on port 4");
	key = bind_key(sw_id, 2, 1, 4, FMD_LD_NONE);
	fmd_conn_id_format(&key, c_id, sizeof(c_id));
	CHECK(conn_state(ctx, c_id) == FMD_CONN_ENABLED, "Connection %s: %d",
	      c_id, conn_state(ctx, c_id));

	/*
	 * Its link goes down with the device still detected, a surprise Link
	 * Down: "the presence pin ... will remain asserted". It is kept.
	 */
	port_event(ctx, ep, 0x34, 4, 0x04, 0x00, port4);
	port = fmd_topology_find_port(&ctx->topology, sw_id, 4);
	CHECK(fmd_topology_find_sld(&ctx->topology, sld_id) && port &&
	      port->link_status == FMD_LINK_DOWN,
	      "a device whose link went down was removed");

	/*
	 * Pulled out of the bound port: "the FM can be notified but is not
	 * involved", and the vPPB stays bound to the port. So does the
	 * connection, absent.
	 */
	port_event(ctx, ep, 0x35, 4, 0x00, 0x00, port4);
	CHECK(!fmd_topology_find(&ctx->topology, sld_id), "the SLD is still there");
	CHECK(connection_state(ctx, c_id, &linked, state, sizeof(state)) &&
	      !strcmp(state, "Absent") && !linked,
	      "connection to the removed SLD: %s", state);

	/*
	 * Deleting it unbinds the vPPB, on the switch found through its host:
	 * the switch takes the Unbind vPPB (5202h) into the background, and
	 * the connection stands until it is done.
	 */
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, port4, unbound);
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_redfish_delete_connection(ctx, "CXL", c_id, &resp);
	json_decref(resp);
	/* started: what only the answer to 5202h says */
	CHECK(rc == FMD_OP_STARTED, "delete: %d", rc);
	CHECK(conn_state(ctx, c_id) == FMD_CONN_UPDATING,
	      "the connection does not stand while it is unbound: %d",
	      conn_state(ctx, c_id));
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_task_binding_changed(ctx, sw, 2, 1, false);
	CHECK(conn_state(ctx, c_id) < 0 && !ops_count(ctx),
	      "the connection is still there");

	/* plugged back in: found again */
	port_event(ctx, ep, 0x36, 4, 0x04, 0x04, unbound);
	dev = fmd_topology_find(&ctx->topology, sld_id);
	CHECK(dev && dev->type == FMD_COMPONENT_SLD, "the SLD was not found again");
	CHECK(root->device_count == devices, "%d devices, %d before the MLD went",
	      root->device_count, devices);
out:
	fmd_ctx_free(ctx);
	report("devices removed from a switch", before);
}

/*
 * MCTP Event Notifications from a memory device (Set MCTP Event Interrupt
 * Policy, Table 8-236): Bits[3:0] for the four logs cxl-fmd reads, Bit[4]
 * for a DCD's Dynamic Capacity Event Log, read-modify-write, with the
 * reserved Bits[14:5] cleared. Only a device on an endpoint of its own.
 */

static void test_device_notifications(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *sld_ep = new_device(ctx), *dcd_ep = new_device(ctx);
	struct cxlmi_endpoint *sw_ep = new_switch(ctx, 0x5012);
	uint8_t info[0x54] = { 0 };
	int before = failures;
	char id[FMD_MAX_ID_LEN + 16], sw_id[FMD_MAX_ID_LEN];
	struct fmd_component *sld, *dcd, *mld;
	unsigned int sent;

	answer_identify(sld_ep, 0xe5, 0x03);
	answer_memdev_identify(sld_ep, 64, 64, 0);
	answer_identify(dcd_ep, 0xe6, 0x03);
	answer_memdev_identify(dcd_ep, 64, 64, 0);
	info[0x00] = 1;
	info[0x01] = 1;
	put16(info + 0x04, 0x07);
	put16(info + 0x08, 0x03);
	put64(info + 0x0c, 4);
	put64(info + 0x14, 1ULL << 28);
	answer(dcd_ep, 0x5600, info, sizeof(info));
	answer_ld_info(sw_ep, 2ULL * 4 << 28, 2);
	answer_ld_allocations(sw_ep, 2, 0, 4);
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0xe5);
	sld = fmd_topology_find(&ctx->topology, id);
	device_id(id, sizeof(id), 0xe6);
	dcd = fmd_topology_find(&ctx->topology, id);
	device_id(sw_id, sizeof(sw_id), 0x5012);
	snprintf(id, sizeof(id), "%.32s_port5_dev", sw_id);
	mld = fmd_topology_find(&ctx->topology, id);
	CHECK(sld && dcd && fmd_memdev_dcd(dcd) && mld, "devices not discovered");
	if (!sld || !dcd || !mld)
		goto out;

	/* Bit[15] already set stays; reserved Bit[5] is cleared */
	answer(sld_ep, 0x0104, (uint8_t[2]){ 0x20, 0x80 }, 2);
	answer_rc(sld_ep, 0x0105, CXLMI_RET_SUCCESS);
	fmd_device_events_enable(ctx, sld);
	CHECK(last_policy_set(sld_ep) == 0x800f, "SLD policy set to %04x",
	      last_policy_set(sld_ep));

	/* a DCD: and its Dynamic Capacity Event Log */
	answer(dcd_ep, 0x0104, (uint8_t[2]){ 0x00, 0x00 }, 2);
	answer_rc(dcd_ep, 0x0105, CXLMI_RET_SUCCESS);
	fmd_device_events_enable(ctx, dcd);
	CHECK(last_policy_set(dcd_ep) == 0x001f, "DCD policy set to %04x",
	      last_policy_set(dcd_ep));

	/*
	 * Already set, by another requester maybe: set again, so that the
	 * device sends cxl-fmd the notifications (Section 8.2.10.2.7).
	 */
	answer(dcd_ep, 0x0104, (uint8_t[2]){ 0x1f, 0x00 }, 2);
	answer_rc(dcd_ep, 0x0105, CXLMI_RET_SUCCESS);
	fmd_device_events_enable(ctx, dcd);
	CHECK(last_policy_set(dcd_ep) == 0x001f,
	      "a policy already set was not set again: %04x",
	      last_policy_set(dcd_ep));

	/*
	 * A Set reported as failed, the policy read back set: a libcxlmi
	 * that does not take Set's response payload says that.
	 */
	answer(sld_ep, 0x0104, (uint8_t[2]){ 0x00, 0x00 }, 2);
	answer_rc(sld_ep, 0x0105, CXLMI_RET_INTERNAL);
	answer(sld_ep, 0x0104, (uint8_t[2]){ 0x0f, 0x00 }, 2);
	fmd_device_events_enable(ctx, sld);
	CHECK(last_opcode(sld_ep) == 0x0104, "the policy was not read back");

	/* an MLD behind a switch has no way to notify the FM: not asked */
	sent = commands_sent(sw_ep);
	fmd_device_events_enable(ctx, mld);
	CHECK(commands_sent(sw_ep) == sent,
	      "%u commands sent to enable notifications through a tunnel",
	      commands_sent(sw_ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("MCTP event notifications from memory devices", before);
}

/*
 * The logs of an MLD behind a switch port, read through the switch: its
 * records go to its LDs. The mock answers a tunnelled command as any
 * other, and records the tunnel it was sent through: the MLD's are sent
 * to its port (Tunnel Management Command, 5300h), the switch's to none.
 */
static void test_switch_mld_logs(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5010);
	struct fmd_ctx *ctx2 = new_ctx();
	struct cxlmi_endpoint *ep2 = new_switch(ctx2, 0x5011);
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16];
	struct fmd_component *mld, *sw;
	struct fmd_ld *ld0, *ld1;
	int before = failures;
	unsigned int sent;

	answer_ld_info(ep, 2ULL * 4 << 28, 2);
	answer_ld_allocations(ep, 2, 0, 4);
	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x5010);
	snprintf(id, sizeof(id), "%.32s_port5_dev", sw_id);
	sw = fmd_topology_find(&ctx->topology, sw_id);
	mld = fmd_topology_find(&ctx->topology, id);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(sw && mld && ld0 && ld1, "no MLD with two LDs on port 5");
	if (!sw || !mld || !ld0 || !ld1)
		goto out;
	CHECK(mld->ep == ep && fmd_to_mld(mld)->switch_port == 5 &&
	      fmd_device_logs_read(ctx, mld), "the MLD's logs are not read");

	/* the switch's logs are empty */
	for (int log = 0; log < 4; log++)
		answer_no_events(ep, false);
	/* the MLD's: a record for LD 1, more to come; one for the device */
	answer_device_event(ep, general_media_event, 0x41, 0x80, 1, 0x02);
	answer_device_event(ep, general_media_event, 0x42, 0x00, 0, 0x00);
	answer_no_events(ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(ep, false);

	sent = commands_sent(ep);
	fmd_switch_events_poll(ctx);
	/* 4 for the switch; 3 Gets and 2 Clears for log 0; 3 Gets for 1-3 */
	CHECK(commands_sent(ep) - sent == 12, "%u commands", commands_sent(ep) - sent);
	CHECK(log_count(sw) == 0 && log_count(mld) == 0 &&
	      log_count(&ld1->base) == 2 && log_count(&ld0->base) == 1,
	      "records: switch %d, MLD %d, LD 0 %d, LD 1 %d", log_count(sw),
	      log_count(mld), log_count(&ld0->base), log_count(&ld1->base));
	{
		struct cxlmi_tunnel_info ti;

		/* the last command was the MLD's: through the tunnel to port 5 */
		cxlmi_mock_get_last_tunnel(ep, &ti);
		CHECK(last_opcode(ep) == 0x0100 && ti.level == 1 && ti.port == 5,
		      "the MLD's log was read through tunnel level %d, port %d",
		      ti.level, ti.port);
	}

	/*
	 * An MLD whose LDs could not be read has nothing to keep its records
	 * on: they would be cleared from it and lost. Its logs are not read.
	 */
	fmd_discover(ctx2);
	device_id(sw_id, sizeof(sw_id), 0x5011);
	snprintf(id, sizeof(id), "%.32s_port5_dev", sw_id);
	mld = fmd_topology_find(&ctx2->topology, id);
	CHECK(mld && !mld->first_child, "no MLD without LDs");
	if (mld)
		CHECK(!fmd_device_logs_read(ctx2, mld),
		      "the logs of an MLD with no LDs are read");
	for (int log = 0; log < 4; log++)
		answer_no_events(ep2, false);
	answer_device_event(ep2, general_media_event, 0x51, 0x00, 0, 0x00);
	sent = commands_sent(ep2);
	fmd_switch_events_poll(ctx2);
	CHECK(commands_sent(ep2) - sent == 4,
	      "%u commands: the MLD's records were read", commands_sent(ep2) - sent);
	{
		struct cxlmi_tunnel_info ti;

		/* so the last command was the switch's own: through none */
		cxlmi_mock_get_last_tunnel(ep2, &ti);
		CHECK(ti.level == 0, "the switch's log was read through a tunnel "
		      "of level %d", ti.level);
	}
out:
	fmd_ctx_free(ctx);
	fmd_ctx_free(ctx2);
	report("event logs of an MLD behind a switch", before);
}

/*
 * The Dynamic Capacity Event Log of a DCD: Add Capacity Response (04h) and
 * Capacity Released (05h) name the host, by its LD-ID (Table 8-229 Host
 * ID), and are kept in that LD's log; the host's events (00h-03h) are not
 * the FM's.
 */
static const uint8_t dc_event[16] = {	/* ca95afa7-f183-4018-... */
	0xca, 0x95, 0xaf, 0xa7, 0xf1, 0x83, 0x40, 0x18,
	0x8c, 0x2f, 0x95, 0x26, 0x8e, 0x10, 0x1a, 0x2a,
};

static void test_dc_records(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld(ctx, 0xe7, 2, 0, 4);
	uint8_t info[0x54] = { 0 }, page[0x20 + 0x80 * 3] = { 0 };
	char id[FMD_MAX_ID_LEN];
	struct fmd_ld *ld0, *ld1;
	struct fmd_log_entry *e;
	int before = failures;

	info[0x00] = 2;				/* Number of Hosts */
	info[0x01] = 1;
	put16(info + 0x04, 0x07);
	put16(info + 0x08, 0x03);
	put64(info + 0x0c, 4);
	put64(info + 0x14, 1ULL << 28);
	answer(ep, 0x5600, info, sizeof(info));
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xe7);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(ld0 && ld1, "no DCD MLD with two LDs");
	if (!ld0 || !ld1)
		goto out;

	/*
	 * One page, overflowed: Capacity Released by host 1, Add Capacity
	 * Response by host 0, and an Add Capacity (00h), which is the host's.
	 */
	page[0x00] = 0x01;			/* Overflow */
	put16(page + 0x02, 5);			/* Overflow Error Count */
	put16(page + 0x14, 3);
	for (int i = 0; i < 3; i++) {
		uint8_t *rec = page + 0x20 + i * 0x80;
		static const uint8_t types[3] = { 0x05, 0x04, 0x00 };
		static const uint8_t hosts[3] = { 1, 0, 1 };

		memcpy(rec, dc_event, 16);
		rec[0x10] = 0x80;
		put16(rec + 0x14, 0x61 + i);
		rec[0x30] = types[i];		/* Dynamic Capacity Event Type */
		put16(rec + 0x32, hosts[i]);	/* Host ID */
	}
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
	answer_no_events(ep, false);
	fmd_dcd_poll_events(ctx);

	CHECK(log_count(&ld1->base) == 1 && log_count(&ld0->base) == 1,
	      "LD 0 has %d records, LD 1 %d", log_count(&ld0->base),
	      log_count(&ld1->base));
	e = log_newest(&ld1->base);
	CHECK(e && e->log == FMD_LOG_DYNAMIC_CAPACITY && e->record.data[0] == 0x05,
	      "LD 1's record is not the Capacity Released");
	CHECK(e && e->overflow && e->overflow_count == 5,
	      "the overflow is not on the last record of the log");
	e = log_newest(&ld0->base);
	CHECK(e && e->log == FMD_LOG_DYNAMIC_CAPACITY && e->record.data[0] == 0x04,
	      "LD 0's record is not the Add Capacity Response");
	if (e) {
		json_t *entry = NULL;
		char ld[8];

		snprintf(ld, sizeof(ld), "0");
		entry = fmd_redfish_ld_log(ctx, id, id, ld, "entry", e->id);
		CHECK(entry && !strcmp(json_string_value(json_object_get(entry,
		      "CXLEntryType")) ?: "", "DynamicCapacity") &&
		      !strcmp(json_string_value(json_object_get(entry,
		      "Severity")) ?: "", "OK"), "the entry is not a DynamicCapacity one");
		json_decref(entry);
	}
out:
	fmd_ctx_free(ctx);
	report("Dynamic Capacity records", before);
}

/*
 * A record whose Clear Event Records failed is still in the device, and is
 * read again: it is kept once.
 */
static void test_record_read_again(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char id[FMD_MAX_ID_LEN];
	struct fmd_component *sld;
	int before = failures;

	answer_identify(ep, 0xe8, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xe8);
	sld = fmd_topology_find(&ctx->topology, id);
	CHECK(sld, "no SLD");
	if (!sld)
		goto out;

	/* read, and its Clear fails: the drain of that log stops */
	answer_device_page(ep, general_media_event, 9, 0x00, 0, 0x00,
			   CXLMI_RET_INTERNAL);
	for (int log = 1; log < 4; log++)
		answer_no_events(ep, false);
	fmd_switch_events_poll(ctx);

	/* read again, cleared this time, then the log is empty */
	answer_device_event(ep, general_media_event, 9, 0x00, 0, 0x00);
	answer_no_events(ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(ep, false);
	fmd_switch_events_poll(ctx);

	CHECK(log_count(sld) == 1, "a record read twice is kept %d times",
	      log_count(sld));
out:
	fmd_ctx_free(ctx);
	report("a record read again", before);
}

/*
 * A host's Add Capacity Response, cleared, then a Get Event Records that
 * fails: the host is still refreshed, as nothing will name it again. The
 * refresh starts with Get Host DC Region Configuration (5601h); that it
 * fails does not stop Dynamic Capacity List Tags (5608h).
 */
static void test_dc_refresh_after_failed_read(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld(ctx, 0xe9, 2, 0, 4);
	uint8_t info[0x54] = { 0 }, page[0x20 + 0x80] = { 0 };
	unsigned int sent;
	int before = failures;

	info[0x00] = 2;
	info[0x01] = 1;
	put16(info + 0x04, 0x07);
	put16(info + 0x08, 0x03);
	put64(info + 0x0c, 4);
	put64(info + 0x14, 1ULL << 28);
	answer(ep, 0x5600, info, sizeof(info));
	fmd_discover(ctx);

	put16(page + 0x14, 1);
	memcpy(page + 0x20, dc_event, 16);
	page[0x20 + 0x10] = 0x80;
	put16(page + 0x20 + 0x14, 0x71);
	page[0x20 + 0x30] = 0x04;		/* Add Capacity Response */
	put16(page + 0x20 + 0x32, 1);		/* by host 1 */
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
	answer_rc(ep, 0x0100, CXLMI_RET_INTERNAL);
	sent = commands_sent(ep);
	fmd_dcd_poll_events(ctx);
	/* read, clear, read; then 5601h, which fails, and 5608h */
	CHECK(last_opcode(ep) == 0x5608 && commands_sent(ep) - sent == 5,
	      "host 1 was not refreshed: last command %04xh, %u commands",
	      last_opcode(ep), commands_sent(ep) - sent);

	fmd_ctx_free(ctx);
	report("a DC refresh after a failed read", before);
}

/* One Dynamic Capacity Event Record (Table 8-229) of @type by @host */
static void put_dc_record(uint8_t *rec, uint16_t handle, uint8_t type, uint16_t host,
			  uint8_t validity, uint32_t extents, uint32_t tags)
{
	memcpy(rec, dc_event, 16);
	rec[0x10] = 0x80;			/* Event Record Length */
	put16(rec + 0x14, handle);		/* Event Record Handle */
	rec[0x30] = type;			/* Dynamic Capacity Event Type */
	rec[0x31] = validity;			/* Validity Flags */
	put16(rec + 0x32, host);		/* Host ID */
	put16(rec + 0x78, extents);		/* Number of Available Extents */
	put16(rec + 0x7c, tags);		/* Number of Available Tags */
}

/*
 * The Host ID of an Add Capacity Response is "the LD-ID of the host
 * interface that triggered the generation of the event" (Table 8-229), and
 * the 5608h bitmaps go up to LD 255. A record of host 200 is not dropped:
 * the device has no LD 200 here, so no host is read, but the tag list is.
 * read, clear, read, 5608h.
 */
static void test_dc_records_ld200(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xdf, 2);
	uint8_t page[0x20 + 0x80] = { 0 };
	unsigned int sent;
	int before = failures;

	if (!dev)
		goto out;

	put16(page + 0x14, 1);			/* Event Record Count */
	put_dc_record(page + 0x20, 0x81, 0x04, 200, 0, 0, 0);
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
	answer_no_events(ep, false);
	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	sent = commands_sent(ep);
	fmd_dcd_poll_events(ctx);
	CHECK(last_opcode(ep) == 0x5608 && commands_sent(ep) - sent == 4,
	      "a record of host 200: last command %04xh, %u commands",
	      last_opcode(ep), commands_sent(ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("a DC record of LD 200", before);
}

/*
 * Number of Available Extents (78h) and Number of Available Tags (7Ch) of
 * an Add Capacity Response are those of the host, valid when Bit[0] of the
 * Validity Flags is set (Table 8-229). Host 1's record sets its counters;
 * host 1 alone is read again, and its 5601h fails, so the counters are the
 * record's: read, clear, read, 5601h, 5608h. A record whose Validity Flags
 * are clear leaves them.
 */
static void test_dc_records_counts(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xe0, 2);
	uint8_t page[0x20 + 0x80] = { 0 };
	char id[FMD_MAX_ID_LEN];
	struct fmd_ld *ld0, *ld1;
	unsigned int sent;
	int before = failures;

	if (!dev)
		goto out;
	device_id(id, sizeof(id), 0xe0);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	if (!ld0 || !ld1)
		goto out;
	ld0->dc_extents_available = ld1->dc_extents_available = 100;
	ld0->dc_tags_available = ld1->dc_tags_available = 50;

	put16(page + 0x14, 1);
	put_dc_record(page + 0x20, 0x82, 0x04, 1, 0x01, 17, 5);
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
	answer_no_events(ep, false);
	answer_rc(ep, 0x5601, CXLMI_RET_INTERNAL);
	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	sent = commands_sent(ep);
	fmd_dcd_poll_events(ctx);
	CHECK(ld1->dc_extents_available == 17 && ld1->dc_tags_available == 5,
	      "host 1: %u extents, %u tags available, not 17 and 5",
	      ld1->dc_extents_available, ld1->dc_tags_available);
	CHECK(ld0->dc_extents_available == 100 && ld0->dc_tags_available == 50,
	      "host 0's counters changed");
	CHECK(last_opcode(ep) == 0x5608 && commands_sent(ep) - sent == 5,
	      "not host 1 alone: last command %04xh, %u commands",
	      last_opcode(ep), commands_sent(ep) - sent);

	put_dc_record(page + 0x20, 0x83, 0x05, 1, 0x00, 99, 99);
	answer(ep, 0x0100, page, sizeof(page));
	answer_rc(ep, 0x0101, CXLMI_RET_SUCCESS);
	answer_no_events(ep, false);
	answer_rc(ep, 0x5601, CXLMI_RET_INTERNAL);
	answer_rc(ep, 0x5608, CXLMI_RET_UNSUPPORTED);
	fmd_dcd_poll_events(ctx);
	CHECK(ld1->dc_extents_available == 17 && ld1->dc_tags_available == 5,
	      "counters a record does not say are valid were taken: %u, %u",
	      ld1->dc_extents_available, ld1->dc_tags_available);
out:
	fmd_ctx_free(ctx);
	report("counters of a DC record", before);
}

/*
 * A device found on a re-scan with records in its logs already: those send
 * no notification until they have been emptied, so the re-scan reads them.
 */
static void test_rescan_reads_new_logs(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	char id[FMD_MAX_ID_LEN];
	struct fmd_component *sld;
	int before = failures;

	fmd_discover(ctx);
	ctx->config.event_poll_sec = 0;		/* no timer: notifications only */
	ctx->config.rescan_sec = 30;
	ctx->next_rescan_ms = 1;		/* due */

	ep = new_device(ctx);
	answer_identify(ep, 0xea, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	answer_device_event(ep, general_media_event, 0x81, 0x00, 0, 0x00);
	answer_no_events(ep, false);
	for (int log = 1; log < 4; log++)
		answer_no_events(ep, false);
	fmd_events_poll(ctx, 0);

	device_id(id, sizeof(id), 0xea);
	sld = fmd_topology_find(&ctx->topology, id);
	CHECK(sld && log_count(sld) == 1,
	      "the logs of a device found on a re-scan were not read");

	fmd_ctx_free(ctx);
	report("a re-scan reads the logs of what it finds", before);
}

/*
 * An Event Notification says only which logs have records; who sent it is
 * the MCTP address it came from. The logs read are those of the component
 * on that endpoint, and every component's for an address that is none of
 * cxl-fmd's endpoints.
 */
static void test_notification_route(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *sw = new_switch(ctx, 0x5013);
	struct cxlmi_endpoint *sld = new_device(ctx), *dcd = new_device(ctx);
	uint8_t info[0x54] = { 0 };
	unsigned int s0, d0, c0;
	int before = failures;

	answer_identify(sld, 0xeb, 0x03);
	answer_memdev_identify(sld, 64, 64, 0);
	answer_identify(dcd, 0xec, 0x03);
	answer_memdev_identify(dcd, 64, 64, 0);
	info[0x00] = 1;
	info[0x01] = 1;
	put16(info + 0x04, 0x07);
	put16(info + 0x08, 0x03);
	put64(info + 0x0c, 4);
	put64(info + 0x14, 1ULL << 28);
	answer(dcd, 0x5600, info, sizeof(info));
	fmd_discover(ctx);

	cxlmi_mock_set_mctp_addr(sw, 1, 10);
	cxlmi_mock_set_mctp_addr(sld, 1, 11);
	cxlmi_mock_set_mctp_addr(dcd, 1, 12);

#define SENT() do { s0 = commands_sent(sw); d0 = commands_sent(sld); \
		     c0 = commands_sent(dcd); } while (0)
#define READ(ep, n0) (commands_sent(ep) != (n0))

	/* from the SLD: its logs, no one else's */
	SENT();
	fmd_events_route(ctx, 1, 11, 0x0001);
	CHECK(READ(sld, d0) && !READ(sw, s0) && !READ(dcd, c0),
	      "from the SLD: SLD %d, switch %d, DCD %d", READ(sld, d0),
	      READ(sw, s0), READ(dcd, c0));

	/* from the switch: its logs alone */
	SENT();
	fmd_events_route(ctx, 1, 10, 0x0002);
	CHECK(READ(sw, s0) && !READ(sld, d0) && !READ(dcd, c0),
	      "from the switch: switch %d, SLD %d, DCD %d", READ(sw, s0),
	      READ(sld, d0), READ(dcd, c0));

	/*
	 * The Dynamic Capacity Event Log of the DCD: its alone. Its drain
	 * fails, the log unanswered: that is not a reason to read anyone's
	 * again, its own included.
	 */
	SENT();
	fmd_events_route(ctx, 1, 12, 0x0010);
	CHECK(READ(dcd, c0) && !READ(sw, s0) && !READ(sld, d0),
	      "DC log from the DCD: DCD %d, switch %d, SLD %d", READ(dcd, c0),
	      READ(sw, s0), READ(sld, d0));
	CHECK(commands_sent(dcd) - c0 == 1,
	      "%u commands to a DCD whose drain failed: it was read again",
	      commands_sent(dcd) - c0);

	/* the same address on another network is none of ours: everyone's */
	SENT();
	fmd_events_route(ctx, 2, 11, 0x0001);
	CHECK(READ(sw, s0) && READ(sld, d0) && READ(dcd, c0),
	      "from no endpoint of ours: switch %d, SLD %d, DCD %d",
	      READ(sw, s0), READ(sld, d0), READ(dcd, c0));

	/*
	 * An endpoint opened on MCTP_NET_ANY reports network 0, and its
	 * notifications come from whichever network the kernel chose.
	 */
	cxlmi_mock_set_mctp_addr(sld, 0, 11);
	SENT();
	fmd_events_route(ctx, 5, 11, 0x0001);
	CHECK(READ(sld, d0) && !READ(sw, s0) && !READ(dcd, c0),
	      "from an endpoint on any network: SLD %d, switch %d, DCD %d",
	      READ(sld, d0), READ(sw, s0), READ(dcd, c0));
	cxlmi_mock_set_mctp_addr(sld, 1, 11);

	/* the DC log from a device that is no DCD: every DCD's */
	SENT();
	fmd_events_route(ctx, 1, 11, 0x0010);
	CHECK(READ(dcd, c0), "a DC log notification read no DC log");
#undef SENT
#undef READ

	fmd_ctx_free(ctx);
	report("event notifications routed to their sender", before);
}

/*
 * An Event Notification is a request, received with its Tag Owner bit set
 * (MCTP_TAG_OWNER in smctp_tag). Its acknowledgement is a response to it:
 * to its sender, with that bit clear and the device's tag, or the kernel
 * sends a new request with a tag of its own.
 */
static void test_notification_ack(void)
{
	struct sockaddr_mctp from = {
		.smctp_family = AF_MCTP,
		.smctp_network = 3,
		.smctp_addr.s_addr = 42,
		.smctp_type = 0x08,
		.smctp_tag = MCTP_TAG_OWNER | 5,
	}, to;
	int before = failures;

	memset(&to, 0xff, sizeof(to));
	fmd_events_ack_addr(&from, &to);
	CHECK(to.smctp_tag == 5, "acknowledged with tag %#x, not as a response "
	      "with tag 5", to.smctp_tag);
	CHECK(to.smctp_family == AF_MCTP && to.smctp_network == 3 &&
	      to.smctp_addr.s_addr == 42 && to.smctp_type == 0x08,
	      "acknowledged to network %u EID %u type %#x", to.smctp_network,
	      to.smctp_addr.s_addr, to.smctp_type);

	report("event notifications acknowledged as responses", before);
}

/*
 * Deleting a Volatile chunk of an SLD moves its partition, which a device
 * whose Partition Alignment is 0 cannot do (Table 8-309): that is the
 * request's fault, 400, as it is for a POST, and the chunk stays.
 */
static void test_chunk_delete_partition_refused(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], chunk[FMD_MAX_ID_LEN + 16];
	int before = failures, rc;

	answer_identify(ep, 0xed, 0x03);
	answer_memdev_identify(ep, 64, 64, 0);
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xed);
	snprintf(dom, sizeof(dom), "%.50s_domain0", id);
	snprintf(chunk, sizeof(chunk), "%.50s_chunk0", id);
	CHECK(fmd_topology_find_chunk(&ctx->topology, chunk), "no chunk %s", chunk);

	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == -4, "delete: %d, not a refused partition", rc);
	CHECK(fmd_topology_find_chunk(&ctx->topology, chunk),
	      "the chunk went although its partition could not");

	fmd_ctx_free(ctx);
	report("a chunk whose partition cannot be moved", before);
}

static void test_mailbox_events_are_the_hosts(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x500d);
	char sw_id[FMD_MAX_ID_LEN];
	struct fmd_switch *sw;
	int before = failures;
	unsigned int sent;

	fmd_discover(ctx);

	/* as if the scan of /dev/cxl had opened it */
	ctx->ioctl_eps = calloc(1, sizeof(*ctx->ioctl_eps));
	if (ctx->ioctl_eps)
		ctx->ioctl_eps[ctx->nioctl_eps++] = ep;
	CHECK(!fmd_endpoint_owns_events(ctx, ep), "a mailbox endpoint owns its events");

	/* nor are its notifications asked for: they are the host's too */
	device_id(sw_id, sizeof(sw_id), 0x500d);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	CHECK(sw, "no switch %s", sw_id);
	sent = commands_sent(ep);
	if (sw)
		fmd_switch_events_enable(ctx, sw);
	CHECK(commands_sent(ep) == sent,
	      "%u commands sent to ask for a mailbox switch's notifications",
	      commands_sent(ep) - sent);

	sent = commands_sent(ep);
	answer_event(ep, physical_switch_event, 0x21, (uint8_t[2]){ 4, 0 }, 2);
	fmd_switch_events_poll(ctx);
	CHECK(commands_sent(ep) == sent,
	      "%u commands sent to a switch whose logs are the host's",
	      commands_sent(ep) - sent);

	fmd_ctx_free(ctx);
	report("event logs behind the kernel's mailbox", before);
}

static void test_not_ready(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	int before = failures, rc;

	answer_rc(ep, 0x0001, CXLMI_RET_RETRY);

	rc = fmd_discover(ctx);
	CHECK(rc == 0, "fmd_discover: %d", rc);
	CHECK(ctx->topology.root->device_count == 0 &&
	      ctx->topology.root->switch_count == 0, "something was added");
	CHECK(commands_sent(ep) == 1, "%u commands sent", commands_sent(ep));

	fmd_ctx_free(ctx);
	report("device not ready", before);
}

/*
 * Get Virtual CXL Switch Info, Table 7-34. USP ID: "Physical Port ID of the
 * current VCS's Upstream Port, or the current VCS's fabric physical Port ID
 * of a Downstream ES VCS. Valid only when the VCS is enabled." A Downstream
 * ES VCS on a Fabric Port, and a disabled VCS whose USP ID is that of
 * another VCS, are both valid.
 */
static struct fmd_port *add_port(struct fmd_ctx *ctx, struct fmd_switch *sw,
				 uint8_t number, enum fmd_port_state state)
{
	char id[FMD_MAX_ID_LEN];
	struct fmd_port *port;

	snprintf(id, sizeof(id), "%.40s_port%d", sw->base.id, number);
	port = fmd_port_create(id, number);
	if (!port) {
		printf("FAIL  cannot create a port\n");
		exit(1);
	}
	port->state = state;
	fmd_topology_add(&ctx->topology, &port->base, &sw->base);
	return port;
}

static void test_validate_usp_id(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct fmd_switch *sw = fmd_switch_create("sw_validate");
	int before = failures, invalid;

	if (!sw) {
		printf("FAIL  cannot create a switch\n");
		exit(1);
	}
	fmd_topology_add(&ctx->topology, &sw->base, NULL);
	add_port(ctx, sw, 0, FMD_PORT_USP);
	add_port(ctx, sw, 1, FMD_PORT_FABRIC);

	sw->num_vcs = 3;
	sw->vcs[0].vcs_id = 0;			/* on the USP */
	sw->vcs[0].state = 0x01;
	sw->vcs[0].upstream_port_id = 0;
	sw->vcs[1].vcs_id = 1;			/* a Downstream ES VCS */
	sw->vcs[1].state = 0x01;
	sw->vcs[1].upstream_port_id = 1;
	sw->vcs[2].vcs_id = 2;			/* disabled: USP ID not valid */
	sw->vcs[2].state = 0x00;
	sw->vcs[2].upstream_port_id = 0;

	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 0, "%d rule(s) broken", invalid);

	/* enabled, the third VCS is on the first one's USP */
	sw->vcs[2].state = 0x01;
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 1, "%d rule(s) broken by two VCSs on one USP", invalid);

	fmd_ctx_free(ctx);
	report("USP ID of a Downstream ES VCS and of a disabled VCS", before);
}

/* fmd_topology_validate(), with what it logs at warning level and above in @buf */
static int validate_logged(struct fmd_ctx *ctx, char *buf, size_t len)
{
	enum fmd_log_level level = ctx->log_level;
	FILE *tmp = tmpfile();
	int saved, invalid;
	size_t n;

	if (!tmp) {
		printf("FAIL  cannot make a temporary file\n");
		exit(1);
	}
	fflush(stderr);
	saved = dup(STDERR_FILENO);
	dup2(fileno(tmp), STDERR_FILENO);
	ctx->log_level = FMD_LOG_WARN;
	invalid = fmd_topology_validate(ctx);
	ctx->log_level = level;
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	rewind(tmp);
	n = fread(buf, 1, len - 1, tmp);
	buf[n] = '\0';
	fclose(tmp);
	return invalid;
}

/*
 * The validator's rules on bindings a real switch may report, which the mock
 * loader cannot load: a vPPB bound to an LD of a head of a Multi-Headed
 * device that presents an MLD is valid, and warned of: "The MH-MLD maps the
 * LD-IDs received at a head to the device-wide LD index in the MH-MLD's LD
 * pool" (CXL r4.0 Section 2.5.1) by no rule the FM is told, so its
 * Connection is unresolved. vPPB 0 of an enabled VCS bound to anything but
 * its USP is refused (vPPB Ordering, Section 7.1.4, as cxl-fmd reads it).
 */
static void test_validate_bindings(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct fmd_switch *sw = fmd_switch_create("sw_vb");
	struct fmd_port *p4;
	struct fmd_mhd *mhd;
	struct fmd_head *h0, *h1;
	struct fmd_ld *ld;
	char id[FMD_MAX_ID_LEN], log[8192];
	int before = failures, invalid;

	fmd_topology_add(&ctx->topology, &sw->base, NULL);
	add_port(ctx, sw, 0, FMD_PORT_USP);
	p4 = add_port(ctx, sw, 4, FMD_PORT_DSP);
	p4->conn_dev_type = FMD_CONN_MLD;
	p4->supported_ld_count = 1;

	mhd = fmd_mhd_create("mhd_vb");
	mhd->num_heads = 2;
	mhd->total_lds = 3;
	fmd_topology_add(&ctx->topology, &mhd->base, NULL);
	h0 = fmd_head_create("mhd_vb_head0", 0);
	h0->presentation = FMD_HEAD_MLD;
	h0->port = p4;
	p4->head = h0;
	h1 = fmd_head_create("mhd_vb_head1", 1);
	h1->presentation = FMD_HEAD_SLD;
	snprintf(h1->attached_host_id, sizeof(h1->attached_host_id), "elsewhere");
	fmd_topology_add(&ctx->topology, &h0->base, &mhd->base);
	fmd_topology_add(&ctx->topology, &h1->base, &mhd->base);
	for (int i = 0; i < 3; i++) {
		snprintf(id, sizeof(id), "mhd_vb_ld%d", i);
		ld = fmd_ld_create(id, i);
		ld->binding_point_id = i < 2 ? 0 : 1;
		fmd_topology_add(&ctx->topology, &ld->base, &mhd->base);
	}

	sw->num_vcs = 1;
	sw->vcs[0].vcs_id = 0;
	sw->vcs[0].state = 0x01;
	sw->vcs[0].upstream_port_id = 0;
	sw->vcs[0].num_vppbs = 2;
	sw->vcs[0].vppbs[0] = (struct fmd_vppb){ .vppb_id = 0,
		.binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 0, .bound_ld_id = 0xff };
	sw->vcs[0].vppbs[1] = (struct fmd_vppb){ .vppb_id = 1,
		.binding_status = FMD_VPPB_BOUND_LD,
		.bound_port_id = 4, .bound_ld_id = 1 };

	invalid = validate_logged(ctx, log, sizeof(log));
	CHECK(invalid == 0 && strstr(log, "doubtful topology: sw_vb: vPPB 1 of VCS 0 "
				     "is bound to LD 1 of head 0 of mhd_vb, which "
				     "presents an MLD"),
	      "an LD of a head that presents an MLD: %d rule(s) broken\n%s",
	      invalid, log);

	/* vPPB 0 is the USP vPPB: bound to the LD instead, it is refused */
	sw->vcs[0].vppbs[0] = sw->vcs[0].vppbs[1];
	sw->vcs[0].vppbs[0].vppb_id = 0;
	sw->vcs[0].vppbs[1] = (struct fmd_vppb){ .vppb_id = 1,
		.binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 0, .bound_ld_id = 0xff };
	invalid = validate_logged(ctx, log, sizeof(log));
	CHECK(invalid == 1 && strstr(log, "vPPB 0 of VCS 0, the USP vPPB, is bound "
				     "to port 4 by LD, not to its USP 0"),
	      "vPPB 0 bound to an LD: %d rule(s) broken\n%s", invalid, log);

	fmd_ctx_free(ctx);
	report("validator rules on a head that presents an MLD and on vPPB 0", before);
}

/*
 * Bound LD ID: "Valid only when vPPB[Start vPPB] Binding Status is 03h;
 * otherwise, the value is FFh" (Table 7-34). A switch that puts something
 * else there for another status is read as FFh, and so are the vPPBs the
 * mock binds and unbinds; the validator refuses any other.
 */
static void test_bound_ld_sentinel(void)
{
	static const uint8_t port2_junk[3] = { 0x02, 2, 0x07 };
	static const uint8_t unbound_zero[3] = { 0x00, 0x00, 0x00 };
	static const uint8_t busy[3] = { 0x01, 4, 0x00 };
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	struct fmd_switch *sw, *msw;
	struct fmd_vcs *vcs;
	char id[FMD_MAX_ID_LEN];
	int before = failures, rc, invalid;

	answer_identify(ep, 0x5100, 0x00);
	answer_switch_ports(ep, 6, 0x00, 0x00);
	answer_vcs(ep, 0, 0, port2_junk, unbound_zero);
	answer_vcs(ep, 2, 1, busy, ld1);
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0x5100);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	vcs = fmd_switch_find_vcs(sw, 0);
	CHECK(vcs && vcs->num_vppbs == 3, "VCS 0");
	if (vcs) {
		CHECK(vcs->vppbs[0].bound_ld_id == 0xff,
		      "USP vPPB (02h): Bound LD ID %d", vcs->vppbs[0].bound_ld_id);
		CHECK(vcs->vppbs[1].binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT &&
		      vcs->vppbs[1].bound_port_id == 2 &&
		      vcs->vppbs[1].bound_ld_id == 0xff,
		      "02h with 07h in Bound LD ID: port %d LD %d",
		      vcs->vppbs[1].bound_port_id, vcs->vppbs[1].bound_ld_id);
		CHECK(vcs->vppbs[2].binding_status == FMD_VPPB_UNBOUND &&
		      vcs->vppbs[2].bound_ld_id == 0xff,
		      "00h with 00h in Bound LD ID: LD %d",
		      vcs->vppbs[2].bound_ld_id);
	}
	vcs = fmd_switch_find_vcs(sw, 2);
	CHECK(vcs && vcs->num_vppbs == 3, "VCS 2");
	if (vcs) {
		/* Bound Port ID is valid only for 02h and 03h */
		CHECK(vcs->vppbs[1].binding_status == FMD_VPPB_BUSY &&
		      vcs->vppbs[1].bound_port_id == 0 &&
		      vcs->vppbs[1].bound_ld_id == 0xff,
		      "01h: port %d LD %d", vcs->vppbs[1].bound_port_id,
		      vcs->vppbs[1].bound_ld_id);
		CHECK(vcs->vppbs[2].binding_status == FMD_VPPB_BOUND_LD &&
		      vcs->vppbs[2].bound_port_id == 5 &&
		      vcs->vppbs[2].bound_ld_id == 1,
		      "03h: port %d LD %d", vcs->vppbs[2].bound_port_id,
		      vcs->vppbs[2].bound_ld_id);
	}

	/* a mock switch: what it binds as a whole and unbinds has LD FFh */
	fmd_ctx_free(ctx);
	ctx = new_ctx();
	msw = fmd_switch_create("sw_sentinel");
	if (!msw) {
		printf("FAIL  cannot create a switch\n");
		exit(1);
	}
	fmd_topology_add(&ctx->topology, &msw->base, NULL);
	add_port(ctx, msw, 0, FMD_PORT_USP);
	add_port(ctx, msw, 4, FMD_PORT_DSP);
	msw->num_vcs = 1;
	msw->vcs[0].vcs_id = 0;
	msw->vcs[0].state = 0x01;
	msw->vcs[0].upstream_port_id = 0;
	msw->vcs[0].num_vppbs = 2;
	msw->vcs[0].vppbs[0] = (struct fmd_vppb){
		.vppb_id = 0, .binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 0, .bound_ld_id = 0xff };
	msw->vcs[0].vppbs[1] = (struct fmd_vppb){
		.vppb_id = 1, .binding_status = FMD_VPPB_UNBOUND,
		.bound_ld_id = 0xff };

	rc = fmd_vppb_bind(ctx, msw, 0, 1, 4, 0xffff);
	CHECK(rc == 0 && msw->vcs[0].vppbs[1].binding_status ==
	      FMD_VPPB_BOUND_PHYSICAL_PORT &&
	      msw->vcs[0].vppbs[1].bound_ld_id == 0xff,
	      "mock bind of a whole port: %d, LD %d", rc,
	      msw->vcs[0].vppbs[1].bound_ld_id);
	rc = fmd_vppb_unbind(ctx, msw, 0, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == 0 && msw->vcs[0].vppbs[1].binding_status == FMD_VPPB_UNBOUND &&
	      msw->vcs[0].vppbs[1].bound_ld_id == 0xff,
	      "mock unbind: %d, LD %d", rc, msw->vcs[0].vppbs[1].bound_ld_id);

	/* in the background, too */
	msw->mock_bind_time_ms = 1;
	rc = fmd_vppb_bind(ctx, msw, 0, 1, 4, 0xffff);
	CHECK(rc == FMD_OP_STARTED &&
	      msw->vcs[0].vppbs[1].mock_next_ld_id == 0xff,
	      "mock background bind: %d, next LD %d", rc,
	      msw->vcs[0].vppbs[1].mock_next_ld_id);
	msw->vcs[0].vppbs[1] = (struct fmd_vppb){
		.vppb_id = 1, .binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 4, .bound_ld_id = 0xff };
	rc = fmd_vppb_unbind(ctx, msw, 0, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == FMD_OP_STARTED &&
	      msw->vcs[0].vppbs[1].mock_next_ld_id == 0xff,
	      "mock background unbind: %d, next LD %d", rc,
	      msw->vcs[0].vppbs[1].mock_next_ld_id);
	msw->vcs[0].vppbs[1] = (struct fmd_vppb){
		.vppb_id = 1, .binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 4, .bound_ld_id = 0xff };

	/* the validator: any other Bound LD ID on a 02h vPPB is refused */
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 0, "%d rule(s) broken with LD FFh", invalid);
	msw->vcs[0].vppbs[1].bound_ld_id = 3;
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 1, "%d rule(s) broken by Bound LD ID 3 on 02h", invalid);
	msw->vcs[0].vppbs[1].binding_status = FMD_VPPB_UNBOUND;
	invalid = fmd_topology_validate(ctx);
	CHECK(invalid == 1, "%d rule(s) broken by Bound LD ID 3 on 00h", invalid);
out:
	fmd_ctx_free(ctx);
	report("Bound LD ID is FFh unless Bound LD", before);
}

/*
 * Bound PID: "For PBR Switches when Binding Status is 04h: Bits[11:0]:
 * vPPB[Start vPPB] Bound PID ... Bits[15:12]: Reserved." (Table 7-34). The
 * two bytes at 5h are 34h F2h: PID 234h, and no port or LD.
 */
static void test_bound_pid_decode(void)
{
	static const uint8_t pid[3] = { 0x04, 0x34, 0xf2 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	char id[FMD_MAX_ID_LEN];
	int before = failures;

	answer_identify(ep, 0x5101, 0x00);
	answer_switch_ports(ep, 6, 0x00, 0x00);
	answer_vcs(ep, 0, 0, pid, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0x5101);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	vcs = sw ? fmd_switch_find_vcs(sw, 0) : NULL;
	CHECK(vcs && vcs->num_vppbs == 3, "no VCS 0");
	if (vcs) {
		struct fmd_vppb *vppb = &vcs->vppbs[1];

		CHECK(vppb->binding_status == FMD_VPPB_BOUND_PBR,
		      "status %d", vppb->binding_status);
		CHECK(vppb->bound_pbr_id == 0x234, "Bound PID %03xh",
		      vppb->bound_pbr_id);
		CHECK(vppb->bound_port_id == 0 && vppb->bound_ld_id == 0xff,
		      "a Bound PID read as port %d LD %d", vppb->bound_port_id,
		      vppb->bound_ld_id);
	}
	fmd_ctx_free(ctx);
	report("Bound PID of a vPPB", before);
}

/*
 * A VCS that is not enabled has no host: its USP ID is "Valid only when the
 * VCS is enabled" (Table 7-34). Its vPPBs are still read.
 */
static void test_disabled_vcs_has_no_host(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char id[FMD_MAX_ID_LEN], host[FMD_MAX_ID_LEN + 16];
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	int before = failures;

	answer_identify(ep, 0x5102, 0x00);
	answer_switch_ports(ep, 6, 0x00, 0x00);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs_state(ep, 2, 0x00, 1, unbound, unbound);
	fmd_discover(ctx);

	device_id(id, sizeof(id), 0x5102);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;
	vcs = fmd_switch_find_vcs(sw, 2);
	CHECK(sw->num_vcs == 2 && vcs && vcs->state == 0x00 &&
	      vcs->num_vppbs == 3, "VCS 2, disabled, not read");
	CHECK(ctx->topology.root->host_count == 1, "%d hosts",
	      ctx->topology.root->host_count);
	snprintf(host, sizeof(host), "%.50s_host0", id);
	CHECK(fmd_topology_find_host(&ctx->topology, host), "no host of VCS 0");
	snprintf(host, sizeof(host), "%.50s_host2", id);
	CHECK(!fmd_topology_find_host(&ctx->topology, host),
	      "a host for disabled VCS 2");
out:
	fmd_ctx_free(ctx);
	report("a disabled VCS has no host", before);
}

/*
 * A Binding Change that no task follows: a bind made by another FM,
 * through another CCI, or by the switch. The VCSs are read again, and the
 * vPPB is what the switch says.
 */
static void test_untracked_binding_change_refreshes(void)
{
	static const uint8_t port4[3] = { 0x02, 4, 0xff };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5104);
	char id[FMD_MAX_ID_LEN];
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	int before = failures;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x5104);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	CHECK(sw, "no switch %s", id);
	if (!sw)
		goto out;

	/* vPPB 2 of VCS 0, now bound to port 4, in the Informational log */
	answer_event(ep, virtual_switch_event, 0x21,
		     (uint8_t[6]){ 0, 2, 0x00, 0x02, 4, 0xff }, 6);
	answer_vcs(ep, 0, 0, port2, port4);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_switch_events_poll(ctx);

	vcs = fmd_switch_find_vcs(sw, 0);
	CHECK(vcs && vcs->vppbs[2].binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT &&
	      vcs->vppbs[2].bound_port_id == 4,
	      "vPPB 2 of VCS 0 after an untracked Binding Change: status %d",
	      vcs ? (int)vcs->vppbs[2].binding_status : -1);
	CHECK(last_opcode(ep) == 0x5200, "the VCSs were not read: last %04xh",
	      last_opcode(ep));
out:
	fmd_ctx_free(ctx);
	report("a Binding Change no task follows reads the VCSs", before);
}

/*
 * A task that is lost: its switch is gone, or does not answer Background
 * Operation Status five times. Its op is freed, and its Connection is what
 * the vPPB is: a DELETE answered 503 while it ran is accepted again.
 */
static void test_lost_task_frees_op(void)
{
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5105);
	char id[FMD_MAX_ID_LEN], conn_id[FMD_CONN_ID_LEN];
	struct fmd_bind_key key;
	struct fmd_switch *sw;
	struct fmd_task *task;
	struct fmd_host *host;
	json_t *resp = NULL;
	int before = failures, rc;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x5105);
	sw = fmd_topology_find_switch(&ctx->topology, id);
	host = sw ? vcs_host(ctx, id, 2) : NULL;
	CHECK(sw && host, "no switch %s or host of VCS 2", id);
	if (!sw || !host)
		goto out;

	/*
	 * A bind of LD 1 of port 5 to vPPB 1 of VCS 2, and a switch that
	 * then does not answer 0002h. On the fifth failed poll the VCSs are
	 * read: the vPPB is bound.
	 */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_bind(ctx, sw, 2, 1, 5, 1);
	CHECK(rc == FMD_OP_STARTED, "bind: %d", rc);
	key = bind_key(id, 2, 1, 5, 1);
	fmd_conn_id_format(&key, conn_id, sizeof(conn_id));
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, &key, 0);
	CHECK(task, "no task");
	if (!task)
		goto out;

	rc = fmd_redfish_delete_connection(ctx, "CXL", conn_id, &resp);
	CHECK(rc == -3, "DELETE while binding: %d", rc);
	for (int i = 0; i < 4; i++)
		fmd_tasks_poll(ctx);
	CHECK(task->state == FMD_TASK_RUNNING, "four failed polls ended the task");
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld1, unbound);
	fmd_tasks_poll(ctx);
	CHECK(task->state == FMD_TASK_EXCEPTION, "task state %d: %s",
	      task->state, task->message);
	CHECK(!ops_count(ctx) && conn_state(ctx, conn_id) >= 0 &&
	      conn_state(ctx, conn_id) != FMD_CONN_STARTING,
	      "the Connection of a bound vPPB after its task is lost: %d, %d ops",
	      conn_state(ctx, conn_id), ops_count(ctx));

	/*
	 * It is deleted, and the vPPB the switch bound is unbound: the one
	 * response queued, to 5202h, is taken.
	 */
	{
		unsigned int taken, then;

		cxlmi_mock_get_stats(ep, NULL, &then);
		answer_rc(ep, 0x5202, CXLMI_RET_SUCCESS);
		rc = fmd_redfish_delete_connection(ctx, "CXL", conn_id, &resp);
		cxlmi_mock_get_stats(ep, NULL, &taken);
		CHECK(rc == 0 && taken - then == 1 && !ops_count(ctx),
		      "DELETE after the task was lost: %d, %u responses taken",
		      rc, taken - then);
	}
	json_decref(resp);
	resp = NULL;

	/* a bind whose switch is gone: nothing says it is bound */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_bind(ctx, sw, 2, 2, 4, 0xffff);
	CHECK(rc == FMD_OP_STARTED, "bind: %d", rc);
	key = bind_key(id, 2, 2, 4, FMD_LD_NONE);
	fmd_conn_id_format(&key, conn_id, sizeof(conn_id));
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, &key, 0);
	CHECK(task, "no task");
	if (!task)
		goto out;
	CHECK(conn_state(ctx, conn_id) == FMD_CONN_STARTING, "%s: %d", conn_id,
	      conn_state(ctx, conn_id));
	snprintf(task->switch_id, sizeof(task->switch_id), "no_such_switch");
	fmd_tasks_poll(ctx);
	CHECK(task->state == FMD_TASK_EXCEPTION, "task state %d", task->state);
	CHECK(conn_state(ctx, conn_id) < 0 && !ops_count(ctx),
	      "a Connection is left Starting by a task whose switch is gone");

	/*
	 * An unbind lost the same way, of a vPPB the switch then reads as
	 * unbound: the Connection is gone. One still bound stands, and
	 * DELETE is accepted again.
	 */
	key = bind_key(id, 2, 1, 5, 1);
	fmd_conn_id_format(&key, conn_id, sizeof(conn_id));
	for (int unbinds = 0; unbinds < 2; unbinds++) {
		bool done = unbinds == 0;

		task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw, &key, 1);
		CHECK(task, "no task");
		if (!task)
			goto out;
		for (int i = 0; i < 4; i++)
			fmd_tasks_poll(ctx);
		answer_vcs(ep, 0, 0, port2, unbound);
		answer_vcs(ep, 2, 1, done ? unbound : ld1, unbound);
		fmd_tasks_poll(ctx);
		CHECK(task->state == FMD_TASK_EXCEPTION && !ops_count(ctx),
		      "task state %d, %d ops", task->state, ops_count(ctx));
		if (done) {
			CHECK(conn_state(ctx, conn_id) < 0,
			      "a Connection stands after its vPPB was unbound");
		} else {
			CHECK(conn_state(ctx, conn_id) >= 0 &&
			      conn_state(ctx, conn_id) != FMD_CONN_UPDATING,
			      "a Connection stays Updating after its unbind was lost: %d",
			      conn_state(ctx, conn_id));
			answer_rc(ep, 0x5202, CXLMI_RET_SUCCESS);
			rc = fmd_redfish_delete_connection(ctx, "CXL", conn_id, &resp);
			CHECK(rc == 0, "DELETE after the unbind was lost: %d", rc);
			json_decref(resp);
			resp = NULL;
		}
	}

out:
	fmd_ctx_free(ctx);
	report("a lost task frees its op", before);
}

/*
 * Connections computed from the switch (src/conn_view.c). Helpers.
 */

/* a chunk of @dev, in its domain (made if it has none), of LD @ld (-1: none) */
static struct fmd_mem_chunk *add_chunk(struct fmd_ctx *ctx,
				       struct fmd_component *dev,
				       const char *name, int ld)
{
	struct fmd_component *dom = fmd_first_child_type(dev, FMD_COMPONENT_DOMAIN);
	char id[FMD_MAX_ID_LEN];
	struct fmd_mem_chunk *chunk;

	if (!dom) {
		struct fmd_mem_domain *d;

		snprintf(id, sizeof(id), "%.50s_domain0", dev->id);
		d = fmd_domain_create(id);
		if (!d) {
			printf("FAIL  cannot create a domain\n");
			exit(1);
		}
		fmd_topology_add(&ctx->topology, &d->base, dev);
		dom = &d->base;
	}
	snprintf(id, sizeof(id), "%.40s_%s", dev->id, name);
	chunk = fmd_chunk_create(id);
	if (!chunk) {
		printf("FAIL  cannot create a chunk\n");
		exit(1);
	}
	chunk->size = 256 << 20;
	chunk->backing_ld = ld;
	fmd_topology_add(&ctx->topology, &chunk->base, dom);
	return chunk;
}

/* The Connections, by ID, of a walk with fmd_conn_for_each() */
struct conn_list {
	int n;
	char id[16][FMD_CONN_ID_LEN];
	enum fmd_conn_state state[16];
};

static int collect_conn(const struct fmd_conn_view *v, void *arg)
{
	struct conn_list *l = arg;

	if (l->n < 16) {
		snprintf(l->id[l->n], sizeof(l->id[0]), "%s", v->id);
		l->state[l->n] = v->state;
	}
	l->n++;
	return 0;
}

static int conn_index(const struct conn_list *l, const char *id)
{
	for (int i = 0; i < l->n && i < 16; i++) {
		if (!strcmp(l->id[i], id))
			return i;
	}
	return -1;
}

static struct conn_list list_conns(struct fmd_ctx *ctx)
{
	struct conn_list l = { 0 };

	fmd_conn_for_each(ctx, collect_conn, &l);
	return l;
}

/* the chunks of a Connection: how many, and whether @want is one */
struct chunk_count {
	int n;
	const struct fmd_mem_chunk *want;
	bool found;
};

static int count_chunk(const struct fmd_mem_chunk *chunk, void *arg)
{
	struct chunk_count *c = arg;

	c->n++;
	if (chunk == c->want)
		c->found = true;
	return 0;
}

static struct chunk_count conn_chunks(const struct fmd_conn_view *v,
				      const struct fmd_mem_chunk *want)
{
	struct chunk_count c = { .want = want };

	fmd_conn_for_each_chunk(v, count_chunk, &c);
	return c;
}

static struct fmd_bind_key bind_key(const char *sw, uint8_t vcs, uint8_t vppb,
				    uint8_t port, uint16_t ld)
{
	struct fmd_bind_key k = { .vcs_id = vcs, .vppb_id = vppb,
				  .port_id = port, .ld_id = ld };

	snprintf(k.switch_id, sizeof(k.switch_id), "%s", sw);
	return k;
}

/*
 * Bindings that were there before cxl-fmd: an SLD bound whole (02h) to a
 * vPPB of VCS 0, LD 1 of an MLD (03h) to one of VCS 2, as Get Virtual CXL
 * Switch Info reads them. Each is a Connection of the host of its VCS, to
 * the SLD with every chunk of it, or to the LD with the chunks on that LD.
 * A bind and an unbind under way show as Starting and Updating.
 */
static void test_conn_view_prebound_sld_ld(void)
{
	static const uint8_t sld4[3] = { 0x02, 4, 0xff };
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char sw_id[FMD_MAX_ID_LEN], want[FMD_CONN_ID_LEN];
	char host[FMD_MAX_ID_LEN + 16], ldid[2 * FMD_MAX_ID_LEN + 64];
	struct fmd_component *sld, *mld, *ld0, *ld1c;
	struct fmd_mem_chunk *s0, *s1, *c0, *c1, *c1uri;
	struct fmd_binding_op *bind_op, *unbind_op;
	struct fmd_conn_view v;
	struct fmd_bind_key key;
	struct chunk_count cc;
	struct conn_list l;
	struct fmd_switch *sw;
	int before = failures, rc, i;

	answer_identify(ep, 0x5200, 0x00);
	answer_switch_ports(ep, 6, 0x04, 0x04);	/* an SLD on port 4 */
	answer_vcs(ep, 0, 0, port2, sld4);
	answer_vcs(ep, 2, 1, ld1, unbound);
	fmd_discover(ctx);

	device_id(sw_id, sizeof(sw_id), 0x5200);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	snprintf(ldid, sizeof(ldid), "%.32s_port4_dev", sw_id);
	sld = fmd_topology_find(&ctx->topology, ldid);
	snprintf(ldid, sizeof(ldid), "%.32s_port5_dev", sw_id);
	mld = fmd_topology_find(&ctx->topology, ldid);
	CHECK(sw && sld && sld->type == FMD_COMPONENT_SLD &&
	      mld && mld->type == FMD_COMPONENT_MLD, "no switch, SLD or MLD");
	if (!sw || !sld || !mld)
		goto out;
	/* the MLD on port 5 has LD-IDs 0 and 1 */
	for (i = 0; i < 2; i++) {
		struct fmd_ld *ld;

		snprintf(ldid, sizeof(ldid), "%.40s_ld%d", mld->id, i);
		ld = fmd_ld_create(ldid, i);
		if (!ld) {
			printf("FAIL  cannot create an LD\n");
			exit(1);
		}
		fmd_topology_add(&ctx->topology, &ld->base, mld);
	}
	ld0 = &fmd_topology_find_ld(&ctx->topology, mld->id, 0)->base;
	ld1c = &fmd_topology_find_ld(&ctx->topology, mld->id, 1)->base;

	s0 = add_chunk(ctx, sld, "chunk0", -1);
	s1 = add_chunk(ctx, sld, "chunk1", -1);
	c0 = add_chunk(ctx, mld, "ld0_chunk", fmd_to_ld(ld0)->ld_id);
	c1 = add_chunk(ctx, mld, "ld1_chunk", fmd_to_ld(ld1c)->ld_id);
	/* a second chunk of LD 1, as a POST makes one */
	c1uri = add_chunk(ctx, mld, "more", 1);

	l = list_conns(ctx);
	CHECK(l.n == 2, "%d Connections, not 2", l.n);

	/* the SLD: whole port, VCS 0 vPPB 2 */
	snprintf(want, sizeof(want), "%s_vcs0_vppb2_p4", sw_id);
	CHECK(conn_index(&l, want) >= 0, "no Connection %s", want);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0, "lookup %s: %d", want, rc);
	if (!rc) {
		snprintf(host, sizeof(host), "%.50s_host0", sw_id);
		CHECK(v.kind == FMD_CONN_SWITCH && v.sw == sw &&
		      v.state == FMD_CONN_ENABLED, "SLD: kind %d state %d",
		      v.kind, v.state);
		CHECK(v.host && !strcmp(v.host->base.id, host), "SLD host %s",
		      v.host ? v.host->base.id : "none");
		CHECK(v.target == sld && v.device == sld && !v.ld && !v.head,
		      "SLD target");
		CHECK(v.key.vcs_id == 0 && v.key.vppb_id == 2 &&
		      v.key.port_id == 4 && v.key.ld_id == FMD_LD_NONE,
		      "SLD key %d/%d/%d/%x", v.key.vcs_id, v.key.vppb_id,
		      v.key.port_id, v.key.ld_id);
		CHECK(!v.op && !v.made_here, "SLD op or mark");
		cc = conn_chunks(&v, s1);
		CHECK(cc.n == 2 && cc.found, "SLD chunks: %d", cc.n);
		(void)s0;
	}

	/* LD 1: VCS 2 (an ID, not an index) vPPB 1 */
	snprintf(want, sizeof(want), "%s_vcs2_vppb1_p5_ld1", sw_id);
	CHECK(conn_index(&l, want) >= 0, "no Connection %s", want);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0, "lookup %s: %d", want, rc);
	if (!rc) {
		snprintf(host, sizeof(host), "%.50s_host2", sw_id);
		CHECK(v.host && !strcmp(v.host->base.id, host), "LD host %s",
		      v.host ? v.host->base.id : "none");
		CHECK(v.state == FMD_CONN_ENABLED && v.target == ld1c &&
		      v.device == mld && v.ld == fmd_to_ld(ld1c),
		      "LD 1: state %d", v.state);
		cc = conn_chunks(&v, c1uri);
		CHECK(cc.n == 2 && cc.found, "LD 1 chunks: %d", cc.n);
		cc = conn_chunks(&v, c0);
		CHECK(!cc.found, "LD 0's chunk on LD 1's Connection");
		(void)c1;
	}

	/* by target, in the VCS it is bound in only */
	rc = fmd_conn_find_target(ctx, sw, 2, 5, 1, &v);
	CHECK(rc == 0 && !strcmp(v.id, want), "find LD 1 in VCS 2: %d", rc);
	rc = fmd_conn_find_target(ctx, sw, 0, 5, 1, &v);
	CHECK(rc == -ENOENT, "LD 1 found in VCS 0: %d", rc);
	rc = fmd_conn_find_target(ctx, sw, 2, 5, 0, &v);
	CHECK(rc == -ENOENT, "LD 0 found in VCS 2: %d", rc);
	rc = fmd_conn_find_target(ctx, sw, 0, 4, FMD_LD_NONE, &v);
	CHECK(rc == 0 && v.target == sld, "find the SLD in VCS 0: %d", rc);

	/* a bind under way on unbound vPPB 2 of VCS 2: Starting, found */
	key = bind_key(sw_id, 2, 2, 5, 0);
	bind_op = fmd_binding_op_add(ctx, &key, FMD_BIND_OP_BIND, 0, "1");
	l = list_conns(ctx);
	snprintf(want, sizeof(want), "%s_vcs2_vppb2_p5_ld0", sw_id);
	i = conn_index(&l, want);
	CHECK(l.n == 3 && i >= 0 && l.state[i] == FMD_CONN_STARTING,
	      "bind under way: %d Connections, %s %s", l.n, want,
	      i >= 0 ? "listed" : "not listed");
	rc = fmd_conn_find_target(ctx, sw, 2, 5, 0, &v);
	CHECK(rc == 0 && v.op == bind_op && v.state == FMD_CONN_STARTING &&
	      v.ld == fmd_to_ld(ld0), "find the LD being bound: %d", rc);
	CHECK(fmd_binding_op_find_target(ctx, sw_id, 5, 0) == bind_op,
	      "the op by target");
	/* 01h while the switch binds: still the op's */
	fmd_vcs_find_vppb(fmd_switch_find_vcs(sw, 2), 2)->binding_status =
		FMD_VPPB_BUSY;
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.state == FMD_CONN_STARTING, "01h, binding: %d", rc);
	fmd_binding_op_remove(ctx, bind_op);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == -ENOENT, "01h with no op: %d", rc);
	fmd_vcs_find_vppb(fmd_switch_find_vcs(sw, 2), 2)->binding_status =
		FMD_VPPB_UNBOUND;

	/* an unbind under way of the SLD: Updating, and gone at 00h */
	key = bind_key(sw_id, 0, 2, 4, FMD_LD_NONE);
	unbind_op = fmd_binding_op_add(ctx, &key, FMD_BIND_OP_UNBIND, 1, "2");
	snprintf(want, sizeof(want), "%s_vcs0_vppb2_p4", sw_id);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.state == FMD_CONN_UPDATING && v.op == unbind_op,
	      "unbind under way: %d, state %d", rc, v.state);
	fmd_vcs_find_vppb(fmd_switch_find_vcs(sw, 0), 2)->binding_status =
		FMD_VPPB_BUSY;
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.state == FMD_CONN_UPDATING, "01h, unbinding: %d", rc);
	fmd_vcs_find_vppb(fmd_switch_find_vcs(sw, 0), 2)->binding_status =
		FMD_VPPB_UNBOUND;
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == -ENOENT, "unbound, the op not yet freed: %d", rc);
	fmd_binding_op_remove(ctx, unbind_op);
	CHECK(!ctx->binding_ops, "an op left");

	/* a mark: made here, until the vPPB no longer holds it */
	snprintf(want, sizeof(want), "%s_vcs2_vppb1_p5_ld1", sw_id);
	key = bind_key(sw_id, 2, 1, 5, 1);
	fmd_binding_mark_add(ctx, &key);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.made_here, "marked: %d", rc);
	fmd_binding_marks_prune(ctx, sw_id);
	CHECK(fmd_binding_mark_find(ctx, &key), "a mark still held dropped");
	fmd_vcs_find_vppb(fmd_switch_find_vcs(sw, 2), 1)->bound_ld_id = 0;
	fmd_binding_marks_prune(ctx, NULL);
	CHECK(!fmd_binding_mark_find(ctx, &key), "a mark no longer held kept");
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == -ENOENT, "LD 1's ID names LD 0's binding: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("Connections of bindings made before cxl-fmd", before);
}

/*
 * vPPBs that give no Connection: vPPB 0, the USP's ("the USP vPPB, then
 * the DSP vPPBs", Section 7.1.4) even bound to a memory device; one of a
 * VCS that is not enabled; one bound to a PCIe device; one with a Bound
 * PID (04h); one being bound or unbound (01h) by no one cxl-fmd knows of.
 */
static void test_conn_view_skips_vppb0_disabled_nonmemory_pid(void)
{
	static const uint8_t pid[3] = { 0x04, 0x34, 0x02 };
	static const uint8_t sld4[3] = { 0x02, 4, 0xff };
	static const uint8_t busy[3] = { 0x01, 0, 0xff };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char sw_id[FMD_MAX_ID_LEN], want[FMD_CONN_ID_LEN];
	char host_id[FMD_MAX_ID_LEN + 16];
	struct fmd_host *host;
	struct fmd_switch *sw;
	struct fmd_vcs *vcs;
	struct conn_list l;
	int before = failures;

	answer_identify(ep, 0x5201, 0x00);
	answer_switch_ports(ep, 6, 0x04, 0x04);	/* an SLD on port 4 */
	answer_vcs(ep, 0, 0, port2, pid);	/* PCIe device, Bound PID */
	answer_vcs_state(ep, 2, 0x00, 1, sld4, busy);	/* not enabled */
	fmd_discover(ctx);

	device_id(sw_id, sizeof(sw_id), 0x5201);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	CHECK(sw && sw->num_vcs == 2, "no switch with 2 VCSs");
	if (!sw)
		goto out;

	/*
	 * A host for VCS 2 as if it had been enabled once, so that only its
	 * state keeps it from a Connection.
	 */
	snprintf(host_id, sizeof(host_id), "%.50s_host2", sw_id);
	host = fmd_host_create(host_id);
	snprintf(host->switch_id, sizeof(host->switch_id), "%s", sw_id);
	host->vcs_id = 2;
	host->direct_attached = false;
	fmd_topology_add(&ctx->topology, &host->base, NULL);

	/* vPPB 0 of VCS 0 bound to the SLD's port, not the USP */
	vcs = fmd_switch_find_vcs(sw, 0);
	vcs->vppbs[0].bound_port_id = 4;

	l = list_conns(ctx);
	CHECK(l.n == 0, "%d Connections: %s", l.n, l.n ? l.id[0] : "");

	/* the same port on a DSP vPPB of VCS 0 is one */
	vcs->vppbs[2] = (struct fmd_vppb){ .vppb_id = 2,
		.binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 4, .bound_ld_id = 0xff };
	l = list_conns(ctx);
	snprintf(want, sizeof(want), "%s_vcs0_vppb2_p4", sw_id);
	CHECK(l.n == 1 && !strcmp(l.id[0], want), "%d Connections, not %s",
	      l.n, want);
out:
	fmd_ctx_free(ctx);
	report("vPPBs that are not Connections", before);
}

/*
 * A Connection ID is the switch's ID with "_vcs{V}_vppb{P}_p{N}", and
 * "_ld{L}" for an LD, read from the right: a switch ID holds '_', and may
 * hold what looks like any of these. One spelling only.
 */
static void test_conn_id_roundtrip_underscored_switch(void)
{
	static const char *const switches[] = {
		"cxl_0000000000005202", "sw_vcs1_vppb2_p3", "a_ld3", "x_p_ld",
		"_", "sw_vcs1_vppb2_p3_ld4",
	};
	static const char *const bad[] = {
		"sw_vcs0_vppb1_p05", "sw_vcs0_vppb1_p+5", "sw_vcs0_vppb1_p256",
		"sw_vcs0_vppb1_p5_ld", "sw_vcs0_vppb1_p5_ld01", "_vcs0_vppb1_p5",
		"sw_vcs0_p5", "sw_vppb1_p5", "sw_vcs0_vppb1", "sw_vcs0_vppb1_p5_",
		"sw_vcs0_vppb1_p5_ld65535", "",
	};
	static const uint8_t sld4[3] = { 0x02, 4, 0xff };
	struct fmd_bind_key k, back;
	char id[FMD_CONN_ID_LEN], first[FMD_CONN_ID_LEN];
	struct fmd_conn_view v;
	struct conn_list l;
	int before = failures, rc;

	for (size_t s = 0; s < sizeof(switches) / sizeof(switches[0]); s++) {
		for (int ld = -1; ld <= 1; ld++) {
			k = bind_key(switches[s], 7, 31, 255,
				     ld < 0 ? FMD_LD_NONE : (uint16_t)(ld ? 4094 : 0));
			rc = fmd_conn_id_format(&k, id, sizeof(id));
			CHECK(rc == 0, "format %s: %d", switches[s], rc);
			memset(&back, 0, sizeof(back));
			rc = fmd_conn_id_parse(id, &back);
			CHECK(rc == 0 && fmd_bind_key_eq(&k, &back),
			      "%s parsed back as %s/%d/%d/%d/%x: %d", id,
			      back.switch_id, back.vcs_id, back.vppb_id,
			      back.port_id, back.ld_id, rc);
		}
	}
	for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
		rc = fmd_conn_id_parse(bad[b], &back);
		CHECK(rc == -EINVAL, "\"%s\" parsed: %d", bad[b], rc);
	}

	/* a switch read twice gives the same ID, as after a restart */
	for (int run = 0; run < 2; run++) {
		struct fmd_ctx *ctx = new_ctx();
		struct cxlmi_endpoint *ep = new_device(ctx);
		char sw_id[FMD_MAX_ID_LEN];

		answer_identify(ep, 0x5202, 0x00);
		answer_switch_ports(ep, 6, 0x04, 0x04);
		answer_vcs(ep, 0, 0, port2, sld4);
		answer_vcs(ep, 2, 1, unbound, unbound);
		fmd_discover(ctx);
		device_id(sw_id, sizeof(sw_id), 0x5202);
		CHECK(strchr(sw_id, '_'), "switch ID %s has no '_'", sw_id);

		l = list_conns(ctx);
		CHECK(l.n == 1, "run %d: %d Connections", run, l.n);
		if (l.n != 1) {
			fmd_ctx_free(ctx);
			continue;
		}
		if (!run)
			snprintf(first, sizeof(first), "%s", l.id[0]);
		else
			CHECK(!strcmp(first, l.id[0]), "%s, then %s", first, l.id[0]);
		rc = fmd_conn_lookup(ctx, l.id[0], &v);
		CHECK(rc == 0 && !strcmp(v.id, l.id[0]), "lookup %s: %d",
		      l.id[0], rc);

		/* another spelling of it is not it */
		snprintf(id, sizeof(id), "%s_vcs0_vppb2_p04", sw_id);
		rc = fmd_conn_lookup(ctx, id, &v);
		CHECK(rc == -ENOENT, "lookup %s: %d", id, rc);

		/* nor is it once its vPPB is bound to another port */
		fmd_switch_find_vcs(fmd_topology_find_switch(&ctx->topology,
							     sw_id), 0)
			->vppbs[2].bound_port_id = 5;
		rc = fmd_conn_lookup(ctx, l.id[0], &v);
		CHECK(rc == -ENOENT, "stale %s: %d", l.id[0], rc);
		fmd_ctx_free(ctx);
	}
	report("Connection IDs, and a switch ID with '_'", before);
}

/*
 * A vPPB bound to a port that has no device: "a physical port whose link
 * is down" is bound whole (Section 7.6.7.2.2), and the device on a bound
 * port may go. The Connection is Absent, reaches nothing, and keeps its
 * ID.
 */
static void test_conn_view_absent(void)
{
	static const uint8_t sld4[3] = { 0x02, 4, 0xff };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char sw_id[FMD_MAX_ID_LEN], want[FMD_CONN_ID_LEN];
	char dev_id[FMD_MAX_ID_LEN + 16];
	struct fmd_component *sld;
	struct fmd_conn_view v;
	struct fmd_switch *sw;
	struct fmd_port *port;
	struct conn_list l;
	int before = failures, rc;

	answer_identify(ep, 0x5203, 0x00);
	answer_switch_ports(ep, 6, 0x04, 0x04);	/* an SLD on port 4 */
	answer_vcs(ep, 0, 0, port2, sld4);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_discover(ctx);

	device_id(sw_id, sizeof(sw_id), 0x5203);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	snprintf(dev_id, sizeof(dev_id), "%.32s_port4_dev", sw_id);
	sld = fmd_topology_find(&ctx->topology, dev_id);
	port = fmd_topology_find_port(&ctx->topology, sw_id, 4);
	CHECK(sw && sld && port, "no switch, SLD or port 4");
	if (!sw || !sld || !port)
		goto out;
	add_chunk(ctx, sld, "chunk0", -1);

	snprintf(want, sizeof(want), "%s_vcs0_vppb2_p4", sw_id);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.state == FMD_CONN_ENABLED && conn_chunks(&v, NULL).n == 1,
	      "before the removal: %d", rc);

	/* the SLD goes, and the port reports no device */
	fmd_topology_remove(&ctx->topology, sld);
	port->conn_dev_type = FMD_CONN_NONE;
	l = list_conns(ctx);
	CHECK(l.n == 1 && !strcmp(l.id[0], want) &&
	      l.state[0] == FMD_CONN_ABSENT, "%d Connections, %s state %d",
	      l.n, l.n ? l.id[0] : "", l.n ? (int)l.state[0] : -1);
	rc = fmd_conn_lookup(ctx, want, &v);
	CHECK(rc == 0 && v.state == FMD_CONN_ABSENT && !v.device &&
	      !v.target && !v.ld && conn_chunks(&v, NULL).n == 0,
	      "absent: %d, state %d", rc, v.state);
	CHECK(v.host != NULL, "an Absent Connection has no host");

	/* a port with a device cxl-fmd does not serve is none */
	port->conn_dev_type = FMD_CONN_TYPE2;
	l = list_conns(ctx);
	CHECK(l.n == 0, "%d Connections to a Type 2 device", l.n);
out:
	fmd_ctx_free(ctx);
	report("a binding to a port with no device is Absent", before);
}

/*
 * A Multi-Headed device on a switch. A head that presents an SLD, bound
 * whole, reaches the one pool LD mapped to it ("A head with one LD mapped
 * shall present itself as an SLD", Section 2.5). One that presents an
 * MLD is bound by an LD-ID "relative to that head", which "The MH-MLD maps
 * ... to the device-wide LD index" (Section 2.5.1) by no rule the FM is
 * told: that Connection reaches the head, Unresolved, with no chunks. So
 * is a binding of an MLD that is not one of its LDs (cxl-fmd's choice).
 */
static void test_conn_view_unresolved_mh_mld(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct fmd_switch *sw = fmd_switch_create("sw_mh");
	struct fmd_port *p4, *p5, *p3;
	struct fmd_mhd *mhd;
	struct fmd_head *h0, *h1;
	struct fmd_mld *mld;
	struct fmd_ld *lds[3], *mld_ld;
	struct fmd_mem_chunk *chunk2;
	struct fmd_host *host;
	struct fmd_conn_view v;
	struct conn_list l;
	int before = failures, rc;
	char id[FMD_MAX_ID_LEN];

	fmd_topology_add(&ctx->topology, &sw->base, NULL);
	add_port(ctx, sw, 0, FMD_PORT_USP);
	p3 = add_port(ctx, sw, 3, FMD_PORT_DSP);
	p4 = add_port(ctx, sw, 4, FMD_PORT_DSP);
	p5 = add_port(ctx, sw, 5, FMD_PORT_DSP);

	mhd = fmd_mhd_create("mhd");
	fmd_topology_add(&ctx->topology, &mhd->base, NULL);
	h0 = fmd_head_create("mhd_head0", 0);
	h0->presentation = FMD_HEAD_MLD;
	h1 = fmd_head_create("mhd_head1", 1);
	h1->presentation = FMD_HEAD_SLD;
	fmd_topology_add(&ctx->topology, &h0->base, &mhd->base);
	fmd_topology_add(&ctx->topology, &h1->base, &mhd->base);
	h0->port = p4;
	p4->head = h0;
	h1->port = p5;
	p5->head = h1;
	for (int i = 0; i < 3; i++) {
		snprintf(id, sizeof(id), "mhd_ld%d", i);
		lds[i] = fmd_ld_create(id, i);
		lds[i]->binding_point_id = i < 2 ? 0 : 1;
		fmd_topology_add(&ctx->topology, &lds[i]->base, &mhd->base);
	}
	add_chunk(ctx, &mhd->base, "ld0_chunk", 0);
	add_chunk(ctx, &mhd->base, "ld1_chunk", 1);
	/* as POST makes one */
	chunk2 = add_chunk(ctx, &mhd->base, "posted", 2);

	/* an MLD on port 3 with LD 0 only */
	mld = fmd_mld_create("mld3");
	fmd_topology_add(&ctx->topology, &mld->base, &p3->base);
	p3->conn_dev_type = FMD_CONN_MLD;
	mld_ld = fmd_ld_create("mld3_ld0", 0);
	fmd_topology_add(&ctx->topology, &mld_ld->base, &mld->base);

	host = fmd_host_create("sw_mh_host0");
	snprintf(host->switch_id, sizeof(host->switch_id), "sw_mh");
	host->vcs_id = 0;
	host->direct_attached = false;
	fmd_topology_add(&ctx->topology, &host->base, NULL);

	sw->num_vcs = 1;
	sw->vcs[0].vcs_id = 0;
	sw->vcs[0].state = 0x01;
	sw->vcs[0].num_vppbs = 5;
	sw->vcs[0].vppbs[0] = (struct fmd_vppb){ .vppb_id = 0,
		.binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 0, .bound_ld_id = 0xff };
	/* LD-ID 1 of the head that presents an MLD */
	sw->vcs[0].vppbs[1] = (struct fmd_vppb){ .vppb_id = 1,
		.binding_status = FMD_VPPB_BOUND_LD,
		.bound_port_id = 4, .bound_ld_id = 1 };
	/* the head that presents an SLD, whole */
	sw->vcs[0].vppbs[2] = (struct fmd_vppb){ .vppb_id = 2,
		.binding_status = FMD_VPPB_BOUND_PHYSICAL_PORT,
		.bound_port_id = 5, .bound_ld_id = 0xff };
	/* LD 3 of an MLD with LD 0 only */
	sw->vcs[0].vppbs[3] = (struct fmd_vppb){ .vppb_id = 3,
		.binding_status = FMD_VPPB_BOUND_LD,
		.bound_port_id = 3, .bound_ld_id = 3 };
	sw->vcs[0].vppbs[4] = (struct fmd_vppb){ .vppb_id = 4,
		.binding_status = FMD_VPPB_UNBOUND, .bound_ld_id = 0xff };
	fmd_switch_resolve_vppb_bindings(&ctx->topology, sw);

	l = list_conns(ctx);
	CHECK(l.n == 3, "%d Connections, not 3", l.n);

	rc = fmd_conn_lookup(ctx, "sw_mh_vcs0_vppb1_p4_ld1", &v);
	CHECK(rc == 0 && v.state == FMD_CONN_UNRESOLVED, "MH-MLD head: %d, state %d",
	      rc, rc ? -1 : (int)v.state);
	if (!rc) {
		CHECK(v.target == &h0->base && v.head == h0 && !v.ld &&
		      v.device == &mhd->base, "MH-MLD head: target %s",
		      v.target ? v.target->id : "none");
		CHECK(conn_chunks(&v, NULL).n == 0, "chunks of an unresolved LD");
	}

	rc = fmd_conn_lookup(ctx, "sw_mh_vcs0_vppb2_p5", &v);
	CHECK(rc == 0 && v.state == FMD_CONN_ENABLED, "MH-SLD head: %d", rc);
	if (!rc) {
		struct chunk_count cc = conn_chunks(&v, chunk2);

		CHECK(v.target == &lds[2]->base && v.ld == lds[2] &&
		      v.head == h1 && v.device == &mhd->base,
		      "MH-SLD head: target %s", v.target ? v.target->id : "none");
		CHECK(cc.n == 1 && cc.found, "MH-SLD head: %d chunks", cc.n);
	}

	rc = fmd_conn_lookup(ctx, "sw_mh_vcs0_vppb3_p3_ld3", &v);
	CHECK(rc == 0 && v.state == FMD_CONN_UNRESOLVED && !v.target &&
	      v.device == &mld->base && conn_chunks(&v, NULL).n == 0,
	      "an LD the MLD does not have: %d", rc);

	/* the handlers' rule for a vPPB is the same */
	CHECK(fmd_bind_target(p4, 1) == &h0->base &&
	      fmd_bind_target(p5, FMD_LD_NONE) == &lds[2]->base &&
	      fmd_bind_target(p5, 0) == NULL && fmd_bind_target(p4, FMD_LD_NONE) == NULL &&
	      fmd_bind_target(p3, 0) == &mld_ld->base &&
	      fmd_bind_target(p3, FMD_LD_NONE) == NULL,
	      "fmd_bind_target");
	CHECK(fmd_ld_head(lds[0]) == h0 && fmd_ld_head(lds[2]) == h1 &&
	      fmd_head_whole_ld(h1) == lds[2] && !fmd_head_whole_ld(h0),
	      "heads and LDs");

	/* served: Enabled, Health Warning, its target the head's Endpoint */
	{
		json_t *c = fmd_redfish_connection(ctx, "CXL", "sw_mh_vcs0_vppb1_p4_ld1");
		json_t *st = json_object_get(c, "Status");
		json_t *t = json_object_get(json_object_get(c, "Links"), "TargetEndpoints");

		CHECK(c && !strcmp(json_string_value(json_object_get(st, "State")), "Enabled") &&
		      !strcmp(json_string_value(json_object_get(st, "Health")), "Warning") &&
		      json_array_size(t) == 1 &&
		      strstr(json_string_value(json_object_get(json_array_get(t, 0),
							       "@odata.id")), "/mhd_head0") &&
		      !json_object_get(c, "MemoryChunkInfo"),
		      "the unresolved Connection as served");
		json_decref(c);
	}

	/*
	 * POST to a head that presents an MLD, as its Endpoint, as a pool LD
	 * mapped to it, or as the device with a chunk of one: 400, and
	 * nothing is bound. The head that presents an SLD has its Connection.
	 */
	{
		static const char *const bodies[] = {
			"mhd_head0\"}]}}",
			"mhd_ld0\"}]}}",
			("mhd\"}]}, \"MemoryChunkInfo\": [{\"MemoryChunk\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/mhd/MemoryDomains/"
			 "mhd_domain0/MemoryChunks/mhd_ld1_chunk\"}}]}"),
		};
		char body[512];
		json_t *resp = NULL;

		for (size_t i = 0; i < 3; i++) {
			snprintf(body, sizeof(body),
				 "{\"Links\": {\"InitiatorEndpoints\": [{\"@odata.id\": "
				 "\"/redfish/v1/Fabrics/CXL/Endpoints/sw_mh_host0\"}], \"TargetEndpoints\": [{\"@odata.id\": "
				 "\"/redfish/v1/Fabrics/CXL/Endpoints/%s", bodies[i]);
			rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
			CHECK(rc == -14 && !resp, "POST %zu to a head that presents an MLD: %d",
			      i, rc);
			json_decref(resp);
			resp = NULL;
		}
		CHECK(sw->vcs[0].vppbs[4].binding_status == FMD_VPPB_UNBOUND &&
		      !ctx->binding_ops, "something was bound for an MLD-presenting head");

		snprintf(body, sizeof(body),
			 "{\"Links\": {\"InitiatorEndpoints\": [{\"@odata.id\": "
			 "\"/redfish/v1/Fabrics/CXL/Endpoints/sw_mh_host0\"}], \"TargetEndpoints\": [{\"@odata.id\": "
			 "\"/redfish/v1/Fabrics/CXL/Endpoints/mhd_ld2\"}]}}");
		rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
		CHECK(rc == FMD_CONN_EXISTS &&
		      !strcmp(json_string_value(json_object_get(resp, "Id")),
			      "sw_mh_vcs0_vppb2_p5"),
		      "POST of the SLD-presenting head's pool LD: %d", rc);
		json_decref(resp);
	}

	/*
	 * Which LD-ID of head 0 pool LD 0 or 1 is, is not known: each is
	 * taken to be in use while the head has a binding, Unresolved too.
	 * The chunk of the head that presents an SLD is its Connection's.
	 */
	CHECK(fmd_redfish_delete_memory_chunk(ctx, "mhd", "mhd_domain0", "mhd_ld0_chunk") == -5 &&
	      fmd_redfish_delete_memory_chunk(ctx, "mhd", "mhd_domain0", "mhd_ld1_chunk") == -5 &&
	      fmd_redfish_delete_memory_chunk(ctx, "mhd", "mhd_domain0", "mhd_posted") == -5,
	      "a chunk of a head with a binding was deleted");

	fmd_ctx_free(ctx);
	report("a binding to a head that presents an MLD is Unresolved", before);
}

/*
 * POST and DELETE of Connections on a switch reached through a CCI. A bind
 * the switch carries out in the background is recorded before 5201h is
 * sent: a second POST of the same target is answered with its task and
 * sends no 5201h, and one of another target does not choose its vPPB,
 * which reads 00h meanwhile. Unsupported is 501. DELETE unbinds with
 * Unbind Option 1h, Simulate Managed Hot-Remove (Table 7-36).
 */
static void test_post_delete_on_a_switch(void)
{
	static const uint8_t ld0[3] = { 0x03, 5, 0 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5106);
	char sw_id[FMD_MAX_ID_LEN], body[512], cid[FMD_CONN_ID_LEN];
	struct fmd_switch *sw;
	struct fmd_host *host;
	struct fmd_task *task;
	json_t *resp = NULL;
	uint8_t pl[16];
	uint16_t op;
	int before = failures, rc, task_id;

	answer_ld_info(ep, 2ULL * 4 << 28, 2);
	answer_ld_allocations(ep, 2, 0, 4);
	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x5106);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	host = sw ? vcs_host(ctx, sw_id, 2) : NULL;
	CHECK(sw && host, "no switch or host of VCS 2");
	if (!sw || !host)
		goto out;

#define POST_LD(n) snprintf(body, sizeof(body), \
	"{\"Links\": {\"InitiatorEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%s\"}], " \
	"\"TargetEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%.32s_port5_dev_ld%d\"}]}}", \
	host->base.id, sw_id, n)

	/* LD 0: the switch binds it in the background, to vPPB 1 */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	POST_LD(0);
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	CHECK(rc == FMD_OP_STARTED && resp, "POST of LD 0: %d", rc);
	task_id = resp ? atoi(json_string_value(json_object_get(resp, "Id"))) : -1;
	json_decref(resp);
	resp = NULL;
	CHECK(ops_count(ctx) == 1 && ctx->binding_ops->key.vppb_id == 1 &&
	      ctx->binding_ops->kind == FMD_BIND_OP_BIND,
	      "%d ops after the POST", ops_count(ctx));
	snprintf(cid, sizeof(cid), "%s_vcs2_vppb1_p5_ld0", sw_id);
	CHECK(conn_state(ctx, cid) == FMD_CONN_STARTING, "%s: %d", cid,
	      conn_state(ctx, cid));

	/* again: the same task, and no 5201h, which nothing would answer */
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	CHECK(rc == FMD_OP_STARTED && resp &&
	      atoi(json_string_value(json_object_get(resp, "Id"))) == task_id &&
	      last_opcode(ep) != 0x5201 && ops_count(ctx) == 1,
	      "POST of LD 0 again: %d, last %04xh", rc, last_opcode(ep));
	json_decref(resp);
	resp = NULL;

	/*
	 * LD 1, while vPPB 1 still reads unbound: vPPB 2 is chosen. The
	 * switch answers Unsupported: 501, and nothing is left of it.
	 */
	answer_rc(ep, 0x5201, CXLMI_RET_UNSUPPORTED);
	POST_LD(1);
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == -17 && op == 0x5201 && pl[0] == 2 && pl[1] == 2 && pl[2] == 5 &&
	      pl[4] == 1 && pl[5] == 0,
	      "POST of LD 1: %d, %04xh to VCS %d vPPB %d port %d LD %d", rc, op,
	      pl[0], pl[1], pl[2], pl[4] | pl[5] << 8);
	CHECK(ops_count(ctx) == 1, "%d ops after a bind refused", ops_count(ctx));

	/* the one being bound cannot be deleted yet */
	rc = fmd_redfish_delete_connection(ctx, "CXL", cid, &resp);
	CHECK(rc == -3, "DELETE while binding: %d", rc);

	/* the bind is over */
	task = fmd_task_find(ctx, task_id);
	answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_SUCCESS);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld0, unbound);
	fmd_tasks_poll(ctx);
	CHECK(task && task->state == FMD_TASK_COMPLETED && !ops_count(ctx) &&
	      conn_state(ctx, cid) == FMD_CONN_ENABLED,
	      "after the bind: task %d, Connection %d", task ? (int)task->state : -1,
	      conn_state(ctx, cid));

	/* and again: 200, that Connection */
	POST_LD(0);
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	CHECK(rc == FMD_CONN_EXISTS && last_opcode(ep) != 0x5201,
	      "POST of a bound LD: %d", rc);
	json_decref(resp);
	resp = NULL;

	/* DELETE: Unbind vPPB with option 1h, which the switch fails */
	answer_rc(ep, 0x5202, CXLMI_RET_INTERNAL);
	rc = fmd_redfish_delete_connection(ctx, "CXL", cid, &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == -2 && op == 0x5202 && pl[0] == 2 && pl[1] == 1 && pl[2] == 0x01,
	      "DELETE: %d, %04xh VCS %d vPPB %d option %d", rc, op, pl[0], pl[1], pl[2]);
	answer_rc(ep, 0x5202, CXLMI_RET_UNSUPPORTED);
	rc = fmd_redfish_delete_connection(ctx, "CXL", cid, &resp);
	CHECK(rc == -5 && !ops_count(ctx) && conn_state(ctx, cid) == FMD_CONN_ENABLED,
	      "DELETE answered Unsupported: %d", rc);
	answer_rc(ep, 0x5202, CXLMI_RET_SUCCESS);
	rc = fmd_redfish_delete_connection(ctx, "CXL", cid, &resp);
	CHECK(rc == 0 && !ops_count(ctx), "DELETE: %d", rc);
#undef POST_LD
out:
	fmd_ctx_free(ctx);
	report("POST and DELETE of Connections on a switch", before);
}

/*
 * A chunk of an LD that a vPPB binds is not deleted, nor another made on
 * that LD, while the binding stands (cxl-fmd's choice: the specification
 * does not say whether Set LD Allocations may change a bound LD). Refused
 * before Get or Set LD Allocations is sent; after the unbind the DELETE
 * goes on to Set LD Allocations (5402h).
 */
static void test_chunk_guard(void)
{
	static const uint8_t ld0[3] = { 0x03, 5, 0 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x5107);
	char sw_id[FMD_MAX_ID_LEN], dev[FMD_MAX_ID_LEN + 16], dom[FMD_MAX_ID_LEN + 32];
	char chunk[FMD_MAX_ID_LEN + 32], body[512], cid[FMD_CONN_ID_LEN];
	uint8_t alloc[4 + 16] = { 0 }, set[4 + 16] = { 0 };
	struct fmd_switch *sw;
	struct fmd_host *host;
	json_t *resp = NULL;
	int before = failures, rc, task_id;
	uint16_t op;

	/* 2 x 1 GiB, of 2.25 GiB: room for a chunk of 256 MiB */
	answer_ld_info(ep, (2ULL << 30) + (256ULL << 20), 2);
	answer_ld_allocations(ep, 2, 0, 4);
	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x5107);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	host = sw ? vcs_host(ctx, sw_id, 2) : NULL;
	snprintf(dev, sizeof(dev), "%.32s_port5_dev", sw_id);
	snprintf(dom, sizeof(dom), "%s_domain0", dev);
	snprintf(chunk, sizeof(chunk), "%s_ld0_chunk", dev);
	CHECK(sw && host && fmd_topology_find_chunk(&ctx->topology, chunk),
	      "no switch, host of VCS 2 or chunk %s", chunk);
	if (!sw || !host || !fmd_topology_find_chunk(&ctx->topology, chunk))
		goto out;

	/* bind LD 0 to vPPB 1 of VCS 2, in the background */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	snprintf(body, sizeof(body),
		 "{\"Links\": {\"InitiatorEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%s\"}], "
		 "\"TargetEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%s_ld0\"}]}}",
		 host->base.id, dev);
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	task_id = resp ? atoi(json_string_value(json_object_get(resp, "Id"))) : -1;
	json_decref(resp);
	resp = NULL;
	CHECK(rc == FMD_OP_STARTED, "POST of LD 0: %d", rc);
	snprintf(cid, sizeof(cid), "%s_vcs2_vppb1_p5_ld0", sw_id);

	/* Starting: the chunk is already the Connection's */
	rc = fmd_redfish_delete_memory_chunk(ctx, dev, dom, chunk);
	CHECK(rc == -5 && fmd_topology_find_chunk(&ctx->topology, chunk),
	      "chunk DELETE while the bind is under way: %d", rc);

	answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_SUCCESS);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld0, unbound);
	fmd_tasks_poll(ctx);
	CHECK(conn_state(ctx, cid) == FMD_CONN_ENABLED, "%s: %d (task %d)", cid,
	      conn_state(ctx, cid), task_id);

	/* bound: DELETE and POST on LD 0 are 409, with nothing sent */
	rc = fmd_redfish_delete_memory_chunk(ctx, dev, dom, chunk);
	op = last_opcode(ep);
	CHECK(rc == -5 && op != 0x5401 && op != 0x5402 &&
	      fmd_topology_find_chunk(&ctx->topology, chunk),
	      "chunk DELETE of a bound LD: %d, last %04xh", rc, op);
	snprintf(body, sizeof(body),
		 "{\"MemoryChunkSizeMiB\": 256, \"InterleaveSets\": [{\"Memory\": "
		 "{\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld0\"}}]}", dev, dev);
	rc = fmd_redfish_post_memory_chunk(ctx, dev, dom, body, strlen(body), &resp);
	op = last_opcode(ep);
	CHECK(rc == -7 && !resp && op != 0x5401 && op != 0x5402,
	      "chunk POST on a bound LD: %d, last %04xh", rc, op);
	json_decref(resp);
	resp = NULL;

	/* unbound: the DELETE sets LD 0's allocation */
	answer_rc(ep, 0x5202, CXLMI_RET_SUCCESS);
	rc = fmd_redfish_delete_connection(ctx, "CXL", cid, &resp);
	json_decref(resp);
	resp = NULL;
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_vcs_refresh(ctx, sw);
	CHECK(rc == 0 && conn_state(ctx, cid) < 0, "DELETE of %s: %d, then %d", cid,
	      rc, conn_state(ctx, cid));

	alloc[0] = 2;				/* Number of LDs */
	alloc[3] = 1;				/* LD Allocation List Length */
	put64(alloc + 4, 4);			/* Range 1 Allocation Multiplier */
	answer(ep, 0x5401, alloc, sizeof(alloc));
	set[0] = 1;				/* Number of LDs */
	answer(ep, 0x5402, set, sizeof(set));
	rc = fmd_redfish_delete_memory_chunk(ctx, dev, dom, chunk);
	CHECK(rc == 0 && last_opcode(ep) == 0x5402 &&
	      !fmd_topology_find_chunk(&ctx->topology, chunk),
	      "chunk DELETE after the unbind: %d, last %04xh", rc, last_opcode(ep));
out:
	fmd_ctx_free(ctx);
	report("no chunk of a bound LD is changed", before);
}

/*
 * A Multi-Headed device on its own endpoint, which answers Get
 * Multi-Headed Info (5500h) with no tunnel: 2 heads, 4 pool LDs of 1, 2, 3
 * and 4 GiB, LD 0 and 1 mapped to head 0 (port 1), LD 2 to head 1 (port
 * 2), LD 3 to none.
 */
/*
 * An MHD of 4 pool LDs of 1 to 4 GiB, whose LD Pool CCI answers Get LD
 * Info with Memory Size @memory_size, or not at all if it is 0.
 */
static struct cxlmi_endpoint *new_mhd_sized(struct fmd_ctx *ctx, uint64_t serial,
					    uint64_t memory_size)
{
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t probe[8 + 1] = { 0 }, info[8 + 4] = { 0 };
	uint8_t heads[4 + 2 * 9] = { 0 };

	answer_identify(ep, serial, 0x03);
	probe[0] = 4; probe[1] = 2; probe[5] = 1;
	answer(ep, 0x5500, probe, sizeof(probe));
	info[0] = 4; info[1] = 2; info[5] = 4;
	info[8] = 1; info[9] = 1; info[10] = 2; info[11] = 0xff;
	answer(ep, 0x5500, info, sizeof(info));
	for (int i = 0; i < 4; i++)
		answer_memdev_identify(ep, 4 * (i + 1), 4 * (i + 1), 0);
	heads[0] = 2; heads[4] = 1; heads[4 + 6] = 4; heads[4 + 9] = 2;
	answer(ep, 0x5501, heads, sizeof(heads));
	if (memory_size)
		answer_ld_info(ep, memory_size, 4);
	return ep;
}

static struct cxlmi_endpoint *new_mhd(struct fmd_ctx *ctx, uint64_t serial)
{
	return new_mhd_sized(ctx, serial, 0);
}

/*
 * Chunk POST plan (review F02). Get LD Allocations (5401h) and Set LD
 * Allocations (5402h) answers for one LD: Range 1 @r1 units of 256 MB.
 */
static void answer_get_ld(struct cxlmi_endpoint *ep, uint8_t ld, uint64_t r1)
{
	uint8_t pl[4 + 16] = { 2, 0, ld, 1 };

	put64(pl + 4, r1);
	answer(ep, 0x5401, pl, sizeof(pl));
}

static void answer_set_ld(struct cxlmi_endpoint *ep, uint8_t ld, uint64_t r1)
{
	uint8_t pl[4 + 16] = { 1, ld };

	put64(pl + 4, r1);
	answer(ep, 0x5402, pl, sizeof(pl));
}

/* the @odata.id of member @key of @obj, or of its first array entry */
static const char *odata_of(json_t *obj, const char *key)
{
	json_t *v = json_object_get(obj, key);

	if (json_is_array(v))
		v = json_array_get(v, 0);
	return json_string_value(json_object_get(v, "@odata.id"));
}

/*
 * A chunk POST on an MLD or a Multi-Headed device names exactly one of
 * its LDs, by the URI of its Memory or of its CXLLogicalDevice, and asks
 * for nothing Set LD Allocations cannot do; anything else is refused
 * before anything is made or sent. Before, each of these was 201: the
 * ones naming no LD with nothing sent (rc 0 kept when no LD was found),
 * the canonical Memory URI of a device with a serial-derived Id too, as
 * it was cut to 63 characters; a forged URI ending in "_ld1" grew LD 1,
 * two entries grew LD 0 by both, and OffsetMiB, RegionId and MemoryLevel
 * were echoed though not sent. A DELETE took off InterleaveSets[0].
 * SizeMiB, not the size the POST added.
 */
static void test_chunk_plan(void)
{
	static const char *const refused[][2] = {
		/* rc, body (%1$s the device's Id) */
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld9\"}}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"%1$s_ld0\"}}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/other/Memory/other_ld0\"}}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/not/a/redfish/uri/%1$s_ld1\"}}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0/\"}}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": ["
			 "{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}, \"SizeMiB\": 512},"
			 "{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld1\"}, \"SizeMiB\": 512}]}" },
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "\"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}]}" },
		/* the Memory of LD 0, the CXLLogicalDevice of LD 1 */
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}}], "
			 "\"Links\": {\"CXLLogicalDevices\": [{\"@odata.id\": "
			 "\"/redfish/v1/Chassis/%1$s/PCIeDevices/%1$s/CXLLogicalDevices/1\"}]}}" },
		/* a Memory URI given as a CXLLogicalDevice */
		{ "-11", "{\"MemoryChunkSizeMiB\": 1024, \"Links\": {\"CXLLogicalDevices\": "
			 "[{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}]}}" },
		{ "-12", "{\"MemoryChunkSizeMiB\": 256, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}, \"SizeMiB\": 1024}]}" },
		{ "-12", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}, \"OffsetMiB\": 777}]}" },
		{ "-12", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}, \"RegionId\": \"r9\"}]}" },
		{ "-12", "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}, \"MemoryLevel\": 3}]}" },
		/* not a multiple of the Memory Granularity, 256 MB */
		{ "-6", "{\"MemoryChunkSizeMiB\": 100, \"InterleaveSets\": [{\"Memory\": "
			 "{\"@odata.id\": \"/redfish/v1/Chassis/%1$s/Memory/%1$s_ld0\"}}]}" },
	};
	struct fmd_ctx *ctx = new_ctx();
	/* 2 x 1 GiB, of 10 GiB */
	struct cxlmi_endpoint *ep = new_mld_sized(ctx, 0xf02, 2, 0, 4, 10ULL << 30);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], body[1024], uri[256];
	char chunk_id[FMD_MAX_ID_LEN];
	struct fmd_mem_domain *domain;
	struct fmd_ld *ld0, *ld1;
	uint8_t pl[64];
	int before = failures, rc, chunks;
	uint64_t rem0;
	unsigned int sent;
	json_t *resp = NULL, *j;
	uint16_t op;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf02);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(domain && ld0 && ld1, "no domain %s or LDs", dom);
	if (!domain || !ld0 || !ld1)
		goto out;
	chunks = fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK);

	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		snprintf(body, sizeof(body), refused[i][1], id);
		sent = commands_sent(ep);
		rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
		CHECK(rc == atoi(refused[i][0]) && !resp && commands_sent(ep) == sent &&
		      fmd_component_count_children(&domain->base,
						   FMD_COMPONENT_CHUNK) == chunks &&
		      ld0->memory_size == 1ULL << 30 && ld1->memory_size == 1ULL << 30,
		      "%s: %d, not %s; %u commands sent", body, rc, refused[i][0],
		      commands_sent(ep) - sent);
		json_decref(resp);
		resp = NULL;
	}

	/*
	 * The Memory URI of LD 0, 71 characters: 5402h for LD 0, which goes
	 * from 4 to 8 units; served with the URI whole, and LD 0's link.
	 */
	answer_get_ld(ep, 0, 4);
	answer_set_ld(ep, 0, 8);
	snprintf(uri, sizeof(uri), "/redfish/v1/Chassis/%s/Memory/%s_ld0", id, id);
	snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": "
		 "[{\"Memory\": {\"@odata.id\": \"%s\"}}]}", uri);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == 0 && op == 0x5402 && pl[0] == 1 && pl[1] == 0 && get64(pl + 4) == 8 &&
	      ld0->memory_size == 2ULL << 30,
	      "%zu-character URI of LD 0: %d, %04xh LD %u Range 1 %" PRIu64,
	      strlen(uri), rc, op, pl[1], get64(pl + 4));
	j = json_array_get(json_object_get(resp, "InterleaveSets"), 0);
	CHECK(resp && !strcmp(odata_of(j, "Memory") ? odata_of(j, "Memory") : "", uri),
	      "InterleaveSets[0].Memory is not %s", uri);
	snprintf(uri, sizeof(uri), "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/0",
		 id, id);
	j = json_object_get(resp, "Links");
	CHECK(odata_of(j, "CXLLogicalDevices") &&
	      !strcmp(odata_of(j, "CXLLogicalDevices"), uri), "Links: not LD 0");
	json_decref(resp);
	resp = NULL;

	/*
	 * LD 1 by its CXLLogicalDevice only: 5402h for LD 1. The chunk as
	 * served is LD 1's, which links it (it said LD 0 before), and the
	 * DELETE gives back what the POST added, no more.
	 */
	rem0 = domain->remaining;
	answer_get_ld(ep, 1, 4);
	answer_set_ld(ep, 1, 5);
	snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 256, \"Links\": "
		 "{\"CXLLogicalDevices\": [{\"@odata.id\": "
		 "\"/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/1\"}]}}", id, id);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == 0 && op == 0x5402 && pl[1] == 1 && get64(pl + 4) == 5 &&
	      domain->remaining == rem0 - (256ULL << 20),
	      "LD 1 by its CXLLogicalDevice: %d, %04xh LD %u Range 1 %" PRIu64,
	      rc, op, pl[1], get64(pl + 4));
	snprintf(chunk_id, sizeof(chunk_id), "%s",
		 resp ? json_string_value(json_object_get(resp, "Id")) : "");
	json_decref(resp);
	resp = fmd_redfish_memory_chunk(ctx, id, dom, chunk_id);
	snprintf(uri, sizeof(uri), "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/1",
		 id, id);
	j = json_object_get(resp, "Links");
	CHECK(odata_of(j, "CXLLogicalDevices") &&
	      !strcmp(odata_of(j, "CXLLogicalDevices"), uri),
	      "GET %s: links %s", chunk_id,
	      odata_of(j, "CXLLogicalDevices") ? odata_of(j, "CXLLogicalDevices") : "none");
	json_decref(resp);
	resp = fmd_redfish_cxl_logical_device(ctx, id, id, "1");
	j = json_object_get(json_object_get(resp, "Links"), "MemoryChunks");
	{
		bool found = false;
		size_t k;
		json_t *m;

		json_array_foreach(j, k, m)
			found |= strstr(json_string_value(json_object_get(m, "@odata.id")),
					chunk_id) != NULL;
		CHECK(found, "LD 1 does not link %s", chunk_id);
	}
	json_decref(resp);
	resp = NULL;

	answer_get_ld(ep, 1, 5);
	answer_set_ld(ep, 1, 4);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk_id);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == 0 && op == 0x5402 && pl[1] == 1 && get64(pl + 4) == 4 &&
	      domain->remaining == rem0 && ld1->memory_size == 1ULL << 30,
	      "DELETE %s: %d, %04xh LD %u Range 1 %" PRIu64, chunk_id, rc, op,
	      pl[1], get64(pl + 4));

	/* a chunk of an MLD with no LD gives nothing back: refused, kept */
	{
		char c0[FMD_MAX_ID_LEN + 16];
		struct fmd_mem_chunk *c;

		snprintf(c0, sizeof(c0), "%s_ld0_chunk", id);
		c = fmd_topology_find_chunk(&ctx->topology, c0);
		CHECK(c, "no chunk %s", c0);
		if (c) {
			c->backing_ld = -1;
			rem0 = domain->remaining;
			sent = commands_sent(ep);
			rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, c0);
			CHECK(rc == -6 && commands_sent(ep) == sent &&
			      fmd_topology_find_chunk(&ctx->topology, c0) &&
			      domain->remaining == rem0,
			      "DELETE of a chunk with no LD: %d, %u sent", rc,
			      commands_sent(ep) - sent);
		}
	}
out:
	fmd_ctx_free(ctx);

	/* a Multi-Headed device's pool, 4 LDs, that names none, or LD 40 */
	ctx = new_ctx();
	ep = new_mhd_sized(ctx, 0xe02, 18ULL << 30);
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xe02);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	CHECK(domain, "no domain %s", dom);
	if (domain) {
		for (int i = 0; i < 2; i++) {
			if (i)
				snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024, "
					 "\"InterleaveSets\": [{\"Memory\": {\"@odata.id\": "
					 "\"/redfish/v1/Chassis/%s/Memory/%s_ld40\"}}]}", id, id);
			else
				snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024}");
			sent = commands_sent(ep);
			rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body),
							   &resp);
			CHECK(rc == -11 && !resp && commands_sent(ep) == sent,
			      "MHD %s: %d, %u commands sent", body, rc,
			      commands_sent(ep) - sent);
			json_decref(resp);
			resp = NULL;
		}
	}
	fmd_ctx_free(ctx);
	report("a chunk POST names one LD, and asks for what 5402h can do", before);
}

/*
 * The allocation of a pool LD is changed on the endpoint that answered
 * Get Multi-Headed Info with no tunnel, the LD Pool CCI as discovery takes
 * it, with no tunnel (review F14). It went to that endpoint (or a head's,
 * which is the same one) through a tunnel to the LD Pool CCI (Target Type
 * 1), from the LD Pool CCI. When no LD Pool CCI is known, nothing is sent:
 * 501 for a chunk POST.
 */
static void test_mhd_pool_route(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mhd_sized(ctx, 0x14a, 18ULL << 30);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], body[512];
	struct cxlmi_tunnel_info ti;
	struct fmd_mem_domain *domain;
	struct fmd_mhd *mhd;
	struct fmd_ld *ld1;
	int before = failures, rc, chunks;
	unsigned int sent;
	json_t *resp = NULL;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0x14a);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	mhd = fmd_topology_find_mhd(&ctx->topology, id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(mhd && domain && ld1, "no MHD %s, domain or pool LD 1", id);
	if (!mhd || !domain || !ld1)
		goto out;

	/* Get LD Allocations, which answers for another LD: nothing more */
	answer_get_ld(ep, 3, 4);
	rc = fmd_mhd_set_ld_allocation(ctx, mhd, 0, 2ULL << 30);
	cxlmi_mock_get_last_tunnel(ep, &ti);
	CHECK(rc == -EIO && last_opcode(ep) == 0x5401 && ti.level == 0 && !ti.mhd,
	      "5401h: %d, %04xh, level %d, LD Pool CCI %d", rc, last_opcode(ep),
	      ti.level, ti.mhd);

	/* a chunk of 1 GiB on pool LD 1, 8 to 12 units of 256 MB */
	answer_get_ld(ep, 1, 8);
	answer_set_ld(ep, 1, 12);
	snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": "
		 "[{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld1\"}}]}",
		 id, id);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	cxlmi_mock_get_last_tunnel(ep, &ti);
	CHECK(rc == 0 && last_opcode(ep) == 0x5402 && ti.level == 0 && !ti.mhd &&
	      ld1->memory_size == 3ULL << 30,
	      "chunk POST: %d, %04xh, level %d, LD Pool CCI %d", rc,
	      last_opcode(ep), ti.level, ti.mhd);
	json_decref(resp);
	resp = NULL;

	/* no LD Pool CCI known: nothing sent, 501 */
	mhd->pool_cci = false;
	chunks = fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK);
	sent = commands_sent(ep);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -13 && !resp && commands_sent(ep) == sent &&
	      fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK) == chunks,
	      "chunk POST with no LD Pool CCI: %d, %u sent", rc, commands_sent(ep) - sent);
	json_decref(resp);
	rc = fmd_mhd_set_ld_allocation(ctx, mhd, 1, 4ULL << 30);
	CHECK(rc == -EOPNOTSUPP && commands_sent(ep) == sent && ld1->memory_size == 3ULL << 30,
	      "allocation with no LD Pool CCI: %d, %u sent", rc,
	      commands_sent(ep) - sent);
out:
	fmd_ctx_free(ctx);
	report("a pool LD's allocation is changed on the LD Pool CCI", before);
}

/*
 * A chunk DELETE of a Multi-Headed device gives back its allocation (review
 * F03). It had no branch for an MHD: on hardware the chunk went and its
 * size was counted free with nothing sent, so a later POST of 1 GiB on
 * pool LD 0 asked for 2 GiB, more than the device has. Now Set LD
 * Allocations shrinks the LD by the chunk, on the LD Pool CCI; a failure
 * leaves everything as it was; and no chunk of a pool LD is changed while
 * the head it is mapped to has a host (direct-attached here).
 */
static void test_mhd_chunk_delete(void)
{
	struct fmd_ctx *ctx = new_ctx();
	/* 10 GiB in the pool LDs, of 11 GiB */
	struct cxlmi_endpoint *ep = new_mhd_sized(ctx, 0xf03, 11ULL << 30);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], chunk[FMD_MAX_ID_LEN + 16];
	char body[512];
	struct cxlmi_tunnel_info ti;
	struct fmd_mem_domain *domain;
	struct fmd_head *head0 = NULL;
	struct fmd_component *c;
	struct fmd_host *host;
	struct fmd_mhd *mhd;
	struct fmd_ld *ld0;
	uint8_t pl[64];
	int before = failures, rc;
	unsigned int sent;
	uint64_t rem0;
	json_t *resp = NULL;
	uint16_t op;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf03);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	snprintf(chunk, sizeof(chunk), "%s_ld0_chunk", id);
	mhd = fmd_topology_find_mhd(&ctx->topology, id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	CHECK(mhd && domain && ld0 && fmd_topology_find_chunk(&ctx->topology, chunk),
	      "no MHD %s, domain, pool LD 0 or chunk %s", id, chunk);
	if (!mhd || !domain || !ld0)
		goto out;
	rem0 = domain->remaining;

	/*
	 * Set LD Allocations fails, and the LD read back has what it had:
	 * the chunk, the domain and the LD stay
	 */
	answer_get_ld(ep, 0, 4);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 0, 4);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == -3 && last_opcode(ep) == 0x5401 &&
	      fmd_topology_find_chunk(&ctx->topology, chunk) &&
	      domain->remaining == rem0 && ld0->memory_size == 1ULL << 30,
	      "DELETE answered Internal Error: %d", rc);

	/* LD 0, 4 units of 256 MB, goes to 0, with no tunnel */
	answer_get_ld(ep, 0, 4);
	answer_set_ld(ep, 0, 0);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	last_payload(ep, &op, pl, sizeof(pl));
	cxlmi_mock_get_last_tunnel(ep, &ti);
	CHECK(rc == 0 && op == 0x5402 && pl[1] == 0 && get64(pl + 4) == 0 &&
	      ti.level == 0 && !fmd_topology_find_chunk(&ctx->topology, chunk) &&
	      domain->remaining == rem0 + (1ULL << 30) && ld0->memory_size == 0,
	      "DELETE: %d, %04xh LD %u Range 1 %" PRIu64 ", tunnel level %d", rc,
	      op, pl[1], get64(pl + 4), ti.level);

	/* the 1 GiB is LD 0's again: 0 to 4 units, not 4 to 8 */
	answer_get_ld(ep, 0, 0);
	answer_set_ld(ep, 0, 4);
	snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": "
		 "[{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld0\"}}]}",
		 id, id);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == 0 && op == 0x5402 && get64(pl + 4) == 4 &&
	      domain->remaining == rem0 && ld0->memory_size == 1ULL << 30,
	      "POST after the DELETE: %d, Range 1 %" PRIu64, rc, get64(pl + 4));
	json_decref(resp);
	resp = NULL;

	/* head 0 (pool LDs 0 and 1) attached to a host: 409, nothing sent */
	fmd_for_each_child_type(&mhd->base, c, FMD_COMPONENT_HEAD)
		if (fmd_to_head(c)->head_id == 0)
			head0 = fmd_to_head(c);
	host = fmd_host_create("f03_host0");
	CHECK(head0 && host, "no head 0 or host");
	if (!head0 || !host)
		goto out;
	host->direct_attached = true;
	fmd_topology_add(&ctx->topology, &host->base, NULL);
	snprintf(head0->attached_host_id, sizeof(head0->attached_host_id), "%s",
		 host->base.id);
	snprintf(chunk, sizeof(chunk), "%s_ld1_chunk", id);
	sent = commands_sent(ep);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == -5 && commands_sent(ep) == sent &&
	      fmd_topology_find_chunk(&ctx->topology, chunk),
	      "DELETE of a chunk of head 0: %d, %u sent", rc, commands_sent(ep) - sent);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -7 && !resp && commands_sent(ep) == sent,
	      "POST on a pool LD of head 0: %d, %u sent", rc, commands_sent(ep) - sent);
	json_decref(resp);
	resp = NULL;

	/* LD 3 is mapped to no head: its chunk goes, 16 units to 0 */
	snprintf(chunk, sizeof(chunk), "%s_ld3_chunk", id);
	answer_get_ld(ep, 3, 16);
	answer_set_ld(ep, 3, 0);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == 0 && last_opcode(ep) == 0x5402 &&
	      !fmd_topology_find_chunk(&ctx->topology, chunk),
	      "DELETE of the chunk of LD 3: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("a Multi-Headed device's chunk DELETE gives back its allocation", before);
}

/*
 * A chunk DELETE gives the domain what the LD gave back, as Set LD
 * Allocations answers it (review F03): a device that answers Success with
 * the allocation unchanged, or shrunk by half the chunk, gave the domain
 * the whole chunk before. Now the chunk keeps what was not given back.
 */
static void test_mld_chunk_delete_partial(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld(ctx, 0xf3, 2, 0, 4);	/* 2 x 1 GiB */
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], chunk[FMD_MAX_ID_LEN + 16];
	struct fmd_mem_domain *domain;
	struct fmd_mem_chunk *c;
	struct fmd_ld *ld;
	int before = failures, rc;
	uint64_t rem0;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf3);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	snprintf(chunk, sizeof(chunk), "%s_ld0_chunk", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld = fmd_topology_find_ld(&ctx->topology, id, 0);
	c = fmd_topology_find_chunk(&ctx->topology, chunk);
	CHECK(domain && ld && c, "no domain, LD 0 or chunk %s", chunk);
	if (!domain || !ld || !c)
		goto out;
	rem0 = domain->remaining;

	/* Success, but LD 0 still has 4 units of 256 MB */
	answer_get_ld(ep, 0, 4);
	answer_set_ld(ep, 0, 4);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == -3 && fmd_topology_find_chunk(&ctx->topology, chunk) == c &&
	      c->size == 1ULL << 30 && domain->remaining == rem0,
	      "nothing given back: %d, domain %" PRIu64 " MB more", rc,
	      (domain->remaining - rem0) >> 20);

	/* Success, LD 0 down to 2 units: 512 MB back, the chunk keeps 512 */
	answer_get_ld(ep, 0, 4);
	answer_set_ld(ep, 0, 2);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == -3 && fmd_topology_find_chunk(&ctx->topology, chunk) == c &&
	      c->size == 512ULL << 20 && domain->remaining == rem0 + (512ULL << 20) &&
	      ld->memory_size == 512ULL << 20,
	      "half given back: %d, chunk %" PRIu64 " MB, domain %" PRIu64 " MB more",
	      rc, c->size >> 20, (domain->remaining - rem0) >> 20);

	/* the rest: the chunk goes, and the domain has all of it back */
	answer_get_ld(ep, 0, 2);
	answer_set_ld(ep, 0, 0);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, chunk);
	CHECK(rc == 0 && !fmd_topology_find_chunk(&ctx->topology, chunk) &&
	      domain->remaining == rem0 + (1ULL << 30) && ld->memory_size == 0,
	      "the rest: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("a chunk DELETE gives the domain what the LD gave back", before);
}

/* Oem.Cxl.Origin of Connection @id as served, "" if it has none */
static const char *conn_origin(struct fmd_ctx *ctx, const char *id, char *buf,
			       size_t len)
{
	json_t *c = fmd_redfish_connection(ctx, "CXL", id);
	const char *o = json_string_value(json_object_get(json_object_get(
				json_object_get(c, "Oem"), "Cxl"), "Origin"));

	snprintf(buf, len, "%s", o ? o : "");
	json_decref(c);
	return buf;
}

/*
 * ForceUnbind sends Unbind vPPB with Unbind Option 2h, Simulate Surprise
 * Hot-Remove (Table 7-36), on its own or while a DELETE's managed unbind
 * (1h) is under way, which the task of that unbind then follows; a switch
 * still running the first answers Busy, and nothing changes. Origin is
 * "Service" for a binding this run made and "Discovered" for one it found.
 */
static void test_force_unbind(void)
{
	static const uint8_t ld0[3] = { 0x03, 5, 0 }, ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_device(ctx);
	char sw_id[FMD_MAX_ID_LEN], body[512], c0[FMD_CONN_ID_LEN], c1[FMD_CONN_ID_LEN];
	char origin[32];
	struct fmd_switch *sw;
	struct fmd_host *host;
	struct fmd_task *task;
	json_t *resp = NULL, *c;
	int before = failures, rc, task_id;
	uint8_t pl[16];
	uint16_t op;

	/* LD 1 of the MLD on port 5 is bound to vPPB 2 of VCS 2 already */
	answer_identify(ep, 0x5108, 0x00);
	answer_switch_ports(ep, 6, 0x00, 0x00);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, ld1);
	answer_ld_info(ep, 2ULL * 4 << 28, 2);
	answer_ld_allocations(ep, 2, 0, 4);
	fmd_discover(ctx);
	device_id(sw_id, sizeof(sw_id), 0x5108);
	sw = fmd_topology_find_switch(&ctx->topology, sw_id);
	host = sw ? vcs_host(ctx, sw_id, 2) : NULL;
	CHECK(sw && host, "no switch or host of VCS 2");
	if (!sw || !host)
		goto out;
	snprintf(c0, sizeof(c0), "%s_vcs2_vppb1_p5_ld0", sw_id);
	snprintf(c1, sizeof(c1), "%s_vcs2_vppb2_p5_ld1", sw_id);

	/* found bound: Discovered, and the action is offered */
	c = fmd_redfish_connection(ctx, "CXL", c1);
	CHECK(c && !strcmp(conn_origin(ctx, c1, origin, sizeof(origin)), "Discovered") &&
	      strstr(json_string_value(json_object_get(json_object_get(json_object_get(
			json_object_get(c, "Actions"), "Oem"), "#Cxl.ForceUnbind"), "target")),
		     "/Connections/") &&
	      strstr(json_string_value(json_object_get(json_object_get(json_object_get(
			json_object_get(c, "Actions"), "Oem"), "#Cxl.ForceUnbind"), "target")),
		     "/Actions/Oem/Cxl.ForceUnbind"),
	      "%s as served: Origin %s", c1, origin);
	json_decref(c);

	/* LD 0, bound by cxl-fmd in the background: Service once it is done */
	answer_rc(ep, 0x5201, CXLMI_RET_BACKGROUND);
	snprintf(body, sizeof(body),
		 "{\"Links\": {\"InitiatorEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%s\"}], "
		 "\"TargetEndpoints\": [{\"@odata.id\": \"/redfish/v1/Fabrics/CXL/Endpoints/%.32s_port5_dev_ld0\"}]}}",
		 host->base.id, sw_id);
	rc = fmd_redfish_post_connection(ctx, "CXL", body, strlen(body), &resp);
	json_decref(resp);
	resp = NULL;
	answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_SUCCESS);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld0, ld1);
	fmd_tasks_poll(ctx);
	CHECK(rc == FMD_OP_STARTED && conn_state(ctx, c0) == FMD_CONN_ENABLED &&
	      !strcmp(conn_origin(ctx, c0, origin, sizeof(origin)), "Service") &&
	      !strcmp(conn_origin(ctx, c1, origin, sizeof(origin)), "Discovered"),
	      "after the bind: %d, %d, Origin %s", rc, conn_state(ctx, c0), origin);

	/*
	 * ForceUnbind of the discovered one: 5202h, option 2h. A success is
	 * followed by a 5200h, so the payload is looked at on a failure.
	 */
	answer_rc(ep, 0x5202, CXLMI_RET_INTERNAL);
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c1, &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == -2 && op == 0x5202 && pl[0] == 2 && pl[1] == 2 && pl[2] == 0x02 &&
	      !ops_count(ctx),
	      "ForceUnbind: %d, %04xh VCS %d vPPB %d option %d", rc, op, pl[0], pl[1], pl[2]);
	answer_rc(ep, 0x5202, CXLMI_RET_SUCCESS);
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c1, &resp);
	CHECK(rc == 0 && !resp && !ops_count(ctx), "ForceUnbind: %d", rc);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld0, unbound);
	fmd_vcs_refresh(ctx, sw);

	/* DELETE of LD 0 goes on in the background, option 1h: Updating */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_redfish_delete_connection(ctx, "CXL", c0, &resp);
	task_id = resp ? atoi(json_string_value(json_object_get(resp, "Id"))) : -1;
	json_decref(resp);
	resp = NULL;
	CHECK(rc == FMD_OP_STARTED && conn_state(ctx, c0) == FMD_CONN_UPDATING &&
	      ops_count(ctx) == 1 && ctx->binding_ops->unbind_option == 1,
	      "DELETE in the background: %d, %d", rc, conn_state(ctx, c0));

	/* forced meanwhile: Busy, 503, and the managed unbind stands */
	answer_rc(ep, 0x5202, CXLMI_RET_BUSY);
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c0, &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == -3 && !resp && op == 0x5202 && pl[2] == 0x02 && ops_count(ctx) == 1 &&
	      ctx->binding_ops->unbind_option == 1,
	      "ForceUnbind while the switch is busy: %d, option %d", rc, pl[2]);

	/* taken: the same task, which the vPPB unbound ends */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c0, &resp);
	CHECK(rc == FMD_OP_STARTED && resp &&
	      atoi(json_string_value(json_object_get(resp, "Id"))) == task_id &&
	      ops_count(ctx) == 1 && ctx->binding_ops->unbind_option == 2,
	      "ForceUnbind of a managed unbind: %d", rc);
	json_decref(resp);
	resp = NULL;

	/* forced already: 503, nothing sent */
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c0, &resp);
	CHECK(rc == -3, "ForceUnbind twice: %d", rc);

	task = fmd_task_find(ctx, task_id);
	answer_bg_status(ep, false, 100, 0x5202, CXLMI_RET_SUCCESS);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_tasks_poll(ctx);
	CHECK(task && task->state == FMD_TASK_COMPLETED && !ops_count(ctx) &&
	      conn_state(ctx, c0) < 0,
	      "after the forced unbind: task %d, Connection %d",
	      task ? (int)task->state : -1, conn_state(ctx, c0));

	/* a stale ID: nothing to force */
	rc = fmd_redfish_action_force_unbind(ctx, "CXL", c0, &resp);
	CHECK(rc == -1, "ForceUnbind of an unbound vPPB: %d", rc);
out:
	fmd_ctx_free(ctx);
	report("ForceUnbind is Unbind vPPB option 2h, and Origin", before);
}

/* the domain of device @serial, and how many chunks it has */
static struct fmd_mem_domain *dev_domain(struct fmd_ctx *ctx, uint64_t serial,
					 int *chunks)
{
	char id[FMD_MAX_ID_LEN], did[FMD_MAX_ID_LEN + 16];
	struct fmd_mem_domain *d;

	device_id(id, sizeof(id), serial);
	snprintf(did, sizeof(did), "%s_domain0", id);
	d = fmd_topology_find_domain(&ctx->topology, did);
	*chunks = d ? fmd_component_count_children(&d->base, FMD_COMPONENT_CHUNK) : -1;
	return d;
}

/*
 * A MemoryDomain of an MLD or MHD is the device's Memory Size, "Total
 * device memory capacity" (Get LD Info, Table 7-46), and what its LDs have
 * not been allocated of it is left (review F01). It was the sum of the
 * allocations, all of it taken: an MLD of 128 GiB with 2 LDs of 16 GiB had
 * a domain of 32 GiB with nothing left, and one with nothing allocated had
 * no domain at all.
 */
static void test_pool_capacity(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct fmd_mem_domain *d;
	int before = failures, chunks;
	uint8_t pl[4 + 2 * 16] = { 2, 2, 0, 1 };

	/* 128 GiB, 2 LDs enabled of 4, 16 GiB each (16 units of 1 GB) */
	new_mld_sized(ctx, 0xf01, 2, 2, 16, 128ULL << 30);
	/* 128 GiB, 4 LDs, nothing allocated */
	new_mld_sized(ctx, 0xf02, 4, 2, 0, 128ULL << 30);
	/* 1 GiB of Memory Size, less than the 32 GiB its LDs have */
	new_mld_sized(ctx, 0xf03, 2, 2, 16, 1ULL << 30);
	/* MHDs of 10 GiB in pool LDs, with Get LD Info of 64 GiB, or none */
	new_mhd_sized(ctx, 0xf04, 64ULL << 30);
	new_mhd(ctx, 0xf05);
	fmd_discover(ctx);

	d = dev_domain(ctx, 0xf01, &chunks);
	CHECK(d && d->capacity == 128ULL << 30 && d->remaining == 96ULL << 30 &&
	      chunks == 2, "128 GiB, 32 GiB allocated: capacity %" PRIu64
	      " MiB, remaining %" PRIu64 " MiB, %d chunks",
	      d ? d->capacity >> 20 : 0, d ? d->remaining >> 20 : 0, chunks);
	d = dev_domain(ctx, 0xf02, &chunks);
	CHECK(d && d->capacity == 128ULL << 30 && d->remaining == 128ULL << 30 &&
	      chunks == 0, "128 GiB, nothing allocated: %s, remaining %" PRIu64
	      " MiB, %d chunks", d ? "a domain" : "no domain",
	      d ? d->remaining >> 20 : 0, chunks);
	d = dev_domain(ctx, 0xf03, &chunks);
	CHECK(d && d->capacity == 32ULL << 30 && d->remaining == 0 && chunks == 2,
	      "Memory Size below the allocations: capacity %" PRIu64 " MiB, "
	      "remaining %" PRIu64 " MiB", d ? d->capacity >> 20 : 0,
	      d ? d->remaining >> 20 : 0);
	d = dev_domain(ctx, 0xf04, &chunks);
	CHECK(d && d->capacity == 64ULL << 30 && d->remaining == 54ULL << 30 &&
	      chunks == 4, "MHD with Get LD Info: capacity %" PRIu64 " MiB, "
	      "remaining %" PRIu64 " MiB", d ? d->capacity >> 20 : 0,
	      d ? d->remaining >> 20 : 0);
	d = dev_domain(ctx, 0xf05, &chunks);
	CHECK(d && d->capacity == 10ULL << 30 && d->remaining == 0 && chunks == 4,
	      "MHD with no Get LD Info: capacity %" PRIu64 " MiB, remaining %"
	      PRIu64 " MiB", d ? d->capacity >> 20 : 0, d ? d->remaining >> 20 : 0);
	fmd_ctx_free(ctx);

	/*
	 * Allocation multipliers past 64 bits, as a sum of the two ranges or
	 * times the granularity: LD 0 of Range 1 FFFF..FFh and Range 2 2,
	 * LD 1 of 2^34 + 1 units of 1 GB. Each wrapped to 1 GiB.
	 */
	ctx = new_ctx();
	{
		struct cxlmi_endpoint *ep = new_device(ctx);
		struct fmd_ld *ld;
		char id[FMD_MAX_ID_LEN];

		answer_identify(ep, 0xf06, 0x03);
		answer_ld_info(ep, 128ULL << 30, 2);
		answer_ld_info(ep, 128ULL << 30, 2);
		put64(pl + 4, UINT64_MAX);
		put64(pl + 12, 2);
		answer(ep, 0x5401, pl, 4 + 16);
		pl[2] = 1;
		put64(pl + 4, (1ULL << 34) + 1);
		put64(pl + 12, 0);
		answer(ep, 0x5401, pl, 4 + 16);
		fmd_discover(ctx);
		device_id(id, sizeof(id), 0xf06);
		for (int i = 0; i < 2; i++) {
			ld = fmd_topology_find_ld(&ctx->topology, id, i);
			CHECK(ld && ld->memory_size == 0, "LD %d past 64 bits: %" PRIu64
			      " bytes", i, ld ? ld->memory_size : 0);
		}
		d = dev_domain(ctx, 0xf06, &chunks);
		CHECK(d && d->remaining == 128ULL << 30 && chunks == 0,
		      "domain of LDs past 64 bits: remaining %" PRIu64 " MiB, %d chunks",
		      d ? d->remaining >> 20 : 0, chunks);
	}
	fmd_ctx_free(ctx);
	report("a domain is the device's Memory Size, less what its LDs have", before);
}

/* the sum of the chunks of LD @ld of @domain, and how many there are */
static uint64_t ld_chunks(struct fmd_mem_domain *domain, int ld, int *n)
{
	struct fmd_component *c;
	uint64_t sum = 0;

	*n = 0;
	fmd_for_each_child_type(&domain->base, c, FMD_COMPONENT_CHUNK) {
		if (fmd_to_chunk(c)->backing_ld == ld) {
			sum += fmd_to_chunk(c)->size;
			(*n)++;
		}
	}
	return sum;
}

/*
 * Get LD Allocations (5401h) for LDs 0 to @nr - 1, all in one response:
 * LD i of Range 1 @units[i] of 256 MB.
 */
static void answer_lds(struct cxlmi_endpoint *ep, uint8_t nr, const uint64_t *units)
{
	uint8_t pl[4 + 16 * 16] = { nr, 0, 0, nr };

	for (int i = 0; i < nr; i++)
		put64(pl + 4 + 16 * i, units[i]);
	answer(ep, 0x5401, pl, 4 + 16 * nr);
}

/*
 * Allocations changed by others than cxl-fmd are read again (review F01):
 * on a rescan, Get LD Info and Get LD Allocations for every MLD and MHD
 * there was, and before a chunk POST or DELETE, Get LD Allocations for
 * its LD. Nothing re-read them: the LDs and the domain kept what discovery
 * found until a restart, and a POST asked Set LD Allocations for the LD's
 * old size plus the chunk, which took from the LD what it had been given.
 */
static void test_ld_alloc_refresh(void)
{
	struct fmd_ctx *ctx = new_ctx();
	/* 2 LDs of 1 GiB, of 10 GiB */
	struct cxlmi_endpoint *ep = new_mld_sized(ctx, 0xf61, 2, 0, 4, 10ULL << 30);
	struct cxlmi_endpoint *mep = new_mhd_sized(ctx, 0xf62, 18ULL << 30);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], body[512], c0[FMD_MAX_ID_LEN + 16];
	struct fmd_mem_domain *domain, *mdomain;
	struct fmd_ld *ld0, *ld1;
	int before = failures, rc, n0, n1, chunks;
	uint8_t pl[64];
	uint16_t op;
	json_t *resp = NULL;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf61);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	mdomain = dev_domain(ctx, 0xf62, &chunks);
	CHECK(domain && ld0 && ld1 && mdomain, "no domain %s or LDs", dom);
	if (!domain || !ld0 || !ld1 || !mdomain)
		goto out;

	/* a rescan the device does not answer: nothing changes */
	rc = fmd_rescan(ctx);
	CHECK(rc == 0 && ld0->memory_size == 1ULL << 30 && ld1->memory_size == 1ULL << 30 &&
	      domain->capacity == 10ULL << 30 && domain->remaining == 8ULL << 30,
	      "rescan with no answers: %d, LD 0 %" PRIu64 " MiB, remaining %" PRIu64 " MiB",
	      rc, ld0->memory_size >> 20, domain->remaining >> 20);

	/* another FM gave LD 0 2 GiB, took LD 1's, and the device is 12 GiB */
	answer_ld_info(ep, 12ULL << 30, 2);
	answer_lds(ep, 2, (const uint64_t[]){ 8, 0 });
	/* and the MHD's pool LD 3 went from 4 GiB to 1 GiB, of 20 GiB */
	answer_ld_info(mep, 20ULL << 30, 4);
	answer_lds(mep, 4, (const uint64_t[]){ 4, 8, 12, 4 });
	rc = fmd_rescan(ctx);
	CHECK(rc == 0 && ld0->memory_size == 2ULL << 30 && ld1->memory_size == 0,
	      "rescan: %d, LD 0 %" PRIu64 " MiB, LD 1 %" PRIu64 " MiB", rc,
	      ld0->memory_size >> 20, ld1->memory_size >> 20);
	CHECK(domain->capacity == 12ULL << 30 && domain->remaining == 10ULL << 30,
	      "domain after the rescan: %" PRIu64 " MiB, %" PRIu64 " MiB left",
	      domain->capacity >> 20, domain->remaining >> 20);
	CHECK(ld_chunks(domain, 0, &n0) == 2ULL << 30 && ld_chunks(domain, 1, &n1) == 0 &&
	      n0 == 2 && n1 == 0, "chunks of LD 0: %d, of LD 1: %d", n0, n1);
	snprintf(c0, sizeof(c0), "%s_ld1_chunk", id);
	CHECK(!fmd_topology_find_chunk(&ctx->topology, c0), "%s is still there", c0);
	CHECK(mdomain->capacity == 20ULL << 30 && mdomain->remaining == 13ULL << 30 &&
	      ld_chunks(mdomain, 3, &n0) == 1ULL << 30,
	      "MHD domain after the rescan: %" PRIu64 " MiB, %" PRIu64 " MiB left",
	      mdomain->capacity >> 20, mdomain->remaining >> 20);

	/*
	 * LD 1 given 3 GiB by another FM since: a POST of 1 GiB on it asks
	 * for 4 GiB (16 units), not 1 GiB, and the domain counts the 3 GiB.
	 */
	answer_get_ld(ep, 1, 12);
	answer_set_ld(ep, 1, 16);
	snprintf(body, sizeof(body), "{\"MemoryChunkSizeMiB\": 1024, \"InterleaveSets\": "
		 "[{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld1\"}}]}",
		 id, id);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	last_payload(ep, &op, pl, sizeof(pl));
	CHECK(rc == 0 && op == 0x5402 && pl[1] == 1 && get64(pl + 4) == 16 &&
	      ld1->memory_size == 4ULL << 30,
	      "POST on LD 1: %d, %04xh Range 1 %" PRIu64 ", LD 1 %" PRIu64 " MiB", rc, op,
	      get64(pl + 4), ld1->memory_size >> 20);
	CHECK(ld_chunks(domain, 1, &n1) == 4ULL << 30 && domain->remaining == 6ULL << 30,
	      "LD 1's chunks %" PRIu64 " MiB, %" PRIu64 " MiB left",
	      ld_chunks(domain, 1, &n1) >> 20, domain->remaining >> 20);
	json_decref(resp);
	resp = NULL;

	/* LD 0 taken by another FM: its chunk is gone, and nothing is set */
	snprintf(c0, sizeof(c0), "%s_ld0_chunk", id);
	answer_get_ld(ep, 0, 0);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, c0);
	CHECK(rc == -1 && last_opcode(ep) == 0x5401 && ld0->memory_size == 0 &&
	      ld_chunks(domain, 0, &n0) == 0 && domain->remaining == 8ULL << 30,
	      "DELETE of %s once LD 0 has nothing: %d, last %04xh", c0, rc,
	      last_opcode(ep));

	/* and the read fails: refused, nothing set, nothing changed */
	answer_rc(ep, 0x5401, CXLMI_RET_INTERNAL);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && !resp && last_opcode(ep) == 0x5401 &&
	      ld1->memory_size == 4ULL << 30 && domain->remaining == 8ULL << 30,
	      "POST when Get LD Allocations fails: %d, last %04xh", rc, last_opcode(ep));
	fmd_ctx_free(ctx);

	/*
	 * 3 LDs of 1 GiB, of 4 GiB, laid out LD 2, LD 1, LD 0: LD 1 loses
	 * its 1 GiB and LD 0 gains 2 GiB, which no free range is as large
	 * as: two chunks, in the 1 GiB LD 1 left and the 1 GiB at the end.
	 */
	ctx = new_ctx();
	ep = new_mld_sized(ctx, 0xf63, 3, 0, 4, 4ULL << 30);
	fmd_discover(ctx);
	domain = dev_domain(ctx, 0xf63, &chunks);
	CHECK(domain && chunks == 3, "no domain of 3 chunks");
	if (!domain)
		goto out;
	answer_ld_info(ep, 4ULL << 30, 3);
	answer_lds(ep, 3, (const uint64_t[]){ 12, 0, 4 });
	fmd_rescan(ctx);
	CHECK(ld_chunks(domain, 0, &n0) == 3ULL << 30 && n0 == 3 &&
	      ld_chunks(domain, 1, &n1) == 0 && domain->remaining == 0,
	      "LD 0 in %d chunks of %" PRIu64 " MiB, %" PRIu64 " MiB left", n0,
	      ld_chunks(domain, 0, &n0) >> 20, domain->remaining >> 20);
out:
	fmd_ctx_free(ctx);
	report("allocations others made are read again", before);
}

/*
 * An outcome of Set LD Allocations (5402h) that is not a refusal is read
 * back with Get LD Allocations before a chunk is rolled back (review F06).
 * The daemon took any error, a response it could not read, or a Success
 * for another LD to mean nothing was done: it removed the new chunk, or
 * kept the deleted one, with no read, though "The allocations provided in
 * the response reflect the state of the LD allocations after the command
 * is processed" (CXL r4.0 Section 7.6.7.4.3) and an error need not mean
 * nothing changed. Each case on an MLD of 2 LDs of 1 GiB, of 10 GiB: a
 * POST of 256 MiB on LD 0 (4 units to 5), a DELETE of its chunk; then a
 * POST and DELETE on a pool LD of an MHD.
 */
static void test_set_ld_read_back(void)
{
	static const char post[] = "{\"MemoryChunkSizeMiB\": 256, \"InterleaveSets\": "
		"[{\"Memory\": {\"@odata.id\": \"/redfish/v1/Chassis/%s/Memory/%s_ld%d\"}}]}";
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld_sized(ctx, 0xf06, 2, 0, 4, 10ULL << 30);
	char id[FMD_MAX_ID_LEN], dom[FMD_MAX_ID_LEN + 16], body[512], cid[FMD_MAX_ID_LEN + 32];
	struct fmd_mem_domain *domain;
	struct fmd_ld *ld0, *ld1;
	int before = failures, rc, chunks, n;
	unsigned int sent;
	uint64_t rem;
	json_t *resp = NULL;

	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf06);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	domain = fmd_topology_find_domain(&ctx->topology, dom);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 0);
	ld1 = fmd_topology_find_ld(&ctx->topology, id, 1);
	CHECK(domain && ld0 && ld1, "no domain %s or LDs", dom);
	if (!domain || !ld0 || !ld1)
		goto out;
	snprintf(body, sizeof(body), post, id, id, 0);

	/* Internal Error, and LD 0 read back as it was: rolled back, 500 */
	chunks = fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK);
	rem = domain->remaining;
	answer_get_ld(ep, 0, 4);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 0, 4);
	sent = commands_sent(ep);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && commands_sent(ep) == sent + 3 && last_opcode(ep) == 0x5401 &&
	      fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK) == chunks &&
	      domain->remaining == rem && ld0->memory_size == 1ULL << 30,
	      "Internal Error, read back as it was: %d, %u sent, last %04xh", rc,
	      commands_sent(ep) - sent, last_opcode(ep));
	json_decref(resp);
	resp = NULL;

	/* Invalid Input: nothing was done, and nothing is read back */
	answer_get_ld(ep, 0, 4);
	answer_rc(ep, 0x5402, CXLMI_RET_INPUT);
	sent = commands_sent(ep);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && commands_sent(ep) == sent + 2 && last_opcode(ep) == 0x5402 &&
	      fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK) == chunks,
	      "Invalid Input: %d, %u sent", rc, commands_sent(ep) - sent);

	/* Internal Error, and LD 0 read back grown: the chunk is made, 201 */
	answer_get_ld(ep, 0, 4);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 0, 5);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == 0 && resp && ld0->memory_size == 5ULL << 28 &&
	      domain->remaining == rem - (256ULL << 20) &&
	      ld_chunks(domain, 0, &n) == 5ULL << 28,
	      "Internal Error, read back grown: %d, LD 0 %" PRIu64 " MiB", rc,
	      ld0->memory_size >> 20);
	snprintf(cid, sizeof(cid), "%s", resp ? json_string_value(json_object_get(resp, "Id")) : "");
	json_decref(resp);
	resp = NULL;

	/* a Success for LD 1, asked for LD 0: read back, as it was */
	answer_get_ld(ep, 0, 5);
	answer_set_ld(ep, 1, 6);
	answer_get_ld(ep, 0, 5);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && last_opcode(ep) == 0x5401 && ld0->memory_size == 5ULL << 28 &&
	      ld_chunks(domain, 0, &n) == 5ULL << 28 && ld1->memory_size == 1ULL << 30,
	      "Success for LD 1: %d, last %04xh, LD 0 %" PRIu64 " MiB", rc,
	      last_opcode(ep), ld0->memory_size >> 20);
	json_decref(resp);
	resp = NULL;

	/* a Success libcxlmi cannot read (2 LDs for 1): read back, grown */
	answer_get_ld(ep, 0, 5);
	cxlmi_mock_set_response(ep, 0x54, 0x02, CXLMI_RET_SUCCESS,
				(uint8_t[4 + 16]){ 2, 0, 0, 0, 6 }, 4 + 16);
	answer_get_ld(ep, 0, 6);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == 0 && ld0->memory_size == 6ULL << 28 &&
	      ld_chunks(domain, 0, &n) == 6ULL << 28,
	      "unreadable Success, read back grown: %d, LD 0 %" PRIu64 " MiB", rc,
	      ld0->memory_size >> 20);
	json_decref(resp);
	resp = NULL;

	/* read back as neither: the domain is what LD 0 has (8 units), 500 */
	answer_get_ld(ep, 0, 6);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 0, 8);
	rem = domain->remaining;
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && ld0->memory_size == 2ULL << 30 &&
	      ld_chunks(domain, 0, &n) == 2ULL << 30 && domain->remaining == rem - (512ULL << 20),
	      "read back as neither: %d, LD 0 %" PRIu64 " MiB, chunks %" PRIu64 " MiB", rc,
	      ld0->memory_size >> 20, ld_chunks(domain, 0, &n) >> 20);
	json_decref(resp);
	resp = NULL;

	/*
	 * Internal Error and no read back: outcome unknown, 500. The chunk
	 * stays, counted allocated, and LD 0 is not known: a POST on LD 1
	 * reads LD 0 first, and changes nothing while it cannot.
	 */
	answer_get_ld(ep, 0, 8);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_rc(ep, 0x5401, CXLMI_RET_INTERNAL);
	chunks = fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK);
	rem = domain->remaining;
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -14 && ld0->alloc_unknown &&
	      fmd_component_count_children(&domain->base, FMD_COMPONENT_CHUNK) == chunks + 1 &&
	      domain->remaining == rem - (256ULL << 20),
	      "no read back: %d, unknown %d", rc, ld0->alloc_unknown);
	json_decref(resp);
	resp = NULL;
	snprintf(body, sizeof(body), post, id, id, 1);
	answer_rc(ep, 0x5401, CXLMI_RET_INTERNAL);
	sent = commands_sent(ep);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == -5 && commands_sent(ep) == sent + 1 && ld0->alloc_unknown &&
	      ld1->memory_size == 1ULL << 30,
	      "POST on LD 1 while LD 0 is not known: %d, %u sent", rc,
	      commands_sent(ep) - sent);
	/* LD 0 read as it was (8 units): its chunk goes, LD 1 grows */
	answer_get_ld(ep, 0, 8);
	answer_get_ld(ep, 1, 4);
	answer_set_ld(ep, 1, 5);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == 0 && !ld0->alloc_unknown && ld_chunks(domain, 0, &n) == 2ULL << 30 &&
	      ld1->memory_size == 5ULL << 28 && domain->remaining == rem - (256ULL << 20),
	      "POST on LD 1 once LD 0 is read: %d, unknown %d, %" PRIu64 " MiB left",
	      rc, ld0->alloc_unknown, domain->remaining >> 20);
	json_decref(resp);
	resp = NULL;

	/* DELETE of the chunk made above: Internal Error, read back shrunk */
	rem = domain->remaining;
	answer_get_ld(ep, 0, 8);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 0, 7);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, cid);
	CHECK(rc == 0 && !fmd_topology_find_chunk(&ctx->topology, cid) &&
	      ld0->memory_size == 7ULL << 28 && domain->remaining == rem + (256ULL << 20),
	      "DELETE, read back shrunk: %d", rc);

	/* DELETE, Internal Error and no read back: kept, 500, LD 0 not known */
	snprintf(cid, sizeof(cid), "%s_ld0_chunk", id);
	rem = domain->remaining;
	answer_get_ld(ep, 0, 7);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_rc(ep, 0x5401, CXLMI_RET_INTERNAL);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, cid);
	CHECK(rc == -8 && fmd_topology_find_chunk(&ctx->topology, cid) &&
	      ld0->alloc_unknown && domain->remaining == rem,
	      "DELETE, no read back: %d", rc);
out:
	fmd_ctx_free(ctx);

	/* an MHD's pool LD 3 (16 units), on its LD Pool CCI: read back grown */
	ctx = new_ctx();
	ep = new_mhd_sized(ctx, 0xf07, 18ULL << 30);
	fmd_discover(ctx);
	device_id(id, sizeof(id), 0xf07);
	snprintf(dom, sizeof(dom), "%s_domain0", id);
	ld0 = fmd_topology_find_ld(&ctx->topology, id, 3);
	snprintf(body, sizeof(body), post, id, id, 3);
	answer_get_ld(ep, 3, 16);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 3, 17);
	rc = fmd_redfish_post_memory_chunk(ctx, id, dom, body, strlen(body), &resp);
	CHECK(rc == 0 && ld0 && ld0->memory_size == 17ULL << 28,
	      "MHD, read back grown: %d", rc);
	snprintf(cid, sizeof(cid), "%s", resp ? json_string_value(json_object_get(resp, "Id")) : "");
	json_decref(resp);
	/* and its DELETE: Internal Error, read back shrunk, 204 */
	answer_get_ld(ep, 3, 17);
	answer_rc(ep, 0x5402, CXLMI_RET_INTERNAL);
	answer_get_ld(ep, 3, 16);
	rc = fmd_redfish_delete_memory_chunk(ctx, id, dom, cid);
	CHECK(rc == 0 && !fmd_topology_find_chunk(&ctx->topology, cid) && ld0 &&
	      ld0->memory_size == 16ULL << 28, "MHD DELETE, read back shrunk: %d", rc);
	fmd_ctx_free(ctx);
	report("an unclear Set LD Allocations is read back", before);
}

/* the build renames main() so that the daemon's own can be linked in */
#undef main

int main(void)
{
	test_sld();
	test_two_mlds();
	test_mld_granularity();
	test_ld_allocation_ranges();
	test_switch();
	test_port_resource();
	test_switch_bind();
	test_switch_unbind();
	test_bind_task(true);
	test_bind_task(false);
	test_bind_busy();
	test_rescan();
	test_switch_events();
	test_mailbox_events_are_the_hosts();
	test_device_removal();
	test_device_logs();
	test_overflow_entry();
	test_device_notifications();
	test_switch_mld_logs();
	test_dc_records();
	test_record_read_again();
	test_dc_refresh_after_failed_read();
	test_dc_records_ld200();
	test_dc_records_counts();
	test_rescan_reads_new_logs();
	test_notification_route();
	test_chunk_delete_partition_refused();
	test_notification_ack();
	test_log_ring();
	test_overcount_switch();
	test_overcount_mld();
	test_mld_ld_counts();
	test_health_mapping();
	test_health_rollup();
	test_health_read_failed();
	test_scan_media();
	test_life_used();
	test_switch_sld_placeholder();
	test_set_partition();
	test_dcd_info();
	test_dc_return_codes();
	test_mib_out_of_range_sends_nothing();
	test_duplicate_id_refused();
	test_dc_reserved_fields();
	test_dc_same_dpa_two_hosts();
	test_dc_region_per_host();
	test_dc_extent_in_decode_tail();
	test_dc_prescriptive_release_payload();
	test_dc_set_region_busy_any_host();
	test_dc_tags_pending();
	test_dc_tags_accepted_and_pending();
	test_dc_tags_unreadable();
	test_dc_ld_size();
	test_dc_tags_validity0();
	test_dc_tags_ld200();
	test_dc_tags_generation_restart();
	test_dc_tags_orphan();
	test_dc_pending_unlisted();
	test_dc_list_stale_keeps_old();
	test_paging_ports();
	test_paging_lds(false);
	test_paging_lds(true);
	test_same_device_twice();
	test_no_serial();
	test_mld_on_switch_and_alone();
	test_mhd(false);
	test_mhd(true);
	test_mhd_past_limits(true);
	test_mhd_past_limits(false);
	test_not_ready();
	test_validate_usp_id();
	test_bound_ld_sentinel();
	test_bound_pid_decode();
	test_disabled_vcs_has_no_host();
	test_untracked_binding_change_refreshes();
	test_lost_task_frees_op();
	test_post_delete_on_a_switch();
	test_chunk_guard();
	test_chunk_plan();
	test_mhd_pool_route();
	test_mhd_chunk_delete();
	test_mld_chunk_delete_partial();
	test_pool_capacity();
	test_ld_alloc_refresh();
	test_set_ld_read_back();
	test_force_unbind();
	test_validate_bindings();
	test_conn_view_prebound_sld_ld();
	test_conn_view_skips_vppb0_disabled_nonmemory_pid();
	test_conn_id_roundtrip_underscored_switch();
	test_conn_view_absent();
	test_conn_view_unresolved_mh_mld();

	printf("%d checks, %d failed, %d expected failures\n", checks,
	       failures, xfails);
	return failures ? 1 : 0;
}
