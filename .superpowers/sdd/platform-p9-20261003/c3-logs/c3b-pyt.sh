#!/usr/bin/env bash
# C3b pytest wrapper (after root's m2j-logs/tools/pyt.sh; logs in pool-15's c3-logs): one invocation, explicit paths,
# -p no:cacheprovider, own --basetemp, PYTHONDONTWRITEBYTECODE=1, one hash seed. Writes <log> (pytest output) and
# appends one line to c3b-pyt-exits.txt: label, head, seed, exit, wall, count of 3221225794 / 0xC0000142 (ENV-1).
# usage: c3b-pyt.sh LABEL SEED BASETEMP LOG -- pytest-args...
set -u
label=$1; seed=$2; bt=$3; log=$4; shift 4
[ "$1" = "--" ] && shift
root=/c/atx-wt/pool-15
logs=$root/.superpowers/sdd/platform-p9-20261003/c3-logs
cd "$root" || exit 2
head=$(git rev-parse HEAD)
dirty=$(git status --porcelain -- . ':!.superpowers' ':!docs/plans' | tr '\n' ' ')
start=$(date -u +%FT%TZ); t0=$(date +%s)
PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=$seed "/c/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider \
  --basetemp "$bt" "$@" > "$log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
env1=$(grep -c -E '3221225794|0xC0000142|0xc0000142' "$log")
summary=$(grep -E '^(=+ )?[0-9]+ (passed|failed)|[0-9]+ passed|no tests ran' "$log" | tail -1)
echo "$label seed $seed head ${head:0:8} code-dirty [${dirty}] exit $rc wall ${wall}s start $start env1-hits $env1 :: $summary" \
  >> "$logs/c3b-pyt-exits.txt"
echo "$label exit $rc wall ${wall}s env1-hits $env1 :: $summary"
exit $rc
