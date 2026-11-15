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

	new_switch(ctx, 0x5006);
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
		json_decref(port);
	}

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
		json_decref(port);
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

	/* the worse of two is by meaning: 03h (all data lost) over 09h */
	CHECK(fmd_worse_media_status(0x03, 0x09) == 0x03 &&
	      fmd_worse_media_status(0x09, 0x03) == 0x03, "worse of 03h and 09h");
	CHECK(fmd_worse_media_status(0x00, 0x02) == 0x02, "worse of 00h and 02h");

	report("health and media status", before);
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
		answer_rc(ep, 0x5201, CXLMI_RET_BUSY);
		rc = fmd_vppb_bind(ctx, sw, 2, 1, 4, 0xffff);
		CHECK(rc == -EAGAIN, "bind on a busy switch: %d", rc);
	}

	fmd_ctx_free(ctx);
	report("bind on a busy switch", before);
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
	CHECK(fmd_connection_find(ctx, task->connection_id) &&
	      conn->pending == FMD_CONN_SETTLED,
	      "the connection does not stand after an unbind that failed");

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
static uint16_t last_policy_set(struct cxlmi_endpoint *ep)
{
	uint8_t pl[2] = { 0 }, set = 0, cmd = 0;
	size_t size = sizeof(pl);

	cxlmi_mock_get_last_command(ep, &set, &cmd, pl, &size);
	return set == 0x01 && cmd == 0x05 ? (uint16_t)(pl[0] | pl[1] << 8) : 0xffff;
}

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

/* A Dynamic Capacity Event Record (Table 8-229) */
static const uint8_t dc_event[16] = {	/* ca95afa7-f183-4018-... */
	0xca, 0x95, 0xaf, 0xa7, 0xf1, 0x83, 0x40, 0x18,
	0x8c, 0x2f, 0x95, 0x26, 0x8e, 0x10, 0x1a, 0x2a,
};

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
 * refresh starts with Get Host DC Region Configuration (5601h).
 */
static void test_dc_refresh_after_failed_read(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_mld(ctx, 0xe9, 2, 0, 4);
	uint8_t info[0x54] = { 0 }, page[0x20 + 0x80] = { 0 };
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
	fmd_dcd_poll_events(ctx);
	CHECK(last_opcode(ep) == 0x5601,
	      "host 1 was not refreshed: last command %04xh", last_opcode(ep));

	fmd_ctx_free(ctx);
	report("a DC refresh after a failed read", before);
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

static void test_mailbox_events_are_the_hosts(void)
{
	struct fmd_ctx *ctx = new_ctx();
	struct cxlmi_endpoint *ep = new_switch(ctx, 0x500d);
	int before = failures;
	unsigned int sent;

	fmd_discover(ctx);

	/* as if the scan of /dev/cxl had opened it */
	ctx->ioctl_eps = calloc(1, sizeof(*ctx->ioctl_eps));
	if (ctx->ioctl_eps)
		ctx->ioctl_eps[ctx->nioctl_eps++] = ep;
	CHECK(!fmd_endpoint_owns_events(ctx, ep), "a mailbox endpoint owns its events");

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

/* the build renames main() so that the daemon's own can be linked in */
#undef main

int main(void)
{
	test_sld();
	test_two_mlds();
	test_mld_granularity();
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
	test_device_notifications();
	test_switch_mld_logs();
	test_record_read_again();
	test_dc_refresh_after_failed_read();
	test_rescan_reads_new_logs();
	test_log_ring();
	test_overcount_switch();
	test_overcount_mld();
	test_mld_ld_counts();
	test_health_mapping();
	test_dcd_info();
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
