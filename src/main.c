/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Main Entry Point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "config.h"
#include "cxl-fmd.h"
#include "mock.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n"
	       "\n"
	       "CXL Fabric Manager Daemon - Redfish API server for CXL devices\n"
	       "\n"
	       "Options:\n"
	       "  -p, --port PORT       HTTP port (default: %d)\n"
	       "  -M, --mock FILE       Use mock topology from JSON file\n"
	       "  -m, --mctp            Enable MCTP device scanning\n"
	       "  -i, --ioctl           Enable ioctl device scanning\n"
	       "  -e, --event-poll SECS Drain the DC event log every SECS seconds\n"
	       "                        (default: %d, 0 disables)\n"
	       "  -v, --verbose         Increase verbosity (can repeat)\n"
	       "  -h, --help            Show this help\n"
	       "  -V, --version         Show version\n"
	       "\n"
	       "Mock Mode:\n"
	       "  When --mock is specified, cxl-fmd uses simulated devices from\n"
	       "  the JSON topology file instead of scanning for real hardware.\n"
	       "  This is useful for testing and development.\n"
	       "\n",
	       prog, FMD_DEFAULT_PORT, FMD_DEFAULT_EVENT_POLL_SEC);
}

static void print_version(void)
{
	printf("%s version %s\n", FMD_NAME, FMD_VERSION);
}

struct fmd_ctx *fmd_ctx_new(void)
{
	struct fmd_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	/* Defaults */
	ctx->config.port = FMD_DEFAULT_PORT;
	ctx->config.scan_mctp = true;
	ctx->config.scan_ioctl = false;
	ctx->log_level = FMD_LOG_INFO;
	ctx->running = true;
	ctx->notify_sd[0] = ctx->notify_sd[1] = -1;
	pthread_mutex_init(&ctx->dcd_lock, NULL);

	/* Initialize topology */
	if (fmd_topology_init(&ctx->topology) < 0) {
		free(ctx);
		return NULL;
	}

	/* Create libcxlmi context */
	ctx->cxlmi = cxlmi_new_ctx(stderr, LOG_WARNING);
	if (!ctx->cxlmi) {
		fmd_topology_cleanup(&ctx->topology);
		free(ctx);
		return NULL;
	}

	return ctx;
}

void fmd_ctx_free(struct fmd_ctx *ctx)
{
	if (!ctx)
		return;

	fmd_redfish_stop(ctx);
	fmd_events_cleanup(ctx);
	fmd_topology_cleanup(&ctx->topology);
	pthread_mutex_destroy(&ctx->dcd_lock);

	if (ctx->mock_fabric)
		fmd_mock_free(ctx->mock_fabric);

	if (ctx->config.mock_topology_path)
		free(ctx->config.mock_topology_path);

	if (ctx->cxlmi)
		cxlmi_free_ctx(ctx->cxlmi);

	free(ctx);
}

int main(int argc, char *argv[])
{
	struct fmd_ctx *ctx = NULL;
	int ret = EXIT_FAILURE;
	int opt;

	static const struct option long_opts[] = {
		{"port",    required_argument, NULL, 'p'},
		{"mock",    required_argument, NULL, 'M'},
		{"mctp",    no_argument,       NULL, 'm'},
		{"ioctl",   no_argument,       NULL, 'i'},
		{"event-poll", required_argument, NULL, 'e'},
		{"verbose", no_argument,       NULL, 'v'},
		{"help",    no_argument,       NULL, 'h'},
		{"version", no_argument,       NULL, 'V'},
		{NULL, 0, NULL, 0}
	};

	int verbosity = 0;
	int port = FMD_DEFAULT_PORT;
	bool use_mctp = false;
	bool use_ioctl = false;
	bool transport_specified = false;
	unsigned int event_poll_sec = FMD_DEFAULT_EVENT_POLL_SEC;
	const char *mock_path = NULL;

	while ((opt = getopt_long(argc, argv, "p:M:mie:vhV", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'p': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (endptr == optarg || *endptr != '\0' ||
			    val <= 0 || val > 65535) {
				fprintf(stderr, "Invalid port: %s\n", optarg);
				return EXIT_FAILURE;
			}
			port = (int)val;
			break;
		}
		case 'M':
			mock_path = optarg;
			break;
		case 'm':
			use_mctp = true;
			transport_specified = true;
			break;
		case 'i':
			use_ioctl = true;
			transport_specified = true;
			break;
		case 'e': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (endptr == optarg || *endptr != '\0' || val < 0) {
				fprintf(stderr, "Invalid event poll interval: %s\n", optarg);
				return EXIT_FAILURE;
			}
			event_poll_sec = (unsigned int)val;
			break;
		}
		case 'v':
			verbosity++;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		case 'V':
			print_version();
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* Default to MCTP if no transport specified */
	if (!transport_specified)
		use_mctp = true;

	/* Create context */
	ctx = fmd_ctx_new();
	if (!ctx) {
		fprintf(stderr, "Failed to create context\n");
		return EXIT_FAILURE;
	}

	ctx->config.port = port;
	ctx->config.scan_mctp = use_mctp;
	ctx->config.scan_ioctl = use_ioctl;
	ctx->config.event_poll_sec = event_poll_sec;
	ctx->config.mock_topology_path = mock_path ? strdup(mock_path) : NULL;

	if (verbosity >= 1)
		ctx->log_level = FMD_LOG_DEBUG;

	/* Setup signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	fmd_info(ctx, "%s version %s starting", FMD_NAME, FMD_VERSION);

	/* Load mock topology or discover real devices */
	if (mock_path) {
		fmd_info(ctx, "Running in MOCK mode");
		if (fmd_mock_load(ctx, mock_path) < 0) {
			fmd_err(ctx, "Failed to load mock topology");
			goto out;
		}
		if (fmd_mock_discover(ctx) < 0) {
			fmd_err(ctx, "Mock discovery failed");
			goto out;
		}
	} else {
		fmd_info(ctx, "Discovering CXL devices...");
		if (fmd_discover(ctx) < 0) {
			fmd_err(ctx, "Discovery failed");
			goto out;
		}
	}

	fmd_info(ctx, "Discovered %d CXL device(s)",
		 ctx->topology.root ? ctx->topology.root->device_count : 0);

	if (!mock_path && use_mctp)
		fmd_events_init(ctx);

	/* Start Redfish server */
	if (fmd_redfish_start(ctx) < 0) {
		fmd_err(ctx, "Failed to start Redfish server");
		goto out;
	}

	fmd_info(ctx, "Redfish API available at http://localhost:%d/redfish/v1",
		 ctx->config.port);

	/* Main loop: drain the DC event log on a timer and on notification */
	while (g_running && ctx->running)
		fmd_events_poll(ctx, 1000);

	fmd_info(ctx, "Shutting down...");
	ret = EXIT_SUCCESS;

out:
	fmd_ctx_free(ctx);
	return ret;
}
