#!/bin/bash
# diffsnap full regression suite (Linux)
# sudo bash linux.sh
set -u
CONF=/etc/diffsnap.conf
LOG=/var/log/diffsnap.log
LOCK=/run/diffsnap.lock
BIN=diffsnap
DS=rpool/clonetest
COMBINED_LOG=./diffsnap_full_log.txt
PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

SAVED_DATETIME=""
restore_clock_and_ntp() {
  if [ -n "$SAVED_DATETIME" ]; then
    date -s "$SAVED_DATETIME" >/dev/null 2>&1
  fi
  timedatectl set-ntp true >/dev/null 2>&1
}
trap restore_clock_and_ntp EXIT

archive_log() {
  local section="$1"
  {
    echo "===== BEGIN SECTION: $section ====="
    cat "$LOG" 2>/dev/null
    echo "===== END SECTION: $section (raw diffsnap.log at this point) ====="
    echo
  } >> "$COMBINED_LOG"
  : > "$LOG"
}

# Installs a test wrapper (written by the caller to $1, a temp file) over
# $ZFS_REAL. Retries briefly on ETXTBSY ("Text file busy"), which can occur
# if a zfs child process spawned by a just-finished diffsnap run hasn't
# fully released its text-segment mapping on the binary yet -- a timing
# race, not a diffsnap defect. Logs a FAIL and returns 1 if the target
# stays busy past the retry budget, rather than silently proceeding with a
# stale/unwrapped binary.
install_zfs_wrapper() {
  local src="$1" tries=0 max_tries=25
  local errfile
  errfile=$(mktemp)
  while ! cp "$src" "$ZFS_REAL" 2>"$errfile"; do
    tries=$((tries+1))
    if [ "$tries" -ge "$max_tries" ]; then
      cat "$errfile" >&2
      rm -f "$errfile"
      bad "failed to install zfs test wrapper after $max_tries attempts (target busy)"
      return 1
    fi
    sleep 0.2
  done
  rm -f "$errfile"
  chmod +x "$ZFS_REAL"
}

echo "== Preflight: required commands =="
missing=0
for cmd in zfs strace flock timedatectl realpath "$BIN"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "MISSING: required command not found: $cmd"
    missing=1
  fi
done
[ "$missing" -eq 1 ] && { echo "Aborting: one or more required commands are missing."; exit 1; }

: > "$COMBINED_LOG"

ORIG_CONF_BACKUP=$(mktemp)
[ -f "$CONF" ] && cp "$CONF" "$ORIG_CONF_BACKUP" || : > "$ORIG_CONF_BACKUP"

