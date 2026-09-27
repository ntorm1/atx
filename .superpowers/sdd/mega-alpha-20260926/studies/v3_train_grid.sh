#!/usr/bin/env bash
# Root pipeline: weighted TRAIN runner (pinned v3 weights) -> NAV band grid with swap-fin-v1 (TRAIN only).
set -u
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v4
FS=389e7fb4993ca577acca6b22101a76b9f86ec903340627f410293c73060b4bd8
L=atx-impl/strategies/pv_fields_ic121_v3.json
LS=5d164ea115c633677dae59de975c24c4882ee05430d03a7e1bafa6bc591cb9f8
SUMM=.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py
for comp in plain netcost; do
  W=build-equity/mega-weights-v3-$comp/composition_weights.json
  WS=$(sha256sum $W | cut -c1-64)
  RO=build-equity/mega-v3w-$comp-train
  for i in 1 2; do
    "$PY" scripts/run_bounded_research.py --output $RO-run$i --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
      --bind build-equity/bin/atx-equity-strategy-ic.exe --bind $R2 --bind $FD/manifest.json --bind $L --bind $W -- \
      build-equity/bin/atx-equity-strategy-ic.exe --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
      --train-fields $FD --train-fields-sha256 $FS --output $RO-$i --max-memory-mib 1536 --min-names 2000 --workers 4 \
      --save-combined --candidate-cache build-equity/mega-candidate-cache --composition-weights $W --composition-weights-sha256 $WS >/dev/null 2>&1
    X=$("$PY" -c "import json;r=json.load(open('$RO-run$i/receipt.json'));print(r['exit_code'],round(r['wall_seconds']),r['sampled_peak_tree_rss_bytes']//2**20)")
    echo "runner $comp run$i: $X"
    if [ "${X%% *}" = "0" ]; then break; fi
  done
  C=$RO-$i/train_combined.json
  [ -f "$C" ] || { echo "no combined for $comp"; continue; }
  CS=$(sha256sum $C | cut -c1-64)
  echo "combined $comp $C $CS"
  for b in 0 0.5 1 2; do
    O=build-equity/mega-nav-v3-$comp-b$b
    "$PY" scripts/run_bounded_research.py --output $O-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
      --bind build-equity/bin/atx-equity-strategy-targets.exe --bind $C --bind $FD/manifest.json -- \
      build-equity/bin/atx-equity-strategy-targets.exe nav --combined $C --combined-sha256 $CS --role $R2 --role-sha256 $R2S \
      --fields $FD/manifest.json --fields-sha256 $FS --output $O --rule baseline-v1 --cadence 1 --trade-fraction 1 \
      --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --band-multiple $b --max-bytes 1073741824 >/dev/null 2>&1
    echo "== nav $comp band $b: $("$PY" -c "import json;r=json.load(open('$O-run/receipt.json'));print(r['outcome'],r['exit_code'],round(r['wall_seconds']),'s')")"
    "$PY" $SUMM $O 2>&1 | grep -E 'swap-fin|flat-300|engine-tiers' | grep -v financing | cut -c1-260
  done
done
