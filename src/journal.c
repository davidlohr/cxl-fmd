// SPDX-License-Identifier: BSD-3-Clause
/*
 * The operation journal: what a restart needs to know of the commands
 * that change the fabric
 *
 * A Bind vPPB (5201h), Unbind vPPB (5202h), Set LD Allocations (5402h),
 * Set Partition Info (4101h), Initiate Dynamic Capacity Add (5604h) or
 * Release (5605h) whose operation is under way when cxl-fmd stops leaves
 * nothing behind in the process: the task a client follows, the vPPB's
 * op, the LD's or SLD's fence and the host's hold on its Tag are all in
 * memory. Discovery reads again what the devices have, but not what was
 * asked of them, nor under which Task. None of the commands carries an
 * identifier that the device would report back (Tables 7-35, 7-36, 7-72,
 * 7-73), so the record has to be the FM's own.
 *
 * With --state-dir (src/main.c), each such command has a record written to
 * DIR/journal.jsonl, one JSON object a line, and synced (fdatasync) before
 * it is sent: an "op" record with the Task's Id (or an Id of the same form
 * for an operation with no task), the device by its Id and serial number,
 * what is asked, the baseline it is judged against and what it holds. If
 * the record cannot be written and synced, nothing is sent and the request
 * is answered 503 (FMD_JOURNAL_UNAVAILABLE): a command without its record
 * is what the journal is there to prevent. An "end" record follows when
 * the operation is over (the device refused it, its task ended, a read
 * showed what it did); it is not synced, as one that is lost only has the
 * next start look again at an operation that is over, which ends on what
 * the device shows. The first line is a "boot" record with the format's
 * version.
 *
 * At start-up the file is read (fmd_journal_open()), before discovery, and
 * once discovery is done each "op" with no "end" is made again
 * (fmd_journal_recover()):
 *  - a bind or unbind: its task, under its Id, Interrupted, and its op,
 *    lost, holding the vPPB; the switch's VCSs are read, and the first
 *    read that sees the vPPB settled ends it (fmd_tasks_reconcile()),
 *    judged against the vPPB as recorded before the command;
 *  - a Set LD Allocations: the LD is read (5401h); until it is, it is
 *    fenced (alloc_unknown), as after a lost response;
 *  - a Set Partition Info: the SLD's partition is read (4100h), fenced
 *    until it is (partition_unknown); a chunk POSTed is made again, staged,
 *    under its Id, if the device has the Next Volatile Capacity it asked
 *    for, and a chunk DELETEd is deleted if the device has the decrease;
 *  - a DC add or release: its task, under its Id, in the shape of one whose
 *    response could not be read (dc->unknown_send), with its hold on the
 *    host and Tag: what the host's lists show ends it, or FMD_DC_QUIET_READS
 *    reads that show nothing, or a DELETE of its monitor (5603h and 5608h
 *    cannot tell an add not sent from one not yet answered).
 * A task made again is served Interrupted, with Oem.Cxl.OperatorDecision
 * "Required", until it ends, and a DELETE of its monitor ends it. One whose
 * switch or device is not found keeps its hold until it is, or until that
 * DELETE. The file is then written again with only the records that are
 * not ended (fmd_journal_compact()), and so again when it grows past
 * FMD_JOURNAL_COMPACT_BYTES.
 *
 * A line that a crash cut short is the last: it is cut off, with a
 * warning. A line before it that cannot be read cannot be from a crash in
 * an append, nor can a record of another version be read as this one: the
 * start is refused, and the operator moves the file away.
 *
 * The journal has its own lock (DIR/journal.lock, never renamed), so that
 * two cxl-fmd with different --lock-file cannot share a state directory.
 * Records are written with fabric_lock held exclusively (or dcd_lock, for
 * the end of a DC task), under the journal's own mutex.
 *
 * The format, the file's place and every rule here are cxl-fmd's.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <jansson.h>

#include "cxl-fmd.h"

#define JOURNAL_TMP "journal.tmp"

struct fmd_journal {
	pthread_mutex_t lock;
	char dir[PATH_MAX];
	int dir_fd;
	int lock_fd;
	int fd;			/* FMD_JOURNAL_FILE, appended to */
	off_t size;
	/* a sync failed: what reached the disk is not known, nothing more */
	bool broken;
	json_t *live;		/* Id -> its last "op" record, not ended */
	json_t *replay;		/* those of the earlier runs, to recover */
};

int (*fmd_journal_sync)(int fd) = fdatasync;

/* 64-bit values as hex strings: JSON integers are not that wide everywhere */
static json_t *hex64(uint64_t v)
{
	char buf[24];

	snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
	return json_string(buf);
}

static uint64_t get_hex64(const json_t *o, const char *key)
{
	const char *s = json_string_value(json_object_get(o, key));

	return s ? strtoull(s, NULL, 16) : 0;
}