echo "== Preflight: build matches working tree =="
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXPECTED_SHA=$(git -c safe.directory="*" -C "$REPO_DIR" describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
ACTUAL_SHA=$("$BIN" --version 2>/dev/null | sed -n 's/.*(\(.*\))/\1/p')
if [ "$EXPECTED_SHA" = "unknown" ] || [ "$ACTUAL_SHA" = "unknown" ]; then
  echo "NOTE: could not determine build SHA for one side (expected=$EXPECTED_SHA, actual=$ACTUAL_SHA); skipping build-match check"
elif [ "$EXPECTED_SHA" != "$ACTUAL_SHA" ]; then
  echo "ABORT: installed diffsnap build ($ACTUAL_SHA) does not match working tree ($EXPECTED_SHA)."
  echo "Run 'git pull && make clean && make && make install' before testing."
  exit 1
else
  echo "Build matches working tree ($ACTUAL_SHA)"
fi

echo "== 0. Clean slate =="
rm -f "$LOCK"
zfs destroy -R "$DS" 2>/dev/null
zfs destroy -R "${DS}_clone" 2>/dev/null
: > "$LOG"
: > "$CONF"
zfs create "$DS"
zfs create "$DS/a"
zfs create "$DS/b"

echo "== 1. Crash regression: malformed lines must not segfault or die from any signal =="
# dataset,interval,retention,prefix,recursive,min_bytes
# (the 5th line below has a stray space after a comma, landing on the
#  prefix field -- config values are NOT trimmed, so this must be
#  rejected, not silently accepted with a leading-space prefix)
cat > "$CONF" <<CONF
badline
,${DS}/a,1,2,t1,no,0
$DS/a,notanumber,2,t1,no,0
$DS/a,1,0,t1,no,0
$DS/a,1,2,bad!prefix,no,0
$DS/a,1,2, t1,no,0
$DS/a,1,2,t1,maybe,0
$DS/a,1,2,t1,no,notanumber
$DS/a,1,2,t1,no,0,trailing
CONF
"$BIN"; rc=$?
if [ $rc -ge 128 ]; then bad "process died from signal on malformed lines (exit $rc, signal $((rc-128)))"
else ok "no fatal signal on malformed lines (exit $rc)"; fi
grep -c "Config error" "$LOG" | grep -q "^9$" && ok "all 9 malformed lines logged" || bad "malformed line count mismatch: $(grep -c 'Config error' "$LOG")"
archive_log "1 - crash regression"

echo "== 2. Feature matrix: valid config =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,rectest,yes,0
$DS/a,1,2,rectest,no,0
$DS/b,1,2,skipme,no,999999999999
$DS/a,1,2,rectest,no,0
nosuch/dataset,1,2,t1,no,0
CONF
"$BIN"
grep -q "Created=$DS@rectest.*Recursive" "$LOG" && ok "recursive snapshot created" || bad "recursive snapshot missing"
if grep -q "$DS/a@rectest" "$LOG"; then bad "overlap dedup failed: $DS/a snapshotted despite recursive parent"
else ok "overlap dedup: $DS/a correctly excluded (covered by recursive parent)"; fi
grep -q "skipme" "$LOG" && bad "min_bytes threshold not respected" || ok "min_bytes skip correct (no skipme entry)"
grep -c "duplicate dataset/prefix" "$LOG" | grep -q "^1$" && ok "duplicate entry detected" || bad "duplicate detection failed"
grep -q "Configured dataset not found or has invalid written metric: nosuch/dataset" "$LOG" && ok "missing dataset logged" || bad "missing dataset not logged"
archive_log "2 - feature matrix"

echo "== 3. Retention drains to exactly 1 =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,1,rtest,no,0
CONF
"$BIN"; sleep 1; "$BIN"
count=$(zfs list -t snap -H -o name | grep -c "$DS/a@rtest")
[ "$count" -eq 1 ] && ok "retention=1 held (count=$count)" || bad "retention=1 violated (count=$count)"
archive_log "3 - retention drain"

echo "== 4. Clone-blocked destroy handled without crash/data loss =="
sleep 2
TS=$(date +%Y-%m-%d_%H:%M:%S)
zfs snapshot "$DS/a@rtest_$TS"
zfs clone "$DS/a@rtest_$TS" "${DS}_clone"
sleep 2
"$BIN"; sleep 2; "$BIN"
grep -q "Failed to prune snapshot $DS/a@rtest_$TS" "$LOG" && ok "clone-blocked prune failure logged (high-level message)" || bad "clone-block high-level message not logged"
if grep -q "cannot destroy '$DS/a@rtest_$TS'" "$LOG" && grep -q "dependent clones" "$LOG"; then
  ok "forwarded ZFS stderr present (cannot destroy / dependent clones)"
else
  bad "forwarded ZFS stderr missing expected cannot destroy/dependent clones text"
fi
zfs list -t snap -H -o name | grep -q "$DS/a@rtest_$TS" && ok "clone-blocked snapshot preserved" || bad "clone-blocked snapshot lost"
zfs destroy "${DS}_clone"
archive_log "4 - clone-blocked destroy"

echo "== 5. Batching: same-dataset collision fixed, cross-dataset batching preserved =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,2,p1,no,0
$DS/a,1,2,p2,no,0
$DS/a,1,2,p3,no,0
$DS/b,1,2,p1,no,0
CONF
rm -f /tmp/trace_batch.log
strace -f -e trace=execve -o /tmp/trace_batch.log "$BIN"
[ -s /tmp/trace_batch.log ] || bad "strace trace file empty/missing for section 5 -- results below are unreliable"
grep -q "cannot create snapshots" "$LOG" && bad "same-dataset collision still occurs" || ok "no same-dataset collision"
snap_pattern='\["[^"]*zfs[^"]*", *"snapshot"'
snapcalls=$(grep -cE "$snap_pattern" /tmp/trace_batch.log)
[ "$snapcalls" -eq 3 ] && ok "zfs snapshot invocation count correct (3: p1[a+b], p2[a], p3[a])" || bad "unexpected snapshot invocation count: $snapcalls (expected 3)"
crossbatch=$(grep -E "$snap_pattern" /tmp/trace_batch.log | grep -c "clonetest/a.*clonetest/b\|clonetest/b.*clonetest/a")
[ "$crossbatch" -ge 1 ] && ok "cross-dataset batching preserved ($DS/a + $DS/b in one call)" || bad "cross-dataset batching lost"
archive_log "5 - batching"

echo "== 6. Interval boundary matrix (per --help spec) =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,50,3,i50,no,0
$DS/a,1440,3,iday,no,0
CONF
SAVED_DATETIME=$(date +"%Y-%m-%d %H:%M:%S")
timedatectl set-ntp false
declare -A expect_i50=( [00:00:00]=1 [00:50:00]=1 [00:10:00]=0 [23:20:00]=1 )
declare -A expect_iday=( [00:00:00]=1 [00:50:00]=0 [00:10:00]=0 [23:20:00]=0 )
check_interval() {
  local prefix="$1" today="$2" t="$3" expect="$4"
  local expected_name="${DS}/a@${prefix}_${today}_${t}"
  if [ "$expect" -eq 1 ]; then
    grep -qF "Created=$expected_name" "$LOG" \
      && ok "$prefix at $t: exact expected snapshot found ($expected_name)" \
      || bad "$prefix at $t: expected snapshot missing ($expected_name)"
  else
    grep -qF "${prefix}_${today}_${t}" "$LOG" \
      && bad "$prefix at $t: unexpectedly fired" \
      || ok "$prefix at $t: correctly did not fire"
  fi
}
for t in "00:00:00" "00:50:00" "00:10:00" "23:20:00"; do
  date -s "$t" > /dev/null
  today=$(date +%Y-%m-%d)
  "$BIN"
  check_interval "i50" "$today" "$t" "${expect_i50[$t]}"
  check_interval "iday" "$today" "$t" "${expect_iday[$t]}"
done
date -s "$SAVED_DATETIME" >/dev/null 2>&1
timedatectl set-ntp true
SAVED_DATETIME=""
archive_log "6 - interval boundary matrix"

echo "== 7. Lock file / single-instance enforcement =="
: > "$CONF"
exec 9>"$LOCK"
flock -n 9
out2=$("$BIN" 2>&1)
rc2=$?
flock -u 9
exec 9>&-
echo "$out2" | grep -q "another instance is already running" \
  && ok "second concurrent instance correctly rejected (message matched)" \
  || bad "concurrent instance not blocked as expected (rc=$rc2, output: $out2)"
archive_log "7 - lock enforcement"

echo "== 8. Maximum-length prefix accepted =="
# dataset,interval,retention,prefix,recursive,min_bytes
maxprefix=$(printf 'a%.0s' $(seq 1 63))
cat > "$CONF" <<CONF
$DS/a,1,2,$maxprefix,no,0
CONF
"$BIN"
grep -q "Created=$DS/a@${maxprefix}_" "$LOG" && ok "max-length (63-char) prefix accepted" || bad "max-length prefix rejected unexpectedly"
archive_log "8 - max prefix accepted"

echo "== 9. Prefix exceeding limit rejected =="
# dataset,interval,retention,prefix,recursive,min_bytes
overprefix=$(printf 'a%.0s' $(seq 1 64))
cat > "$CONF" <<CONF
$DS/a,1,2,$overprefix,no,0
CONF
"$BIN"
grep -q "prefix too long" "$LOG" && ok "over-length (64-char) prefix rejected" || bad "over-length prefix not rejected as expected"
archive_log "9 - prefix exceeding limit rejected"

echo "== 10. Dataset name at diffsnap's internal buffer limit (256 chars) =="
# dataset,interval,retention,prefix,recursive,min_bytes
maxds_child_len=$((256 - ${#DS} - 1))
maxds_child=$(printf 'x%.0s' $(seq 1 $maxds_child_len))
maxds="$DS/$maxds_child"
cat > "$CONF" <<CONF
$maxds,1,2,buftest,no,0
CONF
"$BIN"
if grep -q "dataset name too long" "$LOG"; then bad "256-char dataset name incorrectly rejected by diffsnap's buffer check"
elif grep -q "Configured dataset not found or has invalid written metric: $maxds" "$LOG"; then ok "256-char dataset name accepted intact by diffsnap's parser (buffer boundary correct)"
else bad "unexpected result for buffer-limit dataset name test"; fi
archive_log "10 - dataset name at buffer limit"

echo "== 11. Dataset name exceeding buffer limit rejected =="
# dataset,interval,retention,prefix,recursive,min_bytes
overds="${maxds}xxxxxxxxxx"
cat > "$CONF" <<CONF
$overds,1,2,buftest2,no,0
CONF
"$BIN"
grep -q "dataset name too long" "$LOG" && ok "over-length dataset name (${#overds} chars) rejected" || bad "over-length dataset name not rejected as expected"
archive_log "11 - dataset name exceeding buffer limit"

echo "== 12. Very long ZFS stderr line handling =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run stderr test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  printf 'error: %s\n' "$(printf 'X%.0s' $(seq 1 700))" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
  rm -f /tmp/diffsnap_zfs_wrapper.$$
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a,1,2,longerr,no,0
CONF
  "$BIN"; rc=$?
  [ $rc -lt 128 ] && ok "no crash on >512-byte zfs stderr line" || bad "crashed on >512-byte zfs stderr line (exit $rc)"
  grep -q "Error: zfs:" "$LOG" && ok "oversized stderr line logged (split across reader buffer, not lost)" || bad "oversized stderr line not logged"
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "12 - long zfs stderr"

echo "== 13. --help returns 0 and contains expected text =="
helpout=$("$BIN" --help 2>&1); helprc=$?
if [ $helprc -eq 0 ] && echo "$helpout" | grep -q "Usage:" && echo "$helpout" | grep -q "Config:"; then
  ok "--help exits 0 and contains expected usage/config text"
else bad "--help failed (rc=$helprc)"; fi

echo "== 14. --version returns 0 =="
verout=$("$BIN" --version 2>&1); verrc=$?
if [ $verrc -eq 0 ] && echo "$verout" | grep -qi "diffsnap"; then ok "--version exits 0 and reports version string"
else bad "--version failed (rc=$verrc, output: $verout)"; fi

echo "== 15. Unknown option returns usage and exit 2 =="
badoptout=$("$BIN" --bogus-option 2>&1); badoptrc=$?
if [ $badoptrc -eq 2 ] && echo "$badoptout" | grep -q "Usage:"; then ok "unknown option exits 2 with usage message"
else bad "unknown option handling incorrect (rc=$badoptrc, output: $badoptout)"; fi

echo "== 16. Missing config file returns error =="
mv "$CONF" "${CONF}.bak"
missingconfout=$("$BIN" 2>&1); missingconfrc=$?
mv "${CONF}.bak" "$CONF"
if [ $missingconfrc -eq 1 ] && echo "$missingconfout" | grep -qi "failed to open config file"; then
  ok "missing config file correctly reported as error"
else bad "missing config file not handled as expected (rc=$missingconfrc, output: $missingconfout)"; fi
archive_log "13-16 - help, version, unknown option, missing config"

echo "== 17. Missing log file permissions =="
if id nobody >/dev/null 2>&1; then
  rm -f "$LOCK"; : > "$LOCK"; chmod 666 "$LOCK"
  : > "$LOG"; chown root:root "$LOG"; chmod 600 "$LOG"
  permout=$(sudo -u nobody "$BIN" 2>&1); permrc=$?
  chmod 644 "$LOG"; rm -f "$LOCK"
  if [ $permrc -eq 1 ] && echo "$permout" | grep -qi "failed to open log file"; then
    ok "unwritable log file correctly reported as error"
  else bad "unwritable log file not handled as expected (rc=$permrc, output: $permout)"; fi
else echo "SKIP: 'nobody' user not available on this system"; fi
archive_log "17 - missing log permissions"

echo "== 18. Recursive retention pruning behaves correctly =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,1,rectest2,yes,0
CONF
"$BIN"; sleep 1; "$BIN"
parentcount=$(zfs list -t snap -H -o name | grep -c "^$DS@rectest2")
childcount=$(zfs list -t snap -H -o name | grep -c "^$DS/a@rectest2")
[ "$parentcount" -eq 1 ] && ok "recursive retention held on parent (count=$parentcount)" || bad "recursive retention violated on parent (count=$parentcount)"
[ "$childcount" -eq 1 ] && ok "recursive retention held on child via -r destroy (count=$childcount)" || bad "recursive retention violated on child (count=$childcount)"
archive_log "18 - recursive retention pruning"

echo "== 19. zfs get invoked with -t filesystem,volume filter =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,2,gettest,no,0
CONF
zfs snapshot "$DS/a@marker" 2>/dev/null
zfs bookmark "$DS/a@marker" "$DS/a#marker" 2>/dev/null
rm -f /tmp/trace_get.log
strace -f -e trace=execve -o /tmp/trace_get.log "$BIN"
[ -s /tmp/trace_get.log ] || bad "strace trace file empty/missing for section 19 -- results below are unreliable"
grep -qE '"get".*"-t".*"filesystem,volume"' /tmp/trace_get.log \
  && ok "zfs get invoked with -t filesystem,volume filter" \
  || bad "zfs get missing -t filesystem,volume filter"
zfs destroy "$DS/a#marker" 2>/dev/null
zfs destroy "$DS/a@marker" 2>/dev/null
archive_log "19 - zfs get filesystem,volume filter"

echo "== 20. Oversized zfs get written line skipped, not fatal to batch =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run oversized-metric-line test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "get" ]; then
  printf '%s\t123\n' "$(printf 'x%.0s' $(seq 1 300))"
fi
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
  rm -f /tmp/diffsnap_zfs_wrapper.$$
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a,1,2,skiptest,no,0
CONF
  "$BIN"
  grep -q "Skipping metric line with oversized dataset name" "$LOG" && ok "oversized metric line logged and skipped" || bad "oversized metric line not logged"
  grep -q "Created=$DS/a@skiptest" "$LOG" && ok "valid dataset still snapshotted despite earlier bad line" || bad "good line lost after bad line (batch aborted?)"
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "20 - oversized metric line"

echo "== 21. Snapshot inventory scoped to single root when verification needed =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run inventory-scoping test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  echo "error: simulated snapshot failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
  rm -f /tmp/diffsnap_zfs_wrapper.$$
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a,1,2,scopetest,no,0
CONF
  POOL="${DS%%/*}"
  rm -f /tmp/trace_scope.log
  # Note: strace mirrors the traced program's exit status, and $BIN is
  # expected to exit non-zero here (simulated snapshot failure), so we
  # deliberately do not check strace's own exit code -- only that it
  # actually produced a trace file.
  strace -f -e trace=execve -o /tmp/trace_scope.log "$BIN"
  [ -s /tmp/trace_scope.log ] || bad "strace trace file empty/missing for section 21 -- results below are unreliable"
  grep -qE '"list".*"-r".*"'"$POOL"'"' /tmp/trace_scope.log \
    && ok "single-root batch verification used scoped -r zfs list ($POOL)" \
    || bad "expected scoped zfs list -r $POOL not found in trace"
  grep -q "Snapshot not created: $DS/a@scopetest" "$LOG" \
    && ok "simulated snapshot failure correctly detected via inventory check" \
    || bad "expected 'Snapshot not created' message missing"
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "21 - inventory scoping"

echo "== 22. Overlap dedup only applies to matching prefix (recursive parent + non-recursive child) =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,recA,yes,0
$DS/a,1,2,recB,no,0
CONF
"$BIN"
grep -q "Created=$DS/a@recB" "$LOG" \
  && ok "different-prefix child NOT deduped against recursive parent" \
  || bad "different-prefix child incorrectly deduped"

# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,recSame,yes,0
$DS/a,1,2,recSame,no,0
CONF
"$BIN"
if grep -q "Created=$DS/a@recSame" "$LOG"; then bad "same-prefix child NOT deduped (overlap logic broken)"
else ok "same-prefix child correctly deduped against recursive parent"; fi
grep -q "Skipping $DS/a: covered by a recursive ancestor with prefix 'recSame'" "$LOG" \
  && ok "std/rec overlap dedup logged the skip" \
  || bad "std/rec overlap dedup did not log the skip (silent drop)"
archive_log "22 - prefix-aware overlap dedup"

echo "== 23. Nested recursive overlap, same prefix: descendant dropped before any zfs call =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,recSameNest,yes,0
$DS/a,1,2,recSameNest,yes,0
CONF
"$BIN"
grep -q "Created=$DS@recSameNest.*Recursive" "$LOG" \
  && ok "recursive ancestor snapshot created" \
  || bad "recursive ancestor snapshot missing"
if grep -q "Created=$DS/a@recSameNest" "$LOG"; then
  bad "nested recursive descendant NOT deduped (would collide with -r ancestor snapshot)"
else
  ok "nested recursive descendant correctly deduped (same prefix, both recursive)"
fi
grep -q "Skipping $DS/a: already covered by a recursive ancestor with prefix 'recSameNest'" "$LOG" \
  && ok "nested recursive overlap dedup logged the skip" \
  || bad "nested recursive overlap dedup did not log the skip (silent drop)"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- ancestor+descendant likely collided" \
  || ok "no zfs snapshot batch failure"
archive_log "23 - nested recursive overlap (same prefix)"

echo "== 24. Nested recursive overlap, different prefix: both kept, split into separate passes =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,recX,yes,0
$DS/a,1,2,recY,yes,0
CONF
rm -f /tmp/trace_nested.log
strace -f -e trace=execve -o /tmp/trace_nested.log "$BIN"
[ -s /tmp/trace_nested.log ] || bad "strace trace file empty/missing for section 24 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- ancestor+descendant likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@recX.*Recursive" "$LOG" \
  && ok "recursive ancestor snapshot created (different prefix, kept)" \
  || bad "recursive ancestor snapshot missing"
grep -q "Created=$DS/a@recY.*Recursive" "$LOG" \
  && ok "recursive descendant snapshot created (different prefix, kept)" \
  || bad "recursive descendant snapshot missing"
nestedcalls=$(grep -cE "$snap_pattern" /tmp/trace_nested.log)
[ "$nestedcalls" -eq 2 ] && ok "ancestor and descendant issued as 2 separate 'zfs snapshot' calls" || bad "expected 2 separate snapshot invocations, got $nestedcalls"
archive_log "24 - nested recursive overlap (different prefix, separate passes)"

echo "== 25. Recursive same-dataset duplicate (different prefixes): baseline dedup within rec_b =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,recDup1,yes,0
$DS,1,2,recDup2,yes,0
CONF
rm -f /tmp/trace_recdup.log
strace -f -e trace=execve -o /tmp/trace_recdup.log "$BIN"
[ -s /tmp/trace_recdup.log ] || bad "strace trace file empty/missing for section 25 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- same-dataset recursive duplicates likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@recDup1.*Recursive" "$LOG" && ok "first duplicate recursive snapshot created" || bad "first duplicate recursive snapshot missing"
grep -q "Created=$DS@recDup2.*Recursive" "$LOG" && ok "second duplicate recursive snapshot created" || bad "second duplicate recursive snapshot missing"
recdupcalls=$(grep -cE "$snap_pattern" /tmp/trace_recdup.log)
[ "$recdupcalls" -eq 2 ] && ok "duplicate dataset issued as 2 separate 'zfs snapshot -r' calls (baseline pass dedup)" || bad "expected 2 separate snapshot invocations, got $recdupcalls"
archive_log "25 - recursive same-dataset duplicate"

echo "== 26. Three-level nested recursive chain, distinct prefixes: multiple pass bumps =="
# dataset,interval,retention,prefix,recursive,min_bytes
zfs create -p "$DS/a/c" 2>/dev/null
cat > "$CONF" <<CONF
$DS,1,2,lvl0,yes,0
$DS/a,1,2,lvl1,yes,0
$DS/a/c,1,2,lvl2,yes,0
CONF
rm -f /tmp/trace_chain.log
strace -f -e trace=execve -o /tmp/trace_chain.log "$BIN"
[ -s /tmp/trace_chain.log ] || bad "strace trace file empty/missing for section 26 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- 3-level chain likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@lvl0.*Recursive" "$LOG" && ok "level-0 ancestor snapshot created" || bad "level-0 ancestor snapshot missing"
grep -q "Created=$DS/a@lvl1.*Recursive" "$LOG" && ok "level-1 snapshot created" || bad "level-1 snapshot missing"
grep -q "Created=$DS/a/c@lvl2.*Recursive" "$LOG" && ok "level-2 (deepest) snapshot created" || bad "level-2 snapshot missing"
chaincalls=$(grep -cE "$snap_pattern" /tmp/trace_chain.log)
[ "$chaincalls" -eq 3 ] && ok "3-level chain correctly split into 3 separate passes" || bad "expected 3 separate snapshot invocations, got $chaincalls"
archive_log "26 - three-level nested recursive chain"

echo "== 27. Duplicate ancestor plus descendant: pass assignment must avoid all ancestor passes =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS,1,2,dupA,yes,0
$DS,1,2,dupB,yes,0
$DS/a,1,2,dupC,yes,0
CONF
rm -f /tmp/trace_dupanc.log
strace -f -e trace=execve -o /tmp/trace_dupanc.log "$BIN"
[ -s /tmp/trace_dupanc.log ] || bad "strace trace file empty/missing for section 27 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- duplicate ancestor + descendant likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@dupA.*Recursive" "$LOG" && ok "first duplicate ancestor snapshot created" || bad "first duplicate ancestor snapshot missing"
grep -q "Created=$DS@dupB.*Recursive" "$LOG" && ok "second duplicate ancestor snapshot created" || bad "second duplicate ancestor snapshot missing"
grep -q "Created=$DS/a@dupC.*Recursive" "$LOG" && ok "descendant snapshot created despite duplicated ancestor" || bad "descendant snapshot missing"
dupanccalls=$(grep -cE "$snap_pattern" /tmp/trace_dupanc.log)
[ "$dupanccalls" -eq 3 ] && ok "duplicate ancestor + descendant correctly split into 3 separate passes" || bad "expected 3 separate snapshot invocations, got $dupanccalls"
archive_log "27 - duplicate ancestor plus descendant pass assignment"


echo "== 28. Prune matching does not cross-match dataset name prefixes (e.g. DS/a vs DS/ab) =="
zfs create "$DS/ab" 2>/dev/null
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,1,xmatch,no,0
$DS/ab,1,1,xmatch,no,0
CONF
"$BIN"; sleep 1; "$BIN"; sleep 1; "$BIN"
count_a=$(zfs list -t snap -H -o name | grep -c "^$DS/a@xmatch")
count_ab=$(zfs list -t snap -H -o name | grep -c "^$DS/ab@xmatch")
[ "$count_a" -eq 1 ] && ok "retention correct for $DS/a (not cross-matched with $DS/ab)" || bad "retention violated for $DS/a (count=$count_a) -- possible dataset-name boundary bug"
[ "$count_ab" -eq 1 ] && ok "retention correct for $DS/ab (not cross-matched with $DS/a)" || bad "retention violated for $DS/ab (count=$count_ab) -- possible dataset-name boundary bug"
zfs destroy -R "$DS/ab" 2>/dev/null
archive_log "28 - dataset name prefix boundary in prune matching"

echo "== 29. Combined batch verification: one shared zfs list call covers both std and recursive batches =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run combined-inventory test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  echo "error: simulated snapshot failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
  rm -f /tmp/diffsnap_zfs_wrapper.$$
  # dataset          interval  retention  prefix    recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a,1,2,combA,no,0
$DS/b,1,2,combB,yes,0
CONF
  rm -f /tmp/trace_comb.log
  strace -f -e trace=execve -o /tmp/trace_comb.log "$BIN"
  [ -s /tmp/trace_comb.log ] || bad "strace trace file empty/missing for section 29 -- results below are unreliable"
  list_calls=$(grep -E '"list".*"-t".*"snapshot"' /tmp/trace_comb.log | grep -vc "diffsnap_test_backup")
  [ "$list_calls" -eq 1 ] && ok "exactly one shared zfs list call covers both std and recursive verification (count=$list_calls)" || bad "expected exactly 1 zfs list call for combined verification, got $list_calls"
  grep -q "Snapshot not created: $DS/a@combA" "$LOG" && ok "std batch failure correctly detected via shared inventory" || bad "std batch failure not detected"
  grep -q "Snapshot not created: $DS/b@combB" "$LOG" && ok "recursive batch failure correctly detected via shared inventory" || bad "recursive batch failure not detected"
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "29 - combined batch verification single shared list call"

echo "== 30. Pruning reuses one shared snapshot listing instead of one zfs list call per dataset =="
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,2,prlist1,no,0
$DS/b,1,2,prlist2,no,0
CONF
rm -f /tmp/trace_prlist.log
strace -f -e trace=execve -o /tmp/trace_prlist.log "$BIN"
[ -s /tmp/trace_prlist.log ] || bad "strace trace file empty/missing for section 30 -- results below are unreliable"
list_calls=$(grep -E '"list".*"-t".*"snapshot"' /tmp/trace_prlist.log | grep -vc "diffsnap_test_backup")
[ "$list_calls" -eq 1 ] && ok "single shared zfs list call used for pruning across multiple datasets (count=$list_calls)" || bad "expected exactly 1 zfs list call for pruning, got $list_calls (old per-dataset behavior?)"
archive_log "30 - shared listing reused for pruning across multiple datasets"

echo "== 31. Shared inventory listing failure blocks verification AND pruning for all items (coupled failure) =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run coupled-failure test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "list" ] && [ "$2" = "-H" ]; then
  echo "error: simulated list failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
  rm -f /tmp/diffsnap_zfs_wrapper.$$
  # dataset          interval  retention  prefix    recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a,1,2,listfail,no,0
CONF
  "$BIN"
  grep -q "Unable to list snapshots for batch verification and pruning" "$LOG" && ok "shared inventory failure logged" || bad "shared inventory failure not logged"
  grep -q "Unable to prune.*snapshots for $DS/a: snapshot inventory unavailable" "$LOG" && ok "pruning correctly skipped and logged when inventory unavailable" || bad "pruning failure not logged when inventory unavailable"
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "31 - shared inventory listing failure coupling"

echo "== 32. Written= byte count reflects actual data written (cached metric value used in log) =="
mp=$(zfs get -H -o value mountpoint "$DS/a")
dd if=/dev/urandom of="$mp/testfile" bs=1M count=2 2>/dev/null
POOL="${DS%%/*}"
zpool sync "$POOL" 2>/dev/null || sync
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/a,1,2,wtest,no,0
CONF
"$BIN"
writtenline=$(grep "Created=$DS/a@wtest" "$LOG")
echo "$writtenline" | grep -Eq 'Written=[0-9]' && ! echo "$writtenline" | grep -q "Written=0 " \
  && ok "Written= byte count reflects real data (non-zero, correctly cached value): $writtenline" \
  || bad "Written= value missing or zero despite real data written: $writtenline"
rm -f "$mp/testfile"
archive_log "32 - written byte count accuracy"

echo "== 33. Recursive min_bytes: subtree total (parent + descendants) gates the snapshot, not just the parent's own written bytes =="
POOL="${DS%%/*}"
zfs create "$DS/recmin" 2>/dev/null
zfs create "$DS/recmin/child" 2>/dev/null

echo "-- 33a. Parent alone is quiescent; child is active; subtree SUM clears the threshold -> snapshot IS taken --"
parent_written=$(zfs get -H -p -o value written "$DS/recmin")
if [ "$parent_written" -ge 1000000 ] 2>/dev/null; then
  bad "test setup invalid: $DS/recmin's own written ($parent_written) already exceeds the 1000000 threshold on its own -- this run wouldn't actually exercise subtree summing"
else
  ok "test setup valid: $DS/recmin's own written ($parent_written) is below the threshold by itself (proves the pass below depends on the child's bytes, not just the parent's)"
fi
child_mp=$(zfs get -H -o value mountpoint "$DS/recmin/child")
dd if=/dev/urandom of="$child_mp/data" bs=1M count=2 2>/dev/null
zpool sync "$POOL" 2>/dev/null || sync
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/recmin,1,2,recminA,yes,1000000
CONF
"$BIN"
grep -q "Created=$DS/recmin@recminA.*Recursive" "$LOG" \
  && ok "recursive snapshot created: quiescent parent + active child clears min_bytes via subtree sum" \
  || bad "recursive snapshot missing: subtree summing did not credit the child's written bytes to the parent's threshold check"
recminA_line=$(grep "Created=$DS/recmin@recminA" "$LOG")
echo "$recminA_line" | grep -Eq 'Written=[0-9]' && ! echo "$recminA_line" | grep -q "Written=0 " \
  && ok "logged Written= reflects the real (non-zero) subtree total: $recminA_line" \
  || bad "logged Written= missing or zero despite 2MB written to the child: $recminA_line"

echo "-- 33b. Both parent and child quiescent (relative to the snapshot just taken); subtree SUM stays below threshold -> silently skipped --"
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$DS/recmin,1,2,recminB,yes,999999999999
CONF
"$BIN"
grep -q "recminB" "$LOG" \
  && bad "quiescent recursive subtree (parent+child both below threshold) was unexpectedly snapshotted or errored" \
  || ok "quiescent recursive subtree correctly skipped silently (no Created=, no error)"

zfs destroy -R "$DS/recmin" 2>/dev/null
archive_log "33 - recursive min_bytes subtree summing"

echo "== 34. Dataset name containing a space is handled correctly end-to-end =="
# ZFS dataset names are permitted to contain spaces. This exercises the
# whole pipeline for one: comma-separated config parsing (a space can no
# longer be confused with the field separator), zfs get/metrics matching
# (handle_metric_line tokenizes on tab only, not space), snapshot
# creation via a direct execve'd argv array (no shell involved, so no
# quoting/escaping concern), and retention/pruning -- alongside a
# same-prefix sibling dataset WITHOUT a space, to confirm the space
# doesn't confuse prune-matching's dataset-name boundary logic either.
SPACE_DS="$DS/space test"
zfs create "$SPACE_DS" 2>/dev/null
mp=$(zfs get -H -o value mountpoint "$SPACE_DS")
dd if=/dev/urandom of="$mp/data" bs=1M count=2 2>/dev/null
POOL="${DS%%/*}"
zpool sync "$POOL" 2>/dev/null || sync
# dataset,interval,retention,prefix,recursive,min_bytes
cat > "$CONF" <<CONF
$SPACE_DS,1,1,spacetest,no,0
$DS/a,1,1,spacetest,no,0
CONF
"$BIN"
grep -qF "Created=$SPACE_DS@spacetest" "$LOG" \
  && ok "snapshot created for a dataset name containing a space, with the space preserved intact in the log" \
  || bad "snapshot missing or name corrupted for dataset containing a space"
writtenline=$(grep -F "Created=$SPACE_DS@spacetest" "$LOG")
echo "$writtenline" | grep -Eq 'Written=[0-9]' && ! echo "$writtenline" | grep -q "Written=0 " \
  && ok "Written= for the space-containing dataset reflects real data (metric line correctly matched despite the space)" \
  || bad "Written= missing or zero for space-containing dataset -- metric line matching likely broken by the space"
zfs list -t snap -H -o name | grep -qF "${SPACE_DS}@spacetest" \
  && ok "real snapshot with the space-containing name actually exists on disk" \
  || bad "real snapshot with space-containing name not found via zfs list"
grep -qF "Created=$DS/a@spacetest" "$LOG" \
  && ok "sibling dataset without a space in its name still snapshotted correctly in the same batch" \
  || bad "sibling dataset without a space was not correctly snapshotted"
sleep 1; "$BIN"; sleep 1; "$BIN"
count_space=$(zfs list -t snap -H -o name | grep -cF "${SPACE_DS}@spacetest")
count_a=$(zfs list -t snap -H -o name | grep -c "^$DS/a@spacetest")
[ "$count_space" -eq 1 ] && ok "retention=1 held for the space-containing dataset (count=$count_space)" || bad "retention=1 violated for space-containing dataset (count=$count_space)"
[ "$count_a" -eq 1 ] && ok "retention=1 held for the sibling dataset, unaffected by the space-containing one (count=$count_a)" || bad "retention=1 violated for sibling dataset (count=$count_a)"
zfs destroy -R "$SPACE_DS" 2>/dev/null
archive_log "34 - dataset name containing a space"

echo "== 35. Lock/log/config file descriptors are not leaked into zfs child processes (close-on-exec) =="
ZFS_REAL=$(command -v zfs)
ZFS_BACKUP="${ZFS_REAL}.diffsnap_test_backup"
if [ -f "$ZFS_BACKUP" ]; then
  bad "refusing to run fd close-on-exec test: stale backup exists at $ZFS_BACKUP (restore it manually before retrying)"
else
  cp -a "$ZFS_REAL" "$ZFS_BACKUP"
  restore_real_zfs() { [ -f "$ZFS_BACKUP" ] && cp -a "$ZFS_BACKUP" "$ZFS_REAL" && rm -f "$ZFS_BACKUP"; }
  trap 'restore_real_zfs; restore_clock_and_ntp' EXIT
  FDLEAK_STATE=/tmp/diffsnap_fdleak_state
  rm -f "$FDLEAK_STATE"
  cat > /tmp/diffsnap_zfs_wrapper.$$ <<'WRAP'
#!/bin/bash
# Runs as every "zfs" invocation diffsnap makes. Before forwarding to the
# real binary, inspects its OWN open file descriptors (i.e. exactly what
# survived the parent's execve()) and records any that resolve to
# diffsnap's lock, log, or config file -- fds that should have been
# closed on exec and must never reach here.
REAL="$0.diffsnap_test_backup"
STATE=/tmp/diffsnap_fdleak_state
for fdpath in /proc/self/fd/*; do
  target=$(readlink "$fdpath" 2>/dev/null) || continue
  case "$target" in
    "$DIFFSNAP_TEST_LOCK") echo "leaked LOCK fd ($fdpath -> $target) into: zfs $*" >> "$STATE" ;;
    "$DIFFSNAP_TEST_LOG")  echo "leaked LOG fd ($fdpath -> $target) into: zfs $*"  >> "$STATE" ;;
    "$DIFFSNAP_TEST_CONF") echo "leaked CONF fd ($fdpath -> $target) into: zfs $*" >> "$STATE" ;;
  esac
done
exec "$REAL" "$@"
WRAP
  if install_zfs_wrapper /tmp/diffsnap_zfs_wrapper.$$; then
    rm -f /tmp/diffsnap_zfs_wrapper.$$
    # dataset,interval,retention,prefix,recursive,min_bytes
    cat > "$CONF" <<CONF
$DS/a,1,2,cloexectest,no,0
CONF
    # touch first: the lock file may not exist yet at this point in the
    # suite, and realpath needs an existing target to resolve against.
    touch "$LOCK"
    export DIFFSNAP_TEST_LOCK="$(realpath -m "$LOCK")" \
           DIFFSNAP_TEST_LOG="$(realpath -m "$LOG")" \
           DIFFSNAP_TEST_CONF="$(realpath -m "$CONF")"
    "$BIN"; rc=$?
    unset DIFFSNAP_TEST_LOCK DIFFSNAP_TEST_LOG DIFFSNAP_TEST_CONF
    [ $rc -eq 0 ] && ok "diffsnap completed successfully with the fd-inspecting wrapper in place (rc=$rc)" || bad "diffsnap failed with the fd-inspecting wrapper in place (rc=$rc)"
    if [ -s "$FDLEAK_STATE" ]; then
      bad "lock/log/config file descriptor(s) leaked into a zfs child process:"
      cat "$FDLEAK_STATE"
    else
      ok "no lock/log/config file descriptor was inherited by any zfs child process"
    fi
  else
    rm -f /tmp/diffsnap_zfs_wrapper.$$
  fi
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
  rm -f "$FDLEAK_STATE"
fi
archive_log "35 - close-on-exec for lock/log/config fds"

echo "== 36. Cleanup =="
zfs destroy -R "$DS" 2>/dev/null
cp "$ORIG_CONF_BACKUP" "$CONF"
rm -f "$ORIG_CONF_BACKUP"

echo
echo "================================"
echo "RESULTS: $PASS passed, $FAIL failed"
echo "================================"
[ $FAIL -eq 0 ] && echo "ALL CLEAR" || echo "REVIEW FAILURES ABOVE"
echo "Full raw diffsnap.log transcript for every section saved to: $COMBINED_LOG"
