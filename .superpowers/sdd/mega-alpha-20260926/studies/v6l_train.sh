#!/usr/bin/env bash
# Library v6 (V6-L, v4-prereg.md "## v6 revision"): fit (admission v4-prior-v1 + ONE composition) -> weighted TRAIN pass
# -> nav cell on the v5 reference construction (aim-partial-v5, theta .05, dust .1, fixed rate, price-risk-v1, L 1).
# TRAIN 2020-2022 only (role v2 210fff96, fields-v6 32565c32). The u pass already ran: build-equity/mega-v6l-train-u2-1
# (own candidate cache mega-candidate-cache-v6; ledger ruling 2026-09-27 ~20:45).
#   COMP=ew-theme-v1 bash v6l_train.sh fit|w|nav|all
#   COMP=ew-theme-v1 -> W=build-equity/mega-weights-v6l-ew, WT=mega-v6lw-train-ew, N=mega-nav-v6l-ew-t.05-d.1-fixed
#   COMP=ew-theme-v6 -> W=build-equity/mega-weights-v6l-ew6, WT=mega-v6lw-train-ew6, N=mega-nav-v6l-ew6-t.05-d.1-fixed
#     (needs the V6-W fitter and IC runner cherry-picked and built; refused otherwise by the fitter itself).
# Command lines are task-V6L-report.md section 6 (v51_train.sh conventions). Every input pinned; outputs never overwritten.
set -u
cd C:/atx-wt/pool-2 || exit 1
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe
NAV=build-equity/bin/atx-equity-strategy-targets.exe
STUDIES=.superpowers/sdd/mega-alpha-20260926/studies
L=atx-impl/strategies/fund_industry_ic_v6.json
LS=5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a
LR=atx-impl/strategies/fund_industry_ic_v6.recipe.json
LRS=36c084522237b756d2847bc3e589855fafe589bba55b5111642c444afecfc72b
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
CC=build-equity/mega-candidate-cache-v6
U=build-equity/mega-v6l-train-u2-1
WORK=build-equity/mega-fit-work-v6l
COMP=${COMP:-ew-theme-v1}
case "$COMP" in
  ew-theme-v1) TAG=ew ;;
  ew-theme-v6) TAG=ew6 ;;
  *) echo "COMP must be ew-theme-v1 or ew-theme-v6"; exit 2 ;;
