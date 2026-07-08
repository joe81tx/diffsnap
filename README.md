# diffsnap

`diffsnap` is a lightweight, on-demand snapshot scheduler for OpenZFS. Instead of relying on elapsed time alone, it queries the native ZFS `written` property (bytes modified since the previous snapshot) to create snapshots only after a dataset has changed by a user-defined threshold. 

Designed to complement comprehensive policy tools like Sanoid, `diffsnap` manages and prunes only its own snapshots. It safely coexists with other snapshots like those you create manually.

## The Problem

Traditional time-based snapshotting struggles with high-frequency intervals. Capturing short windows (e.g., every 15 minutes) over a long retention window (e.g., 30 days) forces you to maintain 2,880 snapshots per dataset. While idle snapshots consume negligible block space, excessive snapshot counts bloat pool metadata and can degrade the performance of ZFS commands.

## The diffsnap approach

By monitoring how much data has been written since the most recent snapshot, `diffsnap` enforces a configurable data-change threshold.

Because background metadata processes (such as directory lock updates, protocol leases, or structural TXG syncs) can cause the `written` metric to creep up slightly on an idle dataset, a threshold buffer of `1000000` bytes (1MB) is recommended. This buffer also prevents small file deletions from unnecessarily triggering the snapshot engine. Setting the threshold to `1` causes every detected change, including deletions and metadata updates, to qualify for snapshot creation.

## Features

* **Delta-Driven Automation:** Snapshots are generated only when the configured change threshold is met.
* **Granular Control:** Per-dataset scheduling intervals, retention counts, and byte thresholds.
* **Safety First:** Only prunes snapshots matching its own prefix context and locks to prevent overlapping runs.
* **Hierarchy Aware:** Supports both standard and recursive snapshots.
* **Atomic Batching:** Groups compatible datasets into a single `zfs snapshot` invocation for consistent snapshot timestamps and reduced command overhead.
* **Low Overhead:** Completely stateless; operates strictly via standard `zfs` CLI utilities with no background daemon.
* **System Native:** Easily integrated with `cron` or systemd timers.

## Supported systems

- FreeBSD 14 & 15
- OpenZFS on Linux

The Makefile selects OS-specific defaults at build time:

| File / Property | FreeBSD | Linux |
| --- | --- | --- |
| **Config** | `/usr/local/etc/diffsnap.conf` | `/etc/diffsnap.conf` |
| **Log*** | `/var/log/diffsnap.log` | `/var/log/diffsnap.log` |
| **Lock** | `/var/run/diffsnap.lock` | `/run/diffsnap.lock` |
| **ZFS** | `/sbin/zfs` | `/usr/sbin/zfs` if present, otherwise `/sbin/zfs` |

*Overriding the log file requires manual updates to the log rotation config after installation

Path defaults can be overridden at build time:

```sh
make CONF_PATH=/usr/local/etc/diffsnap.conf ZFS_PATH=/sbin/zfs
```

## Build

```sh
git clone https://github.com/joe81tx/diffsnap.git
cd diffsnap
make
```

## Install

```sh
mdo make install
# or
sudo make install
```

By default this installs `/usr/local/sbin/diffsnap`.

## Usage

```sh
diffsnap --help
diffsnap --version
# Process all configured datasets (run as root)
diffsnap
```

## Configuration

```sh
/usr/local/etc/diffsnap.conf   # FreeBSD  
/etc/diffsnap.conf             # Linux  
```

```text
dataset interval_minutes retention prefix recursive min_bytes
```
Snapshots will be named `dataset@prefix_date_time`

Fields:

- `dataset`: ZFS dataset name.
- `interval_minutes`: Minutes between snapshots - intervals carry over hour boundaries and reset at midnight. 50 evaluates at 00:00 00:50 01:40... 23:20 00:00 (not 00:10). Values greater than 1439 only match at midnight.
- `retention`: number of matching snapshots to keep.
- `prefix`: snapshot prefix using letters, numbers, `_`, or `-`. To avoid pruning snapshots created outside of `diffsnap` make sure this is unique.
- `recursive`: `yes` or `no`.
- `min_bytes`: minimum written bytes needed before snapshotting. 1000000 is a good starting point to avoid metadata changes creating unwanted snapshots. Using 1 instead captures any change.

If recursive datasets overlap with the same snapshot prefix the ancestor configuration takes precedence and descendant entries are ignored.

Blank lines and lines beginning with `#` are ignored.

Example config:  

```text
zroot/jails 30 100 diffsnap no 1000000
tank/media 5 100 diffsnap no 1000000
rpool/USERDATA 1440 14 daily yes 1
```

## Scheduling
`diffsnap` does not run as a continuous background service. It relies on an external system scheduler such as a cron job on FreeBSD or a systemd timer on Linux. `diffsnap` only evaluates datasets when it is invoked. If the scheduler doesn't run `diffsnap` at the expected interval time, that evaluation is skipped. The system scheduler interval must divide evenly into your smallest dataset interval_minutes.

For example, if `diffsnap` is scheduled every 20 minutes (0, 20, 40), datasets configured for 15-minute intervals (0, 15, 30, 45) will only overlap at 0 and the other 3 intervals will be skipped. Running `diffsnap` every minute avoids these unintential skips.

The examples below schedule `diffsnap` to run as root. You can authorize an unprivileged user to execute zfs snapshot and zfs destroy commands using `zfs allow`. This permits the use of a user crontab or a non-root systemd timer, but it also requires manually adjusting filesystem permissions for the configuration and log files. These implementation steps are outside the scope of this document.

FreeBSD Crontab Configuration  

/usr/local/etc/cron.d/diffsnap will be installed automatically. Modify it to adjust the default from every minute or comment the line with a leading # to disable running `diffsnap` automatically.
```cron
* * * * * root /usr/local/sbin/diffsnap
```

Linux systemd Configuration:

```sh
sudo systemctl enable diffsnap.timer --now
```

## License

BSD 2-Clause. See `LICENSE`.
