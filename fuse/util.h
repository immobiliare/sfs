/*
 *  util.h - SFS Asynchronous filesystem replication
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

#ifndef SFS_UTIL_H
#define SFS_UTIL_H

#include <limits.h>
#include "sfs.h"

void sfs_fullpath (char fpath[PATH_MAX], const char *path);
int sfs_sync_path (const char *path, int data_only);
time_t sfs_get_monotonic_time (SfsState* state);
int sfs_begin_access (void);
void sfs_end_access (void);
int sfs_is_directory (const char* path);
int sfs_update_mtime (const char* domain, const char* path);
int sfs_fallback_timer_start (SfsState* state);

#endif