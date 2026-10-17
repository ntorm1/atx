#!/usr/bin/env bash
# M2i C5 (COV report step 4): the P9-B0 Y-1 NAV argv with three tokens changed (output, --risk-model the Debug-built
# container store p9-m2d-cov-risk-container, its manifest pin), through the bounded runner; then nav_cmp against the
# P9-B0 reference build-equity/p9-b0-y1-nav (allowed: summary.json /producer/build_type, /producer/engine_git_sha).
# usage: step4_run.sh rel|dbg
set -u
mode=$1
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/m2i-logs
py="/c/Program Files/Python312/python.exe"
cd "$root" || exit 2
export PYTHONDONTWRITEBYTECODE=1
if [ "$mode" = rel ]; then
  exe='C:\atx-wt\pool-2\build-equity-rel\bin\atx-equity-strategy-targets.exe'; bt=Release; out=p9-m2i-cov-y1-nav
  exe_posix=build-equity-rel/bin/atx-equity-strategy-targets.exe
else
  exe='C:\atx-wt\pool-2\build-equity\bin\atx-equity-strategy-targets.exe'; bt=Debug; out=p9-m2i-cov-y1-nav-dbg
  exe_posix=build-equity/bin/atx-equity-strategy-targets.exe
fi
sha_before=$(sha256sum "$exe_posix" | cut -d' ' -f1)
t0=$(date +%s)
"$py" scripts/run_bounded_research.py --output "build-equity/$out-run" --seconds 600 --max-rss-mib 2048 \
  --build-type "$bt" --admission-wait-seconds 1800 -- \
  "$exe" nav --combined build-equity/mega-v8xw-train-theme-erc-v8ysb-2/train_combined.json \
  --combined-sha256 e13fbc4dab75c00cc47d7a06d8fcfbefd2d1368407565e99001136d19de9da8c \
  --role build-equity/train-2020-2023-lo3/manifest.json \
  --role-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 \
  --fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json \
  --fields-sha256 26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09 \
  --output "build-equity/$out" --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 \
  --aim-leverage 2.0 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 \
  --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache \
  --warm-start-sessions 60 --capacity-curve --vol-target vol-target-v1 \
  --risk-model build-equity/p9-m2d-cov-risk-container \
  --risk-model-sha256 9c12852ea0cd578d2cf968fc858ae7cb986c308f4b533824a34d273ca7e017c7 \
  --label-role build-equity/train-2020-2023-lo3-dlret/manifest.json \
  --label-role-sha256 95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069 \
  > "$logs/runner-$out.log" 2>&1
rc=$?
wall=$(( $(date +%s) - t0 ))
cp "build-equity/$out-run/receipt.json" "$logs/receipt-$out.json" 2>/dev/null
summary=$("$py" -c "
import json
r=json.load(open('build-equity/$out-run/receipt.json'))
print('outcome', r.get('outcome'), 'exit', r.get('exit_code'), 'wall_s', round(r.get('wall_seconds') or -1, 1),
      'peak_MiB', round((r.get('sampled_peak_tree_rss_bytes') or 0)/2**20), 'exe', (r.get('executable_sha256') or '')[:16],
      'source', (r.get('source_sha') or '')[:8], 'build_type', r.get('build_type'))
" 2>&1)
echo "$out runner-exit $rc shell-wall ${wall}s exe-before ${sha_before:0:16} :: $summary" | tee -a "$logs/step4-runs.txt"
# The argv differs from the P9-B0 reference's in exactly three tokens (printed by token index).
"$py" - "$out" <<'EOF' | tee -a "$logs/step4-runs.txt"
import json, sys
out = sys.argv[1]
a = json.load(open("build-equity/p9-b0-y1-nav-run/receipt.json"))["command"][1:]
b = json.load(open(f"build-equity/{out}-run/receipt.json"))["command"][1:]
diff = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
print(f"argv after the exe: reference {len(a)} tokens, this run {len(b)}; differing tokens {len(diff)}: {diff}")
EOF
"$py" .superpowers/sdd/platform-p9-20261003/m2g-logs/tools/nav_cmp.py build-equity/p9-b0-y1-nav "build-equity/$out" \
  --allow summary.json:/producer/build_type --allow summary.json:/producer/engine_git_sha \
  | tee "$logs/step4-cmp-$out.txt"
