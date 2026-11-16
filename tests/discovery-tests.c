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

/* a directly attached MLD with @nr LDs of @mult allocation units each */
static struct cxlmi_endpoint *new_mld(struct fmd_ctx *ctx, uint64_t serial,
				      uint8_t nr, uint8_t granularity,
				      uint64_t mult)
{
	struct cxlmi_endpoint *ep = new_device(ctx);

	answer_identify(ep, serial, 0x03);
	/* asked twice: to tell an MLD from an SLD, then to discover the MLD */
	answer_ld_info(ep, (uint64_t)nr * mult << 28, nr);
	answer_ld_info(ep, (uint64_t)nr * mult << 28, nr);
	answer_ld_allocations(ep, nr, granularity, mult);

	return ep;
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
	 * no tag list and nothing pending.
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

/*
 * Pending is the host's bit in the Pending Reference Bitmap of the tag,
 * "an LD for which the tagged capacity has been added with no host
 * response yet"; the Reference Bitmap has "an LD that has accepted the
 * capacity" (Table 7-78). Hosts 0 and 1 both list tag dc_tag: with host 0
 * accepted and host 1 pending, only host 1's extent is pending; with both
 * accepted, neither is. FM Holds Reference is the tag's.
 */
static void test_dc_tags_pending(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep;
	struct fmd_component *dev = hw_dcd_mld(ctx, &ep, 0xd8, 2);
	struct dc_ext e = { 1ULL << 30, 1ULL << 28, dc_tag, 0 };
	struct fmd_dc_extent *e0, *e1;
	const struct fmd_dc_tag *t;
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
	CHECK(!e0->pending && e1->pending,
	      "pending: host 0 %d, host 1 %d; only host 1's bit is set",
	      e0->pending, e1->pending);
	CHECK(t->fm_ref && fmd_dc_bm_test(t->ref, 0) && fmd_dc_bm_test(t->pend, 1) &&
	      !fmd_dc_bm_test(t->ref, 1), "the tag's entry is not as the device sent it");

	answer_dc_tags(ep, 2, 1, 0x03, &(struct dc_tag_row){ dc_tag, false,
		       { 0, 1 }, 2, { 0 }, 0 }, 1);
	dc_refresh_host_tags(ctx, ep, dev, 1, &e, 1);
	e1 = fmd_dc_find_extent(dev, 1, 1ULL << 30);
	t = fmd_dc_tag_find(dev, dc_tag);
	CHECK(e1 && !e1->pending && !e0->pending, "accepted by host 1, still pending");
	CHECK(t && !t->fm_ref && fmd_memdev_dcd(dev)->tags.gen == 2,
	      "the tag list was not replaced");
out:
	fmd_ctx_free(ctx);
	report("pending from the Pending Reference Bitmap", before);
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
	static const uint8_t ld1[3] = { 0x03, 5, 1 };
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, succeeds ? 0x5007 : 0x5008);
	int before = failures, rc;
	struct fmd_connection *conn;
	struct fmd_switch *sw;
	struct fmd_task *task;
	char id[FMD_MAX_ID_LEN];

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

	conn = fmd_connection_create(ctx, "host", "device", "chunk", 0);
	CHECK(conn, "no connection");
	if (!conn)
		goto out;
	conn->pending = FMD_CONN_BINDING;
	task = fmd_task_start(ctx, FMD_TASK_BIND, sw, 2, 1, conn->id);
	CHECK(task && task->state == FMD_TASK_RUNNING, "no running task");
	if (!task)
		goto out;

	/* in progress, 40% */
	answer_bg_status(ep, true, 40, 0x5201, 0);
	fmd_tasks_poll(ctx);
	CHECK(last_opcode(ep) == 0x0002, "the switch was not asked: %04xh",
	      last_opcode(ep));
	CHECK(task->state == FMD_TASK_RUNNING && task->percent == 40,
	      "state %d at %d%%", task->state, task->percent);
	CHECK(fmd_connection_find(ctx, conn->id) && !conn->bound,
	      "the connection is made before the bind is over");

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
		conn = fmd_connection_find(ctx, task->connection_id);
		CHECK(conn && conn->bound && conn->pending == FMD_CONN_SETTLED,
		      "the connection is not made after the bind");
	} else {
		/* complete, with the return code of a failure */
		answer_bg_status(ep, false, 100, 0x5201, CXLMI_RET_INTERNAL);
		fmd_tasks_poll(ctx);
		CHECK(task->state == FMD_TASK_EXCEPTION, "state %d: %s",
		      task->state, task->message);
		CHECK(!fmd_connection_find(ctx, task->connection_id),
		      "a connection is left by a bind that failed");
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
	char sw_id[FMD_MAX_ID_LEN], id[FMD_MAX_ID_LEN + 16];
	struct fmd_connection *conn;
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
	conn = fmd_connection_create(ctx, "host", "device", "chunk", 0);
	task = conn ? fmd_task_start(ctx, FMD_TASK_BIND, sw, 2, 1, conn->id) : NULL;
	CHECK(task, "no task");
	if (!task)
		goto out;
	conn->pending = FMD_CONN_BINDING;

	answer_event(ep, virtual_switch_event, 0x12,
		     (uint8_t[6]){ 2, 1, 0x00, 0x03, 5, 1 }, 6);
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, ld1, unbound);
	fmd_switch_events_poll(ctx);
	CHECK(task->state == FMD_TASK_COMPLETED, "task state %d: %s",
	      task->state, task->message);
	CHECK(conn->bound && conn->pending == FMD_CONN_SETTLED,
	      "the connection is not made by the binding change");
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
	task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw, 2, 1, conn->id);
	CHECK(task, "no task");
	if (!task)
		goto out;
	conn->pending = FMD_CONN_UNBINDING;

	answer_no_events(ep, false);
	answer_event(ep, virtual_switch_event, 0x13,
		     (uint8_t[6]){ 2, 1, 0x00, 0x03, 5, 1 }, 6);
	fmd_switch_events_poll(ctx);
	CHECK(task->state == FMD_TASK_EXCEPTION, "task state %d: %s",
	      task->state, task->message);
	/* the record has no return code, and Background Operation Status none */
	CHECK(!strcmp(task->message, "Unbind vPPB failed: the switch logged it "
		      "as failed"), "task message: %s", task->message);
	CHECK(fmd_connection_find(ctx, task->connection_id) &&
	      conn->pending == FMD_CONN_SETTLED,
	      "the connection does not stand after an unbind that failed");

	/*
	 * Another that fails, and Background Operation Status (Table 8-217)
	 * has its return code: 100% complete, Unbind vPPB, Invalid Input.
	 */
	answer_rc(ep, 0x5202, CXLMI_RET_BACKGROUND);
	rc = fmd_vppb_unbind(ctx, sw, 2, 1, FMD_UNBIND_MANAGED_HOT_REMOVE);
	CHECK(rc == FMD_OP_STARTED, "unbind: %d", rc);
	task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw, 2, 1, conn->id);
	CHECK(task, "no task");
	if (!task)
		goto out;
	conn->pending = FMD_CONN_UNBINDING;

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

