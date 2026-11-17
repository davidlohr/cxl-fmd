// SPDX-License-Identifier: LGPL-2.1
/*
 * Minimal init for QEMU CXL Redfish testing
 *
 * Supports two modes:
 * - ioctl mode (default): Tests via /dev/cxl/memX devices
 * - MCTP mode (MCTP_TEST defined): Tests via MCTP over I2C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/reboot.h>
#include <net/if.h>
#include <sys/ioctl.h>

#define CXL_DEV_PATH "/dev/cxl"
#define CXL_FMD_BIN "/bin/cxl-fmd"
#define CURL_BIN "/bin/curl"
#define JQ_BIN "/bin/jq"
#define MAX_WAIT_SECS 30
#define REDFISH_PORT 8080
#define CMDLINE_PATH "/proc/cmdline"

static int verbose = 0;
static pid_t fmd_pid = 0;

#ifdef MCTP_TEST
static int mctp_mode = 0;
static int mctp_net = 11;
static int mctp_local_eid = 50;
static int switch_eid = 8;
static int type3_1_eid = 9;
static int type3_2_eid = 10;
static int switch_i2c_addr = 0x4;
static int type3_1_i2c_addr = 0x5;
static int type3_2_i2c_addr = 0x6;
#endif

static int mount_filesystems(void)
{
	if (mount("proc", "/proc", "proc", 0, NULL) < 0 ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0 ||
	    mount("tmpfs", "/tmp", "tmpfs", 0, NULL) < 0) {
		perror("mount");
		return -1;
	}
	return 0;
}

static void setup_loopback(void)
{
	/* Bring up loopback interface for localhost communication */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
		if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
			ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
			ioctl(fd, SIOCSIFFLAGS, &ifr);
		}
		close(fd);
	}
}

#ifdef MCTP_TEST
static int parse_int_param(const char *buf, const char *param, int min, int max)
{
	const char *p = strstr(buf, param);
	char *endptr;
	long val;

	if (!p)
		return -1;

	p += strlen(param);
	errno = 0;
	val = strtol(p, &endptr, 0);

	if (errno != 0 || endptr == p || val < min || val > max)
		return -1;

	return (int)val;
}
#endif

static void parse_cmdline(void)
{
	FILE *f;
	char buf[2048];
#ifdef MCTP_TEST
	int val;
#endif

	f = fopen(CMDLINE_PATH, "r");
	if (!f)
		return;

	if (fgets(buf, sizeof(buf), f)) {
		if (strstr(buf, "cxlfmd_test.verbose=1"))
			verbose = 1;
#ifdef MCTP_TEST
		if (strstr(buf, "cxlfmd_test.mctp=1"))
			mctp_mode = 1;
		if ((val = parse_int_param(buf, "cxlfmd_test.mctp_net=", 1, 255)) > 0)
			mctp_net = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.switch_eid=", 1, 254)) > 0)
			switch_eid = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.type3_1_eid=", 1, 254)) > 0)
			type3_1_eid = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.type3_2_eid=", 1, 254)) > 0)
			type3_2_eid = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.switch_i2c_addr=", 0x01, 0x7F)) > 0)
			switch_i2c_addr = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.type3_1_i2c_addr=", 0x01, 0x7F)) > 0)
			type3_1_i2c_addr = val;
		if ((val = parse_int_param(buf, "cxlfmd_test.type3_2_i2c_addr=", 0x01, 0x7F)) > 0)
			type3_2_i2c_addr = val;
#endif
	}
	fclose(f);
}

static int wait_for_cxl_devices(void)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	printf("Waiting for CXL devices...\n");

	for (int i = 0; i < MAX_WAIT_SECS * 10; i++) {
		dir = opendir(CXL_DEV_PATH);
		if (dir) {
			count = 0;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "mem", 3) == 0)
					count++;
			}
			closedir(dir);
			if (count > 0) {
				printf("Found %d CXL device(s)\n", count);
				return count;
			}
		}
		usleep(100000);
	}
	return 0;
}

#ifdef MCTP_TEST
static int run_cmd_quiet(const char *cmd)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		_exit(127);
	}

	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_cmd(const char *cmd)
{
	if (verbose)
		printf("Running: %s\n", cmd);
	return run_cmd_quiet(cmd);
}

static int wait_for_mctp_i2c(void)
{
	struct stat st;

	printf("Waiting for MCTP I2C device...\n");

	for (int i = 0; i < MAX_WAIT_SECS * 10; i++) {
		DIR *dir = opendir("/sys/bus/mctp/devices");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "mctpi2c", 7) == 0) {
					closedir(dir);
					printf("Found MCTP device: %s\n", entry->d_name);
					usleep(500000);
					return 0;
				}
			}
			closedir(dir);
		}

		if (stat("/sys/class/net/mctpi2c0", &st) == 0) {
			printf("Found MCTP network interface: mctpi2c0\n");
			usleep(500000);
			return 0;
		}

		usleep(100000);
	}

	printf("Timeout waiting for MCTP I2C device\n");
	return -1;
}

