/*
 *  sfs.c - SFS Asynchronous filesystem replication
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

#define FUSE_USE_VERSION 26

#define _XOPEN_SOURCE 700
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <pthread.h>
#include <syslog.h>
#include <stddef.h>

#include "sfs.h"
#include "config.h"
#include "batch.h"
#include "util.h"
#include "set.h"
#include "setproctitle.h"

#define BEGIN_PERM if (!sfs_begin_access ()) { \
	return -EPERM; \
}

#define END_PERM sfs_end_access ();

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//
/** Get file attributes.
*
* Similar to stat().  The 'st_dev' and 'st_blksize' fields are
* ignored.  The 'st_ino' field is ignored except if the 'use_ino'
* mount option is given.
*/
int sfs_getattr(const char *path, struct stat *statbuf) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = lstat(fpath, statbuf);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	}
    
    return retstat;
}

/** Read the target of a symbolic link
*
* The buffer should be filled with a null terminated string.  The
* buffer size argument includes the space for the terminating
* null character.  If the linkname is too long to fit in the
* buffer, it should be truncated.  The return value should be 0
* for success.
*/
// Note the system readlink() will truncate and lose the terminating
// null.  So, the size passed to to the system readlink() must be one
// less than the size passed to sfs_readlink()
// sfs_readlink() code by Bernardo F Costa (thanks!)
int sfs_readlink(const char *path, char *link, size_t size) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = readlink(fpath, link, size - 1);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else  {
		link[retstat] = '\0';
		retstat = 0;
    }
    
    return retstat;
}

/** Create a file node
*
* There is no create() operation, mknod() will be called for
* creation of all non-directory, non-symlink nodes.
*/
// shouldn't that comment be "if" there is no.... ?
int sfs_mknod(const char *path, mode_t mode, dev_t dev) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
	retstat = mknod(fpath, mode, dev);
	END_PERM;
	if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
	retstat = mkdir(fpath, mode);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = unlink(fpath);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Remove a directory */
int sfs_rmdir(const char *path) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = rmdir(fpath);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
int sfs_symlink(const char *path, const char *link) {
    int retstat = 0;
    char flink[PATH_MAX];
    sfs_fullpath(flink, link);

	BEGIN_PERM;
    retstat = symlink(path, flink);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (link, "norec");
	}
    
    return retstat;
}

/** Rename a file */
// both path and newpath are fs-relative
int sfs_rename(const char *path, const char *newpath) {
    int retstat = 0;
    char fpath[PATH_MAX];
    char fnewpath[PATH_MAX];
    sfs_fullpath(fpath, path);
    sfs_fullpath(fnewpath, newpath);

	const char* mode = "norec";
	
	struct stat statbuf;
	if (lstat(fpath, &statbuf) >= 0 && S_ISDIR (statbuf.st_mode)) {
		mode = "rec";
	}
	
	BEGIN_PERM;
	retstat = rename(fpath, fnewpath);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, mode);
		batch_file_event (newpath, mode);
	}
    
    return retstat;
}

/** Create a hard link to a file */
int sfs_link(const char *path, const char *newpath) {
    int retstat = 0;
    char fpath[PATH_MAX], fnewpath[PATH_MAX];
    sfs_fullpath(fpath, path);
    sfs_fullpath(fnewpath, newpath);

	BEGIN_PERM;
    retstat = link(fpath, fnewpath);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (newpath, "norec");
	}
    
    return retstat;
}

