#!/usr/bin/env bash
# GATE step 2, GATE-COVDIFF (ledger "Ruling GATE-COVDIFF"; COVR-1: COV-reading runs through the bounded runner, the
# COV-9 role check first): `atx-equity-strategy-risk cov-diff --legacy STORE` on one container store with the step-1
# Release risk exe (p9-3f), through scripts/run_bounded_research.py (--seconds 600 --max-rss-mib 2048, --build-type
# Release, admission wait 1800 s). The runner dir is build-equity/p9-gate-covdiff-<NAME>-run (its receipt copied to
# gate-logs/receipt-p9-gate-covdiff-<NAME>.json, the run's console to gate-logs/covdiff-<NAME>.log); one line per run
# is appended to gate-logs/covdiff-runs.txt (exe SHA-256 before and after, runner exit, receipt outcome / exit /
# wall / peak, whether the receipt or start record carries a `store` block (SQL3-2: none)). Reads no statistic: the
# verb prints "legacy equal: <sessions> sessions <files>" or the first differing session.
# usage: covdiff_run.sh NAME STORE_DIR
set -u
name=$1; store=$2
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
exe=C:/atx-wt/pool-2/build-equity-rel/bin/atx-equity-strategy-risk.exe
run=build-equity/p9-gate-covdiff-$name-run
cd "$root" || exit 2
if [ -e "$run" ]; then echo "covdiff_run.sh: $run exists"; exit 2; fi
before=$(sha256sum "$exe" | cut -d' ' -f1)
start=$(date -u +%FT%TZ)
PYTHONDONTWRITEBYTECODE=1 "/c/Program Files/Python312/python.exe" scripts/run_bounded_research.py --output "$run" \
  --seconds 600 --max-rss-mib 2048 --build-type Release --admission-wait-seconds 1800 \
  -- "$exe" cov-diff --legacy "$store" > "$logs/covdiff-$name.log" 2>&1
rc=$?
after=$(sha256sum "$exe" | cut -d' ' -f1)
rec="$run/receipt.json"
info="no receipt"
if [ -f "$rec" ]; then
  cp "$rec" "$logs/receipt-p9-gate-covdiff-$name.json"
  info=$("/c/Program Files/Python312/python.exe" -I -c "
import json, sys
r = json.load(open(sys.argv[1], encoding='utf-8'))
s = json.load(open(sys.argv[2], encoding='utf-8'))
print('outcome', r.get('outcome'), 'exit', r.get('exit_code'), 'wall_s', r.get('wall_seconds'), 'peak_MiB',
      round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20), 'exe', (r.get('executable_sha256') or '')[:16],
      'build_type', r.get('build_type'), 'source', (r.get('source_sha') or '')[:8],
      'store-block receipt', 'store' in r, 'start', 'store' in s)
" "$rec" "$run/start.json")
fi
verb="no stdout.log"
if [ -f "$run/stdout.log" ]; then
  cp "$run/stdout.log" "$logs/covdiff-$name-stdout.log"; cp "$run/stderr.log" "$logs/covdiff-$name-stderr.log"
  verb=$(cat "$run/stdout.log" "$run/stderr.log" | tr -d '\r' | tr '\n' ' ' | cut -c1-300)
fi
line="$name store $store start $start exe-before ${before:0:16} exe-after ${after:0:16} runner-exit $rc :: $info :: verb: $verb"
echo "$line" >> "$logs/covdiff-runs.txt"
echo "$line"
exit $rc
