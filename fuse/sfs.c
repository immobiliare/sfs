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


#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include "sfs.h"

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif

#include <libgen.h>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <sys/file.h> /* flock(2) */

#include <pthread.h>
#include <syslog.h>
#include <stddef.h>

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
static int sfs_getattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi) {
	int res = 0;

	BEGIN_PERM;
	if(fi)
		res = fstat(fi->fh, stbuf);
	else {
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);
	res = lstat(fpath, stbuf);
	}
	END_PERM;
	return (res < 0 ? -errno : 0);
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
static int sfs_readlink(const char *path, char *buf, size_t size) {
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = readlink(fpath, buf, size - 1);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	buf[res] = '\0';
   return 0;
}

/** Create a file node
*
* There is no create() operation, mknod() will be called for
* creation of all non-directory, non-symlink nodes.
*/
// shouldn't that comment be "if" there is no.... ?
static int sfs_mknod(const char *path, mode_t mode, dev_t dev) {
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	if (S_ISFIFO(mode))
		res = mkfifo(fpath, mode);
	else
		res = mknod(fpath, mode, dev);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");

	return 0;
}

/** Create a directory */
static int sfs_mkdir(const char *path, mode_t mode) {
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = mkdir(fpath, mode);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");

	return 0;
}

/** Remove a file */
static int sfs_unlink(const char *path) {
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = unlink(fpath);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");
	return 0;
}

/** Remove a directory */
static int sfs_rmdir(const char *path) {
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = rmdir(fpath);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");

	return 0;
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
static int sfs_symlink(const char *from, const char *to) {
	int res;
	char flink[PATH_MAX];
	sfs_fullpath(flink, to);

	BEGIN_PERM;
	res = symlink(from, flink);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (to, "norec");

	return 0;
}

/** Rename a file */
// both path and newpath are fs-relative
static int sfs_rename(const char *path, const char *newpath, unsigned int flags) {
	int res;
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
	res = rename(fpath, fnewpath);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, mode);
	batch_file_event (newpath, mode);

	return 0;
}

/** Create a hard link to a file */
static int sfs_link(const char *path, const char *newpath) {
	int res;
	char fpath[PATH_MAX], fnewpath[PATH_MAX];
	sfs_fullpath(fpath, path);
	sfs_fullpath(fnewpath, newpath);

	BEGIN_PERM;
	res = link(fpath, fnewpath);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (newpath, "norec");
	//add old path as event too, this will ensure on target machine with rsync -h the hardlink is created as well
	batch_file_event(path, "norec");

	return 0;
}

/** Change the permission bits of a file */
static int sfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	if(fi)
		res = fchmod(fi->fh, mode);
	else{
		res = chmod(fpath, mode);
	}
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	sfs_update_mtime ("chmod", fpath);
	batch_file_event (path, "norec");

	return 9;
}

/** Change the owner and group of a file */
static int sfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	if (fi)
		res = fchown(fi->fh, uid, gid);
	else{
		res = chown(fpath, uid, gid);
	}
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	sfs_update_mtime ("chown", fpath);
	batch_file_event (path, "norec");

	return 0;
}

/** Change the size of a file */
static int sfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
	int res = 0;

	BEGIN_PERM;
	if(fi)
		res = ftruncate(fi->fh, size);
	else{
		char fpath[PATH_MAX];
		sfs_fullpath(fpath, path);
		res = truncate(fpath, size);
	}
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");

	return 0;
}

#ifdef HAVE_UTIMENSAT
// Copied fom http://fuse.sourceforge.net/doxygen/fusexmp__fh_8c.html
static int sfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
	int res = 0;
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
	res = utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW);
	END_PERM;
	if (res < 0) {
		res = -errno;
	} else {
		batch_file_event (path, "norec");
	}

	return res;
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
static int sfs_open(const char *path, struct fuse_file_info *fi) {
	int res = 0;
	int fd;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	fd = open(fpath, fi->flags);
	END_PERM;
	if (fd < 0) {
		res = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[open] opened fds %d\n", opened_fds);
		}
	}

	fi->fh = fd;

	return res;
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
static int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	int res = 0;

	if (fi->direct_io) {
		res = pread(fi->fh, buf, size, offset);
		if (res < 0) {
			res = -errno;
		}
	} else {
		while (res < size) {
			int cur = pread(fi->fh, buf, size-res, offset+res);
			if (cur <= 0) {
				if (cur < 0) {
					res = -errno;
				}
				break;
			}
			res += cur;
		}
	}

	return res;
}

