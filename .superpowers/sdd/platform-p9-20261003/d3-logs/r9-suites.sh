#!/usr/bin/env bash
# Lane D3, RESUME 9: the suites on the merged head (r9-dispatch "D3" step 3), one invocation each, in sequence.
# Log per run: d3-logs/r9-<name>.log (header: HEAD, PYTHONHASHSEED, ATX_EQUITY_BIN, start; pytest -q -rfEs; exit, end).
set -u
REPO=C:/atx-wt/pool-20
LOGS=$REPO/.superpowers/sdd/platform-p9-20261003/d3-logs
BT=C:/Users/natha/AppData/Local/Temp/claude/c--atx/a3de34c1-d7ad-4b5c-9079-b9c90532d424/scratchpad/d3r9
PY="C:/Program Files/Python312/python.exe"
BIN=C:/atx-wt/pool-2/build-equity/bin
mkdir -p "$BT"
cd "$REPO" || exit 9
export PYTHONDONTWRITEBYTECODE=1
unset ATX_EQUITY_BIN
git rev-parse HEAD > "$LOGS/r9-head.txt"
run() {  # name seed bin -- pytest args...
  local name=$1 seed=$2 bin=$3; shift 4
  local log=$LOGS/r9-$name.log
  {
    echo "== r9 $name  HEAD $(git rev-parse --short=8 HEAD)  PYTHONHASHSEED=$seed  ATX_EQUITY_BIN=${bin:-unset}  start $(date -u +%FT%TZ)"
    if [ -n "$bin" ]; then
      sha256sum "$bin/atx-equity-strategy-ic.exe" "$bin/atx-equity-strategy-targets.exe"
      PYTHONHASHSEED=$seed ATX_EQUITY_BIN=$bin "$PY" -m pytest -q -rfEs -p no:cacheprovider --basetemp "$BT/$name" "$@"
    else
      PYTHONHASHSEED=$seed "$PY" -m pytest -q -rfEs -p no:cacheprovider --basetemp "$BT/$name" "$@"
    fi
    echo "exit $?  end $(date -u +%FT%TZ)"
  } > "$log" 2>&1
  tail -n 2 "$log"
}
run wave-kinds 0 "" -- scripts/tests/test_wave_kinds.py
run live-build-equity 0 "$BIN" -- scripts/tests/test_wave_kinds.py -k live_rule_tables
run seed0 0 "" -- scripts/tests
run seed1 1 "" -- scripts/tests
run stage-chain-seed0 0 "" -- atx-engine/tools/test_stage_chain.py
run stage-chain-seed1 1 "" -- atx-engine/tools/test_stage_chain.py
echo "== all done $(date -u +%FT%TZ)"
# RESUME 9 note: ruling TEST-1 arrived during the run; the runner was stopped while seed1 ran (see r9-seed1.log);
# the two stage-chain lines above never ran (deferred to the gate).
