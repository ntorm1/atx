#!/usr/bin/env bash
# GATE (RESUME 12; copied from m6-logs/tools/pyt.sh, logs dir gate-logs) pytest wrapper: one invocation, explicit
# paths, -p no:cacheprovider, own --basetemp (its parent created first), PYTHONDONTWRITEBYTECODE=1, one hash seed.
# The exe set is chosen by the caller's environment: with GATE_BUILD=rel (default) every ATX_* exe variable the suites
# read points at the Release exes of build-equity-rel/bin; GATE_BUILD=dbg points them at the Debug exes of
# build-equity/bin. ATX_EQUITY_ROOT is always build-equity (the data root).
# GATE-TMP (RESUME 13, gate agent 2; the one logged change, `gate-logs/tools/CHANGES.txt`): the basetemp AND TMP / TEMP
# of every invocation sit under ONE short per-invocation root, fresh before the run, deleted by literal name by the
# caller after the run's record. INV is a short name (2 characters): the tiny-world canary's IC cache path under TMP
# must stay within W1-I1's 250 characters.
#   GATE_PT=ruled : root C:/atx-wt/pool-2/build-equity/pt/<INV>, basetemp <root>/b, TMP / TEMP <root>/t (the GATE-TMP
#                   ruling's literal path; inside pool-2's git work tree, where `fresh_root` raises and
#                   `research_cycle.py --no-git` refuses (K3): used for the one probe that records this).
#   default       : root C:/Users/natha/AppData/Local/Temp/g2<INV> (short, OUTSIDE any git work tree), basetemp
#                   <root>/b, TMP / TEMP <root> (deepest tiny-world IC cache path 247 <= 250).
# Writes <log> (pytest output) and appends one line to gate-logs/pyt-exits.txt: label, head, seed, build, root,
# code-dirty paths, exit, wall, count of 3221225794 / 0xC0000142 in the log (ENV-1), the pytest summary line.
# usage: [GATE_PT=ruled] pyt.sh LABEL SEED INV LOG -- pytest-args...
set -u
label=$1; seed=$2; inv=$3; log=$4; shift 4
[ "$1" = "--" ] && shift
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
build=${GATE_BUILD:-rel}
if [ "$build" = "rel" ]; then bin=C:/atx-wt/pool-2/build-equity-rel/bin; btype=Release; else bin=C:/atx-wt/pool-2/build-equity/bin; btype=Debug; fi
case "$inv" in
  ''|*/*|*\\*|*' '*) echo "pyt.sh: bad INV '$inv'"; exit 2 ;;
esac
if [ "${GATE_PT:-}" = "ruled" ]; then
  parent=C:/atx-wt/pool-2/build-equity/pt; pt=$parent/$inv; bt=$pt/b; tmp=$pt/t
elif [ "${GATE_PT:-}" = "systmp" ]; then
  # basetemp under the short root, TMP / TEMP = the OS temp dir itself (33 characters): for a test whose golden
  # depends on the TMP length (atx-impl/tools/test_mega_report_v8.py::test_v7_config_renders_the_pre_v8_bytes
  # asserts len(html) with the mkdtemp root inside the html; CHANGES.txt, second entry)
  parent=C:/Users/natha/AppData/Local/Temp; pt=$parent/g2$inv; bt=$pt/b; tmp='C:\Users\natha\AppData\Local\Temp'
else
  parent=C:/Users/natha/AppData/Local/Temp; pt=$parent/g2$inv; bt=$pt/b; tmp=$pt
fi
cd "$root" || exit 2
if [ -e "$pt" ]; then echo "pyt.sh: $pt exists (not fresh)"; exit 2; fi
mkdir -p "$parent" && mkdir "$pt" && mkdir -p "$tmp" || exit 2
head=$(git rev-parse HEAD)
dirty=$(git status --porcelain -- . ':!.superpowers' ':!docs/plans' | tr '\n' ' ')
start=$(date -u +%FT%TZ); t0=$(date +%s)
ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity ATX_EQUITY_BIN=$bin ATX_EQUITY_BUILD_TYPE=$btype \
ATX_EQUITY_IC_EXE=$bin/atx-equity-strategy-ic.exe ATX_EQUITY_TARGETS_EXE=$bin/atx-equity-strategy-targets.exe \
ATX_RESEARCH_STORE_EXE=$bin/atx-research-store.exe ATX_RESEARCH_ADMISSION_EXE=$bin/atx-research-admission.exe \
ATX_RESEARCH_COMPOSITION_EXE=$bin/atx-research-composition.exe ATX_RESEARCH_FIELDS_EXE=$bin/atx-research-fields.exe \
TMP="$tmp" TEMP="$tmp" \
PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=$seed "/c/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider \
  --basetemp "$bt" "$@" > "$log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
env1=$(grep -c -E '3221225794|0xC0000142|0xc0000142' "$log")
summary=$(grep -E '^(=+ )?[0-9]+ (passed|failed)|[0-9]+ passed|no tests ran|[0-9]+ failed|[0-9]+ error' "$log" | tail -1)
echo "$label seed $seed build $build root $pt head ${head:0:8} code-dirty [${dirty}] exit $rc wall ${wall}s start $start env1-hits $env1 :: $summary" \
  >> "$logs/pyt-exits.txt"
echo "$label exit $rc wall ${wall}s env1-hits $env1 :: $summary"
exit $rc
