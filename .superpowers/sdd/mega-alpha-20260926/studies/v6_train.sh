#!/usr/bin/env bash
# v6 construction cells (v4-prereg.md "## v6 revision", V6-C: C1 order basis, C2 nonmember exit rate, C3
# locate-in-aim; review F8 liquidity cache). TRAIN only (role v2 210fff96, fields-v6 32565c32); nothing >= 2023 is
# read. = v51_train.sh's nav phase (T31/T34c) on the SAME v5.1 inputs (library 9e5ea08c, the recorded v5.1 weights
# and weighted combined of COMBINED=ew|aim, read through v51_train.sh's pin files, never re-made here) plus the v6
# knobs, v6-tagged dirs, and a non-zero exit when the nav run fails (T34c m3). Phases u / fit / w are v51_train.sh's.
#
#   COMBINED=ew|aim [ORDER_BASIS=target|delta] [EXIT_RATE=1|.05|.1] bash v6_train.sh nav
#        -> build-equity/mega-nav-v6-$COMBINED-t$THETA-d$DUST-$RATE-ob$ORDER_BASIS-x$EXIT_RATE[-loc][-L$LEV]
#   knobs (defaults = the v5.1 reference cell): THETA=.05 DUST=.1 RATE=fixed|per-name LEV=1
#        ORDER_BASIS=target   (delta: --order-basis delta, C1)
#        EXIT_RATE=1          (r < 1: --exit-rate r, C2; needs DUST > 0)
#        LOCATE_AIM=0         (1: --locate-in-aim, C3; directory suffix -loc)
#        LCACHE=1             (1: --liquidity-cache, F8; bit-identical outputs, so NOT in the directory name;
#                              0 runs the per-book windows)
#   Pre-registered grid (runs without EXTRA_CELL): THETA=.05 DUST=.1 RATE=fixed LEV=1 LOCATE_AIM=0 with
#        ORDER_BASIS in {target, delta} x EXIT_RATE in {1, .05, .1} (C2: r in {theta, 2 theta}) = 6 cells. Every
#        other cell (the conditional theta .03 / dust {0, .2} / C3 / spare / L re-derivation cells of the v6
#        revision, chosen after the grid) needs EXTRA_CELL=1 and its choice recorded in the ledger first.
#   REF (default build-equity/mega-nav-v51-$COMBINED-t.05-d.1-fixed, the v5.1 parent): the paired dSR baseline of
#        nav_summ, and for the obtarget-x1 cell (the parent's own flags) a byte-identity check of every published
#        file against it: a mismatch exits 3 (the v6 binary's default path or the cache changed bytes). With
#        REF_CHECK=1 (default) an absent REF also exits 3, before anything runs (fix round 1, review M5); REF_CHECK=0
#        skips the identity check and admits an absent REF (nav_summ then runs unpaired), e.g. for another binary.
#   DRY=1 ...  print every bounded-runner (and helper) command line instead of running it; writes nothing. Runs in this
#              script's own checkout (or DRY_ROOT=<checkout>, read-only; DRY only); a pinned file absent there is
#              reported, not checked.
# Every input is pinned by SHA-256. Any argument or env value naming validation / VAL / 2023-2025 is refused (exit 2)
# before anything runs. Outputs are never overwritten. Never --band-multiple. RAM/time bound as v51 (180 s, 1536 MiB,
# --max-bytes 1 GiB); the liquidity cache only removes work. pipefail: a failed nav_summ fails the script (exit 1).
set -uo pipefail
DRY=${DRY:-}
if [ -n "$DRY" ]; then cd "${DRY_ROOT:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}" || exit 1
elif [ -n "${DRY_ROOT:-}" ]; then echo "DRY_ROOT is honoured with DRY=1 only (real runs always use C:/atx-wt/pool-2)"; exit 2
else cd C:/atx-wt/pool-2 || exit 1; fi
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
NAV=build-equity/bin/atx-equity-strategy-targets.exe
STUDIES=.superpowers/sdd/mega-alpha-20260926/studies
L=atx-impl/strategies/fund_industry_ic_v5.json           # library v5.1 (38 = frozen v4 37 + opex_at)
LS_PIN=9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json     # TRAIN role v2
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6          # fields-v6 (TRAIN)
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
W_EW=build-equity/mega-weights-v51-ew     # v51_train.sh outputs (read only)
W_AIM=build-equity/mega-weights-v51-aim
WT=build-equity/mega-v51w-train           # v51_train.sh weighted TRAIN pass prefix: $WT-$COMBINED.final / .combined.sha256
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
dsha() { echo "DRY-sha(${1#build-equity/})"; }   # DRY placeholder for the SHA of a file this checkout lacks
pin() {  # file sha: exit 1 on mismatch (DRY: a file absent from this checkout is reported, not checked)
  if [ -n "$DRY" ] && [ ! -e "$1" ]; then echo "DRY pin (absent here) $1 = $2"; return 0; fi
  local got; got=$(sha256sum "$1" 2>/dev/null | cut -c1-64)
  [ "$got" = "$2" ] || { echo "PIN MISMATCH $1: ${got:-missing} != $2"; exit 1; }
}
state() {  # a pin recorded by a v51_train.sh phase (DRY: placeholder when absent)
  if [ -f "$1" ]; then cat "$1"
  elif [ -n "$DRY" ]; then echo "$2"
  else echo "missing $1: run the v51_train.sh phase that records it first" >&2; return 1; fi
}
THETA=${THETA:-.05}; DUST=${DUST:-.1}; RATE=${RATE:-fixed}; LEV=${LEV:-1}   # default = the v5.1 reference cell
ORDER_BASIS=${ORDER_BASIS:-target}; EXIT_RATE=${EXIT_RATE:-1}; LOCATE_AIM=${LOCATE_AIM:-0}; LCACHE=${LCACHE:-1}
REF=${REF:-build-equity/mega-nav-v51-${COMBINED:-none}-t.05-d.1-fixed}; REF_CHECK=${REF_CHECK:-1}
bad "$@" "$DRY" "${DRY_ROOT:-}" "${COMBINED:-}" "$THETA" "$DUST" "$RATE" "$LEV" "$ORDER_BASIS" "$EXIT_RATE" \
  "$LOCATE_AIM" "$LCACHE" "${EXTRA_CELL:-}" "$REF" "$REF_CHECK"