static int get_int(const json_t *o, const char *key, int dflt)
{
	const json_t *v = json_object_get(o, key);

	return json_is_integer(v) ? (int)json_integer_value(v) : dflt;
}

static const char *get_str(const json_t *o, const char *key)
{
	const char *s = json_string_value(json_object_get(o, key));

	return s ? s : "";
}

static uint64_t comp_serial(const struct fmd_component *c)
{
	switch (c->type) {
	case FMD_COMPONENT_SWITCH:
		return fmd_to_switch(c)->identify.serial_num;
	case FMD_COMPONENT_SLD:
		return fmd_to_sld(c)->identify.serial_num;
	case FMD_COMPONENT_MLD:
		return fmd_to_mld(c)->identify.serial_num;
	case FMD_COMPONENT_MHD:
		return fmd_to_mhd(c)->identify.serial_num;
	default:
		return 0;
	}
}

static const char *comp_type(const struct fmd_component *c)
{
	switch (c->type) {
	case FMD_COMPONENT_SWITCH:
		return "switch";
	case FMD_COMPONENT_SLD:
		return "sld";
	case FMD_COMPONENT_MLD:
		return "mld";
	case FMD_COMPONENT_MHD:
		return "mhd";
	default:
		return "other";
	}
}

/*
 * The device: its Id, which discovery makes from the serial number
 * (cxl_{serial}) for a switch and a device alike, and the serial number,
 * checked against what is found under the Id after a restart. The route is
 * only a hint for the operator: how a device is reached can change.
 */
static json_t *dev_json(const struct fmd_component *c)
{
	return json_pack("{s:s, s:o, s:s, s:{s:s?, s:b}}",
			 "type", comp_type(c),
			 "serial", hex64(comp_serial(c)),
			 "id", c->id,
			 "route", "parent", c->parent ? c->parent->id : NULL,
			 "endpoint", c->ep != NULL);
}

/*
 * The device a record names, as discovery found it after the restart: the
 * component of its Id, if its serial number is the one recorded. A device
 * whose serial number is 0 is named after its endpoint (cxl_ep{N}), which
 * need not be the same device after a restart: it is taken as not found
 * (cxl-fmd's choice).
 */
static struct fmd_component *find_dev(struct fmd_ctx *ctx, const json_t *rec)
{
	const json_t *dev = json_object_get(rec, "dev");
	uint64_t serial = get_hex64(dev, "serial");
	struct fmd_component *c;

	c = fmd_topology_find(&ctx->topology, get_str(dev, "id"));
	if (!c || strcmp(comp_type(c), get_str(dev, "type")))
		return NULL;
	if (serial && comp_serial(c) != serial)
		return NULL;
	if (!serial && !fmd_is_mock(ctx))
		return NULL;
	return c;
}

static json_t *ranges_json(const struct fmd_dc_range *r, int n)
{
	json_t *a = json_array();
	char tag[40];

	for (int i = 0; i < n; i++) {
		fmd_dc_tag_to_str(r[i].tag, tag, sizeof(tag));
		json_array_append_new(a, json_pack("{s:o, s:o, s:s, s:i}",
						   "dpa", hex64(r[i].dpa),
						   "len", hex64(r[i].len),
						   "tag", tag,
						   "region", r[i].region_id));
	}
	return a;
}

/* the ranges of @a, allocated (NULL for none); -1 if out of memory */
static int ranges_from(const json_t *a, struct fmd_dc_range **out)
{
	size_t n = json_array_size(a), i;
	const json_t *e;

	*out = NULL;
	if (!n)
		return 0;
	*out = calloc(n, sizeof(**out));
	if (!*out)
		return -1;
	json_array_foreach(a, i, e) {
		(*out)[i].dpa = get_hex64(e, "dpa");
		(*out)[i].len = get_hex64(e, "len");
		(*out)[i].region_id = (uint8_t)get_int(e, "region", 0);
		fmd_dc_tag_from_str(get_str(e, "tag"), (*out)[i].tag);
	}
	return (int)n;
}

/*
 * One line, appended: on a write that fails the file is cut back to what
 * it was. @sync: synced before this returns 0, and on a sync that fails the
 * journal takes nothing more (what reached the disk is not known; a later
 * fdatasync can return 0 without it).
 */
static int write_line(struct fmd_journal *j, int fd, off_t *size,
		      const json_t *rec, bool sync)
{
	char *line = json_dumps(rec, JSON_COMPACT);
	size_t len, done = 0;
	int rc = 0;

	if (!line)
		return -ENOMEM;
	len = strlen(line);
	{
		char *nl = realloc(line, len + 2);

		if (!nl) {
			free(line);
			return -ENOMEM;
		}
		line = nl;
		line[len++] = '\n';
		line[len] = '\0';
	}
	while (done < len) {
		ssize_t n = write(fd, line + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			rc = n < 0 ? -errno : -EIO;
			break;
		}
		done += (size_t)n;
	}
	free(line);
	if (rc) {
		if (ftruncate(fd, *size) < 0)
			j->broken = true;
		return rc;
	}
	*size += (off_t)len;
	if (sync && fmd_journal_sync(fd) < 0) {
		j->broken = true;
		return -EIO;
	}
	return 0;
}