static int sfs_read_buf(const char *path, struct fuse_bufvec **bufp,
			size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec *src;

	(void) path;

	src = malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;

	*src = FUSE_BUFVEC_INIT(size);

	src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;

	*bufp = src;

	return 0;
}

/** Write data to an open file
*
* Write should return exactly the number of bytes requested
* except on error.  An exception to this is when the 'direct_io'
* mount option is specified (see read operation).
*
* Changed in version 2.2
*/
static int sfs_write(const char *path, const char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi) {
	int res = 0;

	if (fi->direct_io) {
		res = pwrite (fi->fh, buf, size, offset);
		if (res < 0) {
			return -errno;
		}
	} else {
		int cur;
		while (res < size) {
			cur = pwrite (fi->fh, buf, size-res, offset+res);
			if (cur <= 0) {
				if (cur < 0) {
					return -errno;
				}
				break;
			}
			res += cur;
		}
	}

	if (res > 0) {
		batch_bytes_written (res);
	}

	return res;
}

static int sfs_write_buf(const char *path, struct fuse_bufvec *buf,
		     off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

	(void) path;

	dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	dst.buf[0].fd = fi->fh;
	dst.buf[0].pos = offset;

	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}


/** Get filesystem statistics
*
* The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
*
* Replaced 'struct statfs' parameter with 'struct statvfs' in
* version 2.5
*/
static int sfs_statfs(const char *path, struct statvfs *statv) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	// get stats for underlying filesystem
	res = statvfs(fpath, statv);
	END_PERM;

	return (res < 0 ? -errno : 0);
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
static int sfs_flush(const char *path, struct fuse_file_info *fi) {
	int res = 0;
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fi->fh));
	if (res == -1)
		return -errno;

	return 0;
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
static int sfs_release(const char *path, struct fuse_file_info *fi) {
	int res;
	res = close(fi->fh);
	if (res < 0) {
		return -errno;
	}
		if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR)) {
			batch_file_event (path, "norec");
		}

		SfsState* state = SFS_STATE;
		int opened_fds = __sync_sub_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[close] opened fds %d\n", opened_fds);
		}


	return 0;
}

/** Synchronize file contents
*
* If the datasync parameter is non-zero, then only the user data
* should be flushed, not the meta data.
*
* Changed in version 2.2
*/
static int sfs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
	int res = 0;

	if (datasync) {
		res = fdatasync(fi->fh);
	} else {
		res = fsync(fi->fh);
	}

	return (res < 0 ? -errno : 0);
}

#ifdef HAVE_FALLOCATE
static int sfs_fallocate(const char *path, int mode,
						 off_t offset, off_t length, struct fuse_file_info *fi) {
	int res;

	(void) path;
	res = fallocate (fi->fh, mode, offset, length);
	if (res < 0) {
		res = -errno;
	}

	return res;
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
static int sfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = lsetxattr(fpath, name, value, size, flags);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
		batch_file_event (path, "norec");

	return 0;
}

/** Get extended attributes */
static int sfs_getxattr(const char *path, const char *name, char *value, size_t size) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = lgetxattr(fpath, name, value, size);
	END_PERM;
	return (res < 0 ?  -errno : 0);
}

/** List extended attributes */
static int sfs_listxattr(const char *path, char *list, size_t size) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = llistxattr(fpath, list, size);
	END_PERM;
	return (res < 0 ? -errno : 0);
}

/** Remove extended attributes */
static int sfs_removexattr(const char *path, const char *name) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = lremovexattr(fpath, name);
	END_PERM;
	if (res < 0) {
		return -errno;
	}
	batch_file_event (path, "norec");

	return 0;
}

struct sfs_dirp {
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

/** Open directory
*
* This method should check if the open operation is permitted for
* this  directory
*
* Introduced in version 2.3
*/
static int sfs_opendir(const char *path, struct fuse_file_info *fi) {
	DIR *dp;
	int res = 0;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	dp = opendir(fpath);
	END_PERM;
	if (dp == NULL) {
		res = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[opendir] opened fds %d\n", opened_fds);
		}
	}

	fi->fh = (intptr_t) dp;

