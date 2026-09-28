#!/usr/bin/env bash
# v5.1 TRAIN pipeline (v4-prereg.md "## v5.1 family" R1''): library v5.1 = frozen v4 (37, daa9663e) + opex_at = 38
# candidates (9e5ea08c). Admission v4-prior-v1 unchanged; BOTH compositions re-fit on the 38 (+2 composition trials);
# construction: the R5' reference cell only (+1 per composition run). TRAIN only (role v2 210fff96, fields-v6 32565c32);
# nothing >= 2023 is read. = v5_train.sh (T31) with L = v5.1, the v4_train.sh u phase, per-composition fit / w outputs
# and v51-tagged dirs (mega-v51-train-u, mega-weights-v51-*, mega-v51w-train-*, mega-nav-v51-*, mega-fit-work-v51).
#
#   bash v51_train.sh u                          -> unweighted TRAIN IC pass build-equity/mega-v51-train-u-<i> (<= 3 bounded
#                                                   passes); records <prefix>.final + orientations / runner-summary pins
#   COMP=ew-theme-v1     bash v51_train.sh fit   -> build-equity/mega-weights-v51-ew  (<= 3 bounded passes, resumable)
#   COMP=ew-theme-aim-v1 bash v51_train.sh fit   -> build-equity/mega-weights-v51-aim (same work dir mega-fit-work-v51)
#   COMBINED=ew|aim bash v51_train.sh w          -> weighted TRAIN IC pass build-equity/mega-v51w-train-$COMBINED-<i>
#   COMBINED=ew|aim bash v51_train.sh nav        -> the reference cell (THETA=.05 DUST=.1 RATE=fixed LEV=1, the defaults):
#                                                   build-equity/mega-nav-v51-$COMBINED-t.05-d.1-fixed
#   EXTRA_CELL=1 COMBINED=.. THETA=.. DUST=.. RATE=fixed|per-name [LEV=..] bash v51_train.sh nav
#                                                -> build-equity/mega-nav-v51-$COMBINED-t$THETA-d$DUST-$RATE[-L$LEV]
#                                                   (any other cell is an undeclared trial: refused unless EXTRA_CELL=1)
#   DRY=1 ...  print every bounded-runner (and helper) command line instead of running it; writes nothing. Runs in this
#              script's own checkout (or DRY_ROOT=<checkout>, read-only; DRY only); a pinned file absent there is
#              reported, not checked.
# Phases chain through pins recorded next to their outputs (<prefix>.final, <prefix>.*.sha256), checked when read.
# Every input is pinned by SHA-256 (here, and the tools refuse unpinned inputs). Any argument or env value naming
# validation / VAL / 2023-2025 is refused (exit 2) before anything runs. Outputs are never overwritten. Never --band-multiple.
set -u
DRY=${DRY:-}
if [ -n "$DRY" ]; then cd "${DRY_ROOT:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}" || exit 1
elif [ -n "${DRY_ROOT:-}" ]; then echo "DRY_ROOT is honoured with DRY=1 only (real runs always use C:/atx-wt/pool-2)"; exit 2
else cd C:/atx-wt/pool-2 || exit 1; fi
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe
NAV=build-equity/bin/atx-equity-strategy-targets.exe
STUDIES=.superpowers/sdd/mega-alpha-20260926/studies
L=atx-impl/strategies/fund_industry_ic_v5.json           # R1'': library v5.1 (38 = frozen v4 37 + opex_at)
LS_PIN=9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0
LR=atx-impl/strategies/fund_industry_ic_v5.recipe.json    # v5.1 recipe (priors, lineage) for the fitter
LRS=a26670b0f7b6d1681a4ea8a4758da83841163ceaea8af5a4256ee718ad7c3aea
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json     # TRAIN role v2
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6          # fields-v6 (TRAIN)
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
A4=build-equity/mega-weights-v4/admission.json             # v4 admission 880a0a6a (31 of 37): printed diff only
A4S=880a0a6a9e6111b9bee4b2335fe36d9671070b4fefdee56586427e1e9a5667df
W4=build-equity/mega-weights-v4/composition_weights.json   # v4 ew-theme-v1 9a9c949a: printed diff only
W4S=9a9c949a5f445bb71e2c3af0c6943efcf2c44693bb5cff25b53864243089cf65
CC=build-equity/mega-candidate-cache
U=build-equity/mega-v51-train-u        # unweighted TRAIN IC pass prefix (O, S = $U-<final>/{orientations,summary}.json)
WORK=build-equity/mega-fit-work-v51    # fitter work dir, shared by both compositions (factor records reused)
W_EW=build-equity/mega-weights-v51-ew
W_AIM=build-equity/mega-weights-v51-aim
WT=build-equity/mega-v51w-train        # weighted TRAIN IC pass prefix: $WT-$COMBINED-<i>/train_combined.json
REF=build-equity/mega-nav-v5-ew-t.05-d.1-fixed   # v5 R5' reference cell (frozen v4 library): paired dSR baseline
bad() {  # refuse (exit 2) any value naming validation / VAL (a whole token, any case) / 2023-2024 (validation) or
         # 2025 (reserved); 64-hex SHA values are not paths
  local a l re='(^|[^a-z])val([^a-z]|$)'
  for a in "$@"; do
    [[ $a =~ ^[0-9a-f]{64}$ ]] && continue
    l=${a,,}
    if [[ $l == *validation* || $l =~ $re || $l == *2023* || $l == *2024* || $l == *2025* ]]; then
      echo "REFUSED (TRAIN only): '$a' names validation / VAL / 2023-2025"; exit 2
    fi
  done
}
q() { local a s=""; for a in "$@"; do case "$a" in *" "*) s="$s \"$a\"" ;; *) s="$s $a" ;; esac; done; echo "${s# }"; }
run() {  # one bounded-runner command line: $BR --output ... -- tool ...; DRY prints it instead
  bad "$@"
  if [ -n "$DRY" ]; then echo "DRY: $(q "$PY" "$@")"; return 0; fi
  "$PY" "$@" >/dev/null 2>&1
}
rc() {  # "exit outcome seconds peak-MiB" of a bounded run ("none no-receipt": the runner refused before launch)
  if [ -n "$DRY" ]; then echo "0 completed 0 0 (dry)"; return 0; fi
  [ -f "$1/receipt.json" ] || { echo "none no-receipt"; return 0; }
  "$PY" -c "import json;r=json.load(open('$1/receipt.json'));print(r['exit_code'],r['outcome'],round(r['wall_seconds']),r['sampled_peak_tree_rss_bytes']//2**20)"
}
logs() { [ -n "$DRY" ] || tail -c "$2" "$1/stderr.log" | tail -"$3"; }   # run-dir bytes lines
hits() { [ -n "$DRY" ] || echo "hits=$(grep -c 'cache=hit' "$1/stdout.log" 2>/dev/null) miss=$(grep -c 'cache=miss' "$1/stdout.log" 2>/dev/null)"; }
dsha() { echo "DRY-sha(${1#build-equity/})"; }   # DRY placeholder for the SHA of a file this checkout lacks
sha() { if [ -n "$DRY" ] && [ ! -e "$1" ]; then dsha "$1"; else sha256sum "$1" | cut -c1-64; fi; }   # SHA of a produced file
pin() {  # file sha: exit 1 on mismatch (DRY: a file absent from this checkout is reported, not checked)
  if [ -n "$DRY" ] && [ ! -e "$1" ]; then echo "DRY pin (absent here) $1 = $2"; return 0; fi
  local got; got=$(sha256sum "$1" 2>/dev/null | cut -c1-64)
  [ "$got" = "$2" ] || { echo "PIN MISMATCH $1: ${got:-missing} != $2"; exit 1; }
}
pinok() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -c1-64)" = "$2" ]; }   # diagnostic-only inputs: no exit
state() {  # a pin recorded by an earlier phase (DRY: placeholder when absent)
  if [ -f "$1" ]; then cat "$1"
  elif [ -n "$DRY" ]; then echo "$2"
  else echo "missing $1: run the phase that records it first" >&2; return 1; fi
}
record() { if [ -n "$DRY" ]; then echo "DRY record $2 <- $1"; else echo "$1" > "$2"; fi; }   # value pin-file
fresh() {  # prefix i: 0 when pass i is unused; a pass left by an earlier invocation is skipped (resume)
  if [ -e "$1-run$2" ]; then echo "pass $2: $1-run$2 exists (earlier invocation); skipped"; return 1; fi
  [ -e "$1-$2" ] && { echo "REFUSED: $1-$2 exists without its receipt dir $1-run$2"; exit 1; }
  return 0
}
bad "$@" "$DRY" "${DRY_ROOT:-}" "${COMP:-}" "${COMBINED:-}" "${THETA:-}" "${DUST:-}" "${RATE:-}" "${LEV:-}" "${EXTRA_CELL:-}"
bad "$PWD" "$PY" $BR $IC $NAV $STUDIES $L $LR $R2 $FD $A4 $W4 $CC $U $WORK $W_EW $W_AIM $WT $REF
LS=$(sha256sum $L | cut -c1-64)                             # computed, then asserted == the v5.1 pin
[ "$LS" = "$LS_PIN" ] || { echo "PIN MISMATCH $L: ${LS:-missing} != $LS_PIN (library v5.1)"; exit 1; }
for f in "$LR $LRS" "$R2 $R2S" "$FD/manifest.json $FS"; do pin $f; done
echo "library v5.1 $LS recipe $LRS train $R2S fields $FS${DRY:+ [DRY: nothing runs or is written]}"
[ $# -gt 0 ] || { echo "usage: [DRY=1 [DRY_ROOT=]] [COMP=ew-theme-v1|ew-theme-aim-v1] [COMBINED=ew|aim] [EXTRA_CELL=1 THETA= DUST= RATE= LEV=] bash v51_train.sh u|fit|w|nav ..."; exit 2; }
for ph in $*; do case "$ph" in u|fit|w|nav) ;; *) echo "unknown phase $ph (u | fit | w | nav); nothing run"; exit 2 ;; esac; done
for phase in "$@"; do
 for ph in $phase; do
  if [ "$ph" = u ]; then
    [ -e $U.final ] && { echo "u: $U.final exists (pass $(cat $U.final)); refusing (outputs are never overwritten)"; exit 1; }
    ok=
    for i in 1 2 3; do
      fresh $U $i || continue
      # = v4_train.sh u: unweighted pass (the runner's own orientations are provenance only; the fitter runs
      # --orientation prior); the 37 v4 entries hit the cache, opex_at is the one miss.
      run $BR --output $U-run$i --bind $IC --bind $L --bind $R2 --bind $FD/manifest.json -- $IC --library $L --library-sha256 $LS \
        --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $U-$i --max-memory-mib 1536 \
        --min-names 1000 --workers 4 --save-combined --candidate-cache $CC
      X=$(rc $U-run$i); echo "u pass $i: $X $(hits $U-run$i)"; logs $U-run$i 200 1
      case "$X" in
        0\ *) ok=1; break ;;
        none*) echo "u: no receipt in $U-run$i (the bounded runner refused before launch: dirty tree?)"; exit 1 ;;
      esac
    done
    [ -n "$ok" ] || { echo "u: no successful pass among 1..3 (report; do not raise the bound)"; exit 1; }
    OS=$(sha $U-$i/orientations.json); SS=$(sha $U-$i/summary.json)
    record $i $U.final; record $OS $U.orientations.sha256; record $SS $U.summary.sha256
    echo "orientations $U-$i/orientations.json $OS"; echo "runner summary $U-$i/summary.json $SS"
  elif [ "$ph" = fit ]; then
    case "${COMP:-}" in
      ew-theme-v1) W=$W_EW ;;
      ew-theme-aim-v1) W=$W_AIM ;;
      *) echo "fit: set COMP=ew-theme-v1 (-> $W_EW) or COMP=ew-theme-aim-v1 (-> $W_AIM)"; exit 2 ;;
    esac
    [ -e "$W" ] && { echo "fit: $W exists; refusing (the fitter never overwrites)"; exit 1; }
    i=$(state $U.final 1) || exit 1
    O=$U-$i/orientations.json; S=$U-$i/summary.json
    OS=$(state $U.orientations.sha256 "$(dsha $O)") || exit 1
    SS=$(state $U.summary.sha256 "$(dsha $S)") || exit 1
    pin $O $OS; pin $S $SS
    ok=
    for j in 1 2 3; do
      [ -e $W-run$j ] && { echo "fit pass $j: $W-run$j exists (earlier invocation; $WORK resumes); skipped"; continue; }
      # --max-seconds 150 < the 180 s bound: a pass stops cleanly between candidates (exit 3, stdout
      # {"status":"incomplete","partial":true,...}); every finished candidate persists in $WORK, the next pass resumes.
      run $BR --output $W-run$j --bind $L --bind $LR --bind $R2 --bind $O --bind $S -- "$PY" \
        atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
        --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
        --orientation prior --screen v4-prior-v1 --composition "$COMP" --recipe $LR --recipe-sha256 $LRS \
        --work-dir $WORK --max-seconds 150 --output $W
      X=$(rc $W-run$j); echo "fit pass $j ($COMP): $X"; logs $W-run$j 300 2
      [ -n "$DRY" ] || { head -c 400 $W-run$j/stdout.log; echo; }
      case "$X" in
        "0 completed"*) ok=1; break ;;
        "3 "*|*time-limit*) ;;
        none*) echo "fit: no receipt in $W-run$j (the bounded runner refused before launch: dirty tree?)"; exit 1 ;;
        *) echo "fit: refused or failed (see $W-run$j/stderr.log); not retrying"; exit 1 ;;
      esac
    done
    [ -n "$ok" ] || { echo "fit: incomplete after 3 passes (report; do not raise the bound)"; exit 1; }
    WS=$(sha $W/composition_weights.json); record $WS $W.weights.sha256
    echo "weights $W/composition_weights.json $WS"; echo "admission $W/admission.json $(sha $W/admission.json)"
    A4X=-; pinok $A4 $A4S && A4X=$A4
    W4X=-; pinok $W4 $W4S && W4X=$W4
    if [ -n "$DRY" ]; then
      echo "DRY: $(q "$PY" - $W/composition_weights.json $W/admission.json $A4X $W4X) <<fit report (admission diff vs v4, opex_at, theme / g_k table)"
    else
      "$PY" - "$W/composition_weights.json" "$W/admission.json" "$A4X" "$W4X" <<'EOF'
