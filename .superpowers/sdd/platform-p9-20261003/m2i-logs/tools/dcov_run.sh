#!/usr/bin/env bash
# M2i C2 / C3: one D-COV risk run through the bounded runner (gate report 4 (d)). S9 first (4 (a)), exe SHA-256 before,
# the runner, the receipt copied to m2i-logs/receipt-<name>.json, one summary line to m2i-logs/dcov-runs.txt.
# usage: dcov_run.sh NAME RECIPE MODE      (MODE = all | aff1; output build-equity/NAME, run dir build-equity/NAME-run)
set -u
name=$1; recipe=$2; mode=$3
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/m2i-logs
py="/c/Program Files/Python312/python.exe"
exe='C:\atx-wt\pool-2\build-equity-rel\bin\atx-equity-strategy-risk.exe'
cd "$root" || exit 2
export PYTHONDONTWRITEBYTECODE=1
{ echo "== before $name ($(date -u +%FT%TZ))"; "$py" .superpowers/sdd/platform-p9-20261003/m2g-logs/tools/s9_check.py \
    build-equity/train-2020-2023-lo3/manifest.json e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4; } \
  >> "$logs/cov-s9.txt" 2>&1
grep -q "S9 HOLDS" <(tail -1 "$logs/cov-s9.txt") || { echo "S9 does not hold: stop"; exit 3; }
sha_before=$(sha256sum build-equity-rel/bin/atx-equity-strategy-risk.exe | cut -d' ' -f1)
runner=(--output "build-equity/$name-run" --seconds 600 --max-rss-mib 2048 --build-type Release
        --admission-wait-seconds 1800 --
        "$exe" risk --role build-equity/train-2020-2023-lo3/manifest.json
        --role-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4
        --fields build-equity/train-2020-2023-lo3-fields-v10/manifest.json
        --fields-sha256 a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809
        --output "build-equity/$name" --emit-exposures all --emit-container --bias-families extended
        --recipe "$recipe")
t0=$(date +%s)
if [ "$mode" = aff1 ]; then
  stop="$logs/.aff-stop-$name"; rm -f "$stop"
  "$py" "$logs/tools/aff_probe.py" "$logs/aff-probe-$name.txt" 900 "$stop" &
  probe=$!
  cmd //c "$(cygpath -w "$logs/tools/aff1.cmd")" "${runner[@]}" > "$logs/runner-$name.log" 2>&1
  rc=$?
  touch "$stop"; wait $probe; rm -f "$stop"
else
  "$py" scripts/run_bounded_research.py "${runner[@]}" > "$logs/runner-$name.log" 2>&1
  rc=$?
fi
wall=$(( $(date +%s) - t0 ))
cp "build-equity/$name-run/receipt.json" "$logs/receipt-$name.json" 2>/dev/null
sha_after=$(sha256sum build-equity-rel/bin/atx-equity-strategy-risk.exe | cut -d' ' -f1)
summary=$("$py" -c "
import json,sys
r=json.load(open('build-equity/$name-run/receipt.json'))
print('outcome', r.get('outcome'), 'exit', r.get('exit_code'), 'wall_s', round(r.get('wall_seconds') or -1, 1),
      'peak_MiB', round((r.get('sampled_peak_tree_rss_bytes') or 0)/2**20), 'exe', (r.get('executable_sha256') or '')[:16],
      'argv_sha', (r.get('argv_sha256') or '')[:16], 'source', (r.get('source_sha') or '')[:8], 'build_type', r.get('build_type'))
" 2>&1)
line="$name recipe $recipe mode $mode runner-exit $rc shell-wall ${wall}s exe-before ${sha_before:0:16} exe-after ${sha_after:0:16} :: $summary"
echo "$line" >> "$logs/dcov-runs.txt"
echo "$line"
