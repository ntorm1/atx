#!/usr/bin/env bash
# Root runner for statarb_cluster_study.py stages under the bounded research runner. Usage: statarb_run.sh <stage> [arg]
set -u
cd C:/atx-wt/pool-2
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
S=.superpowers/sdd/mega-alpha-20260926/studies/statarb_cluster_study.py
tag="$1${2:+-${2//[:+]/_}}"
O=build-equity/statarb-cluster-v1/runs/$tag
"$PY" $BR --output $O --bind build-equity/recent-fast-train-2020-2022-v2/manifest.json \
  --bind build-equity/recent-fast-train-2020-2022-v2-fields-v6/manifest.json -- "$PY" $S "$@" >/dev/null 2>&1
"$PY" -c "import json;r=json.load(open('$O/receipt.json'));print('$tag',r['exit_code'],r['outcome'],round(r['wall_seconds']),'s',r['sampled_peak_tree_rss_bytes']//2**20,'MiB')"
tail -c 1500 $O/stdout.log | tail -6
tail -c 600 $O/stderr.log | tail -4
