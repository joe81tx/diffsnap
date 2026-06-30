# diffsnap

`diffsnap` creates and prunes ZFS snapshots from a simple space-separated
configuration file.

Version: 1.0

## Supported systems

- Linux with ZFS
- FreeBSD 15.1
- FreeBSD 14.4

The program expects `zfs` at `/sbin/zfs`.

## Build

```sh
make
```

## Install

```sh
sudo make install
```

By default this installs:

- Binary: `/usr/local/sbin/diffsnap`
- Config: `/usr/local/etc/diffsnap.conf`

## Usage

```sh
diffsnap --help
diffsnap --version
diffsnap
```

Normal execution reads `/usr/local/etc/diffsnap.conf`, writes logs to
`/var/log/diffsnap.log`, and uses `/var/run/diffsnap.lock` to avoid overlapping
runs.

## Configuration

Config fields must be space separated:

```text
dataset interval_minutes retention prefix recursive min_bytes
```

Example:

```text
zroot/home 60 24 hourly no 0
zroot/jails 1440 14 daily yes 1048576
```

Fields:

- `dataset`: ZFS dataset name.
- `interval_minutes`: run interval from 1 to 1440 minutes.
- `retention`: number of matching snapshots to keep.
- `prefix`: snapshot prefix using letters, numbers, `_`, or `-`.
- `recursive`: `yes` or `no`.
- `min_bytes`: minimum written bytes needed before snapshotting.

Blank lines and lines beginning with `#` are ignored.

## Cron

Example cron line:

```cron
*/15 * * * * /usr/local/sbin/diffsnap
```

## License

BSD 2-Clause. See `LICENSE`.
