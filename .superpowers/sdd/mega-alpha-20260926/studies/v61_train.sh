#!/usr/bin/env bash
# V6.1 sub-alpha (v4-prereg.md "## v6.1 sub-alpha"): FINRA long-horizon shorting flow, library v6.1 = v6 + sv_flow.
# fields (fields-v6b list + sv_ratio126 on the restricted role lo1) -> u (library v6.1, own cache) -> fit (ew-theme-v1)
# -> p1 (sv_flow admission read-out; exit 10 = P1 failed, stop) -> w -> nav (final v6 construction, L FIXED 1.247)
# -> summ (paired vs the v6 final cell, DSR N 29). TRAIN 2020-2022 only. Derived from v6u_train.sh: same conventions,
# every input pinned, outputs never overwritten.
#   bash v61_train.sh fields|u|fit|p1|w|nav|summ|all
set -uo pipefail
cd C:/atx-wt/pool-2 || exit 1
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe
NAV=build-equity/bin/atx-equity-strategy-targets.exe
STUDIES=.superpowers/sdd/mega-alpha-20260926/studies
L=atx-impl/strategies/fund_industry_ic_v61.json
LS=db35c2769f6c13a8d9d5b2897a9e6968855bffbe6214ff96a12f6869cb59f9b5
LR=atx-impl/strategies/fund_industry_ic_v61.recipe.json
LRS=9bf278a6f3dfa78edd506ab3e499b2e8dd1ce34899752fe81df573e65550f547
L6=atx-impl/strategies/fund_industry_ic_v6.json
L6S=5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a
R2LO_DIR=build-equity/recent-fast-train-2020-2022-v2-lo1
R2LO=$R2LO_DIR/manifest.json
R2LOS=3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809
FD6=build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6b   # the v6u fields: its 40 fields must reappear byte-identical
FD6S=c69b9c0faf76e333f69b480a698e57b1309f2882b74a40092d9ebb91b9fa82c8
FD7=build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7
BRIDGE=build-equity/identity-bridge-r4-v1;  BRIDGE_S=ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa
EVENTS=build-equity/fundamental-events-v2;  EVENTS_S=74ed9a50ea686e0b0842ff9b09e78d6653ddeedd0d42f37893873ce269e3dd71
TH=C:/Users/natha/Downloads/TickerHistory3.parquet   # pinned by the builder against the role's source_sha256 (0ed96b26)
SVDIR=C:/atx/atx-db/data/raw/finra_short_volume       # read-only; files read are pinned below via the fields manifest
SV_FILES_SHA=6a968e5aafd820e1e19c014c0f17f4adb6dd55d2b9c395299ad2c5460593d081   # 1112 files 2018-08-01..2022-12-29
CC=build-equity/mega-candidate-cache-v61
U=build-equity/mega-v61-train-u
WORK=build-equity/mega-fit-work-v61
W=build-equity/mega-weights-v61-ew
WT=build-equity/mega-v61w-train-ew
LEV=1.247   # FIXED (prereg v6.1 P2): the v6 final cell's leverage, no re-derivation
N=build-equity/mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L$LEV
REFN=build-equity/mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247
REF_ADM=build-equity/mega-weights-v6u-ew/admission.json
DSR_N=29
XF="--order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache"
FIELDS=si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret,be,at,at_lag4,lt,che,debt,sale_ttm,gp_ttm,oi_ttm,ni_ttm,ni_q,ni_q_lag4,be_lag1q,be_lag1q_lag4,cfo_ttm,capx_ttm,xrd_ttm,dvc_ttm,prstkc_ttm,sstk_ttm,txt_q,txt_q_lag4,shrs_q,shrs_q_lag4,noa,noa_lag4,sue,fscore,me_company,grp_sic2,grp_ff12,grp_ff49,sv_ratio126
sha() { sha256sum "$1" | cut -c1-64; }
pin() { [ "$(sha "$1")" = "$2" ] || { echo "PIN MISMATCH $1"; exit 3; }; }
pin $L $LS; pin $LR $LRS; pin $L6 $L6S; pin $R2LO $R2LOS; pin $FD6/manifest.json $FD6S
pin $BRIDGE/manifest.json $BRIDGE_S; pin $EVENTS/manifest.json $EVENTS_S
phase=${1:-all}
do_fields() {
  if [ ! -f $FD7/manifest.json ]; then
    [ -d $FD7 ] && { echo "partial fields dir exists (never overwritten): $FD7"; return 4; }
    echo "== fields-v7 = fields-v6b list + sv_ratio126 on the restricted role (builder, not the bounded runner)"
    "$PY" atx-engine/tools/prepare_research_fields.py --role $R2LO_DIR --role-sha256 $R2LOS --output $FD7 \
      --fields $FIELDS --finra C:/atx/data/finra_short_interest --tickerhistory $TH --finra-short-volume $SVDIR \
      --identity-bridge $BRIDGE --identity-bridge-sha256 $BRIDGE_S --fund-events $EVENTS --fund-events-sha256 $EVENTS_S \
      --fund-lag-sessions 1 --max-rss-mib 1536 --max-seconds 1800 2>&1 | tail -3
    [ -f $FD7/manifest.json ] || { echo "fields failed"; return 4; }
  fi
  echo "fields-v7 manifest $(sha $FD7/manifest.json)"
  # the 40 v6b fields must be byte-identical; the short-volume files read must be the pinned set
  "$PY" - $FD6/manifest.json $FD7/manifest.json $SV_FILES_SHA <<'EOF'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
names = [f["name"] for f in a["fields"]]
bad = [n for n in names if a["files"][n + ".f64"] != b["files"].get(n + ".f64")]
added = sorted({f["name"] for f in b["fields"]} - set(names))
print(f"fields-v7 vs fields-v6b: {len(names) - len(bad)} of {len(names)} identical {bad or ''}; added {added}")
sv = next(f for f in b["fields"] if f["name"] == "sv_ratio126")
files = sv["short_volume"]["files"]
print(f"sv_ratio126: {sv['short_volume']['formula_id']}; files {files['files_read']} {files['first_file_date']}.."
      f"{files['last_file_date']} sha {files['files_sha256']}")
print("  finite member frac per year:", {y: v["finite_member_frac"] for y, v in sv["coverage"]["per_year"].items()},
      "score window", sv["coverage"]["score_window"]["finite_member_frac"])
sys.exit(1 if bad or added != ["sv_ratio126"] or files["files_sha256"] != sys.argv[3] else 0)
EOF
  [ $? -eq 0 ] || { echo "fields-v7 check FAILED"; return 4; }
}
do_u() {
  FS7=$(sha $FD7/manifest.json)
  "$PY" atx-impl/strategies/check_fund_ic_v6.py --manifest $FD7/manifest.json --library $L --baseline $L6 \
    --require-baseline-prefix --expect-added sv_flow | tail -1 || { echo "static check failed"; return 4; }
  for i in ${UPASS:-1 2 3 4 5 6}; do
    [ -f $U-$i/summary.json ] && grep -q '"status": *"complete"' $U-$i/summary.json && { echo $i > $U.final; echo "u complete: $U-$i"; return 0; }
    echo "== u pass $i"
    "$PY" $BR --output $U-run$i --bind $IC --bind $L --bind $R2LO --bind $FD7/manifest.json -- $IC --library $L \
      --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS --train-fields $FD7 --train-fields-sha256 $FS7 \
      --output $U-$i --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC | grep -E '"exit_code"' | head -1
    [ -f $U-$i/summary.json ] && grep -q '"status": *"complete"' $U-$i/summary.json && { echo $i > $U.final; return 0; }
    tail -c 300 $U-run$i/stderr.log; echo
  done
  echo "u did not complete"; return 4
}
do_fit() {
  [ -f $W/composition_weights.json ] && { echo "fit output exists: $W"; return 0; }
  i=$(cat $U.final); O=$U-$i/orientations.json; OS=$(sha $O); S=$U-$i/summary.json; SS=$(sha $S)
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
# P1 (prereg v6.1): sv_flow admitted by v4-prior-v1 on lo1 (not redundant: |rho| <= .90 vs every admitted member; tau
# within the screen limit; no veto) AND its runner sign agrees with the prior. Exit 10 = P1 failed: stop before w.
do_p1() {
  [ -f $W/admission.json ] || { echo "no admission: run fit first ($W)"; return 4; }
  "$PY" - $W/admission.json $REF_ADM <<'EOF'
import json, sys
adm = json.load(open(sys.argv[1]))
rows = {c["id"]: c for c in adm["candidates"]}
r = rows["sv_flow"]
fmt = lambda x: "None" if x is None else f"{x:.4f}" if isinstance(x, float) else str(x)
print(f"P1 sv_flow: status {r['status']} failed {r['failed_checks']}; runner sign {r['runner_sign']} vs prior "
      f"{r['s_k']} (agrees {r['sign_agrees']}); HAC t {fmt(r['hac_t'])}; tau {fmt(r['tau'])} (limit "
      f"{adm['rules']['tau_limit']}); max |rho| {fmt(r['max_abs_rho'])} with {r['max_abs_rho_with']}; redundant_with "
      f"{r['redundant_with']} {fmt(r['redundant_rho'])}; train days {r['train_days']}")
for k in ("si_ratio", "dtc", "si_change"):
    print(f"  {k}: status {rows[k]['status']}, max |rho| {fmt(rows[k]['max_abs_rho'])} with {rows[k]['max_abs_rho_with']}")
try:  # the 38 v6 members: same definitions and data, so any status change comes from sv_flow in the redundancy pass
    ref = {c["id"]: c["status"] for c in json.load(open(sys.argv[2]))["candidates"]}
    moved = {k: (ref[k], rows[k]["status"]) for k in ref if k in rows and rows[k]["status"] != ref[k]}
    print(f"  v6 members vs {sys.argv[2]}: {len(moved)} status changes {moved or ''}")
except OSError as e:
    print(f"  v6 reference admission unavailable: {e}")
ok = r["status"] == "admitted" and r["sign_agrees"] is True
print("P1 " + ("PASS" if ok else "FAIL: stop (no w / nav / summ)"))
sys.exit(0 if ok else 10)
EOF
}
do_w() {
  do_p1 > /dev/null || { echo "P1 not passed: w refused"; return 10; }
  FS7=$(sha $FD7/manifest.json); WS=$(sha $W/composition_weights.json)
  for i in ${WPASS:-1 2 3}; do
    [ -f $WT-$i/summary.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json && { echo $i > $WT.final; echo "w complete: $WT-$i"; return 0; }
    echo "== weighted pass $i"
    "$PY" $BR --output $WT-run$i --bind $IC --bind $R2LO --bind $FD7/manifest.json --bind $L --bind $W/composition_weights.json -- \
      $IC --library $L --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS --train-fields $FD7 --train-fields-sha256 $FS7 \
      --output $WT-$i --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC \
      --composition-weights $W/composition_weights.json --composition-weights-sha256 $WS | grep -E '"exit_code"' | head -1
    [ -f $WT-$i/summary.json ] && grep -q '"status": *"complete"' $WT-$i/summary.json && { echo $i > $WT.final; return 0; }
    tail -c 300 $WT-run$i/stderr.log; echo
  done
  echo "w did not finish"; return 4
}
do_nav() {
  FS7=$(sha $FD7/manifest.json); i=$(cat $WT.final); C=$WT-$i/train_combined.json; CS=$(sha $C)
  [ -d $N ] && { echo "nav output exists: $N"; return 0; }
  echo "== nav $N (L fixed $LEV; flags: $XF)"
  "$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FD7/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
    --role $R2LO --role-sha256 $R2LOS --fields $FD7/manifest.json --fields-sha256 $FS7 --output $N --rule aim-partial-v5 \
    --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage $LEV --daily-turnover-mean-max .20 \
    --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 $XF | grep -E '"exit_code"' | head -1
  [ -f $N/summary.json ] || { echo "nav failed"; tail -c 400 $N-run/stderr.log; return 6; }
}
do_summ() {
  [ -f $N/summary.json ] || { echo "no nav output: $N"; return 6; }
  [ -d $REFN ] || { echo "missing reference cell $REFN"; return 3; }
  "$PY" $STUDIES/nav_summ.py --weights $W/composition_weights.json --reference $REFN --dsr-n $DSR_N $N
}
case "$phase" in
  fields) do_fields ;; u) do_u ;; fit) do_fit ;; p1) do_p1 ;; w) do_w ;; nav) do_nav ;; summ) do_summ ;;
  all) do_fields && do_u && do_fit && do_p1 && do_w && do_nav && do_summ ;;
  *) echo "phase fields|u|fit|p1|w|nav|summ|all"; exit 2 ;;
esac