static json_t *boot_record(struct fmd_ctx *ctx)
{
	return json_pack("{s:s, s:i, s:s, s:i, s:I}", "t", "boot",
			 "v", FMD_JOURNAL_VERSION, "boot", ctx->topology.boot_id,
			 "pid", (int)getpid(), "ts", (json_int_t)time(NULL));
}

/*
 * The journal written again: the boot record and the records not ended,
 * to journal.tmp, synced, renamed over the file, and the directory
 * synced. The lock is on another file, so the rename does not let it go.
 */
static int compact_locked(struct fmd_ctx *ctx, struct fmd_journal *j)
{
	const char *id;
	json_t *rec, *boot;
	off_t size = 0;
	int fd, rc = 0, nfd;

	fd = openat(j->dir_fd, JOURNAL_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		    0600);
	if (fd < 0) {
		fmd_err(ctx, "journal: cannot write %s/%s: %s", j->dir, JOURNAL_TMP,
			strerror(errno));
		return -EIO;
	}
	boot = boot_record(ctx);
	rc = write_line(j, fd, &size, boot, false);
	json_decref(boot);
	json_object_foreach(j->live, id, rec) {
		if (rc)
			break;
		rc = write_line(j, fd, &size, rec, false);
	}
	if (!rc && fmd_journal_sync(fd) < 0)
		rc = -EIO;
	close(fd);
	if (rc) {
		/* the old file stands, as it was */
		unlinkat(j->dir_fd, JOURNAL_TMP, 0);
		fmd_err(ctx, "journal: %s/%s not compacted", j->dir, FMD_JOURNAL_FILE);
		return -EIO;
	}
	if (renameat(j->dir_fd, JOURNAL_TMP, j->dir_fd, FMD_JOURNAL_FILE) < 0) {
		unlinkat(j->dir_fd, JOURNAL_TMP, 0);
		fmd_err(ctx, "journal: cannot rename %s/%s: %s", j->dir, JOURNAL_TMP,
			strerror(errno));
		return -EIO;
	}
	nfd = openat(j->dir_fd, FMD_JOURNAL_FILE, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fsync(j->dir_fd) < 0 || nfd < 0) {
		fmd_err(ctx, "journal: %s/%s replaced and not synced: no more "
			"records are written", j->dir, FMD_JOURNAL_FILE);
		if (nfd >= 0)
			close(nfd);
		j->broken = true;
		return -EIO;
	}
	close(j->fd);
	j->fd = nfd;
	j->size = size;
	fmd_info(ctx, "journal: %s/%s compacted: %zu operation(s) not ended",
		 j->dir, FMD_JOURNAL_FILE, json_object_size(j->live));
	return 0;
}

int fmd_journal_compact(struct fmd_ctx *ctx)
{
	struct fmd_journal *j = ctx->journal;
	int rc;

	if (!j)
		return 0;
	pthread_mutex_lock(&j->lock);
	rc = j->broken ? -EIO : compact_locked(ctx, j);
	pthread_mutex_unlock(&j->lock);
	return rc;
}

/*
 * The lines of @buf (@len bytes): each a record, applied to j->live. A
 * last line with no newline is a crash's: its offset is set in @cut.
 */
static int journal_replay(struct fmd_ctx *ctx, struct fmd_journal *j,
			  const char *buf, size_t len, size_t *cut,
			  json_t *boots)
{
	size_t start = 0;
	int lineno = 0;

	*cut = len;
	while (start < len) {
		const char *nl = memchr(buf + start, '\n', len - start);
		const char *t, *id;
		json_error_t err;
		json_t *rec;

		lineno++;
		if (!nl) {
			*cut = start;
			fmd_warn(ctx, "journal: line %d of %s/%s was cut short "
				 "(%zu bytes), by a crash while it was written: "
				 "left out", lineno, j->dir, FMD_JOURNAL_FILE,
				 len - start);
			return 0;
		}
		rec = json_loadb(buf + start, (size_t)(nl - buf) - start, 0, &err);
		t = json_string_value(json_object_get(rec, "t"));
		id = json_string_value(json_object_get(rec, "id"));
		if (!json_is_object(rec) || !t) {
			fmd_err(ctx, "journal: line %d of %s/%s cannot be read: "
				"not started (move the file away to start without it)",
				lineno, j->dir, FMD_JOURNAL_FILE);
			json_decref(rec);
			return -EPROTO;
		}
		if (!strcmp(t, "boot")) {
			const char *b = json_string_value(json_object_get(rec, "boot"));

			if (b)
				json_object_set_new(boots, b, json_true());
			if (get_int(rec, "v", -1) != FMD_JOURNAL_VERSION) {
				fmd_err(ctx, "journal: %s/%s is of version %d, not %d: "
					"not started (move the file away to start "
					"without it)", j->dir, FMD_JOURNAL_FILE,
					get_int(rec, "v", -1), FMD_JOURNAL_VERSION);
				json_decref(rec);
				return -EPROTO;
			}
		} else if (!strcmp(t, "op") && id) {
			/* the last record of an Id is what is under way */
			json_object_set(j->live, id, rec);
		} else if (!strcmp(t, "end") && id) {
			json_object_del(j->live, id);
		} else {
			fmd_err(ctx, "journal: line %d of %s/%s is not a record of "
				"version %d: not started (move the file away to start "
				"without it)", lineno, j->dir, FMD_JOURNAL_FILE,
				FMD_JOURNAL_VERSION);
			json_decref(rec);
			return -EPROTO;
		}
		json_decref(rec);
		start = (size_t)(nl - buf) + 1;
	}
	return 0;
}

