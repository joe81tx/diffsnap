#!/bin/bash
# chunking_integration_test.sh
#
# STANDALONE integration test for diffsnap's snapshot-batch chunking
# (ARGV_BYTES_CAP). This is deliberately NOT part of freebsd.sh / linux.sh:
# it creates several hundred real ZFS datasets to force a real command-line
# split through the actual diffsnap binary, which takes tens of seconds and
# has no business running on every regression pass. Run it by hand,
# occasionally, after touching the chunking code.
#
# Works unmodified on both FreeBSD and Linux:
#   - Only path constants (conf/lock file locations) differ by OS; detected
#     via `uname -s`.
#   - No truss/strace dependency at all. Instead of parsing tracer output
#     (whose quoting format differs subtly between tools), the fake `zfs`
#     binary installed for this test records what each `snapshot` call
#     looked like (item count + byte payload) to plain files, then execs
#     the real zfs so the snapshots genuinely get created. That means the
#     verification logic is identical on every platform with a POSIX
#     shell and ZFS.
#
# Usage:
#   sudo bash chunking_integration_test.sh
#   POOL=mypool sudo bash chunking_integration_test.sh   # override pool
#
# What it does:
#   Phase 1: create ~420 real datasets (near-max name length), configure
#            diffsnap to snapshot all of them with one prefix in one batch,
#            run it for real, and confirm:
#              - more than one `zfs snapshot` invocation occurred (proves
#                the batch was actually split)
#              - every invocation's payload stayed under ARGV_BYTES_CAP
#              - every dataset that was supposed to get a snapshot actually
#                has one (no items lost or duplicated across chunks)
#   Phase 2: rerun against the same datasets with the 2nd chunk forced to
#            fail, and confirm only that chunk's datasets are logged as
#            failed while every other chunk's datasets still succeed --
#            i.e. chunk-level failure isolation composes correctly with
#            the real logging/verification path, not just in the
#            standalone unit-test stub.
#
# Cleanup (real zfs binary restore, test datasets, conf) happens via trap
# on exit, success or failure.

set -u
BIN=diffsnap
STATE_DIR=/tmp/diffsnap_chunk_test_state
PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

echo "== Preflight =="
missing=0
for cmd in zfs zpool "$BIN"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "MISSING: required command not found: $cmd"
    missing=1
  fi
done
[ "$missing" -eq 1 ] && { echo "Aborting: one or more required commands are missing."; exit 1; }

# Resolve bash's actual path on THIS system for the wrapper's shebang.
# Hardcoding /bin/bash breaks FreeBSD, where bash (if installed at all) is
# typically /usr/local/bin/bash -- exactly the bug this script had before.
BASH_PATH=$(command -v bash)
if [ -z "$BASH_PATH" ]; then
  echo "Aborting: bash not found on PATH (needed for the zfs test wrapper's shebang)"
  exit 1
fi

OS=$(uname -s)
case "$OS" in
  FreeBSD) CONF=/usr/local/etc/diffsnap.conf; LOCK=/var/run/diffsnap.lock ;;
  Linux)   CONF=/etc/diffsnap.conf;           LOCK=/run/diffsnap.lock ;;
  *) echo "Aborting: unsupported OS '$OS' (expected FreeBSD or Linux)"; exit 1 ;;
esac
LOG=/var/log/diffsnap.log
echo "Detected OS: $OS  (CONF=$CONF LOCK=$LOCK LOG=$LOG)"

POOL="${POOL:-$(zpool list -H -o name 2>/dev/null | head -1)}"
if [ -z "$POOL" ]; then
  echo "Aborting: no zpool found (set POOL=yourpool to override detection)"
  exit 1
fi
DS_CHUNK="${POOL}/diffsnap_chunktest"
echo "Using pool: $POOL  (test dataset tree: $DS_CHUNK)"

ORIG_CONF_BACKUP=$(mktemp)
[ -f "$CONF" ] && cp "$CONF" "$ORIG_CONF_BACKUP" || : > "$ORIG_CONF_BACKUP"

ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  echo "Aborting: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
  exit 1
fi

