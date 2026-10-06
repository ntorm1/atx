#!/usr/bin/env bash
# C3b gtest wrapper (after root's m2j-logs/tools/gt.sh; logs in pool-15's c3-logs): runs one gtest exe with one filter
# from the pool-15 root, logs the output, appends one summary line to c3b-gt-exits.txt (label, exe sha256 prefix,
# filter, exit, wall, passed / skipped / failed / disabled counts, 0xC0000142 hits: ENV-1).
# usage: c3b-gt.sh LABEL EXE LOG FILTER
set -u
label=$1; exe=$2; log=$3; filter=$4
logs=/c/atx-wt/pool-15/.superpowers/sdd/platform-p9-20261003/c3-logs
sha=$(sha256sum "$exe" | cut -c1-16)
t0=$(date +%s)
cd /c/atx-wt/pool-15 && "$exe" --gtest_filter="$filter" --gtest_color=no > "$log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
ran=$(grep -E '^\[==========\] [0-9]+ tests? from .* ran' "$log" | tail -1 | sed -E 's/^\[==========\] ([0-9]+) .*/\1/')
passed=$(grep -E '^\[  PASSED  \] [0-9]+' "$log" | sed -E 's/^\[  PASSED  \] ([0-9]+).*/\1/')
skipped=$(grep -E '^\[  SKIPPED \] [0-9]+ tests?, listed below' "$log" | sed -E 's/^\[  SKIPPED \] ([0-9]+).*/\1/')
failed=$(grep -E '^\[  FAILED  \] [0-9]+ tests?, listed below' "$log" | sed -E 's/^\[  FAILED  \] ([0-9]+).*/\1/')
disabled=$(grep -E 'YOU HAVE [0-9]+ DISABLED' "$log" | sed -E 's/.*YOU HAVE ([0-9]+) DISABLED.*/\1/')
env1=$(grep -c -E '3221225794|0xC0000142|0xc0000142' "$log")
line="$label exe ${sha} filter '$filter' exit $rc wall ${wall}s ran ${ran:-?} passed ${passed:-0} skipped ${skipped:-0} failed ${failed:-0} disabled ${disabled:-0} env1-hits $env1"
echo "$line" >> "$logs/c3b-gt-exits.txt"
echo "$line"
exit $rc