/** Change the permission bits of a file */
int sfs_chmod(const char *path, mode_t mode) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = chmod(fpath, mode);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		sfs_update_mtime ("chmod", fpath);
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Change the owner and group of a file */
int sfs_chown(const char *path, uid_t uid, gid_t gid) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = chown(fpath, uid, gid);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		sfs_update_mtime ("chown", fpath);
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Change the size of a file */
int sfs_truncate(const char *path, off_t newsize) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = truncate(fpath, newsize);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Change the access and/or modification times of a file */
/* note -- I'll want to change this as soon as 2.6 is in debian testing */
int sfs_utime(const char *path, struct utimbuf *ubuf) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
	if (SFS_STATE->forbid_older_mtime) {
		struct stat statbuf;
		if (stat(fpath, &statbuf) < 0) {
			syslog(LOG_CRIT, "[utime] cannot stat to forbid older mtime %s: %s", fpath, strerror(errno));
		} else if (ubuf->modtime < statbuf.st_mtime) {
			END_PERM;
			return -EPERM;
		}
	}
    retstat = utime(fpath, ubuf);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

#ifdef HAVE_UTIMENSAT
// Copied fom http://fuse.sourceforge.net/doxygen/fusexmp__fh_8c.html
static int sfs_utimens(const char *path, const struct timespec ts[2]) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
	if (SFS_STATE->forbid_older_mtime) {
		struct stat statbuf;
		if (stat(fpath, &statbuf) < 0) {
			syslog(LOG_CRIT, "[utimens] cannot stat to forbid older mtime %s: %s", fpath, strerror(errno));
		} else if (ts[1].tv_sec < statbuf.st_mtim.tv_sec || (ts[1].tv_sec == statbuf.st_mtim.tv_sec && ts[1].tv_nsec < statbuf.st_mtim.tv_nsec)) {
			END_PERM;
			return -EPERM;
		}
	}
	/* don't use utime/utimes since they follow symlinks */
	retstat = utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW);
	END_PERM;
	if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
	
	return retstat;
}
#endif

/** File open operation
*
* No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
* will be passed to open().  Open should check if the operation
* is permitted for the given flags.  Optionally open may also
* return an arbitrary filehandle in the fuse_file_info structure,
* which will be passed to all file operations.
*
* Changed in version 2.2
*/
int sfs_open(const char *path, struct fuse_file_info *fi) {
    int retstat = 0;
    int fd;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    fd = open(fpath, fi->flags);
	END_PERM;
    if (fd < 0) {
		retstat = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[open] opened fds %d\n", opened_fds);
		}
	}
    
    fi->fh = fd;
    
    return retstat;
}

/** Read data from an open file
*
* Read should return exactly the number of bytes requested except
* on EOF or error, otherwise the rest of the data will be
* substituted with zeroes.  An exception to this is when the
* 'direct_io' mount option is specified, in which case the return
* value of the read system call will reflect the return value of
* this operation.
*
* Changed in version 2.2
*/
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int retstat = 0;
	
	if (fi->direct_io) {
		retstat = pread(fi->fh, buf, size, offset);
		if (retstat < 0) {
			retstat = -errno;
		}
	} else {
		while (retstat < size) {
			int cur = pread(fi->fh, buf, size-retstat, offset+retstat);
			if (cur <= 0) {
				if (cur < 0) {
					retstat = -errno;
				}
				break;
			}
			retstat += cur;
		}
	}
	
	return retstat;
}

/** Write data to an open file
*
* Write should return exactly the number of bytes requested
* except on error.  An exception to this is when the 'direct_io'
* mount option is specified (see read operation).
*
* Changed in version 2.2
*/
int sfs_write(const char *path, const char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi) {
    int retstat = 0;
	
	if (fi->direct_io) {
		retstat = pwrite (fi->fh, buf, size, offset);
		if (retstat < 0) {
			retstat = -errno;
		}
	} else {
		while (retstat < size) {
			int cur = pwrite (fi->fh, buf, size-retstat, offset+retstat);
			if (cur <= 0) {
				if (cur < 0) {
					retstat = -errno;
				}
				break;
			}
			retstat += cur;
		}
	}
	
	if (retstat > 0) {
		batch_bytes_written (retstat);
	}
    
    return retstat;
}

