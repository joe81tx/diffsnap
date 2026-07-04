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

echo "== Preflight: required commands =="
missing=0
for cmd in zfs strace flock timedatectl "$BIN"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "MISSING: required command not found: $cmd"
    missing=1
  fi
done
[ "$missing" -eq 1 ] && { echo "Aborting: one or more required commands are missing."; exit 1; }

: > "$COMBINED_LOG"

ORIG_CONF_BACKUP=$(mktemp)
[ -f "$CONF" ] && cp "$CONF" "$ORIG_CONF_BACKUP" || : > "$ORIG_CONF_BACKUP"

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
cat > "$CONF" <<CONF
badline
$DS/a notanumber 2 t1 no 0
$DS/a 1 0 t1 no 0
$DS/a 1 2 bad!prefix no 0
$DS/a 1 2 t1 maybe 0
$DS/a 1 2 t1 no notanumber
$DS/a 1 2 t1 no 0 trailing
CONF
"$BIN"; rc=$?
if [ $rc -ge 128 ]; then bad "process died from signal on malformed lines (exit $rc, signal $((rc-128)))"
else ok "no fatal signal on malformed lines (exit $rc)"; fi
grep -c "Config error" "$LOG" | grep -q "^7$" && ok "all 7 malformed lines logged" || bad "malformed line count mismatch: $(grep -c 'Config error' "$LOG")"
archive_log "1 - crash regression"

echo "== 2. Feature matrix: valid config =="
cat > "$CONF" <<CONF
$DS 1 2 rectest yes 0
$DS/a 1 2 rectest no 0
$DS/b 1 999999999999 skipme no 0
$DS/a 1 2 rectest no 0
nosuch/dataset 1 2 t1 no 0
CONF
"$BIN"
grep -q "Snapshot created (recursive): $DS@rectest" "$LOG" && ok "recursive snapshot created" || bad "recursive snapshot missing"
if grep -q "$DS/a@rectest" "$LOG"; then bad "overlap dedup failed: $DS/a snapshotted despite recursive parent"
else ok "overlap dedup: $DS/a correctly excluded (covered by recursive parent)"; fi
grep -q "skipme" "$LOG" && bad "min_bytes threshold not respected" || ok "min_bytes skip correct (no skipme entry)"
grep -c "duplicate dataset/prefix" "$LOG" | grep -q "^1$" && ok "duplicate entry detected" || bad "duplicate detection failed"
grep -q "Configured dataset not found: nosuch/dataset" "$LOG" && ok "missing dataset logged" || bad "missing dataset not logged"
archive_log "2 - feature matrix"

echo "== 3. Retention drains to exactly 1 =="
cat > "$CONF" <<CONF
$DS/a 1 1 rtest no 0
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
cat > "$CONF" <<CONF
$DS/a 1 2 p1 no 0
$DS/a 1 2 p2 no 0
$DS/a 1 2 p3 no 0
$DS/b 1 2 p1 no 0
CONF
strace -f -e trace=execve -o /tmp/trace_batch.log "$BIN"
grep -q "cannot create snapshots" "$LOG" && bad "same-dataset collision still occurs" || ok "no same-dataset collision"
snap_pattern='\["[^"]*zfs[^"]*", *"snapshot"'
snapcalls=$(grep -cE "$snap_pattern" /tmp/trace_batch.log)
[ "$snapcalls" -eq 3 ] && ok "zfs snapshot invocation count correct (3: p1[a+b], p2[a], p3[a])" || bad "unexpected snapshot invocation count: $snapcalls (expected 3)"
crossbatch=$(grep -E "$snap_pattern" /tmp/trace_batch.log | grep -c "clonetest/a.*clonetest/b\|clonetest/b.*clonetest/a")
[ "$crossbatch" -ge 1 ] && ok "cross-dataset batching preserved ($DS/a + $DS/b in one call)" || bad "cross-dataset batching lost"
archive_log "5 - batching"