import json, sys
d, adm = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
a4 = json.load(open(sys.argv[3])) if sys.argv[3] != "-" else None
w4 = json.load(open(sys.argv[4])) if sys.argv[4] != "-" else None
p = d["provenance"]
f = lambda v: f"{v:6.3f}" if isinstance(v, (int, float)) else "   nan"
print(f"{p['rule']}: admitted {len(adm['admitted'])} of {len(adm['candidates'])}; counts {adm['counts']}; "
      f"fitted {p['fitted_candidates']}")
if a4 is None:
    print("vs v4 admission: skipped (880a0a6a not present or pin differs)")
else:
    new, old = set(adm["admitted"]), set(a4["admitted"])
    print(f"vs v4 admission 880a0a6a ({len(old)} admitted): added {sorted(new - old)} dropped {sorted(old - new)}")
c = {r["id"]: r for r in adm["candidates"]}["opex_at"]
print(f"opex_at: {c['status']} tau {f(c['tau'])} hac_t {f(c['hac_t'])} max|rho| {f(c['max_abs_rho'])} "
      f"with {c['max_abs_rho_with']} redundant_with {c['redundant_with']} weight {d['weights'].get('opex_at', 0.0):.4f}")
if p["rule"] == "ew-theme-aim-v1":
    a = p["aim"]
    theme = {r["id"]: r["theme"] for r in p["candidates"]}
    print(f"{'id':24s} {'theme':22s} {'g':>6s} {'g_h1':>6s} {'g_h2':>6s} {'rho1':>6s} {'rho21':>6s} {'cov':>5s} {'w_aim':>7s}")
    for i in sorted(a["members"], key=lambda i: (theme[i], -a["gain"][i])):
        r = a["rho"][i]
        print(f"{i:24s} {theme[i]:22s} {a['gain'][i]:6.3f} {a['gain_half'][i][0]:6.3f} {a['gain_half'][i][1]:6.3f} "
              f"{f(r[1])} {f(r[21])} {a['coverage_mean'][i]:5.2f} {d['weights'][i]:7.4f}")
    for t, e in sorted(p["themes"].items()):
        print(f"theme {t:22s} nominal {e['nominal_theme_weight']:.4f} aim {e['aim_theme_weight']:.4f} "
              f"coverage-effective {a['coverage_effective_theme_weight'][t]:.4f} members {e['admitted_count']}")
    g = [a["gain"][i] for i in a["members"]]
    print(f"gain range [{min(g):.3f}, {max(g):.3f}] all in [0.05, 1]: {all(0.05 <= x <= 1 for x in g)}; "
          f"weighted_standalone_turnover {p['weighted_standalone_turnover']:.4f}")
