# diffsnap

`diffsnap` is a lightweight, on-demand snapshot scheduler for OpenZFS. Instead of blindly relying on elapsed time, it queries the native ZFS `written` property to create snapshots only after a dataset has changed by a user-defined byte threshold. 

Designed to complement—rather than replace—comprehensive policy tools like Sanoid, `diffsnap` manages and prunes only its own snapshots, allowing it to safely coexist alongside existing retention systems.

## The Problem

Traditional time-based snapshotting struggles with high-frequency intervals. Capturing rapid-fire changes (e.g., every 15 minutes) over a long retention window (e.g., 30 days) forces ZFS to maintain 2,880 snapshots per dataset. While idle snapshots consume negligible block space, excessive snapshot counts bloat pool metadata and degrade the performance of core ZFS management commands.

## The `diffsnap` Approach

By monitoring how much data has been written since the most recent snapshot, `diffsnap` enforces an intelligent data-change threshold. 

Because background metadata processes (such as directory lock updates, protocol leases, or structural TXG syncs) can cause the `written` metric to creep up slightly on an idle dataset, a threshold buffer of `1000000` bytes (1MB) is recommended. This buffer also prevents small file deletions from unnecessarily triggering the snapshot engine. Setting the threshold to `0` will capture all modifications, including minor deletions.

## Features

* **Delta-Driven Automation:** Snapshots are generated only when the configured change threshold is met.
* **Granular Control:** Per-dataset scheduling intervals, retention counts, and byte thresholds.
* **Safety First:** Only prunes snapshots matching its own prefix context.
* **Hierarchy Aware:** Supports both standard and recursive snapshot execution paths.
* **Atomic Batching:** Pools concurrent dataset targets into single, atomic `zfs snapshot` invocations to minimize disk I/O overhead.
* **Zero Overhead:** Completely stateless; operates strictly via standard `zfs` CLI utilities with no background daemons required.
* **Concurrency Protection:** Uses `diffsnap.lock` to safely prevent overlapping runs.
* **System Native:** Easily integrated with `cron` or systemd timers.

## Supported systems

- OpenZFS on Linux
- FreeBSD 15.1
- FreeBSD 14.4

The Makefile selects OS-specific defaults at build time:

| File / Property | FreeBSD | Linux |
| --- | --- | --- |
| **Config** | `/usr/local/etc/diffsnap.conf` | `/etc/diffsnap.conf` |
| **Log** | `/var/log/diffsnap.log` | `/var/log/diffsnap.log` |
| **Lock** | `/var/run/diffsnap.lock` | `/run/diffsnap.lock` |
| **ZFS** | `/sbin/zfs` | `/usr/sbin/zfs` if present, otherwise `/sbin/zfs` |

## Build

```sh
make
```

Path defaults can be overridden at build time:

```sh
make CONF_PATH=/usr/local/etc/diffsnap.conf ZFS_PATH=/sbin/zfs
```

## Install

```sh
mdo make install
# or
sudo make install
```

By default this installs `/usr/local/sbin/diffsnap`

## Usage

```sh
diffsnap --help
diffsnap --version
diffsnap
```

## Configuration

```text
dataset interval_minutes retention prefix recursive min_bytes
```

Example:

```text
zroot/downloads 30 100 diffsnap no 1000000
tank/media 5 100 diffsnap no 1000000
zroot/jails 1440 14 daily yes 0
```

Fields:

- `dataset`: ZFS dataset name.
- `interval_minutes`: Intervals $\le$ 60: Must divide evenly into $60$ minutes. Intervals $>$ 60: Must divide evenly into $1,440$ minutes.
- `retention`: number of matching snapshots to keep.
- `prefix`: snapshot prefix using letters, numbers, `_`, or `-`.
- `recursive`: `yes` or `no`.
- `min_bytes`: minimum written bytes needed before snapshotting.

Blank lines and lines beginning with `#` are ignored.

## System Scheduler
diffsnap does not run as a continuous background service. It relies on an external system scheduler (such as a cron job on FreeBSD or a systemd timer on Linux) to wake it up and trigger execution.

For diffsnap to evaluate a dataset, the system scheduler must run the binary the minute that matches the dataset's configured interval_minutes. If not, the snapshot window is missed entirely. The system scheduler interval must divide evenly into your smallest dataset interval_minutes.

If your system schedule for diffsnap is 20 and your interval_minutes is 15 you'll only get one snapshot per hour instead of the intended 4.
If your system schedule for diffsnap is 10 and your interval_minutes is 5 you'll skip every other intended snapshot.
The easiest way to avoid these issues is to set the system schedule to run every minute.

The examples below schedule diffsnap to run as root. You can authorize an unprivileged user to execute zfs snapshot and zfs destroy commands using zfs allow. This permits the use of a user crontab or a non-root systemd timer, but it also requires manually adjusting filesystem permissions for the configuration and log files. These implementation steps are outside the scope of this document.

FreeBSD Crontab Configuration (/etc/cron.d/diffsnap)

```cron
* * * * * root /usr/local/sbin/diffsnap
```

Linux systemd Unit Files (/etc/systemd/system/)

```ini
# diffsnap.service
[Unit]
Description=Create ZFS snapshots using diffsnap

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/diffsnap
```

```ini
# diffsnap.timer
[Unit]
Description=Run diffsnap every minute

[Timer]
OnCalendar=*:*
Persistent=true

[Install]
WantedBy=timers.target
```

## License

BSD 2-Clause. See `LICENSE`.
