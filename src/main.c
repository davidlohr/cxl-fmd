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

/* SIGUSR1 in mock mode: pull out the next device of the topology's list */
static volatile sig_atomic_t g_unplug;

static void unplug_handler(int sig)
{
	(void)sig;
	g_unplug++;
}

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
	       "  -c, --check           Check the topology is a valid CXL fabric and exit\n"
	       "  -m, --mctp            Enable MCTP device scanning\n"
	       "  -i, --ioctl           Enable ioctl device scanning\n"
	       "  -e, --event-poll SECS Read the event logs every SECS seconds\n"
	       "                        (default: %d, 0 disables)\n"
	       "  -r, --rescan SECS     Look for new devices every SECS seconds\n"
	       "                        (default: %d, 0 disables)\n"
	       "  -N, --threads N       Serve requests on N threads, 1 to %d\n"
	       "                        (default: %d). Requests that only read\n"
	       "                        run together; a change runs alone.\n"
	       "  -v, --verbose         Increase verbosity (can repeat)\n"
	       "  -h, --help            Show this help\n"
	       "  -V, --version         Show version\n"
	       "\n"
	       "Mock Mode:\n"
	       "  When --mock is specified, cxl-fmd uses simulated devices from\n"
	       "  the JSON topology file instead of scanning for real hardware.\n"
	       "  This is useful for testing and development.\n"
	       "\n",
	       prog, FMD_DEFAULT_PORT, FMD_DEFAULT_EVENT_POLL_SEC,
	       FMD_DEFAULT_RESCAN_SEC,
	       FMD_MAX_HTTP_THREADS, FMD_DEFAULT_HTTP_THREADS);
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
	{
		pthread_rwlockattr_t attr;

		pthread_rwlockattr_init(&attr);
		pthread_rwlockattr_setkind_np(&attr,
			PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
		pthread_rwlock_init(&ctx->fabric_lock, &attr);
		pthread_rwlockattr_destroy(&attr);
	}

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

	fmd_tasks_free(ctx);
	free(ctx->known_eps);
	free(ctx->ioctl_eps);

	/* connections are kept on the context, not in the topology */
	while (ctx->connections)
		fmd_connection_remove(ctx, ctx->connections);

	fmd_topology_cleanup(&ctx->topology);
	pthread_mutex_destroy(&ctx->dcd_lock);
	pthread_rwlock_destroy(&ctx->fabric_lock);

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
		{"check",   no_argument,       NULL, 'c'},
		{"threads", required_argument, NULL, 'N'},
		{"rescan",  required_argument, NULL, 'r'},
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
	unsigned int http_threads = FMD_DEFAULT_HTTP_THREADS;
	unsigned int rescan_sec = FMD_DEFAULT_RESCAN_SEC;
	const char *mock_path = NULL;
	bool check_only = false;

	while ((opt = getopt_long(argc, argv, "p:M:cN:r:mie:vhV", long_opts, NULL)) != -1) {
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
		case 'c':
			check_only = true;
			break;
		case 'r': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (endptr == optarg || *endptr != '\0' || val < 0 ||
			    val > 86400) {
				fprintf(stderr, "Invalid re-scan interval: %s\n", optarg);
				return EXIT_FAILURE;
			}
			rescan_sec = (unsigned int)val;
			break;
		}
		case 'N': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (endptr == optarg || *endptr != '\0' ||
			    val < 1 || val > FMD_MAX_HTTP_THREADS) {
				fprintf(stderr, "Invalid number of threads: %s "
					"(1 to %d)\n", optarg, FMD_MAX_HTTP_THREADS);
				return EXIT_FAILURE;
			}
			http_threads = (unsigned int)val;
			break;
		}
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
	ctx->config.http_threads = http_threads;
	ctx->config.rescan_sec = rescan_sec;
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

	/*
	 * A mock topology that could not be a CXL fabric is refused: nothing
	 * learnt from it would hold on a real one. A discovered topology is
	 * what the hardware said, so there it is only reported.
	 */
	{
		int invalid = fmd_topology_validate(ctx);

		if (invalid && mock_path) {
			fmd_err(ctx, "%s is not a valid CXL topology: %d error(s)",
				mock_path, invalid);
			goto out;
		}
		if (invalid)
			fmd_warn(ctx, "the discovered topology breaks %d rule(s) "
				 "of the CXL specification", invalid);
		if (check_only) {
			if (!invalid)
				printf("%s: valid\n", mock_path ? mock_path : "topology");
			ret = invalid ? EXIT_FAILURE : EXIT_SUCCESS;
			goto out;
		}
	}

	if (!mock_path && use_mctp)
		fmd_events_init(ctx);

	/* Start Redfish server */
	if (fmd_redfish_start(ctx) < 0) {
		fmd_err(ctx, "Failed to start Redfish server");
		goto out;
	}

	fmd_info(ctx, "Redfish API available at http://localhost:%d/redfish/v1",
		 ctx->config.port);

	/*
	 * Main loop: drain the DC event log on a timer and on notification,
	 * and follow the binds and unbinds the switches have under way, more
	 * often while there are any.
	 */
	if (mock_path)
		signal(SIGUSR1, unplug_handler);

	while (g_running && ctx->running) {
		static sig_atomic_t unplugged;

		fmd_events_poll(ctx, fmd_tasks_running(ctx) ? 100 : 1000);
		fmd_tasks_poll(ctx);

		for (; unplugged != g_unplug; unplugged++) {
			if (fmd_mock_unplug_next(ctx) < 0)
				fmd_warn(ctx, "mock: there is no device left to unplug");
		}
	}

	fmd_info(ctx, "Shutting down...");
	ret = EXIT_SUCCESS;

out:
	fmd_ctx_free(ctx);
	return ret;
}
