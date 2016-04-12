/*
 *  config.c - SFS Asynchronous filesystem replication
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

#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fuse.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <syslog.h>

#include "sfs.h"
#include "inih/ini.h"
#include "config.h"
#include "util.h"
#include "setproctitle.h"

static UpdateMTime parse_update_mtime (const char* value) {
	UpdateMTime res = UPDATE_MTIME_TOUCH;
	if (!strcmp (value, "no")) {
		res = UPDATE_MTIME_NO;
	} else if (!strcmp (value, "touch")) {
		res = UPDATE_MTIME_TOUCH;
	} else if (!strcmp (value, "increment")) {
		res = UPDATE_MTIME_INCREMENT;
	} else {
		syslog (LOG_WARNING, "Unknown update_mtime value %s, fallback to no", value);
	}
	return res;
}

static int parse_facility (const char* facility) {
	int res = -1;
	if (!strcmp (facility, "authpriv")) {
		res = LOG_AUTHPRIV;
	} else if (!strcmp (facility, "cron")) {
		res = LOG_CRON;
	} else if (!strcmp (facility, "daemon")) {
		res = LOG_DAEMON;
	} else if (!strcmp (facility, "ftp")) {
		res = LOG_FTP;
	} else if (!strcmp (facility, "kern")) {
		res = LOG_KERN;
	} else if (!strcmp (facility, "local0")) {
		res = LOG_LOCAL0;
	} else if (!strcmp (facility, "local1")) {
		res = LOG_LOCAL1;
	} else if (!strcmp (facility, "local2")) {
		res = LOG_LOCAL2;
	} else if (!strcmp (facility, "local3")) {
		res = LOG_LOCAL3;
	} else if (!strcmp (facility, "local4")) {
		res = LOG_LOCAL4;
	} else if (!strcmp (facility, "local5")) {
		res = LOG_LOCAL5;
	} else if (!strcmp (facility, "local6")) {
		res = LOG_LOCAL6;
	} else if (!strcmp (facility, "local7")) {
		res = LOG_LOCAL7;
	} else if (!strcmp (facility, "lpr")) {
		res = LOG_LPR;
	} else if (!strcmp (facility, "mail")) {
		res = LOG_MAIL;
	} else if (!strcmp (facility, "news")) {
		res = LOG_NEWS;
	} else if (!strcmp (facility, "syslog")) {
		res = LOG_SYSLOG;
	} else if (!strcmp (facility, "user")) {
		res = LOG_USER;
	} else if (!strcmp (facility, "uucp")) {
		res = LOG_UUCP;
	} else {
		syslog (LOG_WARNING, "Unknown facility %s, fallback to daemon", facility);
	}

	return res;
}

static int ini_handler (void* userdata, const char* section, const char* name,
						const char* value) {
    SfsState* state = (SfsState*) userdata;

    #define MATCH(s, n) !strcmp(section, s) && !strcmp(name, n)
    if (MATCH("sfs", "batch_dir")) {
		if (value[0] == '\0' || !sfs_is_directory (value)) {
			syslog(LOG_CRIT, "[config] invalid batch_dir %s: %s", value, strerror(errno));
			return 0;
		} else {
			state->batch_dir = strndup (value, PATH_MAX);
		}
	} else if (MATCH("sfs", "batch_tmp_dir")) {
		if (value[0] == '\0' || !sfs_is_directory (value)) {
			syslog(LOG_CRIT, "[config] invalid batch_tmp_dir %s: %s", value, strerror(errno));
			return 0;
		} else {
			state->batch_tmp_dir = strndup (value, PATH_MAX);
		}
	} else if (MATCH("sfs", "pid_path")) {
		if (value[0] == '\0') {
			syslog(LOG_CRIT, "[config] empty pid_path");
			return 0;
		} else {
			state->pid_path = strndup (value, PATH_MAX);
		}
	} else if (MATCH("sfs", "node_name")) {
		if (value[0] != '\0') {
			state->node_name = strndup (value, PATH_MAX);
		}
	} else if (MATCH("sfs", "ignore_path_prefix")) {
		if (value[0] != '\0') {
			state->ignore_path_prefix = strndup (value, PATH_MAX);
		}
	} else if (MATCH("sfs", "batch_flush_msec")) {
		long long msec = atoll(value);
		state->batch_flush_ts.tv_sec = msec/1000;
		state->batch_flush_ts.tv_nsec = (msec%1000) * 1000000;
	} else if (MATCH("sfs", "batch_max_events")) {
		state->batch_max_events = atoi (value);
	} else if (MATCH("sfs", "batch_max_bytes")) {
		state->batch_max_bytes = atoll (value);
	} else if (MATCH("sfs", "use_osync")) {
		state->use_osync = atoi (value);
	} else if (MATCH("sfs", "forbid_older_mtime")) {
		state->forbid_older_mtime = atoi (value);
	} else if (MATCH("sfs", "update_mtime")) {
		state->update_mtime = parse_update_mtime (value);
	} else if (MATCH("log", "ident")) {
		state->log_ident = strdup (value);
	} else if (MATCH("log", "facility")) {
		state->log_facility = parse_facility (value);
	} else if (MATCH("log", "debug")) {
		state->log_debug = atoi (value);
    } else {
		syslog(LOG_CRIT, "[config] unknown key %s/%s with value '%s'", section, name, value);
        return 0;
    }
    return 1;
}

static int config_check (SfsState* state) {
	if (!state->pid_path) {
		syslog(LOG_ERR, "[config] sfs/pid_path must be specified");
		goto error;
	}
	if (!state->batch_dir) {
		syslog(LOG_ERR, "[config] sfs/batch_dir must be specified");
		goto error;
	}
	if (!state->batch_tmp_dir) {
		syslog(LOG_ERR, "[config] sfs/batch_tmp_dir must be specified");
		goto error;
	}
	if (!state->node_name) {
		syslog(LOG_ERR, "[config] sfs/node_name must be specified");
		goto error;
	}
	if (state->batch_flush_ts.tv_sec <= 0 && state->batch_flush_ts.tv_nsec <= 0) {
		syslog(LOG_ERR, "[config] sfs/batch_flush_msec must be > 0");
		goto error;
	}
	if (state->batch_max_events <= 0) {
		syslog(LOG_ERR, "[config] sfs/batch_max_events must be > 0");
		goto error;
	}
	if (state->batch_max_bytes <= 0) {
		syslog(LOG_ERR, "[config] sfs/batch_max_bytes must be > 0");
		goto error;
	}
	if (!state->log_ident) {
		state->log_ident = strdup ("sfs-fuse");
	}
	if (state->log_facility < 0) {
		state->log_facility = LOG_DAEMON;
	}

	if (gethostname (state->hostname, sizeof(state->hostname)-1) < 0) {
		strcpy (state->hostname, "invalid");
	}

	return 1;

error:
	return 0;
}

static void config_init (SfsState* state) {
	state->log_facility = -1;
	state->update_mtime = UPDATE_MTIME_NO;
	strncpy (state->hostname, "invalid", sizeof state->hostname);
}

int sfs_config_load (SfsState* state) {
	config_init (state);

	int ret = ini_parse (state->configpath, ini_handler, state);
	if (ret < 0) {
        syslog(LOG_ERR, "[config] can't load config %s: %s", state->configpath, strerror (errno));
		return 0;
    }

	if (!config_check (state)) {
		return 0;
	}
	closelog ();
	setproctitle (state->log_ident);
	openlog (state->log_ident, LOG_PID|LOG_CONS|LOG_PERROR, state->log_facility);
    syslog(LOG_NOTICE, "Config loaded from %s", state->configpath);

	return 1;
}

int sfs_config_reload (void) {
	SfsState* state = SFS_STATE;
	SfsState new_state;
	memset(&new_state, '\0', sizeof(SfsState));
	config_init (&new_state);

	pthread_mutex_lock (&(state->config_mutex));
	syslog(LOG_INFO, "Reloading config %s", state->configpath);

	int ret = ini_parse (state->configpath, ini_handler, &new_state);
	if (ret < 0) {
        syslog(LOG_CRIT, "[config] can't load config %s: %s", state->configpath, strerror (errno));
		goto error;
    }

	if (ret > 0) {
		// error handled in ini_handler
		goto error;
	}

	if (!config_check (&new_state)) {
		goto error;
	}

	#define OLDSFREE(x) if (state->x) { free(state->x); state->x = NULL; }
	OLDSFREE(pid_path);
	OLDSFREE(batch_dir);
	OLDSFREE(batch_tmp_dir);
	OLDSFREE(node_name);
	OLDSFREE(ignore_path_prefix);
	OLDSFREE(log_ident);

	#define NSET(x) state->x = new_state.x;
	NSET(pid_path);
	NSET(batch_dir);
	NSET(batch_tmp_dir);
	NSET(node_name);
	NSET(batch_flush_ts);
	NSET(batch_max_events);
	NSET(batch_max_bytes);
	NSET(ignore_path_prefix);
	NSET(use_osync);
	NSET(update_mtime);
	NSET(forbid_older_mtime);
	NSET(log_ident);
	NSET(log_facility);
	NSET(log_debug);

	closelog ();
	setproctitle (state->log_ident);
	openlog (state->log_ident, LOG_PID, state->log_facility);
    syslog(LOG_NOTICE, "Config reloaded from %s", state->configpath);
	pthread_mutex_unlock (&(state->config_mutex));

	return 1;

error:
	#define NSFREE(x) if (new_state.x) { free(new_state.x); new_state.x = NULL; }
	NSFREE(batch_dir);
	NSFREE(batch_tmp_dir);
	NSFREE(node_name);
	NSFREE(ignore_path_prefix);
	NSFREE(log_ident);

	pthread_mutex_unlock (&(state->config_mutex));
	return 0;
}
