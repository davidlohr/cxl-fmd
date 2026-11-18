/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Main Entry Point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "cxl-fmd.h"
#include "mock.h"

/*
 * Set by signal handlers, read by the main loop. A signal is taken by any
 * thread that does not block it, an HTTP server thread as well, so these
 * are atomics, which are lock-free and safe in a handler, rather than
 * volatile sig_atomic_t, which is only for a handler on the same thread.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "a signal handler needs lock-free atomics");
static atomic_int g_running = 1;

/* SIGUSR1 in mock mode: pull out the next device of the topology's list */
static atomic_uint g_unplug;

static void unplug_handler(int sig)
{
	(void)sig;
	atomic_fetch_add(&g_unplug, 1);
}

/* SIGUSR2 in mock mode: make the next bind or unbind of the "external" list */
static atomic_uint g_external;

static void external_handler(int sig)
{
	(void)sig;
	atomic_fetch_add(&g_external, 1);
}

static void signal_handler(int sig)
{
	(void)sig;
	atomic_store(&g_running, 0);
}

/* Long options with no short one */
enum {
	OPT_ALLOW_MCTP_RESTRICTED = 256,
	OPT_LOCK_FILE,
	OPT_STATE_DIR,
};

/* Where the lock file is when --lock-file does not say */
#define FMD_LOCK_DIR	"/run/cxl-fmd"
#define FMD_LOCK_NAME	"cxl-fmd.lock"

/*
 * Where the operation journal is when --state-dir does not say: the first
 * directory of $STATE_DIRECTORY (systemd's StateDirectory=), else
 * /var/lib/cxl-fmd, which a host reboot keeps, unlike /run (cxl-fmd's
 * choice). Made by fmd_journal_open() if it is not there.
 */
#define FMD_STATE_DIR	"/var/lib/cxl-fmd"

static void state_default_path(char *path, size_t len)
{
	const char *sd = getenv("STATE_DIRECTORY");

	if (sd && *sd)
		snprintf(path, len, "%.*s", (int)strcspn(sd, ":"), sd);
	else
		snprintf(path, len, "%s", FMD_STATE_DIR);
}

/*
 * The lock file's path when none is given: in the first directory of
 * $RUNTIME_DIRECTORY (systemd's RuntimeDirectory=, a list separated by
 * colons), else in /run/cxl-fmd. The directory is made if it is not there.
 */
static int lock_default_path(char *path, size_t len)
{
	const char *rd = getenv("RUNTIME_DIRECTORY");
	char dir[PATH_MAX];

	if (rd && *rd)
		snprintf(dir, sizeof(dir), "%.*s", (int)strcspn(rd, ":"), rd);
	else
		snprintf(dir, sizeof(dir), "%s", FMD_LOCK_DIR);

	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "cxl-fmd: cannot create %s for the lock file: %s; "
			"give --lock-file PATH\n", dir, strerror(errno));
		return -1;
	}
	if ((size_t)snprintf(path, len, "%s/%s", dir, FMD_LOCK_NAME) >= len) {
		fprintf(stderr, "cxl-fmd: lock file path too long: %s/%s\n",
			dir, FMD_LOCK_NAME);
		return -1;
	}
	return 0;
}

/*
 * One cxl-fmd on a host drives the devices: a second would act on what
 * the first does, and unbind what it bound. Each takes an exclusive flock
 * on the lock file and keeps it until it exits; the file holds the PID of
 * the one that has it. It is never removed: one that was could be locked
 * again by a third, while the second still holds the old one.
 *
 * This fences one host only. Another host, or another Fabric Manager,
 * driving the same switch is not seen (see Known Gaps in Redfish-API.md).
 */
