#!/usr/bin/env bash
# S0.1 tail: panel manifest pass, borrow proxy, section 5 metrics, lo1 aligned export. Stops on first real failure.
# A guard stop for machine headroom (137) or a non-admission (78) is retried after a pause, up to 8 attempts.
set -u
cd C:/atx/atx-db
export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
G=../.superpowers/sdd/tier1-parity/run_memory_guarded.py
PY=.venv/Scripts/python.exe
R=../.superpowers/sdd/tier1-v3/receipts
LO1=C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1
step() {
  name=$1; cap=$2; shift 2
  for attempt in 1 2 3 4 5 6 7 8; do
    echo "== $name attempt $attempt start $(date -u +%H:%M:%S)"
    $PY $G --job-gb "$cap" --wait-minutes 120 -- $PY -m "$@" > "$R/s0.1-$name.log" 2>&1
    rc=$($PY -c "import json,sys; print(json.loads(open(sys.argv[1]).read().splitlines()[-1]).get('returncode'))" "$R/s0.1-$name.log" 2>/dev/null)
    echo "== $name rc=$rc $(date -u +%H:%M:%S)"
    [ "$rc" = "0" ] && return 0
    { [ "$rc" = "137" ] || [ "$rc" = "78" ]; } || exit 1
    sleep 120
  done
  exit 1
}
[ -n "${SKIP_MANIFEST:-}" ] || step panel-manifest 0.3 atx_db.alpha_panel.panel --stages "" --years 2018
[ -n "${SKIP_BORROW:-}" ] || step borrow-proxy 0.8 atx_db.alpha_panel.borrow_proxy
[ -n "${SKIP_METRICS:-}" ] || step metrics 1.0 atx_db.alpha_panel.metrics
step export-lo1 1.0 atx_db.alpha_panel.export_impl align --role "$LO1" --out data/alpha_panel/v1/export/lo1-fields-v2
echo "== chain done"