static int assign_mctp_endpoint(int i2c_addr, int expected_eid)
{
	char cmd[512];
	int rc;

	snprintf(cmd, sizeof(cmd),
		 "busctl call au.com.codeconstruct.MCTP1 "
		 "/au/com/codeconstruct/mctp1/interfaces/mctpi2c0 "
		 "au.com.codeconstruct.MCTP.BusOwner1 "
		 "AssignEndpoint ay 1 0x%x",
		 i2c_addr);

	rc = run_cmd(cmd);
	if (rc != 0) {
		printf("Warning: Failed to assign EID to I2C addr 0x%x\n", i2c_addr);
		return rc;
	}

	printf("Assigned EID to device at I2C addr 0x%x (expected EID %d)\n",
	       i2c_addr, expected_eid);
	return 0;
}

static int setup_mctp_network(void)
{
	char cmd[512];
	int rc;

	printf("Setting up MCTP network...\n");

	/* Bring up the MCTP I2C link */
	rc = run_cmd("mctp link set mctpi2c0 up 2>/dev/null || "
		     "ip link set mctpi2c0 up 2>/dev/null");
	if (rc != 0)
		printf("Warning: Failed to bring up mctpi2c0\n");

	/* Set local EID */
	snprintf(cmd, sizeof(cmd), "mctp addr add %d dev mctpi2c0", mctp_local_eid);
	rc = run_cmd(cmd);
	if (rc != 0)
		printf("Warning: Failed to set local EID\n");

	/* Set network ID */
	snprintf(cmd, sizeof(cmd), "mctp link set mctpi2c0 net %d", mctp_net);
	rc = run_cmd(cmd);
	if (rc != 0)
		printf("Warning: Failed to set MCTP network ID\n");

	/* Create directories for dbus and mctpd */
	mkdir("/run", 0755);
	mkdir("/run/dbus", 0755);
	mkdir("/var", 0755);
	mkdir("/var/run", 0755);
	mkdir("/var/lib", 0755);
	mkdir("/var/lib/dbus", 0755);

	/* Create machine-id for dbus */
	FILE *mid = fopen("/var/lib/dbus/machine-id", "w");
	if (mid) {
		fprintf(mid, "00000000000000000000000000000001\n");
		fclose(mid);
	}
	mkdir("/etc", 0755);
	mid = fopen("/etc/machine-id", "w");
	if (mid) {
		fprintf(mid, "00000000000000000000000000000001\n");
		fclose(mid);
	}

	if (verbose) {
		printf("\nMCTP Link Configuration:\n");
		run_cmd("mctp link 2>/dev/null || ip -d link show mctpi2c0 2>/dev/null");
		run_cmd("mctp addr 2>/dev/null");
	}

	/* Start dbus-daemon */
	if (access("/bin/dbus-daemon", X_OK) == 0) {
		printf("Starting dbus-daemon...\n");
		rc = run_cmd("dbus-daemon --system --fork --print-pid");
		if (rc != 0) {
			printf("Warning: Failed to start dbus-daemon (rc=%d)\n", rc);
			run_cmd("dbus-daemon --system --fork --print-address 2>&1 || true");
		}
		usleep(500000);
	}

	/* Start mctpd */
	if (access("/bin/mctpd", X_OK) == 0) {
		printf("Starting mctpd...\n");
		pid_t pid = fork();
		if (pid < 0) {
			printf("Warning: Failed to fork for mctpd\n");
		} else if (pid == 0) {
			setsid();
			execl("/bin/mctpd", "mctpd", "-v", NULL);
			_exit(127);
		} else {
			sleep(3);

			if (verbose) {
				printf("Checking D-Bus services...\n");
				run_cmd("busctl list 2>&1 | head -20 || true");
			}

			printf("Assigning MCTP endpoints...\n");
			assign_mctp_endpoint(switch_i2c_addr, switch_eid);
			usleep(500000);
			assign_mctp_endpoint(type3_1_i2c_addr, type3_1_eid);
			usleep(500000);
			assign_mctp_endpoint(type3_2_i2c_addr, type3_2_eid);
		}
	} else {
		printf("Warning: mctpd not found\n");
	}

	if (verbose) {
		printf("\nFinal MCTP Configuration:\n");
		run_cmd("mctp link 2>/dev/null");
		run_cmd("mctp addr 2>/dev/null");
		run_cmd("mctp route 2>/dev/null");
	}

	return 0;
}
#endif /* MCTP_TEST */

