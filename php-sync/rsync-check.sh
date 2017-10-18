#!/bin/sh

# For each rsync operation, check whether the destination
# filesystem is mounted. Otherwise the operation fails
# and will be retried by the synchronization daemon.

# Put in rscynd.conf: pre-xfer exec = '/home/www-data/php-sync/rsync-check.sh'

exec test -r "${RSYNC_MODULE_PATH}"/.sfs.mounted
