sfs 1.4.1
===============

* fuse:
  - pid_path is now optional
  - fixed compilation on Debian Lenny
  - support fstab mount
  - prettier mount output
  - support dropping privileges to uid and gid
  - create recursive batches only when directories are renamed

* php-sync:
  - allow per-node BULK_MAX_BATCHES and rsync commands
  - do not scan the whole batches directory

sfs 1.4.0
===============

* fuse:
  - batch_flush_seconds has been renamed to batch_flush_msec,
    now accepting milliseconds instead of seconds.
	This change BREAKS BC of the configuration.

* php-sync: support microseconds in the configuration with float numbers.

sfs 1.3.13
===============

* First public release.