/* Whether @id is taken by a run the journal names, or by a record's Id */
static bool boot_id_taken(struct fmd_journal *j, json_t *boots, const char *id)
{
	size_t n = strlen(id);
	const char *key;
	json_t *rec;

	if (json_object_get(boots, id))
		return true;
	json_object_foreach(j->live, key, rec) {
		(void)rec;
		if (!strncmp(key, id, n) && key[n] == '-')
			return true;
	}
	return false;
}

/*
 * This run's boot id is drawn again while it is one an earlier run in the
 * journal had, or the prefix of an operation recovered from it: a Task
 * "<boot>-<n>" of this run must not take the Id of a recovered one.
 * Nothing has been named after the boot id yet: the journal is read before
 * discovery, chunks or Tasks.
 */
static int boot_id_unused(struct fmd_ctx *ctx, struct fmd_journal *j,
			  json_t *boots)
{
	char *id = ctx->topology.boot_id;

	for (int tries = 0; boot_id_taken(j, boots, id); tries++) {
		if (tries == 16) {
			fmd_err(ctx, "journal: no boot id unused by %s/%s",
				j->dir, FMD_JOURNAL_FILE);
			return -EEXIST;
		}
		fmd_boot_id_draw(id, FMD_BOOT_ID_LEN);
	}
	return 0;
}

static void journal_free(struct fmd_journal *j)
{
	if (!j)
		return;
	if (j->fd >= 0)
		close(j->fd);
	if (j->lock_fd >= 0)
		close(j->lock_fd);	/* lets the lock go */
	if (j->dir_fd >= 0)
		close(j->dir_fd);
	json_decref(j->live);
	json_decref(j->replay);
	pthread_mutex_destroy(&j->lock);
	free(j);
}

/* the whole of @fd */
static char *read_all(int fd, size_t *len)
{
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);

	while (buf) {
		ssize_t r;

		if (n == cap) {
			char *b = realloc(buf, cap *= 2);

			if (!b)
				break;
			buf = b;
		}
		r = pread(fd, buf + n, cap - n, (off_t)n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			break;
		if (r == 0) {
			*len = n;
			return buf;
		}
		n += (size_t)r;
	}
	free(buf);
	return NULL;
}