	return res;
}

static inline struct sfs_dirp *get_dirp(struct fuse_file_info *fi)
{
	return (struct sfs_dirp *) (uintptr_t) fi->fh;
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
static int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	                   off_t offset, struct fuse_file_info *fi,
	                   enum fuse_readdir_flags flags)
{
	struct sfs_dirp *d = get_dirp(fi);

	(void) path;
	if (offset != d->offset) {
#ifndef __FreeBSD__
		seekdir(d->dp, offset);
#else
		/* Subtract the one that we add when calling
		   telldir() below */
		seekdir(d->dp, offset-1);
#endif
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		struct stat st;
		off_t nextoff;
		enum fuse_fill_dir_flags fill_flags = 0;

		if (!d->entry) {
			d->entry = readdir(d->dp);
			if (!d->entry)
				break;
		}
#ifdef HAVE_FSTATAT
		if (flags & FUSE_READDIR_PLUS) {
			int res;

			res = fstatat(dirfd(d->dp), d->entry->d_name, &st,
				      AT_SYMLINK_NOFOLLOW);
			if (res != -1)
				fill_flags |= FUSE_FILL_DIR_PLUS;
		}
#endif
		if (!(fill_flags & FUSE_FILL_DIR_PLUS)) {
			memset(&st, 0, sizeof(st));
			st.st_ino = d->entry->d_ino;
			st.st_mode = d->entry->d_type << 12;
		}
		nextoff = telldir(d->dp);
#ifdef __FreeBSD__
		/* Under FreeBSD, telldir() may return 0 the first time
		   it is called. But for libfuse, an offset of zero
		   means that offsets are not supported, so we shift
		   everything by one. */
		nextoff++;
#endif
		if (filler(buf, d->entry->d_name, &st, nextoff, fill_flags))
			break;

		d->entry = NULL;
		d->offset = nextoff;
	}

	return 0;

}

/** Release directory
*
* Introduced in version 2.3
*/
static int sfs_releasedir (const char *path, struct fuse_file_info *fi) {
	int res;
	res = closedir((DIR *) (uintptr_t) fi->fh);
	if (res < 0) {
		return -errno;
	}
	SfsState* state = SFS_STATE;
	int opened_fds = __sync_sub_and_fetch (&state->opened_fds, 1);
	if (state->log_debug) {
		syslog (LOG_DEBUG, "[closedir] opened fds %d\n", opened_fds);
	}

	return 0;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 * caused by mount option dirsync, which causes directory operations e.g. mkdir to be synchronous
 */

static int sfs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
	int res;
	DIR *dp;
	int fd;

	dp = (DIR *) (uintptr_t) fi->fh;
	fd = dirfd (dp);

	if (datasync) {
		res = fdatasync(fd);
	} else {
		res = fsync(fd);
	}

	return (res < 0 ?  -errno : 0);
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
static void *sfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
(void) conn;

	SfsState* state = SFS_STATE;
	state->pid = getpid ();
	cfg->use_ino = 1;
	cfg->nullpath_ok = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	openlog (state->log_ident, LOG_PID, state->log_facility);
	syslog (LOG_INFO, "[main] started sfs");

	// write pid file
	const char* pidpath = state->pid_path;
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
static void sfs_destroy (void *userdata) {
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
static int sfs_access(const char *path, int mask) {
	int res;
	char fpath[PATH_MAX];
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	res = euidaccess (fpath, mask);
	END_PERM;
	return (res < 0 ? -errno : 0);
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
static int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	int res = 0;
	char fpath[PATH_MAX];
	int fd;
	sfs_fullpath(fpath, path);

	BEGIN_PERM;
	fd = open(fpath, fi->flags, mode);
	END_PERM;
	if (fd < 0) {
		res = -errno;
	} else {
		SfsState* state = SFS_STATE;
		int opened_fds = __sync_add_and_fetch (&state->opened_fds, 1);
		if (state->log_debug) {
			syslog (LOG_DEBUG, "[creat] opened fds %d\n", opened_fds);
		}
	}

	fi->fh = fd;

	return res;
}

static struct fuse_operations sfs_oper = {
	.getattr = sfs_getattr,
	.readlink = sfs_readlink,
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
	.write_buf = sfs_write_buf,
	.read_buf = sfs_read_buf,
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

static void sfs_usage() {
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