static int lock_take(const char *path)
{
	char holder[32] = "";
	ssize_t n;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "cxl-fmd: cannot open the lock file %s: %s; "
			"give --lock-file PATH\n", path, strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		int err = errno;

		if (err != EWOULDBLOCK) {
			fprintf(stderr, "cxl-fmd: cannot lock %s: %s\n", path,
				strerror(err));
			close(fd);
			return -1;
		}
		n = pread(fd, holder, sizeof(holder) - 1, 0);
		holder[n > 0 ? n : 0] = '\0';
		holder[strcspn(holder, "\n")] = '\0';
		fprintf(stderr, "cxl-fmd: another cxl-fmd%s%s%s holds the lock "
			"file %s: it drives these devices already\n",
			*holder ? " (PID " : "", holder, *holder ? ")" : "", path);
		close(fd);
		return -1;
	}

	n = snprintf(holder, sizeof(holder), "%d\n", (int)getpid());
	if (ftruncate(fd, 0) < 0 || pwrite(fd, holder, n, 0) != n)
		fprintf(stderr, "cxl-fmd: could not write the PID to %s: %s\n",
			path, strerror(errno));
	return fd;
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
	       "      --allow-mctp-restricted\n"
	       "                        Send Set Partition Info, Scan Media and the\n"
	       "                        security commands as MCTP messages, tunnelled\n"
	       "                        ones too. Only when no host drives the devices:\n"
	       "                        CXL r4.0 Table 8-308 note 4. Refused (409)\n"
	       "                        without it.\n"
	       "      --lock-file PATH  Exit if another cxl-fmd holds PATH, else hold\n"
	       "                        it (default: cxl-fmd.lock in $RUNTIME_DIRECTORY,\n"
	       "                        or in %s; none with --mock)\n"
	       "      --state-dir DIR   Keep the operation journal in DIR, so that\n"
	       "                        operations under way survive a restart\n"
	       "                        (default: $STATE_DIRECTORY, or %s;\n"
	       "                        none with --mock unless given)\n"
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
	       FMD_MAX_HTTP_THREADS, FMD_DEFAULT_HTTP_THREADS, FMD_LOCK_DIR,
	       FMD_STATE_DIR);
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

	/* what is under way is not ended: the next start looks at it */
	fmd_tasks_free(ctx);
	fmd_journal_close(ctx);
	free(ctx->known_eps);
	free(ctx->retry_eps);
	free(ctx->ioctl_eps);

	fmd_binding_state_free(ctx);

	fmd_topology_cleanup(&ctx->topology);
	pthread_mutex_destroy(&ctx->dcd_lock);
	pthread_rwlock_destroy(&ctx->fabric_lock);

	if (ctx->mock_fabric)
		fmd_mock_free(ctx->mock_fabric);

	if (ctx->config.mock_topology_path)
		free(ctx->config.mock_topology_path);
	free(ctx->config.state_dir);

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
		{"allow-mctp-restricted", no_argument, NULL, OPT_ALLOW_MCTP_RESTRICTED},
		{"lock-file", required_argument, NULL, OPT_LOCK_FILE},
		{"state-dir", required_argument, NULL, OPT_STATE_DIR},
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
	bool allow_mctp_restricted = false;
	const char *lock_path = NULL;
	char lock_default[PATH_MAX];
	const char *state_dir = NULL;
	char state_default[PATH_MAX];
	int lock_fd = -1;

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
		case OPT_ALLOW_MCTP_RESTRICTED:
			allow_mctp_restricted = true;
			break;
		case OPT_LOCK_FILE:
			if (!*optarg) {
				fprintf(stderr, "Invalid lock file: an empty path\n");
				return EXIT_FAILURE;
			}
			lock_path = optarg;
			break;
		case OPT_STATE_DIR:
			if (!*optarg) {
				fprintf(stderr, "Invalid state directory: an empty path\n");
				return EXIT_FAILURE;
			}
			state_dir = optarg;
			break;
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

	/*
	 * Before anything is sent to a device, discovery included (and so
	 * with --check too). A mock topology drives no device, so it takes
	 * no lock unless one is named: test suites run many at once.
	 */
	if (!lock_path && !mock_path) {
		if (lock_default_path(lock_default, sizeof(lock_default)) < 0)
			return EXIT_FAILURE;
		lock_path = lock_default;
	}
	if (lock_path) {
		lock_fd = lock_take(lock_path);
		if (lock_fd < 0)
			return EXIT_FAILURE;
	}

	/* Create context */
	ctx = fmd_ctx_new();
	if (!ctx) {
		fprintf(stderr, "Failed to create context\n");
		goto out;
	}

	ctx->config.port = port;
	ctx->config.scan_mctp = use_mctp;
	ctx->config.scan_ioctl = use_ioctl;
	ctx->config.allow_mctp_restricted = allow_mctp_restricted;
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
	if (lock_path)
		fmd_info(ctx, "holding the lock file %s", lock_path);
	if (allow_mctp_restricted)
		fmd_warn(ctx, "--allow-mctp-restricted: Set Partition Info, Scan "
			 "Media and the security commands are sent as MCTP "
			 "messages; no host may be driving these devices "
			 "(CXL r4.0 Table 8-308 note 4)");

	/*
	 * The journal, before anything is sent: it is locked, so that two
	 * cxl-fmd cannot share it, and what it holds of an earlier run is
	 * read. A mock topology keeps none unless one is named; --check
	 * changes nothing, and keeps none.
	 */
	if (!state_dir && !mock_path) {
		state_default_path(state_default, sizeof(state_default));
		state_dir = state_default;
	}
	if (state_dir && !check_only) {
		ctx->config.state_dir = strdup(state_dir);
		if (fmd_journal_open(ctx, state_dir) < 0) {
			fmd_err(ctx, "Cannot use the operation journal in %s",
				state_dir);
			goto out;
		}
	}

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

	/*
	 * What the journal holds of an earlier run is made again on what
	 * discovery found, before any request is taken
	 */
	fmd_journal_recover(ctx);

	if (!mock_path && use_mctp)
		fmd_events_init(ctx);

	/*
	 * Read the event logs once now: a log that already has records when
	 * notifications are enabled sends none until it has been emptied, and
	 * the timer, if there is one, is not due for an interval.
	 */
	if (!mock_path) {
		fmd_dcd_poll_events(ctx);
		fmd_switch_events_poll(ctx);
	}

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
	if (mock_path) {
		signal(SIGUSR1, unplug_handler);
		signal(SIGUSR2, external_handler);
	}

	while (atomic_load(&g_running) && ctx->running) {
		static unsigned int unplugged, externals;

		fmd_events_poll(ctx, fmd_tasks_running(ctx) ? 100 : 1000);
		fmd_tasks_poll(ctx);

		for (; unplugged != atomic_load(&g_unplug); unplugged++) {
			if (fmd_mock_unplug_next(ctx) < 0)
				fmd_warn(ctx, "mock: there is no device left to unplug");
		}
		for (; externals != atomic_load(&g_external); externals++) {
			if (fmd_mock_external_next(ctx) < 0)
				fmd_warn(ctx, "mock: there is no external bind or "
					 "unbind left");
		}
	}

	fmd_info(ctx, "Shutting down...");
	ret = EXIT_SUCCESS;

out:
	fmd_ctx_free(ctx);
	/* closing it lets the lock go; the file stays */
	if (lock_fd >= 0)
		close(lock_fd);
	return ret;
}
