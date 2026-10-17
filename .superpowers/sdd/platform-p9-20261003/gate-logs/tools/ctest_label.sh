#!/usr/bin/env bash
# GATE step 2 (plan 4.2 "suites": `ctest -L atx_research`, `-L atx_equity_strategy` through the wrapper; G-P4): one
# CTest label of one build tree through scripts/atx-build.ps1 -Ctest (serial, -Jobs 1: the evidence default). Writes
# gate-logs/ctest-<build>-<label>.log and appends one line to gate-logs/ctest-exits.txt (exit, wall, the CTest
# summary line, counts of Skipped / Not Run / Failed lines, 0xC0000142 hits).
# usage: ctest_label.sh rel|dbg LABEL
set -u
build=$1; label=$2
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
preset=equity-dev; [ "$build" = "rel" ] && preset=equity-rel
log=$logs/ctest-$build-$label.log
cd "$root" || exit 2
t0=$(date +%s); start=$(date -u +%FT%TZ)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Ctest -Preset "$preset" -Jobs 1 -L "$label" \
  > "$log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
summary=$(grep -E 'tests passed, [0-9]+ tests? failed out of' "$log" | tail -1 | tr -d '\r')
skipped=$(grep -c -E '\*\*\*Skipped|Skipped +[0-9.]+ sec' "$log")
notrun=$(grep -c -E 'Not Run' "$log")
failed=$(grep -c -E '\*\*\*Failed|\*\*\*Exception|\*\*\*Timeout' "$log")
env1=$(grep -c -E '3221225794|0xC0000142|0xc0000142' "$log")
line="$build-$label exit $rc wall ${wall}s start $start :: ${summary:-no summary} | skipped-lines $skipped not-run-lines $notrun failed-lines $failed env1-hits $env1"
echo "$line" >> "$logs/ctest-exits.txt"
echo "$line"
exit $rc