else:
    for t, e in sorted(p["themes"].items()):
        print(f"theme {t:22s} weight {e['theme_weight']:.4f} members {e['admitted_count']}: {', '.join(e['admitted'])}")
    print(f"weighted_standalone_turnover {p['weighted_standalone_turnover']:.4f}")
    if w4 is not None:
        ids = sorted(set(d["weights"]) | set(w4["weights"]))
        dw = max(abs(d["weights"].get(i, 0.0) - w4["weights"].get(i, 0.0)) for i in ids)
        print(f"vs v4 ew-theme-v1 9a9c949a: max |w - w_v4| {dw:.6f} over {len(ids)} ids "
              f"({'SAME weights: C_ew51 reproduces C_ew' if dw == 0 else 'weights differ'})")
EOF
    fi
  elif [ "$ph" = w ]; then
    case "${COMBINED:-}" in
      ew) W=$W_EW; RULE=ew-theme-v1 ;;
      aim) W=$W_AIM; RULE=ew-theme-aim-v1 ;;
      *) echo "w: set COMBINED=ew ($W_EW) or COMBINED=aim ($W_AIM)"; exit 2 ;;
    esac
    P=$WT-$COMBINED
    [ -e $P.final ] && { echo "w: $P.final exists (pass $(cat $P.final)); refusing (outputs are never overwritten)"; exit 1; }
    WF=$W/composition_weights.json
    WS=$(state $W.weights.sha256 "$(dsha $WF)") || exit 1
    iu=$(state $U.final 1) || exit 1
    OS=$(state $U.orientations.sha256 "$(dsha $U-$iu/orientations.json)") || exit 1
    SS=$(state $U.summary.sha256 "$(dsha $U-$iu/summary.json)") || exit 1
    pin $WF $WS; echo "W_$COMBINED (v5.1) $WF $WS"
    if [ -n "$DRY" ]; then
      echo "DRY: $(q "$PY" - $WF $LS $RULE $OS $SS $R2S) <<provenance check (library v5.1, rule, this u pass, TRAIN role v2)"
    else
      "$PY" - "$WF" "$LS" "$RULE" "$OS" "$SS" "$R2S" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1])); p = d["provenance"]