bad "$PWD" "$PY" $BR $NAV $STUDIES $L $R2 $FD $W_EW $W_AIM $WT
LS=$(sha256sum $L | cut -c1-64)                             # computed, then asserted == the v5.1 pin
[ "$LS" = "$LS_PIN" ] || { echo "PIN MISMATCH $L: ${LS:-missing} != $LS_PIN (library v5.1)"; exit 1; }
for f in "$R2 $R2S" "$FD/manifest.json $FS"; do pin $f; done
echo "library v5.1 $LS train $R2S fields $FS${DRY:+ [DRY: nothing runs or is written]}"
[ $# -gt 0 ] || { echo "usage: [DRY=1 [DRY_ROOT=]] COMBINED=ew|aim [ORDER_BASIS= EXIT_RATE= LOCATE_AIM= LCACHE=] [EXTRA_CELL=1 THETA= DUST= RATE= LEV=] bash v6_train.sh nav"; exit 2; }
for ph in $*; do case "$ph" in nav) ;; u|fit|w) echo "phase $ph: run v51_train.sh (v6-C reuses its v5.1 outputs)"; exit 2 ;;
  *) echo "unknown phase $ph (nav); nothing run"; exit 2 ;; esac; done
case "${COMBINED:-}" in
  ew) W=$W_EW ;;
  aim) W=$W_AIM ;;
  *) echo "nav: set COMBINED=ew or COMBINED=aim"; exit 2 ;;
esac
case "$ORDER_BASIS" in target) OB="" ;; delta) OB="--order-basis delta" ;;
  *) echo "nav: ORDER_BASIS must be target or delta"; exit 2 ;; esac
[[ $EXIT_RATE =~ ^(1|0?\.[0-9]*[1-9][0-9]*)$ ]] || { echo "nav: EXIT_RATE must be 1 or a decimal in (0, 1), e.g. .05"; exit 2; }
if [ "$EXIT_RATE" = 1 ]; then XF=""; else XF="--exit-rate $EXIT_RATE"; fi
case "$LOCATE_AIM" in 0) LF="" ;; 1) LF="--locate-in-aim" ;; *) echo "nav: LOCATE_AIM must be 0 or 1"; exit 2 ;; esac
case "$LCACHE" in 0) CF="" ;; 1) CF="--liquidity-cache" ;; *) echo "nav: LCACHE must be 0 or 1"; exit 2 ;; esac
case "$REF_CHECK" in 0|1) ;; *) echo "nav: REF_CHECK must be 0 or 1"; exit 2 ;; esac
case "$RATE" in
  fixed) RF="" ;;
  per-name) RF="--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15" ;;
  *) echo "nav: RATE must be fixed or per-name"; exit 2 ;;
esac
GRID=
if [ "$THETA $DUST $RATE $LEV $LOCATE_AIM" = ".05 .1 fixed 1 0" ]; then
  case "$ORDER_BASIS $EXIT_RATE" in "target 1"|"target .05"|"target .1"|"delta 1"|"delta .05"|"delta .1") GRID=1 ;; esac