int fmd_journal_open(struct fmd_ctx *ctx, const char *dir)
{
	struct fmd_journal *j = calloc(1, sizeof(*j));
	size_t len = 0, cut;
	bool made = false;
	json_t *boot, *boots;
	char *buf;
	int rc;

	if (!j)
		return -ENOMEM;
	pthread_mutex_init(&j->lock, NULL);
	j->dir_fd = j->lock_fd = j->fd = -1;
	j->live = json_object();
	snprintf(j->dir, sizeof(j->dir), "%s", dir);

	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		rc = -errno;
		fmd_err(ctx, "journal: cannot create the state directory %s: %s; "
			"give --state-dir DIR", dir, strerror(errno));
		goto fail;
	}
	j->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (j->dir_fd < 0) {
		rc = -errno;
		fmd_err(ctx, "journal: cannot open the state directory %s: %s; "
			"give --state-dir DIR", dir, strerror(errno));
		goto fail;
	}
	j->lock_fd = openat(j->dir_fd, FMD_JOURNAL_LOCK, O_RDWR | O_CREAT | O_CLOEXEC,
			    0600);
	if (j->lock_fd < 0 || flock(j->lock_fd, LOCK_EX | LOCK_NB) < 0) {
		rc = errno == EWOULDBLOCK ? -EBUSY : -errno;
		fmd_err(ctx, "journal: %s: %s/%s", rc == -EBUSY ?
			"another cxl-fmd uses the state directory" :
			strerror(errno), dir, FMD_JOURNAL_LOCK);
		goto fail;
	}
	j->fd = openat(j->dir_fd, FMD_JOURNAL_FILE, O_RDWR | O_APPEND | O_CLOEXEC);
	if (j->fd < 0 && errno == ENOENT) {
		j->fd = openat(j->dir_fd, FMD_JOURNAL_FILE,
			       O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
		made = true;
	}
	if (j->fd < 0) {
		rc = -errno;
		fmd_err(ctx, "journal: cannot open %s/%s: %s", dir, FMD_JOURNAL_FILE,
			strerror(errno));
		goto fail;
	}
	/* a file made is in the directory only once the directory is synced */
	if (made && fsync(j->dir_fd) < 0) {
		rc = -errno;
		fmd_err(ctx, "journal: cannot sync %s: %s", dir, strerror(errno));
		goto fail;
	}

	buf = read_all(j->fd, &len);
	if (!buf) {
		rc = -EIO;
		fmd_err(ctx, "journal: cannot read %s/%s", dir, FMD_JOURNAL_FILE);
		goto fail;
	}
	boots = json_object();
	rc = boots ? journal_replay(ctx, j, buf, len, &cut, boots) : -ENOMEM;
	free(buf);
	if (!rc)
		rc = boot_id_unused(ctx, j, boots);
	json_decref(boots);
	if (rc)
		goto fail;
	/* a line cut short is cut off before anything is appended to it */
	if (cut != len && (ftruncate(j->fd, (off_t)cut) < 0 ||
			   fmd_journal_sync(j->fd) < 0)) {
		rc = -EIO;
		fmd_err(ctx, "journal: cannot cut %s/%s: %s", dir, FMD_JOURNAL_FILE,
			strerror(errno));
		goto fail;
	}
	j->size = (off_t)cut;
	j->replay = json_copy(j->live);

	boot = boot_record(ctx);
	rc = write_line(j, j->fd, &j->size, boot, true);
	json_decref(boot);
	if (rc) {
		fmd_err(ctx, "journal: cannot write %s/%s: %s", dir, FMD_JOURNAL_FILE,
			strerror(-rc));
		goto fail;
	}
	ctx->journal = j;
	fmd_info(ctx, "journal: %s/%s, %zu operation(s) of an earlier run not ended",
		 dir, FMD_JOURNAL_FILE, json_object_size(j->replay));
	return 0;
fail:
	journal_free(j);
	return rc;
}

void fmd_journal_close(struct fmd_ctx *ctx)
{
	journal_free(ctx->journal);
	ctx->journal = NULL;
}

int fmd_journal_open_ops(struct fmd_ctx *ctx)
{
	int n;

	if (!ctx->journal)
		return 0;
	pthread_mutex_lock(&ctx->journal->lock);
	n = (int)json_object_size(ctx->journal->live);
	pthread_mutex_unlock(&ctx->journal->lock);
	return n;
}

/* An "op" record (@rec, taken over) of @id, written and synced */
static int journal_intent(struct fmd_ctx *ctx, const char *id, json_t *rec)
{
	struct fmd_journal *j = ctx->journal;
	int rc;

	if (!j) {
		json_decref(rec);
		return 0;
	}
	if (!rec)
		return -EIO;
	json_object_set_new(rec, "t", json_string("op"));
	json_object_set_new(rec, "id", json_string(id));
	json_object_set_new(rec, "ts", json_integer((json_int_t)time(NULL)));

	pthread_mutex_lock(&j->lock);
	if (j->broken) {
		rc = -EIO;
	} else {
		if (j->size > FMD_JOURNAL_COMPACT_BYTES)
			compact_locked(ctx, j);
		rc = j->broken ? -EIO : write_line(j, j->fd, &j->size, rec, true);
	}
	if (!rc)
		json_object_set(j->live, id, rec);
	pthread_mutex_unlock(&j->lock);
	if (rc)
		fmd_err(ctx, "journal: the record of %s could not be written to "
			"%s/%s (%s): nothing is sent", id, j->dir, FMD_JOURNAL_FILE,
			strerror(-rc));
	json_decref(rec);
	return rc ? -EIO : 0;
}

void fmd_journal_end(struct fmd_ctx *ctx, const char *id, const char *result)
{
	struct fmd_journal *j = ctx->journal;
	json_t *rec;

	if (!j || !id || !*id)
		return;
	pthread_mutex_lock(&j->lock);
	if (json_object_get(j->live, id)) {
		rec = json_pack("{s:s, s:s, s:s, s:I}", "t", "end", "id", id,
				"result", result, "ts", (json_int_t)time(NULL));
		if (!j->broken && rec && write_line(j, j->fd, &j->size, rec, false))
			fmd_warn(ctx, "journal: the end of %s could not be written: "
				 "the next start looks at it again", id);
		json_decref(rec);
		json_object_del(j->live, id);
	}
	pthread_mutex_unlock(&j->lock);
}

