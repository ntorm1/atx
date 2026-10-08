#!/usr/bin/env bash
# M2 (RESUME 9) B2 TRAIN identity runs on the merged head (B2 re-review 2 "What root must check at merge" item 3):
# flag-absent nav_summ on X-5 and Y-F0 with the merged head's Debug verb p9-2t, each through the bounded runner,
# argv as root's M2f Edbg runs (m2f-logs/tools/b2_identity_run.sh) except the ledger COPY path and the --json path.
# Never --ledger; the read-only ledger flags get a COPY; the reference is never among the dirs.
set -u
ROOT=C:/atx-wt/pool-2
PY="C:/Program Files/Python312/python.exe"
S=C:/Users/natha/AppData/Local/Temp/claude/c--atx/a3de34c1-d7ad-4b5c-9079-b9c90532d424/scratchpad/root-m2
L=$S/ledger-copy/trials.jsonl
OUT=build-equity/p9-m2-b2-identity
X3=build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b
X5=build-equity/p9-c1-s2-x5-nav
YF0=build-equity/p9-b0-yf0-nav
EDBG=$ROOT/build-equity/bin/atx-research-eval.exe
cd "$ROOT" || exit 9
mkdir -p "$S/ledger-copy" "$OUT"
cp build-equity/trials.jsonl "$L"
echo "ledger copy: $(wc -l < "$L") lines sha256 $(sha256sum "$L" | cut -c1-8)"
echo "verb: $(sha256sum "$EDBG" | cut -c1-64)"
export PYTHONDONTWRITEBYTECODE=1
unset ATX_EQUITY_BIN
run() {
  local pair=$1 child=$2 parent=$3; local lp; lp=$(echo "$pair" | tr 'A-Z' 'a-z')
  "$PY" scripts/run_bounded_research.py --output "build-equity/p9-m2-b2-$lp-run" --seconds 600 --max-rss-mib 2048 \
    --bind "$L" -- "$PY" atx-impl/tools/nav_summ.py "$child" --reference "$parent" --protocol v8 --psr \
    --dsr-ledger "$L" --dsr-total "$L" --json "$OUT/$pair.json" --engine-stats "$EDBG" > /dev/null 2>&1
  echo "$pair exit=$?"
}
run X5 "$X5" "$X3"
run YF0 "$YF0" "$X5"
echo "trial ledger after: $(wc -l < build-equity/trials.jsonl) lines sha256 $(sha256sum build-equity/trials.jsonl | cut -c1-8)"
