#!/bin/sh

# For each rsync operation, check whether the destination
# filesystem is mounted. Otherwise the operation fails
# and will be retried by the synchronization daemon.

# Put in rscynd.conf: pre-xfer exec = '/home/www-data/php-sync/rsync-check.sh'

CHECKFILE="/home/www-data/data/.sfs.mounted"

exec ls -1 "$CHECKFILE"
