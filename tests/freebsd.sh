#!/usr/local/bin/bash
# diffsnap full regression suite (FreeBSD)
#   mdo bash freebsd.sh
#   requires pkg install bash flock
set -u
CONF=/usr/local/etc/diffsnap.conf
LOG=/var/log/diffsnap.log
LOCK=/var/run/diffsnap.lock
BIN=diffsnap
DS=zroot/clonetest   # adjust to your pool name
COMBINED_LOG=./diffsnap_full_log.txt
PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

SAVED_DATETIME=""
restore_clock_and_ntp() {
  if [ -n "$SAVED_DATETIME" ]; then
    date "$SAVED_DATETIME" >/dev/null 2>&1
  fi
  service ntpd start >/dev/null 2>&1
}
trap restore_clock_and_ntp EXIT

set_time() {
  # $1 = HH:MM:SS ; preserves today's date, BSD canonical format ccyymmddHHMM.ss
  local t="$1"
  local ymd; ymd=$(date +%Y%m%d)
  local hh="${t:0:2}" mm="${t:3:2}" ss="${t:6:2}"
  date "${ymd}${hh}${mm}.${ss}" >/dev/null 2>&1
}

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

echo "== Preflight: required commands =="
missing=0
for cmd in zfs truss "$BIN"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "MISSING: required command not found: $cmd"
    missing=1
  fi
done
[ "$missing" -eq 1 ] && { echo "Aborting: one or more required commands are missing."; exit 1; }
HAVE_FLOCK=1
command -v flock >/dev/null 2>&1 || { HAVE_FLOCK=0; echo "NOTE: 'flock' not found (pkg install flock). Section 7 will be skipped."; }

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
# dataset   interval  retention  prefix        recursive  min_bytes
cat > "$CONF" <<CONF
badline
$DS/a       notanumber 2         t1            no         0
$DS/a       1          0         t1            no         0
$DS/a       1          2         bad!prefix    no         0
$DS/a       1          2         t1            maybe      0
$DS/a       1          2         t1            no         notanumber
$DS/a       1          2         t1            no         0 trailing
CONF
"$BIN"; rc=$?
if [ $rc -ge 128 ]; then bad "process died from signal on malformed lines (exit $rc, signal $((rc-128)))"
else ok "no fatal signal on malformed lines (exit $rc)"; fi
grep -c "Config error" "$LOG" | grep -q "^7$" && ok "all 7 malformed lines logged" || bad "malformed line count mismatch: $(grep -c 'Config error' "$LOG")"
archive_log "1 - crash regression"

echo "== 2. Feature matrix: valid config =="
# dataset          interval  retention  prefix    recursive  min_bytes
cat > "$CONF" <<CONF
$DS                1         2          rectest   yes        0
$DS/a              1         2          rectest   no         0
$DS/b              1         2          skipme    no         999999999999
$DS/a              1         2          rectest   no         0
nosuch/dataset     1         2          t1        no         0
CONF
"$BIN"
grep -q "Created=$DS@rectest.*Recursive" "$LOG" && ok "recursive snapshot created" || bad "recursive snapshot missing"
if grep -q "$DS/a@rectest" "$LOG"; then bad "overlap dedup failed: $DS/a snapshotted despite recursive parent"
else ok "overlap dedup: $DS/a correctly excluded (covered by recursive parent)"; fi
grep -q "skipme" "$LOG" && bad "min_bytes threshold not respected" || ok "min_bytes skip correct (no skipme entry)"
grep -c "duplicate dataset/prefix" "$LOG" | grep -q "^1$" && ok "duplicate entry detected" || bad "duplicate detection failed"
grep -q "Configured dataset not found: nosuch/dataset" "$LOG" && ok "missing dataset logged" || bad "missing dataset not logged"
archive_log "2 - feature matrix"

