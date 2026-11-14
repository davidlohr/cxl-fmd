/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Logging
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "cxl-fmd.h"

static const char *level_str[] = {
	[FMD_LOG_ERROR] = "ERROR",
	[FMD_LOG_WARN]  = "WARN",
	[FMD_LOG_INFO]  = "INFO",
	[FMD_LOG_DEBUG] = "DEBUG",
};

void fmd_log(struct fmd_ctx *ctx, enum fmd_log_level level,
	     const char *fmt, ...)
{
	va_list ap;
	time_t now;
	struct tm tm;
	char timebuf[32];

	if (ctx && level > ctx->log_level)
		return;

	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

	/* one line at a time, from whichever thread */
	flockfile(stderr);
	fprintf(stderr, "[%s] [%s] ", timebuf, level_str[level]);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
	funlockfile(stderr);
}
