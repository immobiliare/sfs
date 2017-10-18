Sample setup
============

For reference we'll provide an example of SFS setup for three nodes, one of which is the central node running the sync daemon.

On all nodes, we'll refer to:

- the storage containing the real data as `/mnt/data`
- the batches directory as `/mnt/batches`, and `/mnt/batches/tmp` for temporary batches
- the FUSE mountpoint as `/mnt/fuse`
- the SFS user as `sfs`

Therefore we'll create the above dirs for our deployment:

```
# mkdir -p /mnt/data /mnt/batches/tmp /mnt/fuse
# chown sfs /mnt/data /mnt/batches /mnt/batches/tmp /mnt/fuse
```

It is preferable that batches are in the same filesystem of the original data, so that if batch files cannot be written due to full disk
space, neither the data files can be written. SFS will not prevent the data from being written if batches cannot be written.

However SFS will log to syslog which file names should have been written to batches. Beware, this may fill up your logs without
proper rotation.

We call the nodes `node1`, `node2` and `node3`. The `node1` is the one that will be running the sync daemon.

**Note**: running the sync daemon on a single node is not a SPOF. The sync daemon can be run on any other node if the failure is permanent.
Given the replication system is asynchronous, a crash of the sync daemon is acceptable, as long as you have enough disk space and inodes
to hold batch files until the sync daemon comes back up.

Installation
============

System requirements
-------------

- Linux kernel >= 2.6.26
- FUSE >= 2.8.1
- rsync >= 3.1.0
- ntpd is recommended

It's been tested on Debian squeeze, wheezy and jessie. For Debian squeeze and wheezy there's no backport of rsync 3.1.0, our approach was to simply create a jessie chroot with the rsync 3.1.0 binary.

The following steps are assumed to run on Debian jessie, but the same steps may be adapted to any other Linux distribution.

Compiling
------------

```
# apt-get install rsync fuse build-essential git libfuse-dev pkg-config
$ cd /home/sfs
$ git clone https://github.com/immobiliare/sfs
$ cd /home/sfs/sfs/fuse
$ make -j
```

You can now run `./sfs --version`. Installation is not required.

Configuring FUSE
------------

If `/dev/fuse` is owned by the fuse group, you may add the sfs user to the this group. Otherwise find a way to let
the sfs user open `/dev/fuse`, for example:

```
# groupadd fuse
# usermod -G fuse sfs
# chgrp fuse /dev/fuse
# chmod g+rw /dev/fuse
```

SFS-FUSE component
================

Configuring SFS-FUSE
----------------

On each node we will run the SFS-FUSE component. The configuration is written in the filesystem, `/mnt/data/.sfs.conf`:

```
[sfs]
pid_path=/var/run/sfs/sfs.pid
batch_dir=/mnt/batches
batch_tmp_dir=/mnt/batches/tmp
batch_max_events=1000
batch_max_bytes=50000000
batch_flush_msec=1000
ignore_path_prefix=/.tmp
node_name=node1
use_osync=1
```

Create the rundir for SFS:

```
# mkdir -p /var/run/sfs
# chown sfs /var/run/sfs
```

Starting SFS-FUSE
---------------

We can finally mount the filesystem as sfs user on each node:

```
$ sfs -o kernel_cache,use_ino /mnt/data /mnt/fuse
```

Test it by writing a file in the FUSE mountpoint, and checking it created a batch in `/mnt/batches/tmp`:

```
$ echo foo > /mnt/fuse/test
$ cat /mnt/data/test
foo
$ ls /mnt/batches/tmp/
1418212060_node1_node1_1693_00000_norec.batch
```

After 1 second, the batch will be moved into `/mnt/batches`:

```
$ cat /mnt/batches/1418212060_node1_node1_1693_00000_norec.batch 
/test
```

Batches will remain there until some other program consumes them. Once the batches are flushed (marked as completed), they are unmanaged by SFS-FUSE.

### Allowing other users to write to the filesystem ###

Mounted FUSE filesystems are usually available only to the user that mounted it. If you want the filesystem to be
accessible to all other users in the system:

	1. Edit `/etc/fuse.conf` and enable `user_allow_other`
	2. Add the `-o allow_other` mount option

Beware that due to FUSE semantics, all the files are owned by the SFS user mounting the filesystem.

If you want the filesystem to preserve the user permissions, you may run sfs as root (which is **not recommended**) and add `--perms -o default_permissions` to the command line.

Stopping SFS-FUSE
---------------

The FUSE filesystem can be umounted with:

```
$ fusermount -u /mnt/fuse
```

Pending batches under `/mnt/batches/tmp` will be automatically moved to `/mnt/batches` on the next SFS startup.

**Note**: `kill -9` of the process may result in loss of batches if `use_osync` is not enabled in the configuration.


