#!/usr/bin/env bash
# THE single validation run of FREEZE v3-daily-2026-09-27 (ledger 462e0b33). Do not rerun with changes.
set -u
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
L=atx-impl/strategies/pv_fields_ic121_v3.json
LS=5d164ea115c633677dae59de975c24c4882ee05430d03a7e1bafa6bc591cb9f8
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
RV=build-equity/recent-fast-validation-2023-2024-v1/manifest.json
RVS=0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7
VF=build-equity/recent-fast-validation-2023-2024-v1-fields-v4
VFS=2691dcc55e60918a71563ff9eb9e3c009e1d60f54fc52041cbcb62840eea9073
OR=build-equity/mega-v3f4-warm-3/orientations.json
ORS=33bc0f63e16869ee11716cc2e47c10b6be4638731ee74d667c7b37926d766c22
W=build-equity/mega-weights-v3-plain/composition_weights.json
WS=db0a8b9a1b70d6ba81f7af6d1e1a7f8774f3a98ac6e1ecf2566a1610ce764aac
RO=build-equity/mega-v3-VAL
for i in 1 2 3 4; do
  "$PY" scripts/run_bounded_research.py --output $RO-run$i --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
    --bind build-equity/bin/atx-equity-strategy-ic.exe --bind $L --bind $R2 --bind $RV --bind $VF/manifest.json --bind $OR --bind $W -- \
    build-equity/bin/atx-equity-strategy-ic.exe --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
    --validation $RV --validation-sha256 $RVS --orientations $OR --orientations-sha256 $ORS \
    --composition-weights $W --composition-weights-sha256 $WS --validation-fields $VF --validation-fields-sha256 $VFS \
    --candidate-cache build-equity/mega-candidate-cache --max-memory-mib 1536 --min-names 2000 --workers 4 --save-combined \
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
O=build-equity/mega-nav-v3-VAL
"$PY" scripts/run_bounded_research.py --output $O-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --bind build-equity/bin/atx-equity-strategy-targets.exe --bind $C --bind $VF/manifest.json -- \
  build-equity/bin/atx-equity-strategy-targets.exe nav --combined $C --combined-sha256 $CS --role $RV --role-sha256 $RVS \
  --fields $VF/manifest.json --fields-sha256 $VFS --output $O --rule baseline-v1 --cadence 1 --trade-fraction 1 \
  --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --band-multiple 1 --max-bytes 1073741824 >/dev/null 2>&1
echo "== VAL NAV: $("$PY" -c "import json;r=json.load(open('$O-run/receipt.json'));print(r['outcome'],r['exit_code'],round(r['wall_seconds']),'s',r['sampled_peak_tree_rss_bytes']//2**20,'MiB')")"
tail -c 300 $O-run/stderr.log