static int start_cxl_fmd(void)
{
	printf("Starting cxl-fmd...\n");

	/* where cxl-fmd makes /run/cxl-fmd for its lock file */
	mkdir("/run", 0755);

	fmd_pid = fork();
	if (fmd_pid < 0) {
		perror("fork");
		return -1;
	}

	if (fmd_pid == 0) {
		/* Child: exec cxl-fmd */
		setenv("LD_LIBRARY_PATH", "/lib:/lib64", 1);
#ifdef MCTP_TEST
		if (mctp_mode) {
			if (verbose)
				execl(CXL_FMD_BIN, "cxl-fmd", "-m", "-v", NULL);
			else
				execl(CXL_FMD_BIN, "cxl-fmd", "-m", NULL);
		} else
#endif
		{
			if (verbose)
				execl(CXL_FMD_BIN, "cxl-fmd", "-i", "-v", NULL);
			else
				execl(CXL_FMD_BIN, "cxl-fmd", "-i", NULL);
		}
		perror("execl cxl-fmd");
		_exit(127);
	}

	return 0;
}

static void stop_cxl_fmd(void)
{
	if (fmd_pid > 0) {
		kill(fmd_pid, SIGTERM);
		waitpid(fmd_pid, NULL, 0);
		fmd_pid = 0;
	}
}

static int wait_for_redfish(void)
{
	struct sockaddr_in addr;
	int sock;
	int connected = 0;

	printf("Waiting for Redfish service...\n");

	/* Give cxl-fmd a moment to start HTTP server */
	sleep(2);

	for (int i = 0; i < MAX_WAIT_SECS * 10; i++) {
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			usleep(100000);
			continue;
		}

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(REDFISH_PORT);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			connected = 1;
			close(sock);
			printf("Redfish service ready on port %d\n", REDFISH_PORT);
			usleep(500000);
			return 0;
		}
		close(sock);
		usleep(100000);
	}

	if (!connected)
		printf("Timeout waiting for Redfish service (could not connect to port %d)\n", REDFISH_PORT);
	return -1;
}

static int run_curl(const char *url, const char *expected)
{
	char cmd[512];
	char output_file[] = "/tmp/curl_output.XXXXXX";
	int fd, rc;
	FILE *f;
	char buf[4096];

	fd = mkstemp(output_file);
	if (fd < 0)
		return -1;
	close(fd);

	snprintf(cmd, sizeof(cmd),
		 "%s -s -o %s -w '%%{http_code}' http://127.0.0.1:%d%s 2>/dev/null",
		 CURL_BIN, output_file, REDFISH_PORT, url);

	if (verbose)
		printf("  curl %s\n", url);

	f = popen(cmd, "r");
	if (!f) {
		unlink(output_file);
		return -1;
	}

	buf[0] = '\0';
	if (fgets(buf, sizeof(buf), f) == NULL)
		buf[0] = '\0';
	pclose(f);

	/* Check HTTP status code (ignore curl exit status if we got 200) */
	if (strncmp(buf, "200", 3) != 0) {
		printf("  [FAIL] %s - HTTP %s\n", url, buf[0] ? buf : "error");
		unlink(output_file);
		return 1;
	}

	/* Check for expected content if specified */
	if (expected) {
		f = fopen(output_file, "r");
		if (f) {
			int found = 0;
			while (fgets(buf, sizeof(buf), f)) {
				if (strstr(buf, expected)) {
					found = 1;
					break;
				}
			}
			fclose(f);
			if (!found) {
				printf("  [FAIL] %s - missing '%s'\n", url, expected);
				unlink(output_file);
				return 1;
			}
		}
	}

	printf("  [PASS] %s\n", url);
	unlink(output_file);
	return 0;
}