/** Get filesystem statistics
*
* The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
*
* Replaced 'struct statfs' parameter with 'struct statvfs' in
* version 2.5
*/
int sfs_statfs(const char *path, struct statvfs *statv) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    // get stats for underlying filesystem
    retstat = statvfs(fpath, statv);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	}
    
    return retstat;
}

/** Possibly flush cached data
*
* BIG NOTE: This is not equivalent to fsync().  It's not a
* request to sync dirty data.
*
* Flush is called on each close() of a file descriptor.  So if a
* filesystem wants to return write errors in close() and the file
* has cached dirty data, this is a good place to write back data
* and return any errors.  Since many applications ignore close()
* errors this is not always useful.
*
* NOTE: The flush() method may be called more than once for each
* open().  This happens if more than one file descriptor refers
* to an opened file due to dup(), dup2() or fork() calls.  It is
* not possible to determine if a flush is final, so each flush
* should be treated equally.  Multiple write-flush sequences are
* relatively rare, so this shouldn't be a problem.
*
* Filesystems shouldn't assume that flush will always be called
* after some writes, or that if will be called at all.
*
* Changed in version 2.2
*/
int sfs_flush(const char *path, struct fuse_file_info *fi) {
    int retstat = 0;
    return retstat;
}

/** Release an open file
*
* Release is called when there are no more references to an open
* file: all file descriptors are closed and all memory mappings
* are unmapped.
*
* For every open() call there will be exactly one release() call
* with the same flags and file descriptor.  It is possible to
* have a file opened more than once, in which case only the last
* release will mean, that no more reads/writes will happen on the
* file.  The return value of release is ignored.
*
* Changed in version 2.2
*/
int sfs_release(const char *path, struct fuse_file_info *fi) {
    int retstat = 0;
    retstat = close(fi->fh);
	if (retstat < 0) {
		retstat = -errno;
	} else {
		if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR)) {
			batch_file_event (path, "norec");
		}
	
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_sub_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[close] opened fds %d\n", opened_fds);
		}
	}
    
    return retstat;
}

/** Synchronize file contents
*
* If the datasync parameter is non-zero, then only the user data
* should be flushed, not the meta data.
*
* Changed in version 2.2
*/
int sfs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    int retstat = 0;
    
    if (datasync) {
		retstat = fdatasync(fi->fh);
	} else {
		retstat = fsync(fi->fh);
	}
    
    if (retstat < 0) {
		retstat = -errno;
	}
    
    return retstat;
}

#ifdef HAVE_FALLOCATE
static int sfs_fallocate(const char *path, int mode,
						 off_t offset, off_t length, struct fuse_file_info *fi) {
	int retstat = 0;
	
	(void) path;
	retstat = fallocate (fi->fh, mode, offset, length);
	if (retstat < 0) {
		retstat = -errno;
	}
	
	return retstat;
}
#elif HAVE_POSIX_FALLOCATE
static int sfs_fallocate(const char *path, int mode,
						 off_t offset, off_t length, struct fuse_file_info *fi) {
	(void) path;
	if (mode) {
		return -EOPNOTSUPP;
	}
	return -posix_fallocate(fi->fh, offset, length);
}
#endif

/** Set extended attributes */
int sfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = lsetxattr(fpath, name, value, size, flags);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Get extended attributes */
int sfs_getxattr(const char *path, const char *name, char *value, size_t size) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = lgetxattr(fpath, name, value, size);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
    }
	
    return retstat;
}

/** List extended attributes */
int sfs_listxattr(const char *path, char *list, size_t size) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = llistxattr(fpath, list, size);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	}
    
    return retstat;
}

/** Remove extended attributes */
int sfs_removexattr(const char *path, const char *name) {
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    retstat = lremovexattr(fpath, name);
	END_PERM;
    if (retstat < 0) {
		retstat = -errno;
	} else {
		batch_file_event (path, "norec");
	}
    
    return retstat;
}

