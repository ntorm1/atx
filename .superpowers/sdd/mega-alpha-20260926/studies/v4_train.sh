#!/usr/bin/env bash
# v4 TRAIN pipeline (prereg v4-prereg.md): unweighted IC pass(es) -> fitter v4-prior-v1/ew-theme-v1 ->
# pinned weighted TRAIN runner -> ONE NAV construction (band 1). TRAIN only. Usage: v4_train.sh [phase...]
set -u
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe
NAV=build-equity/bin/atx-equity-strategy-targets.exe
L=atx-impl/strategies/fund_industry_ic_v4.json; LS=$(sha256sum $L | cut -c1-64)
LR=atx-impl/strategies/fund_industry_ic_v4.recipe.json; LRS=$(sha256sum $LR | cut -c1-64)
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6
FS=$(sha256sum $FD/manifest.json | cut -c1-64)
CC=build-equity/mega-candidate-cache
U=build-equity/mega-v4-train-u     # unweighted TRAIN artifact prefix
W=build-equity/mega-weights-v4
WT=build-equity/mega-v4w-train     # pinned weighted TRAIN artifact prefix
N=build-equity/mega-nav-v4-train-b1
rc() { "$PY" -c "import json;r=json.load(open('$1/receipt.json'));print(r['exit_code'],r['outcome'],round(r['wall_seconds']),r['sampled_peak_tree_rss_bytes']//2**20)"; }
echo "library $LS recipe $LRS fields $FS"
for phase in "${@:-u fit w nav}"; do
 for ph in $phase; do
  if [ "$ph" = u ]; then
    for i in 1 2 3 4 5 6; do
      "$PY" $BR --output $U-run$i --bind $IC --bind $L --bind $R2 --bind $FD/manifest.json -- $IC --library $L --library-sha256 $LS \
        --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $U-$i --max-memory-mib 1536 \
        --min-names 1000 --workers 4 --save-combined --candidate-cache $CC >/dev/null 2>&1
      X=$(rc $U-run$i); echo "u pass $i: $X hits=$(grep -c 'cache=hit' $U-run$i/stdout.log) miss=$(grep -c 'cache=miss' $U-run$i/stdout.log)"
      tail -c 200 $U-run$i/stderr.log | tail -1
      if [ "${X%% *}" = "0" ]; then echo "$i" > $U.final; break; fi
    done
  elif [ "$ph" = fit ]; then
    i=$(cat $U.final); O=$U-$i/orientations.json; S=$U-$i/summary.json
    for j in 1 2 3 4; do
      "$PY" $BR --output $W-run$j -- "$PY" atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS \
        --train $R2 --train-sha256 $R2S --orientations $O --orientations-sha256 $(sha256sum $O | cut -c1-64) \
        --runner-summary $S --runner-summary-sha256 $(sha256sum $S | cut -c1-64) --orientation prior --screen v4-prior-v1 \
        --composition ew-theme-v1 --recipe $LR --recipe-sha256 $LRS --output $W >/dev/null 2>&1
      X=$(rc $W-run$j); echo "fit run $j: $X"; tail -c 300 $W-run$j/stderr.log | tail -2
      if [ "${X%% *}" = "0" ]; then break; fi
    done
  elif [ "$ph" = w ]; then
    WF=$W/composition_weights.json; WS=$(sha256sum $WF | cut -c1-64)
    for i in 1 2 3; do
      "$PY" $BR --output $WT-run$i --bind $IC --bind $R2 --bind $FD/manifest.json --bind $L --bind $WF -- $IC --library $L \
        --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $WT-$i \
        --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC --composition-weights $WF \
        --composition-weights-sha256 $WS >/dev/null 2>&1
      X=$(rc $WT-run$i); echo "weighted pass $i: $X"; tail -c 200 $WT-run$i/stderr.log | tail -1
      if [ "${X%% *}" = "0" ]; then echo "$i" > $WT.final; break; fi
    done
  elif [ "$ph" = nav ]; then
    C=$WT-$(cat $WT.final)/train_combined.json; CS=$(sha256sum $C | cut -c1-64); echo "combined $C $CS"
    "$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
      --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule baseline-v1 --cadence 1 \
      --trade-fraction 1 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --band-multiple 1 \
      --max-bytes 1073741824 >/dev/null 2>&1
    echo "nav: $(rc $N-run)"; tail -c 300 $N-run/stderr.log
    "$PY" .superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py $N 2>&1 | grep -v financing | cut -c1-300
  fi
 done
done