Adding SFS-FUSE to fstab
---------------
The FUSE filesystem can be added to the /etc/fstab as follows, if the executable is copied to e.g. /usr/local/bin.

Either for a specific user with id 1000 in group 1000:
```
sfs#/mnt/data /mnt/fuse fuse noatime,sfs_uid=1000,sfs_gid=1000 0 0
```
The filesystem will be mounted at startup and the sfs process is owned by the user with the id 1000, group 1000, so every file access on /mnt/data is issued by this user.

Or with normal users ownership:
```
sfs#/mnt/data /mnt/fuse fuse noatime,allow_other,sfs_perms 0 0
```
In this case the filesystem will be mounted at startup, and the sfs process will run as root. Be aware this can cause a security problem if any bufferoverflow in fuse or sfs is discovered.


Sync daemon component
===============

Configuring php-sync on the central node
--------------------

In this setup we're going to run the sync daemon only on the central node.

Write `/home/sfs/sfs/php-sync/config.php`, in the same directory of sync.php:

```
<?php
$RSYNC_OPTS = "-ltpDcuhRO --exclude /.sfs.conf --exclude /.sfs.mounted --delete-missing-args --delete-delay --files-from=%b %s %d";
$CONFIG = array(
"SYNC_DATA_NOREC" => "rsync -d --no-r $RSYNC_OPTS",
"SYNC_DATA_REC" => "rsync -r $RSYNC_OPTS",
"PULL_BATCHES" => "rsync -acduhRO --remove-source-files --include='./' --include='*.batch' --exclude='*' %s %d",
"ACCEPT_STATUS" => array(0, 24),
"NODES" => array(
  "node2" => array("DATA" => "rsync://node2:8173/data/",
                   "BATCHES" => "rsync://node2:8173/batches/"),
  "node3" => array("DATA" => "rsync://node3:8173/data/",
                   "BATCHES" => "rsync://node3:8173/batches/")
),

"PUSHPROCS" => 4,
"PUSHCOUNT" => 10,
"PULLCOUNT" => 3,

"BULK_OLDER_THAN" => 60,
"BULK_MAX_BATCHES" => 100,

"DATADIR" => "/mnt/data/",
"BATCHDIR" => "/mnt/batches",
"CHECKFILE" => "/mnt/data/.sfs.mounted",
"SCANTIME" => 1,
"FAILTIME" => 10,
"LOG_IDENT" => "sfs-sync(%n)",
"LOG_OPTIONS" => LOG_PID|LOG_CONS|LOG_PERROR,
"LOG_FACILITY" => LOG_DAEMON,
"LOG_DEBUG" => false,
"DRYRUN" => false
);
?>
```

Now we can start it:

```
$ php /home/sfs/sfs/php-sync/sync.php
```

You will notice an error, that `/mnt/data/.sfs.mounted` does not exist. The php-sync checks periodically for this file: if it does not exist, for safeness the sync is suspended until the file reappears:

```
$ touch /mnt/data/.sfs.mounted
```

**Note**: the sync daemon works only on the original filesystem, it knows nothing about the FUSE component.

Now you will see another error, the fact that php-sync cannot communicate with the other nodes. It tried to sync the only batch we have in `/mnt/batches` but failed, and will retry periodically. Now we have to setup rsyncd (or sshd) on other nodes to actually transfer files.

Configuring rsyncd on other nodes
-------------------

Assuming this storage setup is used in an internal network, it's better to use rsyncd behind some firewall rules for better performance. In this setup the rsync daemon will be started on all nodes, except `node1`.

Create an `/home/sfs/rsyncd.conf` like this:

```
max connections = 10
munge symlinks = no
reverse lookup = no
transfer logging = no
max verbosity = 0
use chroot = false
exclude = .sfs.conf
read only = false
port = 8173
lock file = /home/sfs/rsyncd.lock
log file = /home/sfs/rsyncd.log
pid file = /home/sfs/rsyncd.pid

pre-xfer exec = '/home/sfs/sfs/php-sync/rsync-check.sh'

[data]
    path = /mnt/data

[batches]
    path = /mnt/batches
```

The `/home/sfs/sfs/php-sync/rsync-check.sh` script is used to check whether the filesystems are effectively mounted:

```
touch /mnt/data/.sfs.mounted /mnt/batches/.sfs.mounted
```

If these files do not exist, rsync will refuse to transfer any file.

Start rsyncd on each node
-------------------

We can start rsyncd on each node, and we're all done:

```
$ rsync --daemon --config /home/sfs/rsyncd.conf
```

Once `node1` realizes that the other two nodes are reachable, it will synchronize the batch we've written earlier and the file `/mnt/data/test` will be present on all nodes.

Find more about the implementation in the [DETAILS](DETAILS.md) page.