static int run_redfish_tests(void)
{
	int pass = 0, fail = 0;

	printf("\n=== Redfish API Tests ===\n\n");

	/* ServiceRoot */
	if (run_curl("/redfish/v1", "RedfishVersion") == 0)
		pass++;
	else
		fail++;

	/* Chassis Collection */
	if (run_curl("/redfish/v1/Chassis", "Members") == 0)
		pass++;
	else
		fail++;

	/* Fabrics Collection */
	if (run_curl("/redfish/v1/Fabrics", "Members") == 0)
		pass++;
	else
		fail++;

	/* CXL Fabric */
	if (run_curl("/redfish/v1/Fabrics/CXL", "FabricType") == 0)
		pass++;
	else
		fail++;

#ifdef MCTP_TEST
	if (mctp_mode) {
		/* MCTP mode: test switch endpoints */
		if (run_curl("/redfish/v1/Fabrics/CXL/Switches", "Members") == 0)
			pass++;
		else
			fail++;

		/* Test MCTP-discovered switch */
		if (run_curl("/redfish/v1/Fabrics/CXL/Switches/cxl_00000000000004d2", "SwitchType") == 0)
			pass++;
		else
			fail++;

		/* Test MCTP-discovered Type3 devices */
		if (run_curl("/redfish/v1/Chassis/cxl_0000000000000003", "@odata.type") == 0)
			pass++;
		else
			fail++;
		if (run_curl("/redfish/v1/Chassis/cxl_0000000000000004", "@odata.type") == 0)
			pass++;
		else
			fail++;
	} else
#endif
	{
		/* ioctl mode: Test discovered devices via /dev/cxl */
		DIR *dir = opendir(CXL_DEV_PATH);
		if (dir) {
			struct dirent *entry;
			char url[256];

			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "mem", 3) != 0)
					continue;
				if (strlen(entry->d_name) > 32)
					continue;

				/* Chassis for this device */
				snprintf(url, sizeof(url), "/redfish/v1/Chassis/%s",
					 entry->d_name);
				if (run_curl(url, "@odata.type") == 0)
					pass++;
				else
					fail++;

				/* PCIeDevices collection */
				snprintf(url, sizeof(url),
					 "/redfish/v1/Chassis/%s/PCIeDevices",
					 entry->d_name);
				if (run_curl(url, "Members") == 0)
					pass++;
				else
					fail++;

				/* PCIeDevice */
				snprintf(url, sizeof(url),
					 "/redfish/v1/Chassis/%s/PCIeDevices/%s",
					 entry->d_name, entry->d_name);
				if (run_curl(url, "PCIeDevice") == 0)
					pass++;
				else
					fail++;

				/* Memory */
				snprintf(url, sizeof(url),
					 "/redfish/v1/Chassis/%s/Memory/%s",
					 entry->d_name, entry->d_name);
				if (run_curl(url, "CapacityMiB") == 0)
					pass++;
				else
					fail++;

				/* MemoryDomains */
				snprintf(url, sizeof(url),
					 "/redfish/v1/Chassis/%s/MemoryDomains",
					 entry->d_name);
				if (run_curl(url, "Members") == 0)
					pass++;
				else
					fail++;
			}
			closedir(dir);
		}
	}

	printf("\n========================================\n");
	printf("  Results: %d passed, %d failed\n", pass, fail);
	printf("========================================\n\n");

	return fail == 0 ? 0 : 1;
}

/* SIGCHLD handler to reap zombie child processes */
static void sigchld_handler(int sig __attribute__((unused)))
{
	int saved_errno = errno;

	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	errno = saved_errno;
}

int main(void)
{
	struct sigaction sa;
	int rc = 1;

	printf("\n========================================\n");
#ifdef MCTP_TEST
	printf("  cxl-fmd QEMU Redfish Tests (MCTP)\n");
#else
	printf("  cxl-fmd QEMU Redfish Tests\n");
#endif
	printf("========================================\n\n");

	if (mount_filesystems() < 0) {
		printf("TEST_RESULT=FAIL\n");
		goto out;
	}

	/* Bring up loopback interface for localhost HTTP */
	setup_loopback();

	/* Install SIGCHLD handler */
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	parse_cmdline();
	if (verbose)
		printf("Verbose mode enabled\n");
#ifdef MCTP_TEST
	if (mctp_mode)
		printf("MCTP transport mode enabled\n");
#endif
	printf("\n");

	/* Check for curl */
	if (access(CURL_BIN, X_OK) != 0) {
		printf("[FAIL] curl not found in initramfs\n");
		printf("TEST_RESULT=FAIL\n");
		goto out;
	}

#ifdef MCTP_TEST
	if (mctp_mode) {
		/* MCTP mode: wait for MCTP I2C and setup network */
		if (wait_for_mctp_i2c() < 0) {
			printf("[FAIL] No MCTP I2C device found\n");
			printf("TEST_RESULT=FAIL\n");
			goto out;
		}

		if (setup_mctp_network() < 0) {
			printf("[FAIL] Failed to setup MCTP network\n");
			printf("TEST_RESULT=FAIL\n");
			goto out;
		}
	} else
#endif
	{
		/* ioctl mode: wait for CXL devices */
		if (wait_for_cxl_devices() == 0) {
			printf("[FAIL] No CXL devices found\n");
			printf("TEST_RESULT=FAIL\n");
			goto out;
		}
	}

	/* Start cxl-fmd */
	if (start_cxl_fmd() < 0) {
		printf("[FAIL] Failed to start cxl-fmd\n");
		printf("TEST_RESULT=FAIL\n");
		goto out;
	}

	/* Wait for Redfish service */
	if (wait_for_redfish() < 0) {
		printf("[FAIL] Redfish service not available\n");
		printf("TEST_RESULT=FAIL\n");
		goto cleanup;
	}

	/* Run tests */
	rc = run_redfish_tests();
	printf("TEST_RESULT=%s\n", rc == 0 ? "PASS" : "FAIL");

cleanup:
	stop_cxl_fmd();

out:
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