/** Open directory
*
* This method should check if the open operation is permitted for
* this  directory
*
* Introduced in version 2.3
*/
int sfs_opendir(const char *path, struct fuse_file_info *fi) {
    DIR *dp;
    int retstat = 0;
    char fpath[PATH_MAX];
    sfs_fullpath(fpath, path);

	BEGIN_PERM;
    dp = opendir(fpath);
	END_PERM;
    if (dp == NULL) {
		retstat = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[opendir] opened fds %d\n", opened_fds);
		}
	}
    
    fi->fh = (intptr_t) dp;
    
    return retstat;
}

/** Read directory
*
* This supersedes the old getdir() interface.  New applications
* should use this.
*
* The filesystem may choose between two modes of operation:
*
* 1) The readdir implementation ignores the offset parameter, and
* passes zero to the filler function's offset.  The filler
* function will not return '1' (unless an error happens), so the
* whole directory is read in a single readdir operation.  This
* works just like the old getdir() method.
*
* 2) The readdir implementation keeps track of the offsets of the
* directory entries.  It uses the offset parameter and always
* passes non-zero offset to the filler function.  When the buffer
* is full (or an error happens) the filler function will return
* '1'.
*
* Introduced in version 2.3
*/
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
				struct fuse_file_info *fi) {
	int retstat = 0;
	DIR *dp;
	struct dirent *de;
	// once again, no need for fullpath -- but note that I need to cast fi->fh
	dp = (DIR *) (uintptr_t) fi->fh;
	
	// reset before doing anything
	errno = 0;
	
	// This will copy the entire directory into the buffer.  The loop exits
	// when either the system readdir() returns NULL, or filler()
	// returns something non-zero.  The first case just means I've
	// read the whole directory; the second means the buffer is full.
	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0) != 0) {
			syslog(LOG_CRIT, "sfs_readdir filler: buffer full while reading dir %s", path);
			return -ENOMEM;
		}
	}
	
	if (de == NULL && errno == EBADF) {
		retstat = -errno;
	}
	
	return retstat;
}

/** Release directory
*
* Introduced in version 2.3
*/
int sfs_releasedir (const char *path, struct fuse_file_info *fi) {
	int retstat = 0;
	retstat = closedir((DIR *) (uintptr_t) fi->fh);
	if (retstat < 0) {
		retstat = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_sub_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[closedir] opened fds %d\n", opened_fds);
		}
	}
	
	return retstat;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 * caused by mount option dirsync, which causes directory operations e.g. mkdir to be synchronous
 */

int sfs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
	int retstat = 0;
	DIR *dp;
	int fd;
	
	dp = (DIR *) (uintptr_t) fi->fh;
	fd = dirfd (dp);
	
	if (datasync) {
		retstat = fdatasync(fd);
	} else {
		retstat = fsync(fd);
	}
	
	if (retstat < 0) {
		retstat = -errno;
	}
	
	return retstat;
}

/**
* Initialize filesystem
*
* The return value will passed in the private_data field of
* fuse_context to all file operations and as a parameter to the
* destroy() method.
*
* Introduced in version 2.3
* Changed in version 2.6
*/
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *sfs_init(struct fuse_conn_info *conn) {
	SfsState* state = SFS_STATE;
	state->pid = getpid ();

	openlog (state->log_ident, LOG_PID, state->log_facility);
	syslog (LOG_INFO, "[main] started sfs");

	// write pid file
	const char* pidpath = state->pid_path;
	if (state->pid_path) {
		FILE *pidfile = fopen (pidpath, "w");
		if (!pidfile) {
			syslog(LOG_ERR, "[main] cannot open %s for write: %s", pidpath, strerror (errno));
		} else {
			if (!fprintf(pidfile, "%d\n", state->pid)) {
				syslog(LOG_ERR, "[main] can't write pid %d to %s: %s.\n",
					   state->pid, pidpath, strerror (errno));
			}
			fflush (pidfile);
			fclose (pidfile);
		}
	}

	batch_start_timer (state);
	return state;
}

