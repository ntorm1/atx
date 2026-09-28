#!/usr/bin/env bash
# V6-U (v4-prereg.md "## v6 revision"): universe linked-operating-v1 on library v6 with the accepted v6 construction.
# restrict (role) -> gate -> fields (fields-v6 recipe on the restricted role) -> u -> fit (ew-theme-v1) -> w -> nav.
# TRAIN 2020-2022 only. Command lines: studies/v6_w.env.example (V6-W lane) adapted to library v6 (task-V6L-report.md §6)
# and the v6 construction knobs of v6l_train.sh. Every input pinned; outputs never overwritten.
#   bash v6u_train.sh restrict|gate|fields|u|fit|w|nav|all
set -uo pipefail
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
BRIDGE=build-equity/identity-bridge-r4-v1;  BRIDGE_S=ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa
EVENTS=build-equity/fundamental-events-v2;  EVENTS_S=74ed9a50ea686e0b0842ff9b09e78d6653ddeedd0d42f37893873ce269e3dd71
R2LO_DIR=build-equity/recent-fast-train-2020-2022-v2-lo1
FDLO=build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6
CC=build-equity/mega-candidate-cache-v6u
U=build-equity/mega-v6u-train-u
WORK=build-equity/mega-fit-work-v6u
W=build-equity/mega-weights-v6u-ew
WT=build-equity/mega-v6uw-train-ew
# accepted v6 construction (ledger ~23:40): theta .05 dust .1 fixed, delta orders, exit rate .05, locate-in-aim, cache
THETA=${THETA:-.05}; DUST=${DUST:-.1}; LEV=${LEV:-1}; NEUT=${NEUT:-price-risk-v1}; DSR_N=${DSR_N:-27}
N=build-equity/mega-nav-v6u-ew-t$THETA-d$DUST-fixed-obdelta-x.05-loc; [ "$LEV" != 1 ] && N="$N-L$LEV"
REFN=${REFN:-build-equity/mega-nav-v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc}
XF="--order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache"
sha() { sha256sum "$1" | cut -c1-64; }
pin() { [ "$(sha "$1")" = "$2" ] || { echo "PIN MISMATCH $1"; exit 3; }; }
pin $L $LS; pin $LR $LRS; pin $R2 $R2S; pin $FD/manifest.json $FS; pin $BRIDGE/manifest.json $BRIDGE_S; pin $EVENTS/manifest.json $EVENTS_S
R2LO=$R2LO_DIR/manifest.json
phase=${1:-all}
do_restrict() {
  [ -f $R2LO ] && { echo "restricted role exists: $R2LO_DIR"; return 0; }
  echo "== restrict role (linked-operating-v1)"
  "$PY" $BR --output $R2LO_DIR-run --bind $R2 --bind $BRIDGE/manifest.json --bind $EVENTS/manifest.json -- "$PY" \
    atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1 \
    --base-role build-equity/recent-fast-train-2020-2022-v2 --base-role-sha256 $R2S \
    --identity-bridge $BRIDGE --identity-bridge-sha256 $BRIDGE_S --sic-events $EVENTS --sic-events-sha256 $EVENTS_S \
    --check-fields $FD --check-fields-sha256 $FS --out $R2LO_DIR --memory-mib 1024 --max-seconds 170 | grep -E '"exit_code"' | head -1
  [ -f $R2LO ] || { echo "restrict failed"; tail -c 600 $R2LO_DIR-run/stderr.log; return 4; }
}
do_gate() {
  "$PY" -c "import json;u=json.load(open('$R2LO'))['universe'];k=u['kept_member_counts'][399:];print('min kept (score window)',min(k));print('dropped share',u['dropped_member_share_score_window']);print(u['dropped_by_reason_score_window'])"
}
do_fields() {
  [ -f $FDLO/manifest.json ] && { echo "fields exist: $FDLO"; return 0; }
  echo "== fields-v6 recipe on the restricted role (not the bounded runner: builder caps 700 MiB / 1800 s)"
  R2LOS=$(sha $R2LO)
  "$PY" atx-engine/tools/prepare_research_fields.py --role $R2LO_DIR --role-sha256 $R2LOS --output $FDLO \
    --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret,be,at,at_lag4,lt,che,debt,sale_ttm,gp_ttm,oi_ttm,ni_ttm,ni_q,ni_q_lag4,be_lag1q,be_lag1q_lag4,cfo_ttm,capx_ttm,xrd_ttm,dvc_ttm,prstkc_ttm,sstk_ttm,txt_q,txt_q_lag4,shrs_q,shrs_q_lag4,noa,noa_lag4,sue,fscore,me_company,grp_sic2,grp_ff12 \
    --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet \
    --identity-bridge $BRIDGE --identity-bridge-sha256 $BRIDGE_S --fund-events $EVENTS --fund-events-sha256 $EVENTS_S \
    --fund-lag-sessions 1 --max-rss-mib 700 --max-seconds 1800 2>&1 | tail -3
  [ -f $FDLO/manifest.json ] || { echo "fields failed"; return 4; }
}
do_u() {
  R2LOS=$(sha $R2LO); FSLO=$(sha $FDLO/manifest.json)
  for i in ${UPASS:-1 2 3 4}; do
    [ -f $U-$i/summary.json ] && grep -q '"status": *"complete"' $U-$i/summary.json && { echo $i > $U.final; echo "u complete: $U-$i"; return 0; }
    echo "== u pass $i"
    "$PY" $BR --output $U-run$i --bind $IC --bind $L --bind $R2LO --bind $FDLO/manifest.json -- $IC --library $L \
      --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS --train-fields $FDLO --train-fields-sha256 $FSLO \
      --output $U-$i --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC | grep -E '"exit_code"' | head -1
    [ -f $U-$i/summary.json ] && grep -q '"status": *"complete"' $U-$i/summary.json && { echo $i > $U.final; return 0; }
    tail -c 300 $U-run$i/stderr.log; echo
  done
  echo "u did not complete"; return 4
}
do_fit() {
  [ -f $W/composition_weights.json ] && { echo "fit output exists: $W"; return 0; }
  R2LOS=$(sha $R2LO); i=$(cat $U.final); O=$U-$i/orientations.json; OS=$(sha $O); S=$U-$i/summary.json; SS=$(sha $S)
  for j in 1 2 3; do
    echo "== fit ew-theme-v1 pass $j"
    "$PY" $BR --output $W-run$j --bind $L --bind $LR --bind $R2LO --bind $O --bind $S -- "$PY" \
      atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS \
      --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
      --orientation prior --screen v4-prior-v1 --composition ew-theme-v1 --recipe $LR --recipe-sha256 $LRS \
      --work-dir $WORK --max-seconds 150 --output $W | grep -E '"exit_code"' | head -1
    [ -f $W/composition_weights.json ] && return 0
  done
  echo "fit did not finish"; return 4
}
do_w() {
  R2LOS=$(sha $R2LO); FSLO=$(sha $FDLO/manifest.json); WS=$(sha $W/composition_weights.json)
  for i in ${WPASS:-1 2 3}; do
    [ -f $WT-$i/summary.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json && { echo $i > $WT.final; echo "w complete: $WT-$i"; return 0; }
    echo "== weighted pass $i"
    "$PY" $BR --output $WT-run$i --bind $IC --bind $R2LO --bind $FDLO/manifest.json --bind $L --bind $W/composition_weights.json -- \
      $IC --library $L --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS --train-fields $FDLO --train-fields-sha256 $FSLO \
      --output $WT-$i --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC \
      --composition-weights $W/composition_weights.json --composition-weights-sha256 $WS | grep -E '"exit_code"' | head -1
    [ -f $WT-$i/summary.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json && { echo $i > $WT.final; return 0; }
    tail -c 300 $WT-run$i/stderr.log; echo
  done
  echo "w did not finish"; return 4
}
do_nav() {
  R2LOS=$(sha $R2LO); FSLO=$(sha $FDLO/manifest.json); i=$(cat $WT.final); C=$WT-$i/train_combined.json; CS=$(sha $C)
  [ -d $N ] && { echo "nav output exists: $N"; exit 5; }
  echo "== nav $N (flags: $XF)"
  "$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FDLO/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
    --role $R2LO --role-sha256 $R2LOS --fields $FDLO/manifest.json --fields-sha256 $FSLO --output $N --rule aim-partial-v5 \
    --cadence 1 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV --daily-turnover-mean-max .20 \
    --daily-turnover-p95-max .30 --neutralize $NEUT --max-bytes 1073741824 $XF | grep -E '"exit_code"' | head -1
  [ -f $N/summary.json ] || { echo "nav failed"; tail -c 400 $N-run/stderr.log; exit 6; }
  "$PY" $STUDIES/nav_summ.py --weights $W/composition_weights.json --reference $REFN --dsr-n $DSR_N $N
}
case "$phase" in
  restrict) do_restrict ;; gate) do_gate ;; fields) do_fields ;; u) do_u ;; fit) do_fit ;; w) do_w ;; nav) do_nav ;;
  all) do_restrict && do_gate && do_fields && do_u && do_fit && do_w && do_nav ;;
  *) echo "phase restrict|gate|fields|u|fit|w|nav|all"; exit 2 ;;
esac