echo "== 6. Interval boundary matrix (per --help spec) =="
cat > "$CONF" <<CONF
$DS/a 50 3 i50 no 0
$DS/a 1440 3 iday no 0
CONF
SAVED_DATETIME=$(date +"%Y-%m-%d %H:%M:%S")
timedatectl set-ntp false
declare -A expect_i50=( [00:00:00]=1 [00:50:00]=1 [00:10:00]=0 [23:20:00]=1 )
declare -A expect_iday=( [00:00:00]=1 [00:50:00]=0 [00:10:00]=0 [23:20:00]=0 )
check_interval() {
  local prefix="$1" today="$2" t="$3" expect="$4"
  local expected_name="${DS}/a@${prefix}_${today}_${t}"
  if [ "$expect" -eq 1 ]; then
    grep -qF "Snapshot created: $expected_name" "$LOG" \
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
maxprefix=$(printf 'a%.0s' $(seq 1 63))
cat > "$CONF" <<CONF
$DS/a 1 2 $maxprefix no 0
CONF
"$BIN"
grep -q "Snapshot created: $DS/a@${maxprefix}_" "$LOG" && ok "max-length (63-char) prefix accepted" || bad "max-length prefix rejected unexpectedly"
archive_log "8 - max prefix accepted"

echo "== 9. Prefix exceeding limit rejected =="
overprefix=$(printf 'a%.0s' $(seq 1 64))
cat > "$CONF" <<CONF
$DS/a 1 2 $overprefix no 0
CONF
"$BIN"
grep -q "prefix too long" "$LOG" && ok "over-length (64-char) prefix rejected" || bad "over-length prefix not rejected as expected"
archive_log "9 - prefix exceeding limit rejected"

echo "== 10. Dataset name at diffsnap's internal buffer limit (256 chars) =="
maxds_child_len=$((256 - ${#DS} - 1))
maxds_child=$(printf 'x%.0s' $(seq 1 $maxds_child_len))
maxds="$DS/$maxds_child"
cat > "$CONF" <<CONF
$maxds 1 2 buftest no 0
CONF
"$BIN"
if grep -q "dataset name too long" "$LOG"; then bad "256-char dataset name incorrectly rejected by diffsnap's buffer check"
elif grep -q "Configured dataset not found: $maxds" "$LOG"; then ok "256-char dataset name accepted intact by diffsnap's parser (buffer boundary correct)"
else bad "unexpected result for buffer-limit dataset name test"; fi
archive_log "10 - dataset name at buffer limit"

echo "== 11. Dataset name exceeding buffer limit rejected =="
overds="${maxds}xxxxxxxxxx"
cat > "$CONF" <<CONF
$overds 1 2 buftest2 no 0
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
#!/bin/bash
REAL="$0.diffsnap_test_backup"
if [ "$1" = "snapshot" ]; then
  printf 'error: %s\n' "$(printf 'X%.0s' $(seq 1 700))" >&2
  exit 1
fi
exec "$REAL" "$@"
WRAP
  chmod +x "$ZFS_REAL"
  cat > "$CONF" <<CONF
$DS/a 1 2 longerr no 0
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
cat > "$CONF" <<CONF
$DS 1 1 rectest2 yes 0
CONF
"$BIN"; sleep 1; "$BIN"
parentcount=$(zfs list -t snap -H -o name | grep -c "^$DS@rectest2")
childcount=$(zfs list -t snap -H -o name | grep -c "^$DS/a@rectest2")
[ "$parentcount" -eq 1 ] && ok "recursive retention held on parent (count=$parentcount)" || bad "recursive retention violated on parent (count=$parentcount)"
[ "$childcount" -eq 1 ] && ok "recursive retention held on child via -r destroy (count=$childcount)" || bad "recursive retention violated on child (count=$childcount)"
archive_log "18 - recursive retention pruning"

echo "== 19. Cleanup =="
zfs destroy -R "$DS" 2>/dev/null
cp "$ORIG_CONF_BACKUP" "$CONF"
rm -f "$ORIG_CONF_BACKUP"

echo
echo "================================"
echo "RESULTS: $PASS passed, $FAIL failed"
echo "================================"
[ $FAIL -eq 0 ] && echo "ALL CLEAR" || echo "REVIEW FAILURES ABOVE"
echo "Full raw diffsnap.log transcript for every section saved to: $COMBINED_LOG"
