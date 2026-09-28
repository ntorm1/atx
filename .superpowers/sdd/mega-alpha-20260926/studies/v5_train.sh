#!/usr/bin/env bash
# v5 TRAIN pipeline (v4-prereg.md "## v5 revision" R4'/R5'): fitter ew-theme-aim-v1 (or the ew-theme-v1 re-fit check)
# -> pinned weighted TRAIN IC pass -> ONE aim-partial-v5 NAV cell per invocation. TRAIN only; nothing >= 2023 is read.
# R1'-R3' are unchanged, so the v4 unweighted TRAIN IC pass (mega-v4-train-u-1) is reused as is (no u phase).
#
#   COMP=ew-theme-aim-v1 bash v5_train.sh fit   -> build-equity/mega-weights-v5-aim (<= 3 bounded passes, resumable)
#   COMP=ew-theme-v1     bash v5_train.sh fit   -> scratch build-equity/mega-weights-v5-ew-refit + equality check
#                                                  against 9a9c949a modulo the fitter-SHA-derived fields (delete after)
#   bash v5_train.sh w                          -> weighted TRAIN IC pass with W_aim: build-equity/mega-v5w-train-aim-<i>
#   COMBINED=ew|aim THETA=.05 DUST=.1 RATE=fixed|per-name [LEV=1] bash v5_train.sh nav
#                                               -> build-equity/mega-nav-v5-$COMBINED-t$THETA-d$DUST-$RATE[-L$LEV]
# Every input is pinned by SHA-256 (checked here, and the tools refuse unpinned inputs). Never --band-multiple.
set -u
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe
NAV=build-equity/bin/atx-equity-strategy-targets.exe
STUDIES=.superpowers/sdd/mega-alpha-20260926/studies
L=atx-impl/strategies/fund_industry_ic_v4.json          # R1': library daa9663e (37)
LS=daa9663e43119102fbb42aba3be9b57961cd34925920c50baee7f340f0e1eddf
LR=atx-impl/strategies/fund_industry_ic_v4.recipe.json
LRS=62b510f12a79cfacd2e74d64e020f20cee202cadb82153653b2e1065ec250c81
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6   # R2': fields-v6 32565c32 (TRAIN)
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
O=build-equity/mega-v4-train-u-1/orientations.json        # unweighted TRAIN IC pass (v4, reused)
OS=11cfd3e44b48d002030a597fb54dc4af54f16f986e136ea3918a598633c3bac3
S=build-equity/mega-v4-train-u-1/summary.json
SS=b32bbed0a2d7b7f64e89890966538fb145f46f3ab2508c9d733fb2bb0e02b796
W_EW=build-equity/mega-weights-v4/composition_weights.json   # reference composition ew-theme-v1 9a9c949a
W_EW_S=9a9c949a5f445bb71e2c3af0c6943efcf2c44693bb5cff25b53864243089cf65
A_EW=build-equity/mega-weights-v4/admission.json             # admission v4-prior-v1 880a0a6a (R3')
A_EW_S=880a0a6a9e6111b9bee4b2335fe36d9671070b4fefdee56586427e1e9a5667df
C_EW=build-equity/mega-v4w-train-1/train_combined.json       # C_ew: weighted TRAIN combined of 9a9c949a
C_EW_S=24a6cc76fc110a91b8e204dfde3c34d6a94f94dbe291159eb63a9994c1d030f3
CC=build-equity/mega-candidate-cache
WORK=build-equity/mega-fit-work-v5
WA=build-equity/mega-weights-v5-aim           # W_aim
WE=build-equity/mega-weights-v5-ew-refit      # scratch ew-theme-v1 re-fit (delete after the check)
WT=build-equity/mega-v5w-train-aim            # weighted TRAIN IC pass prefix (C_aim = $WT-<final>/train_combined.json)
REF=build-equity/mega-nav-v5-ew-t.05-d.1-fixed  # R5' reference cell (C_ew, theta .05, dust .1, fixed rate)
rc() { "$PY" -c "import json;r=json.load(open('$1/receipt.json'));print(r['exit_code'],r['outcome'],round(r['wall_seconds']),r['sampled_peak_tree_rss_bytes']//2**20)"; }
pin() { local got; got=$(sha256sum "$1" | cut -c1-64); [ "$got" = "$2" ] || { echo "PIN MISMATCH $1: $got != $2"; exit 1; }; }
for f in "$L $LS" "$LR $LRS" "$R2 $R2S" "$FD/manifest.json $FS" "$O $OS" "$S $SS"; do pin $f; done
echo "library $LS recipe $LRS train $R2S fields $FS orientations $OS summary $SS"
for phase in "${@:-fit}"; do
 for ph in $phase; do
  if [ "$ph" = fit ]; then
    case "${COMP:-}" in
      ew-theme-aim-v1) W=$WA ;;
      ew-theme-v1) W=$WE ;;
      *) echo "fit: set COMP=ew-theme-aim-v1 (W_aim) or COMP=ew-theme-v1 (scratch re-fit check)"; exit 2 ;;
    esac
    [ -e "$W" ] && { echo "fit: $W exists; refusing (the fitter never overwrites)"; exit 1; }
    for j in 1 2 3; do
      # --max-seconds 150 < the 180 s bound: a pass stops cleanly between candidates (exit 3, stdout
      # {"status":"incomplete","partial":true,...}); every finished candidate persists in $WORK, the next pass resumes.
      "$PY" $BR --output $W-run$j --bind $L --bind $LR --bind $R2 --bind $O --bind $S -- "$PY" \
        atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
        --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
        --orientation prior --screen v4-prior-v1 --composition "$COMP" --recipe $LR --recipe-sha256 $LRS \
        --work-dir $WORK --max-seconds 150 --output $W >/dev/null 2>&1
      X=$(rc $W-run$j); echo "fit pass $j ($COMP): $X"; tail -c 300 $W-run$j/stderr.log | tail -2; echo
      head -c 400 $W-run$j/stdout.log; echo
      case "$X" in
        "0 completed"*) break ;;
        "3 "*|*time-limit*) [ $j = 3 ] && { echo "fit: incomplete after 3 passes (report; do not raise the bound)"; exit 1; } ;;
        *) echo "fit: refused or failed (see $W-run$j/stderr.log); not retrying"; exit 1 ;;
      esac
    done
    echo "weights $W/composition_weights.json $(sha256sum $W/composition_weights.json | cut -c1-64)"
    echo "admission $W/admission.json $(sha256sum $W/admission.json | cut -c1-64)"
    if [ "$COMP" = ew-theme-v1 ]; then
      pin $W_EW $W_EW_S; pin $A_EW $A_EW_S
      # A literal SHA equality with 9a9c949a is impossible by construction: the weights file embeds the fitter's
      # own SHA-256 (script_sha256), the context digest bound to it, and the admission SHA whose admission.json
      # embeds both. Everything else must be byte-identical (the byte_stability contract of the fitter tests).
      "$PY" - "$W_EW" "$W/composition_weights.json" "$A_EW" "$W/admission.json" <<'EOF'