got = {"library_sha256": d["library_sha256"], "rule": p["rule"], "orientations_sha256": p["orientations_sha256"],
       "runner_summary_sha256": p["runner_summary_sha256"], "train_manifest_sha256": d["train_manifest_sha256"]}
off = {k: (v, w) for (k, v), w in zip(got.items(), sys.argv[2:7]) if v != w}
print("weights provenance: " + (f"MISMATCH {off}" if off else "library v5.1, rule, the recorded u pass and TRAIN role v2 all match"))
sys.exit(1 if off else 0)
EOF
      [ $? = 0 ] || exit 1
    fi
    ok=
    for i in 1 2 3; do
      fresh $P $i || continue
      run $BR --output $P-run$i --bind $IC --bind $R2 --bind $FD/manifest.json --bind $L --bind $WF -- $IC --library $L \
        --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $P-$i \
        --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC --composition-weights $WF \
        --composition-weights-sha256 $WS
      X=$(rc $P-run$i); echo "weighted pass $i ($COMBINED): $X"; logs $P-run$i 200 1
      case "$X" in
        0\ *) ok=1; break ;;
        none*) echo "w: no receipt in $P-run$i (the bounded runner refused before launch: dirty tree?)"; exit 1 ;;
      esac
    done
    [ -n "$ok" ] || { echo "w: no successful pass among 1..3 (report; do not raise the bound)"; exit 1; }
    CS=$(sha $P-$i/train_combined.json)
    record $i $P.final; record $CS $P.combined.sha256            # C_$COMBINED pin for the nav phase
    echo "C_$COMBINED (v5.1) $P-$i/train_combined.json $CS"
  elif [ "$ph" = nav ]; then
    case "${COMBINED:-}" in
      ew) W=$W_EW ;;
      aim) W=$W_AIM ;;
      *) echo "nav: set COMBINED=ew or COMBINED=aim"; exit 2 ;;
    esac
    THETA=${THETA:-.05}; DUST=${DUST:-.1}; RATE=${RATE:-fixed}; LEV=${LEV:-1}   # default = the reference cell
    if [ "$THETA $DUST $RATE $LEV" != ".05 .1 fixed 1" ] && [ "${EXTRA_CELL:-}" != 1 ]; then
      echo "nav: v5.1 pre-registers the reference construction only (THETA=.05 DUST=.1 RATE=fixed LEV=1; construction +1"
      echo "     per composition). THETA=$THETA DUST=$DUST RATE=$RATE LEV=$LEV is an undeclared trial: declare it in"
      echo "     v4-prereg.md first, then re-run with EXTRA_CELL=1."; exit 2
    fi
    P=$WT-$COMBINED
    i=$(state $P.final 1) || exit 1
    C=$P-$i/train_combined.json
    CS=$(state $P.combined.sha256 "$(dsha $C)") || exit 1
    WF=$W/composition_weights.json
    WS=$(state $W.weights.sha256 "$(dsha $WF)") || exit 1
    pin $C $CS; pin $WF $WS
    # the combined must come from the recorded weights of this composition, library v5.1, TRAIN role v2, fields-v6
    if [ -n "$DRY" ]; then
      echo "DRY: $(q "$PY" - $C $WS $LS $R2S $FS) <<combined provenance check (W_$COMBINED, library v5.1, TRAIN, fields-v6)"
    else
      "$PY" - "$C" "$WS" "$LS" "$R2S" "$FS" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