static struct fmd_connection *bound_connection(struct fmd_ctx *ctx,
					       struct fmd_host *host,
					       const char *target, uint8_t vppb)
{
	struct fmd_connection *conn = fmd_connection_create(ctx, host->base.id,
							    target, "chunk", 0);

	if (conn) {
		conn->vcs_id = host->vcs_id;
		conn->vppb_id = vppb;
		conn->bound = true;
	}
	return conn;
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
	char sld_id[FMD_MAX_ID_LEN + 16], a_id[FMD_MAX_ID_LEN], b_id[FMD_MAX_ID_LEN];
	char c_id[FMD_MAX_ID_LEN], state[32];
	int before = failures, devices, lds, domains, chunks, rc;
	struct fmd_connection *conn;
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
	conn = bound_connection(ctx, host, mld_id, 1);
	CHECK(conn, "no connection");
	if (!conn)
		goto out;
	snprintf(a_id, sizeof(a_id), "%s", conn->id);

	/* and one the switch is still binding, with its task */
	conn = bound_connection(ctx, host, mld_id, 2);
	task = conn ? fmd_task_start(ctx, FMD_TASK_BIND, sw, 2, 2, conn->id) : NULL;
	CHECK(task, "no task");
	if (!task)
		goto out;
	snprintf(b_id, sizeof(b_id), "%s", conn->id);
	conn->bound = false;
	conn->pending = FMD_CONN_BINDING;

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
	CHECK(!fmd_connection_find(ctx, a_id),
	      "a connection whose vPPB the switch unbound is still there");
	CHECK(fmd_connection_find(ctx, b_id) && task->state == FMD_TASK_RUNNING,
	      "a connection being bound was not left to its task");
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_task_binding_changed(ctx, sw, 2, 2, true);
	CHECK(task->state == FMD_TASK_EXCEPTION && !fmd_connection_find(ctx, b_id),
	      "task %d: %s", task->state, task->message);

	/* an SLD is plugged into port 4, and bound to vPPB 1 of VCS 2 */
	port_event(ctx, ep, 0x33, 4, 0x04, 0x04, port4);
	CHECK(fmd_topology_find_sld(&ctx->topology, sld_id), "no SLD on port 4");
	conn = bound_connection(ctx, host, sld_id, 1);
	CHECK(conn, "no connection");
	if (!conn)
		goto out;
	snprintf(c_id, sizeof(c_id), "%s", conn->id);

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
	conn = fmd_connection_find(ctx, c_id);
	CHECK(conn && conn->pending == FMD_CONN_UNBINDING,
	      "the connection does not stand while it is unbound");
	answer_vcs(ep, 0, 0, port2, unbound);
	answer_vcs(ep, 2, 1, unbound, unbound);
	fmd_task_binding_changed(ctx, sw, 2, 1, false);
	CHECK(!fmd_connection_find(ctx, c_id), "the connection is still there");

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
	test_scan_media();
	test_life_used();
	test_switch_sld_placeholder();
	test_set_partition();
	test_dcd_info();
	test_dc_return_codes();
	test_dc_reserved_fields();
	test_dc_same_dpa_two_hosts();
	test_dc_region_per_host();
	test_dc_extent_in_decode_tail();
	test_dc_prescriptive_release_payload();
	test_dc_set_region_busy_any_host();
	test_dc_tags_pending();
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

	printf("%d checks, %d failed, %d expected failures\n", checks,
	       failures, xfails);
	return failures ? 1 : 0;
}