import json, sys
w_old, w_new, a_old, a_new = (open(p, "rb").read() for p in sys.argv[1:5])
po, pn = json.loads(w_old)["provenance"], json.loads(w_new)["provenance"]
ok = True
for name, old, new in (("composition_weights.json", w_old, w_new), ("admission.json", a_old, a_new)):
    for key in ("script_sha256", "context_sha256", "admission_sha256"):
        old = old.replace(po[key].encode(), pn[key].encode())
    same = old == new
    ok &= same
    print(f"EW-REFIT {name}: {'IDENTICAL' if same else 'DIFFERS'} modulo script/context/admission SHA "
          f"(old script {po['script_sha256'][:8]} -> new {pn['script_sha256'][:8]})")
sys.exit(0 if ok else 1)
EOF
      [ $? = 0 ] || { echo "EW-REFIT CHECK FAILED"; exit 1; }
    else
      "$PY" - "$W/composition_weights.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
p, a = d["provenance"], d["provenance"]["aim"]
theme = {r["id"]: r["theme"] for r in p["candidates"]}
print(f"{'id':24s} {'theme':22s} {'g':>6s} {'g_h1':>6s} {'g_h2':>6s} {'rho1':>6s} {'rho21':>6s} {'cov':>5s} {'w_aim':>7s}")
for i in sorted(a["members"], key=lambda i: (theme[i], -a["gain"][i])):
    r = a["rho"][i]
    f = lambda v: f"{v:6.3f}" if v is not None else "   nan"
    print(f"{i:24s} {theme[i]:22s} {a['gain'][i]:6.3f} {a['gain_half'][i][0]:6.3f} {a['gain_half'][i][1]:6.3f} "
          f"{f(r[1])} {f(r[21])} {a['coverage_mean'][i]:5.2f} {d['weights'][i]:7.4f}")
