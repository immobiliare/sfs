/*
 *  batch.c - SFS Asynchronous filesystem replication
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
#include <sys/time.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

#include "sfs.h"
#include "util.h"
#include "config.h"

static void batch_clear (SfsState* state);
static void batch_flush (SfsState* state);

static void* batch_timer_handler (void* arg) {
	SfsState* state = (SfsState*) arg;

	while (1) {
		struct timespec flush_ts = state->batch_flush_ts;

		struct timespec sleep_ts = flush_ts;
		while (sleep_ts.tv_sec > 0 || sleep_ts.tv_nsec > 0) {
			struct timespec rem_ts;
			if (nanosleep (&sleep_ts, &rem_ts) < 0 && errno == EINTR) {
				sleep_ts = rem_ts;
			} else {
				break;
			}
		}

		pthread_mutex_lock (&(state->batch_mutex));
		struct timespec curtime, difftime, dummy;
		sfs_get_monotonic_time (state, &curtime);
		// curtime - batch_time
		sfs_timespec_subtract (&difftime, &curtime, &(state->batch_time));
		if (sfs_timespec_subtract (&dummy, &flush_ts, &difftime)) {
			// diff time > flush time
			batch_flush (state);
		}
		pthread_mutex_unlock (&(state->batch_mutex));
	}

	return NULL;
}

int batch_start_timer (SfsState* state) {
	pthread_t timer_thread;
	if (pthread_create (&timer_thread, NULL, batch_timer_handler, state) != 0) {
		syslog(LOG_CRIT, "[init_thread] cannot start timer thread: %s", strerror (errno));
		return 0;
	}

	if (pthread_detach (timer_thread) != 0) {
		syslog(LOG_CRIT, "[init_thread] cannot detach timer thread: %s", strerror (errno));
		return 0;
	}

	return 1;
}

static void batch_clear (SfsState* state) {
	if (state->batch_tmp_file >= 0) {
		if (close (state->batch_tmp_file) < 0) {
			syslog(LOG_WARNING, "[batch_clear] error while closing tmp batch: %s", strerror (errno));
		}
		state->batch_tmp_file = -1;
	}
	if (state->batch_tmp_path) {
		free (state->batch_tmp_path);
		state->batch_tmp_path = NULL;
	}
	if (state->batch_name) {
		free (state->batch_name);
		state->batch_name = NULL;
	}
	state->batch_events = 0;
	state->batch_bytes = 0;
	sfs_set_clear (state->batch_file_set);
}

static void batch_flush (SfsState* state) {
	const char* batch_dir = state->batch_dir;
	char* batch_path = NULL;

	if (state->batch_tmp_file < 0) {
		goto cleanup;
	}

	if (state->log_debug&1) {
		syslog(LOG_DEBUG, "[batch_flush] flushing %s", state->batch_tmp_path);
	}

	if (close (state->batch_tmp_file) < 0) {
		syslog(LOG_WARNING, "[batch_flush] error while closing fd %d of tmp batch %s: %s", state->batch_tmp_file, state->batch_tmp_path, strerror (errno));
	}
	state->batch_tmp_file = -1;

	if (asprintf(&batch_path, "%s/%s", batch_dir, state->batch_name) < 0) {
		syslog(LOG_CRIT, "[batch_flush] batch_path asprintf of %s/%s failed: %s", batch_dir, state->batch_name, strerror (errno));
		goto cleanup;
	}

	int retry=0;
retry:
	if (rename (state->batch_tmp_path, batch_path) < 0) {
		syslog(LOG_CRIT, "[batch_flush] (%d) rename of %s to %s failed: %s", retry, state->batch_tmp_path, batch_path, strerror (errno));
		if(++retry<3){
			usleep(1000);
			goto retry;
		}
		goto cleanup;
	}
	if(retry){
		syslog(LOG_NOTICE, "[batch_flush] rename success after %d retrys", retry);
	}
	sfs_sync_path (state->batch_dir, 0);
	sfs_sync_path (state->batch_tmp_dir, 0);

cleanup:
	if (batch_path) {
		free (batch_path);
	}
	batch_clear (state);
}

// Line with length but must still be zero-terminated!
void batch_event (const char* line, int len, const char* type) {
	SfsState* state = SFS_STATE;

	if (state->log_debug&1) {
		syslog (LOG_DEBUG, "[batch_event] batching %s", line);
	}

	pthread_mutex_lock (&(state->batch_mutex));
	if (state->batch_type && strcmp (state->batch_type, type)) {
		batch_flush (state);
	}
	state->batch_type = type;

	if (state->batch_tmp_file < 0) {
		struct timespec curtime;
		sfs_get_monotonic_time (state, &curtime);

		int subid = state->batch_subid;
		if (curtime.tv_sec == state->batch_time.tv_sec) {
			// same second, increment subid
			subid++;
		} else {
			subid = 0;
		}

		const char* node_name = state->node_name;
		const char* batch_tmp_dir = state->batch_tmp_dir;

		if (asprintf(&(state->batch_name), "%ld_%s_%s_%d_%05d_%s.batch", curtime.tv_sec, node_name, state->hostname, state->pid, subid, type) < 0) {
			syslog(LOG_CRIT, "[batch_event] batchname asprintf failed for event %s: %s", line, strerror (errno));
			goto error;
		}

		if (asprintf(&(state->batch_tmp_path), "%s/%s", batch_tmp_dir, state->batch_name) < 0) {
			syslog(LOG_CRIT, "[batch_event] batchpath asprintf failed for event %s, batchname %s: %s", line, state->batch_name, strerror (errno));
			goto error;
		}

		int extra_flags =  O_CREAT | O_WRONLY | O_NOATIME | O_EXCL | O_NONBLOCK | (state->use_osync ? O_SYNC : 0);

		state->batch_tmp_file = open (state->batch_tmp_path, extra_flags, 0666 & (~(state->fuse_umask)));
		if (state->batch_tmp_file < 0) {
			syslog(LOG_CRIT, "[batch_event] cannot open batch %s for writing event %s: %s", state->batch_tmp_path, line, strerror (errno));
			goto error;
		}

		if (state->log_debug&1) {
			syslog (LOG_DEBUG, "Created batch %s", state->batch_tmp_path);
		}

		sfs_sync_path (state->batch_tmp_dir, 0);

		state->batch_time = curtime;
		state->batch_subid = subid;
    }

	if (write (state->batch_tmp_file, line, len) < 0) {
		syslog(LOG_CRIT, "[batch_event] error while writing batch event %s to %s with fd %d, clearing batch file: %s", line, state->batch_tmp_path, state->batch_tmp_file, strerror(errno));
		goto error;
	}

	if (state->batch_events++ >= state->batch_max_events ||
		state->batch_bytes >= state->batch_max_bytes) {
		batch_flush (state);
	}

	pthread_mutex_unlock (&(state->batch_mutex));
	return;

error:
	batch_flush (state);
	pthread_mutex_unlock (&(state->batch_mutex));
}

void batch_file_event (const char* path, const char* type) {
	SfsState* state = SFS_STATE;
	const char* ignore_path_prefix = state->ignore_path_prefix;

	if (!strcmp (path, "/.sfs.conf")) {
		sfs_config_reload ();
	} else if (!strcmp (path, "/.sfs.mounted")) {
		// skip
	} else if (ignore_path_prefix && strstr(path, ignore_path_prefix) == path) {
		// skipping ignored path
	} else if (strstr(path, ".fuse_hidden") != NULL) {
		// skip fuse hidden files
	} else {
		if (sfs_set_add (state->batch_file_set, path)) {
			return;
		}

		size_t len = strlen(path);
		char nlpath[len+2];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
		strncpy (nlpath, path, len);
#pragma GCC diagnostic pop
		nlpath[len] = '\n';
		nlpath[len+1] = '\0';
		batch_event (nlpath, len+1, type);
	}
}

void batch_bytes_written (int bytes) {
	SfsState* state = SFS_STATE;
	__sync_add_and_fetch (&state->batch_bytes, bytes);
}
