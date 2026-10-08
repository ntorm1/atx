#!/usr/bin/env bash
# M2: the identity argv WITHOUT --dsr-total (B2 re-review 2 item 3, third bullet), merged head, Debug verb p9-2t, bounded.
set -u
ROOT=C:/atx-wt/pool-2
PY="C:/Program Files/Python312/python.exe"
L=C:/Users/natha/AppData/Local/Temp/claude/c--atx/a3de34c1-d7ad-4b5c-9079-b9c90532d424/scratchpad/root-m2/ledger-copy/trials.jsonl
OUT=build-equity/p9-m2-b2-identity
cd "$ROOT" || exit 9
export PYTHONDONTWRITEBYTECODE=1; unset ATX_EQUITY_BIN
run() {
  local pair=$1 child=$2 parent=$3; local lp; lp=$(echo "$pair" | tr 'A-Z' 'a-z')
  "$PY" scripts/run_bounded_research.py --output "build-equity/p9-m2-b2-$lp-nototal-run" --seconds 600 --max-rss-mib 2048 \
    --bind "$L" -- "$PY" atx-impl/tools/nav_summ.py "$child" --reference "$parent" --protocol v8 --psr \
    --dsr-ledger "$L" --json "$OUT/$pair-nototal.json" --engine-stats "$ROOT/build-equity/bin/atx-research-eval.exe" \
    > /dev/null 2>&1
  echo "$pair nototal exit=$?"
}
run X5 build-equity/p9-c1-s2-x5-nav build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b
run YF0 build-equity/p9-b0-yf0-nav build-equity/p9-c1-s2-x5-nav