/**
* Clean up filesystem
*
* Called on filesystem exit.
*
* Introduced in version 2.3
*/
void sfs_destroy (void *userdata) {
	/* SfsState* state = (SfsState*) userdata; */
	// nop, other threads might still be accessing this struct
}

/**
* Check file access permissions
*
* This will be called for the access() system call.  If the
* 'default_permissions' mount option is given, this method is not
* called.
*
* This method is not called under Linux kernel versions 2.4.x
*
* Introduced in version 2.5
*/
int sfs_access(const char *path, int mask) {
	int retval;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	retval = euidaccess (fpath, mask);
	END_PERM;
	if (retval < 0) {
		retval = -errno;
	}

	return retval;
}

/**
* Create and open a file
*
* If the file does not exist, first create it with the specified
* mode, and then open it.
*
* If this method is not implemented or under Linux kernel
* versions earlier than 2.6.15, the mknod() and open() methods
* will be called instead.
*
* Introduced in version 2.5
*/
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	int retstat = 0;
	char fpath[PATH_MAX];
	int fd;
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	fd = creat(fpath, mode);
	END_PERM;
	if (fd < 0) {
		retstat = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[creat] opened fds %d\n", opened_fds);
		}
	}
	
	fi->fh = fd;
	
	return retstat;
}

/**
* Change the size of an open file
*
* This method is called instead of the truncate() method if the
* truncation was invoked from an ftruncate() system call.
*
* If this method is not implemented or under Linux kernel
* versions earlier than 2.6.15, the truncate() method will be
* called instead.
*
* Introduced in version 2.5
*/
int sfs_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi) {
	int retstat = 0;
	
	(void) path;
	retstat = ftruncate(fi->fh, offset);
	if (retstat < 0) {
		retstat = -errno;
	}
	
	return retstat;
}

/**
* Get attributes from an open file
*
* This method is called instead of the getattr() method if the
* file information is available.
*
* Currently this is only called after the create() method if that
* is implemented (see above).  Later it may be called for
* invocations of fstat() too.
*
* Introduced in version 2.5
*/
// Since it's currently only called after sfs_create(), and sfs_create()
// opens the file, I ought to be able to just use the fd and ignore
// the path...
int sfs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi) {
	int retstat = 0;
	
	(void) path;
	retstat = fstat(fi->fh, statbuf);
	if (retstat < 0) {
		retstat = -errno;
	}
	
	return retstat;
}

struct fuse_operations sfs_oper = {
	.getattr = sfs_getattr,
	.readlink = sfs_readlink,
	// no .getdir -- that's deprecated
	.getdir = NULL,
	.mknod = sfs_mknod,
	.mkdir = sfs_mkdir,
	.unlink = sfs_unlink,
	.rmdir = sfs_rmdir,
	.symlink = sfs_symlink,
	.rename = sfs_rename,
	.link = sfs_link,
	.chmod = sfs_chmod,
	.chown = sfs_chown,
	.truncate = sfs_truncate,
	.utime = sfs_utime,
	.open = sfs_open,
	.read = sfs_read,
	.write = sfs_write,
	.statfs = sfs_statfs,
	.flush = sfs_flush,
	.release = sfs_release,
	.fsync = sfs_fsync,
	.setxattr = sfs_setxattr,
	.getxattr = sfs_getxattr,
	.listxattr = sfs_listxattr,
	.removexattr = sfs_removexattr,
	.opendir = sfs_opendir,
	.readdir = sfs_readdir,
	.releasedir = sfs_releasedir,
	.fsyncdir = sfs_fsyncdir,
	.init = sfs_init,
	.destroy = sfs_destroy,
	.access = sfs_access,
	.create = sfs_create,
	.ftruncate = sfs_ftruncate,
	.fgetattr = sfs_fgetattr,
	#ifdef HAVE_UTIMENSAT
	.utimens = sfs_utimens,
	#endif
	/* Others
	.lock - for networking, local by default
	.flock - for networking, local by default
	.write_buf - version 2.8
	.read_buf - version 2.8
	.poll - version 2.8
	.ioctl - version 2.8
	.bmap - for block device
	.fallocate = sfs_fallocate, version 2.9.1
	*/
};