echo "== 3. Retention drains to exactly 1 =="
# dataset   interval  retention  prefix  recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       1         1          rtest   no         0
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
# dataset   interval  retention  prefix  recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       1         2          p1      no         0
$DS/a       1         2          p2      no         0
$DS/a       1         2          p3      no         0
$DS/b       1         2          p1      no         0
CONF
rm -f /tmp/trace_batch.log
truss -f -a -o /tmp/trace_batch.log -- "$BIN" || bad "truss failed to run for section 5 (exit $?)"
[ -s /tmp/trace_batch.log ] || bad "truss trace file empty/missing for section 5 -- results below are unreliable"
grep -q "cannot create snapshots" "$LOG" && bad "same-dataset collision still occurs" || ok "no same-dataset collision"
strace_snap_pattern='zfs", "snapshot"'
snapcalls=$(grep -c "$strace_snap_pattern" /tmp/trace_batch.log)
[ "$snapcalls" -eq 3 ] && ok "zfs snapshot invocation count correct (3: p1[a+b], p2[a], p3[a])" || bad "unexpected snapshot invocation count: $snapcalls (expected 3)"
crossbatch=$(grep "$strace_snap_pattern" /tmp/trace_batch.log | grep -c "clonetest/a.*clonetest/b\|clonetest/b.*clonetest/a")
[ "$crossbatch" -ge 1 ] && ok "cross-dataset batching preserved (zroot/clonetest/a + zroot/clonetest/b in one call)" || bad "cross-dataset batching lost"
archive_log "5 - batching"

echo "== 6. Interval boundary matrix (per --help spec) =="
# dataset   interval  retention  prefix  recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       50        3          i50     no         0
$DS/a       1440      3          iday    no         0
CONF
SAVED_DATETIME=$(date "+%Y%m%d%H%M.%S")
service ntpd stop >/dev/null 2>&1
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
  set_time "$t"
  today=$(date +%Y-%m-%d)
  "$BIN"
  check_interval "i50" "$today" "$t" "${expect_i50[$t]}"
  check_interval "iday" "$today" "$t" "${expect_iday[$t]}"
done
date "$SAVED_DATETIME" >/dev/null 2>&1
service ntpd start >/dev/null 2>&1
SAVED_DATETIME=""
archive_log "6 - interval boundary matrix"

echo "== 7. Lock file / single-instance enforcement =="
if [ "$HAVE_FLOCK" -eq 1 ]; then
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
else
  echo "SKIP: flock not installed (pkg install flock)"
fi
archive_log "7 - lock enforcement"

echo "== 8. Maximum-length prefix accepted =="
maxprefix=$(printf 'a%.0s' $(seq 1 63))
# dataset   interval  retention  prefix       recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       1         2          $maxprefix   no         0
CONF
"$BIN"
grep -q "Created=$DS/a@${maxprefix}_" "$LOG" && ok "max-length (63-char) prefix accepted" || bad "max-length prefix rejected unexpectedly"
archive_log "8 - max prefix accepted"

echo "== 9. Prefix exceeding limit rejected =="
overprefix=$(printf 'a%.0s' $(seq 1 64))
# dataset   interval  retention  prefix       recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       1         2          $overprefix  no         0
CONF
"$BIN"
grep -q "prefix too long" "$LOG" && ok "over-length (64-char) prefix rejected" || bad "over-length prefix not rejected as expected"
archive_log "9 - prefix exceeding limit rejected"

