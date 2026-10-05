#!/usr/bin/env bash
# M2f B2 TRAIN identity runs (list committed first, ea52989a): P / Erel / Edbg per pair, each through the bounded runner.
set -u
ROOT=C:/atx-wt/pool-2
PY="C:/Program Files/Python312/python.exe"
L=C:/Users/natha/AppData/Local/Temp/claude/c--atx/6ea15be9-c1e8-4316-99d4-4854dfd7c03c/scratchpad/m2f-ledger-copy/trials.jsonl
OUT=build-equity/p9-m2f-b2-identity
X3=build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b
X5=build-equity/p9-c1-s2-x5-nav
YF0=build-equity/p9-b0-yf0-nav
EREL=$ROOT/build-equity-rel/bin/atx-research-eval.exe
EDBG=$ROOT/build-equity/bin/atx-research-eval.exe
cd "$ROOT" || exit 9
mkdir -p "$OUT"
export PYTHONDONTWRITEBYTECODE=1
run() {
  local pair=$1 mode=$2 child=$3 parent=$4; shift 4
  local lp; lp=$(echo "$pair" | tr 'A-Z' 'a-z'); local lm; lm=$(echo "$mode" | tr 'A-Z' 'a-z')
  "$PY" scripts/run_bounded_research.py --output "build-equity/p9-m2f-b2-$lp-$lm-run" --seconds 600 --max-rss-mib 2048 \
    --bind "$L" -- "$PY" atx-impl/tools/nav_summ.py "$child" --reference "$parent" --protocol v8 --psr \
    --dsr-ledger "$L" --dsr-total "$L" --json "$OUT/$pair-$mode.json" "$@"
  echo "$pair $mode exit=$?"
}
for spec in "X5 $X5 $X3" "YF0 $YF0 $X5"; do
  set -- $spec
  run "$1" P "$2" "$3"
  run "$1" Erel "$2" "$3" --engine-stats "$EREL"
  run "$1" Edbg "$2" "$3" --engine-stats "$EDBG"
done
