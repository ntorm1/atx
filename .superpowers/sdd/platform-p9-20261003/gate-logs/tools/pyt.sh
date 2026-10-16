#!/usr/bin/env bash
# GATE (RESUME 12; copied from m6-logs/tools/pyt.sh, logs dir gate-logs) pytest wrapper: one invocation, explicit
# paths, -p no:cacheprovider, own --basetemp (its parent created first), PYTHONDONTWRITEBYTECODE=1, one hash seed.
# The exe set is chosen by the caller's environment: with GATE_BUILD=rel (default) every ATX_* exe variable the suites
# read points at the Release exes of build-equity-rel/bin; GATE_BUILD=dbg points them at the Debug exes of
# build-equity/bin. ATX_EQUITY_ROOT is always build-equity (the data root). TMP / TEMP point at <basetemp>-tmp.
# Writes <log> (pytest output) and appends one line to gate-logs/pyt-exits.txt: label, head, seed, build, code-dirty
# paths, exit, wall, count of 3221225794 / 0xC0000142 in the log (ENV-1), the pytest summary line.
# usage: pyt.sh LABEL SEED BASETEMP LOG -- pytest-args...
set -u
label=$1; seed=$2; bt=$3; log=$4; shift 4
[ "$1" = "--" ] && shift
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
build=${GATE_BUILD:-rel}
if [ "$build" = "rel" ]; then bin=C:/atx-wt/pool-2/build-equity-rel/bin; btype=Release; else bin=C:/atx-wt/pool-2/build-equity/bin; btype=Debug; fi
cd "$root" || exit 2
mkdir -p "$(dirname "$bt")" "$bt-tmp"
head=$(git rev-parse HEAD)
dirty=$(git status --porcelain -- . ':!.superpowers' ':!docs/plans' | tr '\n' ' ')
start=$(date -u +%FT%TZ); t0=$(date +%s)
ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity ATX_EQUITY_BIN=$bin ATX_EQUITY_BUILD_TYPE=$btype \
ATX_EQUITY_IC_EXE=$bin/atx-equity-strategy-ic.exe ATX_EQUITY_TARGETS_EXE=$bin/atx-equity-strategy-targets.exe \
ATX_RESEARCH_STORE_EXE=$bin/atx-research-store.exe ATX_RESEARCH_ADMISSION_EXE=$bin/atx-research-admission.exe \
ATX_RESEARCH_COMPOSITION_EXE=$bin/atx-research-composition.exe ATX_RESEARCH_FIELDS_EXE=$bin/atx-research-fields.exe \
TMP="$bt-tmp" TEMP="$bt-tmp" \
PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=$seed "/c/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider \
  --basetemp "$bt" "$@" > "$log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
env1=$(grep -c -E '3221225794|0xC0000142|0xc0000142' "$log")
summary=$(grep -E '^(=+ )?[0-9]+ (passed|failed)|[0-9]+ passed|no tests ran|[0-9]+ failed|[0-9]+ error' "$log" | tail -1)
echo "$label seed $seed build $build head ${head:0:8} code-dirty [${dirty}] exit $rc wall ${wall}s start $start env1-hits $env1 :: $summary" \
  >> "$logs/pyt-exits.txt"
echo "$label exit $rc wall ${wall}s env1-hits $env1 :: $summary"
exit $rc
