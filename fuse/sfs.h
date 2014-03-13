/*
 *  sfs.h - SFS Asynchronous filesystem replication
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

#ifndef SFS_H
#define SFS_H

#define SFS_VERSION "1.3.13"

// maintain global sfs state in here
#include <stdio.h>
#include <pthread.h>
#include <fuse.h>

#include "set.h"

#ifndef CLOCK_MONOTONIC_RAW
// Added in kernel 2.6.28 but not in glibc
#define CLOCK_MONOTONIC_RAW 4
#endif

typedef enum {
	UPDATE_MTIME_NO,
	UPDATE_MTIME_TOUCH,
	UPDATE_MTIME_INCREMENT
} UpdateMTime;

typedef struct {
	// general
    char* rootdir;
	int rootdir_len;
	char* configpath;
	time_t last_time;
	pid_t pid;
	pthread_mutex_t access_mutex;
	int perm_checks;
	int fuse_umask;
	volatile int opened_fds;
	char hostname[1024];

	// current batch
	pthread_mutex_t batch_mutex;
	int batch_tmp_file;
	char* batch_tmp_path;
	char* batch_name;
	const char* batch_type;
	volatile int batch_events;
	volatile uint64_t batch_bytes;
	SfsSet* batch_file_set;
	
	// preserve accross multiple batch creations
	time_t batch_time;
	int batch_subid;
	
	// config
	pthread_mutex_t config_mutex;
	char* pid_path;
	char* batch_dir;
	char* batch_tmp_dir;
	char* node_name;
	char* ignore_path_prefix;
	int batch_flush_seconds;
	int batch_max_events;
	uint64_t batch_max_bytes;
	int use_osync;
	UpdateMTime update_mtime;
	int forbid_older_mtime;
	
	char* log_ident;
	int log_facility;
	int log_debug;
} SfsState;

#define SFS_STATE ((SfsState *) fuse_get_context()->private_data)

#endif