static json_t *seen_json(const struct fmd_vppb_seen *seen)
{
	return json_pack("{s:b, s:i, s:i, s:i}", "read", seen->read,
			 "status", (int)seen->status, "port", seen->port_id,
			 "ld", seen->ld_id);
}

int fmd_journal_vppb(struct fmd_ctx *ctx, const struct fmd_task *task,
		     uint8_t unbind_option)
{
	struct fmd_switch *sw;
	char uri[FMD_CONN_ID_LEN + 64];

	if (!ctx->journal)
		return 0;
	sw = fmd_topology_find_switch(&ctx->topology, task->switch_id);
	if (!sw)
		return -EIO;
	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Connections/%s",
		 task->connection_id);
	return journal_intent(ctx, task->id, json_pack(
		"{s:s, s:o, s:{s:i, s:i, s:i, s:i, s:i}, s:o, s:{s:s, s:i, s:i}, s:s}",
		"kind", task->kind == FMD_TASK_BIND ? "bind" : "unbind",
		"dev", dev_json(&sw->base),
		"scope", "vcs", task->vcs_id, "vppb", task->vppb_id,
			 "port", task->port_id, "ld", task->ld_id,
			 "unbind_option", unbind_option,
		"baseline", seen_json(&task->before),
		"hold", "switch", sw->base.id, "vcs", task->vcs_id,
			"vppb", task->vppb_id,
		"uri", uri));
}

int fmd_journal_ld_alloc(struct fmd_ctx *ctx, const char *id,
			 const struct fmd_component *dev, const struct fmd_ld *ld,
			 uint64_t old, uint64_t new, const char *chunk_id)
{
	if (!ctx->journal)
		return 0;
	return journal_intent(ctx, id, json_pack(
		"{s:s, s:o, s:{s:i, s:o, s:o, s:s}, s:{s:o}, s:o, s:{s:i}}",
		"kind", "ld_alloc",
		"dev", dev_json(dev),
		"scope", "ld", ld->ld_id, "old_size", hex64(old),
			 "new_size", hex64(new), "chunk_id", chunk_id ? chunk_id : "",
		"baseline", "size", hex64(old),
		"delta", hex64(new),
		"hold", "ld", ld->ld_id));
}

int fmd_journal_partition(struct fmd_ctx *ctx, const char *id,
			  const struct fmd_component *dev, const struct fmd_ld *ld,
			  const char *what, const struct fmd_mem_chunk *chunk,
			  uint64_t next,
			  const struct cxlmi_cmd_memdev_get_partition_info_rsp *before)
{
	if (!ctx->journal)
		return 0;
	return journal_intent(ctx, id, json_pack(
		"{s:s, s:o, s:{s:s, s:i, s:s, s:o, s:o, s:s}, s:{s:o, s:o, s:o, s:o}, s:o}",
		"kind", "sld_partition",
		"dev", dev_json(dev),
		"scope", "kind", what, "ld", ld ? ld->ld_id : -1,
			 "chunk_id", chunk ? chunk->base.id : "",
			 "size", hex64(chunk ? chunk->size : 0),
			 "offset", hex64(chunk ? chunk->base_address : 0),
			 "type", chunk ? chunk->address_range_type : "",
		"baseline", "active_vmem", hex64(before->active_vmem),
			    "active_pmem", hex64(before->active_pmem),
			    "next_vmem", hex64(before->next_vmem),
			    "next_pmem", hex64(before->next_pmem),
		"delta", hex64(next)));
}

int fmd_journal_dc(struct fmd_ctx *ctx, const struct fmd_task *task)
{
	const struct fmd_dc_task *dc = task->dc;
	struct fmd_component *dev;
	char tag[40];
	json_t *rec;

	if (!ctx->journal)
		return 0;
	dev = fmd_topology_find(&ctx->topology, dc->dev_id);
	if (!dev)
		return -EIO;
	fmd_dc_tag_to_str(dc->tag, tag, sizeof(tag));
	rec = json_pack(
		"{s:s, s:o, s:{s:i, s:s, s:s, s:s, s:i, s:s, s:i, s:o, s:b, s:b, s:o},"
		" s:{s:I, s:o}, s:{s:i, s:s}}",
		"kind", task->kind == FMD_TASK_DC_ADD ? "dc_add" : "dc_release",
		"dev", dev_json(dev),
		"scope", "host_id", dc->host_id, "chassis_id", dc->chassis_id,
			 "ld_id", dc->ld_id, "ext_id", dc->ext_id,
			 "region", dc->region_id, "tag", tag, "policy", dc->policy,
			 "length", hex64(dc->length), "forced", dc->forced,
			 "sanitize", dc->sanitize,
			 "extents", ranges_json(dc->req, dc->n_req),
		"baseline", "gen", (json_int_t)dc->base_gen,
			    "extents", ranges_json(dc->base, dc->n_base),
		"hold", "host_id", dc->host_id, "tag", tag);
	/* the body as it came, if it is not long: the Task's Payload */
	if (rec && dc->body && strlen(dc->body) <= FMD_JOURNAL_BODY_MAX)
		json_object_set_new(rec, "body", json_string(dc->body));
	else if (rec && dc->body)
		json_object_set_new(rec, "body_omitted", json_true());
	return journal_intent(ctx, task->id, rec);
}