enum {
	KEY_HELP,
	KEY_VERSION,
	KEY_PERMS,
};

#define SFS_OPT(t, p, v) { t, offsetof(SfsState, p), v }

static struct fuse_opt sfs_opts[] = {
	SFS_OPT("sfs_perms", perm_checks, 1),
	SFS_OPT("sfs_uid=%i", uid, 0),
	SFS_OPT("sfs_gid=%i", gid, 0),
	FUSE_OPT_KEY("-V", KEY_VERSION),
	FUSE_OPT_KEY("--version", KEY_VERSION),
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),
	FUSE_OPT_KEY("--perms", KEY_PERMS),
	FUSE_OPT_END
};

void sfs_usage() {
	fprintf(stderr,
		"usage: sfs rootdir mountpoint\n"
		"\n"
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -o big_writes          uses '-o max_write' instead of 4k chunks\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n"
		"SFS options:\n"
		"    --perms                equivalent to '-o perms'\n"
		"    -o sfs_uid=N           drop privileges to user\n"
		"    -o sfs_gid=N           drop privileges to group\n"
		"    -o sfs_perms           allow startup as root (not recommended)\n"
		"\n"
	);
	abort();
}

static int sfs_opt_handler (void *data, const char *arg, int key, struct fuse_args *outargs) {
	SfsState* state = (SfsState*) data;
	switch (key) {
		case KEY_PERMS:
			state->perm_checks = 1;
			return 0;
		case KEY_HELP:
			execlp("man", "man", "mount.fuse", NULL);
		case KEY_VERSION:
			return -1;
		case FUSE_OPT_KEY_OPT:
			break;
		case FUSE_OPT_KEY_NONOPT:
			if (!state->rootdir) {
				state->rootdir = realpath(arg, NULL);
				if (!state->rootdir) {
					syslog(LOG_ERR, "[main] directory '%s' does not exist", arg);
					return -1;
				}
				state->rootdir_len = state->rootdir ? strlen(state->rootdir) : -1;
				return 0;
			}
			break;
	}

	return 1;
}