ws, ls, r2s, fs = sys.argv[2:6]
want = {"composition_weights_sha256": ws, "library_sha256": ls, "role": "train", "role_manifest_sha256": r2s,
        "train_manifest_sha256": r2s, "research_fields_manifest_sha256": fs, "status": "complete"}
off = {k: (d.get(k), v) for k, v in want.items() if d.get(k) != v}
print("combined provenance: " + (f"MISMATCH {off}" if off else
      "recorded weights, library v5.1, TRAIN role v2, fields-v6, complete: all match"))
sys.exit(1 if off else 0)
EOF
      [ $? = 0 ] || exit 1
    fi
    case "$RATE" in
      fixed) RF="" ;;
      per-name) RF="--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15" ;;
      *) echo "nav: RATE must be fixed or per-name"; exit 2 ;;
    esac
    N=build-equity/mega-nav-v51-$COMBINED-t$THETA-d$DUST-$RATE
    [ "$LEV" = 1 ] || N=$N-L$LEV     # an extra-leverage cell keeps its own directory
    bad $N
    if [ -e "$N" ] || [ -e "$N-run" ]; then echo "nav: $N or $N-run exists; refusing"; exit 1; fi
    echo "combined $C $CS -> $N"
    run $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
      --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 \
      --cadence 1 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV $RF \
      --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 \
      --max-bytes 1073741824
    echo "nav: $(rc $N-run)"; [ -n "$DRY" ] || { tail -c 300 $N-run/stderr.log; echo; }
    if [ -n "$DRY" ]; then
      echo "DRY: $(q "$PY" $STUDIES/nav_summ.py --weights $WF --reference $REF $N) (no --reference when $REF is absent)"
    elif [ -d "$REF" ]; then
      "$PY" $STUDIES/nav_summ.py --weights $WF --reference $REF $N 2>&1 | grep -v financing | cut -c1-400
    else
      "$PY" $STUDIES/nav_summ.py --weights $WF $N 2>&1 | grep -v financing | cut -c1-400
    fi
  else
    echo "unknown phase $ph (u | fit | w | nav)"; exit 2
  fi
 done
done