/* ---- start-up ---- */

static void recover_vppb(struct fmd_ctx *ctx, const char *id, const json_t *rec,
			 bool bind)
{
	const json_t *scope = json_object_get(rec, "scope");
	const json_t *base = json_object_get(rec, "baseline");
	struct fmd_component *c = find_dev(ctx, rec);
	struct fmd_vppb_seen before = { 0 };
	struct fmd_bind_key key = { 0 };
	struct fmd_task *task;

	snprintf(key.switch_id, sizeof(key.switch_id), "%s",
		 get_str(json_object_get(rec, "dev"), "id"));
	key.vcs_id = (uint8_t)get_int(scope, "vcs", 0);
	key.vppb_id = (uint8_t)get_int(scope, "vppb", 0);
	key.port_id = (uint8_t)get_int(scope, "port", 0);
	key.ld_id = (uint16_t)get_int(scope, "ld", FMD_LD_NONE);
	before.read = json_is_true(json_object_get(base, "read"));
	before.status = (enum fmd_vppb_binding_status)get_int(base, "status", 0);
	before.port_id = (uint8_t)get_int(base, "port", 0);
	before.ld_id = (uint16_t)get_int(base, "ld", FMD_LD_NONE);
	if (c && c->type != FMD_COMPONENT_SWITCH)
		c = NULL;

	task = fmd_task_recover_vppb(ctx, id, bind ? FMD_TASK_BIND : FMD_TASK_UNBIND,
				     &key, &before,
				     (uint8_t)get_int(scope, "unbind_option", 0), !c);
	if (!task) {
		fmd_err(ctx, "journal: %s: no memory for its task: kept for the "
			"next start", id);
		return;
	}
	/*
	 * The switch's VCSs are read now: one that shows the vPPB settled
	 * ends the task (fmd_tasks_reconcile()). A mock's switch is what its
	 * file says, not what the command did: its task waits for a request
	 * that reads the VCSs.
	 */
	if (c && c->ep)
		fmd_vcs_refresh(ctx, fmd_to_switch(c));
}

static struct fmd_ld *dev_ld_of(struct fmd_component *dev, int ld_id)
{
	struct fmd_component *child;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->ld_id == ld_id)
			return fmd_to_ld(child);
	}
	return NULL;
}

static void recover_ld_alloc(struct fmd_ctx *ctx, const char *id, const json_t *rec)
{
	const json_t *scope = json_object_get(rec, "scope");
	struct fmd_component *dev = find_dev(ctx, rec);
	struct fmd_ld *ld;
	char result[64];

	ld = dev && (dev->type == FMD_COMPONENT_MLD || dev->type == FMD_COMPONENT_MHD) ?
	     dev_ld_of(dev, get_int(scope, "ld", -1)) : NULL;
	if (!ld) {
		fmd_warn(ctx, "journal: %s: Set LD Allocations of a device or LD "
			 "not found: kept for the next start", id);
		return;
	}
	/* what the LD has is what Get LD Allocations says now */
	if (!fmd_ld_alloc_refresh_ld(ctx, dev, ld)) {
		snprintf(result, sizeof(result), "Read: %" PRIu64 " MB",
			 ld->memory_size >> 20);
		fmd_info(ctx, "journal: %s: LD %u of %s read after the restart: "
			 "%" PRIu64 " MB", id, ld->ld_id, dev->id, ld->memory_size >> 20);
		fmd_journal_end(ctx, id, result);
		return;
	}
	fmd_warn(ctx, "journal: %s: LD %u of %s could not be read: not known, "
		 "and no chunk of it changes until it is", id, ld->ld_id, dev->id);
	ld->alloc_unknown = true;
	snprintf(ld->journal_id, sizeof(ld->journal_id), "%s", id);
}