cleanup() {
  [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"
  zfs destroy -R "$DS_CHUNK" >/dev/null 2>&1
  [ -f "$ORIG_CONF_BACKUP" ] && cp "$ORIG_CONF_BACKUP" "$CONF" 2>/dev/null
  rm -f "$ORIG_CONF_BACKUP"
  rm -rf "$STATE_DIR"
}
trap cleanup EXIT

echo "== Backing up real zfs binary =="
cp -a "$ZFS_REAL" "$ZFS_BACKUP"
if [ ! -x "$ZFS_BACKUP" ] || [ ! -s "$ZFS_BACKUP" ]; then
  echo "Aborting: backup at $ZFS_BACKUP is missing, empty, or not executable -- refusing to touch $ZFS_REAL"
  rm -f "$ZFS_BACKUP"
  exit 1
fi
echo "Backup verified: $ZFS_BACKUP ($(wc -c < "$ZFS_BACKUP") bytes)"

echo "== Clean slate =="
zfs destroy -R "$DS_CHUNK" >/dev/null 2>&1
rm -f "$LOCK"
: > "$LOG"
zfs create -p "$DS_CHUNK"

# Retry-safe wrapper install, same rationale as the shell suites: a
# just-finished zfs child process can leave the binary briefly busy.
# NOTE: this only installs the wrapper over $ZFS_REAL -- the backup that
# makes this safe to undo was already created above, unconditionally,
# before this function is ever called.
install_zfs_wrapper() {
  local src="$1" tries=0 max_tries=25
  local errfile; errfile=$(mktemp)
  while ! cp "$src" "$ZFS_REAL" 2>"$errfile"; do
    tries=$((tries+1))
    if [ "$tries" -ge "$max_tries" ]; then
      cat "$errfile" >&2; rm -f "$errfile"
      bad "failed to install zfs test wrapper after $max_tries attempts (target busy)"
      return 1
    fi
    sleep 0.2
  done
  rm -f "$errfile"
  chmod +x "$ZFS_REAL"
}

echo "== Building instrumented zfs wrapper =="
mkdir -p "$STATE_DIR"
printf '#!%s\n' "$BASH_PATH" > /tmp/diffsnap_chunk_wrapper.$$
cat >> /tmp/diffsnap_chunk_wrapper.$$ <<'WRAP'
REAL="$0.diffsnap_test_backup"
STATE_DIR=/tmp/diffsnap_chunk_test_state
if [ "$1" = "snapshot" ]; then
  n=$(( $(cat "$STATE_DIR/call_count" 2>/dev/null || echo 0) + 1 ))
  echo "$n" > "$STATE_DIR/call_count"
  rest=("$@")
  unset 'rest[0]'                                   # drop "snapshot"
  if [ "${rest[1]:-}" = "-r" ]; then unset 'rest[1]'; fi
  printf '%s\n' "${rest[@]}" > "$STATE_DIR/call_${n}.args"
  bytes=0
  while IFS= read -r a; do bytes=$((bytes + ${#a} + 1)); done < "$STATE_DIR/call_${n}.args"
  echo "$bytes" > "$STATE_DIR/call_${n}.bytes"
  if [ -f "$STATE_DIR/fail_on_call" ] && [ "$(cat "$STATE_DIR/fail_on_call")" = "$n" ]; then
    echo "error: simulated failure on call $n" >&2
    exit 1
  fi
fi
exec "$REAL" "$@"
WRAP
if ! install_zfs_wrapper /tmp/diffsnap_chunk_wrapper.$$; then
  rm -f /tmp/diffsnap_chunk_wrapper.$$
  exit 1
fi
rm -f /tmp/diffsnap_chunk_wrapper.$$

echo "== Smoke-testing the installed wrapper before doing anything destructive =="
# This must succeed AND actually reach the real backend before we create
# any datasets or touch diffsnap's config. If the wrapper is broken (bad
# shebang, bad backup, whatever), we find out here -- with nothing at risk
# yet -- instead of discovering it mid-test with real data on the line.
smoke_out=$(zfs list -H -o name "$POOL" 2>&1)
smoke_rc=$?
if [ "$smoke_rc" -ne 0 ] || ! printf '%s\n' "$smoke_out" | grep -q "^${POOL}$"; then
  echo "Aborting: wrapper smoke test failed (rc=$smoke_rc, output: $smoke_out)"
  echo "Restoring real zfs binary now via cleanup trap; nothing destructive was done."
  exit 1
fi
echo "Smoke test passed: wrapper correctly forwards to the real zfs backend."

echo "== Generating config for datasets in one batch =="
# Near-max lengths to minimize how many real datasets we need to create --
# but the length limit that matters for `zfs snapshot` is on the FULL
# "dataset@prefix_timestamp" string, not just the dataset name. ZFS's
# real usable limit is 255 chars (ZFS_MAX_DATASET_NAME_LEN=256 including
# the null terminator). Budget the whole snapshot name against that, with
# real margin, then work backwards to how long the dataset portion and
# prefix can each be.
TOTAL_ITEM_BUDGET=240     # full "dataset@prefix_timestamp" length target, 15 chars under the real 255 limit
PREFIX_LEN=40
PREFIX=$(printf 'p%.0s' $(seq 1 "$PREFIX_LEN"))
PARENT_LEN=${#DS_CHUNK}
FIXED_OVERHEAD=$(( PREFIX_LEN + 1 + 1 + 19 ))   # "@" + prefix + "_" + 19-char timestamp
DATASET_BUDGET=$(( TOTAL_ITEM_BUDGET - FIXED_OVERHEAD ))
LEAF_PAD_LEN=$(( DATASET_BUDGET - PARENT_LEN - 1 - 6 ))    # room for "/" + 6-digit suffix
[ "$LEAF_PAD_LEN" -lt 1 ] && LEAF_PAD_LEN=1
LEAF_PAD=$(printf 'd%.0s' $(seq 1 "$LEAF_PAD_LEN"))
ITEM_BYTES=$(( PARENT_LEN + 1 + LEAF_PAD_LEN + 6 + PREFIX_LEN + 19 + 3 ))
N=$(( (131072 * 6 / 5) / ITEM_BYTES + 1 ))      # ~1.2x cap worth of items (item size is fixed-width, so no variance to margin against)
echo "item_bytes~=$ITEM_BYTES (full snapshot name target: $TOTAL_ITEM_BUDGET chars), using N=$N datasets"

echo "Verifying the target dataset+snapshot name length is actually accepted by this ZFS before committing to $N creates..."
probe_name="${DS_CHUNK}/${LEAF_PAD}999999"
probe_snap="${probe_name}@${PREFIX}_9999-99-99_99:99:99"
probe_err=$(zfs create "$probe_name" 2>&1)
if [ $? -ne 0 ]; then
  echo "Aborting: probe dataset creation failed (name length ${#probe_name} chars)."
  echo "zfs said: $probe_err"
  echo "Try lowering TOTAL_ITEM_BUDGET in this script and rerunning."
  exit 1
fi
snap_err=$(zfs snapshot "$probe_snap" 2>&1)
snap_rc=$?
zfs destroy -R "$probe_name" >/dev/null 2>&1
if [ "$snap_rc" -ne 0 ]; then
  echo "Aborting: probe SNAPSHOT creation failed (full name length ${#probe_snap} chars)."
  echo "zfs said: $snap_err"
  echo "This is the length that actually matters for 'zfs snapshot' -- try lowering TOTAL_ITEM_BUDGET and rerunning."
  exit 1
fi
echo "Probe succeeded (dataset ${#probe_name} chars, full snapshot name ${#probe_snap} chars, both accepted)."

: > "$CONF"
echo "Creating $N datasets (this takes a while)..."
i=0
while [ "$i" -lt "$N" ]; do
  suffix=$(printf '%06d' "$i")
  name="${DS_CHUNK}/${LEAF_PAD}${suffix}"
  create_err=$(zfs create "$name" 2>&1)
  if [ $? -ne 0 ]; then
    bad "zfs create failed for dataset index $i ($name)"
    echo "zfs said: $create_err"
    exit 1
  fi
  echo "$name 1 1 $PREFIX no 0" >> "$CONF"
  i=$((i+1))
  if [ $((i % 50)) -eq 0 ]; then echo "  ...$i/$N created"; fi
done
echo "Done creating $N datasets."

echo
echo "== Phase 1: real run, no forced failures -- confirm the batch actually splits =="
rm -f "$STATE_DIR"/call_count "$STATE_DIR"/call_*.args "$STATE_DIR"/call_*.bytes "$STATE_DIR"/fail_on_call
: > "$LOG"
"$BIN"; rc=$?
[ "$rc" -eq 0 ] && ok "diffsnap exited 0 (no failures expected in phase 1)" || bad "diffsnap exited $rc (expected 0)"

total_calls=$(cat "$STATE_DIR/call_count" 2>/dev/null || echo 0)
[ "$total_calls" -gt 1 ] && ok "batch was split into multiple zfs snapshot calls (calls=$total_calls)" || bad "expected more than 1 zfs snapshot call, got $total_calls -- batch was NOT split"

over_cap=0
total_items_seen=0
for f in "$STATE_DIR"/call_*.bytes; do
  [ -f "$f" ] || continue
  b=$(cat "$f")
  if [ "$b" -gt 131072 ]; then over_cap=$((over_cap+1)); fi
done
for f in "$STATE_DIR"/call_*.args; do
  [ -f "$f" ] || continue
  c=$(grep -c . "$f")
  total_items_seen=$((total_items_seen + c))
done
[ "$over_cap" -eq 0 ] && ok "every zfs snapshot call stayed under the 128KiB cap" || bad "$over_cap call(s) exceeded the cap -- chunking is broken"
[ "$total_items_seen" -eq "$N" ] && ok "all $N items accounted for across chunks (no loss/duplication)" || bad "expected $total_items_seen == $N items across chunks, mismatch"

real_snap_count=$(zfs list -t snap -H -o name 2>/dev/null | grep -c "^${DS_CHUNK}/.*@${PREFIX}_")
[ "$real_snap_count" -eq "$N" ] && ok "all $N real snapshots actually exist on disk" || bad "expected $N real snapshots, found $real_snap_count"

created_log_count=$(grep -c "Created=${DS_CHUNK}/.*@${PREFIX}_" "$LOG")
[ "$created_log_count" -eq "$N" ] && ok "diffsnap.log shows $N Created= lines" || bad "expected $N Created= log lines, found $created_log_count"

echo
if [ "$total_calls" -lt 2 ]; then
  echo "== Phase 2 skipped: batch didn't split in phase 1, can't test chunk-failure isolation =="
else
  echo "== Phase 2: force chunk #2 to fail -- confirm ONLY that chunk's datasets fail, others still succeed =="
  # Chunk composition is deterministic (same config file order, same
  # deterministic packing), so call_2.args from phase 1 tells us exactly
  # which real dataset names will be in the failing chunk this time too.
  cp "$STATE_DIR/call_2.args" "$STATE_DIR/chunk2_datasets_raw.txt"
  sed -E 's/@.*$//' "$STATE_DIR/chunk2_datasets_raw.txt" > "$STATE_DIR/chunk2_datasets.txt"
  chunk2_count=$(grep -c . "$STATE_DIR/chunk2_datasets.txt")

  # Everything NOT in chunk 2 (from any other chunk) should still succeed.
  : > "$STATE_DIR/other_datasets.txt"
  for f in "$STATE_DIR"/call_*.args; do
    case "$f" in *call_2.args) continue ;; esac
    sed -E 's/@.*$//' "$f" >> "$STATE_DIR/other_datasets.txt"
  done
  other_count=$(grep -c . "$STATE_DIR/other_datasets.txt")

  rm -f "$STATE_DIR"/call_count "$STATE_DIR"/call_*.args "$STATE_DIR"/call_*.bytes
  echo 2 > "$STATE_DIR/fail_on_call"
  : > "$LOG"
  "$BIN"; rc=$?
  [ "$rc" -ne 0 ] && ok "diffsnap exited non-zero as expected (forced failure present)" || bad "diffsnap exited 0, expected non-zero"
  [ "$rc" -lt 128 ] && ok "no crash/fatal signal on the forced chunk failure (exit $rc)" || bad "process died from a signal (exit $rc)"

  grep -q "zfs snapshot batch execution failed" "$LOG" && ok "batch failure logged" || bad "expected batch failure message missing from log"

  # Build "Snapshot not created: <dataset>@<prefix>" patterns for chunk 2's
  # datasets and confirm every one of them is present.
  awk -v p="$PREFIX" '{print "Snapshot not created: " $0 "@" p}' "$STATE_DIR/chunk2_datasets.txt" > "$STATE_DIR/expect_failed.txt"
  matched_failed=$(grep -F -f "$STATE_DIR/expect_failed.txt" -c "$LOG")
  [ "$matched_failed" -eq "$chunk2_count" ] \
    && ok "all $chunk2_count chunk-2 datasets correctly logged as 'Snapshot not created'" \
    || bad "expected $chunk2_count chunk-2 datasets logged as failed, found $matched_failed"

  # None of chunk 2's datasets should show up as Created=.
  awk -v p="$PREFIX" '{print "Created=" $0 "@" p}' "$STATE_DIR/chunk2_datasets.txt" > "$STATE_DIR/unexpected_created.txt"
  leaked_success=$(grep -F -f "$STATE_DIR/unexpected_created.txt" -c "$LOG")
  [ "$leaked_success" -eq 0 ] \
    && ok "no chunk-2 dataset was incorrectly logged as Created=" \
    || bad "$leaked_success chunk-2 dataset(s) incorrectly show as Created= despite the forced failure"

  # Every OTHER chunk's datasets should show up as Created= (unaffected).
  awk -v p="$PREFIX" '{print "Created=" $0 "@" p}' "$STATE_DIR/other_datasets.txt" > "$STATE_DIR/expect_created.txt"
  matched_created=$(grep -F -f "$STATE_DIR/expect_created.txt" -c "$LOG")
  [ "$matched_created" -eq "$other_count" ] \
    && ok "all $other_count non-chunk-2 datasets still correctly logged as Created= (unaffected by chunk 2's failure)" \
    || bad "expected $other_count non-chunk-2 datasets Created=, found $matched_created -- failure leaked across chunks"
fi

echo
echo "================================"
echo "RESULTS: $PASS passed, $FAIL failed"
echo "================================"
if [ $FAIL -eq 0 ]; then
  echo "ALL CLEAR"
else
  echo "REVIEW FAILURES ABOVE"
  echo
  echo "--- diffsnap.log from the last run, for diagnosis ---"
  cat "$LOG" 2>/dev/null
  echo "--- end diffsnap.log ---"
fi
[ $FAIL -eq 0 ]
