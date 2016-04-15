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
$ sfs /mnt/data /mnt/fuse
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

Prerequisites
--------------------
Install php, if not already present.

Make sure you have installed php together with:
- php-sysvmsg
- php-sysvsem
- php-sysvshm

Check the configuration directive `disable_functions` of `/etc/php.ini` for occurences of
- proc_open
- proc_close
- proc_get_status
- proc_nice
- proc_terminate
- posix_getpid
- system


Installation
--------------------

In this setup we're going to run the sync daemon only on the central node.
You can run the sync daemon using rsync over ssh or use the plain rsync transfer in trusted environments.

First copy the file `sfs-sync.php` into the directory `/usr/local/bin`.

If you want to use systemd to start the sync-daemon at system startup, copy the contents of `etc` to your systems etc, by running `cp -R etc/ /etc/`.
Our configuration files are located in `etc/sysconfig/sfs/`. We provide two example configs, one for plain rsync, and one usings rsync over ssh.


Configuring sfs-sync with plain rsync-transfer on the central node
--------------------

Use `etc/sysconfig/sfs/config.php.sample` copy it to `/etc/sysconfig/sfs/www.php` and change only the following lines:

```
"NODES" => array(
  "node2" => array("DATA" => "rsync://node2:8173/data/",
                   "BATCHES" => "rsync://node2:8173/batches/"),
  "node3" => array("DATA" => "rsync://node3:8173/data/",
                   "BATCHES" => "rsync://node3:8173/batches/")
),
```

IMPORTANT: Nodenames in NODES-Array MUST match the names configured in sfs.

Now we can start it via a direct call:

```
$ sfs-sync -c /etc/sysconfig/sfs/www.php -p /var/run/www.pid
```

You will notice an error, that `/mnt/data/.sfs.mounted` does not exist. The sfs-sync checks periodically for this file: if it does not exist, for safeness the sync is suspended until the file reappears:

```
$ touch /mnt/data/.sfs.mounted
```

**Note**: the sync daemon works only on the original filesystem, it knows nothing about the FUSE component.

Now you will see another error, the fact that sfs-sync cannot communicate with the other nodes. It tried to sync the only batch we have in `/mnt/batches` but failed, and will retry periodically. Now we have to setup rsyncd on other nodes to actually transfer files.


Configuring rsyncd on other nodes (only for plain rsync transfer!)
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

Edit `/home/sfs/sfs/php-sync/rsync-check.sh` to use `CHECKFILE="/mnt/data/.sfs.mounted"`. This file is used to check whether the original filesystem is effectively mounted:

```
touch /mnt/data/.sfs.mounted
```

If the file does not exist, rsync will refuse to transfer any file.

Start rsyncd on each node (only for plain rsync transfer!)
-------------------

We can start rsyncd on each node, and we're all done:

```
$ rsync --daemon --config /home/sfs/rsyncd.conf
```

Once `node1` realizes that the other two nodes are reachable, it will synchronize the batch we've written earlier and the file `/mnt/data/test` will be present on all nodes.

Find more about the implementation in the [DETAILS](DETAILS.md) page.


Configuring sfs-sync with ssh-rsync-transfer on the central node
--------------------

Use `etc/sysconfig/sfs/config-ssh.php.sample` copy it to `/etc/sysconfig/sfs/www.php` and and change only the following lines:

```
        "NODES" => array(
                "node2" => array("DATA" => 'node2:' . $DATADIR,
                        "BATCHES" => 'node2:' . $BATCHDIR
                ),
                "node3" => array("DATA" => 'node3:' . $DATADIR,
                        "BATCHES" => 'node3:' . $BATCHDIR
                ),
        ),
```

IMPORTANT: Nodenames in NODES-Array MUST match the names configured in sfs.

Now we can start it via a direct call:

```
$ sfs-sync -c /etc/sysconfig/sfs/www.php -p /var/run/www.pid
```

You will notice an error, that `/mnt/data/.sfs.mounted` does not exist. The sfs-sync checks periodically for this file: if it does not exist, for safeness the sync is suspended until the file reappears:

```
$ touch /mnt/data/.sfs.mounted
```

**Note**: the sync daemon works only on the original filesystem, it knows nothing about the FUSE component.

Now you will see another error, the fact that sfs-sync cannot communicate with the other nodes. It tried to sync the only batch we have in `/mnt/batches` but failed, and will retry periodically. Now we have to setup sshd on other nodes to actually transfer files.


Configuring ssh for ssh-rsync-transfer
-------------------
We assume you know how to configure ssh in principle.
Since the filetransfer is unable to pipe passwords or passphrases, you have to provide generate a private key and put the key in ~/.ssh directory. For our setup the public key of node1 must be in ~/.ssh/authorized_keys of node2 and node3.

From node1 you should try to do execute
```
$ ssh node2
```

and the same for node3
```
$ ssh node3
```

Both must work in order to use the transfer later on. Keep in mind the key on node1 must be in /root/.ssh directory if you run it by root, or in the user's directory if you drop the rights to a specific user.

After this configuration, no service restart is needed and the transfer should work.
Since node1 polls quite often, the ssh connection would be genereated quite often. To overcome this, you sould add these lines to `~/.ssh/config`:

```
Host *
  ControlMaster auto
  ControlPersist 10
  ControlPath ~/.ssh/master-%r@%h:%p
```

This will prevent ssh to disconnect from the other nodes. The connection is kept for 10 seconds. Change this according to your sfs-sync-configuration.


Using systemd to start sfs-sync
-------------------

Assuming our configuration named `www` is finished, we may not want to start sfs-sync via systemd. We can do this, by calling

```
$ systemctl start sfs-sync@www
```

or enable it to start automatically on system-startup

```
$ systemctl enable sfs-sync@www
```