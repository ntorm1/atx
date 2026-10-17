#!/usr/bin/env bash
# M6B-FINISH (RESUME 12) item 7 (SQL4 review item 10, SQL4-S S-6): a fresh real-tree catalog OUTSIDE
# build-equity/research-store/ (SQL3-2: the default path stays absent, so the record hook stays off), built through the
# bounded runner (--seconds 600 --max-rss-mib 2048), then `verify --catalog <it> --ledger build-equity/trials.jsonl`.
# The trial ledger is only read; its line count and sha256 prefix are logged before and after.
# usage: realcat.sh <catalog-path-outside-the-repo> <runner-dir-name> <logs-dir>
set -u
DB=$1; RUN=$2; LOGS=$3
ROOT=C:/atx-wt/pool-2
EXE=$ROOT/build-equity/bin/atx-research-store.exe
VC=/c/atx-cache/vcpkg_installed/x64-windows
export PATH="$VC/debug/bin:$VC/bin:$PATH" PYTHONDONTWRITEBYTECODE=1
PY="C:/Program Files/Python312/python.exe"
cd "$ROOT" || exit 9
led() { echo "trial ledger $(wc -l < build-equity/trials.jsonl) lines sha256 $(sha256sum build-equity/trials.jsonl | cut -c1-8); research-store default dir: $([ -e build-equity/research-store ] && echo PRESENT || echo absent)"; }
echo "time $(date -u +%FT%TZ) head $(git rev-parse --short=8 HEAD) store exe sha256 $(sha256sum "$EXE" | cut -c1-64)"
echo "before: $(led)"
[ -e "$DB" ] && { echo "catalog $DB exists: stop"; exit 8; }
"$PY" scripts/run_bounded_research.py --output "build-equity/$RUN" --seconds 600 --max-rss-mib 2048 --min-free-mib 512 \
  --build-type Debug --admission-wait-seconds 3600 -- "$EXE" catalog --catalog "$DB" --root "$ROOT" \
  --ledger build-equity/trials.jsonl --classes atx-engine/schemas/research_store/classes.json > "$LOGS/realcat-runner.log" 2>&1
echo "runner exit $?"
"$PY" -c "
import json
o = json.load(open('build-equity/$RUN/receipt.json', encoding='utf-8'))
print('runner outcome', o['outcome'], 'exit', o['exit_code'], 'wall_seconds', round(o['wall_seconds'], 1),
      'peak_rss_mib', (o.get('sampled_peak_tree_rss_bytes') or 0) >> 20, 'store block', o.get('store'))
"
echo "catalog stdout (counts and digest lines only):"
grep -E "files_seen|catalog_digest" "build-equity/$RUN/stdout.log"
echo "verify --catalog <it> --ledger build-equity/trials.jsonl:"
"$EXE" verify --catalog "$DB" --ledger build-equity/trials.jsonl > "$LOGS/realcat-verify.txt" 2> "$LOGS/realcat-verify.err"
echo "verify exit $?"
cat "$LOGS/realcat-verify.txt" "$LOGS/realcat-verify.err"
echo "after: $(led)"