int main(int argc, char **argv) {
	int fuse_stat;
	SfsState *state;

	fprintf(stderr, "sfs-fuse version %s\n", SFS_VERSION);

	state = calloc(1, sizeof(SfsState));
	if (state == NULL) {
		perror("[main] state calloc failed");
		abort();
	}
	
	openlog ("sfs-startup", LOG_PID|LOG_CONS|LOG_PERROR, LOG_DAEMON);
	
	struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
	fuse_opt_parse (&args, state, sfs_opts, sfs_opt_handler);
	
	if (!state->rootdir) {
		sfs_usage();
	}

	if (!sfs_is_directory (state->rootdir)) {
		syslog(LOG_ERR, "[main] root %s is not a directory", state->rootdir);
		return 1;
	}

	if (state->uid || state->gid) {
		if (!(state->uid && state->gid)) {
			syslog(LOG_ERR, "uid and gid must be set");
			abort();
		}
		if (setgid(state->gid) == -1) {
			syslog(LOG_ERR, "unable to drop privileges to gid %i", state->gid);
			abort();

		}
		if (setuid(state->uid) == -1) {
			syslog(LOG_ERR, "unable to drop privileges to uid %i", state->uid);
			abort();
		}

		syslog(LOG_NOTICE, "Drop privileges to uid=%i, gid=%i\n", state->uid, state->gid);
	}

	if (!state->perm_checks && (getuid() == 0 || geteuid() == 0)) {
		syslog(LOG_ERR, "[main] cannot run as root without --perms");
		abort ();
	}

	if (state->perm_checks && (getuid() != 0)) {
		syslog(LOG_ERR, "[main] running as non-root with --perms will not have the expected behavior\n");
		abort ();
	}

	if (state->perm_checks && pthread_mutex_init (&(state->access_mutex), NULL) != 0) {
		syslog(LOG_ERR, "[main] cannot init access mutex: %s", strerror (errno));
		return 2;
	}

	// init set proctitle
	initproctitle (argc, argv);
	
	// config
	if (pthread_mutex_init (&(state->config_mutex), NULL) != 0) {
		syslog(LOG_ERR, "[main] cannot init config mutex: %s", strerror (errno));
		return 3;
	}
	if (asprintf(&state->configpath, "%s/.sfs.conf", state->rootdir) < 0) {
		syslog(LOG_ERR, "[main] configpath asprintf for %s/%s failed: %s", state->rootdir, state->rootdir, strerror (errno));
		return 4;
	}
	if (!sfs_config_load (state)) {
		return 5;
	}
	
	// startup values
	sfs_get_monotonic_time (state, &(state->last_time));
	
	if (pthread_mutex_init (&(state->batch_mutex), NULL) != 0) {
		syslog(LOG_CRIT, "[main] cannot init batch mutex: %s", strerror (errno));
		return 7;
	}

	state->batch_tmp_file = -1;
	state->batch_file_set = sfs_set_new ();
	
	// flush pending batches
	DIR* dir = opendir (state->batch_tmp_dir);
	if (!dir) {
		syslog (LOG_ERR, "[main] cannot open tmp batch dir %s: %s", state->batch_tmp_dir, strerror(errno));
		return 8;
	}
	struct dirent* ent;
	int flushed = 0;
	while ((ent = readdir (dir))) {
		if (strstr (ent->d_name, ".batch")) {
			// move pending tmp batch to the batch dir
			char* tmp_path = NULL;
			if (asprintf(&tmp_path, "%s/%s", state->batch_tmp_dir, ent->d_name) < 0) {
				syslog(LOG_ERR, "[main] tmp_path asprintf for %s/%s failed: %s", state->batch_tmp_dir, ent->d_name, strerror (errno));
				return 9;
			}
			
			char* batch_path = NULL;
			if (asprintf(&batch_path, "%s/%s", state->batch_dir, ent->d_name) < 0) {
				syslog(LOG_ERR, "[main] batch_path asprintf for %s/%s failed: %s", state->batch_dir, ent->d_name, strerror (errno));
				return 10;
			}

			if (rename (tmp_path, batch_path) < 0) {
				syslog(LOG_ERR, "[main] rename of %s to %s failed: %s", tmp_path, batch_path, strerror (errno));
				return 11;
			}

			free (tmp_path);
			free (batch_path);

			flushed++;
		}
	}
	closedir(dir);
	sfs_sync_path (state->batch_dir, 0);
	sfs_sync_path (state->batch_tmp_dir, 0);
	syslog(LOG_NOTICE, "[main] flushed %d pending batches from tmp dir %s to %s", flushed, state->batch_tmp_dir, state->batch_dir);

	// save fuse process umask
	state->fuse_umask = umask (0);
	umask (state->fuse_umask);

	syslog (LOG_INFO, "[main] starting sfs with root=%s, uid=%d, gid=%d, umask=%03o; closing console syslog", state->rootdir, getuid(), getgid(), state->fuse_umask);
	closelog ();

	// set pretty fsname and fstype
	char buf[1024];
	snprintf(buf, sizeof buf, "-ofsname=%s", state->rootdir);
	fuse_opt_add_arg(&args, buf);
	fuse_opt_add_arg(&args, "-osubtype=sfs");

	// turn over control to fuse
	fuse_stat = fuse_main(args.argc, args.argv, &sfs_oper, state);
	syslog (LOG_INFO, "[main] fuse_main returned %d\n", fuse_stat);
	
	closelog();
	return fuse_stat;
}