fi
if [ -z "$GRID" ] && [ "${EXTRA_CELL:-}" != 1 ]; then
  echo "nav: the v6 revision pre-registers the 6-cell grid THETA=.05 DUST=.1 RATE=fixed LEV=1 LOCATE_AIM=0 x"
  echo "     ORDER_BASIS {target, delta} x EXIT_RATE {1, .05, .1}. THETA=$THETA DUST=$DUST RATE=$RATE LEV=$LEV"
  echo "     ORDER_BASIS=$ORDER_BASIS EXIT_RATE=$EXIT_RATE LOCATE_AIM=$LOCATE_AIM is a conditional/extra trial: record"
  echo "     its choice in the ledger (v4-prereg.md '## v6 revision'), then re-run with EXTRA_CELL=1."; exit 2
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
N=build-equity/mega-nav-v6-$COMBINED-t$THETA-d$DUST-$RATE-ob$ORDER_BASIS-x$EXIT_RATE
[ "$LOCATE_AIM" = 1 ] && N=$N-loc
[ "$LEV" = 1 ] || N=$N-L$LEV     # an extra-leverage cell keeps its own directory
bad $N
if [ -e "$N" ] || [ -e "$N-run" ]; then echo "nav: $N or $N-run exists; refusing"; exit 1; fi
# REF is the obtarget-x1 cell's byte-identity parent and every cell's paired nav_summ baseline: with REF_CHECK=1 it
# must exist before anything runs (review M5; exit 3). REF_CHECK=0: no identity check; unpaired if absent.
if [ "$REF_CHECK" = 1 ] && [ ! -d "$REF" ]; then
  if [ -n "$DRY" ]; then echo "DRY: REF $REF absent here (a real run exits 3 before running; REF_CHECK=0 skips)"
  else echo "nav: REF $REF absent with REF_CHECK=1: nothing run (REF_CHECK=0 runs without the v5.1 parent)"; exit 3; fi
fi
echo "combined $C $CS -> $N (order basis $ORDER_BASIS, exit rate $EXIT_RATE, locate-in-aim $LOCATE_AIM, cache $LCACHE)"
run $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
  --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 \
  --cadence 1 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV $RF $OB $XF $LF $CF \
  --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 \
  --max-bytes 1073741824
X=$(rc $N-run); echo "nav: $X"; [ -n "$DRY" ] || { tail -c 300 $N-run/stderr.log; echo; }
case "$X" in
  "0 completed"*) ;;
  none*) echo "nav: no receipt in $N-run (the bounded runner refused before launch: dirty tree?)"; exit 1 ;;
  *) echo "nav: FAILED ($X): see $N-run/stderr.log; nothing to summarise (report; do not raise the bound)"; exit 1 ;;
esac
if [ -z "$DRY" ] && [ ! -f "$N/summary.json" ]; then echo "nav: $N/summary.json missing after a completed run"; exit 1; fi
# The parent's own flags (obtarget-x1, no locate, the reference theta/dust/rate/L) must reproduce the v5.1 parent byte
# for byte: the v6 default path and the liquidity cache change no output.
if [ "$REF_CHECK" = 1 ] && [ "$ORDER_BASIS $EXIT_RATE $LOCATE_AIM $THETA $DUST $RATE $LEV" = "target 1 0 .05 .1 fixed 1" ]; then
  if [ -n "$DRY" ]; then echo "DRY: byte-identity check of every file in $N against $REF"
  elif [ -d "$REF" ]; then
    diffs=0
    for f in "$REF"/*; do
      b=$(basename "$f")
      [ "$(sha256sum "$f" | cut -c1-64)" = "$(sha256sum "$N/$b" 2>/dev/null | cut -c1-64)" ] || { echo "  differs: $b"; diffs=1; }
    done
    [ "$(ls "$REF" | wc -l)" = "$(ls "$N" | wc -l)" ] || { echo "  file sets differ"; diffs=1; }
    if [ $diffs = 1 ]; then echo "REF-IDENTITY MISMATCH: $N vs $REF (investigate before any v6 comparison)"; exit 3; fi
    echo "ref identity: every file of $N equals $REF byte for byte"
  else
    echo "REF-IDENTITY: $REF absent (REF_CHECK=1)"; exit 3   # unreachable: refused before the run
  fi
fi
if [ -n "$DRY" ]; then
  echo "DRY: $(q "$PY" $STUDIES/nav_summ.py --weights $WF --reference $REF $N) (no --reference when REF_CHECK=0 and $REF is absent)"
else
  if [ -d "$REF" ]; then REFA=(--reference "$REF"); else REFA=(); fi   # absent only under REF_CHECK=0
  "$PY" $STUDIES/nav_summ.py --weights $WF ${REFA[@]+"${REFA[@]}"} $N 2>&1 | grep -v financing | cut -c1-400
  s=${PIPESTATUS[0]}
  [ "$s" = 0 ] || { echo "nav_summ: FAILED (exit $s) on $N"; exit 1; }
fi
