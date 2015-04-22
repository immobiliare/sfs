SFS Asynchronous filesystem replication
===================

SFS (or SyncFS) is a filesystem for replicating files between geographically distributed nodes of a network, with multi-master active/active configuration and especially suited for a star network topology.

SFS is designed for GNU/Linux systems and is based on [FUSE](http://fuse.sourceforge.net/) (Filesystem in Userspace) and [rsync](http://rsync.samba.org/).

SFS is developed and maintained by Immobiliare.it.

Characteristics
-------------

- Transparent replication layer on top of an existing filesystem
- Quorum is not a requirement
- No locks involved
- Eventually consistent
- Introspectable state from simple text files
- Clients can read and write on any node of the network
- Supports multiple kinds of network setups
- Manages and synchronizes terabytes of data

Target audience
-------------

SFS works well for replicating a storage filesystem holding images or documents uploaded by users, where such users are unlikely to update the same file at the same time on two different nodes.

Immobiliare.it is using SFS for synchronizing images between nodes in the same LAN with low timer settings, and for nodes in the WAN with higher timer settings.

System requirements
-------------

- Linux kernel >= 2.6.26
- FUSE >= 2.8.1
- rsync >= 3.1.0
- ntpd is recommended
- PHP >= 5.3

It's been tested on Debian squeeze, wheezy and jessie. For Debian squeeze and wheezy there's no backport of rsync 3.1.0, our approach was to simply create a jessie chroot with the rsync 3.1.0 binary.

Installation
-------------

SFS may appear a little contrived to install, but the concepts are very simple and everything will be straightforward after your first setup.

Installation instructions for a network of three nodes can be found in the [INSTALL](docs/INSTALL.md) page.

How it works
-------------

SFS has two main components: the FUSE component and the sync daemon component.

### The FUSE layer ###

The FUSE component is written in C and its only purpose is to register write events that happen to the files on the local storage. SFS acts as a proxy to a local filesystem, thus the original filesystem is still accessible, while the FUSE layer is mounted to a different directory.
The events are simply the file names that have seen a write and are written in **batch** files. There may be multiple *closed* batches waiting for synchronization, and few temporary *opened* batches still filling up with file names.

The FUSE component is critical and must be fast in order to be transparent to the end user, it only serves as a proxy for registering writes to the files. Since it's not memory-expensive, it is recommended to disable the OOM killer for this process.

If FUSE cannot write batch files, it will log the failure to syslog, however the data will still be written on disk because the process must be transparent to the user.

### The synchronization daemon ###

The sync daemon component is used for synchronizing the changes between multiple nodes and is cluster-aware. In a star topology, there is a single sync daemon on one node, and rsyncd (or sshd) on other nodes. This component is not critical and can be stopped at any time.

The sync daemon periodically reads closed batches written by the local FUSE component, and spawns rsync processes to synchronize the files from the local storage to the other remote nodes. In an active/active setup, the same sync daemon also reads batches written by remote FUSE components and spawns rsync processes to synchronize the files from the remote storage to the local storage.

The sync daemon reads the batches list in order and stops at the first batch that fails to be delivered for a particular node. That batch will be retried after some time indefinitely until it succeeds. Once a batch succeeds it is unlinked from the batches directory.

If the synchronization process is behind a certain amount of time, multiple batches are bulked together up to a certain limit to improve network efficiency and spawn less rsync processes.

The rsync processes are called on the original filesystems only for the modified files and is thus very efficient.

Find more about the implementation in the [DETAILS](docs/DETAILS.md) page.

Consistency model
-------------

Because of no locks, the last write wins according to rsync update semantics based on file timestamp + checksum. In case two files have the same mtime, rsync compares the checksum to decide which one wins.

Because quorum is not a requirement, a split brain of all nodes can happen and clients can keep reading and writing on the filesystems. Once all nodes communicate again, they will synchronize.

The storage is supposed to be inconsistent at any time whenever clients write on any of the nodes, because the replication is asynchronous. Due to this, it's preferable that the same client reads and writes on the same node within a session lifetime.

The storage is supposed to be consistent at a given time when all nodes are able to communicate and no client is writing on any node.

Known problems
-------------

### Renaming is slow ###

The sync daemon only spawns rsync processes, therefore renaming files or directories imply a copy of the files on the destination node. In the case of single files this is no problem, but for whole directory trees it is very inefficient.
On the other hand, renaming may lead to inconsistencies whereas deleting and uploading again is consistent.

For example two servers have the same directory `x`. At the same time, the first server renames `x` to `y`, and the second to `z`.

Now if there was a protocol which executed the rename on the destination, the final result would be `y` on first server, and `z` on second server, which is not easily recoverable.

Without an atomic rename on the destination, `y` would be copied to the second server, and `z` would be copied to the first server, leading to both `y` and `z` on both servers. Duplicated files, but consistent.

### Parallelism of the sync daemon ###

The sync daemon is quite limited in parallelism in that it allows only one pull at time, and one push per destination node. Also it's only possible to do either a pull or a push at time. This is to ensure consistency of all the filesystems.

### Ownership of files ###

If you are running SFS as simple user instead of root (running as root is not recommended), all the files are owned by that user.

Running SFS as root not only may provide unknown exploitation vectors, but is also slower.

License
-------------

Copyright © 2014  Immobiliare.it S.p.A.

Licensed under a GPL3+ license: http://www.gnu.org/licenses/gpl-3.0.txt