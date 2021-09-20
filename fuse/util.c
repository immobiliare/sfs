/*
 *  util.c - SFS Asynchronous filesystem replication
 *
 *  Copyright © 2014  Immobiliare.it S.p.A.
 *
 *  This file is part of SFS.
 *
 *  SFS is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  SFS is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with SFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fsuid.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include "sfs.h"
#include "util.h"

/*  All the paths I see are relative to the root of the mounted
 *  filesystem.  In order to get to the underlying filesystem, I need to
 *  have the mountpoint.  I'll save it away early on in main(), and then
 *  whenever I need a path for something I'll call this to construct
 *  it. */
void sfs_fullpath (char fpath[PATH_MAX], const char *path) {
	SfsState* data = SFS_STATE;	
    memcpy(fpath, data->rootdir, data->rootdir_len);
    strncpy(&fpath[data->rootdir_len], path, PATH_MAX-data->rootdir_len);
	fpath[PATH_MAX-1] = '\0';
}

int sfs_sync_path (const char *path, int data_only) {
    int fd = open (path, O_RDONLY);
	
    if (fd < 0) {
		syslog (LOG_CRIT, "[sync_path] cannot open() path %s, this may lead to batch loss: %s", path, strerror (errno));
		return 0;
    }

	if (data_only) {
		if (fdatasync (fd) != 0) {
			syslog (LOG_CRIT, "[sync_path] cannot fdatasync() path %s, this may lead to batch_loss: %s", path, strerror (errno));
			return 0;
		}
	} else {
		if (fsync (fd) != 0) {
			syslog (LOG_CRIT, "[sync_path] cannot fsync() path %s, this may lead to batch_loss: %s", path, strerror (errno));
			return 0;
		}
	}
	
	close (fd);
    return 1;
}

void sfs_get_monotonic_time (SfsState* state, struct timespec *ret) {
	// CLOCK_MONOTONIC has a weird behavior with old kernels, also support kernel < 2.6.32
	if (clock_gettime(CLOCK_REALTIME, ret) < 0) {
		syslog (LOG_ERR, "[monotonic_time] cannot clock_gettime(): %s", strerror (errno));
		*ret = state->last_time;
		return;
	}

	struct timespec dummy;
	if (sfs_timespec_subtract (&dummy, ret, &(state->last_time)) < 0) {
		// don't go back in time
		*ret = state->last_time;
		return;
	}
	
	state->last_time = *ret;
	return;
}

int sfs_begin_access (void) {
	SfsState* state = SFS_STATE;
	
	struct fuse_context* ctx = fuse_get_context();
	if (!state->perm_checks) {
		// only honor umask
		#ifdef FUSE_28
		umask (ctx->umask);
		#endif
		return 1;
	}

	pthread_mutex_lock (&(state->access_mutex));

	// get pw groups
	errno = 0;
	struct passwd *pwd = getpwuid(ctx->uid);
	if (!pwd && errno != 0) {
		syslog(LOG_CRIT, "[access] cannot read /etc/passwd: %s", strerror(errno));
		goto error;
	}
	if (pwd) {
		if (initgroups (pwd->pw_name, ctx->gid) < 0) {
			syslog(LOG_CRIT, "[access] cannot init groups for user %s: %s", pwd->pw_name, strerror(errno));
			goto error;
		}
	}
	
	if (setfsgid (ctx->gid) < 0) {
		syslog(LOG_CRIT, "[access] cannot seteuid to %d: %s", ctx->gid, strerror(errno));
		goto error;
	}

	if (setfsuid (ctx->uid) < 0) {
		syslog(LOG_CRIT, "[access] cannot seteuid to %d: %s", ctx->uid, strerror(errno));
		goto error;
	}

	#ifdef FUSE_28
	umask (ctx->umask);
	#endif
	pthread_mutex_unlock (&(state->access_mutex));
	return 1;
	
error:
	pthread_mutex_unlock (&(state->access_mutex));
	return 0;
}

void sfs_end_access (void) {
	SfsState* state = SFS_STATE;
	if (!state->perm_checks) {
		// only honor umask
		umask (state->fuse_umask);
		return;
	}
	
	if (setfsgid (0) < 0) {
		syslog(LOG_CRIT, "[access] cannot seteuid back to 0: %s", strerror(errno));
	}
		
	if (setfsuid (0) < 0) {
		syslog(LOG_CRIT, "[access] cannot setegid back to 0: %s", strerror(errno));
	}

	umask (state->fuse_umask);

	pthread_mutex_unlock (&(state->access_mutex));
}

int sfs_is_directory (const char* path) {
	struct stat buf;
	int ret = stat (path, &buf);
	if (ret < 0) {
		return 0;
	}

	if (S_ISDIR(buf.st_mode)) {
		return 1;
	}
	return 0;
}

int sfs_update_mtime (const char* domain, const char* path) {
	UpdateMTime update_mtime = SFS_STATE->update_mtime;
	if (update_mtime == UPDATE_MTIME_TOUCH) {
		struct timespec ts[2] = { {0}, {0} };
		ts[0].tv_nsec = UTIME_OMIT;
		ts[1].tv_nsec = UTIME_NOW;
		if (utimensat(0, path, ts, 0) < 0) {
			syslog(LOG_CRIT, "[%s] could not update mtime of %s: %s", domain, path, strerror(errno));
			return 0;
		}
	} else if (update_mtime == UPDATE_MTIME_INCREMENT) {
		struct stat statbuf;
		if (stat(path, &statbuf) < 0) {
			syslog(LOG_CRIT, "[%s] could not stat %s: %s", domain, path, strerror(errno));
			return 0;
		}
		
		struct timespec ts[2];
		ts[0].tv_nsec = UTIME_OMIT;
		ts[1] = statbuf.st_mtim;
		ts[1].tv_nsec++;
		if (utimensat(0, path, ts, 0) < 0) {
			syslog(LOG_CRIT, "[%s] could not update mtime of %s: %s", domain, path, strerror(errno));
			return 0;
		}
	}

	return 1;
}

// http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
int sfs_timespec_subtract (struct timespec *result, struct timespec *x, struct timespec *y) {
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_nsec < y->tv_nsec) {
		long dsec = (y->tv_nsec - x->tv_nsec) / 1000000000 + 1;
		y->tv_nsec -= 1000000000 * dsec;
		y->tv_sec += dsec;
	}
	if (x->tv_nsec - y->tv_nsec > 1000000000) {
		long nsec = (x->tv_nsec - y->tv_nsec) / 1000000000;
		y->tv_nsec += 1000000000 * nsec;
		y->tv_sec -= nsec;
	}
	
	/* Compute the time remaining to wait.
	tv_nsec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = x->tv_nsec - y->tv_nsec;
	
	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

