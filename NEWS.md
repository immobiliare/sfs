sfs 1.3.14
===============

* fuse:
  - batch_flush_seconds has been renamed to batch_flush_msec,
    now accepting milliseconds instead of seconds.
	This change BREAKS BC of the configuration.

* php-sync: support microseconds in the configuration with float numbers.

sfs 1.3.13
===============

* First public release.