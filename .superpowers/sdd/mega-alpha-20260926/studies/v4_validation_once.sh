#!/usr/bin/env bash
# VALIDATION TRIAL #2: FREEZE v4.1-daily-2026-09-27 (see ledger). Single run. Do not rerun with changes.
set -u
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
L=atx-impl/strategies/fund_industry_ic_v4.json
LS=daa9663e43119102fbb42aba3be9b57961cd34925920c50baee7f340f0e1eddf
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
RV=build-equity/recent-fast-validation-2023-2024-v1/manifest.json
RVS=0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7
VF=build-equity/recent-fast-validation-2023-2024-v1-fields-v6
VFS=c034ecf3d91e2351ffa23103344018b8add9ccf43d0f04f9c1a5217be34ded5c
OR=build-equity/mega-v4-train-u-1/orientations.json
ORS=11cfd3e44b48d002030a597fb54dc4af54f16f986e136ea3918a598633c3bac3
W=build-equity/mega-weights-v4/composition_weights.json
WS=9a9c949a5f445bb71e2c3af0c6943efcf2c44693bb5cff25b53864243089cf65
RO=build-equity/mega-v4-VAL
for i in 1 2 3 4; do
  "$PY" scripts/run_bounded_research.py --output $RO-run$i --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
    --bind build-equity/bin/atx-equity-strategy-ic.exe --bind $L --bind $R2 --bind $RV --bind $VF/manifest.json --bind $OR --bind $W -- \
    build-equity/bin/atx-equity-strategy-ic.exe --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
    --validation $RV --validation-sha256 $RVS --orientations $OR --orientations-sha256 $ORS \
    --composition-weights $W --composition-weights-sha256 $WS --validation-fields $VF --validation-fields-sha256 $VFS \
    --candidate-cache build-equity/mega-candidate-cache --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined \
    --output $RO-$i >/dev/null 2>&1
  X=$("$PY" -c "import json;r=json.load(open('$RO-run$i/receipt.json'));print(r['exit_code'],r['outcome'],round(r['wall_seconds']),r['sampled_peak_tree_rss_bytes']//2**20)")
  echo "runner VAL run$i: $X hits=$(grep -c 'cache=hit' $RO-run$i/stdout.log) miss=$(grep -c 'cache=miss' $RO-run$i/stdout.log)"
  tail -c 200 $RO-run$i/stderr.log | tail -1
  if [ "${X%% *}" = "0" ]; then break; fi
done
ls $RO-$i
C=$(ls $RO-$i/validation_combined.json 2>/dev/null || ls $RO-$i/*combined.json | head -1)
CS=$(sha256sum $C | cut -c1-64)
echo "VAL combined $C $CS"
O=build-equity/mega-nav-v4-VAL-b2-f.25
"$PY" scripts/run_bounded_research.py --output $O-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --bind build-equity/bin/atx-equity-strategy-targets.exe --bind $C --bind $VF/manifest.json -- \
  build-equity/bin/atx-equity-strategy-targets.exe nav --combined $C --combined-sha256 $CS --role $RV --role-sha256 $RVS \
  --fields $VF/manifest.json --fields-sha256 $VFS --output $O --rule baseline-v1 --cadence 1 --trade-fraction .25 \
  --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --band-multiple 2 --max-bytes 1073741824 >/dev/null 2>&1
echo "== VAL NAV: $("$PY" -c "import json;r=json.load(open('$O-run/receipt.json'));print(r['outcome'],r['exit_code'],round(r['wall_seconds']),'s',r['sampled_peak_tree_rss_bytes']//2**20,'MiB')")"
tail -c 300 $O-run/stderr.log
