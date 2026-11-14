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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

	pl[0] = nr;				/* Number of LDs: enabled */
	pl[1] = granularity;			/* Memory Granularity */
	pl[2] = 0;				/* Start LD ID */
	pl[3] = nr;				/* LD Allocation List Length */
	for (int i = 0; i < nr; i++)
		put64(pl + 4 + 16 * i, mult);	/* Range 1 Allocation Multiplier */
	answer(ep, 0x5401, pl, 4 + 16 * nr);
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

static void answer_vcs(struct cxlmi_endpoint *ep, uint8_t vcs_id, uint8_t usp,
		       const uint8_t *vppb1, const uint8_t *vppb2)
{
	uint8_t pl[4 + 4 + 3 * 4] = { 0 }, *b = pl + 4;

	pl[0] = 1;				/* Number of VCSs */
	b[0] = vcs_id;				/* Virtual CXL Switch ID */
	b[1] = 0x01;				/* VCS State: enabled */
	b[2] = usp;				/* USP ID */
	b[3] = 3;				/* Number of vPPBs */
	b[4] = 0x02;				/* vPPB 0: bound physical port */
	b[5] = usp;
	b[6] = 0xff;
	memcpy(b + 8, vppb1, 3);
	memcpy(b + 12, vppb2, 3);
	answer(ep, 0x5200, pl, sizeof(pl));
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

static struct cxlmi_endpoint *new_switch_claiming(struct fmd_ctx *ctx,
						  uint64_t serial,
						  uint8_t claimed_ports)
{
	struct cxlmi_endpoint *ep = new_device(ctx);
	uint8_t id[0x49] = { 0 }, ports[4 + 6 * 16] = { 0 };

	answer_identify(ep, serial, 0x00);

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
	port_block(ports + 4 + 4 * 16, 4, 0x03, 0, 0x00, 0x00, 0x0000);
	port_block(ports + 4 + 5 * 16, 5, 0x03, 0x02, 0x05, 0x04, 0x0004);
	ports[4 + 5 * 16 + 0xf] = 1;		/* Supported LD Count: 1 more */
	answer(ep, 0x5101, ports, sizeof(ports));

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
 * 7.2.3 says may be bound: "The Fabric Manager can bind any unused physical
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

	/* a vPPB that is bound is not an unbound vPPB */
	rc = try_bind(ctx, ep, sw, 0, 1, 4, 0xffff, &sent);
	CHECK(rc == -EBUSY && !sent, "bind of a bound vPPB: %d", rc);

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
	CHECK(rc == 0, "bind of LD 1 of port 5: %d", rc);

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
	CHECK(rc == 0, "unbind in VCS 2: %d", rc);
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
	CHECK(rc == 0, "unbind returns %d for Background Command Started",
	      rc);
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

	/* 16 LDs of 1 to 16 units; an entry is 16 bytes, so 15 fit 256 */
	alloc[0] = 16;				/* Number of LDs */
	for (int i = 0; i < 16; i++)
		put64(alloc + 4 + 16 * i, i + 1);
	if (large) {
		alloc[3] = 16;			/* LD Allocation List Length */
		answer(ep, 0x5401, alloc, sizeof(alloc));
	} else {
		alloc[3] = 15;
		answer(ep, 0x5401, alloc, 4 + 15 * 16);
		alloc[2] = 15;			/* Start LD ID */
		alloc[3] = 1;
		put64(alloc + 4, 16);
		answer(ep, 0x5401, alloc, 4 + 16);
	}

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

/* the build renames main() so that the daemon's own can be linked in */
#undef main

int main(void)
{
	test_sld();
	test_two_mlds();
	test_mld_granularity();
	test_switch();
	test_switch_bind();
	test_switch_unbind();
	test_overcount_switch();
	test_overcount_mld();
	test_paging_ports();
	test_paging_lds(false);
	test_paging_lds(true);
	test_same_device_twice();
	test_no_serial();
	test_mld_on_switch_and_alone();
	test_mhd(false);
	test_mhd(true);
	test_not_ready();

	printf("%d checks, %d failed, %d expected failures\n", checks,
	       failures, xfails);
	return failures ? 1 : 0;
}