esac
W=build-equity/mega-weights-v6l-$TAG
WT=build-equity/mega-v6lw-train-$TAG
N=build-equity/mega-nav-v6l-$TAG-t.05-d.1-fixed
REF=build-equity/mega-nav-v51-ew-t.05-d.1-fixed
sha() { sha256sum "$1" | cut -c1-64; }
pin() { [ "$(sha "$1")" = "$2" ] || { echo "PIN MISMATCH $1"; exit 3; }; }
pin $L $LS; pin $LR $LRS; pin $R2 $R2S; pin $FD/manifest.json $FS
[ -f $U/summary.json ] || { echo "missing u pass $U"; exit 3; }
grep -q '"status": *"complete"' $U/summary.json || { echo "u pass not complete"; exit 3; }
O=$U/orientations.json; OS=$(sha $O); S=$U/summary.json; SS=$(sha $S)
phase=${1:-all}
do_fit() {
  [ -d $W ] && { echo "fit output exists: $W"; return 0; }
  for j in 1 2 3; do
    echo "== fit $COMP pass $j"
    "$PY" $BR --output $W-run$j --bind $L --bind $LR --bind $R2 --bind $O --bind $S -- "$PY" \
      atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
      --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
      --orientation prior --screen v4-prior-v1 --composition "$COMP" --recipe $LR --recipe-sha256 $LRS \
      --work-dir $WORK --max-seconds 150 --output $W | grep -E '"exit_code"|"status"' | head -2
    [ -f $W/composition_weights.json ] && return 0
  done
  echo "fit did not finish in 3 passes"; return 4
}
WMEM=1536; [ "$COMP" = ew-theme-v6 ] && WMEM=2304  # IC runner admit ESTIMATE for theme planes (real RSS ~1.2 GiB); runner RSS cap stays 1536
do_w() {
  WS=$(sha $W/composition_weights.json)
  for i in ${WPASS:-1 2 3}; do
    [ -f $WT-$i/train_combined.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json 2>/dev/null && { echo "weighted pass exists: $WT-$i"; echo $i > $WT.final; return 0; }
    echo "== weighted pass $i"
    "$PY" $BR --output $WT-run$i --bind $IC --bind $R2 --bind $FD/manifest.json --bind $L --bind $W/composition_weights.json -- \
      $IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS \
      --output $WT-$i --max-memory-mib $WMEM --min-names 1000 --workers 4 --save-combined --candidate-cache $CC \
      --composition-weights $W/composition_weights.json --composition-weights-sha256 $WS | grep -E '"exit_code"|"status"' | head -2
    if [ -f $WT-$i/summary.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json; then echo $i > $WT.final; return 0; fi
  done
  echo "weighted pass did not finish in 3 passes"; return 4
}
# Construction knobs (V6-C grid, ledger ruling ~21:15: parent = the v6l ew cell). Defaults reproduce the parent cell name.
#   ORDER_BASIS=target|delta (C1)  EXIT_RATE=1|.05|.1 (C2; < 1 needs DUST > 0)  LOCATE_AIM=0|1 (C3)  LCACHE=1|0 (F8)
#   THETA=.05 DUST=.1 LEV=1 NEUT=price-risk-v1 (C5: price-risk-ind-v1 / -v2 once built)  DSR_N=<N> REFN=<paired reference dir>
ORDER_BASIS=${ORDER_BASIS:-target}; EXIT_RATE=${EXIT_RATE:-1}; LOCATE_AIM=${LOCATE_AIM:-0}; LCACHE=${LCACHE:-1}
THETA=${THETA:-.05}; DUST=${DUST:-.1}; LEV=${LEV:-1}; NEUT=${NEUT:-price-risk-v1}; DSR_N=${DSR_N:-14}
SUF=""
[ "$THETA$DUST" != ".05.1" ] && N=build-equity/mega-nav-v6l-$TAG-t$THETA-d$DUST-fixed
[ "$ORDER_BASIS" != target ] && SUF="$SUF-ob$ORDER_BASIS"
[ "$EXIT_RATE" != 1 ] && SUF="$SUF-x$EXIT_RATE"
[ "$LOCATE_AIM" = 1 ] && SUF="$SUF-loc"
[ "$NEUT" != price-risk-v1 ] && SUF="$SUF-n${NEUT#price-risk-}"
[ "$LEV" != 1 ] && SUF="$SUF-L$LEV"
N="$N$SUF${NSUF:-}"   # NSUF: identity re-run suffix (never overwrite)
REFN=${REFN:-build-equity/mega-nav-v6l-ew-t.05-d.1-fixed}
XF=""
[ "$ORDER_BASIS" != target ] && XF="$XF --order-basis $ORDER_BASIS"
[ "$EXIT_RATE" != 1 ] && XF="$XF --exit-rate $EXIT_RATE"
[ "$LOCATE_AIM" = 1 ] && XF="$XF --locate-in-aim"
[ "$LCACHE" = 1 ] && XF="$XF --liquidity-cache"
do_nav() {
  i=$(cat $WT.final); C=$WT-$i/train_combined.json; CS=$(sha $C)
  [ -d $N ] && { echo "nav output exists: $N"; exit 5; }
  echo "== nav $N (flags:$XF)"
  "$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
    --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 \
    --cadence 1 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV --daily-turnover-mean-max .20 \
    --daily-turnover-p95-max .30 --neutralize $NEUT --max-bytes 1073741824 $XF | grep -E '"exit_code"|"status"' | head -2
  [ -f $N/summary.json ] || { echo "nav failed"; exit 6; }
  [ -d "$REFN" ] && [ "$REFN" != "$N" ] && REF=$REFN
  "$PY" $STUDIES/nav_summ.py --weights $W/composition_weights.json --reference $REF --dsr-n $DSR_N $N
}
case "$phase" in
  fit) do_fit ;;
  w) do_w ;;
  nav) do_nav ;;
  all) do_fit && do_w && do_nav ;;
  *) echo "phase fit|w|nav|all"; exit 2 ;;
esac