echo "== 10. Dataset name at diffsnap's internal buffer limit (256 chars) =="
maxds_child_len=$((256 - ${#DS} - 1))
maxds_child=$(printf 'x%.0s' $(seq 1 $maxds_child_len))
maxds="$DS/$maxds_child"
# dataset   interval  retention  prefix    recursive  min_bytes
cat > "$CONF" <<CONF
$maxds      1         2          buftest   no         0
CONF
"$BIN"
if grep -q "dataset name too long" "$LOG"; then bad "256-char dataset name incorrectly rejected by diffsnap's buffer check"
elif grep -q "Configured dataset not found: $maxds" "$LOG"; then ok "256-char dataset name accepted intact by diffsnap's parser (buffer boundary correct)"
else bad "unexpected result for buffer-limit dataset name test"; fi
archive_log "10 - dataset name at buffer limit"

echo "== 11. Dataset name exceeding buffer limit rejected =="
overds="${maxds}xxxxxxxxxx"
# dataset   interval  retention  prefix     recursive  min_bytes
cat > "$CONF" <<CONF
$overds     1         2          buftest2   no         0
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
  cat > "$ZFS_REAL" <<'WRAP'
#!/usr/local/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  printf 'error: %s\n' "$(printf 'X%.0s' $(seq 1 700))" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a       1         2          longerr   no         0
CONF
  "$BIN"; rc=$?
  [ $rc -lt 128 ] && ok "no crash on >512-byte zfs stderr line" || bad "crashed on >512-byte zfs stderr line (exit $rc)"
  grep -q "Error: zfs:" "$LOG" && ok "oversized stderr line logged (split across reader buffer, not lost)" || bad "oversized stderr line not logged"
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
  ORIG_NOBODY_SHELL=$(getent passwd nobody 2>/dev/null | cut -d: -f7)
  [ -z "$ORIG_NOBODY_SHELL" ] && ORIG_NOBODY_SHELL=$(grep '^nobody:' /etc/passwd | cut -d: -f7)
  pw usermod nobody -s /bin/sh
  restore_nobody_shell() { pw usermod nobody -s "$ORIG_NOBODY_SHELL" 2>/dev/null; }

  rm -f "$LOCK"; : > "$LOCK"; chmod 666 "$LOCK"
  : > "$LOG"; chown root:wheel "$LOG"; chmod 600 "$LOG"
  permout=$(su nobody -c "$BIN" 2>&1); permrc=$?
  chmod 644 "$LOG"; rm -f "$LOCK"

  restore_nobody_shell

  if [ $permrc -ne 0 ] && echo "$permout" | grep -qi "failed to open log file"; then
    ok "unwritable log file correctly reported as error"
  else
    bad "unwritable log file not handled as expected (rc=$permrc, output: $permout)"
  fi
else
  echo "SKIP: 'nobody' user not available on this system"
fi
archive_log "17 - missing log permissions"

echo "== 18. Recursive retention pruning behaves correctly =="
# dataset   interval  retention  prefix     recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         1          rectest2   yes        0
CONF
"$BIN"; sleep 1; "$BIN"
parentcount=$(zfs list -t snap -H -o name | grep -c "^$DS@rectest2")
childcount=$(zfs list -t snap -H -o name | grep -c "^$DS/a@rectest2")
[ "$parentcount" -eq 1 ] && ok "recursive retention held on parent (count=$parentcount)" || bad "recursive retention violated on parent (count=$parentcount)"
[ "$childcount" -eq 1 ] && ok "recursive retention held on child via -r destroy (count=$childcount)" || bad "recursive retention violated on child (count=$childcount)"
archive_log "18 - recursive retention pruning"

echo "== 19. zfs get invoked with -t filesystem,volume filter =="
# dataset   interval  retention  prefix    recursive  min_bytes
cat > "$CONF" <<CONF
$DS/a       1         2          gettest   no         0
CONF
zfs snapshot "$DS/a@marker" 2>/dev/null
zfs bookmark "$DS/a@marker" "$DS/a#marker" 2>/dev/null
rm -f /tmp/trace_get.log
truss -f -a -o /tmp/trace_get.log -- "$BIN" || bad "truss failed to run for section 19 (exit $?)"
[ -s /tmp/trace_get.log ] || bad "truss trace file empty/missing for section 19 -- results below are unreliable"
get_pattern='"get", "-H", "-p".*"-t", "filesystem,volume"'
grep -qE "$get_pattern" /tmp/trace_get.log \
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
  cat > "$ZFS_REAL" <<'WRAP'
#!/usr/local/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "get" ]; then
  printf '%s\t123\n' "$(printf 'x%.0s' $(seq 1 300))"
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a       1         2          skiptest   no         0
CONF
  "$BIN"
  grep -q "Skipping metric line with oversized dataset name" "$LOG" && ok "oversized metric line logged and skipped" || bad "oversized metric line not logged"
  grep -q "Created=$DS/a@skiptest" "$LOG" && ok "valid dataset still snapshotted despite earlier bad line" || bad "good line lost after bad line (batch aborted?)"
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
  cat > "$ZFS_REAL" <<'WRAP'
#!/usr/local/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  echo "error: simulated snapshot failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  # dataset   interval  retention  prefix       recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a       1         2          scopetest   no         0
CONF
  POOL="${DS%%/*}"
rm -f /tmp/trace_scope.log
truss -f -a -o /tmp/trace_scope.log -- "$BIN" || bad "truss failed to run for section 21 (exit $?)"
[ -s /tmp/trace_scope.log ] || bad "truss trace file empty/missing for section 21 -- results below are unreliable"
  list_pattern='"list", "-H", "-r", "-t", "snapshot", "-o", "name", "-S", "creation", "'"$POOL"'"'
  grep -q "$list_pattern" /tmp/trace_scope.log \
    && ok "single-root batch verification used scoped -r zfs list ($POOL)" \
    || bad "expected scoped zfs list -r $POOL not found in trace"
  grep -q "Snapshot not created: $DS/a@scopetest" "$LOG" \
    && ok "simulated snapshot failure correctly detected via inventory check" \
    || bad "expected 'Snapshot not created' message missing"
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "21 - inventory scoping"

echo "== 22. Overlap dedup only applies to matching prefix (recursive parent + non-recursive child) =="
# dataset   interval  retention  prefix   recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          recA     yes        0
$DS/a       1         2          recB     no         0
CONF
"$BIN"
grep -q "Created=$DS/a@recB" "$LOG" \
  && ok "different-prefix child NOT deduped against recursive parent" \
  || bad "different-prefix child incorrectly deduped"

# dataset   interval  retention  prefix     recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          recSame   yes        0
$DS/a       1         2          recSame   no         0
CONF
"$BIN"
if grep -q "Created=$DS/a@recSame" "$LOG"; then bad "same-prefix child NOT deduped (overlap logic broken)"
else ok "same-prefix child correctly deduped against recursive parent"; fi
grep -q "Skipping $DS/a: covered by a recursive ancestor with prefix 'recSame'" "$LOG" \
  && ok "std/rec overlap dedup logged the skip" \
  || bad "std/rec overlap dedup did not log the skip (silent drop)"
archive_log "22 - prefix-aware overlap dedup"

echo "== 23. Nested recursive overlap, same prefix: descendant dropped before any zfs call =="
# dataset   interval  retention  prefix        recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          recSameNest   yes        0
$DS/a       1         2          recSameNest   yes        0
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
# dataset   interval  retention  prefix   recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          recX     yes        0
$DS/a       1         2          recY     yes        0
CONF
rm -f /tmp/trace_nested.log
truss -f -a -o /tmp/trace_nested.log -- "$BIN" || bad "truss failed to run for section 24 (exit $?)"
[ -s /tmp/trace_nested.log ] || bad "truss trace file empty/missing for section 24 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- ancestor+descendant likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@recX.*Recursive" "$LOG" \
  && ok "recursive ancestor snapshot created (different prefix, kept)" \
  || bad "recursive ancestor snapshot missing"
grep -q "Created=$DS/a@recY.*Recursive" "$LOG" \
  && ok "recursive descendant snapshot created (different prefix, kept)" \
  || bad "recursive descendant snapshot missing"
nested_snap_pattern='zfs", "snapshot"'
nestedcalls=$(grep -c "$nested_snap_pattern" /tmp/trace_nested.log)
[ "$nestedcalls" -eq 2 ] && ok "ancestor and descendant issued as 2 separate 'zfs snapshot' calls" || bad "expected 2 separate snapshot invocations, got $nestedcalls"
archive_log "24 - nested recursive overlap (different prefix, separate passes)"

echo "== 25. Recursive same-dataset duplicate (different prefixes): baseline dedup within rec_b =="
# dataset   interval  retention  prefix    recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          recDup1   yes        0
$DS         1         2          recDup2   yes        0
CONF
rm -f /tmp/trace_recdup.log
truss -f -a -o /tmp/trace_recdup.log -- "$BIN" || bad "truss failed to run for section 25 (exit $?)"
[ -s /tmp/trace_recdup.log ] || bad "truss trace file empty/missing for section 25 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- same-dataset recursive duplicates likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@recDup1.*Recursive" "$LOG" && ok "first duplicate recursive snapshot created" || bad "first duplicate recursive snapshot missing"
grep -q "Created=$DS@recDup2.*Recursive" "$LOG" && ok "second duplicate recursive snapshot created" || bad "second duplicate recursive snapshot missing"
recdup_snap_pattern='zfs", "snapshot"'
recdupcalls=$(grep -c "$recdup_snap_pattern" /tmp/trace_recdup.log)
[ "$recdupcalls" -eq 2 ] && ok "duplicate dataset issued as 2 separate 'zfs snapshot -r' calls (baseline pass dedup)" || bad "expected 2 separate snapshot invocations, got $recdupcalls"
archive_log "25 - recursive same-dataset duplicate"

echo "== 26. Three-level nested recursive chain, distinct prefixes: multiple pass bumps =="
# dataset   interval  retention  prefix   recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          lvl0     yes        0
$DS/a       1         2          lvl1     yes        0
$DS/a/c     1         2          lvl2     yes        0
CONF
zfs create -p "$DS/a/c" 2>/dev/null
rm -f /tmp/trace_chain.log
truss -f -a -o /tmp/trace_chain.log -- "$BIN" || bad "truss failed to run for section 26 (exit $?)"
[ -s /tmp/trace_chain.log ] || bad "truss trace file empty/missing for section 26 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- 3-level chain likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@lvl0.*Recursive" "$LOG" && ok "level-0 ancestor snapshot created" || bad "level-0 ancestor snapshot missing"
grep -q "Created=$DS/a@lvl1.*Recursive" "$LOG" && ok "level-1 snapshot created" || bad "level-1 snapshot missing"
grep -q "Created=$DS/a/c@lvl2.*Recursive" "$LOG" && ok "level-2 (deepest) snapshot created" || bad "level-2 snapshot missing"
chain_snap_pattern='zfs", "snapshot"'
chaincalls=$(grep -c "$chain_snap_pattern" /tmp/trace_chain.log)
[ "$chaincalls" -eq 3 ] && ok "3-level chain correctly split into 3 separate passes" || bad "expected 3 separate snapshot invocations, got $chaincalls"
archive_log "26 - three-level nested recursive chain"

echo "== 27. Duplicate ancestor plus descendant: pass assignment must avoid all ancestor passes =="
# dataset   interval  retention  prefix   recursive  min_bytes
cat > "$CONF" <<CONF
$DS         1         2          dupA     yes        0
$DS         1         2          dupB     yes        0
$DS/a       1         2          dupC     yes        0
CONF
rm -f /tmp/trace_dupanc.log
truss -f -a -o /tmp/trace_dupanc.log -- "$BIN" || bad "truss failed to run for section 27 (exit $?)"
[ -s /tmp/trace_dupanc.log ] || bad "truss trace file empty/missing for section 27 -- results below are unreliable"
grep -q "zfs snapshot batch execution failed" "$LOG" \
  && bad "zfs snapshot batch failed -- duplicate ancestor + descendant likely collided" \
  || ok "no zfs snapshot batch failure"
grep -q "Created=$DS@dupA.*Recursive" "$LOG" && ok "first duplicate ancestor snapshot created" || bad "first duplicate ancestor snapshot missing"
grep -q "Created=$DS@dupB.*Recursive" "$LOG" && ok "second duplicate ancestor snapshot created" || bad "second duplicate ancestor snapshot missing"
grep -q "Created=$DS/a@dupC.*Recursive" "$LOG" && ok "descendant snapshot created despite duplicated ancestor" || bad "descendant snapshot missing"
dupanc_snap_pattern='zfs", "snapshot"'
dupanccalls=$(grep -c "$dupanc_snap_pattern" /tmp/trace_dupanc.log)
[ "$dupanccalls" -eq 3 ] && ok "duplicate ancestor + descendant correctly split into 3 separate passes" || bad "expected 3 separate snapshot invocations, got $dupanccalls"
archive_log "27 - duplicate ancestor plus descendant pass assignment"

echo "== 28. Prune matching does not cross-match dataset name prefixes (e.g. DS/a vs DS/ab) =="
zfs create "$DS/ab" 2>/dev/null
cat > "$CONF" <<CONF
$DS/a       1         1          xmatch    no         0
$DS/ab      1         1          xmatch    no         0
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
  cat > "$ZFS_REAL" <<'WRAP'
#!/usr/local/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  echo "error: simulated snapshot failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  # dataset   interval  retention  prefix     recursive  min_bytes
  cat > "$CONF" <<CONF
$DS/a       1         2          combA      no         0
$DS/b       1         2          combB      yes        0
CONF
  rm -f /tmp/trace_comb.log
  truss -f -a -o /tmp/trace_comb.log -- "$BIN" || bad "truss failed to run for section 29 (exit $?)"
  [ -s /tmp/trace_comb.log ] || bad "truss trace file empty/missing for section 29 -- results below are unreliable"
  list_calls=$(grep '"list", "-H".*"-t", "snapshot"' /tmp/trace_comb.log | grep -vc "diffsnap_test_backup")
  [ "$list_calls" -eq 1 ] && ok "exactly one shared zfs list call covers both std and recursive verification (count=$list_calls)" || bad "expected exactly 1 zfs list call for combined verification, got $list_calls"
  grep -q "Snapshot not created: $DS/a@combA" "$LOG" && ok "std batch failure correctly detected via shared inventory" || bad "std batch failure not detected"
  grep -q "Snapshot not created: $DS/b@combB" "$LOG" && ok "recursive batch failure correctly detected via shared inventory" || bad "recursive batch failure not detected"
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "29 - combined batch verification single shared list call"

echo "== 30. Pruning reuses one shared snapshot listing instead of one zfs list call per dataset =="
cat > "$CONF" <<CONF
$DS/a       1         2          prlist1   no         0
$DS/b       1         2          prlist2   no         0
CONF
rm -f /tmp/trace_prlist.log
truss -f -a -o /tmp/trace_prlist.log -- "$BIN" || bad "truss failed to run for section 30 (exit $?)"
[ -s /tmp/trace_prlist.log ] || bad "truss trace file empty/missing for section 30 -- results below are unreliable"
list_calls=$(grep '"list", "-H".*"-t", "snapshot"' /tmp/trace_prlist.log | grep -vc "diffsnap_test_backup")
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
  cat > "$ZFS_REAL" <<'WRAP'
#!/usr/local/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "list" ] && [ "$2" = "-H" ]; then
  echo "error: simulated list failure" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  cat > "$CONF" <<CONF
$DS/a       1         2          listfail   no         0
CONF
  "$BIN"
  grep -q "Unable to list snapshots for batch verification and pruning" "$LOG" && ok "shared inventory failure logged" || bad "shared inventory failure not logged"
  grep -q "Unable to prune.*snapshots for $DS/a: snapshot inventory unavailable" "$LOG" && ok "pruning correctly skipped and logged when inventory unavailable" || bad "pruning failure not logged when inventory unavailable"
  restore_real_zfs
  trap restore_clock_and_ntp EXIT
fi
archive_log "31 - shared inventory listing failure coupling"

echo "== 32. Written= byte count reflects actual data written (cached metric value used in log) =="
mp=$(zfs get -H -o value mountpoint "$DS/a")
dd if=/dev/urandom of="$mp/testfile" bs=1m count=2 2>/dev/null
POOL="${DS%%/*}"
zpool sync "$POOL" 2>/dev/null || sync
cat > "$CONF" <<CONF
$DS/a       1         2          wtest     no         0
CONF
"$BIN"
writtenline=$(grep "Created=$DS/a@wtest" "$LOG")
echo "$writtenline" | grep -Eq 'Written=[0-9]' && ! echo "$writtenline" | grep -q "Written=0 " \
  && ok "Written= byte count reflects real data (non-zero, correctly cached value): $writtenline" \
  || bad "Written= value missing or zero despite real data written: $writtenline"
rm -f "$mp/testfile"
archive_log "32 - written byte count accuracy"

echo "== 33. Cleanup =="
zfs destroy -R "$DS" 2>/dev/null
cp "$ORIG_CONF_BACKUP" "$CONF"
rm -f "$ORIG_CONF_BACKUP"

echo
echo "================================"
echo "RESULTS: $PASS passed, $FAIL failed"
echo "================================"
[ $FAIL -eq 0 ] && echo "ALL CLEAR" || echo "REVIEW FAILURES ABOVE"
echo "Full raw diffsnap.log transcript for every section saved to: $COMBINED_LOG"