for t, e in sorted(p["themes"].items()):
    print(f"theme {t:22s} nominal {e['nominal_theme_weight']:.4f} aim {e['aim_theme_weight']:.4f} "
          f"coverage-effective {a['coverage_effective_theme_weight'][t]:.4f} members {e['admitted_count']}")
g = [a["gain"][i] for i in a["members"]]
print(f"gain range [{min(g):.3f}, {max(g):.3f}] all in [0.05, 1]: {all(0.05 <= x <= 1 for x in g)}; "
      f"weighted_standalone_turnover {p['weighted_standalone_turnover']:.4f}")
EOF
    fi
  elif [ "$ph" = w ]; then
    WF=$WA/composition_weights.json; WS=$(sha256sum $WF | cut -c1-64); echo "W_aim $WF $WS"
    for i in 1 2 3; do
      "$PY" $BR --output $WT-run$i --bind $IC --bind $R2 --bind $FD/manifest.json --bind $L --bind $WF -- $IC --library $L \
        --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $WT-$i \
        --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC --composition-weights $WF \
        --composition-weights-sha256 $WS >/dev/null 2>&1
      X=$(rc $WT-run$i); echo "weighted pass $i: $X"; tail -c 200 $WT-run$i/stderr.log | tail -1
      if [ "${X%% *}" = "0" ]; then
        echo "$i" > $WT.final
        sha256sum $WT-$i/train_combined.json | cut -c1-64 > $WT.combined.sha256   # C_aim pin for the nav phase
        echo "C_aim $WT-$i/train_combined.json $(cat $WT.combined.sha256)"; break
      fi
    done
  elif [ "$ph" = nav ]; then
    : "${COMBINED:?COMBINED=ew|aim}" "${THETA:?THETA e.g. .05}" "${DUST:?DUST e.g. .1}" "${RATE:?RATE=fixed|per-name}"
    LEV=${LEV:-1}
    case "$COMBINED" in
      ew) C=$C_EW; CS=$C_EW_S; WF=$W_EW ;;
      aim) C=$WT-$(cat $WT.final)/train_combined.json; CS=$(cat $WT.combined.sha256); WF=$WA/composition_weights.json ;;
      *) echo "nav: COMBINED must be ew or aim"; exit 2 ;;
    esac
    pin $C $CS
    case "$RATE" in
      fixed) RF="" ;;
      per-name) RF="--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15" ;;
      *) echo "nav: RATE must be fixed or per-name"; exit 2 ;;
    esac
    N=build-equity/mega-nav-v5-$COMBINED-t$THETA-d$DUST-$RATE
    [ "$LEV" = 1 ] || N=$N-L$LEV     # the R5' extra-leverage cell keeps its own directory
    [ -e "$N" ] && { echo "nav: $N exists; refusing"; exit 1; }
    echo "combined $C $CS -> $N"
    "$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
      --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 \
      --cadence 1 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV $RF \
      --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 \
      --max-bytes 1073741824 >/dev/null 2>&1
    echo "nav: $(rc $N-run)"; tail -c 300 $N-run/stderr.log; echo
    if [ -d "$REF" ] && [ "$N" != "$REF" ]; then
      "$PY" $STUDIES/nav_summ.py --weights $WF --reference $REF $N 2>&1 | grep -v financing | cut -c1-400
    else
      "$PY" $STUDIES/nav_summ.py --weights $WF $N 2>&1 | grep -v financing | cut -c1-400
    fi
  else
    echo "unknown phase $ph (fit | w | nav)"; exit 2
  fi
 done
done