static void recover_partition(struct fmd_ctx *ctx, const char *id, const json_t *rec)
{
	const json_t *scope = json_object_get(rec, "scope");
	struct fmd_component *dev = find_dev(ctx, rec);
	const char *what = get_str(scope, "kind");
	struct fmd_sld *sld;

	if (!dev) {
		fmd_warn(ctx, "journal: %s: Set Partition Info of a device not "
			 "found: kept for the next start", id);
		return;
	}
	/*
	 * An LD's partition, and a mock's, is as discovery has just read it;
	 * nothing of it waits on the outcome
	 */
	if (dev->type != FMD_COMPONENT_SLD || !dev->ep) {
		fmd_journal_end(ctx, id, "Read");
		return;
	}
	sld = fmd_to_sld(dev);
	sld->partition_unknown = true;
	snprintf(sld->journal_id, sizeof(sld->journal_id), "%s", id);
	sld->unknown_next = get_hex64(rec, "delta");
	if (!strcmp(what, "DELETE"))
		snprintf(sld->unknown_delete, sizeof(sld->unknown_delete), "%s",
			 get_str(scope, "chunk_id"));
	if (!strcmp(what, "POST")) {
		snprintf(sld->unknown_post, sizeof(sld->unknown_post), "%s",
			 get_str(scope, "chunk_id"));
		sld->unknown_post_offset = get_hex64(scope, "offset");
		sld->unknown_post_size = get_hex64(scope, "size");
	}
	/* read now; until it is, nothing that changes it is sent */
	if (fmd_sld_partition_refresh(ctx, sld))
		fmd_warn(ctx, "journal: %s: the partition of %s could not be read: "
			 "not known until it is", id, dev->id);
}

static void recover_dc(struct fmd_ctx *ctx, const char *id, const json_t *rec,
		       bool add)
{
	const json_t *scope = json_object_get(rec, "scope");
	const json_t *base = json_object_get(rec, "baseline");
	struct fmd_component *dev = find_dev(ctx, rec);
	struct fmd_dc_task *dc = calloc(1, sizeof(*dc));
	const char *body = json_string_value(json_object_get(rec, "body"));
	int n;

	if (!dc)
		goto nomem;
	snprintf(dc->dev_id, sizeof(dc->dev_id), "%s",
		 get_str(json_object_get(rec, "dev"), "id"));
	snprintf(dc->chassis_id, sizeof(dc->chassis_id), "%s",
		 get_str(scope, "chassis_id"));
	snprintf(dc->ld_id, sizeof(dc->ld_id), "%s", get_str(scope, "ld_id"));
	snprintf(dc->ext_id, sizeof(dc->ext_id), "%s", get_str(scope, "ext_id"));
	dc->host_id = get_int(scope, "host_id", -1);
	dc->policy = (uint8_t)get_int(scope, "policy", 0);
	dc->region_id = (uint8_t)get_int(scope, "region", 0);
	fmd_dc_tag_from_str(get_str(scope, "tag"), dc->tag);
	dc->length = get_hex64(scope, "length");
	dc->forced = json_is_true(json_object_get(scope, "forced"));
	dc->sanitize = json_is_true(json_object_get(scope, "sanitize"));
	dc->base_gen = (uint32_t)json_integer_value(json_object_get(base, "gen"));
	if (body && !(dc->body = strdup(body)))
		goto nomem;
	n = ranges_from(json_object_get(scope, "extents"), &dc->req);
	if (n < 0)
		goto nomem;
	dc->n_req = n;
	n = ranges_from(json_object_get(base, "extents"), &dc->base);
	if (n < 0)
		goto nomem;
	dc->n_base = n;

	pthread_mutex_lock(&ctx->dcd_lock);
	if (!fmd_task_recover_dc(ctx, id, add ? FMD_TASK_DC_ADD : FMD_TASK_DC_RELEASE,
				 dc, !dev || !fmd_memdev_dcd(dev))) {
		pthread_mutex_unlock(&ctx->dcd_lock);
		goto nomem;
	}
	pthread_mutex_unlock(&ctx->dcd_lock);
	return;
nomem:
	if (dc) {
		free(dc->body);
		free(dc->req);
		free(dc->base);
		free(dc);
	}
	fmd_err(ctx, "journal: %s: no memory for its task: kept for the next start",
		id);
}

void fmd_journal_recover(struct fmd_ctx *ctx)
{
	struct fmd_journal *j = ctx->journal;
	const char *id;
	json_t *rec;

	if (!j)
		return;
	pthread_rwlock_wrlock(&ctx->fabric_lock);
	json_object_foreach(j->replay, id, rec) {
		const char *kind = get_str(rec, "kind");

		fmd_warn(ctx, "journal: operation %s (%s of %s) of an earlier run "
			 "had not ended", id, kind,
			 get_str(json_object_get(rec, "dev"), "id"));
		if (!strcmp(kind, "bind") || !strcmp(kind, "unbind"))
			recover_vppb(ctx, id, rec, !strcmp(kind, "bind"));
		else if (!strcmp(kind, "ld_alloc"))
			recover_ld_alloc(ctx, id, rec);
		else if (!strcmp(kind, "sld_partition"))
			recover_partition(ctx, id, rec);
		else if (!strcmp(kind, "dc_add") || !strcmp(kind, "dc_release"))
			recover_dc(ctx, id, rec, !strcmp(kind, "dc_add"));
		else
			fmd_err(ctx, "journal: %s: an operation of kind \"%s\" "
				"is not known: kept", id, kind);
	}
	json_decref(j->replay);
	j->replay = NULL;
	pthread_rwlock_unlock(&ctx->fabric_lock);

	fmd_journal_compact(ctx);
}
