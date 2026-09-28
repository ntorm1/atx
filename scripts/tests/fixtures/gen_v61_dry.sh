#!/usr/bin/env bash
# Capture the v6.1 ladder's command lines as DRY=1 output: the fixture research_cycle.py's `plan specs/v61.json`
# must equal line for line (test_research_cycle.py). Nothing is executed and nothing is written outside stdout:
# v61_train.sh is copied to a temp file with every executing command prefixed by `dry` (prints its argv, quoting an
# argument that holds a space), its inline python checks replaced by `true`, its output-exists short-circuits and
# .final writes removed, and its exit-code greps dropped. The script itself still cds to C:/atx-wt/pool-2 and
# computes every pin with sha256sum from the existing TRAIN artefacts, exactly as it did for the real run.
#   bash scripts/tests/fixtures/gen_v61_dry.sh [path/to/v61_train.sh] > lines.txt
# Output: "DRY: <command>" lines (the fixture v61_train_dry.txt keeps them without the prefix) and
# "PIN: <relpath> <sha256>" lines for every file whose SHA the resolved lines carry (fixture v61_pins.json).
set -euo pipefail
SRC=${1:-"$(cd "$(dirname "$0")/../../.." && pwd)/.superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh"}
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
sed -e '/^set -uo pipefail$/a dry() { local a out=""; for a in "$@"; do case "$a" in *" "*) out+="\\"$a\\" ";; *) out+="$a ";; esac; done; echo "DRY: ${out% }"; }' \
    -e 's#"\$PY" atx-engine/tools/prepare_research_fields.py#dry "$PY" atx-engine/tools/prepare_research_fields.py#' \
    -e 's#"\$PY" atx-impl/strategies/check_fund_ic_v6.py#dry "$PY" atx-impl/strategies/check_fund_ic_v6.py#' \
    -e 's#"\$PY" \$BR #dry "$PY" $BR #' \
    -e 's#"\$PY" \$STUDIES/nav_summ.py#dry "$PY" $STUDIES/nav_summ.py#' \
    -e 's#"\$PY" - #true #' \
    -e "s# | grep -E '\"exit_code\"' | head -1##" \
    -e 's# 2>&1 | tail -3##' \
    -e 's#if \[ ! -f \$FD7/manifest.json \]; then#if true; then#' \
    -e '/\[ -d \$FD7 \] && {/d' \
    -e '/echo "u complete/d' -e '/echo "w complete/d' \
    -e 's#echo \$i > \$U.final#true#' -e 's#echo \$i > \$WT.final#true#' \
    -e '/echo "fit output exists/d' -e '/echo "nav output exists/d' \
    "$SRC" > "$TMP"
cat >> "$TMP" <<'EOF'
for f in $L $LR $L6 $R2LO $FD6/manifest.json $BRIDGE/manifest.json $EVENTS/manifest.json $REF_ADM $REFN/summary.json \
         $FD7/manifest.json \
         $U-1/orientations.json $U-1/summary.json $W/composition_weights.json $WT-1/train_combined.json \
         $WT-1/summary.json $N/summary.json; do
  echo "PIN: $f $(sha "$f")"
done
EOF
bash "$TMP" all | grep -E '^(DRY|PIN): '
